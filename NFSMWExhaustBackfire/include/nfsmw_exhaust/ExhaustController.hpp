#pragma once

#include "AudioBank.hpp"
#include "Config.hpp"
#include "IGameBridge.hpp"
#include "Types.hpp"

#include <cstdint>
#include <cstddef>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

namespace nfsmw_exhaust {

class ExhaustController {
public:
    explicit ExhaustController(IGameBridge& bridge,
                               ExhaustConfig config = ExhaustConfig{});
    ~ExhaustController();

    ExhaustController(const ExhaustController&) = delete;
    ExhaustController& operator=(const ExhaustController&) = delete;

    void tick(std::uint64_t nowMs);
    void tick(const VehicleSnapshot* snapshots, std::size_t count,
              std::uint64_t nowMs);
    void reset();

    const ExhaustConfig& config() const noexcept { return config_; }
    ExhaustConfig& config() noexcept { return config_; }
    bool loadAudioManifest(const char* path, std::string* error = nullptr) {
        return audioBank_.loadManifest(path, error);
    }
    std::size_t trackedVehicleCount() const noexcept { return states_.size(); }

private:
    struct VehicleState {
        enum class PairedPattern : std::uint8_t {
            None = 0,
            Simultaneous,
            Sequential,
        };

        enum class ShiftPhase : std::uint8_t {
            Idle = 0,
            Lead,
            Active,
            Tail,
        };

        struct ScheduledShot {
            std::uint64_t dueMs = 0;
            std::uint64_t deadlineMs = 0;
            ExhaustSide side = ExhaustSide::Left;
            bool fixedSide = false;
            FlamePattern pattern = FlamePattern::Standalone;
            std::uint32_t sequenceId = 0;
        };

        struct ScheduledAudio {
            std::uint64_t dueMs = 0;
            ExhaustSide side = ExhaustSide::Left;
        };

        bool initialized = false;
        bool highTimerActive = false;
        bool sustainedArmed = false;
        bool previousNearLimit = false;
        bool previousShiftInProgress = false;
        ShiftPhase shiftPhase = ShiftPhase::Idle;
        ShiftDirection shiftDirection = ShiftDirection::None;
        bool upshiftLimitLatched = false;
        bool downshiftLimitLatched = false;
        bool shiftCycleHandled = false;
        std::uint64_t highSinceMs = 0;
        std::uint64_t nextSustainedAtMs = 0;
        bool nitrousStateValid = false;
        bool previousNitrousActive = false;

        bool burstActive = false;
        std::vector<ScheduledShot> burstShots;
        std::size_t burstIndex = 0;
        std::vector<ScheduledAudio> nitrousAudio;

        bool hasEmitted = false;
        std::uint64_t lastEmissionMs = 0;
        std::uint64_t lastTickMs = 0;
        std::uint32_t nextSequenceId = 0;

        bool previousShiftEvent = false;
        bool previousGearChanged = false;
        bool previousGearValid = false;
        std::int32_t previousGear = 0;
        bool vanillaDisabled = false;
    };

    void processVehicle(const VehicleSnapshot& snapshot,
                        std::uint64_t nowMs);
    void expireMissingVehicles(const VehicleId* ids, std::size_t count);
    void startBurst(VehicleState& state, const VehicleSnapshot& snapshot,
                    std::uint64_t nowMs);
    void startPairedShift(VehicleState& state, std::uint64_t nowMs,
                          VehicleState::PairedPattern pattern);
    void startNitrousAudio(VehicleState& state,
                           const VehicleSnapshot& snapshot,
                           std::uint64_t nowMs, bool starting);
    void serviceBurst(VehicleState& state, const VehicleSnapshot& snapshot,
                      std::uint64_t nowMs);
    void serviceSustained(VehicleState& state,
                          const VehicleSnapshot& snapshot,
                          std::uint64_t nowMs);
    void serviceNitrousAudio(VehicleState& state,
                             const VehicleSnapshot& snapshot,
                             std::uint64_t nowMs);
    bool emitOne(VehicleState& state, const VehicleSnapshot& snapshot,
                 ExhaustSide side, std::uint64_t scheduledAtMs,
                 std::uint64_t nowMs,
                 FlamePattern pattern = FlamePattern::Standalone,
                 std::uint32_t sequenceId = 0);
    bool shouldTrigger(const VehicleSnapshot& snapshot);
    bool shouldTrigger(const VehicleSnapshot& snapshot,
                       ShiftDirection direction);
    bool shouldTrigger(float probability);
    VehicleState::PairedPattern chooseShiftPattern(float eventProbability);
    void emitAudio(const VehicleSnapshot& snapshot, ExhaustSide side,
                   std::uint64_t scheduledAtMs, std::uint64_t nowMs);
    bool isNearLimit(const VehicleSnapshot& snapshot) const noexcept;
    bool isAtDownshiftRpm(
        const VehicleSnapshot& snapshot) const noexcept;
    bool isAtSustainedLimit(
        const VehicleSnapshot& snapshot) const noexcept;
    ExhaustSide chooseSide(const VehicleSnapshot& snapshot);
    std::uint32_t nextRandom();
    std::uint32_t randomBetween(std::uint32_t min, std::uint32_t max);

    IGameBridge& bridge_;
    ExhaustConfig config_;
    AudioBank audioBank_;
    std::mt19937 rng_;
    std::unordered_map<VehicleId, VehicleState> states_;
};

}  // namespace nfsmw_exhaust
