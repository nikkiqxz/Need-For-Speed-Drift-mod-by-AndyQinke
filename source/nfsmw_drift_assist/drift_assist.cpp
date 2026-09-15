#include "drift_assist.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nfsmw_drift {
namespace {

constexpr float kMinimumVectorLength = 1.0e-6f;
constexpr float kMaximumControllerDt = 0.10f;
constexpr float kMaximumSteeringOwnershipDt = 0.25f;
constexpr float kMaximumMeasuredSideslipRate = 20.0f;
constexpr float kMaximumBodyYawRate = 20.0f;
constexpr float kMaximumSameDirectionFullSteerSeconds = 1.0f;
constexpr float kMaximumCountersteerTransitionSeconds = 2.0f;
constexpr float kMaximumBodyYawAccelerationRadS2 = 100.0f;
constexpr float kMaximumEntryYawBoostSeconds = 0.25f;
constexpr float kMaximumEntryYawBoostRateRadS = 0.90f;
constexpr float kMaximumEntryYawBoostAccelerationRadS2 = 6.0f;
constexpr float kMaximumEntryYawBoostOffsetCutoffRad = DegToRad(15.0f);
constexpr float kMaximumEntryYawBoostAngularDeltaRadS = 0.20f;
constexpr float kMaximumDriftAngleEntrySeconds = 0.25f;
constexpr float kMaximumDriftAngleEntrySpeedRadS = 1.20f;
constexpr float kMaximumDriftAngleEntryAccelerationRadS2 = 20.0f;
constexpr float kMaximumDriftAngleEntryOffsetCutoffRad = DegToRad(15.0f);
constexpr float kMaximumDriftAngleEntryAngularDeltaRadS = 0.50f;
constexpr float kMinimumDriftAcquireCaptureMarginRad = DegToRad(0.5f);
constexpr float kMaximumFastYawAccelerationRadS2 = 20.0f;
constexpr float kMaximumFastYawAngularDeltaRadS = 0.50f;
constexpr float kMaximumManualSameDirectionYawRateRadS = 0.5625f;
constexpr float kMaximumManualOppositeRecoveryYawRateRadS = 0.45f;
constexpr float kMaximumManualSameDirectionYawAccelerationRadS2 = 3.0f;
constexpr float kMaximumManualOppositeYawAccelerationRadS2 = 4.5f;
constexpr float kMaximumManualYawAngularDeltaRadS = 0.15f;
constexpr float kMaximumManualYawHardBodyOffsetRad = DegToRad(35.0f);
constexpr float kMaximumManualSameDirectionRampSeconds = 2.0f;
constexpr float kMaximumManualSteeringTransitionSeconds = 10.0f;
constexpr float kMaximumGroundContactLossGraceSeconds = 10.0f;
constexpr float kMaximumPendulumTransitionZeroBandRad = DegToRad(10.0f);
constexpr float kMaximumPendulumTransitionConfirmOffsetRad = DegToRad(30.0f);
constexpr float kMaximumPendulumTransitionWindowSeconds = 2.0f;
constexpr float kMaximumDriftAngleRateFilterSeconds = 1.0f;
constexpr float kMaximumExitSettleSeconds = 2.0f;
constexpr float kMinimumCenterToleranceRad = 1.0e-4f;
// Do not finish auto-centering while the rigid body is still spinning.  The
// correction is emitted as a yaw-rate delta below, so waiting for this small
// residual rate prevents a high-rate pass through zero from escaping the
// controller with momentum still applied.  This guard is only used when the
// current frame has a validated attitude-write path; without one, centering
// must remain fail-closed rather than waiting forever on an unchangeable rate.
constexpr float kCenterYawRateToleranceRadS = DegToRad(2.0f);

bool IsFinite(float value) {
    return std::isfinite(value) != 0;
}

bool IsFinite(Vec3 value) {
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

float FiniteOr(float value, float fallback) {
    return IsFinite(value) ? value : fallback;
}

float NonNegativeOr(float value, float fallback) {
    return std::max(0.0f, FiniteOr(value, fallback));
}

float UnitOr(float value, float fallback) {
    return Clamp(FiniteOr(value, fallback), 0.0f, 1.0f);
}

// Automatic countersteer is only allowed while the steering sample is fully
// neutral.  Any valid player event, including an event in the same direction
// as the automatic countersteer (for example, an attempt to add more
// countersteer), must release ownership immediately.  The direction mask is
// retained for diagnostics and for hosts that observe input between physics
// updates; it is intentionally not used to grant automatic ownership.
bool AutomaticCountersteerInputAllowed(bool currentManualInput,
                                       bool manualInputObserved,
                                       std::uint8_t directionMask,
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
        directionMask &
        (kNegativeSteeringDirectionObserved |
         kPositiveSteeringDirectionObserved);
    (void)driftSide;
    return !currentManualInput && !manualInputObserved && knownMask == 0;
}

float HandbrakeYawDeltaLimit(float maximumAngularDelta,
                             float acceleration,
                             float dt) {
    return std::min(maximumAngularDelta,
                    acceleration * dt);
}

float BrakingLimitedSpeed(float maximumSpeed,
                          float distance,
                          float acceleration) {
    if (maximumSpeed <= 0.0f || distance <= 0.0f || acceleration <= 0.0f) {
        return 0.0f;
    }
    const float stoppingLimited =
        std::sqrt(2.0f * acceleration * distance);
    return std::min(maximumSpeed,
                    FiniteOr(stoppingLimited, maximumSpeed));
}

float AngleTrackingRate(float currentOffset,
                        float targetOffset,
                        float maximumSpeed,
                        float acceleration,
                        float slowdownOffset,
                        float dt,
                        bool& braking) {
    const float error = targetOffset - currentOffset;
    const float distance = std::fabs(error);
    if (distance <= kMinimumCenterToleranceRad ||
        maximumSpeed <= 0.0f || dt <= kMinimumVectorLength) {
        braking = maximumSpeed > 0.0f;
        return 0.0f;
    }

    const float proportionalSpeed = slowdownOffset > kMinimumVectorLength
                                        ? maximumSpeed * Clamp(
                                              distance / slowdownOffset,
                                              0.0f,
                                              1.0f)
                                        : maximumSpeed;
    const float requestedSpeed = std::min(
        std::min(proportionalSpeed,
                 BrakingLimitedSpeed(maximumSpeed, distance, acceleration)),
        distance / dt);
    braking = requestedSpeed + kMinimumVectorLength < maximumSpeed;
    return (error > 0.0f ? 1.0f : -1.0f) * requestedSpeed;
}

float Sign(float value) {
    return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
}

float MoveTowards(float current, float target, float maximumDelta) {
    if (maximumDelta <= 0.0f) {
        return current;
    }
    const float difference = target - current;
    if (std::fabs(difference) <= maximumDelta) {
        return target;
    }
    return current + Sign(difference) * maximumDelta;
}

Vec3 FiniteVectorOr(Vec3 value, Vec3 fallback) {
    return IsFinite(value) ? value : fallback;
}

Basis MakeOrthonormalBasis(const Basis& supplied) {
    Vec3 forward = NormalizeOr(FiniteVectorOr(supplied.forward, {0.0f, 0.0f, 1.0f}),
                               {0.0f, 0.0f, 1.0f});

    Vec3 upCandidate = FiniteVectorOr(supplied.up, {0.0f, 1.0f, 0.0f});
    upCandidate -= forward * Dot(upCandidate, forward);
    if (Length(upCandidate) <= kMinimumVectorLength) {
        const Vec3 fallbackUp = std::fabs(forward.y) < 0.95f
                                    ? Vec3{0.0f, 1.0f, 0.0f}
                                    : Vec3{1.0f, 0.0f, 0.0f};
        upCandidate = fallbackUp - forward * Dot(fallbackUp, forward);
    }
    Vec3 up = NormalizeOr(upCandidate, {0.0f, 1.0f, 0.0f});

    Vec3 right = NormalizeOr(Cross(up, forward),
                             FiniteVectorOr(supplied.right, {1.0f, 0.0f, 0.0f}));
    up = NormalizeOr(Cross(forward, right), up);
    return {right, up, forward};
}

float AverageRearSlip(const VehicleState& state) {
    const float leftLoad = NonNegativeOr(state.wheelLoad[2], 0.0f);
    const float rightLoad = NonNegativeOr(state.wheelLoad[3], 0.0f);
    // Normalize before summing: two large finite loads can overflow a direct
    // sum and turn the weighted average into inf / inf (NaN).
    const float loadScale = std::max(leftLoad, rightLoad);
    if (!IsFinite(loadScale) || loadScale <= kMinimumVectorLength) {
        return 0.0f;
    }

    const float normalizedLeftLoad = leftLoad / loadScale;
    const float normalizedRightLoad = rightLoad / loadScale;
    const float normalizedTotal = normalizedLeftLoad + normalizedRightLoad;
    if (!IsFinite(normalizedTotal) || normalizedTotal <= kMinimumVectorLength) {
        return 0.0f;
    }

    const float leftSlip = std::fabs(FiniteOr(state.wheelSlip[2], 0.0f));
    const float rightSlip = std::fabs(FiniteOr(state.wheelSlip[3], 0.0f));
    const float leftWeight = normalizedLeftLoad / normalizedTotal;
    const float rightWeight = normalizedRightLoad / normalizedTotal;
    const float weightedSlip = leftSlip * leftWeight + rightSlip * rightWeight;
    return IsFinite(weightedSlip)
               ? std::max(0.0f, weightedSlip)
               : std::numeric_limits<float>::max();
}

bool SteeringEnabled(ActuationMode mode) {
    return mode == ActuationMode::SteeringOnly ||
           mode == ActuationMode::SteeringAndAttitude;
}

bool AttitudeEnabled(ActuationMode mode) {
    return mode == ActuationMode::AttitudeOnly ||
           mode == ActuationMode::SteeringAndAttitude;
}

float RecoveryAmount(float absoluteSideslip, const AssistConfig& config) {
    if (absoluteSideslip <= config.recoverySideslipRad) {
        return 0.0f;
    }
    const float range = config.maximumSideslipRad - config.recoverySideslipRad;
    if (range <= 1.0e-5f) {
        return 1.0f;
    }
    return Clamp((absoluteSideslip - config.recoverySideslipRad) / range, 0.0f, 1.0f);
}

Vec3 ProjectOntoPlane(Vec3 value, Vec3 normal) {
    return value - normal * Dot(value, normal);
}

float SignedYawAngle(Vec3 from, Vec3 to, Vec3 axis) {
    axis = NormalizeOr(axis, {0.0f, 1.0f, 0.0f});
    from = NormalizeOr(ProjectOntoPlane(from, axis), {0.0f, 0.0f, 1.0f});
    to = NormalizeOr(ProjectOntoPlane(to, axis), from);
    const float sine = Dot(Cross(from, to), axis);
    const float cosine = Clamp(Dot(from, to), -1.0f, 1.0f);
    return WrapPi(std::atan2(sine, cosine));
}

float DirectionSign(float steering, float deadzone) {
    if (steering < -deadzone) {
        return -1.0f;
    }
    if (steering > deadzone) {
        return 1.0f;
    }
    return 0.0f;
}

float CenterTolerance(const AssistConfig& config) {
    // Keep a small finish tolerance to avoid chattering around zero, but the
    // final centering command still targets the actual zero offset.
    return std::max(kMinimumCenterToleranceRad,
                    std::min(config.exitBodyOffsetRad, DegToRad(0.5f)));
}

float BlendMultiplier(float multiplier, float authority) {
    return 1.0f + (multiplier - 1.0f) * Clamp(authority, 0.0f, 1.0f);
}

float ContactAuthority(std::uint8_t groundedWheels, const AssistConfig& config) {
    const float minimumContacts = static_cast<float>(
        std::max<std::uint8_t>(config.minGroundedWheels, 1));
    const float contactFraction = Clamp(
        static_cast<float>(std::min<std::uint8_t>(groundedWheels, 4)) /
            minimumContacts,
        0.0f,
        1.0f);
    return config.airborneAuthority +
           (1.0f - config.airborneAuthority) * contactFraction;
}

} // namespace

Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator-(Vec3 v) {
    return {-v.x, -v.y, -v.z};
}

Vec3 operator*(Vec3 v, float scalar) {
    return {v.x * scalar, v.y * scalar, v.z * scalar};
}

Vec3 operator*(float scalar, Vec3 v) {
    return v * scalar;
}

Vec3 operator/(Vec3 v, float scalar) {
    return {v.x / scalar, v.y / scalar, v.z / scalar};
}

Vec3& operator+=(Vec3& a, Vec3 b) {
    a.x += b.x;
    a.y += b.y;
    a.z += b.z;
    return a;
}

Vec3& operator-=(Vec3& a, Vec3 b) {
    a.x -= b.x;
    a.y -= b.y;
    a.z -= b.z;
    return a;
}

Vec3& operator*=(Vec3& a, float scalar) {
    a.x *= scalar;
    a.y *= scalar;
    a.z *= scalar;
    return a;
}

float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float Length(Vec3 v) {
    if (!IsFinite(v)) {
        return std::numeric_limits<float>::infinity();
    }
    return std::hypot(v.x, v.y, v.z);
}

Vec3 NormalizeOr(Vec3 v, Vec3 fallback) {
    const float length = Length(v);
    if (IsFinite(length) && length > kMinimumVectorLength) {
        return v / length;
    }

    const float fallbackLength = Length(fallback);
    if (IsFinite(fallbackLength) && fallbackLength > kMinimumVectorLength) {
        return fallback / fallbackLength;
    }
    return {0.0f, 0.0f, 0.0f};
}

float Clamp(float value, float low, float high) {
    if (!IsFinite(low)) {
        low = -std::numeric_limits<float>::max();
    }
    if (!IsFinite(high)) {
        high = std::numeric_limits<float>::max();
    }
    if (low > high) {
        std::swap(low, high);
    }
    if (std::isnan(value)) {
        return Clamp(0.0f, low, high);
    }
    return std::max(low, std::min(value, high));
}

float WrapPi(float angle) {
    if (!IsFinite(angle)) {
        return 0.0f;
    }
    return std::remainder(angle, 2.0f * kPi);
}

ManualYawCommand ComputeManualYawCommand(const AssistConfig& config,
                                         float bodyYawOffsetRad,
                                         float bodyYawOffsetRateRadS,
                                         float rawSteering,
                                         int driftSide,
                                         float dt,
                                         float manualYawStrength) {
    ManualYawCommand command{};
    if (!config.enableManualYawAssist ||
        !AttitudeEnabled(config.actuation) ||
        (driftSide != -1 && driftSide != 1) ||
        !IsFinite(bodyYawOffsetRad) ||
        !IsFinite(bodyYawOffsetRateRadS) ||
        !IsFinite(rawSteering) ||
        !IsFinite(manualYawStrength) ||
        !IsFinite(dt) || dt <= 0.0f || dt > kMaximumControllerDt) {
        return command;
    }

    const float steering = Clamp(rawSteering, -1.0f, 1.0f);
    const float deadzone = Clamp(
        FiniteOr(config.directionDeadzone, 1.0f), 0.0f, 1.0f);
    if (std::fabs(steering) <= deadzone ||
        deadzone >= 1.0f - kMinimumVectorLength) {
        return command;
    }

    const float inputMagnitude = Clamp(
        (std::fabs(steering) - deadzone) / (1.0f - deadzone),
        0.0f,
        1.0f);
    const int inputDirection = steering < 0.0f ? -1 : 1;
    const float side = static_cast<float>(driftSide);
    if (inputMagnitude <= 0.0f) {
        return command;
    }

    if (inputDirection == driftSide) {
        command.mode = YawResponseMode::ManualSameDirectionAssist;
        const float strength = Clamp(manualYawStrength, 0.0f, 1.0f);
        const float targetRate = Clamp(
            NonNegativeOr(config.manualSameDirectionTargetYawRateRadS, 0.0f),
            0.0f,
            kMaximumManualSameDirectionYawRateRadS) * inputMagnitude;
        const float acceleration = Clamp(
            NonNegativeOr(config.manualSameDirectionYawAccelerationRadS2, 0.0f),
            0.0f,
            kMaximumManualSameDirectionYawAccelerationRadS2);
        const float maximumDelta = Clamp(
            NonNegativeOr(config.manualSameDirectionMaximumYawAngularDelta, 0.0f),
            0.0f,
            kMaximumManualYawAngularDeltaRadS);
        command.targetYawRateRadS = side * targetRate;
        command.accelerationLimitRadS2 =
            acceleration * inputMagnitude * strength;
        command.velocityDeltaLimitRadS =
            maximumDelta * inputMagnitude * strength;

        const float hardOffset = Clamp(
            NonNegativeOr(config.manualYawHardBodyOffsetRad, 0.0f),
            0.0f,
            kMaximumManualYawHardBodyOffsetRad);
        const bool pushingBeyondHardOffset =
            hardOffset <= kMinimumVectorLength ||
            (std::fabs(bodyYawOffsetRad) + kMinimumVectorLength >= hardOffset &&
             bodyYawOffsetRad * side > 0.0f);
        if (pushingBeyondHardOffset) {
            command.bodyOffsetLimited = true;
            return command;
        }

        const float rateAlongDriftSide = side * bodyYawOffsetRateRadS;
        const float remaining = targetRate - rateAlongDriftSide;
        if (remaining <= 0.0f) {
            return command;
        }
        const float step = HandbrakeYawDeltaLimit(
            command.velocityDeltaLimitRadS,
            command.accelerationLimitRadS2,
            dt);
        command.yawVelocityDeltaRadS = side * std::min(remaining, step);
        return command;
    }

    if (inputDirection != -driftSide) {
        return command;
    }

    command.mode = YawResponseMode::ManualOppositeRecovery;
    const float dampingAcceleration = Clamp(
        NonNegativeOr(config.manualOppositeYawDampingAccelerationRadS2, 0.0f),
        0.0f,
        kMaximumManualOppositeYawAccelerationRadS2);
    const float maximumDelta = Clamp(
        NonNegativeOr(config.manualOppositeMaximumYawAngularDelta, 0.0f),
        0.0f,
        kMaximumManualYawAngularDeltaRadS);
    command.accelerationLimitRadS2 = dampingAcceleration * inputMagnitude;
    command.velocityDeltaLimitRadS = maximumDelta * inputMagnitude;
    const float step = HandbrakeYawDeltaLimit(
        command.velocityDeltaLimitRadS,
        command.accelerationLimitRadS2,
        dt);

    const float centerGuard = Clamp(
        NonNegativeOr(config.pendulumTransitionZeroBandRad, 0.0f),
        0.0f,
        kMaximumPendulumTransitionZeroBandRad);
    const float distanceToCenter =
        std::max(0.0f, std::fabs(bodyYawOffsetRad) - centerGuard);
    if (distanceToCenter <= kMinimumCenterToleranceRad ||
        bodyYawOffsetRad * side < -centerGuard) {
        return command;
    }

    const float configuredMaximum = Clamp(
        NonNegativeOr(config.manualOppositeMaximumRecoveryYawRateRadS, 0.0f),
        0.0f,
        kMaximumManualOppositeRecoveryYawRateRadS) * inputMagnitude;
    const float stoppingLimited = dampingAcceleration > 0.0f
                                      ? std::sqrt(2.0f * dampingAcceleration *
                                                  distanceToCenter)
                                      : 0.0f;
    const float oneFrameLimited = distanceToCenter / dt;
    const float safeRate = std::min(
        configuredMaximum,
        std::min(FiniteOr(stoppingLimited, 0.0f), oneFrameLimited));
    const float targetRate = -Sign(bodyYawOffsetRad) * safeRate;
    command.targetYawRateRadS = targetRate;
    if (step <= 0.0f) {
        return command;
    }

    const float nextRate = MoveTowards(
        bodyYawOffsetRateRadS, targetRate, step);
    command.yawVelocityDeltaRadS = nextRate - bodyYawOffsetRateRadS;
    command.dampingActive =
        std::fabs(command.yawVelocityDeltaRadS) > kMinimumVectorLength;
    return command;
}

DriftAssistController::DriftAssistController(AssistConfig config) {
    setConfig(config);
    reset();
}

void DriftAssistController::setConfig(const AssistConfig& config) {
    const AssistConfig defaults{};
    config_ = config;

    switch (config_.activation) {
    case ActivationMode::HandbrakeHold:
    case ActivationMode::Manual:
    case ActivationMode::Automatic:
    case ActivationMode::ManualOrAutomatic:
        break;
    default:
        config_.activation = defaults.activation;
        break;
    }
    switch (config_.actuation) {
    case ActuationMode::SteeringOnly:
    case ActuationMode::SteeringAndAttitude:
    case ActuationMode::AttitudeOnly:
        break;
    default:
        config_.actuation = defaults.actuation;
        break;
    }

    config_.minSpeedMps = NonNegativeOr(config_.minSpeedMps, defaults.minSpeedMps);
    config_.minLongitudinalSpeedMps =
        NonNegativeOr(config_.minLongitudinalSpeedMps, defaults.minLongitudinalSpeedMps);
    config_.minGroundedWheels = std::min<std::uint8_t>(config_.minGroundedWheels, 4);
    config_.groundContactLossGraceSeconds = Clamp(
        NonNegativeOr(config_.groundContactLossGraceSeconds,
                      defaults.groundContactLossGraceSeconds),
        0.0f,
        kMaximumGroundContactLossGraceSeconds);
    config_.enterSideslipRad = NonNegativeOr(config_.enterSideslipRad, defaults.enterSideslipRad);
    config_.exitSideslipRad = std::min(
        NonNegativeOr(config_.exitSideslipRad, defaults.exitSideslipRad),
        config_.enterSideslipRad);
    config_.enterRearSlip = NonNegativeOr(config_.enterRearSlip, defaults.enterRearSlip);
    config_.exitRearSlip = std::min(
        NonNegativeOr(config_.exitRearSlip, defaults.exitRearSlip),
        config_.enterRearSlip);
    config_.enterHandbrake = UnitOr(config_.enterHandbrake, defaults.enterHandbrake);

    config_.handbrakeActivationHoldSeconds = NonNegativeOr(
        config_.handbrakeActivationHoldSeconds, defaults.handbrakeActivationHoldSeconds);
    config_.handbrakeActivationThreshold = UnitOr(
        config_.handbrakeActivationThreshold, defaults.handbrakeActivationThreshold);
    config_.directionDeadzone = UnitOr(config_.directionDeadzone, defaults.directionDeadzone);
    config_.countersteerActivationBodyOffsetRad = Clamp(
        NonNegativeOr(config_.countersteerActivationBodyOffsetRad,
                      defaults.countersteerActivationBodyOffsetRad),
        0.0f,
        0.5f * kPi);
    config_.smartCountersteerAngleRad = Clamp(
        NonNegativeOr(config_.smartCountersteerAngleRad,
                      defaults.smartCountersteerAngleRad),
        0.0f,
        DegToRad(85.0f));
    config_.smartCountersteerReengageDelaySeconds = Clamp(
        NonNegativeOr(config_.smartCountersteerReengageDelaySeconds,
                      defaults.smartCountersteerReengageDelaySeconds),
        0.0f,
        kMaximumCountersteerTransitionSeconds);
    // The legacy handoff key is retained for INI compatibility, but the live
    // 0.9.5 ownership path always gives a valid manual event to the player on
    // the first sample.  A non-zero same-side dwell reintroduced the delayed
    // takeover that made countersteer feel sticky, so normalize it away here.
    config_.smartCountersteerHandoffDelaySeconds = 0.0f;
    config_.manualSteeringTransitionSeconds = Clamp(
        NonNegativeOr(config_.manualSteeringTransitionSeconds,
                      defaults.manualSteeringTransitionSeconds),
        0.0f,
        kMaximumManualSteeringTransitionSeconds);
    config_.manualSameDirectionTargetYawRateRadS = Clamp(
        NonNegativeOr(config_.manualSameDirectionTargetYawRateRadS,
                      defaults.manualSameDirectionTargetYawRateRadS),
        0.0f,
        kMaximumManualSameDirectionYawRateRadS);
    config_.manualSameDirectionYawAccelerationRadS2 = Clamp(
        NonNegativeOr(config_.manualSameDirectionYawAccelerationRadS2,
                      defaults.manualSameDirectionYawAccelerationRadS2),
        0.0f,
        kMaximumManualSameDirectionYawAccelerationRadS2);
    config_.manualSameDirectionMaximumYawAngularDelta = Clamp(
        NonNegativeOr(config_.manualSameDirectionMaximumYawAngularDelta,
                      defaults.manualSameDirectionMaximumYawAngularDelta),
        0.0f,
        kMaximumManualYawAngularDeltaRadS);
    config_.manualSameDirectionRampSeconds = Clamp(
        NonNegativeOr(config_.manualSameDirectionRampSeconds,
                      defaults.manualSameDirectionRampSeconds),
        0.0f,
        kMaximumManualSameDirectionRampSeconds);
    config_.manualSameDirectionInitialStrength = UnitOr(
        config_.manualSameDirectionInitialStrength,
        defaults.manualSameDirectionInitialStrength);
    config_.manualOppositeMaximumRecoveryYawRateRadS = Clamp(
        NonNegativeOr(config_.manualOppositeMaximumRecoveryYawRateRadS,
                      defaults.manualOppositeMaximumRecoveryYawRateRadS),
        0.0f,
        kMaximumManualOppositeRecoveryYawRateRadS);
    config_.manualOppositeYawDampingAccelerationRadS2 = Clamp(
        NonNegativeOr(config_.manualOppositeYawDampingAccelerationRadS2,
                      defaults.manualOppositeYawDampingAccelerationRadS2),
        0.0f,
        kMaximumManualOppositeYawAccelerationRadS2);
    config_.manualOppositeMaximumYawAngularDelta = Clamp(
        NonNegativeOr(config_.manualOppositeMaximumYawAngularDelta,
                      defaults.manualOppositeMaximumYawAngularDelta),
        0.0f,
        kMaximumManualYawAngularDeltaRadS);
    config_.manualYawHardBodyOffsetRad = Clamp(
        NonNegativeOr(config_.manualYawHardBodyOffsetRad,
                      defaults.manualYawHardBodyOffsetRad),
        0.0f,
        kMaximumManualYawHardBodyOffsetRad);
    config_.pendulumTransitionZeroBandRad = Clamp(
        NonNegativeOr(config_.pendulumTransitionZeroBandRad,
                      defaults.pendulumTransitionZeroBandRad),
        0.0f,
        kMaximumPendulumTransitionZeroBandRad);
    config_.pendulumTransitionConfirmBodyOffsetRad = Clamp(
        NonNegativeOr(config_.pendulumTransitionConfirmBodyOffsetRad,
                      defaults.pendulumTransitionConfirmBodyOffsetRad),
        config_.pendulumTransitionZeroBandRad,
        kMaximumPendulumTransitionConfirmOffsetRad);
    config_.pendulumTransitionWindowSeconds = Clamp(
        NonNegativeOr(config_.pendulumTransitionWindowSeconds,
                      defaults.pendulumTransitionWindowSeconds),
        0.0f,
        kMaximumPendulumTransitionWindowSeconds);
    config_.sameDirectionFullSteerSeconds = Clamp(
        NonNegativeOr(config_.sameDirectionFullSteerSeconds,
                      defaults.sameDirectionFullSteerSeconds),
        0.0f,
        kMaximumSameDirectionFullSteerSeconds);
    config_.countersteerTransitionSeconds = Clamp(
        NonNegativeOr(config_.countersteerTransitionSeconds,
                      defaults.countersteerTransitionSeconds),
        0.0f,
        kMaximumCountersteerTransitionSeconds);
    config_.maximumCountersteerAngleRad = Clamp(
        NonNegativeOr(config_.maximumCountersteerAngleRad,
                      defaults.maximumCountersteerAngleRad),
        0.0f,
        DegToRad(35.0f));
    config_.minimumCountersteerAngleRad = Clamp(
        NonNegativeOr(config_.minimumCountersteerAngleRad,
                      defaults.minimumCountersteerAngleRad),
        0.0f,
        config_.maximumCountersteerAngleRad);
    config_.maximumBodyOffsetRad = Clamp(
        NonNegativeOr(config_.maximumBodyOffsetRad, defaults.maximumBodyOffsetRad),
        0.0f,
        kPi);
    config_.exitBodyOffsetRad = Clamp(
        NonNegativeOr(config_.exitBodyOffsetRad, defaults.exitBodyOffsetRad),
        0.0f,
        config_.maximumBodyOffsetRad);
    config_.noDirectionTimeoutSeconds = NonNegativeOr(
        config_.noDirectionTimeoutSeconds, defaults.noDirectionTimeoutSeconds);
    config_.minimumDriftBodyOffsetRad = Clamp(
        NonNegativeOr(config_.minimumDriftBodyOffsetRad,
                      defaults.minimumDriftBodyOffsetRad),
        0.0f,
        config_.maximumBodyOffsetRad);
    config_.bodyOffsetHoldSpeedRadS = NonNegativeOr(
        config_.bodyOffsetHoldSpeedRadS, defaults.bodyOffsetHoldSpeedRadS);
    config_.bodyOffsetHoldYawAccelerationRadS2 = Clamp(
        NonNegativeOr(config_.bodyOffsetHoldYawAccelerationRadS2,
                      defaults.bodyOffsetHoldYawAccelerationRadS2),
        0.0f,
        kMaximumFastYawAccelerationRadS2);
    config_.bodyOffsetHoldMaximumYawAngularDelta = Clamp(
        NonNegativeOr(config_.bodyOffsetHoldMaximumYawAngularDelta,
                      defaults.bodyOffsetHoldMaximumYawAngularDelta),
        0.0f,
        kMaximumFastYawAngularDeltaRadS);
    config_.bodyOffsetHoldSlowdownOffsetRad = Clamp(
        NonNegativeOr(config_.bodyOffsetHoldSlowdownOffsetRad,
                      defaults.bodyOffsetHoldSlowdownOffsetRad),
        0.0f,
        kPi);
    config_.driftAngleEntrySeconds = Clamp(
        NonNegativeOr(config_.driftAngleEntrySeconds,
                      defaults.driftAngleEntrySeconds),
        0.0f,
        kMaximumDriftAngleEntrySeconds);
    config_.driftAngleEntrySpeedRadS = Clamp(
        NonNegativeOr(config_.driftAngleEntrySpeedRadS,
                      defaults.driftAngleEntrySpeedRadS),
        0.0f,
        kMaximumDriftAngleEntrySpeedRadS);
    config_.driftAngleEntryYawAccelerationRadS2 = Clamp(
        NonNegativeOr(config_.driftAngleEntryYawAccelerationRadS2,
                      defaults.driftAngleEntryYawAccelerationRadS2),
        0.0f,
        kMaximumDriftAngleEntryAccelerationRadS2);
    config_.driftAngleEntryOffsetCutoffRad = Clamp(
        NonNegativeOr(config_.driftAngleEntryOffsetCutoffRad,
                      defaults.driftAngleEntryOffsetCutoffRad),
        0.0f,
        std::min(config_.minimumDriftBodyOffsetRad,
                 kMaximumDriftAngleEntryOffsetCutoffRad));
    config_.driftAngleEntryMaximumYawAngularDelta = Clamp(
        NonNegativeOr(config_.driftAngleEntryMaximumYawAngularDelta,
                      defaults.driftAngleEntryMaximumYawAngularDelta),
        0.0f,
        kMaximumDriftAngleEntryAngularDeltaRadS);
    config_.bodyRotationSpeedRadS = NonNegativeOr(
        config_.bodyRotationSpeedRadS, defaults.bodyRotationSpeedRadS);
    config_.bodyRotationSlowdownOffsetRad = Clamp(
        NonNegativeOr(config_.bodyRotationSlowdownOffsetRad,
                      defaults.bodyRotationSlowdownOffsetRad),
        0.0f,
        kPi);
    config_.entryYawBoostSeconds = Clamp(
        NonNegativeOr(config_.entryYawBoostSeconds,
                      defaults.entryYawBoostSeconds),
        0.0f,
        kMaximumEntryYawBoostSeconds);
    config_.entryYawBoostRateRadS = Clamp(
        NonNegativeOr(config_.entryYawBoostRateRadS,
                      defaults.entryYawBoostRateRadS),
        0.0f,
        kMaximumEntryYawBoostRateRadS);
    config_.entryYawBoostAccelerationRadS2 = Clamp(
        NonNegativeOr(config_.entryYawBoostAccelerationRadS2,
                      defaults.entryYawBoostAccelerationRadS2),
        0.0f,
        kMaximumEntryYawBoostAccelerationRadS2);
    config_.entryYawBoostOffsetCutoffRad = Clamp(
        NonNegativeOr(config_.entryYawBoostOffsetCutoffRad,
                      defaults.entryYawBoostOffsetCutoffRad),
        0.0f,
        kMaximumEntryYawBoostOffsetCutoffRad);
    config_.entryYawBoostMaximumYawAngularDelta = Clamp(
        NonNegativeOr(config_.entryYawBoostMaximumYawAngularDelta,
                      defaults.entryYawBoostMaximumYawAngularDelta),
        0.0f,
        kMaximumEntryYawBoostAngularDeltaRadS);
    config_.bodyYawAccelerationRadS2 = Clamp(
        NonNegativeOr(config_.bodyYawAccelerationRadS2,
                      defaults.bodyYawAccelerationRadS2),
        0.0f,
        kMaximumBodyYawAccelerationRadS2);
    config_.directionChangeSpeedRadS = NonNegativeOr(
        config_.directionChangeSpeedRadS, defaults.directionChangeSpeedRadS);
    config_.directionChangeYawAccelerationRadS2 = Clamp(
        NonNegativeOr(config_.directionChangeYawAccelerationRadS2,
                      defaults.directionChangeYawAccelerationRadS2),
        0.0f,
        kMaximumFastYawAccelerationRadS2);
    config_.directionChangeMaximumYawAngularDelta = Clamp(
        NonNegativeOr(config_.directionChangeMaximumYawAngularDelta,
                      defaults.directionChangeMaximumYawAngularDelta),
        0.0f,
        kMaximumFastYawAngularDeltaRadS);
    config_.directionChangeSlowdownOffsetRad = Clamp(
        NonNegativeOr(config_.directionChangeSlowdownOffsetRad,
                      defaults.directionChangeSlowdownOffsetRad),
        0.0f,
        kPi);
    config_.autoCenterSpeedRadS = NonNegativeOr(
        config_.autoCenterSpeedRadS, defaults.autoCenterSpeedRadS);
    config_.autoCenterYawAccelerationRadS2 = Clamp(
        NonNegativeOr(config_.autoCenterYawAccelerationRadS2,
                      defaults.autoCenterYawAccelerationRadS2),
        0.0f,
        kMaximumFastYawAccelerationRadS2);
    config_.autoCenterMaximumYawAngularDelta = Clamp(
        NonNegativeOr(config_.autoCenterMaximumYawAngularDelta,
                      defaults.autoCenterMaximumYawAngularDelta),
        0.0f,
        kMaximumFastYawAngularDeltaRadS);
    config_.autoCenterSlowdownOffsetRad = Clamp(
        NonNegativeOr(config_.autoCenterSlowdownOffsetRad,
                      defaults.autoCenterSlowdownOffsetRad),
        0.0f,
        kPi);
    config_.exitDriftAngleRateRadS = NonNegativeOr(
        config_.exitDriftAngleRateRadS, defaults.exitDriftAngleRateRadS);
    config_.exitSettleSeconds = Clamp(
        NonNegativeOr(config_.exitSettleSeconds, defaults.exitSettleSeconds),
        0.0f,
        kMaximumExitSettleSeconds);
    config_.driftAngleRateFilterSeconds = Clamp(
        NonNegativeOr(config_.driftAngleRateFilterSeconds,
                      defaults.driftAngleRateFilterSeconds),
        0.0f,
        kMaximumDriftAngleRateFilterSeconds);
    config_.frontGripMultiplierDuringDrift = Clamp(
        FiniteOr(config_.frontGripMultiplierDuringDrift,
                 defaults.frontGripMultiplierDuringDrift),
        0.0f,
        3.0f);
    config_.rearDriveMultiplierDuringDrift = Clamp(
        FiniteOr(config_.rearDriveMultiplierDuringDrift,
                 defaults.rearDriveMultiplierDuringDrift),
        0.0f,
        3.0f);

    config_.maximumSideslipRad = Clamp(
        NonNegativeOr(config_.maximumSideslipRad, defaults.maximumSideslipRad), 0.0f, 0.5f * kPi);
    config_.targetSideslipRad = std::min(
        NonNegativeOr(config_.targetSideslipRad, defaults.targetSideslipRad),
        config_.maximumSideslipRad);
    config_.wheelbaseM = std::max(0.10f, FiniteOr(config_.wheelbaseM, defaults.wheelbaseM));
    config_.maximumSteerAngleRad = Clamp(
        NonNegativeOr(config_.maximumSteerAngleRad, defaults.maximumSteerAngleRad),
        0.0f, DegToRad(85.0f));
    config_.yawReferenceScale = NonNegativeOr(config_.yawReferenceScale, defaults.yawReferenceScale);
    config_.maximumYawRateRadS =
        NonNegativeOr(config_.maximumYawRateRadS, defaults.maximumYawRateRadS);

    config_.sideslipKp = NonNegativeOr(config_.sideslipKp, defaults.sideslipKp);
    config_.sideslipKd = NonNegativeOr(config_.sideslipKd, defaults.sideslipKd);
    config_.yawRateKp = NonNegativeOr(config_.yawRateKp, defaults.yawRateKp);
    config_.yawRateDamping = NonNegativeOr(config_.yawRateDamping, defaults.yawRateDamping);
    config_.maximumSteeringCorrection =
        UnitOr(config_.maximumSteeringCorrection, defaults.maximumSteeringCorrection);
    config_.maximumSteeringCorrectionRate = NonNegativeOr(
        config_.maximumSteeringCorrectionRate, defaults.maximumSteeringCorrectionRate);
    config_.steeringAuthority = UnitOr(config_.steeringAuthority, defaults.steeringAuthority);

    config_.blendInSeconds = NonNegativeOr(config_.blendInSeconds, defaults.blendInSeconds);
    config_.blendOutSeconds = NonNegativeOr(config_.blendOutSeconds, defaults.blendOutSeconds);
    config_.collisionAuthority = UnitOr(config_.collisionAuthority, defaults.collisionAuthority);

    config_.rollKp = NonNegativeOr(config_.rollKp, defaults.rollKp);
    config_.rollKd = NonNegativeOr(config_.rollKd, defaults.rollKd);
    config_.pitchKp = NonNegativeOr(config_.pitchKp, defaults.pitchKp);
    config_.pitchKd = NonNegativeOr(config_.pitchKd, defaults.pitchKd);
    config_.yawVelocityKp = NonNegativeOr(config_.yawVelocityKp, defaults.yawVelocityKp);
    config_.attitudeAuthority = UnitOr(config_.attitudeAuthority, defaults.attitudeAuthority);
    config_.airborneAuthority = UnitOr(config_.airborneAuthority, defaults.airborneAuthority);
    config_.maximumRollAngularDelta =
        NonNegativeOr(config_.maximumRollAngularDelta, defaults.maximumRollAngularDelta);
    config_.maximumPitchAngularDelta =
        NonNegativeOr(config_.maximumPitchAngularDelta, defaults.maximumPitchAngularDelta);
    config_.maximumYawAngularDelta =
        NonNegativeOr(config_.maximumYawAngularDelta, defaults.maximumYawAngularDelta);

    config_.throttleCutAtMaximumSlip =
        UnitOr(config_.throttleCutAtMaximumSlip, defaults.throttleCutAtMaximumSlip);
    config_.recoveryBrakeAtMaximumSlip =
        UnitOr(config_.recoveryBrakeAtMaximumSlip, defaults.recoveryBrakeAtMaximumSlip);
    config_.recoverySideslipRad = std::min(
        NonNegativeOr(config_.recoverySideslipRad, defaults.recoverySideslipRad),
        config_.maximumSideslipRad);

    if (!config_.enabled) {
        reset();
    }
}

void DriftAssistController::reset() {
    phase_ = DriftPhase::Off;
    blend_ = 0.0f;
    driftSide_ = 0;
    previousSideslip_ = 0.0f;
    previousSteeringDelta_ = 0.0f;
    initialized_ = false;
    resetHandbrakeState();
}

void DriftAssistController::resetHandbrakeState() {
    noDirectionSeconds_ = 0.0f;
    handbrakeModeActive_ = false;
    handbrakeRearmRequired_ = false;
    groundContactLossSeconds_ = 0.0f;
    hasMeasuredBodyOffset_ = false;
    previousMeasuredBodyOffsetRad_ = 0.0f;
    exitSettleSeconds_ = 0.0f;
    steeringNeutralSeconds_ = 0.0f;
    countersteerHandoffSeconds_ = 0.0f;
    manualSteeringSuppressed_ = false;
    smartCountersteerThresholdReached_ = false;
    smartCountersteerOwnsSteering_ = false;
    manualSameDirectionSeconds_ = 0.0f;
    manualSameDirectionSide_ = 0;
    sideTransitionActive_ = false;
    sideTransitionTargetSide_ = 0;
    sideTransitionSeconds_ = 0.0f;
    driftSide_ = 0;
    phase_ = DriftPhase::Off;
    blend_ = 0.0f;
}

void DriftAssistController::notifyAttitudeWriteSkipped() {
    // Manual yaw commands are stateless and re-sampled by the host, so there is
    // no controller-side prediction to roll back.
}

void DriftAssistController::notifyAttitudeWritePlanned(float /*yawDeltaRadS*/) {
    // Manual yaw commands are stateless; measured body motion remains the only
    // source of feedback on the next input sample.
}

void DriftAssistController::updatePhase(bool candidate, float dt) {
    if (candidate) {
        if (phase_ == DriftPhase::Off || phase_ == DriftPhase::Exiting) {
            phase_ = DriftPhase::Entering;
        }

        if (config_.blendInSeconds <= 1.0e-5f) {
            blend_ = 1.0f;
        } else {
            blend_ = Clamp(blend_ + dt / config_.blendInSeconds, 0.0f, 1.0f);
        }
        if (blend_ >= 1.0f) {
            phase_ = DriftPhase::Holding;
        }
        return;
    }

    if (phase_ == DriftPhase::Off) {
        blend_ = 0.0f;
        return;
    }
    phase_ = DriftPhase::Exiting;
    if (config_.blendOutSeconds <= 1.0e-5f) {
        blend_ = 0.0f;
    } else {
        blend_ = Clamp(blend_ - dt / config_.blendOutSeconds, 0.0f, 1.0f);
    }
    if (blend_ <= 0.0f) {
        phase_ = DriftPhase::Off;
        driftSide_ = 0;
        previousSteeringDelta_ = 0.0f;
    }
}

bool DriftAssistController::activationRequested(const VehicleState& state,
                                                float sideslip,
                                                float rearSlip) const {
    const bool alreadyEngaged = phase_ != DriftPhase::Off;
    const float sideslipThreshold = alreadyEngaged
                                        ? config_.exitSideslipRad
                                        : config_.enterSideslipRad;
    const float rearSlipThreshold = alreadyEngaged
                                        ? config_.exitRearSlip
                                        : config_.enterRearSlip;
    const float handbrakeThreshold = alreadyEngaged
                                         ? 0.5f * config_.enterHandbrake
                                         : config_.enterHandbrake;

    const bool manual = state.assistRequested;
    const bool automatic =
        std::fabs(sideslip) >= sideslipThreshold ||
        rearSlip >= rearSlipThreshold ||
        Clamp(FiniteOr(state.handbrakeInput, 0.0f), 0.0f, 1.0f) >= handbrakeThreshold;

    switch (config_.activation) {
    case ActivationMode::Manual:
        return manual;
    case ActivationMode::Automatic:
        return automatic;
    case ActivationMode::ManualOrAutomatic:
        return manual || automatic;
    default:
        return false;
    }
}

void DriftAssistController::chooseDriftSide(float sideslip, float steering, float yawRate) {
    if (driftSide_ != 0) {
        if (std::fabs(sideslip) >= config_.enterSideslipRad &&
            Sign(sideslip) != static_cast<float>(driftSide_)) {
            driftSide_ = sideslip > 0.0f ? 1 : -1;
        }
        return;
    }

    if (std::fabs(sideslip) > DegToRad(0.5f)) {
        driftSide_ = sideslip > 0.0f ? 1 : -1;
    } else if (std::fabs(steering) > 0.02f) {
        // Countersteer and sideslip normally have the opposite sign to the turn input.
        driftSide_ = steering > 0.0f ? -1 : 1;
    } else if (std::fabs(yawRate) > 0.02f) {
        driftSide_ = yawRate > 0.0f ? -1 : 1;
    }
}

ControlOutput DriftAssistController::updateHandbrakeMode(const VehicleState& state,
                                                         float dt,
                                                         bool activationMotionEligible,
                                                         bool activeMotionEligible,
                                                         bool contactEligible,
                                                         ControlOutput output) {
    const float originalSteering = Clamp(
        FiniteOr(state.steeringInput, 0.0f), -1.0f, 1.0f);
    const float handbrake = Clamp(
        FiniteOr(state.handbrakeInput, 0.0f), 0.0f, 1.0f);
    const bool handbrakePressed =
        handbrake > 0.0f &&
        handbrake >= config_.handbrakeActivationThreshold;
    const bool currentManualInput =
        std::fabs(originalSteering) > config_.directionDeadzone;
    const float steeringOwnershipDt =
        IsFinite(state.steeringOwnershipDt) &&
                state.steeringOwnershipDt > 0.0f &&
                state.steeringOwnershipDt <= kMaximumSteeringOwnershipDt
            ? state.steeringOwnershipDt
            : dt;
    const float bodyOffset = Clamp(
        -output.sideslipAngleRad, -0.5f * kPi, 0.5f * kPi);

    const auto clearYawOutputs = [&]() {
        output.bodyYawOffsetTargetRad = 0.0f;
        output.bodyYawRateTargetRadS = 0.0f;
        output.bodyYawAccelerationLimitRadS2 = 0.0f;
        output.bodyYawVelocityDeltaLimitRadS = 0.0f;
        output.manualYawStrength = 0.0f;
        output.manualYawControlSide = 0;
        output.entryYawBoostBlend = 0.0f;
        output.yawResponseMode = YawResponseMode::None;
        output.acquiringMinimumDriftAngle = false;
        output.bodyOffsetBrakeActive = false;
        output.manualYawBodyOffsetLimited = false;
        output.targetYawRateRadS = 0.0f;
        output.angularVelocityDeltaLocal.y = 0.0f;
    };
    const auto setPassthrough = [&]() {
        output.forceCountersteer = false;
        output.smartCountersteerActive = false;
        output.manualSteeringOverride = currentManualInput;
        output.steeringCommand = originalSteering;
        output.steeringDelta = 0.0f;
    };
    const auto setInactiveOutput = [&]() {
        output.active = false;
        output.phase = DriftPhase::Off;
        output.blend = 0.0f;
        output.driftSide = 0;
        output.centering = false;
        output.countersteerAngleRad = 0.0f;
        output.countersteerCommand = originalSteering;
        output.smartCountersteerCommand = 0.0f;
        output.steeringNeutralSeconds = 0.0f;
        output.countersteerHandoffSeconds = 0.0f;
        output.countersteerHandoffPending = false;
        output.sideTransitionActive = false;
        output.sideTransitionTargetSide = 0;
        output.sideTransitionSeconds = 0.0f;
        output.steeringAssistMode = SteeringAssistMode::None;
        output.bodyYawOffsetRad = 0.0f;
        output.bodyYawOffsetSafetyRad = 0.0f;
        output.bodyYawOffsetRateRadS = 0.0f;
        output.frontGripScale = 1.0f;
        output.rearDriveScale = 1.0f;
        output.groundContactGraceActive = false;
        setPassthrough();
        output.manualSteeringOverride = false;
        clearYawOutputs();
    };

    output.frontGripScale = 1.0f;
    output.rearDriveScale = 1.0f;
    output.countersteerCommand = originalSteering;
    output.smartCountersteerCommand = 0.0f;
    output.steeringNeutralSeconds = steeringNeutralSeconds_;
    output.countersteerHandoffSeconds = countersteerHandoffSeconds_;
    output.countersteerHandoffPending = false;
    output.sideTransitionActive = sideTransitionActive_;
    output.sideTransitionTargetSide = sideTransitionTargetSide_;
    output.sideTransitionSeconds = sideTransitionSeconds_;
    output.steeringAssistMode = SteeringAssistMode::None;
    output.handbrakeHoldSeconds = 0.0f;
    output.handbrakeHoldProgress = handbrakeModeActive_ ? 1.0f : 0.0f;
    output.groundContactLossSeconds = groundContactLossSeconds_;
    output.groundContactGraceActive = false;
    setPassthrough();
    clearYawOutputs();

    if (!handbrakeModeActive_) {
        if (handbrakeRearmRequired_) {
            if (handbrakePressed) {
                phase_ = DriftPhase::Off;
                blend_ = 0.0f;
                setInactiveOutput();
                previousSideslip_ = output.sideslipAngleRad;
                initialized_ = true;
                return output;
            }
            handbrakeRearmRequired_ = false;
        }

        if (!handbrakePressed) {
            phase_ = DriftPhase::Off;
            blend_ = 0.0f;
            setInactiveOutput();
            previousSideslip_ = output.sideslipAngleRad;
            initialized_ = true;
            return output;
        }

        if (!activationMotionEligible || !contactEligible) {
            phase_ = DriftPhase::Charging;
            blend_ = 0.0f;
            setInactiveOutput();
            output.phase = DriftPhase::Charging;
            previousSideslip_ = output.sideslipAngleRad;
            initialized_ = true;
            return output;
        }

        // Reaching the handbrake threshold creates the session immediately.
        // The legacy long-press duration remains in AssistConfig only so old
        // profiles still parse; it no longer delays activation.
        handbrakeModeActive_ = true;
        groundContactLossSeconds_ = 0.0f;
        noDirectionSeconds_ = 0.0f;
        steeringNeutralSeconds_ = 0.0f;
        countersteerHandoffSeconds_ = 0.0f;
        manualSteeringSuppressed_ = false;
        smartCountersteerThresholdReached_ = false;
        smartCountersteerOwnsSteering_ = false;
        manualSameDirectionSeconds_ = 0.0f;
        manualSameDirectionSide_ = 0;
        sideTransitionActive_ = false;
        sideTransitionTargetSide_ = 0;
        sideTransitionSeconds_ = 0.0f;
        hasMeasuredBodyOffset_ = true;
        previousMeasuredBodyOffsetRad_ = bodyOffset;
        exitSettleSeconds_ = 0.0f;
        driftSide_ = 0;
        blend_ = 0.0f;
        phase_ = DriftPhase::WaitingDirection;
    }

    const bool reverseInvalid =
        !config_.allowReverse && output.localLongitudinalSpeed <= 0.0f;
    if (contactEligible) {
        groundContactLossSeconds_ = 0.0f;
    } else {
        groundContactLossSeconds_ = std::min(
            groundContactLossSeconds_ + dt,
            config_.groundContactLossGraceSeconds);
    }
    const bool contactGraceExpired =
        !contactEligible &&
        (config_.groundContactLossGraceSeconds <= kMinimumVectorLength ||
         groundContactLossSeconds_ + std::max(kMinimumVectorLength,
                                              dt * 0.01f) >=
             config_.groundContactLossGraceSeconds);
    output.groundContactLossSeconds = groundContactLossSeconds_;
    output.groundContactGraceActive = !contactEligible && !contactGraceExpired;
    if (!activeMotionEligible || contactGraceExpired || reverseInvalid ||
        state.collisionRecent) {
        const bool requireRelease = handbrakePressed;
        resetHandbrakeState();
        handbrakeRearmRequired_ = requireRelease;
        setInactiveOutput();
        previousSideslip_ = output.sideslipAngleRad;
        initialized_ = true;
        return output;
    }

    const bool hadPreviousBodyOffset = hasMeasuredBodyOffset_;
    const float previousBodyOffset = previousMeasuredBodyOffsetRad_;
    float bodyOffsetRate = 0.0f;
    if (hadPreviousBodyOffset && dt > kMinimumVectorLength) {
        bodyOffsetRate = Clamp(
            WrapPi(bodyOffset - previousBodyOffset) / dt,
            -kMaximumBodyYawRate,
            kMaximumBodyYawRate);
    }
    hasMeasuredBodyOffset_ = true;
    previousMeasuredBodyOffsetRad_ = bodyOffset;

    if (config_.blendInSeconds <= kMinimumVectorLength) {
        blend_ = 1.0f;
    } else {
        blend_ = Clamp(
            blend_ + dt / config_.blendInSeconds, 0.0f, 1.0f);
    }

    output.active = true;
    output.blend = blend_;
    output.bodyYawOffsetRad = bodyOffset;
    output.bodyYawOffsetSafetyRad = bodyOffset;
    output.bodyYawOffsetRateRadS = bodyOffsetRate;
    output.handbrakeHoldProgress = 1.0f;

    const float absoluteBodyOffset = std::fabs(bodyOffset);
    const int manualDirection = originalSteering < -config_.directionDeadzone
                                    ? -1
                                    : (originalSteering > config_.directionDeadzone
                                           ? 1
                                           : 0);
    constexpr std::uint8_t kKnownSteeringDirectionMask =
        kNegativeSteeringDirectionObserved |
        kPositiveSteeringDirectionObserved;
    const std::uint8_t reportedManualDirectionMask =
        state.manualSteeringDirectionMaskObserved &
        kKnownSteeringDirectionMask;
    const std::uint8_t observedManualDirectionMask =
        reportedManualDirectionMask |
        SteeringDirectionObservationMask(manualDirection);

    if (sideTransitionActive_) {
        sideTransitionSeconds_ = std::min(
            sideTransitionSeconds_ + dt,
            config_.pendulumTransitionWindowSeconds);
        const bool confirmedNewSide =
            sideTransitionTargetSide_ != 0 &&
            bodyOffset * static_cast<float>(sideTransitionTargetSide_) +
                    kMinimumVectorLength >=
                config_.pendulumTransitionConfirmBodyOffsetRad;
        const bool returnedToOldSide =
            driftSide_ != 0 &&
            bodyOffset * static_cast<float>(driftSide_) +
                    kMinimumVectorLength >=
                config_.pendulumTransitionConfirmBodyOffsetRad;
        const bool expired =
            config_.pendulumTransitionWindowSeconds <= kMinimumVectorLength ||
            sideTransitionSeconds_ + kMinimumVectorLength >=
                config_.pendulumTransitionWindowSeconds;

        smartCountersteerOwnsSteering_ = false;
        countersteerHandoffSeconds_ = 0.0f;
        manualSameDirectionSeconds_ = 0.0f;
        manualSameDirectionSide_ = 0;
        setPassthrough();
        clearYawOutputs();

        if (confirmedNewSide) {
            driftSide_ = sideTransitionTargetSide_;
            sideTransitionActive_ = false;
            sideTransitionTargetSide_ = 0;
            sideTransitionSeconds_ = 0.0f;
            output.sideTransitionActive = false;
            output.sideTransitionTargetSide = 0;
            output.sideTransitionSeconds = 0.0f;
            smartCountersteerThresholdReached_ = false;
            exitSettleSeconds_ = 0.0f;
        } else if (returnedToOldSide || manualDirection == driftSide_ ||
                   expired) {
            const bool requireRelease = handbrakePressed;
            resetHandbrakeState();
            handbrakeRearmRequired_ = requireRelease;
            setInactiveOutput();
            previousSideslip_ = output.sideslipAngleRad;
            initialized_ = true;
            return output;
        } else {
            phase_ = DriftPhase::SideTransition;
            output.phase = phase_;
            output.driftSide = driftSide_;
            output.sideTransitionActive = true;
            output.sideTransitionTargetSide = sideTransitionTargetSide_;
            output.sideTransitionSeconds = sideTransitionSeconds_;
            output.frontGripScale = BlendMultiplier(
                config_.frontGripMultiplierDuringDrift, blend_);
            output.rearDriveScale = BlendMultiplier(
                config_.rearDriveMultiplierDuringDrift, blend_);
            previousSideslip_ = output.sideslipAngleRad;
            initialized_ = true;
            return output;
        }
    }

    if (driftSide_ == 0) {
        if (absoluteBodyOffset + kMinimumVectorLength >=
                config_.pendulumTransitionConfirmBodyOffsetRad &&
            absoluteBodyOffset > kMinimumVectorLength) {
            driftSide_ = bodyOffset < 0.0f ? -1 : 1;
        } else if (currentManualInput) {
            driftSide_ = manualDirection;
        }
    } else if (!smartCountersteerThresholdReached_ &&
               absoluteBodyOffset + kMinimumVectorLength >=
                   config_.pendulumTransitionConfirmBodyOffsetRad &&
               bodyOffset * static_cast<float>(driftSide_) < 0.0f) {
        // Before the first established 15-degree drift, measured motion may
        // correct a provisional side chosen from the initial steering input.
        driftSide_ = bodyOffset < 0.0f ? -1 : 1;
        manualSameDirectionSeconds_ = 0.0f;
        manualSameDirectionSide_ = 0;
    }

    const bool angleEligible =
        (driftSide_ == -1 || driftSide_ == 1) &&
        bodyOffset * static_cast<float>(driftSide_) +
                kMinimumVectorLength >=
            config_.countersteerActivationBodyOffsetRad;
    if (angleEligible) {
        smartCountersteerThresholdReached_ = true;
    }

    const bool reachedTransitionBand =
        smartCountersteerThresholdReached_ &&
        (driftSide_ == -1 || driftSide_ == 1) &&
        bodyOffset * static_cast<float>(driftSide_) <=
            config_.pendulumTransitionZeroBandRad + kMinimumVectorLength;
    if (reachedTransitionBand && manualDirection == -driftSide_) {
        if (smartCountersteerOwnsSteering_ || manualSteeringSuppressed_) {
            manualSteeringSuppressed_ = true;
            steeringNeutralSeconds_ = 0.0f;
        }
        sideTransitionActive_ = true;
        sideTransitionTargetSide_ = -driftSide_;
        sideTransitionSeconds_ = 0.0f;
        smartCountersteerOwnsSteering_ = false;
        countersteerHandoffSeconds_ = 0.0f;
        manualSameDirectionSeconds_ = 0.0f;
        manualSameDirectionSide_ = 0;
        phase_ = DriftPhase::SideTransition;
        output.phase = phase_;
        output.driftSide = driftSide_;
        output.sideTransitionActive = true;
        output.sideTransitionTargetSide = sideTransitionTargetSide_;
        output.sideTransitionSeconds = 0.0f;
        setPassthrough();
        clearYawOutputs();
        output.frontGripScale = BlendMultiplier(
            config_.frontGripMultiplierDuringDrift, blend_);
        output.rearDriveScale = BlendMultiplier(
            config_.rearDriveMultiplierDuringDrift, blend_);
        previousSideslip_ = output.sideslipAngleRad;
        initialized_ = true;
        return output;
    }
    if (reachedTransitionBand && !currentManualInput) {
        const bool requireRelease = handbrakePressed;
        resetHandbrakeState();
        handbrakeRearmRequired_ = requireRelease;
        setInactiveOutput();
        previousSideslip_ = output.sideslipAngleRad;
        initialized_ = true;
        return output;
    }

    const bool observedManualEvent =
        currentManualInput || state.manualSteeringInputObserved ||
        observedManualDirectionMask != 0;
    const bool automaticInputAllowed = AutomaticCountersteerInputAllowed(
        currentManualInput, state.manualSteeringInputObserved,
        observedManualDirectionMask, driftSide_,
        smartCountersteerOwnsSteering_, smartCountersteerOwnsSteering_);
    const bool manualTakeoverEvent =
        observedManualEvent && !automaticInputAllowed;
    // Any observed player input releases automatic ownership, including input
    // in the automatic countersteer direction.  This keeps a deliberate
    // increase of countersteer from being clamped to the old automatic target.
    bool releasedAutomaticForManualInput = false;
    bool countersteerHandoffPending = false;

    if (smartCountersteerOwnsSteering_ && manualTakeoverEvent) {
        smartCountersteerOwnsSteering_ = false;
        manualSteeringSuppressed_ = true;
        steeringNeutralSeconds_ = 0.0f;
        releasedAutomaticForManualInput = true;
    }
    countersteerHandoffSeconds_ = 0.0f;

    if (!smartCountersteerOwnsSteering_) {
        if (observedManualEvent &&
            (releasedAutomaticForManualInput || manualSteeringSuppressed_)) {
            manualSteeringSuppressed_ = true;
            steeringNeutralSeconds_ = 0.0f;
        } else if (manualSteeringSuppressed_ && !observedManualEvent) {
            const float delay = config_.smartCountersteerReengageDelaySeconds;
            steeringNeutralSeconds_ = std::min(
                steeringNeutralSeconds_ + steeringOwnershipDt, delay);
            if (steeringNeutralSeconds_ + kMinimumVectorLength >= delay) {
                manualSteeringSuppressed_ = false;
            }
        }
    }

    if (smartCountersteerOwnsSteering_ &&
        (!config_.enableSmartCountersteer ||
         absoluteBodyOffset <= config_.exitBodyOffsetRad)) {
        smartCountersteerOwnsSteering_ = false;
        countersteerHandoffSeconds_ = 0.0f;
        countersteerHandoffPending = false;
        if (manualTakeoverEvent) {
            // A player event coincident with the automatic release remains a
            // manual takeover and must not be forgotten by the reengagement
            // gate on the following neutral frame.
            manualSteeringSuppressed_ = true;
            steeringNeutralSeconds_ = 0.0f;
            releasedAutomaticForManualInput = true;
        }
    }
    output.steeringNeutralSeconds = steeringNeutralSeconds_;
    output.countersteerHandoffSeconds = countersteerHandoffSeconds_;
    output.countersteerHandoffPending = countersteerHandoffPending;

    const bool canAcquireOwnership =
        config_.enableSmartCountersteer && angleEligible &&
        !currentManualInput && !observedManualEvent &&
        !manualSteeringSuppressed_ &&
        config_.maximumSteerAngleRad > kMinimumVectorLength &&
        config_.smartCountersteerAngleRad > kMinimumVectorLength;
    if (!smartCountersteerOwnsSteering_ && canAcquireOwnership) {
        smartCountersteerOwnsSteering_ = true;
        countersteerHandoffSeconds_ = 0.0f;
        countersteerHandoffPending = false;
        output.countersteerHandoffSeconds = 0.0f;
        output.countersteerHandoffPending = false;
    }

    const bool hasCountersteerDirection =
        (driftSide_ == -1 || driftSide_ == 1) &&
        (angleEligible || smartCountersteerOwnsSteering_);
    if (hasCountersteerDirection) {
        output.countersteerAngleRad =
            -static_cast<float>(driftSide_) *
            config_.smartCountersteerAngleRad;
        output.smartCountersteerCommand =
            config_.maximumSteerAngleRad > kMinimumVectorLength
                ? Clamp(output.countersteerAngleRad /
                            config_.maximumSteerAngleRad,
                        -1.0f,
                        1.0f)
                : 0.0f;
        output.countersteerCommand = output.smartCountersteerCommand;
    }
    output.driftSide = driftSide_;

    const bool automaticControl = smartCountersteerOwnsSteering_;
    if (automaticControl) {
        output.forceCountersteer = true;
        output.smartCountersteerActive = true;
        output.manualSteeringOverride = false;
        output.steeringAssistMode = SteeringAssistMode::SmartCountersteer;
        output.steeringCommand = output.smartCountersteerCommand;
        output.steeringDelta = output.steeringCommand - originalSteering;
    } else if (currentManualInput) {
        output.steeringAssistMode = SteeringAssistMode::ManualOverride;
    } else if (manualSteeringSuppressed_) {
        output.steeringAssistMode = SteeringAssistMode::ReengageDelay;
    } else if (config_.enableSmartCountersteer) {
        output.steeringAssistMode = SteeringAssistMode::WaitingForDrift;
    }

    if (config_.enableManualYawAssist && AttitudeEnabled(config_.actuation) &&
        currentManualInput && !output.forceCountersteer &&
        (driftSide_ == -1 || driftSide_ == 1)) {
        float manualYawStrength = 1.0f;
        if (manualDirection == driftSide_) {
            if (manualSameDirectionSide_ == driftSide_) {
                manualSameDirectionSeconds_ = std::min(
                    manualSameDirectionSeconds_ + dt,
                    config_.manualSameDirectionRampSeconds);
            } else {
                manualSameDirectionSide_ = driftSide_;
                manualSameDirectionSeconds_ = 0.0f;
            }
            if (config_.manualSameDirectionRampSeconds <=
                kMinimumVectorLength) {
                manualYawStrength = 1.0f;
            } else {
                const float progress = Clamp(
                    manualSameDirectionSeconds_ /
                        config_.manualSameDirectionRampSeconds,
                    0.0f,
                    1.0f);
                manualYawStrength =
                    config_.manualSameDirectionInitialStrength +
                    (1.0f - config_.manualSameDirectionInitialStrength) *
                        progress;
            }
        } else {
            manualSameDirectionSeconds_ = 0.0f;
            manualSameDirectionSide_ = 0;
        }
        output.manualYawStrength = manualYawStrength;
        output.manualYawControlSide = driftSide_;
        const ManualYawCommand yawCommand = ComputeManualYawCommand(
            config_,
            bodyOffset,
            bodyOffsetRate,
            originalSteering,
            driftSide_,
            dt,
            manualYawStrength);
        output.targetYawRateRadS = yawCommand.targetYawRateRadS;
        output.bodyYawRateTargetRadS = yawCommand.targetYawRateRadS;
        output.bodyYawAccelerationLimitRadS2 =
            yawCommand.accelerationLimitRadS2;
        output.bodyYawVelocityDeltaLimitRadS =
            yawCommand.velocityDeltaLimitRadS;
        output.yawResponseMode = yawCommand.mode;
        output.bodyOffsetBrakeActive = yawCommand.dampingActive;
        output.manualYawBodyOffsetLimited = yawCommand.bodyOffsetLimited;
        output.angularVelocityDeltaLocal.y =
            yawCommand.yawVelocityDeltaRadS;
    } else {
        manualSameDirectionSeconds_ = 0.0f;
        manualSameDirectionSide_ = 0;
    }
    if (output.forceCountersteer) {
        // Front-wheel automatic control and body-yaw control are mutually
        // exclusive, even if future arbitration ordering changes.
        clearYawOutputs();
    }

    phase_ = smartCountersteerThresholdReached_
                 ? DriftPhase::Holding
                 : DriftPhase::WaitingDirection;
    output.phase = phase_;

    if (!smartCountersteerThresholdReached_) {
        if (!handbrakePressed && !observedManualEvent) {
            noDirectionSeconds_ += dt;
        } else {
            noDirectionSeconds_ = 0.0f;
        }
    } else {
        noDirectionSeconds_ = 0.0f;
    }

    bool finish = false;
    if (!smartCountersteerThresholdReached_ && !handbrakePressed) {
        finish = config_.noDirectionTimeoutSeconds <= kMinimumVectorLength ||
                 noDirectionSeconds_ + kMinimumVectorLength >=
                     config_.noDirectionTimeoutSeconds;
    }
    const bool settledNearCenter =
        smartCountersteerThresholdReached_ && !currentManualInput &&
        absoluteBodyOffset <= config_.exitBodyOffsetRad &&
        std::fabs(bodyOffsetRate) <= config_.exitDriftAngleRateRadS;
    if (settledNearCenter) {
        exitSettleSeconds_ += dt;
        finish = finish ||
                 config_.exitSettleSeconds <= kMinimumVectorLength ||
                 exitSettleSeconds_ + kMinimumVectorLength >=
                     config_.exitSettleSeconds;
    } else {
        exitSettleSeconds_ = 0.0f;
    }

    output.frontGripScale = BlendMultiplier(
        config_.frontGripMultiplierDuringDrift, blend_);
    output.rearDriveScale = BlendMultiplier(
        config_.rearDriveMultiplierDuringDrift, blend_);

    if (finish) {
        const bool requireRelease = handbrakePressed;
        resetHandbrakeState();
        handbrakeRearmRequired_ = requireRelease;
        setInactiveOutput();
    }

    previousSideslip_ = output.sideslipAngleRad;
    initialized_ = true;
    return output;
}

ControlOutput DriftAssistController::update(const VehicleState& state) {
    ControlOutput output{};
    output.steeringCommand = Clamp(FiniteOr(state.steeringInput, 0.0f), -1.0f, 1.0f);

    const Basis body = MakeOrthonormalBasis(state.body);
    const Vec3 linearVelocity = FiniteVectorOr(state.linearVelocityWorld, {});
    const Vec3 angularVelocity = FiniteVectorOr(state.angularVelocityWorld, {});
    // A large frame gap is not a valid physics sample. Clamping it would make
    // the hold timer and yaw controller advance by a value unrelated to the
    // game tick, so fail closed and let the host reset/re-sample instead.
    const bool validDt = IsFinite(state.dt) && state.dt > 0.0f &&
                         state.dt <= kMaximumControllerDt;
    const float dt = Clamp(FiniteOr(state.dt, 0.0f), 0.0f, kMaximumControllerDt);

    output.localLongitudinalSpeed = FiniteOr(Dot(linearVelocity, body.forward), 0.0f);
    output.localLateralSpeed = FiniteOr(Dot(linearVelocity, body.right), 0.0f);
    output.sideslipAngleRad = std::atan2(
        output.localLateralSpeed,
        std::fabs(output.localLongitudinalSpeed));
    output.sideslipAngleRad = Clamp(
        FiniteOr(output.sideslipAngleRad, 0.0f), -0.5f * kPi, 0.5f * kPi);
    output.yawRateRadS = FiniteOr(Dot(angularVelocity, body.up), 0.0f);

    if (initialized_ && dt > 1.0e-5f) {
        output.sideslipRateRadS = Clamp(
            WrapPi(output.sideslipAngleRad - previousSideslip_) / dt,
            -kMaximumMeasuredSideslipRate,
            kMaximumMeasuredSideslipRate);
    }

    Vec3 surfaceNormal = state.hasSurfaceNormal
                             ? NormalizeOr(FiniteVectorOr(state.surfaceNormal, body.up), body.up)
                             : body.up;
    if (Dot(surfaceNormal, body.up) < 0.0f) {
        surfaceNormal = -surfaceNormal;
    }
    const Vec3 attitudeErrorWorld = Cross(body.up, surfaceNormal);
    output.attitudeErrorLocal = {
        FiniteOr(Dot(attitudeErrorWorld, body.right), 0.0f),
        0.0f,
        FiniteOr(Dot(attitudeErrorWorld, body.forward), 0.0f),
    };

    const bool hardStop = !config_.enabled || !state.inRace || state.resetState || !validDt ||
                          (config_.requirePlayer && !state.playerControlled);
    if (hardStop) {
        reset();
        output.phase = phase_;
        return output;
    }

    float speed = FiniteOr(state.speedMps, 0.0f);
    if (speed <= 0.0f) {
        speed = Length(linearVelocity);
    }
    speed = IsFinite(speed) ? std::max(0.0f, speed) : 0.0f;

    const std::uint8_t groundedWheels = std::min<std::uint8_t>(state.groundedWheels, 4);
    const float longitudinalForGate = config_.allowReverse
                                          ? std::fabs(output.localLongitudinalSpeed)
                                          : output.localLongitudinalSpeed;
    const bool speedEligible = speed >= config_.minSpeedMps;
    const bool motionEligible = speedEligible &&
                                longitudinalForGate >= config_.minLongitudinalSpeedMps;
    const bool contactEligible = groundedWheels >= config_.minGroundedWheels;

    if (config_.activation == ActivationMode::HandbrakeHold) {
        return updateHandbrakeMode(state,
                                   dt,
                                   motionEligible,
                                   speedEligible,
                                   contactEligible,
                                   output);
    }

    const float rearSlip = AverageRearSlip(state);
    const bool candidate = motionEligible && contactEligible &&
                           activationRequested(state, output.sideslipAngleRad, rearSlip);

    if (candidate) {
        chooseDriftSide(output.sideslipAngleRad,
                        output.steeringCommand,
                        output.yawRateRadS);
    }
    updatePhase(candidate, dt);

    output.phase = phase_;
    output.blend = blend_;
    output.active = phase_ != DriftPhase::Off;
    output.driftSide = driftSide_;

    const float maximumTargetSideslip = std::min(
        config_.targetSideslipRad, config_.maximumSideslipRad);
    output.targetSideslipRad = static_cast<float>(driftSide_) * maximumTargetSideslip;

    const float steeringAngle = output.steeringCommand * config_.maximumSteerAngleRad;
    const float yawReference = output.localLongitudinalSpeed / config_.wheelbaseM *
                               std::tan(steeringAngle) * config_.yawReferenceScale;
    output.targetYawRateRadS = Clamp(
        FiniteOr(yawReference, 0.0f),
        -config_.maximumYawRateRadS,
        config_.maximumYawRateRadS);

    const float collisionScale = state.collisionRecent ? config_.collisionAuthority : 1.0f;
    const float commonAuthority = blend_ * collisionScale;

    if (SteeringEnabled(config_.actuation) && output.active) {
        const float sideslipError = output.sideslipAngleRad - output.targetSideslipRad;
        const float yawError = output.yawRateRadS - output.targetYawRateRadS;
        float desiredCorrection =
            config_.sideslipKp * sideslipError +
            config_.sideslipKd * output.sideslipRateRadS -
            config_.yawRateKp * yawError -
            config_.yawRateDamping * output.yawRateRadS;
        desiredCorrection = Clamp(
            FiniteOr(desiredCorrection, 0.0f),
            -config_.maximumSteeringCorrection,
            config_.maximumSteeringCorrection);
        desiredCorrection *= config_.steeringAuthority * commonAuthority;

        const float maximumStep = config_.maximumSteeringCorrectionRate * dt;
        float correction = MoveTowards(previousSteeringDelta_, desiredCorrection, maximumStep);
        correction = Clamp(correction,
                           -config_.maximumSteeringCorrection,
                           config_.maximumSteeringCorrection);
        output.steeringCommand = Clamp(output.steeringCommand + correction, -1.0f, 1.0f);
        output.steeringDelta = output.steeringCommand -
                               Clamp(FiniteOr(state.steeringInput, 0.0f), -1.0f, 1.0f);
        previousSteeringDelta_ = output.steeringDelta;
    } else {
        output.steeringDelta = 0.0f;
        previousSteeringDelta_ = 0.0f;
    }

    if (config_.enableThrottleProtection && output.active) {
        const float recovery = RecoveryAmount(std::fabs(output.sideslipAngleRad), config_);
        output.throttleScale = Clamp(
            1.0f - config_.throttleCutAtMaximumSlip * recovery * commonAuthority,
            0.0f,
            1.0f);
        output.brakeAdd = Clamp(
            config_.recoveryBrakeAtMaximumSlip * recovery * commonAuthority,
            0.0f,
            1.0f);
    }

    if (AttitudeEnabled(config_.actuation) && output.active) {
        const float minimumContacts = static_cast<float>(
            std::max<std::uint8_t>(config_.minGroundedWheels, 1));
        const float contactFraction = Clamp(
            static_cast<float>(groundedWheels) / minimumContacts, 0.0f, 1.0f);
        const float contactAuthority = config_.airborneAuthority +
                                       (1.0f - config_.airborneAuthority) * contactFraction;
        const float authority = config_.attitudeAuthority * commonAuthority * contactAuthority;

        const Vec3 localAngularVelocity{
            FiniteOr(Dot(angularVelocity, body.right), 0.0f),
            output.yawRateRadS,
            FiniteOr(Dot(angularVelocity, body.forward), 0.0f),
        };
        const float pitchDelta =
            (config_.pitchKp * output.attitudeErrorLocal.x -
             config_.pitchKd * localAngularVelocity.x) * dt;
        const float rollDelta =
            (config_.rollKp * output.attitudeErrorLocal.z -
             config_.rollKd * localAngularVelocity.z) * dt;

        output.angularVelocityDeltaLocal.x = authority * Clamp(
            FiniteOr(pitchDelta, 0.0f),
            -config_.maximumPitchAngularDelta,
            config_.maximumPitchAngularDelta);
        output.angularVelocityDeltaLocal.z = authority * Clamp(
            FiniteOr(rollDelta, 0.0f),
            -config_.maximumRollAngularDelta,
            config_.maximumRollAngularDelta);

        if (config_.enableYawVelocityAssist) {
            const float yawDelta = config_.yawVelocityKp *
                                   (output.targetYawRateRadS - output.yawRateRadS) * dt;
            output.angularVelocityDeltaLocal.y = authority * Clamp(
                FiniteOr(yawDelta, 0.0f),
                -config_.maximumYawAngularDelta,
                config_.maximumYawAngularDelta);
        }
    }

    // Legacy activation modes remain unable to write body yaw. The narrowly
    // scoped manual HandbrakeHold path returns before reaching this block.
    output.angularVelocityDeltaLocal.y = 0.0f;
    output.targetYawRateRadS = 0.0f;
    output.bodyYawOffsetTargetRad = 0.0f;
    output.bodyYawRateTargetRadS = 0.0f;
    output.bodyYawAccelerationLimitRadS2 = 0.0f;
    output.bodyYawVelocityDeltaLimitRadS = 0.0f;
    output.entryYawBoostBlend = 0.0f;
    output.yawResponseMode = YawResponseMode::None;
    output.acquiringMinimumDriftAngle = false;
    output.bodyOffsetBrakeActive = false;
    output.manualYawBodyOffsetLimited = false;

    previousSideslip_ = output.sideslipAngleRad;
    initialized_ = true;
    return output;
}

} // namespace nfsmw_drift
