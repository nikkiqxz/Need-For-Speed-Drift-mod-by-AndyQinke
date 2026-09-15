#include "drift_assist.hpp"
#include "drift_assist_ini.hpp"
#include "asi_host/player_vehicle_selection.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

using nfsmw_drift::ActivationMode;
using nfsmw_drift::ActuationMode;
using nfsmw_drift::AssistConfig;
using nfsmw_drift::ControlOutput;
using nfsmw_drift::DegToRad;
using nfsmw_drift::DriftAssistController;
using nfsmw_drift::DriftPhase;
using nfsmw_drift::IniParseErrorCode;
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

bool Near(float actual, float expected, float tolerance = 1.0e-4f) {
    return std::fabs(actual - expected) <= tolerance;
}

bool Finite(float value) {
    return std::isfinite(value) != 0;
}

AssistConfig SmartConfig() {
    AssistConfig config{};
    config.activation = ActivationMode::HandbrakeHold;
    config.actuation = ActuationMode::SteeringAndAttitude;
    config.handbrakeActivationHoldSeconds = 30.0f;
    config.handbrakeActivationThreshold = 0.50f;
    config.directionDeadzone = 0.15f;
    config.enableSmartCountersteer = true;
    config.countersteerActivationBodyOffsetRad = DegToRad(15.0f);
    config.smartCountersteerAngleRad = DegToRad(55.0f);
    config.smartCountersteerReengageDelaySeconds = 0.20f;
    // Any valid steering direction is an immediate player takeover, including
    // the signed countersteer direction. Keep the legacy field explicit so
    // these tests do not accidentally inherit a stale handoff dwell from a
    // default profile.
    config.smartCountersteerHandoffDelaySeconds = 0.0f;
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
    config.minSpeedMps = 10.0f;
    config.minLongitudinalSpeedMps = 7.0f;
    config.minGroundedWheels = 2;
    config.exitBodyOffsetRad = DegToRad(5.0f);
    config.exitDriftAngleRateRadS = DegToRad(2.0f);
    config.exitSettleSeconds = 0.10f;
    config.noDirectionTimeoutSeconds = 2.0f;
    config.frontGripMultiplierDuringDrift = 1.10f;
    config.rearDriveMultiplierDuringDrift = 1.20f;
    config.blendInSeconds = 0.0f;
    return config;
}

VehicleState MovingState(float dt = 0.05f) {
    VehicleState state{};
    state.dt = dt;
    state.linearVelocityWorld = {0.0f, 0.0f, 20.0f};
    state.speedMps = 20.0f;
    state.groundedWheels = 4;
    state.wheelLoad = {1.0f, 1.0f, 1.0f, 1.0f};
    state.hasSurfaceNormal = true;
    state.surfaceNormal = {0.0f, 1.0f, 0.0f};
    state.playerControlled = true;
    state.inRace = true;
    return state;
}

void SetBodyYaw(VehicleState& state, float yawRad) {
    const float sine = std::sin(yawRad);
    const float cosine = std::cos(yawRad);
    state.body.right = {cosine, 0.0f, -sine};
    state.body.up = {0.0f, 1.0f, 0.0f};
    state.body.forward = {sine, 0.0f, cosine};
}

void SetCourseYaw(VehicleState& state, float yawRad) {
    state.linearVelocityWorld = {
        std::sin(yawRad) * 20.0f,
        0.0f,
        std::cos(yawRad) * 20.0f,
    };
    state.speedMps = 20.0f;
}

void RequireNoYawCommands(const ControlOutput& output,
                          const std::string& stage) {
    Require(Near(output.targetYawRateRadS, 0.0f) &&
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
                !output.manualYawBodyOffsetLimited &&
                Near(output.angularVelocityDeltaLocal.y, 0.0f),
            stage + " must expose no yaw target, boost, hold, or write");
}

ControlOutput StartSession(DriftAssistController& controller,
                           VehicleState& state,
                           float bodyYawRad,
                           float steering = 0.0f) {
    SetBodyYaw(state, bodyYawRad);
    SetCourseYaw(state, 0.0f);
    state.handbrakeInput = 1.0f;
    state.steeringInput = steering;
    return controller.update(state);
}

void TestImmediateSessionAndEligibility() {
    AssistConfig config = SmartConfig();
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    state.handbrakeInput = 0.49f;
    Require(!controller.update(state).active,
            "handbrake below threshold must not create a session");

    state.handbrakeInput = 0.50f;
    const ControlOutput active = controller.update(state);
    Require(active.active && active.phase == DriftPhase::WaitingDirection &&
                Near(active.handbrakeHoldProgress, 1.0f),
            "handbrake threshold must create a session immediately despite the legacy hold duration");
    Require(active.steeringAssistMode ==
                SteeringAssistMode::WaitingForDrift &&
                !active.forceCountersteer,
            "a zero-angle session must wait for drift without steering takeover");
    RequireNoYawCommands(active, "session activation");

    for (int testCase = 0; testCase < 3; ++testCase) {
        DriftAssistController blockedController(config);
        VehicleState blocked = MovingState();
        blocked.handbrakeInput = 1.0f;
        if (testCase == 0) {
            blocked.speedMps = 9.0f;
        } else if (testCase == 1) {
            blocked.linearVelocityWorld.z = 6.0f;
        } else {
            blocked.groundedWheels = 1;
        }
        const ControlOutput output = blockedController.update(blocked);
        Require(!output.active && !output.forceCountersteer,
                "low total speed, low forward speed, and lost contact must block activation");
        RequireNoYawCommands(output, "blocked activation");
    }
}

