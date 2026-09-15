#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nfsmw_drift_asi::steering_response {

constexpr float kDriftResponseScale = 0.50f;
// The accepted 0.9.5 manual path uses a fixed 2.0 command/s slew. A complete
// -1 to +1 sweep takes one second; shorter corrections finish proportionally.
constexpr float kManualSteeringMaximumRatePerSecond = 2.0f;

// These bits mirror the direction journal maintained by the input bridge.
// Keeping the values here avoids making the limiter depend on the core
// controller header while allowing ownership arbitration to be tested in
// isolation.
constexpr std::uint8_t kNegativeSteeringDirectionObserved = 1u << 0;
constexpr std::uint8_t kPositiveSteeringDirectionObserved = 1u << 1;

// A single physics frame may produce several input-hook polls.  After a
// player takes over from automatic countersteer, a later neutral poll in that
// same frame is bookkeeping rather than a new zero target.  During the
// controller's explicit reengagement state the same rule applies across
// physics frames, after the one-frame direction journal has been cleared.
// Holding the last applied command here keeps the live bridge from creating
// an artificial countersteer -> zero -> countersteer jump.
constexpr bool HoldNeutralAfterManualTakeover(
    bool currentManualInput,
    bool manualInputObserved,
    bool automaticOutputStillActive,
    bool manualOverrideOutput,
    bool reengagementDelayOutput) {
    if (currentManualInput) {
        return false;
    }
    return reengagementDelayOutput ||
           (manualInputObserved &&
            (automaticOutputStillActive || manualOverrideOutput));
}

constexpr bool WaitingNeutralPassthrough(bool currentManualInput,
                                         bool manualInputObserved,
                                         bool waitingForDriftOutput) {
    return waitingForDriftOutput && !currentManualInput &&
           !manualInputObserved;
}

inline float ClampCommand(float command) {
    return std::isfinite(command)
               ? std::clamp(command, -1.0f, 1.0f)
               : 0.0f;
}

inline float ScaledMaximumRate(float referenceMaximumRatePerSecond) {
    if (!std::isfinite(referenceMaximumRatePerSecond) ||
        referenceMaximumRatePerSecond <= 0.0f) {
        return 0.0f;
    }
    return referenceMaximumRatePerSecond * kDriftResponseScale;
}

inline float MoveTowards(float current, float target, float maximumDelta) {
    const float difference = target - current;
    if (std::fabs(difference) <= maximumDelta) {
        return target;
    }
    return current + std::copysign(maximumDelta, difference);
}

// Limits only the command sent to the game. Drift-side detection, manual
// ownership and re-engagement timing must continue to use the raw input.
class Limiter {
public:
    void Reset() {
        valid_ = false;
        serial_ = 0;
        appliedCommand_ = 0.0f;
        serialStartCommand_ = 0.0f;
        mode_ = Mode::None;
        transitionStartCommand_ = 0.0f;
        transitionTargetCommand_ = 0.0f;
        transitionElapsedSeconds_ = 0.0f;
        transitionElapsedAtSerialStartSeconds_ = 0.0f;
        transitionDurationSeconds_ = 0.0f;
        commandBudgetConsumedThisSerial_ = false;
    }

    void BeginSession(float currentCommand, std::uint64_t physicsSerial) {
        valid_ = true;
        serial_ = physicsSerial;
        appliedCommand_ = ClampCommand(currentCommand);
        serialStartCommand_ = appliedCommand_;
        mode_ = Mode::None;
        transitionStartCommand_ = appliedCommand_;
        transitionTargetCommand_ = appliedCommand_;
        transitionElapsedSeconds_ = 0.0f;
        transitionElapsedAtSerialStartSeconds_ = 0.0f;
        transitionDurationSeconds_ = 0.0f;
        commandBudgetConsumedThisSerial_ = false;
    }

    // Re-anchor the visible command when automatic countersteer takes
    // ownership again.  The accepted 0.9.5 bridge did not continue a stale
    // manual transition through zero; it synchronized the limiter to the
    // automatic target at the ownership edge, then resumed the normal rate
    // path on subsequent physics frames.
    void SynchronizeAutomaticTakeover(float automaticCommand,
                                      std::uint64_t physicsSerial) {
        const float target = ClampCommand(automaticCommand);
        valid_ = true;
        serial_ = physicsSerial;
        appliedCommand_ = target;
        serialStartCommand_ = target;
        mode_ = Mode::Rate;
        transitionStartCommand_ = target;
        transitionTargetCommand_ = target;
        transitionElapsedSeconds_ = 0.0f;
        transitionElapsedAtSerialStartSeconds_ = 0.0f;
        transitionDurationSeconds_ = 0.0f;
        // The synchronization itself is the one command for this physics
        // serial. Repeated input polls must not spend another dt or rewind it.
        commandBudgetConsumedThisSerial_ = true;
    }

