#include "drift_assist.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>

namespace {

using nfsmw_drift::ActuationMode;
using nfsmw_drift::ActivationMode;
using nfsmw_drift::AssistConfig;
using nfsmw_drift::ControlOutput;
using nfsmw_drift::DegToRad;
using nfsmw_drift::DriftAssistController;
using nfsmw_drift::DriftPhase;
using nfsmw_drift::kPositiveSteeringDirectionObserved;
using nfsmw_drift::SteeringAssistMode;
using nfsmw_drift::VehicleState;
using nfsmw_drift::YawResponseMode;

int failures = 0;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool Finite(float value) {
    return std::isfinite(value) != 0;
}

bool Near(float actual, float expected, float tolerance = 1.0e-4f) {
    return std::fabs(actual - expected) <= tolerance;
}

VehicleState MovingState() {
    VehicleState state{};
    state.dt = 1.0f / 60.0f;
    state.linearVelocityWorld = {0.0f, 0.0f, 20.0f};
    state.groundedWheels = 4;
    state.wheelLoad = {1.0f, 1.0f, 1.0f, 1.0f};
    state.hasSurfaceNormal = true;
    state.surfaceNormal = {0.0f, 1.0f, 0.0f};
    state.attitudeWriteAvailable = true;
    return state;
}

AssistConfig ImmediateConfig() {
    AssistConfig config{};
    config.blendInSeconds = 0.0f;
    config.blendOutSeconds = 0.0f;
    config.maximumSteeringCorrectionRate = 100.0f;
    return config;
}

void TestLowSpeedDoesNotActivate() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Automatic;
    DriftAssistController controller(config);

    VehicleState state = MovingState();
    state.linearVelocityWorld.z = 5.0f;
    state.handbrakeInput = 1.0f;
    const ControlOutput output = controller.update(state);

    Require(!output.active, "low speed must not activate the assist");
    Require(output.phase == DriftPhase::Off, "low speed must leave the phase Off");
}

void TestHandbrakeEntry() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Automatic;
    DriftAssistController controller(config);

    VehicleState state = MovingState();
    state.handbrakeInput = 1.0f;
    state.steeringInput = -0.30f;
    const ControlOutput output = controller.update(state);

    Require(output.active, "handbrake above its threshold must enter drift assist");
    Require(output.phase == DriftPhase::Holding, "zero blend-in time must enter Holding immediately");
    Require(output.driftSide == 1, "left turn input should infer rightward local sideslip");
}

void TestSideslipEntry() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Automatic;
    DriftAssistController controller(config);

    VehicleState state = MovingState();
    state.linearVelocityWorld.x = std::tan(DegToRad(10.0f)) * state.linearVelocityWorld.z;
    const ControlOutput output = controller.update(state);

    Require(output.active, "sideslip above the entry threshold must activate");
    Require(output.driftSide == 1, "positive local lateral speed must select the right drift side");
    Require(output.sideslipAngleRad > config.enterSideslipRad,
            "reported sideslip must reflect the body-local velocity");
}

void TestSideslipHysteresis() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Automatic;
    DriftAssistController controller(config);
    VehicleState state = MovingState();

    state.linearVelocityWorld.x = std::tan(DegToRad(10.0f)) * state.linearVelocityWorld.z;
    Require(controller.update(state).active, "hysteresis setup must enter above the entry threshold");

    state.linearVelocityWorld.x = std::tan(DegToRad(6.0f)) * state.linearVelocityWorld.z;
    Require(controller.update(state).active,
            "assist must remain active between entry and exit sideslip thresholds");

    state.linearVelocityWorld.x = std::tan(DegToRad(2.0f)) * state.linearVelocityWorld.z;
    const ControlOutput output = controller.update(state);
    Require(!output.active, "assist must exit below all exit thresholds");
    Require(output.phase == DriftPhase::Off, "zero blend-out time must finish at Off immediately");
}

void TestRearSlipUsesWheelLoad() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Automatic;
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    state.wheelSlip = {0.0f, 0.0f, 0.50f, 0.50f};
    state.wheelLoad = {1.0f, 1.0f, 0.0f, 0.0f};

    Require(!controller.update(state).active,
            "unloaded rear-wheel spin must not activate drift assist");

    state.wheelLoad[2] = 1.0f;
    state.wheelLoad[3] = 1.0f;
    Require(controller.update(state).active,
            "loaded rear-wheel slip above threshold must activate drift assist");
}

void TestExtremeFiniteWheelLoadsRemainStable() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Automatic;
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    const float largestFinite = std::numeric_limits<float>::max();
    state.wheelLoad = {1.0f, 1.0f, largestFinite, largestFinite};
    state.wheelSlip = {0.0f, 0.0f, 1.0f, 1.0f};

    const ControlOutput output = controller.update(state);
    Require(output.active,
            "large but finite rear loads must still allow a valid slip trigger");
    Require(Finite(output.steeringCommand) && Finite(output.targetYawRateRadS),
            "large finite wheel loads must not contaminate control outputs");
}

void TestDriftSideReversal() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Automatic;
    DriftAssistController controller(config);
    VehicleState state = MovingState();

    state.linearVelocityWorld.x = std::tan(DegToRad(12.0f)) * state.linearVelocityWorld.z;
    Require(controller.update(state).driftSide == 1,
            "positive sideslip must establish the positive drift side");

    state.linearVelocityWorld.x = -std::tan(DegToRad(12.0f)) * state.linearVelocityWorld.z;
    Require(controller.update(state).driftSide == -1,
            "strong opposite sideslip must replace the stale drift side");
}

void TestExcessYawProducesCountersteer() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Automatic;
    config.steeringAuthority = 1.0f;
    DriftAssistController controller(config);

    VehicleState state = MovingState();
    state.handbrakeInput = 1.0f;
    state.angularVelocityWorld.y = 3.0f;
    const ControlOutput output = controller.update(state);

    Require(output.steeringDelta < 0.0f,
            "excess positive yaw must request steering in the opposing direction");
    Require(std::fabs(output.steeringDelta) <= config.maximumSteeringCorrection + 1.0e-6f,
            "countersteer must respect its configured amplitude limit");
}

void TestAttitudeRecoveryDirections() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Manual;
    config.actuation = ActuationMode::AttitudeOnly;
    config.attitudeAuthority = 1.0f;

    constexpr float angle = 0.20f;
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);

    DriftAssistController rollController(config);
    VehicleState rollState = MovingState();
    rollState.assistRequested = true;
    rollState.body.right = {cosine, -sine, 0.0f};
    rollState.body.up = {sine, cosine, 0.0f};
    rollState.body.forward = {0.0f, 0.0f, 1.0f};
    const ControlOutput rollOutput = rollController.update(rollState);
    Require(rollOutput.attitudeErrorLocal.z > 0.0f,
            "right-leaning body must report positive local roll recovery error");
    Require(rollOutput.angularVelocityDeltaLocal.z > 0.0f,
            "right-leaning body must receive a restoring roll angular delta");

    DriftAssistController pitchController(config);
    VehicleState pitchState = MovingState();
    pitchState.assistRequested = true;
    pitchState.body.right = {1.0f, 0.0f, 0.0f};
    pitchState.body.up = {0.0f, cosine, sine};
    pitchState.body.forward = {0.0f, -sine, cosine};
    const ControlOutput pitchOutput = pitchController.update(pitchState);
    Require(pitchOutput.attitudeErrorLocal.x < 0.0f,
            "nose-up body must report negative local pitch recovery error");
    Require(pitchOutput.angularVelocityDeltaLocal.x < 0.0f,
            "nose-up body must receive a restoring pitch angular delta");
}

void TestRecoveryAndAirborneAuthority() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Manual;
    config.actuation = ActuationMode::SteeringAndAttitude;
    config.attitudeAuthority = 1.0f;
    config.airborneAuthority = 0.0f;
    config.blendOutSeconds = 0.20f;
    config.recoverySideslipRad = DegToRad(10.0f);
    config.maximumSideslipRad = DegToRad(20.0f);
    config.throttleCutAtMaximumSlip = 0.40f;
    config.recoveryBrakeAtMaximumSlip = 0.20f;
    DriftAssistController controller(config);

    VehicleState state = MovingState();
    state.assistRequested = true;
    state.throttleInput = 1.0f;
    state.linearVelocityWorld.x = std::tan(DegToRad(30.0f)) * state.linearVelocityWorld.z;
    state.body.right = {std::cos(0.2f), -std::sin(0.2f), 0.0f};
    state.body.up = {std::sin(0.2f), std::cos(0.2f), 0.0f};

    const ControlOutput grounded = controller.update(state);
    Require(grounded.throttleScale < 1.0f,
            "extreme sideslip must reduce throttle when recovery protection is enabled");
    Require(grounded.brakeAdd > 0.0f,
            "extreme sideslip must request bounded recovery braking");
    Require(std::fabs(grounded.angularVelocityDeltaLocal.z) > 0.0f,
            "a grounded tilted body must receive roll attitude assistance");

    state.groundedWheels = 0;
    const ControlOutput airborne = controller.update(state);
    Require(airborne.phase == DriftPhase::Exiting,
            "loss of contact must move an active assist into its exit phase");
    Require(std::fabs(airborne.angularVelocityDeltaLocal.x) <= 1.0e-7f &&
                std::fabs(airborne.angularVelocityDeltaLocal.y) <= 1.0e-7f &&
                std::fabs(airborne.angularVelocityDeltaLocal.z) <= 1.0e-7f,
            "zero airborne authority must suppress every attitude correction");
}