void TestMirroredThresholdAndNormalization() {
    float commands[2]{};
    int index = 0;
    for (const float direction : {-1.0f, 1.0f}) {
        DriftAssistController controller(SmartConfig());
        VehicleState state = MovingState();
        ControlOutput output = StartSession(
            controller, state, direction * DegToRad(14.99f));
        Require(output.active && !output.forceCountersteer &&
                    output.steeringAssistMode ==
                        SteeringAssistMode::WaitingForDrift,
                "14.99 degrees must stay below the activation boundary");

        SetBodyYaw(state, direction * DegToRad(15.0f));
        output = controller.update(state);
        commands[index++] = output.steeringCommand;
        Require(output.forceCountersteer &&
                    output.smartCountersteerActive &&
                    output.driftSide == static_cast<int>(direction) &&
                    output.steeringAssistMode ==
                        SteeringAssistMode::SmartCountersteer,
                "the inclusive 15-degree boundary must acquire steering ownership");
        Require(Near(output.bodyYawOffsetRad,
                     direction * DegToRad(15.0f), DegToRad(0.01f)) &&
                    Near(output.countersteerAngleRad,
                         -direction * DegToRad(55.0f), 1.0e-5f) &&
                    Near(output.smartCountersteerCommand,
                         -direction * (55.0f / 60.0f), 1.0e-5f) &&
                    Near(output.steeringCommand,
                         -direction * (55.0f / 60.0f), 1.0e-5f),
                "55 physical degrees over a 60-degree range must produce mirrored normalized 0.9167");
        RequireNoYawCommands(output, "smart-countersteer threshold");
    }
    Require(Near(commands[0], -commands[1]),
            "left and right countersteer commands must mirror exactly");
}

void TestOwnedSteeringDoesNotChatterAtThreshold() {
    DriftAssistController controller(SmartConfig());
    VehicleState state = MovingState();
    ControlOutput output = StartSession(
        controller, state, DegToRad(15.0f));
    Require(output.smartCountersteerActive,
            "threshold-latch setup must acquire steering");
    for (const float degrees : {14.9f, 15.1f, 14.8f, 15.0f}) {
        SetBodyYaw(state, DegToRad(degrees));
        output = controller.update(state);
        Require(output.smartCountersteerActive && output.forceCountersteer &&
                    Near(output.steeringCommand, -(55.0f / 60.0f)),
                "owned steering must remain latched across 15-degree sensor jitter");
    }
}

void TestManualPassthroughAndReengagement() {
    DriftAssistController controller(SmartConfig());
    VehicleState state = MovingState();
    ControlOutput output = StartSession(
        controller, state, DegToRad(15.0f));
    Require(output.smartCountersteerActive,
            "manual test must begin under automatic control");

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
            "manual input must pass through unchanged while same-direction yaw assistance remains available");

    state.steeringInput = 0.0f;
    for (int frame = 1; frame <= 3; ++frame) {
        output = controller.update(state);
        Require(!output.forceCountersteer &&
                    output.steeringAssistMode ==
                        SteeringAssistMode::ReengageDelay &&
                    output.yawResponseMode == YawResponseMode::None &&
                    Near(output.angularVelocityDeltaLocal.y, 0.0f) &&
                    Near(output.steeringNeutralSeconds,
                         0.05f * static_cast<float>(frame), 1.0e-5f),
                "automatic steering must remain released before 0.2 continuous neutral seconds");
    }
    output = controller.update(state);
    Require(output.smartCountersteerActive && output.forceCountersteer &&
                Near(output.steeringNeutralSeconds, 0.20f) &&
                Near(output.steeringCommand, -(55.0f / 60.0f)),
            "automatic steering must reacquire at the inclusive 0.2-second boundary");
}

void TestSignedManualDirectionOwnership() {
    // Every signed direction input revokes automatic ownership immediately,
    // including an input that agrees with the current countersteer and is
    // intended to add more countersteer. The old INI key is deliberately set
    // to a non-zero value to prove it cannot reintroduce a hidden handoff
    // dwell.
    for (const float driftDirection : {-1.0f, 1.0f}) {
        for (const float inputDirection : {-1.0f, 1.0f}) {
            AssistConfig config = SmartConfig();
            config.smartCountersteerHandoffDelaySeconds = 1.75f;
            DriftAssistController controller(config);
            VehicleState state = MovingState();
            state.dt = 0.05f;
            ControlOutput output = StartSession(
                controller, state, driftDirection * DegToRad(15.0f));
            Require(output.smartCountersteerActive && output.forceCountersteer,
                    "both-direction takeover setup must begin under automatic control");

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
                    "any signed steering input must take ownership immediately");
        }
    }
}

void TestOnlyPostOwnershipInputStartsDelay() {
    DriftAssistController controller(SmartConfig());
    VehicleState state = MovingState();
    ControlOutput output = StartSession(
        controller, state, DegToRad(14.0f), 0.60f);
    Require(output.manualSteeringOverride &&
                Near(output.steeringCommand, 0.60f),
            "entry steering before ownership must pass through");
    state.steeringInput = 0.0f;
    SetBodyYaw(state, DegToRad(15.0f));
    output = controller.update(state);
    Require(output.smartCountersteerActive &&
                Near(output.steeringCommand, -(55.0f / 60.0f)),
            "pre-ownership manual steering must not impose a reengagement delay");

    state.manualSteeringInputObserved = true;
    output = controller.update(state);
    Require(!output.forceCountersteer &&
                !output.manualSteeringOverride &&
                output.steeringAssistMode ==
                    SteeringAssistMode::ReengageDelay &&
                Near(output.steeringCommand, 0.0f) &&
                output.yawResponseMode == YawResponseMode::None &&
                Near(output.angularVelocityDeltaLocal.y, 0.0f) &&
                Near(output.steeringNeutralSeconds, 0.0f),
            "a late manual event with current neutral input must start delay at zero");
    state.manualSteeringInputObserved = false;
    output = controller.update(state);
    Require(Near(output.steeringNeutralSeconds, state.dt),
            "the manual-event frame itself must not count as neutral dwell");

    DriftAssistController neverOwned(SmartConfig());
    state = MovingState();
    state.manualSteeringInputObserved = true;
    output = StartSession(neverOwned, state, DegToRad(14.0f));
    state.manualSteeringInputObserved = false;
    SetBodyYaw(state, DegToRad(15.0f));
    output = neverOwned.update(state);
    Require(output.smartCountersteerActive,
            "an event before automatic steering ever owned control must not impose delay");

    DriftAssistController previouslyOwned(SmartConfig());
    state = MovingState();
    StartSession(previouslyOwned, state, DegToRad(15.0f));
    SetBodyYaw(state, DegToRad(4.0f));
    output = previouslyOwned.update(state);
    Require(!output.forceCountersteer,
            "automatic steering must release ownership near center");
    state.steeringInput = 0.40f;
    previouslyOwned.update(state);
    state.steeringInput = 0.0f;
    SetBodyYaw(state, DegToRad(15.0f));
    output = previouslyOwned.update(state);
    Require(output.smartCountersteerActive &&
                Near(output.steeringNeutralSeconds, 0.0f),
            "manual input after automatic ownership was already released must not start a historical reengagement delay");
}