    float Advance(float targetCommand,
                  float elapsedSeconds,
                  float maximumRatePerSecond,
                  std::uint64_t physicsSerial) {
        if (!valid_) {
            BeginSession(targetCommand, physicsSerial);
            return appliedCommand_;
        }

        BeginSerial(physicsSerial);

        if (!std::isfinite(targetCommand) ||
            !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0f ||
            !std::isfinite(maximumRatePerSecond) ||
            maximumRatePerSecond <= 0.0f) {
            return appliedCommand_;
        }

        const float target = ClampCommand(targetCommand);
        if (commandBudgetConsumedThisSerial_) {
            return appliedCommand_;
        }
        const float maximumDelta = maximumRatePerSecond * elapsedSeconds;
        mode_ = Mode::Rate;
        appliedCommand_ = ClampCommand(MoveTowards(
            serialStartCommand_, target, maximumDelta));
        commandBudgetConsumedThisSerial_ = true;
        return appliedCommand_;
    }

    // Manual steering follows the 0.9.5 fixed-rate path.  The older
    // AdvanceOverDuration API remains available for compatibility with
    // standalone callers, but the live write path uses this method so a
    // short target distance is completed in proportion to its distance
    // instead of being stretched over the full compatibility duration.  This
    // keeps small countersteer corrections responsive and lets a pendulum
    // reversal cross its observation window.
    float AdvanceManual(float targetCommand,
                        float elapsedSeconds,
                        std::uint64_t physicsSerial) {
        if (!valid_) {
            BeginSession(targetCommand, physicsSerial);
            return appliedCommand_;
        }

        BeginSerial(physicsSerial);

        if (!std::isfinite(targetCommand) ||
            !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0f) {
            return appliedCommand_;
        }

        const float target = ClampCommand(targetCommand);
        if (commandBudgetConsumedThisSerial_) {
            return appliedCommand_;
        }

        mode_ = Mode::Rate;
        appliedCommand_ = ClampCommand(MoveTowards(
            serialStartCommand_, target,
            kManualSteeringMaximumRatePerSecond * elapsedSeconds));
        commandBudgetConsumedThisSerial_ = true;
        return appliedCommand_;
    }

    // Starts a fresh linear transition whenever the stable manual target
    // changes. The duration is independent of distance: a small remaining
    // angle and a full left-to-right reversal each take the configured time.
    // Repeated polls in one physics serial all use the same time budget.
    float AdvanceOverDuration(float targetCommand,
                              float elapsedSeconds,
                              float durationSeconds,
                              std::uint64_t physicsSerial) {
        if (!valid_) {
            BeginSession(targetCommand, physicsSerial);
            return appliedCommand_;
        }

        BeginSerial(physicsSerial);

        if (!std::isfinite(targetCommand) ||
            !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0f ||
            !std::isfinite(durationSeconds) || durationSeconds < 0.0f) {
            return appliedCommand_;
        }

        const float target = ClampCommand(targetCommand);
        if (durationSeconds <= 0.0f) {
            mode_ = Mode::Timed;
            transitionStartCommand_ = target;
            transitionTargetCommand_ = target;
            transitionElapsedSeconds_ = 0.0f;
            transitionElapsedAtSerialStartSeconds_ = 0.0f;
            transitionDurationSeconds_ = 0.0f;
            appliedCommand_ = target;
            commandBudgetConsumedThisSerial_ = true;
            return appliedCommand_;
        }

        constexpr float kTargetChangeTolerance = 1.0e-5f;
        const bool retarget =
            mode_ != Mode::Timed ||
            std::fabs(target - transitionTargetCommand_) >
                kTargetChangeTolerance ||
            std::fabs(durationSeconds - transitionDurationSeconds_) >
                kTargetChangeTolerance;
        if (retarget) {
            mode_ = Mode::Timed;
            transitionStartCommand_ = appliedCommand_;
            transitionTargetCommand_ = target;
            transitionElapsedSeconds_ = 0.0f;
            transitionElapsedAtSerialStartSeconds_ = 0.0f;
            transitionDurationSeconds_ = durationSeconds;
        }

        if (commandBudgetConsumedThisSerial_) {
            return appliedCommand_;
        }

        transitionElapsedSeconds_ = std::min(
            transitionElapsedAtSerialStartSeconds_ + elapsedSeconds,
            transitionDurationSeconds_);
        const float progress = std::clamp(
            transitionElapsedSeconds_ / transitionDurationSeconds_,
            0.0f,
            1.0f);
        appliedCommand_ = ClampCommand(
            transitionStartCommand_ +
            (transitionTargetCommand_ - transitionStartCommand_) * progress);
        commandBudgetConsumedThisSerial_ = true;
        return appliedCommand_;
    }