void TestLimitsAndNonFiniteInputs() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Manual;
    config.steeringAuthority = 1.0f;
    config.attitudeAuthority = 1.0f;
    config.enableYawVelocityAssist = true;
    config.maximumSteeringCorrection = 0.10f;
    config.maximumSteeringCorrectionRate = 0.50f;
    config.maximumPitchAngularDelta = 0.02f;
    config.maximumRollAngularDelta = 0.03f;
    config.maximumYawAngularDelta = 0.015f;
    config.sideslipKp = 100.0f;
    config.yawRateKp = 100.0f;
    config.rollKp = 100.0f;
    config.pitchKp = 100.0f;
    config.yawVelocityKp = 100.0f;
    DriftAssistController controller(config);

    VehicleState state = MovingState();
    state.dt = 0.02f;
    state.assistRequested = true;
    state.linearVelocityWorld.x = 20.0f;
    state.angularVelocityWorld = {50.0f, 50.0f, 50.0f};
    state.body.right = {std::cos(0.2f), -std::sin(0.2f), 0.0f};
    state.body.up = {std::sin(0.2f), std::cos(0.2f), 0.0f};
    const ControlOutput limited = controller.update(state);

    Require(std::fabs(limited.steeringDelta) <= 0.01001f,
            "steering correction must respect its per-second rate limit");
    Require(std::fabs(limited.angularVelocityDeltaLocal.x) <=
                config.maximumPitchAngularDelta + 1.0e-6f,
            "pitch delta must respect its amplitude limit");
    Require(std::fabs(limited.angularVelocityDeltaLocal.y) <=
                config.maximumYawAngularDelta + 1.0e-6f,
            "yaw delta must respect its amplitude limit");
    Require(std::fabs(limited.angularVelocityDeltaLocal.z) <=
                config.maximumRollAngularDelta + 1.0e-6f,
            "roll delta must respect its amplitude limit");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    AssistConfig invalidConfig{};
    invalidConfig.sideslipKp = nan;
    invalidConfig.rollKd = infinity;
    invalidConfig.maximumSteeringCorrection = nan;
    DriftAssistController robustController(invalidConfig);
    VehicleState invalid{};
    invalid.dt = nan;
    invalid.speedMps = infinity;
    invalid.steeringInput = nan;
    invalid.linearVelocityWorld = {infinity, nan, -infinity};
    invalid.angularVelocityWorld = {nan, infinity, -infinity};
    invalid.body.right = {nan, nan, nan};
    invalid.body.up = {infinity, nan, 0.0f};
    invalid.body.forward = {nan, 0.0f, infinity};
    invalid.hasSurfaceNormal = true;
    invalid.surfaceNormal = {nan, infinity, nan};
    invalid.groundedWheels = 255;
    invalid.assistRequested = true;
    const ControlOutput robust = robustController.update(invalid);

    Require(Finite(robust.localLongitudinalSpeed), "longitudinal speed must stay finite");
    Require(Finite(robust.localLateralSpeed), "lateral speed must stay finite");
    Require(Finite(robust.sideslipAngleRad), "sideslip angle must stay finite");
    Require(Finite(robust.sideslipRateRadS), "sideslip rate must stay finite");
    Require(Finite(robust.yawRateRadS), "yaw rate must stay finite");
    Require(Finite(robust.steeringDelta) && Finite(robust.steeringCommand),
            "steering outputs must stay finite");
    Require(Finite(robust.throttleScale) && Finite(robust.brakeAdd),
            "pedal outputs must stay finite");
    Require(Finite(robust.angularVelocityDeltaLocal.x) &&
                Finite(robust.angularVelocityDeltaLocal.y) &&
                Finite(robust.angularVelocityDeltaLocal.z),
            "angular outputs must stay finite");
}

void TestDeterministicStress() {
    AssistConfig config{};
    config.maximumSteeringCorrection = 0.30f;
    config.maximumPitchAngularDelta = 0.70f;
    config.maximumRollAngularDelta = 0.90f;
    config.maximumYawAngularDelta = 0.50f;
    config.enableYawVelocityAssist = true;
    DriftAssistController controller(config);

    std::mt19937 generator(0x4D573035U);
    std::uniform_real_distribution<float> value(-100.0f, 100.0f);
    std::uniform_real_distribution<float> input(-2.0f, 2.0f);
    bool valid = true;

    for (int frame = 0; frame < 2000; ++frame) {
        VehicleState state{};
        state.dt = (frame % 97 == 0) ? std::numeric_limits<float>::quiet_NaN()
                                     : std::fabs(value(generator)) * 0.01f;
        state.body.right = {value(generator), value(generator), value(generator)};
        state.body.up = {value(generator), value(generator), value(generator)};
        state.body.forward = {value(generator), value(generator), value(generator)};
        state.linearVelocityWorld = {value(generator), value(generator), value(generator)};
        state.angularVelocityWorld = {value(generator), value(generator), value(generator)};
        state.surfaceNormal = {value(generator), value(generator), value(generator)};
        state.hasSurfaceNormal = (frame & 1) != 0;
        state.speedMps = (frame % 89 == 0) ? std::numeric_limits<float>::infinity()
                                          : std::fabs(value(generator));
        state.steeringInput = input(generator);
        state.throttleInput = input(generator);
        state.brakeInput = input(generator);
        state.handbrakeInput = input(generator);
        state.groundedWheels = static_cast<std::uint8_t>(frame % 7);
        state.wheelSlip = {value(generator), value(generator), value(generator), value(generator)};
        state.wheelLoad = {value(generator), value(generator), value(generator), value(generator)};
        state.assistRequested = (frame % 3) == 0;
        state.playerControlled = (frame % 31) != 0;
        state.inRace = (frame % 43) != 0;
        state.resetState = (frame % 127) == 0;
        state.collisionRecent = (frame % 11) == 0;

        const ControlOutput output = controller.update(state);
        valid = valid &&
                Finite(output.blend) && output.blend >= 0.0f && output.blend <= 1.0f &&
                Finite(output.sideslipAngleRad) && Finite(output.sideslipRateRadS) &&
                Finite(output.yawRateRadS) && Finite(output.targetYawRateRadS) &&
                Finite(output.steeringCommand) && output.steeringCommand >= -1.0f &&
                output.steeringCommand <= 1.0f && Finite(output.steeringDelta) &&
                (output.forceCountersteer
                     ? std::fabs(output.steeringDelta) <= 2.0f
                     : std::fabs(output.steeringDelta) <=
                           config.maximumSteeringCorrection + 1.0e-5f) &&
                Finite(output.throttleScale) && output.throttleScale >= 0.0f &&
                output.throttleScale <= 1.0f && Finite(output.brakeAdd) &&
                output.brakeAdd >= 0.0f && output.brakeAdd <= 1.0f &&
                Finite(output.angularVelocityDeltaLocal.x) &&
                std::fabs(output.angularVelocityDeltaLocal.x) <=
                    config.maximumPitchAngularDelta + 1.0e-5f &&
                Finite(output.angularVelocityDeltaLocal.y) &&
                std::fabs(output.angularVelocityDeltaLocal.y) <=
                    config.maximumYawAngularDelta + 1.0e-5f &&
                Finite(output.angularVelocityDeltaLocal.z) &&
                std::fabs(output.angularVelocityDeltaLocal.z) <=
                    config.maximumRollAngularDelta + 1.0e-5f;
    }

    Require(valid, "deterministic stress inputs must always produce finite bounded outputs");
}

#if 0
AssistConfig VelocityRelativeEntryConfig(float dt) {
    AssistConfig config{};
    config.activation = ActivationMode::HandbrakeHold;
    config.actuation = ActuationMode::AttitudeOnly;
    config.useVelocityRelativeDriftAngle = true;
    config.minSpeedMps = 8.0f;
    config.minLongitudinalSpeedMps = 6.0f;
    config.minGroundedWheels = 2;
    config.handbrakeActivationHoldSeconds = 0.0f;
    config.handbrakeActivationThreshold = 0.5f;
    config.noDirectionTimeoutSeconds = 10.0f;
    config.minimumDriftBodyOffsetRad = DegToRad(15.0f);
    config.bodyOffsetHoldSpeedRadS = 0.40f;
    config.bodyOffsetHoldYawAccelerationRadS2 = 4.0f;
    config.bodyOffsetHoldMaximumYawAngularDelta = 0.10f;
    config.bodyOffsetHoldSlowdownOffsetRad = DegToRad(10.0f);
    config.driftAngleEntrySeconds = 0.18f;
    config.driftAngleEntrySpeedRadS = 0.90f;
    config.driftAngleEntryYawAccelerationRadS2 = 9.0f;
    config.driftAngleEntryOffsetCutoffRad = DegToRad(12.0f);
    config.driftAngleEntryMaximumYawAngularDelta = 0.20f;
    config.bodyRotationSpeedRadS = 0.75f;
    config.bodyYawAccelerationRadS2 = 6.0f;
    config.maximumYawAngularDelta = 0.15f;
    config.bodyRotationSlowdownOffsetRad = DegToRad(10.0f);
    config.directionChangeSpeedRadS = 0.38f;
    config.directionChangeYawAccelerationRadS2 = 4.0f;
    config.directionChangeMaximumYawAngularDelta = 0.10f;
    config.directionChangeSlowdownOffsetRad = DegToRad(15.0f);
    config.autoCenterSpeedRadS = 0.38f;
    config.autoCenterYawAccelerationRadS2 = 4.0f;
    config.autoCenterMaximumYawAngularDelta = 0.10f;
    config.autoCenterSlowdownOffsetRad = DegToRad(15.0f);
    config.maximumBodyOffsetRad = DegToRad(32.0f);
    config.entryYawBoostSeconds = 0.0f;
    config.entryYawBoostRateRadS = 0.0f;
    config.blendInSeconds = 0.0f;
    config.blendOutSeconds = 0.0f;
    config.attitudeAuthority = 1.0f;
    config.airborneAuthority = 0.0f;
    (void)dt;
    return config;
}

