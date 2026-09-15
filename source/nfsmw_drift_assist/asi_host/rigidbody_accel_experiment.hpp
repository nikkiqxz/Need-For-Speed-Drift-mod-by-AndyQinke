#pragma once

// Safety and planning helpers for the optional forward-body acceleration
// experiment.  The helpers are deliberately independent of Win32 and of the
// game object ABI so the gate can be unit-tested without loading speed.exe.

#include "drift_assist.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace nfsmw_drift_asi::rigidbody_accel {

constexpr float kMinimumForwardLength = 0.85f;
constexpr float kMaximumForwardLength = 1.15f;
constexpr float kMinimumThrottle = 0.05f;
constexpr float kMaximumThrottle = 1.0f;
constexpr float kMaximumBrakeForAcceleration = 0.05f;
constexpr float kMaximumPlanDt = 0.05f;
constexpr float kMaximumPlanSpeedMps = 110.0f;
constexpr float kMaximumPlanVectorMagnitude = 1000.0f;
constexpr float kMaximumTargetAccelerationMps2 = 12.0f;
// Release fallback strength. These values are deliberately kept in the
// portable planning header so the bridge, tests, and release notes cannot
// drift apart. The 5.25 m/s^2 value is the nominal profile target; the
// release output is intentionally attenuated to 61.2% across every speed band.
// The prior 72% release scale was 0.80 * 0.90; this release applies the
// requested additional 15% relative reduction.
constexpr float kConfiguredTargetAccelerationMps2 = 5.25f;
constexpr float kPreviousReleaseStrengthScale = 0.80f;
constexpr float kCallbackReductionScale = 0.90f;
constexpr float kAdditionalStrengthReductionScale = 0.85f;
// Apply the reductions multiplicatively: 0.80 * 0.90 * 0.85 = 0.612. This
// keeps the requested reduction consistent at every speed-fade knot rather
// than subtracting a fixed acceleration.
constexpr float kConfiguredStrengthScale =
    kPreviousReleaseStrengthScale * kCallbackReductionScale *
    kAdditionalStrengthReductionScale;
constexpr float kConfiguredEffectiveTargetAccelerationMps2 =
    kConfiguredTargetAccelerationMps2 * kConfiguredStrengthScale;
constexpr float kMaximumDecelerationToAccelerationRatio = 1.15f;
constexpr float kConfiguredMaximumDecelerationMps2 =
    kConfiguredEffectiveTargetAccelerationMps2 *
    kMaximumDecelerationToAccelerationRatio;
constexpr float kMinimumDecelerationStrengthScale = 0.15f;
constexpr float kHandbrakeDecelerationHoldSeconds = 0.20f;
constexpr float kDecelerationMinimumSpeedMps =
    100.0f * (1.0f / 3.6f);
constexpr float kDecelerationMaximumSpeedMps =
    180.0f * (1.0f / 3.6f);
constexpr float kMinimumVelocityForDecelerationMps = 0.10f;
static_assert(kConfiguredStrengthScale > 0.0f &&
                  kConfiguredStrengthScale <= 1.0f,
              "rigid-body release strength scale must be in (0, 1]");
constexpr float kMaximumDeltaVPerTickMps = 0.35f;
constexpr float kSpeedSoftLimitBandMps = 15.0f;
constexpr float kKilometersPerHourToMetersPerSecond = 1.0f / 3.6f;
constexpr float kSpeedFadeFullMps = 70.0f * kKilometersPerHourToMetersPerSecond;
constexpr float kSpeedFadeHalfMps = 125.0f * kKilometersPerHourToMetersPerSecond;
constexpr float kSpeedFadeLowMps = 150.0f * kKilometersPerHourToMetersPerSecond;
constexpr float kSpeedFadeZeroMps = 170.0f * kKilometersPerHourToMetersPerSecond;
constexpr float kAppliedDeltaProjectedRelativeTolerance = 0.05f;
constexpr float kAppliedDeltaMagnitudeRelativeTolerance = 0.05f;
constexpr float kAppliedDeltaOrthogonalRelativeTolerance = 0.02f;
constexpr float kAppliedDeltaAbsoluteToleranceMps = 1.0e-5f;
constexpr float kAppliedDeltaFloatRoundoffUlps = 16.0f;
constexpr float kAppliedDeltaMinimumObservedFraction = 0.50f;