void TestDelayContinuityAndFrameRates() {
    for (const int fps : {30, 60, 120}) {
        const float dt = 1.0f / static_cast<float>(fps);
        DriftAssistController controller(SmartConfig());
        VehicleState state = MovingState(dt);
        ControlOutput output = StartSession(
            controller, state, -DegToRad(15.0f));
        state.steeringInput = -0.40f;
        controller.update(state);
        state.steeringInput = 0.0f;
        const int frames = static_cast<int>(std::ceil(0.20f / dt));
        for (int frame = 1; frame <= frames; ++frame) {
            output = controller.update(state);
            if (frame < frames) {
                Require(!output.forceCountersteer,
                        "30/60/120 FPS runs must not reacquire early");
            }
        }
        Require(output.smartCountersteerActive &&
                    Near(output.steeringNeutralSeconds, 0.20f, dt + 1.0e-5f),
                "30/60/120 FPS runs must reacquire after equivalent neutral time");
    }

    DriftAssistController controller(SmartConfig());
    VehicleState state = MovingState();
    StartSession(controller, state, DegToRad(15.0f));
    state.steeringInput = 0.40f;
    controller.update(state);
    state.steeringInput = 0.0f;
    controller.update(state);
    controller.update(state);
    state.steeringInput = -0.40f;
    const ControlOutput reset = controller.update(state);
    Require(Near(reset.steeringNeutralSeconds, 0.0f),
            "renewed manual input must reset the continuous-neutral timer");
}

void TestDelayExpiryBelowThresholdWaitsForAngle() {
    DriftAssistController controller(SmartConfig());
    VehicleState state = MovingState();
    ControlOutput output = StartSession(
        controller, state, DegToRad(15.0f));
    state.steeringInput = 0.40f;
    controller.update(state);
    state.steeringInput = 0.0f;
    for (int frame = 0; frame < 3; ++frame) {
        controller.update(state);
    }
    SetBodyYaw(state, DegToRad(14.0f));
    output = controller.update(state);
    Require(!output.forceCountersteer &&
                output.steeringAssistMode ==
                    SteeringAssistMode::WaitingForDrift &&
                Near(output.steeringNeutralSeconds, 0.20f),
            "delay expiry below threshold must return to angle-wait mode");
    SetBodyYaw(state, DegToRad(15.0f));
    output = controller.update(state);
    Require(output.smartCountersteerActive,
            "reaching threshold after delay expiry must reacquire immediately");
}

void TestSafeExitTimeoutAndRearm() {
    AssistConfig timeoutConfig = SmartConfig();
    timeoutConfig.noDirectionTimeoutSeconds = 0.20f;
    DriftAssistController timeoutController(timeoutConfig);
    VehicleState timeoutState = MovingState();
    ControlOutput output = StartSession(timeoutController, timeoutState, 0.0f);
    timeoutState.handbrakeInput = 0.0f;
    for (int frame = 0; frame < 3; ++frame) {
        output = timeoutController.update(timeoutState);
        Require(output.active,
                "unused released session must remain before its timeout");
    }
    output = timeoutController.update(timeoutState);
    Require(!output.active,
            "unused released session must finish at timeout");

    for (const float direction : {-1.0f, 1.0f}) {
        DriftAssistController controller(SmartConfig());
        VehicleState state = MovingState();
        output = StartSession(
            controller, state, direction * DegToRad(15.0f));
        state.handbrakeInput = 0.0f;
        output = controller.update(state);
        Require(output.active && output.smartCountersteerActive,
                "handbrake release must not end an established session");
        SetBodyYaw(state, direction * DegToRad(4.0f));
        output = controller.update(state);
        Require(output.active && !output.forceCountersteer,
                "countersteer must release near center without unsafe instant exit");
        output = controller.update(state);
        Require(output.active,
                "one settled sample must not satisfy a 0.10-second exit dwell");
        output = controller.update(state);
        Require(!output.active && output.phase == DriftPhase::Off &&
                    output.steeringAssistMode == SteeringAssistMode::None,
                "mirrored sessions must exit after offset and rate settle");
    }

    AssistConfig rearmConfig = SmartConfig();
    rearmConfig.exitSettleSeconds = 0.0f;
    DriftAssistController rearmController(rearmConfig);
    VehicleState rearmState = MovingState();
    StartSession(rearmController, rearmState, DegToRad(15.0f));
    SetBodyYaw(rearmState, DegToRad(4.0f));
    rearmController.update(rearmState);
    output = rearmController.update(rearmState);
    Require(!output.active,
            "settled near-center frame must exit with zero dwell");
    Require(!rearmController.update(rearmState).active,
            "held handbrake must not immediately rearm after exit");
    rearmState.handbrakeInput = 0.0f;
    rearmController.update(rearmState);
    rearmState.handbrakeInput = 1.0f;
    SetBodyYaw(rearmState, DegToRad(15.0f));
    Require(rearmController.update(rearmState).smartCountersteerActive,
            "one released frame must permit a new session");
}

