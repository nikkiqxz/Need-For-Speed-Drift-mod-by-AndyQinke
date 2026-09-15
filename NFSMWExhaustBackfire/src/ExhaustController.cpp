#include "nfsmw_exhaust/ExhaustController.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace nfsmw_exhaust {
namespace {

constexpr std::int32_t kNeutralGear = 1;

std::uint32_t defaultSeed() noexcept {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    return static_cast<std::uint32_t>(now.count() ^
                                      (now.count() >> 32));
}

}  // namespace

ExhaustController::ExhaustController(IGameBridge& bridge,
                                     ExhaustConfig config)
    : bridge_(bridge), config_(std::move(config)),
      rng_(config_.randomSeed == 0 ? defaultSeed() : config_.randomSeed) {
    config_.normalize();
}

ExhaustController::~ExhaustController() {
    /* Integrations restore state explicitly while callbacks and IDs are live. */
}

void ExhaustController::tick(std::uint64_t nowMs) {
    const std::size_t capacity =
        std::max<std::size_t>(1u, config_.maxTrackedVehicles);
    std::vector<VehicleSnapshot> snapshots(capacity);
    std::size_t count = bridge_.collectVehicles(snapshots.data(), capacity);
    if (count > capacity) count = capacity;
    tick(snapshots.data(), count, nowMs);
}

void ExhaustController::tick(const VehicleSnapshot* snapshots,
                             std::size_t count, std::uint64_t nowMs) {
    if (snapshots == nullptr && count != 0) return;

    std::vector<VehicleId> observed;
    observed.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (!snapshots[i].valid || snapshots[i].id == 0) continue;
        if (std::find(observed.begin(), observed.end(), snapshots[i].id) !=
            observed.end()) {
            continue;
        }
        observed.push_back(snapshots[i].id);
        processVehicle(snapshots[i], nowMs);
    }
    expireMissingVehicles(observed.data(), observed.size());
}

void ExhaustController::reset() {
    for (const auto& entry : states_) {
        if (entry.second.vanillaDisabled) {
            bridge_.setVanillaExhaustEnabled(entry.first, true);
        }
    }
    states_.clear();
}