// The acceleration direction is blended in the vehicle's horizontal body
// frame.  A 90-degree midpoint would be tangent to the vehicle and makes a
// malformed steering value capable of producing a lateral impulse; keep a
// small margin below that limit and fail closed above it.
constexpr float kMinimumDirectionAxisLength = 1.0e-3f;
constexpr float kMaximumDirectionAxisLength = 1000.0f;
constexpr float kMaximumDirectionSteeringAngleRad =
    nfsmw_drift::DegToRad(89.0f);
constexpr float kDirectionBasisAlignmentMinimum = 0.75f;
constexpr float kSteeringCommandInputTolerance = 1.0e-4f;
// Retain only 25% of the previous half-angle deviation. Because the base
// calculation already bisects the wheel angle, the final acceleration
// direction deviates from the body heading by 12.5% of applied countersteer.
// Same-direction and centered steering remain aligned with the body heading.
constexpr float kCountersteerDirectionAngleScale = 0.25f;

enum class PlanMode : std::uint8_t {
    None,
    Acceleration,
    Deceleration,
};

constexpr PlanMode SelectPlanMode(bool decelerationEligible,
                                  bool accelerationEligible) {
    return decelerationEligible
               ? PlanMode::Deceleration
               : (accelerationEligible ? PlanMode::Acceleration
                                       : PlanMode::None);
}

enum class RejectReason : std::uint8_t {
    None,
    Disabled,
    SessionInactive,
    IdentityUnstable,
    PhysicsUnstable,
    NoGroundContact,
    NoThrottle,
    Braking,
    InvalidDt,
    InvalidForward,
    InvalidSteeringDirection,
    InvalidVelocity,
    ReverseMotion,
    SpeedLimit,
    InvalidTarget,
    HandbrakeNotHeld,
    ZeroDemand,
};

struct Inputs {
    bool enabled = false;
    bool sessionActive = false;
    bool identityStable = false;
    bool physicsStable = false;
    bool grounded = false;
    float throttle = 0.0f;
    float brake = 0.0f;
    float dt = 0.0f;
    // `forward` is always the body heading and remains the axis used by the
    // reverse-motion guard.  When useCountersteerDirection is true, the
    // planner derives the final acceleration direction from this heading,
    // bodyUp, and the command that was actually applied to the steering rows.
    nfsmw_drift::Vec3 forward{};
    nfsmw_drift::Vec3 bodyUp{};
    float appliedSteeringCommand = 0.0f;
    int driftSide = 0;
    float maximumSteerAngleRad = 0.0f;
    bool useCountersteerDirection = false;
    nfsmw_drift::Vec3 linearVelocity{};
    float targetAccelerationMps2 = 0.0f;
    float ramp = 0.0f;
};

struct DecelerationInputs {
    bool enabled = false;
    bool sessionActive = false;
    bool identityStable = false;
    bool physicsStable = false;
    bool grounded = false;
    float handbrake = 0.0f;
    float handbrakeThreshold = 0.5f;
    float handbrakeHeldSeconds = 0.0f;
    float dt = 0.0f;
    nfsmw_drift::Vec3 linearVelocity{};
    float maximumDecelerationMps2 =
        kConfiguredMaximumDecelerationMps2;
};

struct Plan {
    bool accepted = false;
    PlanMode mode = PlanMode::None;
    RejectReason reason = RejectReason::None;
    nfsmw_drift::Vec3 direction{};
    // The projection is retained for the reverse-motion guard and telemetry.
    // Speed attenuation and the safety ceiling use the full linear speed.
    float longitudinalSpeedMps = 0.0f;
    float speedMps = 0.0f;
    float speedScale = 0.0f;
    float targetAccelerationMps2 = 0.0f;
    float deltaVMps = 0.0f;
};

