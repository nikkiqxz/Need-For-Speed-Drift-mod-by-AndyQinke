#include "driver_assist.hpp"

#include <algorithm>
#include <cmath>

namespace nfsmw_drift_asi::driver_assist {
namespace {

constexpr float kMaximumDt = 0.1f;
constexpr float kDirectionDeadzone = 0.35f;
constexpr float kStraightDeadzone = 0.08f;
constexpr float kStraightDelaySeconds = 0.3f;
constexpr float kStraightRiseSeconds = 1.5f;
constexpr float kStraightFallSeconds = 0.25f;
constexpr float kStraightMaximumYawAccelerationRadS2 = 0.16f;
constexpr float kStraightMaximumTargetYawRadS = 0.08f;
constexpr float kDirectionChangeWindowSeconds = 2.0f;
constexpr int kRequiredDirectionChanges = 3;
constexpr float kRecoverySlipStartRad = 8.0f * 3.14159265358979323846f / 180.0f;
constexpr float kRecoverySlipStopRad = 2.0f * 3.14159265358979323846f / 180.0f;
constexpr float kRecoveryCenteredHoldSeconds = 0.25f;
constexpr float kRecoveryMaximumSeconds = 3.0f;
constexpr float kRecoveryRiseSeconds = 0.9f;
constexpr float kRecoveryFallSeconds = 0.3f;
constexpr float kRecoveryMaximumYawAccelerationRadS2 = 0.48f * 1.15f;
constexpr float kRecoveryMaximumTargetYawRadS = 0.28f;
constexpr float kRecoveryDirectOffsetRad =
    10.0f * 3.14159265358979323846f / 180.0f;
constexpr float kRecoveryMaximumAngleGainOffsetRad =
    35.0f * 3.14159265358979323846f / 180.0f;
constexpr float kThrottleStartThreshold = 0.35f;
constexpr float kThrottleResetThreshold = 0.15f;
constexpr float kTcsEpisodeSeconds = 1.8f;

float MoveTowards(float current, float target, float maximumDelta) noexcept {
    if (current < target) {
        return std::min(current + maximumDelta, target);
    }
    return std::max(current - maximumDelta, target);
}

float RecoveryAngleGain(float bodyOffsetRad) noexcept {
    const float range =
        kRecoveryMaximumAngleGainOffsetRad - kRecoveryDirectOffsetRad;
    const float normalized = std::clamp(
        (std::fabs(bodyOffsetRad) - kRecoveryDirectOffsetRad) / range,
        0.0f, 1.0f);
    const float smooth =
        normalized * normalized * (3.0f - 2.0f * normalized);
    return 1.0f + smooth;
}

}  // namespace

Controller::Controller(std::uint32_t randomSeed) noexcept
    : randomState_(randomSeed != 0 ? randomSeed : 0x53444641u) {}

void Controller::ResetOperationalState() noexcept {
    brakeHeldSeconds_ = 0.0f;
    neutralSteeringSeconds_ = 0.0f;
    straightRamp_ = 0.0f;
    lastSteeringDirection_ = 0;
    directionChanges_ = 0;
    directionChangeWindowSeconds_ = 0.0f;
    recoveryActive_ = false;
    recoverySeconds_ = 0.0f;
    recoveryCenteredSeconds_ = 0.0f;
    recoveryRamp_ = 0.0f;
    throttleEpisodeArmed_ = true;
    tcsRemainingSeconds_ = 0.0f;
    lcActive_ = false;
    lcStartedWithBrake_ = false;
    lcStartedWithHandbrake_ = false;
}

void Controller::Reset(bool initiallyActive) noexcept {
    modeActive_ = initiallyActive;
    waitingAfterDrift_ = !initiallyActive;
    reactivationSeconds_ = 0.0f;
    ResetOperationalState();
}

float Controller::NextRandomUnit() noexcept {
    std::uint32_t value = randomState_;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    randomState_ = value != 0 ? value : 0x53444641u;
    return static_cast<float>(randomState_ & 0x00FFFFFFu) /
           static_cast<float>(0x01000000u);
}

Output Controller::Update(const Input& input) noexcept {
    Output output{};
    if (!std::isfinite(input.dt) || input.dt <= 0.0f ||
        input.dt > kMaximumDt || !std::isfinite(input.speedMps) ||
        !std::isfinite(input.steering) || !std::isfinite(input.brake) ||
        !std::isfinite(input.throttle) ||
        !std::isfinite(input.handbrake) ||
        !std::isfinite(input.longitudinalSpeedMps) ||
        !std::isfinite(input.bodyOffsetRad) ||
        !std::isfinite(input.yawRateRadS)) {
        Reset(false);
        return output;
    }

    const float dt = input.dt;
    if (input.driftActive) {
        modeActive_ = false;
        waitingAfterDrift_ = true;
        reactivationSeconds_ = 0.0f;
        ResetOperationalState();
        return output;
    }
    if (waitingAfterDrift_) {
        reactivationSeconds_ += dt;
        if (reactivationSeconds_ + 1.0e-6f <
            kActivationDelayAfterDriftSeconds) {
            return output;
        }
        waitingAfterDrift_ = false;
        modeActive_ = true;
        ResetOperationalState();
    }
    if (!modeActive_) {
        return output;
    }
    output.modeActive = true;

    const float brake = std::clamp(input.brake, 0.0f, 1.0f);
    const float handbrake = std::clamp(input.handbrake, 0.0f, 1.0f);
    const float throttle = std::clamp(input.throttle, 0.0f, 1.0f);
    const bool throttleHeld = throttle >= kLaunchControlThrottleThreshold;
    const bool brakeHeld = brake >= kLaunchControlBrakeThreshold;
    const bool handbrakeHeld =
        handbrake >= kLaunchControlBrakeThreshold;
    const bool launchEntryEligible =
        input.speedMps < kLaunchControlMaximumSpeedMps &&
        input.longitudinalSpeedMps >= -0.1f && throttleHeld &&
        (brakeHeld || handbrakeHeld);
    if (!lcActive_ && launchEntryEligible) {
        lcActive_ = true;
        lcStartedWithBrake_ = brakeHeld;
        lcStartedWithHandbrake_ = handbrakeHeld;
        tcsRemainingSeconds_ = 0.0f;
        throttleEpisodeArmed_ = false;
    } else if (lcActive_) {
        const bool initiatingBrakeReleased =
            lcStartedWithBrake_ && !brakeHeld;
        const bool initiatingHandbrakeReleased =
            lcStartedWithHandbrake_ && !handbrakeHeld;
        if (!throttleHeld || initiatingBrakeReleased ||
            initiatingHandbrakeReleased) {
            lcActive_ = false;
            lcStartedWithBrake_ = false;
            lcStartedWithHandbrake_ = false;
        }
    }
    output.lcWorking = lcActive_;

    if (!lcActive_ && brake >= kAbsBrakeThreshold) {
        brakeHeldSeconds_ += dt;
        if (brakeHeldSeconds_ + 1.0e-6f >= kAbsHoldSeconds) {
            const float workingAge =
                std::max(0.0f, brakeHeldSeconds_ - kAbsHoldSeconds);
            const float fade = std::clamp(
                workingAge / kAbsFadeSeconds, 0.0f, 1.0f);
            const float envelope =
                1.0f + (kAbsMinimumEnvelope - 1.0f) * fade;
            output.absWorking = true;
            output.absDecelerationScale =
                kAbsDriftDecelerationFraction * envelope;
        }
    } else {
        brakeHeldSeconds_ = 0.0f;
    }

    const float steering = std::clamp(input.steering, -1.0f, 1.0f);
    const bool escSpeedEligible = input.speedMps > kMinimumEscSpeedMps;
    directionChangeWindowSeconds_ += dt;
    if (directionChanges_ > 0 &&
        directionChangeWindowSeconds_ > kDirectionChangeWindowSeconds) {
        directionChanges_ = 0;
        directionChangeWindowSeconds_ = 0.0f;
    }
    const int direction = std::fabs(steering) >= kDirectionDeadzone
                              ? (steering < 0.0f ? -1 : 1)
                              : 0;
    if (direction != 0) {
        if (lastSteeringDirection_ != 0 &&
            direction != lastSteeringDirection_) {
            if (directionChanges_ == 0 ||
                directionChangeWindowSeconds_ >
                    kDirectionChangeWindowSeconds) {
                directionChanges_ = 1;
                directionChangeWindowSeconds_ = 0.0f;
            } else {
                ++directionChanges_;
            }
        }
        lastSteeringDirection_ = direction;
    }

    if (!escSpeedEligible) {
        neutralSteeringSeconds_ = 0.0f;
        straightRamp_ = MoveTowards(
            straightRamp_, 0.0f, dt / kStraightFallSeconds);
        recoveryActive_ = false;
        recoverySeconds_ = 0.0f;
        recoveryCenteredSeconds_ = 0.0f;
        recoveryRamp_ = MoveTowards(
            recoveryRamp_, 0.0f, dt / kRecoveryFallSeconds);
        directionChanges_ = 0;
        directionChangeWindowSeconds_ = 0.0f;
        lastSteeringDirection_ = 0;
    } else {
        const bool rapidDirectionSlip =
            directionChanges_ >= kRequiredDirectionChanges &&
            std::fabs(input.bodyOffsetRad) >= kRecoverySlipStartRad;
        const bool directStabilityDemand =
            std::fabs(input.bodyOffsetRad) >= kRecoveryDirectOffsetRad ||
            input.fourWheelSlip;
        if (!recoveryActive_ &&
            (rapidDirectionSlip || directStabilityDemand)) {
            recoveryActive_ = true;
            recoverySeconds_ = 0.0f;
            recoveryCenteredSeconds_ = 0.0f;
        }
        if (recoveryActive_) {
            recoverySeconds_ += dt;
            if (std::fabs(input.bodyOffsetRad) <= kRecoverySlipStopRad) {
                recoveryCenteredSeconds_ += dt;
            } else {
                recoveryCenteredSeconds_ = 0.0f;
            }
            if (recoverySeconds_ >= kRecoveryMaximumSeconds ||
                recoveryCenteredSeconds_ >=
                    kRecoveryCenteredHoldSeconds) {
                recoveryActive_ = false;
                directionChanges_ = 0;
                directionChangeWindowSeconds_ = 0.0f;
            }
        }
        recoveryRamp_ = MoveTowards(
            recoveryRamp_, recoveryActive_ ? 1.0f : 0.0f,
            dt / (recoveryActive_ ? kRecoveryRiseSeconds
                                  : kRecoveryFallSeconds));

        if (std::fabs(steering) <= kStraightDeadzone) {
            neutralSteeringSeconds_ += dt;
        } else {
            neutralSteeringSeconds_ = 0.0f;
        }
        const bool straightRequested =
            neutralSteeringSeconds_ >= kStraightDelaySeconds &&
            !recoveryActive_;
        straightRamp_ = MoveTowards(
            straightRamp_, straightRequested ? 1.0f : 0.0f,
            dt / (straightRequested ? kStraightRiseSeconds
                                    : kStraightFallSeconds));
    }

    output.escStraightWorking = straightRamp_ > 0.001f;
    output.escRecoveryWorking = recoveryRamp_ > 0.001f;
    output.escWorking = output.escStraightWorking ||
                        output.escRecoveryWorking;
    if (output.escRecoveryWorking) {
        const float targetYaw = std::clamp(
            -input.bodyOffsetRad * 0.8f,
            -kRecoveryMaximumTargetYawRadS,
            kRecoveryMaximumTargetYawRadS);
        const float manualAuthority =
            1.0f - 0.85f * std::fabs(steering);
        const float angleGain = RecoveryAngleGain(input.bodyOffsetRad);
        const float maximumStep =
            kRecoveryMaximumYawAccelerationRadS2 * angleGain * recoveryRamp_ *
            std::max(0.15f, manualAuthority) * dt;
        output.yawVelocityDeltaRadS = std::clamp(
            targetYaw - input.yawRateRadS, -maximumStep, maximumStep);
    } else if (output.escStraightWorking) {
        const float targetYaw = std::clamp(
            -input.bodyOffsetRad * 0.2f,
            -kStraightMaximumTargetYawRadS,
            kStraightMaximumTargetYawRadS);
        const float maximumStep =
            kStraightMaximumYawAccelerationRadS2 * straightRamp_ * dt;
        output.yawVelocityDeltaRadS = std::clamp(
            targetYaw - input.yawRateRadS, -maximumStep, maximumStep);
    }

    if (lcActive_) {
        tcsRemainingSeconds_ = 0.0f;
        output.tcsWorking = false;
        return output;
    }
    if (throttle <= kThrottleResetThreshold) {
        throttleEpisodeArmed_ = true;
    }
    if (throttleEpisodeArmed_ && throttle >= kThrottleStartThreshold) {
        throttleEpisodeArmed_ = false;
        if (NextRandomUnit() < TcsProbabilityForGear(input.gear)) {
            tcsRemainingSeconds_ = kTcsEpisodeSeconds;
        }
    }
    tcsRemainingSeconds_ = std::max(0.0f, tcsRemainingSeconds_ - dt);
    output.tcsWorking = output.escRecoveryWorking ||
                        tcsRemainingSeconds_ > 0.0f;
    return output;
}

}  // namespace nfsmw_drift_asi::driver_assist
