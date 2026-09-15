#pragma once

#include "drift_assist.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace nfsmw_drift_asi::alpha_limits {

constexpr float kMaximumYawRateRadS = 0.75f;
constexpr float kMaximumEntryYawRateRadS = 0.90f;
constexpr float kMaximumAssistYawAccelerationRadS2 = 4.0f;
constexpr float kMaximumFastYawAccelerationRadS2 = 6.0f;
constexpr float kMaximumManualSameDirectionYawRateRadS = 0.5625f;
constexpr float kMaximumManualOppositeYawRateRadS = 0.45f;
constexpr float kMaximumManualSameDirectionYawAccelerationRadS2 = 3.0f;
constexpr float kMaximumManualOppositeYawAccelerationRadS2 = 4.5f;
constexpr float kMaximumManualYawVelocityStepRadS = 0.15f;
constexpr float kMaximumAcquisitionYawAccelerationRadS2 = 10.0f;
constexpr float kMaximumControlledYawAccelerationRadS2 = 4.0f;
constexpr float kMaximumYawVelocityStepRadS = 0.20f;
constexpr float kMaximumAcquisitionSeconds = 0.25f;
constexpr float kMaximumBodyOffsetRad =
    nfsmw_drift::DegToRad(35.0f);
constexpr float kMaximumAcquisitionOffsetCutoffRad =
    nfsmw_drift::DegToRad(15.0f);
constexpr float kMaximumMeasuredBodyOffsetRateRadS = 20.0f;
constexpr float kMinimumMeasuredBodyOffsetChangeRad = 1.0e-6f;

struct YawLimitResult {
    float delta = 0.0f;
    float stepLimit = 0.0f;
    bool assistOnlySuppressed = false;
    bool targetCrossed = false;
    bool bodyOffsetLimited = false;
    bool yawRateLimited = false;
};

inline bool IsAssistOnly(nfsmw_drift::YawResponseMode mode) {
    return mode ==
               nfsmw_drift::YawResponseMode::ManualSameDirectionAssist ||
           mode == nfsmw_drift::YawResponseMode::AssistOnly ||
           mode == nfsmw_drift::YawResponseMode::EntryAssistOnly;
}

inline bool PendingYawMatchesLatestInput(
    nfsmw_drift::YawResponseMode mode,
    nfsmw_drift::DriftPhase phase,
    int driftSide,
    float rawSteering,
    float deadzone,
    bool entryBoostActive = false) {
    if (!std::isfinite(rawSteering) || !std::isfinite(deadzone)) {
        return false;
    }
    if (phase == nfsmw_drift::DriftPhase::Centering &&
        mode != nfsmw_drift::YawResponseMode::None) {
        return true;
    }
    const float normalizedDeadzone = nfsmw_drift::Clamp(
        deadzone, 0.0f, 1.0f);
    const int inputDirection = rawSteering < -normalizedDeadzone
                                   ? -1
                                   : (rawSteering > normalizedDeadzone ? 1 : 0);
    switch (mode) {
    case nfsmw_drift::YawResponseMode::ManualSameDirectionAssist:
        return (driftSide == -1 || driftSide == 1) &&
               inputDirection == driftSide;
    case nfsmw_drift::YawResponseMode::ManualOppositeRecovery:
        return (driftSide == -1 || driftSide == 1) &&
               inputDirection == -driftSide;
    case nfsmw_drift::YawResponseMode::AssistOnly:
    case nfsmw_drift::YawResponseMode::EntryAssistOnly:
        return (driftSide == -1 || driftSide == 1) &&
               inputDirection == driftSide;
    case nfsmw_drift::YawResponseMode::AcquireBase15:
        return (driftSide == -1 || driftSide == 1) &&
               inputDirection != -driftSide &&
               (!entryBoostActive || inputDirection == driftSide);
    case nfsmw_drift::YawResponseMode::AngleHold:
        return (driftSide == -1 || driftSide == 1) &&
               inputDirection == 0;
    case nfsmw_drift::YawResponseMode::SameDirection:
        return (driftSide == -1 || driftSide == 1) &&
               inputDirection == driftSide;
    case nfsmw_drift::YawResponseMode::DirectionChange:
        return (driftSide == -1 || driftSide == 1) &&
               inputDirection == -driftSide;
    case nfsmw_drift::YawResponseMode::Centering:
        return true;
    case nfsmw_drift::YawResponseMode::None:
        return false;
    }
    return false;
}

inline float MaximumYawRate(nfsmw_drift::YawResponseMode mode) {
    if (mode ==
        nfsmw_drift::YawResponseMode::ManualSameDirectionAssist) {
        return kMaximumManualSameDirectionYawRateRadS;
    }
    if (mode == nfsmw_drift::YawResponseMode::ManualOppositeRecovery) {
        return kMaximumManualOppositeYawRateRadS;
    }
    return mode == nfsmw_drift::YawResponseMode::EntryAssistOnly ||
                   mode == nfsmw_drift::YawResponseMode::AcquireBase15
               ? kMaximumEntryYawRateRadS
               : kMaximumYawRateRadS;
}