void SetRelativeBodyYaw(VehicleState& state, float yawRad) {
    const float sine = std::sin(yawRad);
    const float cosine = std::cos(yawRad);
    state.body.right = {cosine, 0.0f, -sine};
    state.body.up = {0.0f, 1.0f, 0.0f};
    state.body.forward = {sine, 0.0f, cosine};
}

void SetRelativeCourseYaw(VehicleState& state,
                          float yawRad,
                          float speedMps = 20.0f) {
    state.linearVelocityWorld = {
        std::sin(yawRad) * speedMps,
        0.0f,
        std::cos(yawRad) * speedMps,
    };
}

ControlOutput StepVelocityRelative(DriftAssistController& controller,
                                   VehicleState& state,
                                   float& bodyYawRad,
                                   float& courseYawRad,
                                   float& bodyYawRateRadS,
                                   float steeringInput,
                                   bool applyYaw = true) {
    SetRelativeBodyYaw(state, bodyYawRad);
    SetRelativeCourseYaw(state, courseYawRad);
    state.angularVelocityWorld = {0.0f, bodyYawRateRadS, 0.0f};
    state.steeringInput = steeringInput;
    const ControlOutput output = controller.update(state);
    if (applyYaw) {
        controller.notifyAttitudeWritePlanned(
            output.angularVelocityDeltaLocal.y);
        bodyYawRateRadS += output.angularVelocityDeltaLocal.y;
        bodyYawRad = nfsmw_drift::WrapPi(
            bodyYawRad + bodyYawRateRadS * state.dt);
    } else {
        controller.notifyAttitudeWriteSkipped();
    }
    return output;
}

ControlOutput ActivateVelocityRelative(DriftAssistController& controller,
                                       VehicleState& state,
                                       float& bodyYawRad,
                                       float& courseYawRad,
                                       float& bodyYawRateRadS,
                                       int driftSide,
                                       bool applyYaw = true) {
    state.handbrakeInput = 1.0f;
    const ControlOutput waiting = StepVelocityRelative(
        controller, state, bodyYawRad, courseYawRad, bodyYawRateRadS,
        0.0f, applyYaw);
    Require(waiting.active && waiting.phase == DriftPhase::WaitingDirection,
            "velocity-relative entry setup must wait for its first direction");
    const ControlOutput locked = StepVelocityRelative(
        controller, state, bodyYawRad, courseYawRad, bodyYawRateRadS,
        static_cast<float>(driftSide), applyYaw);
    state.handbrakeInput = 0.0f;
    return locked;
}

void TestVelocityRelativeFirstTargetAcquiresBaseFifteen() {
    float mirroredTargets[2]{};
    float mirroredDeltas[2]{};
    int index = 0;
    for (const int driftSide : {-1, 1}) {
        const float dt = 1.0f / 60.0f;
        AssistConfig config = VelocityRelativeEntryConfig(dt);
        DriftAssistController controller(config);
        VehicleState state = MovingState();
        state.dt = dt;
        float bodyYaw = 0.0f;
        float courseYaw = 0.0f;
        float bodyYawRate = 0.0f;
        const ControlOutput locked = ActivateVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate,
            driftSide, false);

        mirroredTargets[index] = locked.bodyYawOffsetTargetRad;
        mirroredDeltas[index] = locked.angularVelocityDeltaLocal.y;
        ++index;
        Require(locked.active && locked.driftSide == driftSide &&
                    locked.yawResponseMode == YawResponseMode::AcquireBase15 &&
                    locked.acquiringMinimumDriftAngle,
                "first side-lock frame must enter AcquireBase15");
        Require(Near(locked.bodyYawOffsetTargetRad,
                     static_cast<float>(driftSide) * DegToRad(15.0f)) &&
                    Near(locked.entryYawBoostBlend, 1.0f),
                "first side-lock target must be fixed at mirrored +/-15 degrees with entry envelope active");
        Require(Near(locked.bodyYawRateTargetRadS,
                     static_cast<float>(driftSide) * 0.90f) &&
                    Near(locked.bodyYawAccelerationLimitRadS2, 9.0f) &&
                    Near(locked.bodyYawVelocityDeltaLimitRadS, 0.20f),
                "AcquireBase15 entry envelope must exceed the sustained 0.75/6 profile");
        Require(!locked.forceCountersteer &&
                    Near(locked.steeringCommand,
                         static_cast<float>(driftSide)),
                "AcquireBase15 must leave the player's front-wheel command untouched");

        bodyYaw = static_cast<float>(driftSide) * DegToRad(14.0f);
        bodyYawRate = 0.0f;
        const ControlOutput beforeBase = StepVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate,
            static_cast<float>(driftSide), false);
        Require(beforeBase.acquiringMinimumDriftAngle &&
                    beforeBase.yawResponseMode == YawResponseMode::AcquireBase15 &&
                    Near(beforeBase.entryYawBoostBlend, 0.0f) &&
                    Near(beforeBase.bodyYawAccelerationLimitRadS2, 4.0f) &&
                    Near(beforeBase.bodyYawOffsetTargetRad,
                         static_cast<float>(driftSide) * DegToRad(15.0f)),
                "the 12-degree entry cutoff must drop to hold settings without opening 32 degrees before the base angle is acquired");

        bodyYaw = static_cast<float>(driftSide) * DegToRad(14.99f);
        const ControlOutput justBelowBase = StepVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate,
            static_cast<float>(driftSide), false);
        Require(justBelowBase.acquiringMinimumDriftAngle &&
                    justBelowBase.yawResponseMode ==
                        YawResponseMode::AcquireBase15 &&
                    Near(justBelowBase.bodyYawOffsetTargetRad,
                         static_cast<float>(driftSide) * DegToRad(15.0f)),
                "same-direction input must not open the 32-degree target below the full 15-degree base angle");

        bodyYaw = static_cast<float>(driftSide) * DegToRad(15.0f);
        const ControlOutput acquired = StepVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate,
            static_cast<float>(driftSide), false);
        Require(!acquired.acquiringMinimumDriftAngle &&
                    acquired.yawResponseMode == YawResponseMode::SameDirection &&
                    Near(acquired.bodyYawOffsetTargetRad,
                         static_cast<float>(driftSide) * DegToRad(32.0f)),
                "same-direction input may open the 32-degree target only after acquiring the base angle");
    }
    Require(Near(mirroredTargets[0], -mirroredTargets[1]) &&
                Near(mirroredDeltas[0], -mirroredDeltas[1]),
            "AcquireBase15 first-frame target and correction must mirror left/right exactly");
}

void TestVelocityRelativeEntryCancellation() {
    const float dt = 1.0f / 60.0f;
    const AssistConfig config = VelocityRelativeEntryConfig(dt);

    {
        DriftAssistController controller(config);
        VehicleState state = MovingState();
        state.dt = dt;
        float bodyYaw = 0.0f;
        float courseYaw = 0.0f;
        float bodyYawRate = 0.0f;
        const ControlOutput locked = ActivateVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate, -1);
        Require(locked.entryYawBoostBlend > 0.0f,
                "release cancellation setup must start in the strong entry envelope");
        const ControlOutput released = StepVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate, 0.0f);
        const ControlOutput pressedAgain = StepVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate, -1.0f);
        Require(released.yawResponseMode == YawResponseMode::AcquireBase15 &&
                    released.acquiringMinimumDriftAngle &&
                    Near(released.entryYawBoostBlend, 0.0f) &&
                    Near(released.bodyYawAccelerationLimitRadS2, 4.0f),
                "releasing same-direction input must cancel the strong envelope but continue base-angle acquisition on hold settings");
        Require(pressedAgain.yawResponseMode == YawResponseMode::AcquireBase15 &&
                    Near(pressedAgain.entryYawBoostBlend, 0.0f),
                "reapplying same-direction input must not retrigger a cancelled entry envelope");
    }

    {
        DriftAssistController controller(config);
        VehicleState state = MovingState();
        state.dt = dt;
        float bodyYaw = 0.0f;
        float courseYaw = 0.0f;
        float bodyYawRate = 0.0f;
        ActivateVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate, -1);
        const ControlOutput opposing = StepVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate, 1.0f);
        Require(opposing.centering &&
                    opposing.yawResponseMode == YawResponseMode::DirectionChange &&
                    Near(opposing.entryYawBoostBlend, 0.0f),
                "opposite input must cancel entry and latch controlled recovery in the same frame");
    }

    for (int cancellation = 0; cancellation < 5; ++cancellation) {
        DriftAssistController controller(config);
        VehicleState state = MovingState();
        state.dt = dt;
        float bodyYaw = 0.0f;
        float courseYaw = 0.0f;
        float bodyYawRate = 0.0f;
        ActivateVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate, -1);

        SetRelativeBodyYaw(state, bodyYaw);
        SetRelativeCourseYaw(state, courseYaw);
        state.angularVelocityWorld = {0.0f, bodyYawRate, 0.0f};
        state.steeringInput = -1.0f;
        state.handbrakeInput = 0.0f;
        if (cancellation == 0) {
            state.collisionRecent = true;
        } else if (cancellation == 1) {
            state.speedMps = 20.0f;
            state.linearVelocityWorld = {20.0f, 0.0f, 2.0f};
        } else if (cancellation == 2) {
            state.speedMps = 20.0f;
            state.linearVelocityWorld = {0.0f, 0.0f, -20.0f};
        } else if (cancellation == 3) {
            state.groundedWheels = 0;
        } else {
            const float nan = std::numeric_limits<float>::quiet_NaN();
            state.speedMps = 20.0f;
            state.linearVelocityWorld = {nan, nan, nan};
        }
        const ControlOutput cancelled = controller.update(state);
        Require(cancelled.centering &&
                    cancelled.phase == DriftPhase::Centering &&
                    Near(cancelled.entryYawBoostBlend, 0.0f),
                "collision, low forward speed, reverse, lost contact, and invalid motion must cancel entry immediately");
        if (cancellation != 0) {
            Require(Near(cancelled.bodyYawRateTargetRadS, 0.0f) &&
                        cancelled.angularVelocityDeltaLocal.y * bodyYawRate <=
                            1.0e-6f,
                    "invalid velocity/contact recovery must stop angle extrapolation and only remove residual yaw");
        }
    }
}