void TestUnsafeStatesFailClosed() {
    for (int testCase = 0; testCase < 3; ++testCase) {
        DriftAssistController controller(SmartConfig());
        VehicleState state = MovingState();
        Require(StartSession(controller, state, DegToRad(15.0f))
                    .smartCountersteerActive,
                "unsafe-state setup must acquire steering");
        state.handbrakeInput = 0.0f;
        state.steeringInput = 0.42f;
        if (testCase == 0) {
            state.collisionRecent = true;
        } else if (testCase == 1) {
            state.speedMps = 5.0f;
        } else if (testCase == 2) {
            state.linearVelocityWorld = {0.0f, 0.0f, -20.0f};
        }
        const ControlOutput output = controller.update(state);
        Require(!output.active && !output.forceCountersteer &&
                    Near(output.steeringCommand, 0.42f) &&
                    output.steeringAssistMode == SteeringAssistMode::None,
                "collision, low speed, and reverse must fail closed with input passthrough");
        RequireNoYawCommands(output, "unsafe-state exit");
    }
}

void TestActiveGroundContactLossGraceWindow() {
    AssistConfig config = SmartConfig();
    config.groundContactLossGraceSeconds = 3.0f;
    DriftAssistController controller(config);
    VehicleState state = MovingState(0.05f);
    Require(StartSession(controller, state, DegToRad(15.0f))
                .smartCountersteerActive,
            "contact-grace setup must acquire smart countersteer");
    state.handbrakeInput = 0.0f;
    state.groundedWheels = 1;

    ControlOutput output{};
    for (int frame = 0; frame < 59; ++frame) {
        output = controller.update(state);
        Require(output.active && output.groundContactGraceActive,
                "active drift must survive contact loss before three seconds");
    }
    Require(Near(output.groundContactLossSeconds, 2.95f, 2.0e-4f),
            "contact-loss telemetry must accumulate to 2.95 seconds");
    output = controller.update(state);
    Require(!output.active && !output.groundContactGraceActive &&
                output.phase == DriftPhase::Off,
            "continuous contact loss must end the drift at three seconds");

    DriftAssistController recoveredController(config);
    state = MovingState(0.05f);
    StartSession(recoveredController, state, DegToRad(15.0f));
    state.handbrakeInput = 0.0f;
    state.groundedWheels = 0;
    for (int frame = 0; frame < 20; ++frame) {
        output = recoveredController.update(state);
    }
    Require(output.active &&
                Near(output.groundContactLossSeconds, 1.0f, 2.0e-4f),
            "one second of contact loss must stay inside the grace window");
    state.groundedWheels = 4;
    output = recoveredController.update(state);
    Require(output.active && !output.groundContactGraceActive &&
                Near(output.groundContactLossSeconds, 0.0f),
            "restored wheel contact must reset the grace timer immediately");
    state.groundedWheels = 0;
    for (int frame = 0; frame < 59; ++frame) {
        output = recoveredController.update(state);
    }
    Require(output.active,
            "a recovered contact timer must grant a fresh three-second window");
}

void TestDisabledAndZeroRangeFailClosed() {
    for (int testCase = 0; testCase < 2; ++testCase) {
        AssistConfig config = SmartConfig();
        if (testCase == 0) {
            config.enableSmartCountersteer = false;
        } else {
            config.maximumSteerAngleRad = 0.0f;
        }
        DriftAssistController controller(config);
        VehicleState state = MovingState();
        const ControlOutput output = StartSession(
            controller, state, DegToRad(35.0f));
        Require(output.active && !output.forceCountersteer &&
                    !output.smartCountersteerActive &&
                    Near(output.steeringCommand, 0.0f),
                "disabled assist and zero steering range must remain passive");
        RequireNoYawCommands(output, "passive session");
    }
}

void TestGripDriveAndHardStops() {
    DriftAssistController controller(SmartConfig());
    VehicleState state = MovingState();
    ControlOutput output = StartSession(controller, state, 0.0f);
    Require(Near(output.frontGripScale, 1.10f) &&
                Near(output.rearDriveScale, 1.20f),
            "active session must retain configured grip and drive scales");
    state.handbrakeInput = 0.0f;
    state.inRace = false;
    state.steeringInput = 0.33f;
    output = controller.update(state);
    Require(!output.active && !output.forceCountersteer &&
                Near(output.steeringCommand, 0.33f) &&
                Near(output.frontGripScale, 1.0f) &&
                Near(output.rearDriveScale, 1.0f),
            "hard stop must restore neutral scales and preserve driver steering");
    RequireNoYawCommands(output, "hard stop");
}