inline float MaximumYawAcceleration(nfsmw_drift::YawResponseMode mode) {
    switch (mode) {
    case nfsmw_drift::YawResponseMode::ManualSameDirectionAssist:
        return kMaximumManualSameDirectionYawAccelerationRadS2;
    case nfsmw_drift::YawResponseMode::ManualOppositeRecovery:
        return kMaximumManualOppositeYawAccelerationRadS2;
    case nfsmw_drift::YawResponseMode::AssistOnly:
        return kMaximumAssistYawAccelerationRadS2;
    case nfsmw_drift::YawResponseMode::EntryAssistOnly:
    case nfsmw_drift::YawResponseMode::SameDirection:
        return kMaximumFastYawAccelerationRadS2;
    case nfsmw_drift::YawResponseMode::AcquireBase15:
        return kMaximumAcquisitionYawAccelerationRadS2;
    case nfsmw_drift::YawResponseMode::AngleHold:
    case nfsmw_drift::YawResponseMode::DirectionChange:
    case nfsmw_drift::YawResponseMode::Centering:
        return kMaximumControlledYawAccelerationRadS2;
    case nfsmw_drift::YawResponseMode::None:
        return 0.0f;
    }
    return 0.0f;
}

inline float YawVelocityStepLimit(
    nfsmw_drift::YawResponseMode mode,
    float requestedAcceleration,
    float requestedVelocityDeltaLimit,
    float dt) {
    if (!std::isfinite(requestedAcceleration) ||
        !std::isfinite(requestedVelocityDeltaLimit) ||
        !std::isfinite(dt) || dt <= 0.0f ||
        mode == nfsmw_drift::YawResponseMode::None) {
        return 0.0f;
    }
    const float acceleration = nfsmw_drift::Clamp(
        requestedAcceleration, 0.0f, MaximumYawAcceleration(mode));
    const float requestedStep = nfsmw_drift::Clamp(
        requestedVelocityDeltaLimit, 0.0f,
        mode == nfsmw_drift::YawResponseMode::ManualSameDirectionAssist ||
                mode == nfsmw_drift::YawResponseMode::ManualOppositeRecovery
            ? kMaximumManualYawVelocityStepRadS
            : kMaximumYawVelocityStepRadS);
    return std::min(requestedStep, acceleration * dt);
}

inline YawLimitResult LimitYawDelta(
    float currentYawRate,
    float requestedDelta,
    float targetYawRate,
    float bodyOffset,
    float requestedAcceleration,
    float requestedVelocityDeltaLimit,
    nfsmw_drift::YawResponseMode mode,
    float dt) {
    YawLimitResult result{};
    if (!std::isfinite(currentYawRate) || !std::isfinite(requestedDelta) ||
        !std::isfinite(targetYawRate) || !std::isfinite(bodyOffset)) {
        return result;
    }

    result.stepLimit = YawVelocityStepLimit(
        mode, requestedAcceleration, requestedVelocityDeltaLimit, dt);
    result.delta = nfsmw_drift::Clamp(
        requestedDelta, -result.stepLimit, result.stepLimit);

    if (IsAssistOnly(mode)) {
        const float direction = targetYawRate > 0.0f
                                    ? 1.0f
                                    : (targetYawRate < 0.0f ? -1.0f : 0.0f);
        const float remaining = direction * (targetYawRate - currentYawRate);
        const float requestedAlongDirection = direction * result.delta;
        if (direction == 0.0f || remaining <= 0.0f ||
            requestedAlongDirection <= 0.0f) {
            result.delta = 0.0f;
            result.assistOnlySuppressed = true;
        } else {
            result.delta = direction *
                std::min(requestedAlongDirection, remaining);
        }
    }

    if (std::fabs(bodyOffset) >= kMaximumBodyOffsetRad &&
        result.delta * bodyOffset > 0.0f) {
        result.delta = 0.0f;
        result.bodyOffsetLimited = true;
    }

    const float maximumYawRate = MaximumYawRate(mode);
    const float candidateYawRate = currentYawRate + result.delta;
    if (std::fabs(candidateYawRate) > maximumYawRate &&
        std::fabs(candidateYawRate) > std::fabs(currentYawRate)) {
        result.yawRateLimited = true;
        if (std::fabs(currentYawRate) >= maximumYawRate) {
            result.delta = 0.0f;
        } else {
            result.delta = std::copysign(maximumYawRate,
                                         candidateYawRate) -
                           currentYawRate;
        }
    }
    return result;
}