struct AcquireProfileResult {
    float integratedEntrySeconds = 0.0f;
    float acquisitionSeconds = 0.0f;
    float offsetAtOpenRad = 0.0f;
    bool openedMaximumTarget = false;
    bool usedHoldAfterEntry = false;
};

AcquireProfileResult RunAcquireProfile(int fps, int driftSide) {
    const float dt = 1.0f / static_cast<float>(fps);
    const AssistConfig config = VelocityRelativeEntryConfig(dt);
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    state.dt = dt;
    float bodyYaw = 0.0f;
    float courseYaw = 0.0f;
    float bodyYawRate = 0.0f;
    ControlOutput output = ActivateVelocityRelative(
        controller, state, bodyYaw, courseYaw, bodyYawRate, driftSide);

    AcquireProfileResult result{};
    result.integratedEntrySeconds += output.entryYawBoostBlend * dt;
    result.acquisitionSeconds += dt;
    for (int sample = 0; sample < fps * 3; ++sample) {
        output = StepVelocityRelative(
            controller, state, bodyYaw, courseYaw, bodyYawRate,
            static_cast<float>(driftSide));
        result.integratedEntrySeconds += output.entryYawBoostBlend * dt;
        result.acquisitionSeconds += dt;
        if (output.acquiringMinimumDriftAngle) {
            const bool fixedBaseTarget = Near(
                output.bodyYawOffsetTargetRad,
                static_cast<float>(driftSide) * DegToRad(15.0f),
                DegToRad(0.01f));
            Require(fixedBaseTarget,
                    "every AcquireBase15 frame must retain the fixed 15-degree target");
            if (Near(output.entryYawBoostBlend, 0.0f) &&
                Near(output.bodyYawAccelerationLimitRadS2, 4.0f)) {
                result.usedHoldAfterEntry = true;
            }
        } else if (output.yawResponseMode == YawResponseMode::SameDirection) {
            result.openedMaximumTarget = Near(
                output.bodyYawOffsetTargetRad,
                static_cast<float>(driftSide) * DegToRad(32.0f),
                DegToRad(0.01f));
            result.offsetAtOpenRad = output.bodyYawOffsetRad;
            break;
        }
    }
    return result;
}

void TestVelocityRelativeAcquireIsFrameRateIndependent() {
    float minimumAcquireSeconds = std::numeric_limits<float>::infinity();
    float maximumAcquireSeconds = 0.0f;
    for (const int fps : {30, 60, 120}) {
        const AcquireProfileResult left = RunAcquireProfile(fps, -1);
        const AcquireProfileResult right = RunAcquireProfile(fps, 1);
        Require(left.openedMaximumTarget && right.openedMaximumTarget &&
                    left.usedHoldAfterEntry && right.usedHoldAfterEntry,
                "30/60/120 FPS mirrored runs must finish base acquisition through the post-entry hold envelope");
        Require(Near(left.integratedEntrySeconds, 0.18f, 1.0e-4f) &&
                    Near(right.integratedEntrySeconds, 0.18f, 1.0e-4f),
                "entry envelope exposure must integrate to exactly 0.18 seconds at 30/60/120 FPS");
        Require(Near(left.offsetAtOpenRad, -right.offsetAtOpenRad,
                     DegToRad(0.10f)) &&
                    std::fabs(left.offsetAtOpenRad) >= DegToRad(15.0f) &&
                    std::fabs(right.offsetAtOpenRad) >= DegToRad(15.0f),
                "mirrored runs must not open the 32-degree target before reaching the 15-degree base angle");
        minimumAcquireSeconds = std::min(
            minimumAcquireSeconds,
            std::min(left.acquisitionSeconds, right.acquisitionSeconds));
        maximumAcquireSeconds = std::max(
            maximumAcquireSeconds,
            std::max(left.acquisitionSeconds, right.acquisitionSeconds));
    }
    Require(maximumAcquireSeconds - minimumAcquireSeconds <= 0.12f,
            "base-angle acquisition timing must remain stable across 30/60/120 FPS");
}
#endif

AssistConfig SmartCountersteerConfig(float dt = 0.05f) {
    AssistConfig config{};
    config.activation = ActivationMode::HandbrakeHold;
    config.handbrakeActivationHoldSeconds = 10.0f;
    config.handbrakeActivationThreshold = 0.50f;
    config.directionDeadzone = 0.15f;
    config.enableSmartCountersteer = true;
    config.countersteerActivationBodyOffsetRad = DegToRad(15.0f);
    config.smartCountersteerAngleRad = DegToRad(55.0f);
    config.smartCountersteerReengageDelaySeconds = 0.20f;
    config.smartCountersteerHandoffDelaySeconds = 0.0f;
    config.manualSteeringTransitionSeconds = 1.375f;
    config.enableManualYawAssist = true;
    config.manualSameDirectionRampSeconds = 2.0f;
    config.manualSameDirectionInitialStrength = 0.15f;
    config.manualSameDirectionTargetYawRateRadS = 0.5625f;
    config.manualSameDirectionYawAccelerationRadS2 = 3.0f;
    config.manualSameDirectionMaximumYawAngularDelta = 0.15f;
    config.manualOppositeMaximumRecoveryYawRateRadS = 0.45f;
    config.manualOppositeYawDampingAccelerationRadS2 = 4.5f;
    config.manualOppositeMaximumYawAngularDelta = 0.15f;
    config.manualYawHardBodyOffsetRad = DegToRad(35.0f);
    config.pendulumTransitionZeroBandRad = DegToRad(2.0f);
    config.pendulumTransitionConfirmBodyOffsetRad = DegToRad(6.0f);
    config.pendulumTransitionWindowSeconds = 0.50f;
    config.maximumSteerAngleRad = DegToRad(60.0f);
    config.exitBodyOffsetRad = DegToRad(5.0f);
    config.exitDriftAngleRateRadS = DegToRad(2.0f);
    config.exitSettleSeconds = 0.10f;
    config.noDirectionTimeoutSeconds = 2.0f;
    config.minSpeedMps = 10.0f;
    config.minLongitudinalSpeedMps = 7.0f;
    config.minGroundedWheels = 2;
    config.blendInSeconds = 0.0f;
    (void)dt;
    return config;
}

void SetSmartBodyYaw(VehicleState& state, float yawRad) {
    const float sine = std::sin(yawRad);
    const float cosine = std::cos(yawRad);
    state.body.right = {cosine, 0.0f, -sine};
    state.body.up = {0.0f, 1.0f, 0.0f};
    state.body.forward = {sine, 0.0f, cosine};
}

void RequireYawDisabled(const ControlOutput& output,
                        const std::string& stage) {
    Require(Near(output.angularVelocityDeltaLocal.y, 0.0f) &&
                Near(output.bodyYawOffsetTargetRad, 0.0f) &&
                Near(output.bodyYawRateTargetRadS, 0.0f) &&
                Near(output.bodyYawAccelerationLimitRadS2, 0.0f) &&
                Near(output.bodyYawVelocityDeltaLimitRadS, 0.0f) &&
                Near(output.manualYawStrength, 0.0f) &&
                output.manualYawControlSide == 0 &&
                Near(output.entryYawBoostBlend, 0.0f) &&
                output.yawResponseMode == YawResponseMode::None &&
                !output.acquiringMinimumDriftAngle &&
                !output.bodyOffsetBrakeActive &&
                !output.manualYawBodyOffsetLimited,
            stage + " must leave every legacy yaw command disabled");
}

ControlOutput StartSmartSession(DriftAssistController& controller,
                                VehicleState& state,
                                float bodyYawRad,
                                float steering = 0.0f) {
    SetSmartBodyYaw(state, bodyYawRad);
    state.handbrakeInput = 1.0f;
    state.steeringInput = steering;
    return controller.update(state);
}

void TestSmartCountersteerMirroredThresholdAndFixedAngle() {
    for (const float direction : {-1.0f, 1.0f}) {
        AssistConfig config = SmartCountersteerConfig();
        DriftAssistController controller(config);
        VehicleState state = MovingState();
        state.dt = 0.05f;

        ControlOutput output = StartSmartSession(
            controller, state, direction * DegToRad(14.99f));
        Require(output.active &&
                    output.phase == DriftPhase::WaitingDirection &&
                    output.steeringAssistMode ==
                        SteeringAssistMode::WaitingForDrift,
                "handbrake threshold must create a session immediately below the angle threshold");
        Require(!output.forceCountersteer &&
                    Near(output.steeringCommand, 0.0f),
                "14.99 degrees must not trigger automatic countersteer");
        RequireYawDisabled(output, "below-threshold session");

        SetSmartBodyYaw(state, direction * DegToRad(15.0f));
        output = controller.update(state);
        Require(output.active && output.forceCountersteer &&
                    output.smartCountersteerActive &&
                    output.steeringAssistMode ==
                        SteeringAssistMode::SmartCountersteer &&
                    output.driftSide == static_cast<int>(direction),
                "the inclusive 15-degree boundary must activate mirrored smart countersteer");
        Require(Near(output.countersteerAngleRad,
                     -direction * DegToRad(55.0f), 1.0e-5f) &&
                    Near(output.smartCountersteerCommand,
                         -direction * (55.0f / 60.0f), 1.0e-5f) &&
                    Near(output.steeringCommand,
                         -direction * (55.0f / 60.0f), 1.0e-5f),
                "55 degrees over a 60-degree steering range must normalize to mirrored +/-0.9167");
        RequireYawDisabled(output, "active smart countersteer");
    }
}