void TestConfigAndIni() {
    const AssistConfig defaults{};
    Require(Near(defaults.smartCountersteerAngleRad, DegToRad(55.0f)) &&
                Near(defaults.maximumSteerAngleRad, DegToRad(60.0f)) &&
                Near(defaults.smartCountersteerHandoffDelaySeconds, 0.0f) &&
                Near(defaults.manualSteeringTransitionSeconds, 1.375f) &&
                Near(nfsmw_drift::Clamp(
                         defaults.smartCountersteerAngleRad /
                             defaults.maximumSteerAngleRad,
                         -1.0f, 1.0f),
                     55.0f / 60.0f) &&
                Near(defaults.manualSameDirectionTargetYawRateRadS, 0.5625f) &&
                Near(defaults.manualSameDirectionYawAccelerationRadS2, 3.0f) &&
                Near(defaults.manualSameDirectionMaximumYawAngularDelta, 0.15f) &&
                Near(defaults.manualSameDirectionRampSeconds, 2.0f) &&
                Near(defaults.manualSameDirectionInitialStrength, 0.15f) &&
                Near(defaults.manualOppositeMaximumRecoveryYawRateRadS, 0.45f) &&
                Near(defaults.manualOppositeYawDampingAccelerationRadS2, 4.5f) &&
                Near(defaults.manualOppositeMaximumYawAngularDelta, 0.15f),
            "default profile must expose the 55-over-60-degree countersteer calibration and reduced two-second manual-yaw envelope");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    AssistConfig invalid{};
    invalid.countersteerActivationBodyOffsetRad = nan;
    invalid.smartCountersteerAngleRad = -1.0f;
    invalid.smartCountersteerReengageDelaySeconds = 99.0f;
    invalid.smartCountersteerHandoffDelaySeconds = 99.0f;
    invalid.manualSteeringTransitionSeconds = 99.0f;
    invalid.manualSameDirectionTargetYawRateRadS = nan;
    invalid.manualSameDirectionYawAccelerationRadS2 = 99.0f;
    invalid.manualSameDirectionMaximumYawAngularDelta = 99.0f;
    invalid.manualSameDirectionRampSeconds = 99.0f;
    invalid.manualSameDirectionInitialStrength = -1.0f;
    invalid.manualOppositeMaximumRecoveryYawRateRadS = 99.0f;
    invalid.manualOppositeYawDampingAccelerationRadS2 = 99.0f;
    invalid.manualOppositeMaximumYawAngularDelta = 99.0f;
    invalid.manualYawHardBodyOffsetRad = 99.0f;
    invalid.pendulumTransitionZeroBandRad = 99.0f;
    invalid.pendulumTransitionConfirmBodyOffsetRad = -1.0f;
    invalid.pendulumTransitionWindowSeconds = 99.0f;
    DriftAssistController controller(invalid);
    Require(Near(controller.config().countersteerActivationBodyOffsetRad,
                 DegToRad(15.0f)) &&
                Near(controller.config().smartCountersteerAngleRad, 0.0f) &&
                Near(controller.config().smartCountersteerReengageDelaySeconds,
                     2.0f) &&
                Near(controller.config().smartCountersteerHandoffDelaySeconds,
                     0.0f) &&
                Near(controller.config().manualSteeringTransitionSeconds,
                     10.0f) &&
                Near(controller.config().manualSameDirectionTargetYawRateRadS,
                     0.5625f) &&
                Near(controller.config().manualSameDirectionYawAccelerationRadS2,
                     3.0f) &&
                Near(controller.config().manualSameDirectionMaximumYawAngularDelta,
                     0.15f) &&
                Near(controller.config().manualSameDirectionRampSeconds,
                     2.0f) &&
                Near(controller.config().manualSameDirectionInitialStrength,
                     0.0f) &&
                Near(controller.config().manualOppositeMaximumRecoveryYawRateRadS,
                     0.45f) &&
                Near(controller.config().manualOppositeYawDampingAccelerationRadS2,
                     4.5f) &&
                Near(controller.config().manualOppositeMaximumYawAngularDelta,
                     0.15f) &&
                Near(controller.config().manualYawHardBodyOffsetRad,
                     DegToRad(35.0f)) &&
                Near(controller.config().pendulumTransitionZeroBandRad,
                     DegToRad(10.0f)) &&
                Near(controller.config().pendulumTransitionConfirmBodyOffsetRad,
                     DegToRad(10.0f)) &&
                Near(controller.config().pendulumTransitionWindowSeconds,
                     2.0f),
            "active settings must sanitize invalid values while the legacy handoff delay remains disabled");

    AssistConfig loaded{};
    const auto result = nfsmw_drift::ParseAssistConfigIni(
        "[AssistConfig]\n"
        "enableSmartCountersteer=false\n"
        "countersteerActivationBodyOffsetRad=0.50\n"
        "smartCountersteerAngleRad=0.60\n"
        "smartCountersteerReengageDelaySeconds=0.30\n"
        "smartCountersteerHandoffDelaySeconds=1.60\n"
        "manualSteeringTransitionSeconds=2.75\n"
        "enableManualYawAssist=false\n"
        "manualSameDirectionTargetYawRateRadS=0.70\n"
        "manualSameDirectionYawAccelerationRadS2=3.50\n"
        "manualSameDirectionMaximumYawAngularDelta=0.15\n"
        "manualSameDirectionRampSeconds=0.80\n"
        "manualSameDirectionInitialStrength=0.20\n"
        "manualOppositeMaximumRecoveryYawRateRadS=0.30\n"
        "manualOppositeYawDampingAccelerationRadS2=3.25\n"
        "manualOppositeMaximumYawAngularDelta=0.12\n"
        "manualYawHardBodyOffsetRad=0.55\n"
        "pendulumTransitionZeroBandRad=0.03\n"
        "pendulumTransitionConfirmBodyOffsetRad=0.12\n"
        "pendulumTransitionWindowSeconds=0.45\n",
        loaded);
    Require(result.ok && !loaded.enableSmartCountersteer &&
                Near(loaded.countersteerActivationBodyOffsetRad, 0.50f) &&
                Near(loaded.smartCountersteerAngleRad, 0.60f) &&
                Near(loaded.smartCountersteerReengageDelaySeconds, 0.30f) &&
                Near(loaded.smartCountersteerHandoffDelaySeconds, 1.60f) &&
                Near(loaded.manualSteeringTransitionSeconds, 2.75f) &&
                !loaded.enableManualYawAssist &&
                Near(loaded.manualSameDirectionTargetYawRateRadS, 0.70f) &&
                Near(loaded.manualSameDirectionYawAccelerationRadS2, 3.50f) &&
                Near(loaded.manualSameDirectionMaximumYawAngularDelta, 0.15f) &&
                Near(loaded.manualSameDirectionRampSeconds, 0.80f) &&
                Near(loaded.manualSameDirectionInitialStrength, 0.20f) &&
                Near(loaded.manualOppositeMaximumRecoveryYawRateRadS, 0.30f) &&
                Near(loaded.manualOppositeYawDampingAccelerationRadS2, 3.25f) &&
                Near(loaded.manualOppositeMaximumYawAngularDelta, 0.12f) &&
                Near(loaded.manualYawHardBodyOffsetRad, 0.55f) &&
                Near(loaded.pendulumTransitionZeroBandRad, 0.03f) &&
                Near(loaded.pendulumTransitionConfirmBodyOffsetRad, 0.12f) &&
                Near(loaded.pendulumTransitionWindowSeconds, 0.45f),
            "INI parser must load every smart-countersteer and manual-yaw setting");

    DriftAssistController loadedController(loaded);
    Require(Near(loaded.smartCountersteerHandoffDelaySeconds, 1.60f) &&
                Near(loadedController.config()
                         .smartCountersteerHandoffDelaySeconds,
                     0.0f),
            "the parser must accept the legacy handoff key while the controller disables its value");

    AssistConfig unchanged{};
    unchanged.smartCountersteerAngleRad = 0.70f;
    unchanged.smartCountersteerHandoffDelaySeconds = 1.65f;
    unchanged.manualSteeringTransitionSeconds = 2.90f;
    unchanged.manualSameDirectionRampSeconds = 0.90f;
    unchanged.pendulumTransitionWindowSeconds = 0.40f;
    const auto malformed = nfsmw_drift::ParseAssistConfigIni(
        "[AssistConfig]\n"
        "manualSteeringTransitionSeconds=2.50\n"
        "manualSameDirectionRampSeconds=0.25\n"
        "pendulumTransitionWindowSeconds=invalid\n",
        unchanged);
    Require(!malformed.ok && malformed.line == 4 &&
                malformed.error == IniParseErrorCode::InvalidValue &&
                unchanged.enableManualYawAssist &&
                Near(unchanged.smartCountersteerAngleRad, 0.70f) &&
                Near(unchanged.smartCountersteerHandoffDelaySeconds, 1.65f) &&
                Near(unchanged.manualSteeringTransitionSeconds, 2.90f) &&
                Near(unchanged.manualSameDirectionRampSeconds, 0.90f) &&
                Near(unchanged.pendulumTransitionWindowSeconds, 0.40f),
            "invalid new INI value must reject the transaction");
}