inline bool PendingYawMotionEligible(
    float speedMps,
    float longitudinalSpeedMps,
    std::uint32_t groundedWheels,
    float minimumSpeedMps,
    float minimumLongitudinalSpeedMps,
    std::uint8_t minimumGroundedWheels) {
    return std::isfinite(speedMps) &&
           std::isfinite(longitudinalSpeedMps) &&
           std::isfinite(minimumSpeedMps) &&
           std::isfinite(minimumLongitudinalSpeedMps) &&
           speedMps >= std::max(0.0f, minimumSpeedMps) &&
           longitudinalSpeedMps >=
               std::max(0.0f, minimumLongitudinalSpeedMps) &&
           groundedWheels >= minimumGroundedWheels;
}

inline float LatestBodyOffsetRate(
    float currentBodyOffset,
    float sourceBodyOffset,
    float currentYawRate,
    float sourcePathYawRate,
    bool advancedPhysicsSerial,
    float dt) {
    if (!std::isfinite(currentBodyOffset) ||
        !std::isfinite(sourceBodyOffset) ||
        !std::isfinite(currentYawRate) ||
        !std::isfinite(sourcePathYawRate)) {
        return 0.0f;
    }
    const float yawPathFallback = nfsmw_drift::Clamp(
        currentYawRate - sourcePathYawRate,
        -kMaximumMeasuredBodyOffsetRateRadS,
        kMaximumMeasuredBodyOffsetRateRadS);
    if (advancedPhysicsSerial && std::isfinite(dt) && dt > 0.0f) {
        const float bodyOffsetChange = nfsmw_drift::WrapPi(
            currentBodyOffset - sourceBodyOffset);
        // Input polling can publish a command after the previous physics step,
        // then the next physics entry can consume it before pose integration.
        // In that ordering an unchanged angle is not evidence that residual
        // relative yaw is zero; keep the instantaneous yaw/path estimate so a
        // delayed centering command can still brake the pending rotation.
        if (std::fabs(bodyOffsetChange) >
            kMinimumMeasuredBodyOffsetChangeRad) {
            return nfsmw_drift::Clamp(
                bodyOffsetChange / dt,
                -kMaximumMeasuredBodyOffsetRateRadS,
                kMaximumMeasuredBodyOffsetRateRadS);
        }
    }
    return yawPathFallback;
}

inline YawLimitResult LimitVelocityRelativeYawDelta(
    float currentBodyOffsetRate,
    float requestedDelta,
    float targetBodyOffsetRate,
    float bodyOffset,
    float targetBodyOffset,
    float requestedAcceleration,
    float requestedVelocityDeltaLimit,
    nfsmw_drift::YawResponseMode mode,
    float dt) {
    YawLimitResult result{};
    if (!std::isfinite(currentBodyOffsetRate) ||
        !std::isfinite(requestedDelta) ||
        !std::isfinite(targetBodyOffsetRate) ||
        !std::isfinite(bodyOffset) ||
        !std::isfinite(targetBodyOffset)) {
        return result;
    }

    result.stepLimit = YawVelocityStepLimit(
        mode, requestedAcceleration, requestedVelocityDeltaLimit, dt);
    result.delta = nfsmw_drift::Clamp(
        requestedDelta, -result.stepLimit, result.stepLimit);
    if (result.delta == 0.0f) {
        return result;
    }

    const float maximumRate = MaximumYawRate(mode);
    const float boundedTargetRate = nfsmw_drift::Clamp(
        targetBodyOffsetRate, -maximumRate, maximumRate);
    if (boundedTargetRate != targetBodyOffsetRate) {
        result.yawRateLimited = true;
    }

    // The pending command was generated from an earlier body angle. If the
    // body crossed its target before the physics write, an old outward target
    // must not be applied on the new side of 15/32/0 degrees.
    const float targetError = nfsmw_drift::WrapPi(
        targetBodyOffset - bodyOffset);
    if ((targetError == 0.0f && boundedTargetRate != 0.0f) ||
        targetError * boundedTargetRate < 0.0f) {
        result.delta = 0.0f;
        result.targetCrossed = true;
        return result;
    }

    // Re-evaluate the rate error with the newest rigid-body yaw. This prevents
    // a one-frame-old delta from crossing the requested relative angular rate.
    const float remainingRate = boundedTargetRate - currentBodyOffsetRate;
    if (result.delta * remainingRate <= 0.0f) {
        result.delta = 0.0f;
        result.assistOnlySuppressed = IsAssistOnly(mode);
        return result;
    }
    if (std::fabs(result.delta) > std::fabs(remainingRate)) {
        result.delta = remainingRate;
    }

    // At the hard angle, allow braking and inward motion. Never let this write
    // create or increase an outward relative rate.
    if (std::fabs(bodyOffset) >= kMaximumBodyOffsetRad) {
        const float offsetDirection = std::copysign(1.0f, bodyOffset);
        const float currentOutwardRate =
            currentBodyOffsetRate * offsetDirection;
        const float candidateOutwardRate =
            (currentBodyOffsetRate + result.delta) * offsetDirection;
        const float maximumOutwardRate = std::max(0.0f, currentOutwardRate);
        if (candidateOutwardRate > maximumOutwardRate) {
            result.delta = offsetDirection *
                (maximumOutwardRate - currentOutwardRate);
            result.bodyOffsetLimited = true;
        }
    }

    const float candidateRate = currentBodyOffsetRate + result.delta;
    if (std::fabs(candidateRate) > maximumRate &&
        std::fabs(candidateRate) > std::fabs(currentBodyOffsetRate)) {
        result.yawRateLimited = true;
        if (std::fabs(currentBodyOffsetRate) >= maximumRate) {
            result.delta = 0.0f;
        } else {
            result.delta = std::copysign(maximumRate, candidateRate) -
                           currentBodyOffsetRate;
        }
    }
    return result;
}