    bool valid() const { return valid_; }
    float appliedCommand() const { return appliedCommand_; }

private:
    enum class Mode : std::uint8_t {
        None,
        Rate,
        Timed,
    };

    void BeginSerial(std::uint64_t physicsSerial) {
        if (physicsSerial == serial_) {
            return;
        }
        serial_ = physicsSerial;
        serialStartCommand_ = appliedCommand_;
        transitionElapsedAtSerialStartSeconds_ = transitionElapsedSeconds_;
        commandBudgetConsumedThisSerial_ = false;
    }

    bool valid_ = false;
    std::uint64_t serial_ = 0;
    float appliedCommand_ = 0.0f;
    float serialStartCommand_ = 0.0f;
    Mode mode_ = Mode::None;
    float transitionStartCommand_ = 0.0f;
    float transitionTargetCommand_ = 0.0f;
    float transitionElapsedSeconds_ = 0.0f;
    float transitionElapsedAtSerialStartSeconds_ = 0.0f;
    float transitionDurationSeconds_ = 0.0f;
    bool commandBudgetConsumedThisSerial_ = false;
};

constexpr bool AutomaticOverrideAllowed(bool currentManualInput,
                                         bool manualInputObserved,
                                         bool manualDirectionObserved,
                                         bool smartCountersteerActive,
                                         bool forceCountersteer) {
    return smartCountersteerActive && forceCountersteer &&
           !currentManualInput && !manualInputObserved &&
           !manualDirectionObserved;
}

// Automatic countersteer is eligible only for a fully neutral sample.  Any
// valid player event, including a signed event that agrees with the automatic
// countersteer direction, is an immediate takeover request.  The direction
// mask remains part of the contract so hosts can journal input observed between
// physics updates, but its sign must never grant automatic ownership.
constexpr bool AutomaticOverrideAllowed(bool currentManualInput,
                                         bool manualInputObserved,
                                         std::uint8_t manualDirectionMask,
                                         int driftSide,
                                         bool smartCountersteerActive,
                                         bool forceCountersteer) {
    if (!smartCountersteerActive || !forceCountersteer) {
        return false;
    }
    if (driftSide != -1 && driftSide != 1) {
        return false;
    }
    const std::uint8_t knownMask =
        manualDirectionMask &
        (kNegativeSteeringDirectionObserved |
         kPositiveSteeringDirectionObserved);
    return !currentManualInput && !manualInputObserved && knownMask == 0;
}

// Compatibility overload for adapters that only journal whether a direction
// was seen, but not its sign.  Such an event cannot be safely associated with
// a drift side and therefore vetoes automatic ownership.
constexpr bool AutomaticOverrideAllowed(bool currentManualInput,
                                         bool manualInputObserved,
                                         bool manualDirectionObserved,
                                         int /*driftSide*/,
                                         bool smartCountersteerActive,
                                         bool forceCountersteer) {
    return AutomaticOverrideAllowed(currentManualInput,
                                    manualInputObserved,
                                    manualDirectionObserved,
                                    smartCountersteerActive,
                                    forceCountersteer);
}

enum class WriteMode : std::uint8_t {
    Passthrough,
    Automatic,
    Manual,
};