void TestManualYawRequirementContract() {
    const AssistConfig config = SmartConfig();
    DriftAssistController controller(config);
    VehicleState state = MovingState();
    ControlOutput output = StartSession(controller, state, 0.0f, 1.0f);
    Require(output.active && output.driftSide == 1 &&
                output.manualSteeringOverride &&
                Near(output.steeringCommand, 1.0f) &&
                output.yawResponseMode ==
                    YawResponseMode::ManualSameDirectionAssist &&
                Near(output.manualYawStrength, 0.15f) &&
                output.manualYawControlSide == 1 &&
                Near(output.bodyYawRateTargetRadS, 0.5625f) &&
                Near(output.bodyYawAccelerationLimitRadS2, 0.45f) &&
                output.angularVelocityDeltaLocal.y > 0.0f,
            "the first same-direction frame must pass steering through and begin yaw assistance at 0.15 strength toward 0.5625 rad/s");

    state.steeringInput = 0.0f;
    SetBodyYaw(state, DegToRad(10.0f));
    output = controller.update(state);
    Require(!output.forceCountersteer &&
                output.yawResponseMode == YawResponseMode::None &&
                Near(output.angularVelocityDeltaLocal.y, 0.0f),
            "neutral steering below 15 degrees must not start, hold, or center body yaw");
    RequireNoYawCommands(output, "neutral manual-yaw frame");

    const auto outwardRecovery = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(20.0f), 0.10f, -1.0f, 1, state.dt, 1.0f);
    Require(outwardRecovery.mode ==
                YawResponseMode::ManualOppositeRecovery &&
                Near(outwardRecovery.targetYawRateRadS, -0.45f) &&
                Near(outwardRecovery.yawVelocityDeltaRadS, -0.15f),
            "opposite input must actively reverse a small outward rate toward center within the configured delta cap");

    const auto fastRecovery = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(20.0f), -0.70f, -1.0f, 1, state.dt, 1.0f);
    Require(Near(fastRecovery.targetYawRateRadS, -0.45f) &&
                Near(fastRecovery.yawVelocityDeltaRadS, 0.15f) &&
                -0.70f + fastRecovery.yawVelocityDeltaRadS < 0.0f,
            "recovery faster than 0.45 rad/s must be trimmed to the cap without reversing its sign");

    const auto hardLimit = nfsmw_drift::ComputeManualYawCommand(
        config, DegToRad(35.0f), 0.0f, 1.0f, 1, state.dt, 1.0f);
    Require(hardLimit.mode ==
                YawResponseMode::ManualSameDirectionAssist &&
                hardLimit.bodyOffsetLimited &&
                Near(hardLimit.yawVelocityDeltaRadS, 0.0f),
            "same-direction intent at the 35-degree hard limit must not produce a setter-worthy delta");
}

void TestManualYawFrameRateAndRampContract() {
    const AssistConfig config = SmartConfig();
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
                "manual yaw acceleration must be frame-rate independent at 30/60/120 FPS");

        DriftAssistController controller(config);
        VehicleState state = MovingState(dt);
        ControlOutput output = StartSession(
            controller, state, DegToRad(15.0f), 1.0f);
        Require(Near(output.manualYawStrength, 0.15f) &&
                    Near(output.bodyYawRateTargetRadS, 0.5625f) &&
                    Near(output.bodyYawAccelerationLimitRadS2, 0.45f) &&
                    Near(output.bodyYawVelocityDeltaLimitRadS, 0.0225f),
                "the two-second ramp must begin at exactly 15 percent strength");

        for (int frame = 0; frame < fps; ++frame) {
            output = controller.update(state);
        }
        Require(Near(output.manualYawStrength, 0.575f, 1.0e-4f) &&
                    Near(output.bodyYawAccelerationLimitRadS2, 1.725f,
                         1.0e-4f) &&
                    Near(output.bodyYawVelocityDeltaLimitRadS, 0.08625f,
                         1.0e-4f),
                "the two-second ramp must reach its exact midpoint after one second at 30/60/120 FPS");

        for (int frame = 0; frame < fps; ++frame) {
            output = controller.update(state);
        }
        Require(Near(output.manualYawStrength, 1.0f, 1.0e-4f) &&
                    Near(output.bodyYawAccelerationLimitRadS2, 3.0f,
                         1.0e-4f) &&
                    Near(output.bodyYawVelocityDeltaLimitRadS, 0.15f,
                         1.0e-4f),
                "the ramp must reach full reduced authority after exactly two seconds at 30/60/120 FPS");
    }
}