void TestManualInputPassthroughAndDelayedReengagement() {
    AssistConfig config = SmartCountersteerConfig();
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    state.dt = 0.05f;
    ControlOutput output = StartSmartSession(
        controller, state, DegToRad(15.0f));
    Require(output.smartCountersteerActive,
            "manual-override setup must begin under automatic control");

    state.steeringInput = 0.37f;
    output = controller.update(state);
    Require(!output.forceCountersteer && output.manualSteeringOverride &&
                output.steeringAssistMode == SteeringAssistMode::ManualOverride &&
                Near(output.steeringCommand, 0.37f) &&
                Near(output.steeringDelta, 0.0f) &&
                output.yawResponseMode ==
                    YawResponseMode::ManualSameDirectionAssist &&
                output.angularVelocityDeltaLocal.y > 0.0f &&
                Near(output.steeringNeutralSeconds, 0.0f),
            "manual steering must pass through unchanged while same-direction yaw receives a bounded assist");

    state.steeringInput = 0.0f;
    for (int frame = 1; frame <= 3; ++frame) {
        output = controller.update(state);
        Require(!output.forceCountersteer &&
                    output.steeringAssistMode ==
                        SteeringAssistMode::ReengageDelay &&
                    Near(output.steeringCommand, 0.0f) &&
                    output.yawResponseMode == YawResponseMode::None &&
                    Near(output.angularVelocityDeltaLocal.y, 0.0f) &&
                    Near(output.steeringNeutralSeconds,
                         0.05f * static_cast<float>(frame), 1.0e-5f),
                "smart countersteer must stay released before 0.2 continuous neutral seconds");
    }
    output = controller.update(state);
    Require(output.forceCountersteer && output.smartCountersteerActive &&
                output.steeringAssistMode ==
                    SteeringAssistMode::SmartCountersteer &&
                Near(output.steeringNeutralSeconds, 0.20f) &&
                Near(output.steeringCommand, -(55.0f / 60.0f)),
            "smart countersteer must resume on the inclusive 0.2-second boundary");
}

void TestEitherSteeringDirectionTakesOverImmediately() {
    for (const int fps : {10, 30, 60, 120}) {
        for (const float driftDirection : {-1.0f, 1.0f}) {
            for (const float inputDirection : {-1.0f, 1.0f}) {
                AssistConfig config = SmartCountersteerConfig();
                config.smartCountersteerHandoffDelaySeconds = 2.0f;
                DriftAssistController controller(config);
                Require(Near(controller.config().smartCountersteerHandoffDelaySeconds,
                             0.0f),
                        "legacy handoff settings must be normalized away before ownership arbitration");
                VehicleState state = MovingState();
                state.dt = std::min(
                    1.0f / static_cast<float>(fps), 0.05f);
                ControlOutput output = StartSmartSession(
                    controller, state,
                    driftDirection * DegToRad(15.0f));
                Require(output.smartCountersteerActive,
                        "immediate-handoff setup must begin under automatic control");

                state.steeringInput = inputDirection;
                output = controller.update(state);
                const YawResponseMode expectedYawMode =
                    inputDirection == driftDirection
                        ? YawResponseMode::ManualSameDirectionAssist
                        : YawResponseMode::ManualOppositeRecovery;
                Require(!output.forceCountersteer &&
                            !output.smartCountersteerActive &&
                            output.manualSteeringOverride &&
                            output.steeringAssistMode ==
                                SteeringAssistMode::ManualOverride &&
                            Near(output.steeringCommand, inputDirection) &&
                            output.yawResponseMode ==
                                expectedYawMode &&
                            !output.countersteerHandoffPending &&
                            Near(output.countersteerHandoffSeconds, 0.0f),
                        "any steering direction must take ownership immediately, including added countersteer");
            }
        }
    }
}

void TestLegacyHandoffSettingIsParsedButIgnored() {
    AssistConfig config = SmartCountersteerConfig();
    config.smartCountersteerHandoffDelaySeconds = 1.75f;
    DriftAssistController controller(config);
    Require(Near(controller.config().smartCountersteerHandoffDelaySeconds,
                 0.0f),
            "the controller must neutralize the legacy handoff delay setting");

    VehicleState state = MovingState();
    state.dt = 0.05f;
    ControlOutput output = StartSmartSession(
        controller, state, DegToRad(15.0f));
    Require(output.forceCountersteer &&
                !output.countersteerHandoffPending &&
                Near(output.countersteerHandoffSeconds, 0.0f),
            "automatic control must never expose pending handoff telemetry");

    // For a +1 drift side, both signs are valid player takeover events.  The
    // automatic countersteer direction is no longer privileged once the player
    // supplies a steering sample.
    state.steeringInput = 1.0f;
    output = controller.update(state);
    Require(!output.forceCountersteer && output.manualSteeringOverride &&
                output.steeringAssistMode ==
                    SteeringAssistMode::ManualOverride &&
                !output.countersteerHandoffPending &&
                Near(output.countersteerHandoffSeconds, 0.0f),
                "a legacy nonzero handoff value must not delay same-side takeover");
}

void TestSignedLateSameDirectionTapReleasesOwnershipImmediately() {
    for (const int driftDirection : {-1, 1}) {
        DriftAssistController controller(SmartCountersteerConfig());
        VehicleState state = MovingState();
        state.dt = 0.05f;
        ControlOutput output = StartSmartSession(
            controller, state,
            static_cast<float>(driftDirection) * DegToRad(15.0f));
        Require(output.forceCountersteer,
                "mirrored late-input setup must begin under automatic control");

        state.steeringInput = 0.0f;
        state.manualSteeringInputObserved = true;
        state.manualSteeringDirectionMaskObserved =
            nfsmw_drift::SteeringDirectionObservationMask(driftDirection);
        output = controller.update(state);
        Require(!output.forceCountersteer &&
                    !output.smartCountersteerActive &&
                    !output.manualSteeringOverride &&
                    output.steeringAssistMode ==
                        SteeringAssistMode::ReengageDelay &&
                    Near(output.steeringNeutralSeconds, 0.0f) &&
                    !output.countersteerHandoffPending &&
                    Near(output.countersteerHandoffSeconds, 0.0f),
                "a mirrored signed same-direction tap must release automatic ownership immediately and start neutral timing at zero");

        state.manualSteeringInputObserved = false;
        state.manualSteeringDirectionMaskObserved = 0;
        for (int frame = 1; frame < 4; ++frame) {
            output = controller.update(state);
            Require(!output.forceCountersteer &&
                        output.steeringAssistMode ==
                            SteeringAssistMode::ReengageDelay &&
                        Near(output.steeringNeutralSeconds,
                             0.05f * static_cast<float>(frame), 1.0e-5f),
                    "late-tap release must remain manual until 0.20 seconds of subsequent neutral input");
        }

        output = controller.update(state);
        Require(output.forceCountersteer &&
                    output.smartCountersteerActive &&
                    output.steeringAssistMode ==
                        SteeringAssistMode::SmartCountersteer &&
                    Near(output.steeringNeutralSeconds, 0.20f) &&
                    Near(output.steeringCommand,
                         -static_cast<float>(driftDirection) *
                             (55.0f / 60.0f)),
                "mirrored same-direction takeover must resume exactly after 0.20 seconds of neutral input");
    }
}

void TestPreTakeoverInputDoesNotStartReengageDelay() {
    AssistConfig config = SmartCountersteerConfig();
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    state.dt = 0.05f;

    ControlOutput output = StartSmartSession(
        controller, state, DegToRad(14.0f), 0.60f);
    Require(output.active && output.manualSteeringOverride &&
                output.steeringAssistMode == SteeringAssistMode::ManualOverride &&
                Near(output.steeringCommand, 0.60f),
            "entry steering before the first automatic takeover must pass through");

    state.steeringInput = 0.0f;
    SetSmartBodyYaw(state, DegToRad(15.0f));
    output = controller.update(state);
    Require(output.smartCountersteerActive && output.forceCountersteer &&
                Near(output.steeringCommand, -(55.0f / 60.0f)),
            "pre-takeover manual steering must not impose the reengagement delay");
}

void TestLateManualInputEventStartsDelayAtZero() {
    AssistConfig config = SmartCountersteerConfig();
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    state.dt = 0.05f;
    ControlOutput output = StartSmartSession(
        controller, state, -DegToRad(15.0f));
    Require(output.smartCountersteerActive,
            "late-input setup must first allow automatic control");

    state.steeringInput = 0.0f;
    state.manualSteeringInputObserved = true;
    output = controller.update(state);
    Require(!output.forceCountersteer && !output.manualSteeringOverride &&
                output.steeringAssistMode ==
                    SteeringAssistMode::ReengageDelay &&
                Near(output.steeringCommand, 0.0f) &&
                output.yawResponseMode == YawResponseMode::None &&
                Near(output.angularVelocityDeltaLocal.y, 0.0f) &&
                Near(output.steeringNeutralSeconds, 0.0f),
            "a late manual-input event must start the next-frame neutral delay at zero even when raw input is already neutral");

    state.manualSteeringInputObserved = false;
    output = controller.update(state);
    Require(output.steeringAssistMode ==
                SteeringAssistMode::ReengageDelay &&
                Near(output.steeringNeutralSeconds, 0.05f),
            "the event frame itself must not count toward the neutral delay");
}