struct WriteRequest {
    bool sessionActive = false;
    bool currentManualInput = false;
    bool manualInputObserved = false;
    bool manualDirectionObserved = false;
    bool smartCountersteerActive = false;
    bool forceCountersteer = false;
    std::uint8_t manualDirectionMask = 0;
    float rawCommand = 0.0f;
    float sessionStartCommand = 0.0f;
    float automaticCommand = 0.0f;
    float elapsedSeconds = 0.0f;
    float automaticMaximumRatePerSecond = 0.0f;
    // Retained for adapter/source compatibility; the live manual path uses
    // kManualSteeringMaximumRatePerSecond instead of this legacy duration.
    float manualTransitionSeconds = 0.0f;
    // Waiting for the first eligible drift angle must pass raw neutral input
    // straight through. Re-engagement after a manual takeover is different:
    // it holds the last command actually written until automatic ownership
    // resumes, avoiding an artificial move to zero and back.
    bool holdNeutralCommand = false;
    // Optional side/lifecycle hints retained for bridge compatibility and
    // pendulum-transition arbitration. Zero keeps older adapters valid.
    int automaticSide = 0;
    bool sideTransitionActive = false;
    std::uint64_t physicsSerial = 0;
};

struct WriteResult {
    bool write = false;
    float command = 0.0f;
    WriteMode mode = WriteMode::Passthrough;
};

// Owns the one command trajectory that the game actually receives. Automatic
// and manual targets may use different slew rules, but neither is allowed to
// resume from a hidden, frozen command after a pass-through frame.
class SessionLimiter {
public:
    void Reset() {
        trajectory_.Reset();
        sessionValid_ = false;
        serial_ = 0;
        serialStartCommand_ = 0.0f;
        appliedCommand_ = 0.0f;
        lastMode_ = WriteMode::Passthrough;
        serialRequestValid_ = false;
        serialMode_ = WriteMode::Passthrough;
        serialTargetCommand_ = 0.0f;
        manualVetoThisSerial_ = false;
    }

    bool valid() const { return sessionValid_; }
    float appliedCommand() const { return appliedCommand_; }

    WriteResult Prepare(const WriteRequest& request) {
        WriteResult result{};
        result.command = ClampCommand(request.rawCommand);
        if (!request.sessionActive) {
            Reset();
            return result;
        }

        if (!sessionValid_) {
            sessionValid_ = true;
            serial_ = request.physicsSerial;
            appliedCommand_ = ClampCommand(request.sessionStartCommand);
            serialStartCommand_ = appliedCommand_;
            lastMode_ = WriteMode::Passthrough;
        } else if (request.physicsSerial < serial_) {
            // A restarted physics sequence cannot safely reuse the old wheel
            // command. Rebuild from this sample's trusted session start.
            Reset();
            sessionValid_ = true;
            serial_ = request.physicsSerial;
            appliedCommand_ = ClampCommand(request.sessionStartCommand);
            serialStartCommand_ = appliedCommand_;
        } else if (request.physicsSerial != serial_) {
            serial_ = request.physicsSerial;
            serialStartCommand_ = appliedCommand_;
            serialRequestValid_ = false;
            manualVetoThisSerial_ = false;
        }

        const bool manualEvent = request.currentManualInput ||
                                 request.manualInputObserved ||
                                 request.manualDirectionObserved;
        const bool automaticOwnershipAllowed = AutomaticOverrideAllowed(
            request.currentManualInput,
            request.manualInputObserved || request.manualDirectionObserved,
            request.manualDirectionMask, request.automaticSide,
            request.smartCountersteerActive, request.forceCountersteer);
        // Any player event revokes automatic ownership for the rest of this
        // physics serial.  This includes a signed event that agrees with the
        // automatic countersteer direction, so an intentional increase of
        // countersteer cannot be clamped by a later poll.
        const bool automaticCandidate =
            request.smartCountersteerActive && request.forceCountersteer;
        manualVetoThisSerial_ =
            manualVetoThisSerial_ ||
            (manualEvent && automaticCandidate && !automaticOwnershipAllowed) ||
            request.holdNeutralCommand;

        const bool automatic = automaticOwnershipAllowed &&
                               !manualVetoThisSerial_ &&
                               !request.sideTransitionActive;

        if (automatic) {
            result.mode = WriteMode::Automatic;
            result.command = AdvanceAutomatic(request);
            result.write = true;
            appliedCommand_ = result.command;
            lastMode_ = WriteMode::Automatic;
            return result;
        }

        if (request.currentManualInput) {
            result.mode = WriteMode::Manual;
            result.command = AdvanceManual(request);
            result.write = true;
            appliedCommand_ = result.command;
            lastMode_ = WriteMode::Manual;
            return result;
        }

        if (request.holdNeutralCommand) {
            // Hold the command that is already visible in game; do not invent
            // a zero target during the 0.2-second re-engagement window.
            result.write = true;
            result.command = appliedCommand_;
            result.mode = WriteMode::Manual;
            lastMode_ = WriteMode::Manual;
            return result;
        }

        // Initial waiting-angle and neutral side-transition frames are true
        // pass-through. Synchronize to raw because it is now the command the
        // game received, and retire every old automatic/manual trajectory.
        trajectory_.Reset();
        appliedCommand_ = ClampCommand(request.rawCommand);
        serialStartCommand_ = appliedCommand_;
        serialRequestValid_ = false;
        lastMode_ = WriteMode::Passthrough;
        return result;
    }

private:
    bool SerialTargetChanged(WriteMode mode, float target) const {
        constexpr float kTargetChangeTolerance = 1.0e-5f;
        return serialRequestValid_ &&
               (serialMode_ != mode ||
                std::fabs(serialTargetCommand_ - target) >
                    kTargetChangeTolerance);
    }

