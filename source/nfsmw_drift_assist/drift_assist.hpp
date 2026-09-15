#pragma once

#include <array>
#include <cstdint>

namespace nfsmw_drift {

constexpr float kPi = 3.14159265358979323846f;
constexpr float DegToRad(float degrees) { return degrees * (kPi / 180.0f); }

constexpr std::uint8_t kNegativeSteeringDirectionObserved = 1u << 0;
constexpr std::uint8_t kPositiveSteeringDirectionObserved = 1u << 1;
constexpr std::uint8_t SteeringDirectionObservationMask(int direction) {
    return direction < 0 ? kNegativeSteeringDirectionObserved
                         : (direction > 0
                                ? kPositiveSteeringDirectionObserved
                                : 0u);
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 operator+(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 v);
Vec3 operator*(Vec3 v, float scalar);
Vec3 operator*(float scalar, Vec3 v);
Vec3 operator/(Vec3 v, float scalar);
Vec3& operator+=(Vec3& a, Vec3 b);
Vec3& operator-=(Vec3& a, Vec3 b);
Vec3& operator*=(Vec3& a, float scalar);

float Dot(Vec3 a, Vec3 b);
Vec3 Cross(Vec3 a, Vec3 b);
float Length(Vec3 v);
Vec3 NormalizeOr(Vec3 v, Vec3 fallback);
float Clamp(float value, float low, float high);
float WrapPi(float angle);

struct Basis {
    // The game uses right (+X), up (+Y), forward (+Z) body axes.
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 forward{0.0f, 0.0f, 1.0f};
};

enum class ActivationMode : std::uint8_t {
    // Latch the mode after handbrakeInput stays above the configured threshold.
    HandbrakeHold,
    Manual,
    Automatic,
    ManualOrAutomatic,
};

enum class ActuationMode : std::uint8_t {
    SteeringOnly,
    SteeringAndAttitude,
    AttitudeOnly,
};

enum class DriftPhase : std::uint8_t {
    Off,
    Charging,
    WaitingDirection,
    Entering,
    Holding,
    SideTransition,
    Centering,
    Exiting,
};

enum class YawResponseMode : std::uint8_t {
    None,
    // Manual steering may add yaw only toward the already locked drift side.
    ManualSameDirectionAssist,
    // Manual opposite steering actively rotates the body back toward zero
    // offset, with stopping-distance limiting near the center line.
    ManualOppositeRecovery,
    // Only add yaw in the driver's requested direction. Natural yaw that is
    // already faster than the target is left untouched.
    AssistOnly,
    EntryAssistOnly,
    // Closed-loop velocity-relative acquisition of the minimum drift angle.
    // Unlike EntryAssistOnly, this tracks path yaw and may brake overshoot.
    AcquireBase15,
    // Closed-loop velocity-relative drift-angle control. AngleHold is valid
    // only while the driver's steering is neutral; SameDirection is valid
    // only while steering remains on the locked drift side.
    AngleHold,
    SameDirection,
    // Bidirectional authority used to reverse an established yaw motion.
    DirectionChange,
    Centering,
};

enum class SteeringAssistMode : std::uint8_t {
    None,
    WaitingForDrift,
    SmartCountersteer,
    // Retained for source/log compatibility; the controller no longer emits
    // a delayed countersteer handoff state.
    CountersteerHandoffDelay,
    ManualOverride,
    ReengageDelay,
};

struct VehicleState {
    float dt = 0.0f;
    // Optional validated real physics-frame duration for steering ownership
    // timers. Hosts that clamp dt for bounded yaw control can provide the
    // unclamped cadence here; zero falls back to dt.
    float steeringOwnershipDt = 0.0f;
    Basis body{};
    Vec3 linearVelocityWorld{};
    Vec3 angularVelocityWorld{};
    Vec3 surfaceNormal{0.0f, 1.0f, 0.0f};
    bool hasSurfaceNormal = false;

    // If speedMps is zero or negative, the controller derives speed from velocity.
    float speedMps = 0.0f;
    float steeringInput = 0.0f;
    // Set when a later input poll in the previous physics serial observed
    // manual steering, even if the current sampled input has returned neutral.
    bool manualSteeringInputObserved = false;
    // Preserves the sign of every manual input seen in that serial. Bit 0 is
    // left/negative and bit 1 is right/positive. A host that cannot provide
    // this detail may leave it zero and use manualSteeringInputObserved alone.
    std::uint8_t manualSteeringDirectionMaskObserved = 0;
    float throttleInput = 0.0f;
    float brakeInput = 0.0f;
    float handbrakeInput = 0.0f;
    // Canonical controller order: front-left (FL), front-right (FR),
    // rear-left (RL), rear-right (RR).  An SDK adapter must translate its
    // native wheel enum into this order before filling these arrays.
    std::array<float, 4> wheelSlip{};
    std::array<float, 4> wheelLoad{};
    std::uint8_t groundedWheels = 0;

    // The host can use this to map a custom hotkey or the game's HANDBRAKE action.
    bool assistRequested = false;
    bool playerControlled = true;
    bool inRace = true;
    bool resetState = false;
    bool collisionRecent = false;

    // Set by the host only when this exact sample has a validated path for
    // writing the attitude correction before the physics integrator.  The
    // controller uses it to decide whether short-term body-offset prediction
    // is safe; false keeps centering tied to measured rigid-body read-back.
    bool attitudeWriteAvailable = false;
};

struct AssistConfig {
    bool enabled = true;
    ActivationMode activation = ActivationMode::HandbrakeHold;
    ActuationMode actuation = ActuationMode::SteeringAndAttitude;
    bool requirePlayer = true;
    bool allowReverse = false;
    bool enableYawVelocityAssist = false;
    bool enableThrottleProtection = true;
    // HandbrakeHold can use the body heading relative to the actual direction
    // of travel instead of a heading captured in world space. This prevents a
    // long corner from consuming the entire body-offset allowance.
    bool useVelocityRelativeDriftAngle = false;

    float minSpeedMps = 10.0f;
    // Used to reject near-sideways/near-stopped activation and legacy
    // automatic modes. HandbrakeHold keeps using total speed after latching.
    float minLongitudinalSpeedMps = 7.0f;
    std::uint8_t minGroundedWheels = 2;
    // Once a handbrake drift session is active, tolerate transient suspension
    // contact loss for this long before ending the session. Activation still
    // requires minGroundedWheels immediately.
    float groundContactLossGraceSeconds = 3.0f;
    float enterSideslipRad = DegToRad(8.0f);
    float exitSideslipRad = DegToRad(4.0f);
    float enterRearSlip = 0.12f;
    float exitRearSlip = 0.07f;
    float enterHandbrake = 0.25f;

    // Dedicated handbrake-session settings. Reaching the input threshold arms
    // immediately; the duration field is retained only for INI/source
    // compatibility with older profiles.
    float handbrakeActivationHoldSeconds = 0.35f;
    float handbrakeActivationThreshold = 0.50f;
    float directionDeadzone = 0.15f;
    bool enableSmartCountersteer = true;
    float countersteerActivationBodyOffsetRad = DegToRad(15.0f);
    float smartCountersteerAngleRad = DegToRad(55.0f);
    float smartCountersteerReengageDelaySeconds = 0.20f;
    // Legacy compatibility key. The accepted 0.9.5 path gives every valid
    // manual direction immediate ownership, so this value is normalized to
    // zero by the controller and cannot introduce a same-side dwell.
    float smartCountersteerHandoffDelaySeconds = 0.0f;
    // Legacy compatibility key. The live 0.9.5 manual path uses a fixed
    // 2.0 command/s rate, so travel time follows the remaining distance.
    float manualSteeringTransitionSeconds = 1.375f;
    // Manual yaw assist is active only while the driver is currently steering.
    // Neutral and automatic-countersteer frames never request body yaw.
    bool enableManualYawAssist = true;
    float manualSameDirectionTargetYawRateRadS = 0.5625f;
    float manualSameDirectionYawAccelerationRadS2 = 3.0f;
    float manualSameDirectionMaximumYawAngularDelta = 0.15f;
    // Same-direction yaw authority grows linearly from the initial strength
    // to full authority while the same input is held continuously.
    float manualSameDirectionRampSeconds = 2.0f;
    float manualSameDirectionInitialStrength = 0.15f;
    float manualOppositeMaximumRecoveryYawRateRadS = 0.45f;
    float manualOppositeYawDampingAccelerationRadS2 = 4.5f;
    float manualOppositeMaximumYawAngularDelta = 0.15f;
    float manualYawHardBodyOffsetRad = DegToRad(35.0f);
    // After an established drift crosses the center line under deliberate
    // opposite steering, suspend all automatic writes while waiting for the
    // new side to become real. A confirmed pendulum keeps the session; an
    // unconfirmed one exits it cleanly.
    float pendulumTransitionZeroBandRad = DegToRad(2.0f);
    float pendulumTransitionConfirmBodyOffsetRad = DegToRad(6.0f);
    float pendulumTransitionWindowSeconds = 0.50f;
    // After the first direction lock, force full steering into the drift
    // before blending to the dynamically calculated countersteer command.
    float sameDirectionFullSteerSeconds = 0.25f;
    float countersteerTransitionSeconds = 0.20f;
    float minimumCountersteerAngleRad = DegToRad(10.0f);
    float maximumCountersteerAngleRad = DegToRad(35.0f);
    float exitBodyOffsetRad = DegToRad(5.0f);
    float noDirectionTimeoutSeconds = 2.0f;
    // Minimum velocity-relative drift angle held after a side is locked.
    float minimumDriftBodyOffsetRad = DegToRad(15.0f);
    float bodyOffsetHoldSpeedRadS = 0.40f;
    float bodyOffsetHoldYawAccelerationRadS2 = 4.0f;
    float bodyOffsetHoldMaximumYawAngularDelta = 0.10f;
    float bodyOffsetHoldSlowdownOffsetRad = DegToRad(10.0f);
    // Short high-response envelope used only while acquiring the initial
    // velocity-relative drift angle. The angle target remains fixed at
    // minimumDriftBodyOffsetRad throughout this envelope.
    float driftAngleEntrySeconds = 0.18f;
    float driftAngleEntrySpeedRadS = 0.90f;
    float driftAngleEntryYawAccelerationRadS2 = 9.0f;
    float driftAngleEntryOffsetCutoffRad = DegToRad(12.0f);
    float driftAngleEntryMaximumYawAngularDelta = 0.20f;
    float bodyRotationSpeedRadS = DegToRad(90.0f);
    float bodyRotationSlowdownOffsetRad = DegToRad(10.0f);
    // One-shot yaw-rate target after the first drift direction is locked.
    // It is assist-only: natural same-direction yaw above this target is never
    // braked. A target of zero disables it.
    float entryYawBoostSeconds = 0.18f;
    float entryYawBoostRateRadS = 0.0f;
    float entryYawBoostAccelerationRadS2 = 4.0f;
    float entryYawBoostOffsetCutoffRad = DegToRad(12.0f);
    float entryYawBoostMaximumYawAngularDelta = 0.20f;
    // Limits how quickly the requested handbrake-mode yaw velocity can change.
    // This keeps steering/yaw behavior consistent across different frame rates.
    float bodyYawAccelerationRadS2 = 3.0f;
    // Reversing the body uses an independent, faster response envelope.
    float directionChangeSpeedRadS = 0.38f;
    float directionChangeYawAccelerationRadS2 = 6.0f;
    float directionChangeMaximumYawAngularDelta = 0.20f;
    float directionChangeSlowdownOffsetRad = DegToRad(15.0f);
    // Zero means use bodyRotationSpeedRadS while centering.
    float autoCenterSpeedRadS = 0.0f;
    float autoCenterYawAccelerationRadS2 = 6.0f;
    float autoCenterMaximumYawAngularDelta = 0.20f;
    // Centering reduces its target rate linearly inside this offset.
    float autoCenterSlowdownOffsetRad = DegToRad(12.0f);
    // Velocity-relative recovery exits only after both angle and relative
    // angular motion have remained settled for this duration.
    float exitDriftAngleRateRadS = DegToRad(2.0f);
    float exitSettleSeconds = 0.10f;
    // Filters the inferred travel-direction yaw rate used as feed-forward.
    float driftAngleRateFilterSeconds = 0.10f;
    // Safety limit for the stored body yaw offset; independent of the 35 deg
    // countersteer cap.
    float maximumBodyOffsetRad = DegToRad(90.0f);
    // Multipliers: 1 = unchanged, <1 = weaken, >1 = strengthen.
    float frontGripMultiplierDuringDrift = 1.0f;
    float rearDriveMultiplierDuringDrift = 1.0f;

    float targetSideslipRad = DegToRad(11.0f);
    float maximumSideslipRad = DegToRad(35.0f);
    float wheelbaseM = 2.65f;
    float maximumSteerAngleRad = DegToRad(60.0f);
    float yawReferenceScale = 0.80f;
    float maximumYawRateRadS = 2.8f;

    float sideslipKp = 1.25f;
    float sideslipKd = 0.10f;
    float yawRateKp = 0.28f;
    float yawRateDamping = 0.08f;
    float maximumSteeringCorrection = 0.24f;
    float maximumSteeringCorrectionRate = 3.5f;
    float steeringAuthority = 0.80f;

    float blendInSeconds = 0.12f;
    float blendOutSeconds = 0.22f;
    float collisionAuthority = 0.25f;

    float rollKp = 4.0f;
    float rollKd = 1.8f;
    float pitchKp = 2.8f;
    float pitchKd = 1.4f;
    float yawVelocityKp = 0.40f;
    float attitudeAuthority = 0.65f;
    float airborneAuthority = 0.0f;
    float maximumRollAngularDelta = 1.20f;
    float maximumPitchAngularDelta = 0.80f;
    float maximumYawAngularDelta = 0.60f;

    float throttleCutAtMaximumSlip = 0.16f;
    float recoveryBrakeAtMaximumSlip = 0.10f;
    float recoverySideslipRad = DegToRad(28.0f);
};

struct ManualYawCommand {
    YawResponseMode mode = YawResponseMode::None;
    float targetYawRateRadS = 0.0f;
    float accelerationLimitRadS2 = 0.0f;
    float velocityDeltaLimitRadS = 0.0f;
    float yawVelocityDeltaRadS = 0.0f;
    bool bodyOffsetLimited = false;
    bool dampingActive = false;
};

// Stateless so the input hook and the post-physics sink can apply exactly the
// same rules using their respective latest body samples. The caller remains
// responsible for validating the active session, vehicle identity, motion,
// contact, and collision gates before using a non-zero command.
ManualYawCommand ComputeManualYawCommand(const AssistConfig& config,
                                         float bodyYawOffsetRad,
                                         float bodyYawOffsetRateRadS,
                                         float rawSteering,
                                         int driftSide,
                                         float dt,
                                         float manualYawStrength = 1.0f);

struct ControlOutput {
    bool active = false;
    DriftPhase phase = DriftPhase::Off;
    float blend = 0.0f;
    int driftSide = 0; // -1 = left, +1 = right in the vehicle's local frame.

    float localLongitudinalSpeed = 0.0f;
    float localLateralSpeed = 0.0f;
    float sideslipAngleRad = 0.0f;
    float sideslipRateRadS = 0.0f;
    float yawRateRadS = 0.0f;
    float targetSideslipRad = 0.0f;
    float targetYawRateRadS = 0.0f;

    // Dedicated handbrake-session telemetry. bodyYawOffsetRad is the signed
    // body heading relative to the direction of travel. Negative = left,
    // positive = right. Only current manual input can populate the yaw command
    // fields; neutral and automatic-countersteer frames leave them at zero.
    float bodyYawOffsetRad = 0.0f;
    // Unclamped measured/predicted offset for host-side emergency limits.
    float bodyYawOffsetSafetyRad = 0.0f;
    float bodyYawOffsetTargetRad = 0.0f;
    float bodyYawOffsetRateRadS = 0.0f;
    float bodyYawRateTargetRadS = 0.0f;
    float bodyYawAccelerationLimitRadS2 = 0.0f;
    float bodyYawVelocityDeltaLimitRadS = 0.0f;
    float manualYawStrength = 0.0f;
    int manualYawControlSide = 0;
    float entryYawBoostBlend = 0.0f;
    YawResponseMode yawResponseMode = YawResponseMode::None;
    bool acquiringMinimumDriftAngle = false;
    bool bodyOffsetBrakeActive = false;
    bool manualYawBodyOffsetLimited = false;
    float countersteerAngleRad = 0.0f;
    // Final dynamic countersteer target. steeringCommand is the actual command
    // for this frame and may still be in the same-direction entry sequence.
    float countersteerCommand = 0.0f;
    float smartCountersteerCommand = 0.0f;
    float steeringNeutralSeconds = 0.0f;
    // Retained as zero-valued compatibility telemetry; no handoff dwell exists.
    float countersteerHandoffSeconds = 0.0f;
    float handbrakeHoldSeconds = 0.0f;
    float handbrakeHoldProgress = 0.0f;
    float groundContactLossSeconds = 0.0f;
    bool groundContactGraceActive = false;
    float frontGripScale = 1.0f;
    float rearDriveScale = 1.0f;
    // When true, the host must override steering with steeringCommand.
    bool forceCountersteer = false;
    bool smartCountersteerActive = false;
    bool manualSteeringOverride = false;
    bool countersteerHandoffPending = false;
    bool sideTransitionActive = false;
    int sideTransitionTargetSide = 0;
    float sideTransitionSeconds = 0.0f;
    SteeringAssistMode steeringAssistMode = SteeringAssistMode::None;
    bool centering = false;

    float steeringDelta = 0.0f;
    float steeringCommand = 0.0f;
    float throttleScale = 1.0f;
    float brakeAdd = 0.0f;

    // Add this vector to the body's local angular velocity once for this frame.
    // Units are rad/s. The host should apply it only after validating its layout.
    Vec3 angularVelocityDeltaLocal{};
    Vec3 attitudeErrorLocal{}; // x = pitch, z = roll; useful for telemetry.
};

class DriftAssistController {
public:
    explicit DriftAssistController(AssistConfig config = {});

    void setConfig(const AssistConfig& config);
    const AssistConfig& config() const { return config_; }
    void reset();
    // Tell the controller that a host-side pending yaw write was revoked
    // before physics consumed it, so delayed read-back prediction skips it.
    void notifyAttitudeWriteSkipped();
    // Replace the controller's pre-host estimate with the exact bounded delta
    // that the host plans to apply.
    void notifyAttitudeWritePlanned(float yawDeltaRadS);

    ControlOutput update(const VehicleState& state);

    DriftPhase phase() const { return phase_; }
    float blend() const { return blend_; }
    int driftSide() const { return driftSide_; }

private:
    AssistConfig config_{};
    DriftPhase phase_ = DriftPhase::Off;
    float blend_ = 0.0f;
    int driftSide_ = 0;
    float previousSideslip_ = 0.0f;
    float previousSteeringDelta_ = 0.0f;
    bool initialized_ = false;

    // State for the explicit handbrake-hold mode.
    float noDirectionSeconds_ = 0.0f;
    bool handbrakeModeActive_ = false;
    bool handbrakeRearmRequired_ = false;
    float groundContactLossSeconds_ = 0.0f;
    bool hasMeasuredBodyOffset_ = false;
    float previousMeasuredBodyOffsetRad_ = 0.0f;
    float exitSettleSeconds_ = 0.0f;
    float steeringNeutralSeconds_ = 0.0f;
    float countersteerHandoffSeconds_ = 0.0f;
    bool manualSteeringSuppressed_ = false;
    bool smartCountersteerThresholdReached_ = false;
    bool smartCountersteerOwnsSteering_ = false;
    float manualSameDirectionSeconds_ = 0.0f;
    int manualSameDirectionSide_ = 0;
    bool sideTransitionActive_ = false;
    int sideTransitionTargetSide_ = 0;
    float sideTransitionSeconds_ = 0.0f;

    void updatePhase(bool candidate, float dt);
    bool activationRequested(const VehicleState& state, float sideslip, float rearSlip) const;
    void chooseDriftSide(float sideslip, float steering, float yawRate);
    ControlOutput updateHandbrakeMode(const VehicleState& state,
                                      float dt,
                                      bool activationMotionEligible,
                                      bool activeMotionEligible,
                                      bool contactEligible,
                                      ControlOutput output);
    void resetHandbrakeState();
};

} // namespace nfsmw_drift