inline nfsmw_drift::AssistConfig ClampConfig(
    nfsmw_drift::AssistConfig config) {
    config.bodyRotationSpeedRadS = std::min(
        config.bodyRotationSpeedRadS, kMaximumYawRateRadS);
    config.bodyOffsetHoldSpeedRadS = std::min(
        config.bodyOffsetHoldSpeedRadS, kMaximumYawRateRadS);
    config.directionChangeSpeedRadS = std::min(
        config.directionChangeSpeedRadS, kMaximumYawRateRadS);
    if (config.autoCenterSpeedRadS > 0.0f) {
        config.autoCenterSpeedRadS = std::min(
            config.autoCenterSpeedRadS, kMaximumYawRateRadS);
    }
    config.bodyYawAccelerationRadS2 = std::min(
        config.bodyYawAccelerationRadS2,
        kMaximumFastYawAccelerationRadS2);
    config.bodyOffsetHoldYawAccelerationRadS2 = std::min(
        config.bodyOffsetHoldYawAccelerationRadS2,
        kMaximumControlledYawAccelerationRadS2);
    config.bodyOffsetHoldMaximumYawAngularDelta = std::min(
        config.bodyOffsetHoldMaximumYawAngularDelta,
        kMaximumYawVelocityStepRadS);
    config.maximumYawAngularDelta = std::min(
        config.maximumYawAngularDelta,
        kMaximumYawVelocityStepRadS);
    config.directionChangeYawAccelerationRadS2 = std::min(
        config.directionChangeYawAccelerationRadS2,
        kMaximumControlledYawAccelerationRadS2);
    config.directionChangeMaximumYawAngularDelta = std::min(
        config.directionChangeMaximumYawAngularDelta,
        kMaximumYawVelocityStepRadS);
    config.autoCenterYawAccelerationRadS2 = std::min(
        config.autoCenterYawAccelerationRadS2,
        kMaximumControlledYawAccelerationRadS2);
    config.autoCenterMaximumYawAngularDelta = std::min(
        config.autoCenterMaximumYawAngularDelta,
        kMaximumYawVelocityStepRadS);
    config.entryYawBoostRateRadS = std::min(
        config.entryYawBoostRateRadS,
        kMaximumEntryYawRateRadS);
    config.entryYawBoostAccelerationRadS2 = std::min(
        config.entryYawBoostAccelerationRadS2,
        kMaximumFastYawAccelerationRadS2);
    config.entryYawBoostMaximumYawAngularDelta = std::min(
        config.entryYawBoostMaximumYawAngularDelta,
        kMaximumYawVelocityStepRadS);
    config.driftAngleEntrySeconds = std::min(
        config.driftAngleEntrySeconds,
        kMaximumAcquisitionSeconds);
    config.driftAngleEntrySpeedRadS = std::min(
        config.driftAngleEntrySpeedRadS,
        kMaximumEntryYawRateRadS);
    config.driftAngleEntryYawAccelerationRadS2 = std::min(
        config.driftAngleEntryYawAccelerationRadS2,
        kMaximumAcquisitionYawAccelerationRadS2);
    config.driftAngleEntryOffsetCutoffRad = std::min(
        config.driftAngleEntryOffsetCutoffRad,
        kMaximumAcquisitionOffsetCutoffRad);
    config.driftAngleEntryMaximumYawAngularDelta = std::min(
        config.driftAngleEntryMaximumYawAngularDelta,
        kMaximumYawVelocityStepRadS);
    config.maximumBodyOffsetRad = std::min(
        config.maximumBodyOffsetRad, kMaximumBodyOffsetRad);
    return config;
}

}  // namespace nfsmw_drift_asi::alpha_limits