void TestSideTransitionRequirementContract() {
    DriftAssistController controller(SmartConfig());
    VehicleState state = MovingState();
    ControlOutput output = StartSession(
        controller, state, DegToRad(15.0f));
    Require(output.forceCountersteer && output.driftSide == 1,
            "pendulum acceptance setup must establish the old side at 15 degrees");

    state.handbrakeInput = 0.0f;
    state.steeringInput = -0.70f;
    SetBodyYaw(state, DegToRad(2.0f));
    output = controller.update(state);
    Require(output.active && output.phase == DriftPhase::SideTransition &&
                output.sideTransitionActive &&
                output.sideTransitionTargetSide == -1 &&
                !output.forceCountersteer &&
                Near(output.steeringCommand, -0.70f),
            "opposite input entering the two-degree band must release automatic steering and open the transition window");
    RequireNoYawCommands(output, "side-transition release frame");

    SetBodyYaw(state, -DegToRad(6.0f));
    output = controller.update(state);
    Require(output.active && !output.sideTransitionActive &&
                output.driftSide == -1 &&
                Near(output.steeringCommand, -0.70f) &&
                output.yawResponseMode ==
                    YawResponseMode::ManualSameDirectionAssist &&
                Near(output.manualYawStrength, 0.15f) &&
                output.manualYawControlSide == -1,
            "six degrees on the opposite side must commit the new side even after handbrake release and restart the yaw envelope");

    DriftAssistController timeoutController(SmartConfig());
    VehicleState timeoutState = MovingState();
    output = StartSession(timeoutController, timeoutState,
                          -DegToRad(15.0f));
    Require(output.forceCountersteer && output.driftSide == -1,
            "timeout acceptance setup must establish the mirrored old side");
    timeoutState.steeringInput = 0.60f;
    SetBodyYaw(timeoutState, -DegToRad(2.0f));
    output = timeoutController.update(timeoutState);
    Require(output.sideTransitionActive,
            "mirrored center crossing must enter the observation window");
    SetBodyYaw(timeoutState, DegToRad(5.99f));
    for (int frame = 1; frame < 10; ++frame) {
        output = timeoutController.update(timeoutState);
        Require(output.active && output.sideTransitionActive,
                "an unconfirmed new side must remain active before 0.50 seconds");
    }
    output = timeoutController.update(timeoutState);
    Require(!output.active && output.phase == DriftPhase::Off &&
                !output.sideTransitionActive &&
                Near(output.steeringCommand, 0.60f) &&
                !output.forceCountersteer,
            "an unconfirmed side transition must fail passive on the exact 0.50-second boundary");
    RequireNoYawCommands(output, "side-transition timeout frame");
}