struct AppliedDeltaVerification {
    bool evaluated = false;
    bool inputsValid = false;
    bool projectedMatch = false;
    bool magnitudeMatch = false;
    bool orthogonalMatch = false;
    bool accepted = false;
    nfsmw_drift::Vec3 beforeVelocity{};
    nfsmw_drift::Vec3 afterVelocity{};
    nfsmw_drift::Vec3 actualDelta{};
    nfsmw_drift::Vec3 orthogonalDelta{};
    float plannedDeltaVMps = 0.0f;
    float actualDeltaVMps = 0.0f;
    float projectedDeltaVMps = 0.0f;
    float orthogonalResidualMps = 0.0f;
    float projectedToleranceMps = 0.0f;
    float magnitudeToleranceMps = 0.0f;
    float orthogonalToleranceMps = 0.0f;
};

constexpr bool SustainedDriftEligible(bool sessionActive,
                                      nfsmw_drift::DriftPhase phase,
                                      int driftSide) {
    return sessionActive && phase == nfsmw_drift::DriftPhase::Holding &&
           (driftSide == -1 || driftSide == 1);
}

inline bool FiniteVector(nfsmw_drift::Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

inline float VectorLength(nfsmw_drift::Vec3 value) {
    return std::sqrt(nfsmw_drift::Dot(value, value));
}

inline float MaximumAbsoluteComponent(nfsmw_drift::Vec3 value) {
    return std::max({std::fabs(value.x), std::fabs(value.y),
                     std::fabs(value.z)});
}

// Build the forward acceleration vector from the actual wheel command.  The
// command is normalized to [-1, 1], with positive values mapping to the
// vehicle's +right axis.  Drift-side signs follow the controller convention:
// +1 is a right-hand drift and -1 is a left-hand drift.  A wheel command is
// considered countersteer only when it points opposite the committed drift
// side.  Straight wheels and same-direction ("positive") steering therefore
// return the body heading exactly, which avoids injecting a sideways force
// while the player is tightening the drift.
inline bool ComputeCountersteerAccelerationDirectionFromAngle(
    nfsmw_drift::Vec3 forward,
    nfsmw_drift::Vec3 up,
    float appliedSteeringAngleRad,
    int driftSide,
    nfsmw_drift::Vec3* direction) {
    if (direction == nullptr) {
        return false;
    }
    *direction = nfsmw_drift::Vec3{};
    if (!FiniteVector(forward) || !FiniteVector(up) ||
        !std::isfinite(appliedSteeringAngleRad) ||
        std::fabs(appliedSteeringAngleRad) >
            kMaximumDirectionSteeringAngleRad ||
        (driftSide != -1 && driftSide != 1)) {
        return false;
    }

    const float forwardLength = VectorLength(forward);
    const float upLength = VectorLength(up);
    if (!std::isfinite(forwardLength) ||
        forwardLength < kMinimumDirectionAxisLength ||
        forwardLength > kMaximumDirectionAxisLength ||
        !std::isfinite(upLength) ||
        upLength < kMinimumDirectionAxisLength ||
        upLength > kMaximumDirectionAxisLength) {
        return false;
    }
    const nfsmw_drift::Vec3 heading = forward / forwardLength;

    // Remove pitch/roll contamination from the sampled up vector.  This
    // keeps the midpoint in the vehicle's ground-facing steering plane while
    // retaining the real body orientation for the horizontal axes.
    nfsmw_drift::Vec3 horizontalUp =
        up - heading * nfsmw_drift::Dot(up, heading);
    const float horizontalUpLength = VectorLength(horizontalUp);
    if (!std::isfinite(horizontalUpLength) ||
        horizontalUpLength < kMinimumDirectionAxisLength) {
        return false;
    }
    horizontalUp = horizontalUp / horizontalUpLength;
    nfsmw_drift::Vec3 right =
        nfsmw_drift::Cross(horizontalUp, heading);
    const float rightLength = VectorLength(right);
    if (!std::isfinite(rightLength) ||
        rightLength < kMinimumDirectionAxisLength) {
        return false;
    }
    right = right / rightLength;
    const float handedness =
        nfsmw_drift::Dot(nfsmw_drift::Cross(right, horizontalUp), heading);
    if (!std::isfinite(handedness) ||
        handedness < kDirectionBasisAlignmentMinimum) {
        return false;
    }

    // Keep a strict heading result for straight/same-direction steering.  In
    // particular, do not let a tiny sign change around zero create a lateral
    // acceleration due to floating-point noise.
    const bool isCountersteer =
        appliedSteeringAngleRad * static_cast<float>(driftSide) < 0.0f;
    if (!isCountersteer ||
        std::fabs(appliedSteeringAngleRad) <
            kSteeringCommandInputTolerance) {
        *direction = heading;
        return true;
    }

    const float halfAngle = appliedSteeringAngleRad * 0.5f *
                            kCountersteerDirectionAngleScale;
    const float cosine = std::cos(halfAngle);
    const float sine = std::sin(halfAngle);
    nfsmw_drift::Vec3 midpoint = heading * cosine + right * sine;
    const float midpointLength = VectorLength(midpoint);
    if (!std::isfinite(midpointLength) ||
        midpointLength < kMinimumDirectionAxisLength) {
        return false;
    }
    midpoint = midpoint / midpointLength;
    if (!FiniteVector(midpoint)) {
        return false;
    }
    *direction = midpoint;
    return true;
}

// Convert the limiter's actual normalized command to a physical steering
// angle, then use the common angle-based implementation above.  Keeping this
// conversion at the planner boundary ensures the rigid-body path follows the
// command that was really written to the game's steering rows rather than raw
// input or a stale controller target.
inline bool ComputeCountersteerAccelerationDirection(
    nfsmw_drift::Vec3 forward,
    nfsmw_drift::Vec3 up,
    float appliedSteeringCommand,
    int driftSide,
    float maximumSteerAngleRad,
    nfsmw_drift::Vec3* direction) {
    if (!std::isfinite(appliedSteeringCommand) ||
        appliedSteeringCommand < -1.0f - kSteeringCommandInputTolerance ||
        appliedSteeringCommand > 1.0f + kSteeringCommandInputTolerance ||
        !std::isfinite(maximumSteerAngleRad) ||
        maximumSteerAngleRad < 0.0f ||
        maximumSteerAngleRad > kMaximumDirectionSteeringAngleRad) {
        return false;
    }
    const float command =
        std::clamp(appliedSteeringCommand, -1.0f, 1.0f);
    return ComputeCountersteerAccelerationDirectionFromAngle(
        forward, up, command * maximumSteerAngleRad, driftSide, direction);
}

// Return the requested longitudinal acceleration multiplier for the current
// vehicle speed magnitude. The thresholds are specified in km/h by the
// user-facing tuning, but the planner operates in m/s. Using the full linear
// speed (rather than only its forward projection) keeps the fade predictable
// during large drift angles and side slip.
inline float SpeedAttenuation(float speedMps) {
    if (!std::isfinite(speedMps) || speedMps < 0.0f) {
        return 0.0f;
    }
    if (speedMps <= kSpeedFadeFullMps) {
        return 1.0f;
    }
    if (speedMps <= kSpeedFadeHalfMps) {
        const float fraction =
            (speedMps - kSpeedFadeFullMps) /
            (kSpeedFadeHalfMps - kSpeedFadeFullMps);
        return 1.0f + (0.5f - 1.0f) * fraction;
    }
    if (speedMps <= kSpeedFadeLowMps) {
        const float fraction =
            (speedMps - kSpeedFadeHalfMps) /
            (kSpeedFadeLowMps - kSpeedFadeHalfMps);
        return 0.5f + (0.15f - 0.5f) * fraction;
    }
    if (speedMps <= kSpeedFadeZeroMps) {
        const float fraction =
            (speedMps - kSpeedFadeLowMps) /
            (kSpeedFadeZeroMps - kSpeedFadeLowMps);
        return 0.15f * (1.0f - fraction);
    }
    return 0.0f;
}

inline bool HandbrakeDecelerationEligible(float handbrake,
                                          float threshold,
                                          float heldSeconds) {
    return std::isfinite(handbrake) && std::isfinite(threshold) &&
           std::isfinite(heldSeconds) && threshold >= 0.0f &&
           threshold <= 1.0f && handbrake >= threshold &&
           handbrake <= 1.0f &&
           heldSeconds >= kHandbrakeDecelerationHoldSeconds;
}

// The requested deceleration remains deliberately weak at and below
// 100 km/h, then rises linearly to full strength at 180 km/h.
inline float DecelerationStrengthScale(float speedMps) {
    if (!std::isfinite(speedMps) || speedMps < 0.0f) {
        return 0.0f;
    }
    if (speedMps <= kDecelerationMinimumSpeedMps) {
        return kMinimumDecelerationStrengthScale;
    }
    if (speedMps >= kDecelerationMaximumSpeedMps) {
        return 1.0f;
    }
    const float fraction =
        (speedMps - kDecelerationMinimumSpeedMps) /
        (kDecelerationMaximumSpeedMps - kDecelerationMinimumSpeedMps);
    return kMinimumDecelerationStrengthScale +
           (1.0f - kMinimumDecelerationStrengthScale) * fraction;
}

inline AppliedDeltaVerification VerifyAppliedDelta(
    nfsmw_drift::Vec3 before,
    nfsmw_drift::Vec3 after,
    nfsmw_drift::Vec3 plannedDirection,
    float plannedDeltaVMps) {
    AppliedDeltaVerification verification{};
    verification.evaluated = true;
    verification.beforeVelocity = before;
    verification.afterVelocity = after;
    verification.plannedDeltaVMps = plannedDeltaVMps;

    if (!FiniteVector(before) || !FiniteVector(after) ||
        !FiniteVector(plannedDirection) ||
        !std::isfinite(plannedDeltaVMps) || plannedDeltaVMps <= 0.0f ||
        plannedDeltaVMps > kMaximumDeltaVPerTickMps) {
        return verification;
    }
    const float directionLength = VectorLength(plannedDirection);
    if (!std::isfinite(directionLength) || directionLength < 0.99f ||
        directionLength > 1.01f) {
        return verification;
    }

    const nfsmw_drift::Vec3 direction =
        plannedDirection / directionLength;
    verification.actualDelta = after - before;
    verification.actualDeltaVMps = VectorLength(verification.actualDelta);
    verification.projectedDeltaVMps =
        nfsmw_drift::Dot(verification.actualDelta, direction);
    verification.orthogonalDelta =
        verification.actualDelta -
        direction * verification.projectedDeltaVMps;
    verification.orthogonalResidualMps =
        VectorLength(verification.orthogonalDelta);
    if (!FiniteVector(verification.actualDelta) ||
        !FiniteVector(verification.orthogonalDelta) ||
        !std::isfinite(verification.actualDeltaVMps) ||
        !std::isfinite(verification.projectedDeltaVMps) ||
        !std::isfinite(verification.orthogonalResidualMps)) {
        return verification;
    }

    // The game stores velocity as three floats.  Account for cancellation
    // when a small increment is read back on top of a large base velocity,
    // while keeping a relative envelope around the requested delta.  The
    // minimum-observed check ensures that a no-op never passes merely because
    // its requested delta is close to float resolution.
    const float velocityScale = std::max(
        {1.0f, MaximumAbsoluteComponent(before),
         MaximumAbsoluteComponent(after)});
    const float floatRoundoffMps = std::max(
        kAppliedDeltaAbsoluteToleranceMps,
        kAppliedDeltaFloatRoundoffUlps *
            std::numeric_limits<float>::epsilon() * velocityScale);
    verification.projectedToleranceMps = std::max(
        floatRoundoffMps,
        plannedDeltaVMps * kAppliedDeltaProjectedRelativeTolerance);
    verification.magnitudeToleranceMps = std::max(
        floatRoundoffMps,
        plannedDeltaVMps * kAppliedDeltaMagnitudeRelativeTolerance);
    verification.orthogonalToleranceMps = std::max(
        floatRoundoffMps * 2.0f,
        plannedDeltaVMps * kAppliedDeltaOrthogonalRelativeTolerance);

    const float minimumObservedDelta =
        plannedDeltaVMps * kAppliedDeltaMinimumObservedFraction;
    verification.projectedMatch =
        verification.projectedDeltaVMps >= minimumObservedDelta &&
        std::fabs(verification.projectedDeltaVMps - plannedDeltaVMps) <=
            verification.projectedToleranceMps;
    verification.magnitudeMatch =
        verification.actualDeltaVMps >= minimumObservedDelta &&
        std::fabs(verification.actualDeltaVMps - plannedDeltaVMps) <=
            verification.magnitudeToleranceMps;
    verification.orthogonalMatch =
        verification.orthogonalResidualMps <=
        verification.orthogonalToleranceMps;
    verification.inputsValid = true;
    verification.accepted = verification.projectedMatch &&
                            verification.magnitudeMatch &&
                            verification.orthogonalMatch;
    return verification;
}

// Advance independently of frame rate.  Entry is intentionally slower than
// exit so releasing the accelerator cannot leave a stale boost behind.
inline float AdvanceRamp(float current,
                         bool eligible,
                         float dt,
                         float riseSeconds = 1.0f,
                         float fallSeconds = 0.25f) {
    if (!std::isfinite(current)) {
        current = 0.0f;
    }
    current = std::clamp(current, 0.0f, 1.0f);
    if (!std::isfinite(dt) || dt <= 0.0f) {
        return current;
    }
    const float duration = eligible ? riseSeconds : fallSeconds;
    if (!std::isfinite(duration) || duration <= 0.0f) {
        return eligible ? 1.0f : 0.0f;
    }
    const float step = std::clamp(dt / duration, 0.0f, 1.0f);
    return eligible ? std::min(1.0f, current + step)
                    : std::max(0.0f, current - step);
}

inline Plan MakePlan(const Inputs& input) {
    Plan plan{};
    plan.mode = PlanMode::Acceleration;
    plan.targetAccelerationMps2 = input.targetAccelerationMps2;
    plan.speedScale = std::clamp(input.ramp, 0.0f, 1.0f);

    if (!input.enabled) {
        plan.reason = RejectReason::Disabled;
        return plan;
    }
    if (!input.sessionActive) {
        plan.reason = RejectReason::SessionInactive;
        return plan;
    }
    if (!input.identityStable) {
        plan.reason = RejectReason::IdentityUnstable;
        return plan;
    }
    if (!input.physicsStable) {
        plan.reason = RejectReason::PhysicsUnstable;
        return plan;
    }
    if (!input.grounded) {
        plan.reason = RejectReason::NoGroundContact;
        return plan;
    }
    if (!std::isfinite(input.throttle) ||
        input.throttle < kMinimumThrottle ||
        input.throttle > kMaximumThrottle) {
        plan.reason = RejectReason::NoThrottle;
        return plan;
    }
    if (!std::isfinite(input.brake) || input.brake < 0.0f ||
        input.brake > 1.0f) {
        plan.reason = RejectReason::Braking;
        return plan;
    }
    if (input.brake > kMaximumBrakeForAcceleration) {
        plan.reason = RejectReason::Braking;
        return plan;
    }
    if (!std::isfinite(input.dt) || input.dt <= 0.0f ||
        input.dt > kMaximumPlanDt) {
        plan.reason = RejectReason::InvalidDt;
        return plan;
    }
    if (!FiniteVector(input.forward)) {
        plan.reason = RejectReason::InvalidForward;
        return plan;
    }
    const float forwardLength = VectorLength(input.forward);
    if (!std::isfinite(forwardLength) ||
        forwardLength < kMinimumForwardLength ||
        forwardLength > kMaximumForwardLength) {
        plan.reason = RejectReason::InvalidForward;
        return plan;
    }
    if (!FiniteVector(input.linearVelocity)) {
        plan.reason = RejectReason::InvalidVelocity;
        return plan;
    }
    const float speedMagnitude = VectorLength(input.linearVelocity);
    if (!std::isfinite(speedMagnitude) ||
        speedMagnitude > kMaximumPlanVectorMagnitude) {
        plan.reason = RejectReason::InvalidVelocity;
        return plan;
    }
    plan.direction = input.forward / forwardLength;
    plan.speedMps = speedMagnitude;
    plan.longitudinalSpeedMps =
        nfsmw_drift::Dot(input.linearVelocity, plan.direction);
    if (!std::isfinite(plan.longitudinalSpeedMps) ||
        plan.longitudinalSpeedMps <= 0.0f) {
        plan.reason = RejectReason::ReverseMotion;
        return plan;
    }

    // Keep the safety projection above tied to the body heading.  Only after
    // that guard passes do we rotate the actual impulse toward the midpoint
    // between the heading and the applied countersteering wheel direction.
    // This prevents a large drift slip angle from making a valid forward
    // vehicle look like reverse motion when the midpoint is used.
    if (input.useCountersteerDirection) {
        nfsmw_drift::Vec3 steeredDirection{};
        if (!ComputeCountersteerAccelerationDirection(
                plan.direction, input.bodyUp,
                input.appliedSteeringCommand, input.driftSide,
                input.maximumSteerAngleRad, &steeredDirection)) {
            plan.reason = RejectReason::InvalidSteeringDirection;
            return plan;
        }
        plan.direction = steeredDirection;
    }
    if (plan.speedMps >= kMaximumPlanSpeedMps) {
        plan.reason = RejectReason::SpeedLimit;
        return plan;
    }
    if (!std::isfinite(input.targetAccelerationMps2) ||
        input.targetAccelerationMps2 < 0.0f ||
        input.targetAccelerationMps2 > kMaximumTargetAccelerationMps2) {
        plan.reason = RejectReason::InvalidTarget;
        return plan;
    }

    // Apply both the requested ramp and the two speed protections: the
    // user-facing four-point speed attenuation and the internal hard-ceiling
    // fade. The release strength scale is applied after those protections so
    // it reduces every speed band by the same amount without changing the curve.
    // The throttle is intentionally not squared: the caller can choose a
    // target acceleration independently of the input device's analog curve.
    const float headroom = kMaximumPlanSpeedMps - plan.speedMps;
    const float hardCeilingScale =
        std::clamp(headroom / kSpeedSoftLimitBandMps, 0.0f, 1.0f);
    plan.speedScale *= hardCeilingScale *
                       SpeedAttenuation(plan.speedMps);
    plan.targetAccelerationMps2 *=
        plan.speedScale * std::clamp(input.throttle, 0.0f, 1.0f) *
        kConfiguredStrengthScale;
    plan.deltaVMps = std::clamp(
        plan.targetAccelerationMps2 * input.dt, 0.0f,
        kMaximumDeltaVPerTickMps);
    plan.deltaVMps = std::min(plan.deltaVMps, headroom);
    if (!std::isfinite(plan.deltaVMps) || plan.deltaVMps <= 0.0f) {
        plan.reason = RejectReason::ZeroDemand;
        return plan;
    }
    plan.accepted = true;
    plan.reason = RejectReason::None;
    return plan;
}

inline Plan MakeDecelerationPlan(const DecelerationInputs& input) {
    Plan plan{};
    plan.mode = PlanMode::Deceleration;
    plan.targetAccelerationMps2 = input.maximumDecelerationMps2;

    if (!input.enabled) {
        plan.reason = RejectReason::Disabled;
        return plan;
    }
    if (!input.sessionActive) {
        plan.reason = RejectReason::SessionInactive;
        return plan;
    }
    if (!input.identityStable) {
        plan.reason = RejectReason::IdentityUnstable;
        return plan;
    }
    if (!input.physicsStable) {
        plan.reason = RejectReason::PhysicsUnstable;
        return plan;
    }
    if (!input.grounded) {
        plan.reason = RejectReason::NoGroundContact;
        return plan;
    }
    if (!HandbrakeDecelerationEligible(
            input.handbrake, input.handbrakeThreshold,
            input.handbrakeHeldSeconds)) {
        plan.reason = RejectReason::HandbrakeNotHeld;
        return plan;
    }
    if (!std::isfinite(input.dt) || input.dt <= 0.0f ||
        input.dt > kMaximumPlanDt) {
        plan.reason = RejectReason::InvalidDt;
        return plan;
    }
    if (!FiniteVector(input.linearVelocity)) {
        plan.reason = RejectReason::InvalidVelocity;
        return plan;
    }
    const float speed = VectorLength(input.linearVelocity);
    if (!std::isfinite(speed) ||
        speed < kMinimumVelocityForDecelerationMps ||
        speed > kMaximumPlanVectorMagnitude) {
        plan.reason = RejectReason::InvalidVelocity;
        return plan;
    }
    if (!std::isfinite(input.maximumDecelerationMps2) ||
        input.maximumDecelerationMps2 <= 0.0f ||
        input.maximumDecelerationMps2 > kMaximumTargetAccelerationMps2) {
        plan.reason = RejectReason::InvalidTarget;
        return plan;
    }

    plan.speedMps = speed;
    plan.direction = input.linearVelocity * (-1.0f / speed);
    plan.longitudinalSpeedMps = speed;
    plan.speedScale = DecelerationStrengthScale(speed);
    plan.targetAccelerationMps2 =
        input.maximumDecelerationMps2 * plan.speedScale;
    plan.deltaVMps = std::clamp(
        plan.targetAccelerationMps2 * input.dt, 0.0f,
        kMaximumDeltaVPerTickMps);
    plan.deltaVMps = std::min(plan.deltaVMps, speed);
    if (!std::isfinite(plan.deltaVMps) || plan.deltaVMps <= 0.0f) {
        plan.reason = RejectReason::ZeroDemand;
        return plan;
    }
    plan.accepted = true;
    plan.reason = RejectReason::None;
    return plan;
}

inline const char* PlanModeName(PlanMode mode) {
    switch (mode) {
        case PlanMode::None:
            return "none";
        case PlanMode::Acceleration:
            return "acceleration";
        case PlanMode::Deceleration:
            return "deceleration";
    }
    return "unknown";
}

inline const char* RejectReasonName(RejectReason reason) {
    switch (reason) {
        case RejectReason::None:
            return "none";
        case RejectReason::Disabled:
            return "disabled";
        case RejectReason::SessionInactive:
            return "session-inactive";
        case RejectReason::IdentityUnstable:
            return "identity-unstable";
        case RejectReason::PhysicsUnstable:
            return "physics-unstable";
        case RejectReason::NoGroundContact:
            return "no-ground-contact";
        case RejectReason::NoThrottle:
            return "no-throttle";
        case RejectReason::Braking:
            return "braking";
        case RejectReason::InvalidDt:
            return "invalid-dt";
        case RejectReason::InvalidForward:
            return "invalid-forward";
        case RejectReason::InvalidSteeringDirection:
            return "invalid-steering-direction";
        case RejectReason::InvalidVelocity:
            return "invalid-velocity";
        case RejectReason::ReverseMotion:
            return "reverse-motion";
        case RejectReason::SpeedLimit:
            return "speed-limit";
        case RejectReason::InvalidTarget:
            return "invalid-target";
        case RejectReason::HandbrakeNotHeld:
            return "handbrake-not-held";
        case RejectReason::ZeroDemand:
            return "zero-demand";
    }
    return "unknown";
}

}  // namespace nfsmw_drift_asi::rigidbody_accel