void TestDelayExpiryBelowThresholdWaitsForAngle() {
    AssistConfig config = SmartCountersteerConfig();
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    state.dt = 0.05f;
    ControlOutput output = StartSmartSession(
        controller, state, DegToRad(15.0f));
    Require(output.smartCountersteerActive,
            "threshold-wait setup must start under automatic control");

    state.steeringInput = 0.40f;
    controller.update(state);
    state.steeringInput = 0.0f;
    for (int frame = 0; frame < 3; ++frame) {
        controller.update(state);
    }
    SetSmartBodyYaw(state, DegToRad(14.0f));
    output = controller.update(state);
    Require(!output.forceCountersteer &&
                output.steeringAssistMode ==
                    SteeringAssistMode::WaitingForDrift &&
                Near(output.steeringNeutralSeconds, 0.20f),
            "an expired delay below 15 degrees must transition to waiting for angle");

    SetSmartBodyYaw(state, DegToRad(15.0f));
    output = controller.update(state);
    Require(output.smartCountersteerActive &&
                Near(output.steeringCommand, -(55.0f / 60.0f)),
            "after delay expiry, reaching 15 degrees again must reengage immediately");
}

void TestSmartCountersteerSafeExitAndRearm() {
    AssistConfig config = SmartCountersteerConfig();
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    state.dt = 0.05f;
    ControlOutput output = StartSmartSession(
        controller, state, DegToRad(15.0f));
    Require(output.smartCountersteerActive,
            "safe-exit setup must first cross the drift threshold");

    SetSmartBodyYaw(state, DegToRad(4.0f));
    output = controller.update(state);
    Require(output.active,
            "a fast crossing into the exit window must not finish immediately");
    output = controller.update(state);
    Require(output.active,
            "the first settled 0.05 second sample must retain the session");
    output = controller.update(state);
    Require(!output.active && output.phase == DriftPhase::Off &&
                !output.forceCountersteer &&
                Near(output.steeringCommand, 0.0f),
            "two settled samples must safely finish the 0.10-second exit window");
    RequireYawDisabled(output, "finished session");

    output = controller.update(state);
    Require(!output.active,
            "a still-held handbrake must not immediately rearm after exit");
    state.handbrakeInput = 0.0f;
    controller.update(state);
    state.handbrakeInput = 1.0f;
    output = controller.update(state);
    Require(output.active,
            "releasing the handbrake once must permit a fresh immediate session");
}

void TestManualYawPureControlLaw() {
    const AssistConfig config = SmartCountersteerConfig();
    const float dt = 0.05f;

    for (const float invalidDt : {
             0.0f,
             -0.01f,
             std::numeric_limits<float>::infinity(),
             0.1001f}) {
        const auto invalid = nfsmw_drift::ComputeManualYawCommand(
            config, DegToRad(20.0f), 0.0f, 1.0f, 1, invalidDt, 1.0f);
        Require(invalid.mode == YawResponseMode::None &&
                    Near(invalid.targetYawRateRadS, 0.0f) &&
                    Near(invalid.accelerationLimitRadS2, 0.0f) &&
                    Near(invalid.velocityDeltaLimitRadS, 0.0f) &&
                    Near(invalid.yawVelocityDeltaRadS, 0.0f) &&
                    !invalid.bodyOffsetLimited && !invalid.dampingActive,
                "zero, negative, non-finite, and over-0.1-second dt values must fail closed with an empty manual-yaw command");
    }

    const auto same = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(20.0f), 0.0f, 1.0f, 1, dt, 1.0f);
    Require(same.mode == YawResponseMode::ManualSameDirectionAssist &&
                Near(same.targetYawRateRadS, 0.5625f) &&
                Near(same.accelerationLimitRadS2, 3.0f) &&
                Near(same.velocityDeltaLimitRadS, 0.15f) &&
                Near(same.yawVelocityDeltaRadS, 0.15f),
            "full same-direction input must add only the bounded yaw needed toward 0.5625 rad/s");

    const auto weakSame = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(20.0f), 0.0f, 1.0f, 1, dt, 0.15f);
    Require(weakSame.mode == YawResponseMode::ManualSameDirectionAssist &&
                Near(weakSame.targetYawRateRadS, 0.5625f) &&
                Near(weakSame.accelerationLimitRadS2, 0.45f) &&
                Near(weakSame.velocityDeltaLimitRadS, 0.0225f) &&
                Near(weakSame.yawVelocityDeltaRadS, 0.0225f),
            "the initial 0.15 strength must scale same-direction acceleration and delta without lowering the 0.5625 target rate");

    const auto mirroredSame = nfsmw_drift::ComputeManualYawCommand(
        config, -DegToRad(20.0f), 0.0f, -1.0f, -1, dt, 1.0f);
    Require(mirroredSame.mode ==
                YawResponseMode::ManualSameDirectionAssist &&
                Near(mirroredSame.targetYawRateRadS,
                     -same.targetYawRateRadS) &&
                Near(mirroredSame.yawVelocityDeltaRadS,
                     -same.yawVelocityDeltaRadS),
            "same-direction yaw assist must mirror left and right exactly");

    const auto analog = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(20.0f), 0.0f, 0.575f, 1, dt, 1.0f);
    Require(Near(analog.targetYawRateRadS, 0.28125f) &&
                Near(analog.accelerationLimitRadS2, 1.5f) &&
                Near(analog.velocityDeltaLimitRadS, 0.075f) &&
                Near(analog.yawVelocityDeltaRadS, 0.075f),
            "manual yaw target and step must scale with input beyond the deadzone");

    const auto alreadyFast = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(20.0f), 0.90f, 1.0f, 1, dt, 1.0f);
    Require(alreadyFast.mode ==
                YawResponseMode::ManualSameDirectionAssist &&
                Near(alreadyFast.yawVelocityDeltaRadS, 0.0f),
            "same-direction assist must never brake naturally faster yaw");

    const auto hardLimited = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(35.0f), 0.0f, 1.0f, 1, dt, 1.0f);
    Require(hardLimited.mode ==
                YawResponseMode::ManualSameDirectionAssist &&
                hardLimited.bodyOffsetLimited &&
                Near(hardLimited.yawVelocityDeltaRadS, 0.0f),
            "the 35-degree hard angle may report manual intent but must emit no outward yaw write");

    const auto mirroredHardLimited = nfsmw_drift::ComputeManualYawCommand(
        config, -DegToRad(35.0f), 0.0f, -1.0f, -1, dt, 1.0f);
    Require(mirroredHardLimited.mode == hardLimited.mode &&
                mirroredHardLimited.bodyOffsetLimited &&
                Near(mirroredHardLimited.targetYawRateRadS,
                     -hardLimited.targetYawRateRadS) &&
                Near(mirroredHardLimited.accelerationLimitRadS2,
                     hardLimited.accelerationLimitRadS2) &&
                Near(mirroredHardLimited.velocityDeltaLimitRadS,
                     hardLimited.velocityDeltaLimitRadS) &&
                Near(mirroredHardLimited.yawVelocityDeltaRadS,
                     -hardLimited.yawVelocityDeltaRadS),
            "same-direction input at negative 35 degrees must mirror the positive-side hard limit and emit no outward yaw write");

    const auto outward = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(20.0f), 0.12f, -1.0f, 1, dt, 0.15f);
    const float outwardNextRate = 0.12f + outward.yawVelocityDeltaRadS;
    Require(outward.mode == YawResponseMode::ManualOppositeRecovery &&
                Near(outward.targetYawRateRadS, -0.45f) &&
                Near(outward.accelerationLimitRadS2, 4.5f) &&
                Near(outward.velocityDeltaLimitRadS, 0.15f) &&
                Near(outward.yawVelocityDeltaRadS, -0.15f) &&
                DegToRad(20.0f) + outwardNextRate * dt >
                    config.pendulumTransitionZeroBandRad,
            "opposite input must actively target center at full recovery strength even while the same-direction envelope is weak");

    const auto fastOutward = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(20.0f), 1.0f, -1.0f, 1, dt, 1.0f);
    Require(Near(fastOutward.yawVelocityDeltaRadS, -0.15f) &&
                1.0f + fastOutward.yawVelocityDeltaRadS > 0.0f,
            "active recovery must obey its per-frame delta limit while removing outward yaw");

    const auto fastRecovery = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(20.0f), -0.70f, -1.0f, 1, dt, 1.0f);
    Require(Near(fastRecovery.targetYawRateRadS, -0.45f) &&
                Near(fastRecovery.yawVelocityDeltaRadS, 0.15f) &&
                -0.70f + fastRecovery.yawVelocityDeltaRadS < 0.0f,
            "recovery faster than 0.45 rad/s must be reduced exactly to the distant-angle cap without reversing yaw");

    const auto mirroredFastRecovery = nfsmw_drift::ComputeManualYawCommand(
        config, -DegToRad(20.0f), 0.70f, 1.0f, -1, dt, 1.0f);
    Require(mirroredFastRecovery.mode == fastRecovery.mode &&
                mirroredFastRecovery.dampingActive &&
                Near(mirroredFastRecovery.targetYawRateRadS,
                     -fastRecovery.targetYawRateRadS) &&
                Near(mirroredFastRecovery.yawVelocityDeltaRadS,
                     -fastRecovery.yawVelocityDeltaRadS) &&
                0.70f + mirroredFastRecovery.yawVelocityDeltaRadS > 0.0f,
            "negative-side active recovery must mirror the positive side without manufacturing excess yaw");

    const auto assistedRecovery = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(20.0f), -0.20f, -1.0f, 1, dt, 1.0f);
    Require(assistedRecovery.mode ==
                YawResponseMode::ManualOppositeRecovery &&
                Near(assistedRecovery.targetYawRateRadS, -0.45f) &&
                Near(assistedRecovery.yawVelocityDeltaRadS, -0.15f),
            "opposite input must actively accelerate a slow natural recovery toward 0.45 rad/s");

    const auto inZeroBand = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(2.0f), -0.50f, -1.0f, 1, dt, 1.0f);
    Require(inZeroBand.mode == YawResponseMode::ManualOppositeRecovery &&
                Near(inZeroBand.targetYawRateRadS, 0.0f) &&
                Near(inZeroBand.yawVelocityDeltaRadS, 0.0f),
            "the inclusive +/-2-degree transition band must emit no manual yaw write");

    const float distanceOutsideBand = DegToRad(1.0f);
    const float stoppingLimited = std::sqrt(
        2.0f * 4.5f * distanceOutsideBand);
    const auto stoppingBound = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(3.0f), 0.0f, -1.0f, 1, 0.01f, 1.0f);
    Require(Near(stoppingBound.targetYawRateRadS, -stoppingLimited) &&
                Near(stoppingBound.yawVelocityDeltaRadS, -0.045f),
            "just outside the zero band, sqrt(2*a*distance) must bound the center-seeking target");

    const auto oneFrameBound = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(3.0f), 0.0f, -1.0f, 1, 0.10f, 1.0f);
    const float oneFrameTarget = -distanceOutsideBand / 0.10f;
    Require(Near(oneFrameBound.targetYawRateRadS, oneFrameTarget) &&
                Near(oneFrameBound.yawVelocityDeltaRadS, -0.15f) &&
                DegToRad(3.0f) +
                        oneFrameBound.yawVelocityDeltaRadS * 0.10f >
                    config.pendulumTransitionZeroBandRad,
            "distance/dt must bound the target while the stricter angular-delta cap keeps the sample outside the zero band");

    const auto mirroredOutward = nfsmw_drift::ComputeManualYawCommand(
        config, -DegToRad(20.0f), -0.12f, 1.0f, -1, dt, 0.15f);
    Require(Near(mirroredOutward.yawVelocityDeltaRadS,
                 -outward.yawVelocityDeltaRadS),
            "active opposite recovery must mirror left and right exactly");

    for (const auto disabled : {
             nfsmw_drift::ComputeManualYawCommand(
                 config, DegToRad(10.0f), 0.0f, 0.0f, 1, dt, 1.0f),
             nfsmw_drift::ComputeManualYawCommand(
                 config, DegToRad(10.0f), 0.0f, 1.0f, 0, dt, 1.0f)}) {
        Require(disabled.mode == YawResponseMode::None &&
                    Near(disabled.yawVelocityDeltaRadS, 0.0f),
                "neutral steering and an unknown drift side must fail closed");
    }

    AssistConfig steeringOnly = config;
    steeringOnly.actuation = ActuationMode::SteeringOnly;
    const auto unavailable = nfsmw_drift::ComputeManualYawCommand(
        steeringOnly, DegToRad(10.0f), 0.0f, 1.0f, 1, dt, 1.0f);
    Require(unavailable.mode == YawResponseMode::None &&
                Near(unavailable.yawVelocityDeltaRadS, 0.0f),
            "SteeringOnly actuation must keep the manual body-yaw channel closed");
}