void ExhaustController::expireMissingVehicles(const VehicleId* ids,
                                              std::size_t count) {
    for (auto it = states_.begin(); it != states_.end();) {
        bool present = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (ids[i] == it->first) {
                present = true;
                break;
            }
        }
        if (!present) {
            /* Its ID lifetime has ended; never call the adapter with it. */
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
}

void ExhaustController::processVehicle(const VehicleSnapshot& snapshot,
                                       std::uint64_t nowMs) {
    VehicleState& state = states_[snapshot.id];
    if (state.initialized && nowMs < state.lastTickMs) {
        /* A loading transition or timer wrap must not create a long burst. */
        const bool vanillaDisabled = state.vanillaDisabled;
        state = VehicleState{};
        state.vanillaDisabled = vanillaDisabled;
    }
    state.initialized = true;
    state.lastTickMs = nowMs;

    if (config_.suppressVanilla && !state.vanillaDisabled) {
        bridge_.setVanillaExhaustEnabled(snapshot.id, false);
        state.vanillaDisabled = true;
    } else if (!config_.suppressVanilla && state.vanillaDisabled) {
        bridge_.setVanillaExhaustEnabled(snapshot.id, true);
        state.vanillaDisabled = false;
    }

    const bool markerGate = config_.requireBothMarkers
                                ? snapshot.hasBothExhaustMarkers()
                                : (snapshot.leftExhaust.present ||
                                   snapshot.rightExhaust.present);
    if (!markerGate || snapshot.maxRpm <= 0.0f ||
        !std::isfinite(snapshot.rpm) || !std::isfinite(snapshot.maxRpm) ||
        snapshot.rpm <= kMinimumBackfireRpm) {
        state.highTimerActive = false;
        state.sustainedArmed = false;
        state.burstActive = false;
        state.burstShots.clear();
        state.burstIndex = 0;
        state.nitrousAudio.clear();
        state.previousNearLimit = false;
        state.previousShiftInProgress = snapshot.shiftInProgress;
        state.shiftPhase = VehicleState::ShiftPhase::Idle;
        state.shiftDirection = ShiftDirection::None;
        state.upshiftLimitLatched = false;
        state.downshiftLimitLatched = false;
        state.shiftCycleHandled = false;
        state.previousShiftEvent = snapshot.shiftEvent;
        state.previousGearChanged = snapshot.gearChanged;
        state.previousGear = snapshot.gear;
        state.previousGearValid = true;
        if (snapshot.driverControlsValid) {
            state.nitrousStateValid = true;
            state.previousNitrousActive = snapshot.nitrousActive;
        }
        return;
    }

    const bool shiftPulse =
        snapshot.shiftEvent && !state.previousShiftEvent;
    const bool gearChangedPulse =
        snapshot.gearChanged && !state.previousGearChanged;
    const bool gearValueChanged =
        state.previousGearValid && snapshot.gear != state.previousGear;
    const bool shiftSignal = shiftPulse || gearChangedPulse || gearValueChanged;
    const bool nearLimit = isNearLimit(snapshot);
    const bool downshiftRpm = isAtDownshiftRpm(snapshot);
    const bool sustainedLimit = isAtSustainedLimit(snapshot);

    ShiftDirection observedDirection = snapshot.shiftDirection;
    if (observedDirection == ShiftDirection::None && gearValueChanged) {
        observedDirection = snapshot.gear > state.previousGear
                                ? ShiftDirection::Up
                                : ShiftDirection::Down;
    }

    const bool enteringShift =
        snapshot.shiftInProgress && !state.previousShiftInProgress;
    const bool gearSignal = gearChangedPulse || gearValueChanged;
    bool closeShiftCycleAfterFrame = false;

    auto beginShiftCycle = [&](VehicleState::ShiftPhase phase) {
        state.shiftPhase = phase;
        state.shiftDirection = observedDirection;
        state.upshiftLimitLatched = nearLimit || state.previousNearLimit;
        state.downshiftLimitLatched = downshiftRpm;
        state.shiftCycleHandled = false;
    };

    switch (state.shiftPhase) {
        case VehicleState::ShiftPhase::Idle:
            if (enteringShift) {
                beginShiftCycle(VehicleState::ShiftPhase::Active);
            } else if (shiftPulse || gearSignal) {
                beginShiftCycle(VehicleState::ShiftPhase::Lead);
            }
            break;

        case VehicleState::ShiftPhase::Lead:
            if (snapshot.shiftInProgress) {
                state.shiftPhase = VehicleState::ShiftPhase::Active;
            } else if (!snapshot.shiftEvent && !snapshot.gearChanged &&
                       !gearSignal) {
                closeShiftCycleAfterFrame = true;
            }
            break;

        case VehicleState::ShiftPhase::Active:
            if (!snapshot.shiftInProgress) {
                /* Keep a one-frame tail so a delayed gear signal remains
                 * part of this shift instead of opening another burst. */
                state.shiftPhase = VehicleState::ShiftPhase::Tail;
            }
            break;

        case VehicleState::ShiftPhase::Tail:
            if (shiftPulse || enteringShift) {
                beginShiftCycle(enteringShift
                                    ? VehicleState::ShiftPhase::Active
                                    : VehicleState::ShiftPhase::Lead);
            } else if (!snapshot.gearChanged && !gearSignal) {
                closeShiftCycleAfterFrame = true;
            }
            break;
    }

    if (state.shiftPhase != VehicleState::ShiftPhase::Idle) {
        state.upshiftLimitLatched =
            state.upshiftLimitLatched || nearLimit;
        if (observedDirection != ShiftDirection::None) {
            if (state.shiftDirection == ShiftDirection::None &&
                observedDirection == ShiftDirection::Down) {
                state.downshiftLimitLatched = downshiftRpm;
            }
            state.shiftDirection = observedDirection;
        }
    }

    const bool directionKnown =
        state.shiftDirection != ShiftDirection::None;
    const bool shiftRpmEligible = config_.pairedShiftMode
        ? true
        : (state.shiftDirection == ShiftDirection::Down
               ? state.downshiftLimitLatched
               : state.upshiftLimitLatched);
    if (state.shiftPhase != VehicleState::ShiftPhase::Idle && directionKnown &&
        !state.shiftCycleHandled) {
        /* One probability decision per physical shift cycle. Later signals
         * cannot retry a miss or append a duplicate burst. */
        state.shiftCycleHandled = true;
        const float probability = config_.pairedShiftMode
            ? (state.shiftDirection == ShiftDirection::Down
                   ? config_.pairedDownshiftProbability
                   : config_.shiftSimultaneousProbability +
                         config_.shiftSequentialProbability)
            : shiftTriggerProbabilityForMaxRpm(
                  snapshot.maxRpm, state.shiftDirection, config_);
        VehicleState::PairedPattern pattern = VehicleState::PairedPattern::None;
        bool triggered = false;
        if (shiftRpmEligible) {
            if (config_.pairedShiftMode) {
                pattern = chooseShiftPattern(probability);
                triggered = pattern != VehicleState::PairedPattern::None;
            } else {
                triggered = shouldTrigger(snapshot, state.shiftDirection);
            }
        }
        const char* patternName =
            pattern == VehicleState::PairedPattern::Simultaneous
                ? "SIMULTANEOUS"
                : (pattern == VehicleState::PairedPattern::Sequential
                       ? "SEQUENTIAL"
                       : "NONE");
        char message[288]{};
        std::snprintf(
            message, sizeof(message),
            "SHIFT_DECISION vehicle=%u direction=%s rpm=%.1f redline=%.1f "
            "controls=%d gas=%.3f brake=%.3f handbrake=%.3f eligible=%d "
            "probability=%.3f pattern=%s triggered=%d",
            snapshot.id,
            state.shiftDirection == ShiftDirection::Down ? "DOWN" : "UP",
            static_cast<double>(snapshot.rpm),
            static_cast<double>(snapshot.redlineRpm),
            snapshot.driverControlsValid ? 1 : 0,
            static_cast<double>(snapshot.gasInput),
            static_cast<double>(snapshot.brakeInput),
            static_cast<double>(snapshot.handBrakeInput),
            shiftRpmEligible ? 1 : 0, static_cast<double>(probability),
            patternName, triggered ? 1 : 0);
        bridge_.log(message);
        if (triggered) {
            if (config_.pairedShiftMode) {
                startPairedShift(state, nowMs, pattern);
            } else {
                startBurst(state, snapshot, nowMs);
            }
        }
    }

    if (snapshot.driverControlsValid) {
        if (!state.nitrousStateValid) {
            state.nitrousStateValid = true;
            state.previousNitrousActive = snapshot.nitrousActive;
        } else if (snapshot.nitrousActive != state.previousNitrousActive) {
            startNitrousAudio(state, snapshot, nowMs,
                              snapshot.nitrousActive);
            state.previousNitrousActive = snapshot.nitrousActive;
        }
    }

    const bool shifting =
        state.shiftPhase != VehicleState::ShiftPhase::Idle || shiftSignal;

    if (shifting) {
        state.highTimerActive = false;
        state.sustainedArmed = false;
    } else if (!state.sustainedArmed) {
        if (sustainedLimit) {
            if (!state.highTimerActive) {
                state.highTimerActive = true;
                state.highSinceMs = nowMs;
            }
            if (nowMs - state.highSinceMs >=
                config_.sustainedThresholdMs) {
                state.sustainedArmed = true;
                state.nextSustainedAtMs = nowMs;
            }
        } else {
            /* The first second must be continuously at the tighter limit. */
            state.highTimerActive = false;
        }
    } else if (!sustainedLimit) {
        /* Sustained fire is valid only while the limiter condition remains. */
        state.highTimerActive = false;
        state.sustainedArmed = false;
    }

    if (state.burstActive) {
        serviceBurst(state, snapshot, nowMs);
    }
    if (!state.burstActive && state.sustainedArmed) {
        serviceSustained(state, snapshot, nowMs);
    }
    serviceNitrousAudio(state, snapshot, nowMs);

    state.previousShiftEvent = snapshot.shiftEvent;
    state.previousGearChanged = snapshot.gearChanged;
    state.previousGear = snapshot.gear;
    state.previousGearValid = true;
    state.previousNearLimit = nearLimit;
    if (closeShiftCycleAfterFrame) {
        state.shiftPhase = VehicleState::ShiftPhase::Idle;
        state.shiftDirection = ShiftDirection::None;
        state.upshiftLimitLatched = false;
        state.downshiftLimitLatched = false;
        state.shiftCycleHandled = false;
    }
    state.previousShiftInProgress = snapshot.shiftInProgress;
}

void ExhaustController::startBurst(VehicleState& state,
                                   const VehicleSnapshot& snapshot,
                                   std::uint64_t nowMs) {
    (void)snapshot;
    const bool downshift = state.shiftDirection == ShiftDirection::Down;
    const std::uint32_t shots = downshift
                                    ? randomBetween(
                                          config_.downshiftBurstMinShots,
                                          config_.downshiftBurstMaxShots)
                                    : randomBetween(config_.burstMinShots,
                                                    config_.burstMaxShots);
    if (state.burstIndex != 0) {
        state.burstShots.erase(
            state.burstShots.begin(),
            state.burstShots.begin() +
                static_cast<std::ptrdiff_t>(state.burstIndex));
        state.burstIndex = 0;
    }
    state.burstShots.reserve(state.burstShots.size() + shots);
    const std::uint32_t window = config_.burstWindowMs;
    const std::uint32_t activeWindow =
        std::max(shots - 1u, (window * 3u) / 4u);
    const std::uint32_t positions = activeWindow + 1u;

    /* Disjoint random time buckets spread the shots while leaving the final
     * quarter of the 500 ms window as low-frame-rate catch-up headroom. */
    for (std::uint32_t i = 0; i < shots; ++i) {
        const std::uint32_t segmentStart = (i * positions) / shots;
        const std::uint32_t segmentEnd =
            ((i + 1u) * positions) / shots - 1u;
        const std::uint32_t offset = randomBetween(segmentStart, segmentEnd);
        state.burstShots.push_back(
            VehicleState::ScheduledShot{nowMs + offset, nowMs + window});
    }
    std::sort(state.burstShots.begin(), state.burstShots.end(),
              [](const VehicleState::ScheduledShot& left,
                 const VehicleState::ScheduledShot& right) {
                  if (left.dueMs != right.dueMs) return left.dueMs < right.dueMs;
                  return left.deadlineMs < right.deadlineMs;
              });
    state.burstIndex = 0;
    state.burstActive = !state.burstShots.empty();
}

void ExhaustController::startPairedShift(
    VehicleState& state, std::uint64_t nowMs,
    VehicleState::PairedPattern pattern) {
    if (pattern == VehicleState::PairedPattern::None) return;
    if (state.burstIndex != 0) {
        state.burstShots.erase(
            state.burstShots.begin(),
            state.burstShots.begin() +
                static_cast<std::ptrdiff_t>(state.burstIndex));
        state.burstIndex = 0;
    }

    ExhaustSide first = (nextRandom() & 1u) == 0u
                            ? ExhaustSide::Left
                            : ExhaustSide::Right;
    const ExhaustSide second = first == ExhaustSide::Left
                                   ? ExhaustSide::Right
                                   : ExhaustSide::Left;
    const std::uint64_t secondAt =
        pattern == VehicleState::PairedPattern::Sequential
            ? nowMs + config_.pairedSideDelayMs
            : nowMs;
    const std::uint64_t deadline =
        secondAt + std::max<std::uint32_t>(250u, config_.pairedSideDelayMs);
    ++state.nextSequenceId;
    if (state.nextSequenceId == 0) ++state.nextSequenceId;
    const FlamePattern flamePattern =
        pattern == VehicleState::PairedPattern::Sequential
            ? FlamePattern::Sequential
            : FlamePattern::Simultaneous;
    state.burstShots.push_back(
        VehicleState::ScheduledShot{nowMs, deadline, first, true,
                                    flamePattern, state.nextSequenceId});
    state.burstShots.push_back(
        VehicleState::ScheduledShot{secondAt, deadline, second, true,
                                    flamePattern, state.nextSequenceId});
    std::stable_sort(
        state.burstShots.begin(), state.burstShots.end(),
        [](const VehicleState::ScheduledShot& left,
           const VehicleState::ScheduledShot& right) {
            return left.dueMs < right.dueMs;
        });
    state.burstActive = true;
}

void ExhaustController::serviceBurst(VehicleState& state,
                                     const VehicleSnapshot& snapshot,
                                     std::uint64_t nowMs) {
    while (state.burstActive && state.burstIndex < state.burstShots.size() &&
           state.burstShots[state.burstIndex].deadlineMs < nowMs) {
        ++state.burstIndex;
    }

    /* Paired mode deliberately permits one left and one right request in the
     * same frame. Legacy random bursts remain limited to one shot per frame. */
    while (state.burstActive && state.burstIndex < state.burstShots.size() &&
           state.burstShots[state.burstIndex].dueMs <= nowMs) {
        const VehicleState::ScheduledShot shot =
            state.burstShots[state.burstIndex];
        if (!shot.fixedSide && state.hasEmitted &&
            state.lastEmissionMs == nowMs) {
            break;
        }
        const ExhaustSide side =
            shot.fixedSide ? shot.side : chooseSide(snapshot);
        if (emitOne(state, snapshot, side, shot.dueMs, nowMs,
                    shot.pattern, shot.sequenceId)) {
            ++state.burstIndex;
        } else {
            break;
        }
    }
    if (state.burstIndex >= state.burstShots.size()) {
        state.burstActive = false;
        state.burstShots.clear();
        state.burstIndex = 0;
    }
}

void ExhaustController::startNitrousAudio(
    VehicleState& state, const VehicleSnapshot& snapshot,
    std::uint64_t nowMs, bool starting) {
    const float probability = starting ? config_.nitrousStartProbability
                                       : config_.nitrousEndProbability;
    const bool triggered = shouldTrigger(probability);
    const bool sequential =
        triggered && shouldTrigger(config_.nitrousSequentialProbability);
    char message[224]{};
    std::snprintf(
        message, sizeof(message),
        "NITROUS_DECISION vehicle=%u phase=%s probability=%.3f triggered=%d "
        "pattern=%s",
        snapshot.id, starting ? "START" : "END",
        static_cast<double>(probability), triggered ? 1 : 0,
        !triggered ? "NONE" : (sequential ? "SEQUENTIAL" : "SIMULTANEOUS"));
    bridge_.log(message);
    if (!triggered) return;

    const ExhaustSide first = (nextRandom() & 1u) == 0u
                                  ? ExhaustSide::Left
                                  : ExhaustSide::Right;
    const ExhaustSide second = first == ExhaustSide::Left
                                   ? ExhaustSide::Right
                                   : ExhaustSide::Left;
    state.nitrousAudio.push_back(
        VehicleState::ScheduledAudio{nowMs, first});
    state.nitrousAudio.push_back(VehicleState::ScheduledAudio{
        sequential ? nowMs + config_.pairedSideDelayMs : nowMs, second});
    std::stable_sort(
        state.nitrousAudio.begin(), state.nitrousAudio.end(),
        [](const VehicleState::ScheduledAudio& left,
           const VehicleState::ScheduledAudio& right) {
            return left.dueMs < right.dueMs;
        });
}

void ExhaustController::serviceNitrousAudio(
    VehicleState& state, const VehicleSnapshot& snapshot,
    std::uint64_t nowMs) {
    while (!state.nitrousAudio.empty() &&
           state.nitrousAudio.front().dueMs <= nowMs) {
        const VehicleState::ScheduledAudio request =
            state.nitrousAudio.front();
        state.nitrousAudio.erase(state.nitrousAudio.begin());
        emitAudio(snapshot, request.side, request.dueMs, nowMs);
    }
}

void ExhaustController::serviceSustained(VehicleState& state,
                                         const VehicleSnapshot& snapshot,
                                         std::uint64_t nowMs) {
    if (nowMs < state.nextSustainedAtMs) return;
    if (state.hasEmitted && state.lastEmissionMs == nowMs) return;

    const ExhaustSide side = chooseSide(snapshot);
    if (shouldTrigger(snapshot)) {
        emitOne(state, snapshot, side, nowMs, nowMs);
    }
    state.nextSustainedAtMs =
        nowMs + randomBetween(config_.sustainedMinIntervalMs,
                              config_.sustainedMaxIntervalMs);
}

bool ExhaustController::emitOne(VehicleState& state,
                                const VehicleSnapshot& snapshot,
                                ExhaustSide side,
                                std::uint64_t scheduledAtMs,
                                std::uint64_t nowMs,
                                FlamePattern pattern,
                                std::uint32_t sequenceId) {
    const ExhaustMarker& marker =
        side == ExhaustSide::Left ? snapshot.leftExhaust
                                  : snapshot.rightExhaust;
    if (!marker.present) return false;

    FlameRequest flame{};
    flame.vehicleId = snapshot.id;
    flame.scheduledAtMs = scheduledAtMs;
    flame.emittedAtMs = nowMs;
    flame.effectVariant = config_.effectVariant;
    flame.sequenceId = sequenceId;
    flame.effectId = config_.effectId.c_str();
    flame.side = side;
    flame.pattern = pattern;
    flame.marker = marker;
    if (!bridge_.spawnExhaustFlame(flame)) return false;

    if (shouldTrigger(config_.flameAudioProbability)) {
        emitAudio(snapshot, side, scheduledAtMs, nowMs);
    }

    state.hasEmitted = true;
    state.lastEmissionMs = nowMs;
    return true;
}

void ExhaustController::emitAudio(const VehicleSnapshot& snapshot,
                                  ExhaustSide side,
                                  std::uint64_t scheduledAtMs,
                                  std::uint64_t nowMs) {
    AudioRequest audio{};
    audio.vehicleId = snapshot.id;
    audio.scheduledAtMs = scheduledAtMs;
    audio.emittedAtMs = nowMs;
    audio.side = side;
    audio.marker = side == ExhaustSide::Left ? snapshot.leftExhaust
                                              : snapshot.rightExhaust;
    if (!audio.marker.present) return;
    const std::uint32_t clipIndex =
        randomBetween(0u, static_cast<std::uint32_t>(AudioBank::kClipCount - 1u));
    audio.cue = audioBank_.at(clipIndex);
    bridge_.playBackfireAudio(audio);
}

bool ExhaustController::shouldTrigger(const VehicleSnapshot& snapshot,
                                      ShiftDirection direction) {
    const float probability =
        shiftTriggerProbabilityForMaxRpm(snapshot.maxRpm, direction, config_);
    if (probability <= 0.0f) return false;
    if (probability >= 1.0f) return true;
    return std::bernoulli_distribution(probability)(rng_);
}

bool ExhaustController::shouldTrigger(const VehicleSnapshot& snapshot) {
    const bool neutralThrottle =
        snapshot.gear == kNeutralGear && snapshot.driverControlsValid &&
        snapshot.gasInput >= config_.neutralMinGasInput;
    return shouldTrigger(neutralThrottle ? config_.neutralSustainedProbability
                                        : config_.sustainedProbability);
}

bool ExhaustController::shouldTrigger(float probability) {
    if (probability <= 0.0f) return false;
    if (probability >= 1.0f) return true;
    return std::bernoulli_distribution(probability)(rng_);
}

ExhaustController::VehicleState::PairedPattern
ExhaustController::chooseShiftPattern(float eventProbability) {
    eventProbability = std::clamp(eventProbability, 0.0f, 1.0f);
    const float patternProbability = config_.shiftSimultaneousProbability +
                                     config_.shiftSequentialProbability;
    if (eventProbability <= 0.0f || patternProbability <= 0.0f) {
        return VehicleState::PairedPattern::None;
    }
    const float value = std::generate_canonical<float, 24>(rng_);
    if (value >= eventProbability) {
        return VehicleState::PairedPattern::None;
    }
    const float simultaneousThreshold = eventProbability *
        (config_.shiftSimultaneousProbability / patternProbability);
    if (value < simultaneousThreshold) {
        return VehicleState::PairedPattern::Simultaneous;
    }
    return VehicleState::PairedPattern::Sequential;
}

bool ExhaustController::isAtDownshiftRpm(
    const VehicleSnapshot& snapshot) const noexcept {
    if (!std::isfinite(snapshot.rpm) || snapshot.rpm < 0.0f ||
        !std::isfinite(snapshot.maxRpm) || snapshot.maxRpm <= 0.0f) {
        return false;
    }
    const float limit =
        std::isfinite(snapshot.redlineRpm) && snapshot.redlineRpm > 0.0f
            ? snapshot.redlineRpm
            : snapshot.maxRpm;
    return snapshot.rpm >= limit * config_.downshiftMinRpmRatio;
}

bool ExhaustController::isNearLimit(const VehicleSnapshot& snapshot) const noexcept {
    if (snapshot.atRedline || snapshot.atMaxRpm) return true;
    if (!std::isfinite(snapshot.rpm) ||
        !std::isfinite(snapshot.maxRpm) || snapshot.maxRpm <= 0.0f) {
        return false;
    }
    if (snapshot.redlineRpm > 0.0f &&
        snapshot.rpm >= snapshot.redlineRpm * config_.nearLimitRatio) {
        return true;
    }
    return snapshot.rpm >= snapshot.maxRpm * config_.nearLimitRatio;
}

bool ExhaustController::isAtSustainedLimit(
    const VehicleSnapshot& snapshot) const noexcept {
    if (snapshot.atRedline || snapshot.atMaxRpm) return true;
    if (!std::isfinite(snapshot.rpm) ||
        !std::isfinite(snapshot.maxRpm) || snapshot.maxRpm <= 0.0f) {
        return false;
    }
    if (std::isfinite(snapshot.redlineRpm) && snapshot.redlineRpm > 0.0f &&
        snapshot.rpm >=
            snapshot.redlineRpm * config_.sustainedLimitRatio) {
        return true;
    }
    return snapshot.rpm >=
           snapshot.maxRpm * config_.sustainedLimitRatio;
}

ExhaustSide ExhaustController::chooseSide(
    const VehicleSnapshot& snapshot) {
    const std::uint32_t random = nextRandom();
    const bool left = snapshot.leftExhaust.present;
    const bool right = snapshot.rightExhaust.present;
    if (!left) return ExhaustSide::Right;
    if (!right) return ExhaustSide::Left;
    return (random & 1u) == 0u ? ExhaustSide::Left : ExhaustSide::Right;
}

std::uint32_t ExhaustController::nextRandom() {
    return rng_();
}

std::uint32_t ExhaustController::randomBetween(std::uint32_t min,
                                               std::uint32_t max) {
    if (max <= min) return min;
    std::uniform_int_distribution<std::uint32_t> distribution(min, max);
    return distribution(rng_);
}

}  // namespace nfsmw_exhaust