void TestNonManualYawAndNonFiniteSafety() {
    for (const ActivationMode activation : {
             ActivationMode::HandbrakeHold,
             ActivationMode::Manual,
             ActivationMode::Automatic,
             ActivationMode::ManualOrAutomatic}) {
        for (const ActuationMode actuation : {
                 ActuationMode::SteeringOnly,
                 ActuationMode::SteeringAndAttitude,
                 ActuationMode::AttitudeOnly}) {
            AssistConfig config = SmartConfig();
            config.activation = activation;
            config.actuation = actuation;
            config.enableYawVelocityAssist = true;
            config.entryYawBoostRateRadS = 0.90f;
            config.driftAngleEntrySpeedRadS = 0.90f;
            DriftAssistController controller(config);
            VehicleState state = MovingState();
            SetBodyYaw(state, DegToRad(30.0f));
            state.handbrakeInput = 1.0f;
            state.assistRequested = true;
            state.angularVelocityWorld = {0.0f, 12.0f, 0.0f};
            RequireNoYawCommands(controller.update(state),
                                 "activation/actuation matrix");
        }
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    DriftAssistController controller(SmartConfig());
    VehicleState state = MovingState();
    state.dt = nan;
    state.steeringInput = nan;
    state.angularVelocityWorld = {nan, nan, nan};
    const ControlOutput output = controller.update(state);
    Require(!output.active && !output.forceCountersteer &&
                Finite(output.steeringCommand) &&
                Finite(output.smartCountersteerCommand) &&
                Finite(output.steeringNeutralSeconds),
            "non-finite state must fail closed with finite steering telemetry");
    RequireNoYawCommands(output, "non-finite state");
}

void TestPlayerVehicleSelectionIgnoresStalePlayerPointers() {
    using nfsmw_drift_asi::player_vehicle_selection::CandidateObservation;
    using nfsmw_drift_asi::player_vehicle_selection::HasCompletedRecoveryProbation;
    using nfsmw_drift_asi::player_vehicle_selection::ReverseOwnershipProvesRetired;
    using nfsmw_drift_asi::player_vehicle_selection::Selection;
    using nfsmw_drift_asi::player_vehicle_selection::kRecoveryProbationSamples;

    Selection selection{};
    selection.Observe(CandidateObservation{
        0x1000u, 0x2000u, 0, false, false, false, 0x102Cu, 0});
    selection.Observe(CandidateObservation{
        0x3000u, 0x4000u, 0x4000u, true, true, true,
        0x302Cu, 0x302Cu});
    selection.Observe(CandidateObservation{
        0x5000u, 0x6000u, 0x6000u, true, true, false,
        0x502Cu, 0x502Cu});
    selection.Observe(CandidateObservation{
        0x7000u, 0x8000u, 0x9000u, true, true, true,
        0x702Cu, 0x702Cu});

    // A retiring vehicle can still report the same player and ownership
    // predicates. The player's current GetSimable result must select only the
    // newly attached vehicle instead of waiting for the old pool slot to die.
    selection.Observe(CandidateObservation{
        0xA000u, 0x4000u, 0x4000u, true, true, true,
        0xA02Cu, 0x302Cu});

    Require(selection.nonNullPlayers == 5 &&
                selection.qualifiedPlayers == 1 &&
                selection.HasUniquePlayer() &&
                selection.pvehicle == 0x3000u &&
                selection.player == 0x4000u,
            "stale player pointers and retiring vehicles must not hide the one vehicle referenced by the player's current simable");

    selection.Observe(CandidateObservation{
        0xB000u, 0xC000u, 0xC000u, true, true, true,
        0xB02Cu, 0xB02Cu});
    Require(selection.qualifiedPlayers == 2 &&
                !selection.HasUniquePlayer(),
            "two reverse-owned player vehicles must still fail the unique-player safety rule");

    Require(kRecoveryProbationSamples == 3 &&
                !HasCompletedRecoveryProbation(0) &&
                !HasCompletedRecoveryProbation(1) &&
                !HasCompletedRecoveryProbation(2) &&
                HasCompletedRecoveryProbation(3) &&
                HasCompletedRecoveryProbation(4),
            "recovery must require exactly three stable samples before writes resume");

    Require(ReverseOwnershipProvesRetired(0xA02Cu, 0x302Cu) &&
                !ReverseOwnershipProvesRetired(0xA02Cu, 0xA02Cu) &&
                !ReverseOwnershipProvesRetired(0xA02Cu, 0) &&
                !ReverseOwnershipProvesRetired(0, 0x302Cu),
            "only a validated non-null reverse pointer to another simable may retire a stale pool slot");
}

void TestPlayerVehicleSelectionDeduplicatesRepeatedSlots() {
    using nfsmw_drift_asi::player_vehicle_selection::CandidateObservation;
    using nfsmw_drift_asi::player_vehicle_selection::ClassifyInstanceSlot;
    using nfsmw_drift_asi::player_vehicle_selection::InstanceSlotState;
    using nfsmw_drift_asi::player_vehicle_selection::Selection;

    Require(ClassifyInstanceSlot(0, 0) == InstanceSlotState::Empty &&
                ClassifyInstanceSlot(0x1000u, 0) ==
                    InstanceSlotState::Disabled &&
                ClassifyInstanceSlot(0x1000u, 1) ==
                    InstanceSlotState::Enabled &&
                ClassifyInstanceSlot(0, 1) == InstanceSlotState::Invalid &&
                ClassifyInstanceSlot(0x1000u, 2) ==
                    InstanceSlotState::Invalid,
            "only canonical enabled slots with a vehicle may participate in player selection");

    const CandidateObservation first{
        0x1000u, 0x2000u, 0x2000u, true, true, true,
        0x102Cu, 0x102Cu};
    const CandidateObservation second{
        0x3000u, 0x4000u, 0x4000u, true, true, true,
        0x302Cu, 0x302Cu};

    Selection selection{};
    selection.Observe(first);
    selection.Observe(second);
    selection.Observe(second);
    selection.Observe(first);

    Require(selection.nonNullPlayers == 4 &&
                selection.qualifiedPlayers == 2 &&
                selection.duplicateQualifiedPlayers == 2 &&
                !selection.HasUniquePlayer(),
            "repeated slots must be deduplicated without hiding two distinct qualified player vehicles");

    Selection repeatedOnly{};
    repeatedOnly.Observe(first);
    repeatedOnly.Observe(first);
    Require(repeatedOnly.nonNullPlayers == 2 &&
                repeatedOnly.qualifiedPlayers == 1 &&
                repeatedOnly.duplicateQualifiedPlayers == 1 &&
                repeatedOnly.HasUniquePlayer() &&
                repeatedOnly.pvehicle == first.pvehicle &&
                repeatedOnly.player == first.reportedPlayer &&
                repeatedOnly.simable == first.simable &&
                repeatedOnly.alternatePvehicle == 0,
            "the same qualified identity in two slots must remain one unique player vehicle");

    Require(selection.pvehicle == first.pvehicle &&
                selection.player == first.reportedPlayer &&
                selection.simable == first.simable &&
                selection.alternatePvehicle == second.pvehicle &&
                selection.alternatePlayer == second.reportedPlayer &&
                selection.alternateSimable == second.simable,
            "selection diagnostics must retain the first two distinct qualified identities");
}

} // namespace

int main() {
    TestImmediateSessionAndEligibility();
    TestMirroredThresholdAndNormalization();
    TestOwnedSteeringDoesNotChatterAtThreshold();
    TestManualPassthroughAndReengagement();
    TestSignedManualDirectionOwnership();
    TestOnlyPostOwnershipInputStartsDelay();
    TestDelayContinuityAndFrameRates();
    TestDelayExpiryBelowThresholdWaitsForAngle();
    TestSafeExitTimeoutAndRearm();
    TestUnsafeStatesFailClosed();
    TestActiveGroundContactLossGraceWindow();
    TestDisabledAndZeroRangeFailClosed();
    TestGripDriveAndHardStops();
    TestConfigAndIni();
    TestManualYawRequirementContract();
    TestManualYawFrameRateAndRampContract();
    TestSideTransitionRequirementContract();
    TestNonManualYawAndNonFiniteSafety();
    TestPlayerVehicleSelectionIgnoresStalePlayerPointers();
    TestPlayerVehicleSelectionDeduplicatesRepeatedSlots();

    if (failures != 0) {
        std::cerr << failures << " requirement test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All drift requirement tests passed\n";
    return EXIT_SUCCESS;
}