void TestManualYawFrameRateConsistency() {
    const AssistConfig config = SmartCountersteerConfig();
    for (const int fps : {30, 60, 120}) {
        const float dt = 1.0f / static_cast<float>(fps);
        float sameDirectionRate = 0.0f;
        float outwardRate = 1.0f;
        for (int frame = 0; frame < fps / 10; ++frame) {
            sameDirectionRate += nfsmw_drift::ComputeManualYawCommand(
                config, DegToRad(20.0f), sameDirectionRate,
                1.0f, 1, dt, 1.0f).yawVelocityDeltaRadS;
            outwardRate += nfsmw_drift::ComputeManualYawCommand(
                config, DegToRad(20.0f), outwardRate,
                -1.0f, 1, dt, 1.0f).yawVelocityDeltaRadS;
        }
        Require(Near(sameDirectionRate, 0.30f, 1.0e-5f) &&
                    Near(outwardRate, 0.55f, 1.0e-5f),
                "same-direction acceleration and opposite recovery must each remain stable at 30/60/120 FPS");
    }
}

void TestManualSameDirectionStrengthEnvelope() {
    for (const int fps : {30, 60, 120}) {
        const float dt = 1.0f / static_cast<float>(fps);
        DriftAssistController controller(SmartCountersteerConfig());
        VehicleState state = MovingState();
        state.dt = dt;

        ControlOutput output = StartSmartSession(
            controller, state, DegToRad(15.0f), 1.0f);
        Require(output.active && output.driftSide == 1 &&
                    output.manualSteeringOverride &&
                    Near(output.steeringCommand, 1.0f) &&
                    output.yawResponseMode ==
                        YawResponseMode::ManualSameDirectionAssist &&
                    Near(output.manualYawStrength, 0.15f) &&
                    output.manualYawControlSide == 1 &&
                    Near(output.bodyYawRateTargetRadS, 0.5625f) &&
                    Near(output.bodyYawAccelerationLimitRadS2, 0.45f) &&
                    Near(output.bodyYawVelocityDeltaLimitRadS, 0.0225f),
                "the first same-direction frame must start at 0.15 strength without reducing the 0.5625 rad/s target");

        for (int frame = 0; frame < fps; ++frame) {
            output = controller.update(state);
            Require(Near(output.bodyYawRateTargetRadS, 0.5625f),
                    "the same-direction target rate must stay fixed throughout the strength ramp");
        }
        Require(Near(output.manualYawStrength, 0.575f, 1.0e-4f) &&
                    Near(output.bodyYawAccelerationLimitRadS2, 1.725f,
                         1.0e-4f) &&
                    Near(output.bodyYawVelocityDeltaLimitRadS, 0.08625f,
                         1.0e-4f),
                "1.00 second of continuous same-direction input must reach exactly 0.575 strength");

        for (int frame = 0; frame < fps; ++frame) {
            output = controller.update(state);
        }
        Require(Near(output.manualYawStrength, 1.0f, 1.0e-4f) &&
                    Near(output.bodyYawRateTargetRadS, 0.5625f),
                "2.00 seconds of continuous same-direction input must reach full strength");
        output = controller.update(state);
        Require(Near(output.manualYawStrength, 1.0f, 1.0e-4f),
                "same-direction strength must remain capped at one after the ramp");

        state.steeringInput = 0.0f;
        output = controller.update(state);
        RequireYawDisabled(output, "neutral envelope reset");
        state.steeringInput = 1.0f;
        output = controller.update(state);
        Require(Near(output.manualYawStrength, 0.15f) &&
                    output.manualYawControlSide == 1,
                "neutral steering must reset the next same-direction frame to 0.15 strength");

        state.steeringInput = -1.0f;
        output = controller.update(state);
        Require(output.yawResponseMode ==
                    YawResponseMode::ManualOppositeRecovery &&
                    output.manualYawControlSide == 1,
                "opposite steering must use full recovery independently of the same-direction envelope");
        state.steeringInput = 1.0f;
        output = controller.update(state);
        Require(Near(output.manualYawStrength, 0.15f),
                "opposite steering must reset the next same-direction frame to 0.15 strength");

        state.handbrakeInput = 0.0f;
        state.steeringInput = 0.0f;
        SetSmartBodyYaw(state, 0.0f);
        output = controller.update(state);
        Require(!output.active && output.phase == DriftPhase::Off,
                "returning through center after handbrake release must end the established session");
        state.handbrakeInput = 1.0f;
        state.steeringInput = 1.0f;
        SetSmartBodyYaw(state, DegToRad(15.0f));
        output = controller.update(state);
        Require(output.active && Near(output.manualYawStrength, 0.15f) &&
                    output.manualYawControlSide == 1,
                "a new session after exit must restart same-direction strength at 0.15");
    }
}

void TestSideTransitionRequiresEstablishedDrift() {
    DriftAssistController controller(SmartCountersteerConfig());
    VehicleState state = MovingState();
    state.dt = 0.05f;
    ControlOutput output = StartSmartSession(
        controller, state, 0.0f, 1.0f);
    Require(output.driftSide == 1 && !output.sideTransitionActive,
            "manual entry may provisionally choose a side before a measured drift is established");

    SetSmartBodyYaw(state, DegToRad(2.0f));
    state.steeringInput = -1.0f;
    output = controller.update(state);
    Require(output.active && !output.sideTransitionActive &&
                output.phase != DriftPhase::SideTransition,
            "crossing the center band before reaching 15 degrees must not start a pendulum transition");
}