    void PrepareTrajectory(WriteMode mode, float target) {
        // The input hook can poll more than once during one physics frame. A
        // late target/mode change intentionally keeps the command already
        // written to the game; Limiter's per-serial budget then rejects a
        // second dt. Re-seeding from serialStartCommand_ would rewind an
        // automatic countersteer toward zero before that second step.
        if (!SerialTargetChanged(mode, target) && !trajectory_.valid()) {
            trajectory_.BeginSession(appliedCommand_, serial_);
        }
        serialRequestValid_ = true;
        serialMode_ = mode;
        serialTargetCommand_ = target;
    }

    float AdvanceAutomatic(const WriteRequest& request) {
        const float target = ClampCommand(request.automaticCommand);
        PrepareTrajectory(WriteMode::Automatic, target);
        return trajectory_.Advance(
            target, request.elapsedSeconds,
            request.automaticMaximumRatePerSecond,
            request.physicsSerial);
    }

    float AdvanceManual(const WriteRequest& request) {
        const float target = ClampCommand(request.rawCommand);
        PrepareTrajectory(WriteMode::Manual, target);
        // 0.9.5's manual path is a fixed command-rate slew.  Do not use the
        // compatibility timed-transition API here: it makes even a tiny
        // remaining countersteer angle take the entire configured duration.
        return trajectory_.AdvanceManual(
            target, request.elapsedSeconds, request.physicsSerial);
    }

    Limiter trajectory_{};
    bool sessionValid_ = false;
    std::uint64_t serial_ = 0;
    float serialStartCommand_ = 0.0f;
    float appliedCommand_ = 0.0f;
    WriteMode lastMode_ = WriteMode::Passthrough;
    bool serialRequestValid_ = false;
    WriteMode serialMode_ = WriteMode::Passthrough;
    float serialTargetCommand_ = 0.0f;
    bool manualVetoThisSerial_ = false;
};

inline WriteResult PrepareWrite(SessionLimiter* limiter,
                                const WriteRequest& request) {
    if (limiter == nullptr) {
        WriteResult result{};
        result.command = ClampCommand(request.rawCommand);
        return result;
    }
    return limiter->Prepare(request);
}

// Tracks the last distinct physics frame so a drift session that begins on
// the same frame as a digital steering edge starts from the prior command.
class PreviousCommandTracker {
public:
    void Reset() {
        valid_ = false;
        serial_ = 0;
        previousCommand_ = 0.0f;
        currentCommand_ = 0.0f;
    }

    void Observe(float command, std::uint64_t physicsSerial) {
        const float bounded = ClampCommand(command);
        if (!valid_ || physicsSerial < serial_) {
            valid_ = true;
            serial_ = physicsSerial;
            previousCommand_ = bounded;
            currentCommand_ = bounded;
            return;
        }
        if (physicsSerial != serial_) {
            previousCommand_ = currentCommand_;
            serial_ = physicsSerial;
        }
        currentCommand_ = bounded;
    }

    float PreviousCommand(std::uint64_t physicsSerial,
                          float fallback) const {
        return valid_ && serial_ == physicsSerial
                   ? previousCommand_
                   : ClampCommand(fallback);
    }

private:
    bool valid_ = false;
    std::uint64_t serial_ = 0;
    float previousCommand_ = 0.0f;
    float currentCommand_ = 0.0f;
};

}  // namespace nfsmw_drift_asi::steering_response