void TestSideTransitionMirroredConfirmation() {
    for (const float oldSide : {-1.0f, 1.0f}) {
        for (const bool releaseHandbrake : {false, true}) {
            DriftAssistController controller(SmartCountersteerConfig());
            VehicleState state = MovingState();
            state.dt = 0.05f;
            ControlOutput output = StartSmartSession(
                controller, state, oldSide * DegToRad(15.0f));
            Require(output.forceCountersteer &&
                        output.driftSide == static_cast<int>(oldSide),
                    "side-transition setup must first establish the old side at 15 degrees");

            if (releaseHandbrake) {
                state.handbrakeInput = 0.0f;
            }
            state.steeringInput = -oldSide;
            SetSmartBodyYaw(state, oldSide * DegToRad(2.0f));
            output = controller.update(state);
            Require(output.active &&
                        output.phase == DriftPhase::SideTransition &&
                        output.sideTransitionActive &&
                        output.sideTransitionTargetSide ==
                            -static_cast<int>(oldSide) &&
                        !output.forceCountersteer &&
                        output.manualSteeringOverride &&
                        Near(output.steeringCommand, -oldSide) &&
                        Near(output.manualYawStrength, 0.0f) &&
                        output.manualYawControlSide == 0 &&
                        output.yawResponseMode == YawResponseMode::None &&
                        Near(output.angularVelocityDeltaLocal.y, 0.0f),
                    "opposite steering at the inclusive two-degree band must release front-wheel and yaw ownership immediately");

            state.steeringInput = -oldSide * 0.73f;
            SetSmartBodyYaw(state, -oldSide * DegToRad(5.99f));
            output = controller.update(state);
            Require(output.sideTransitionActive &&
                        output.driftSide == static_cast<int>(oldSide) &&
                        Near(output.steeringCommand, state.steeringInput) &&
                        !output.forceCountersteer &&
                        output.yawResponseMode == YawResponseMode::None &&
                        Near(output.angularVelocityDeltaLocal.y, 0.0f),
                    "5.99 degrees on the new side must remain in the observation window with exact steering passthrough");

            SetSmartBodyYaw(state, -oldSide * DegToRad(6.0f));
            output = controller.update(state);
            Require(output.active && !output.sideTransitionActive &&
                        output.phase == DriftPhase::WaitingDirection &&
                        output.driftSide == -static_cast<int>(oldSide) &&
                        Near(output.steeringCommand, state.steeringInput) &&
                        output.yawResponseMode ==
                            YawResponseMode::ManualSameDirectionAssist &&
                        Near(output.manualYawStrength, 0.15f) &&
                        output.manualYawControlSide ==
                            -static_cast<int>(oldSide),
                    "the inclusive six-degree boundary must commit the mirrored new side and restart same-direction strength at 0.15");
        }
    }
}

void TestSideTransitionPreservesManualReengagementDelay() {
    for (const int fps : {30, 60, 120}) {
        for (const float oldSide : {-1.0f, 1.0f}) {
            DriftAssistController controller(SmartCountersteerConfig());
            VehicleState state = MovingState();
            state.dt = 1.0f / static_cast<float>(fps);
            ControlOutput output = StartSmartSession(
                controller, state, oldSide * DegToRad(15.0f));
            Require(output.forceCountersteer,
                    "reengagement-delay transition setup must begin under automatic countersteer");

            state.steeringInput = -oldSide;
            SetSmartBodyYaw(state, oldSide * DegToRad(2.0f));
            output = controller.update(state);
            Require(output.sideTransitionActive &&
                        !output.forceCountersteer &&
                        output.manualSteeringOverride,
                    "manual opposite input at the center band must interrupt automatic countersteer");

            state.steeringInput = 0.0f;
            SetSmartBodyYaw(state, -oldSide * DegToRad(15.0f));
            output = controller.update(state);
            Require(output.active && !output.sideTransitionActive &&
                        output.driftSide == -static_cast<int>(oldSide) &&
                        output.steeringAssistMode ==
                            SteeringAssistMode::ReengageDelay &&
                        !output.forceCountersteer &&
                        Near(output.steeringCommand, 0.0f),
                    "a neutral new-side confirmation must not bypass the 0.2-second delay after manual interruption");

            const int neutralFrames = static_cast<int>(std::lround(
                controller.config().smartCountersteerReengageDelaySeconds /
                state.dt));
            for (int frame = 2; frame < neutralFrames; ++frame) {
                output = controller.update(state);
                Require(output.steeringAssistMode ==
                            SteeringAssistMode::ReengageDelay &&
                            !output.forceCountersteer,
                        "automatic countersteer must remain released before the inclusive reengagement boundary");
            }
            output = controller.update(state);
            Require(output.smartCountersteerActive &&
                        output.forceCountersteer &&
                        Near(output.steeringCommand,
                             oldSide * (55.0f / 60.0f)),
                    "automatic countersteer must reacquire on the 0.2-second neutral boundary after a pendulum confirmation");
        }
    }
}

void TestSideTransitionExactTimeout() {
    for (const float oldSide : {-1.0f, 1.0f}) {
        DriftAssistController controller(SmartCountersteerConfig());
        VehicleState state = MovingState();
        state.dt = 0.05f;
        ControlOutput output = StartSmartSession(
            controller, state, oldSide * DegToRad(15.0f));
        Require(output.forceCountersteer,
                "transition-timeout setup must establish automatic countersteer");

        state.steeringInput = -oldSide * 0.64f;
        SetSmartBodyYaw(state, oldSide * DegToRad(2.0f));
        output = controller.update(state);
        Require(output.sideTransitionActive &&
                    Near(output.sideTransitionSeconds, 0.0f),
                "the transition observation clock must start at zero on the center-band frame");

        SetSmartBodyYaw(state, -oldSide * DegToRad(5.99f));
        for (int frame = 1; frame < 10; ++frame) {
            output = controller.update(state);
            Require(output.active && output.sideTransitionActive &&
                        Near(output.sideTransitionSeconds,
                             0.05f * static_cast<float>(frame), 1.0e-5f) &&
                        Near(output.steeringCommand, state.steeringInput) &&
                        !output.forceCountersteer &&
                        output.yawResponseMode == YawResponseMode::None &&
                        Near(output.angularVelocityDeltaLocal.y, 0.0f),
                    "an unconfirmed pendulum must remain passive before the exact 0.50-second boundary");
        }

        output = controller.update(state);
        Require(!output.active && output.phase == DriftPhase::Off &&
                    !output.sideTransitionActive &&
                    Near(output.steeringCommand, state.steeringInput) &&
                    !output.forceCountersteer,
                "an unconfirmed transition must exit exactly at 0.50 seconds while preserving driver steering");
        RequireYawDisabled(output, "transition timeout frame");

        state.steeringInput = oldSide * 0.31f;
        output = controller.update(state);
        Require(!output.active && Near(output.steeringCommand,
                                       state.steeringInput),
                "after transition timeout, the controller must remain fully passive until rearmed");
        RequireYawDisabled(output, "post-timeout frame");
    }
}

void TestReleasedAutomaticOwnershipDoesNotStartDelay() {
    DriftAssistController controller(SmartCountersteerConfig());
    VehicleState state = MovingState();
    state.dt = 0.05f;
    ControlOutput output = StartSmartSession(
        controller, state, DegToRad(15.0f));
    Require(output.forceCountersteer,
            "released-ownership setup must begin with automatic countersteer");

    SetSmartBodyYaw(state, DegToRad(4.0f));
    output = controller.update(state);
    Require(!output.forceCountersteer,
            "automatic countersteer must release itself inside the exit angle");

    state.steeringInput = 0.40f;
    output = controller.update(state);
    Require(output.manualSteeringOverride &&
                output.steeringAssistMode == SteeringAssistMode::ManualOverride,
            "manual input after automatic release must remain a direct passthrough event");

    state.steeringInput = 0.0f;
    SetSmartBodyYaw(state, DegToRad(15.0f));
    output = controller.update(state);
    Require(output.forceCountersteer &&
                output.steeringAssistMode ==
                    SteeringAssistMode::SmartCountersteer &&
                Near(output.steeringNeutralSeconds, 0.0f),
            "manual input after ownership was already released must not impose a historical 0.2-second delay");
}

void TestLegacyAndDisabledManualYawRemainOff() {
    AssistConfig config = ImmediateConfig();
    config.activation = ActivationMode::Manual;
    config.enableYawVelocityAssist = true;
    config.actuation = ActuationMode::SteeringAndAttitude;
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    state.assistRequested = true;
    state.angularVelocityWorld.y = 8.0f;
    ControlOutput output = controller.update(state);
    Require(Near(output.angularVelocityDeltaLocal.y, 0.0f),
            "legacy activation modes must remain unable to emit body yaw");

    config = SmartCountersteerConfig();
    config.enableManualYawAssist = false;
    controller.setConfig(config);
    controller.reset();
    state = MovingState();
    state.dt = 0.05f;
    output = StartSmartSession(controller, state, 0.0f, 1.0f);
    Require(output.manualSteeringOverride &&
                Near(output.steeringCommand, 1.0f),
            "disabling manual yaw must not interfere with driver steering");
    RequireYawDisabled(output, "disabled manual-yaw frame");
}

} // namespace

int main() {
    TestLowSpeedDoesNotActivate();
    TestHandbrakeEntry();
    TestSideslipEntry();
    TestSideslipHysteresis();
    TestRearSlipUsesWheelLoad();
    TestExtremeFiniteWheelLoadsRemainStable();
    TestDriftSideReversal();
    TestExcessYawProducesCountersteer();
    TestAttitudeRecoveryDirections();
    TestRecoveryAndAirborneAuthority();
    TestLimitsAndNonFiniteInputs();
    TestDeterministicStress();
    TestSmartCountersteerMirroredThresholdAndFixedAngle();
    TestManualInputPassthroughAndDelayedReengagement();
    TestEitherSteeringDirectionTakesOverImmediately();
    TestLegacyHandoffSettingIsParsedButIgnored();
    TestSignedLateSameDirectionTapReleasesOwnershipImmediately();
    TestPreTakeoverInputDoesNotStartReengageDelay();
    TestLateManualInputEventStartsDelayAtZero();
    TestDelayExpiryBelowThresholdWaitsForAngle();
    TestSmartCountersteerSafeExitAndRearm();
    TestManualYawPureControlLaw();
    TestManualYawFrameRateConsistency();
    TestManualSameDirectionStrengthEnvelope();
    TestSideTransitionRequiresEstablishedDrift();
    TestSideTransitionMirroredConfirmation();
    TestSideTransitionPreservesManualReengagementDelay();
    TestSideTransitionExactTimeout();
    TestReleasedAutomaticOwnershipDoesNotStartDelay();
    TestLegacyAndDisabledManualYawRemainOff();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All drift assist tests passed\n";
    return EXIT_SUCCESS;
}
