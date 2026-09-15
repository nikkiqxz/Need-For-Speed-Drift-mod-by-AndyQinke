#include "asi_host/steering_response.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using nfsmw_drift_asi::steering_response::AutomaticOverrideAllowed;
using nfsmw_drift_asi::steering_response::ClampCommand;
using nfsmw_drift_asi::steering_response::HoldNeutralAfterManualTakeover;
using nfsmw_drift_asi::steering_response::kManualSteeringMaximumRatePerSecond;
using nfsmw_drift_asi::steering_response::Limiter;
using nfsmw_drift_asi::steering_response::MoveTowards;
using nfsmw_drift_asi::steering_response::PrepareWrite;
using nfsmw_drift_asi::steering_response::PreviousCommandTracker;
using nfsmw_drift_asi::steering_response::ScaledMaximumRate;
using nfsmw_drift_asi::steering_response::SessionLimiter;
using nfsmw_drift_asi::steering_response::WriteMode;
using nfsmw_drift_asi::steering_response::WriteRequest;
using nfsmw_drift_asi::steering_response::WaitingNeutralPassthrough;

constexpr float kManualTransitionSeconds = 1.375f;
constexpr float kAutomaticRatePerSecond = 2.0f;
constexpr float kFrameDt = 1.0f / 60.0f;
constexpr std::uint8_t kNegativeDirection =
    nfsmw_drift_asi::steering_response::kNegativeSteeringDirectionObserved;
constexpr std::uint8_t kPositiveDirection =
    nfsmw_drift_asi::steering_response::kPositiveSteeringDirectionObserved;

int failures = 0;

void Require(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool Near(float actual, float expected, float tolerance = 1.0e-5f) {
    return std::fabs(actual - expected) <= tolerance;
}

WriteRequest ActiveRequest(float rawCommand, std::uint64_t physicsSerial) {
    WriteRequest request{};
    request.sessionActive = true;
    request.rawCommand = rawCommand;
    request.sessionStartCommand = rawCommand;
    request.elapsedSeconds = kFrameDt;
    request.automaticMaximumRatePerSecond = kAutomaticRatePerSecond;
    request.manualTransitionSeconds = kManualTransitionSeconds;
    request.physicsSerial = physicsSerial;
    return request;
}

float TimedStep(float start,
                float target,
                float elapsed,
                float duration) {
    const float progress = std::clamp(elapsed / duration, 0.0f, 1.0f);
    return ClampCommand(start + (target - start) * progress);
}

float ManualRateStep(float start, float target, float elapsed) {
    return ClampCommand(MoveTowards(
        start, target, kManualSteeringMaximumRatePerSecond * elapsed));
}

void TestCommandHelpers() {
    Require(Near(ClampCommand(-2.0f), -1.0f) &&
                Near(ClampCommand(2.0f), 1.0f) &&
                Near(ClampCommand(0.25f), 0.25f),
            "commands must be bounded to the normalized steering range");
    Require(Near(ClampCommand(std::numeric_limits<float>::quiet_NaN()), 0.0f),
            "non-finite commands must fail closed to neutral");
    Require(Near(ScaledMaximumRate(4.0f), 2.0f) &&
                Near(ScaledMaximumRate(0.0f), 0.0f) &&
                Near(ScaledMaximumRate(-2.0f), 0.0f) &&
                Near(ScaledMaximumRate(
                         std::numeric_limits<float>::quiet_NaN()),
                     0.0f),
            "the calibrated drift rate must be exactly half and reject invalid input");

    Require(Near(MoveTowards(-0.25f, 0.25f, 0.1f), -0.15f) &&
                Near(MoveTowards(0.25f, -0.25f, 1.0f), -0.25f),
            "MoveTowards must preserve direction and snap only within its budget");
}

void TestNeutralPollAfterManualTakeoverIsHeld() {
    Require(HoldNeutralAfterManualTakeover(
                false, true, false, true, false) &&
                HoldNeutralAfterManualTakeover(
                    false, false, false, false, true) &&
                HoldNeutralAfterManualTakeover(
                    false, true, true, false, false),
            "a same-frame neutral poll after takeover must hold the applied command");
    Require(!HoldNeutralAfterManualTakeover(
                 true, true, false, true, false) &&
                !HoldNeutralAfterManualTakeover(
                    false, false, false, true, false) &&
                !HoldNeutralAfterManualTakeover(
                    false, true, false, false, false),
            "ordinary manual and passive neutral polls must not be held");

    Require(HoldNeutralAfterManualTakeover(
                false, false, false, false, true),
            "re-engagement must hold even after the direction journal is cleared");

    // Model the live Limiter sequence: automatic countersteer, player
    // takeover, a second neutral poll in the same serial, then re-engagement.
    Limiter limiter;
    limiter.BeginSession(1.0f, 600);
    const float automatic = limiter.Advance(
        -0.9167f, kFrameDt, kAutomaticRatePerSecond, 600);
    const float manual = limiter.AdvanceManual(1.0f, kFrameDt, 601);
    Require(manual > automatic,
            "manual takeover must move from the applied automatic command");
    const float held = limiter.appliedCommand();
    Require(Near(held, manual),
            "the held command must be the value already written to the game");
    // The bridge's hold branch intentionally does not call AdvanceManual
    // for the same-frame neutral sample, so the value cannot drift toward zero.
    Require(Near(limiter.appliedCommand(), held),
            "same-frame neutral handling must preserve the applied command");
    const float resumed = limiter.Advance(
        -0.9167f, kFrameDt, kAutomaticRatePerSecond, 602);
    Require(Near(resumed,
                 MoveTowards(held, -0.9167f,
                             kAutomaticRatePerSecond * kFrameDt)),
            "automatic re-engagement must continue from the held command");
}

void TestWaitingNeutralPassesThroughAndRetiresTrajectory() {
    Require(WaitingNeutralPassthrough(false, false, true),
            "waiting-angle neutral input must pass through");
    Require(!WaitingNeutralPassthrough(true, false, true) &&
                !WaitingNeutralPassthrough(false, true, true) &&
                !WaitingNeutralPassthrough(false, false, false),
            "waiting-angle passthrough must require a genuinely neutral frame");
}

void TestAutomaticOverrideImmediateMatrix() {
    // Automatic ownership is valid only while the current sample and frame
    // journal are neutral, and only for a valid committed drift side.
    Require(AutomaticOverrideAllowed(false, false, static_cast<std::uint8_t>(0),
                                     -1, true, true) &&
                AutomaticOverrideAllowed(false, false,
                                         static_cast<std::uint8_t>(0), 1, true,
                                         true) &&
                !AutomaticOverrideAllowed(false, false,
                                          static_cast<std::uint8_t>(0), 0, true,
                                          true),
            "neutral input must leave automatic countersteer eligible only for a valid side");

    // Every valid player event is an immediate takeover, even when its sign
    // matches the automatic countersteer direction and is intended to add
    // more countersteer.
    Require(!AutomaticOverrideAllowed(true, true, kNegativeDirection, 1,
                                      true, true) &&
                !AutomaticOverrideAllowed(true, true, kPositiveDirection, 1,
                                          true, true) &&
                !AutomaticOverrideAllowed(false, true, kPositiveDirection, -1,
                                          true, true) &&
                !AutomaticOverrideAllowed(false, false, kNegativeDirection, 1,
                                          true, true),
            "any raw or observed steering event must release automatic ownership");
    Require(!AutomaticOverrideAllowed(
                 true, true,
                 static_cast<std::uint8_t>(kNegativeDirection |
                                           kPositiveDirection),
                 1,
                 true, true) &&
                !AutomaticOverrideAllowed(true, true, kNegativeDirection, 0,
                                           true, true),
            "ambiguous and unknown-side observations must veto automation");
    Require(!AutomaticOverrideAllowed(true, true, kNegativeDirection, 1,
                                      false, true) &&
                !AutomaticOverrideAllowed(true, true, kNegativeDirection, 1,
                                          true, false),
            "inactive smart/forced modes must never claim steering ownership");

    // Keep the legacy five-argument adapter contract covered as well. It has no
    // sign information, so a reported direction is conservatively a veto.
    Require(AutomaticOverrideAllowed(false, false, false, true, true),
            "legacy helper must allow a genuinely neutral automatic frame");
    Require(!AutomaticOverrideAllowed(false, false, true, true, true),
            "legacy helper must veto an observed direction without a sign");
}

void TestLimiterAutomaticRatePath() {
    Limiter limiter;
    limiter.BeginSession(-0.75f, 10);

    const float first = limiter.Advance(1.0f, 0.05f,
                                        kAutomaticRatePerSecond, 11);
    Require(Near(first, -0.65f),
            "automatic countersteer must use the 0.9.5 rate path");

    const float repeated = limiter.Advance(1.0f, 0.05f,
                                           kAutomaticRatePerSecond, 11);
    Require(Near(repeated, first),
            "repeated polls in one physics serial must not spend dt twice");

    // A late target update in the same physics frame may replace the target,
    // but it cannot create another command step.
    const float lateTarget = limiter.Advance(-1.0f, 0.05f,
                                              kAutomaticRatePerSecond, 11);
    Require(Near(lateTarget, first),
            "same-serial automatic retargeting must preserve the applied command");

    const float nextFrame = limiter.Advance(-1.0f, 0.05f,
                                             kAutomaticRatePerSecond, 12);
    Require(Near(nextFrame, -0.75f),
            "the next physics serial must spend exactly one fresh automatic step");

    const float invalid = limiter.Advance(0.5f, -1.0f,
                                          kAutomaticRatePerSecond, 13);
    Require(Near(invalid, nextFrame),
            "invalid elapsed time must preserve the last applied command");
    const float afterInvalid = limiter.Advance(0.5f, 0.05f,
                                               kAutomaticRatePerSecond, 13);
    Require(Near(afterInvalid, -0.65f),
            "a rejected sample must not consume the following valid frame budget");
}

void TestAutomaticTakeoverReanchorsAfterManualTransition() {
    Limiter limiter;
    limiter.BeginSession(0.95f, 10);

    // Model the stale trajectory that caused the regression: the player had
    // taken over while the wheel was still on the old positive side.
    const float manual = limiter.AdvanceManual(1.0f, kFrameDt, 11);
    Require(manual > 0.90f,
            "the regression setup must leave a positive manual command applied");

    constexpr float automaticTarget = -0.9167f;
    limiter.SynchronizeAutomaticTakeover(automaticTarget, 12);
    Require(Near(limiter.appliedCommand(), automaticTarget),
            "automatic takeover must anchor the applied command at its target");

    // A repeated poll in the takeover serial must not spend another budget or
    // reconstruct the old manual transition.
    const float repeated = limiter.Advance(
        automaticTarget, kFrameDt, kAutomaticRatePerSecond, 12);
    Require(Near(repeated, automaticTarget),
            "the takeover serial must remain at the synchronized target");

    const float next = limiter.Advance(
        automaticTarget, kFrameDt, kAutomaticRatePerSecond, 13);
    Require(Near(next, automaticTarget),
            "the next automatic frame must continue from the anchored target");
}

void TestLimiterAutomaticFrameRateIndependence() {
    for (const int fps : {30, 60, 120}) {
        Limiter limiter;
        limiter.BeginSession(0.0f, 0);
        const float dt = 1.0f / static_cast<float>(fps);
        float command = 0.0f;
        for (int frame = 1; frame <= fps / 2; ++frame) {
            command = limiter.Advance(1.0f, dt, kAutomaticRatePerSecond,
                                      static_cast<std::uint64_t>(frame));
        }
        Require(Near(command, 1.0f, 3.0e-5f),
                "automatic full travel must take the same 0.5 seconds at every frame rate");
    }
}

void TestManualTransitionStartsFromAppliedCommand() {
    Limiter limiter;
    limiter.BeginSession(0.40f, 0);

    constexpr float target = -1.0f;
    constexpr float dt = 0.125f;
    float command = 0.40f;
    for (std::uint64_t frame = 1; frame <= 10; ++frame) {
        command = limiter.AdvanceOverDuration(
            target, dt, kManualTransitionSeconds, frame);
        const float expected = TimedStep(
            0.40f, target, static_cast<float>(frame) * dt,
            kManualTransitionSeconds);
        Require(Near(command, expected, 3.0e-5f),
                "manual transition must interpolate from the currently applied command");
    }
    Require(command > target && command < -0.85f,
            "manual transition must still be in flight immediately before its duration expires");

    command = limiter.AdvanceOverDuration(
        target, dt, kManualTransitionSeconds, 11);
    Require(Near(command, target, 3.0e-5f),
            "full manual reversal must arrive at exactly 1.375 seconds");
}

void TestManualTransitionDurationIsIndependentOfDistance() {
    Limiter limiter;
    limiter.BeginSession(-0.20f, 0);

    constexpr float target = 0.20f;
    constexpr float dt = 0.125f;
    float command = -0.20f;
    for (std::uint64_t frame = 1; frame <= 10; ++frame) {
        command = limiter.AdvanceOverDuration(
            target, dt, kManualTransitionSeconds, frame);
    }
    Require(command < target && command > 0.15f,
            "a short manual movement must not complete before the configured duration");
    command = limiter.AdvanceOverDuration(
        target, dt, kManualTransitionSeconds, 11);
    Require(Near(command, target, 3.0e-5f),
            "a short manual movement must also use the complete configured duration");
}

void TestManualTransitionSameSerialConsumesOneBudget() {
    Limiter limiter;
    limiter.BeginSession(0.0f, 10);

    const float first = limiter.AdvanceOverDuration(
        1.0f, 0.1f, kManualTransitionSeconds, 11);
    const float repeated = limiter.AdvanceOverDuration(
        1.0f, 0.1f, kManualTransitionSeconds, 11);
    Require(Near(first, 0.1f / kManualTransitionSeconds) &&
                Near(repeated, first),
            "repeated manual polls must not advance the timed transition twice");

    const float retargeted = limiter.AdvanceOverDuration(
        -1.0f, 0.1f, kManualTransitionSeconds, 11);
    Require(Near(retargeted, first),
            "a same-serial manual retarget must start from the applied value without spending dt again");

    const float next = limiter.AdvanceOverDuration(
        -1.0f, 0.1f, kManualTransitionSeconds, 12);
    const float expected = TimedStep(
        first, -1.0f, 0.1f, kManualTransitionSeconds);
    Require(Near(next, expected),
            "the retargeted transition must spend one dt from the prior applied value on the next serial");
}

void TestPrepareWriteAutomaticAndManualOwnership() {
    SessionLimiter limiter;
    auto automatic = ActiveRequest(0.0f, 100);
    automatic.smartCountersteerActive = true;
    automatic.forceCountersteer = true;
    automatic.automaticSide = 1;
    automatic.automaticCommand = -0.75f;

    const auto firstAutomatic = PrepareWrite(&limiter, automatic);
    Require(firstAutomatic.write && firstAutomatic.mode == WriteMode::Automatic &&
                Near(firstAutomatic.command,
                     -kAutomaticRatePerSecond * kFrameDt),
            "eligible automatic steering must write one bounded 0.9.5 step");

    const auto repeatedAutomatic = PrepareWrite(&limiter, automatic);
    Require(Near(repeatedAutomatic.command, firstAutomatic.command),
            "repeated automatic input polls must be idempotent within a serial");

    // Positive input is opposite the side +1 mapping and therefore hands the
    // wheel to the player. The first manual value must begin at the command
    // that was actually written by the automatic path.
    auto manual = automatic;
    manual.physicsSerial = 101;
    manual.rawCommand = 1.0f;
    manual.currentManualInput = true;
    manual.manualInputObserved = true;
    manual.manualDirectionObserved = true;
    manual.manualDirectionMask = kPositiveDirection;
    const auto manualResult = PrepareWrite(&limiter, manual);
    const float expectedManual = ManualRateStep(
        firstAutomatic.command, manual.rawCommand, kFrameDt);
    Require(manualResult.write && manualResult.mode == WriteMode::Manual &&
                Near(manualResult.command, expectedManual),
            "manual takeover must slew from the last automatic applied command at the restored fixed rate");

    const auto repeatedManual = PrepareWrite(&limiter, manual);
    Require(Near(repeatedManual.command, manualResult.command),
            "repeated manual polls must not consume another transition budget");
}

void TestPrepareWriteSameSerialRetargetPreservesAppliedCommand() {
    SessionLimiter limiter;
    auto request = ActiveRequest(0.0f, 150);
    request.smartCountersteerActive = true;
    request.forceCountersteer = true;
    request.automaticSide = 1;
    request.automaticCommand = -0.75f;

    const auto first = PrepareWrite(&limiter, request);
    Require(first.mode == WriteMode::Automatic && first.write,
            "the regression setup must acquire automatic countersteer");

    // A late poll in the same physics serial can observe a newly selected
    // target. It must not rewind to serialStartCommand_ and spend another dt.
    request.automaticCommand = 0.75f;
    const auto lateRetarget = PrepareWrite(&limiter, request);
    Require(lateRetarget.mode == WriteMode::Automatic && lateRetarget.write &&
                Near(lateRetarget.command, first.command),
            "same-serial automatic retargeting must preserve the command already written");

    // The next serial may advance once, but only from the last command that
    // was actually written, never from the frame's original session command.
    request.physicsSerial = 151;
    const auto next = PrepareWrite(&limiter, request);
    const float expected = MoveTowards(
        first.command, request.automaticCommand,
        kAutomaticRatePerSecond * kFrameDt);
    Require(next.mode == WriteMode::Automatic && next.write &&
                Near(next.command, expected),
            "the next serial must take one automatic step from the prior applied command");
}

void TestPrepareWriteSameSerialModeChangePreservesAppliedCommand() {
    SessionLimiter limiter;
    auto request = ActiveRequest(0.0f, 160);
    request.smartCountersteerActive = true;
    request.forceCountersteer = true;
    request.automaticSide = 1;
    request.automaticCommand = -0.75f;

    const auto automatic = PrepareWrite(&limiter, request);
    Require(automatic.mode == WriteMode::Automatic && automatic.write,
            "the mode-change regression setup must acquire automatic countersteer");

    // A same-serial opposite-direction event changes ownership mode, but it
    // still cannot reopen the command budget or rewind the visible command.
    request.rawCommand = 1.0f;
    request.currentManualInput = true;
    request.manualInputObserved = true;
    request.manualDirectionObserved = true;
    request.manualDirectionMask = kPositiveDirection;
    const auto sameSerialManual = PrepareWrite(&limiter, request);
    Require(sameSerialManual.mode == WriteMode::Manual &&
                sameSerialManual.write &&
                Near(sameSerialManual.command, automatic.command),
            "same-serial automatic-to-manual handoff must not spend a second dt");

    request.physicsSerial = 161;
    const auto nextManual = PrepareWrite(&limiter, request);
    const float expected = ManualRateStep(
        automatic.command, request.rawCommand, kFrameDt);
    Require(nextManual.mode == WriteMode::Manual && nextManual.write &&
                Near(nextManual.command, expected),
            "the next manual serial must take one fixed-rate step from the last applied command");
}

void TestPrepareWriteImmediateManualTakeover() {
    struct ManualInput {
        float command;
        std::uint8_t directionMask;
    };
    constexpr ManualInput kInputs[] = {
        {-1.0f, kNegativeDirection},
        {1.0f, kPositiveDirection},
    };

    for (const int side : {-1, 1}) {
        for (const auto& input : kInputs) {
            SessionLimiter limiter;
            auto request = ActiveRequest(
                0.0f, 200 + static_cast<std::uint64_t>(side + 1));
            request.smartCountersteerActive = true;
            request.forceCountersteer = true;
            request.automaticSide = side;
            request.automaticCommand = side == 1 ? -0.75f : 0.75f;

            const auto automatic = PrepareWrite(&limiter, request);
            Require(automatic.mode == WriteMode::Automatic,
                    "a neutral frame must acquire automatic countersteer");

            request.physicsSerial += 1;
            request.rawCommand = input.command;
            request.currentManualInput = true;
            request.manualInputObserved = true;
            request.manualDirectionObserved = true;
            request.manualDirectionMask = input.directionMask;
            const auto manual = PrepareWrite(&limiter, request);
            const float expected = ManualRateStep(
                automatic.command, input.command, kFrameDt);
            Require(manual.mode == WriteMode::Manual && manual.write &&
                        Near(manual.command, expected),
                    "either steering direction must take ownership and slew from the applied command");
        }
    }
}

void TestManualCountersteerIncreaseAndPendulumReversalRates() {
    constexpr float automaticCountersteer = -55.0f / 60.0f;

    // A right-hand drift already held at 55 degrees left must hand a further
    // left input to the player immediately. Only the remaining five degrees
    // are rate-limited, so it finishes in about 0.042 seconds rather than
    // consuming the old full 1.375-second transition.
    SessionLimiter countersteerLimiter;
    auto request = ActiveRequest(0.0f, 700);
    request.sessionStartCommand = automaticCountersteer;
    request.smartCountersteerActive = true;
    request.forceCountersteer = true;
    request.automaticSide = 1;
    request.automaticCommand = automaticCountersteer;
    const auto automatic = PrepareWrite(&countersteerLimiter, request);
    Require(automatic.mode == WriteMode::Automatic &&
                Near(automatic.command, automaticCountersteer),
            "the regression setup must hold the existing 55-degree countersteer");

    request.rawCommand = -1.0f;
    request.currentManualInput = true;
    request.manualInputObserved = true;
    request.manualDirectionObserved = true;
    request.manualDirectionMask = kNegativeDirection;
    request.physicsSerial = 701;
    auto increased = PrepareWrite(&countersteerLimiter, request);
    Require(increased.mode == WriteMode::Manual &&
                Near(increased.command,
                     ManualRateStep(automaticCountersteer, -1.0f, kFrameDt)),
            "same-direction countersteer input must take ownership on its first frame");
    request.physicsSerial = 702;
    increased = PrepareWrite(&countersteerLimiter, request);
    request.physicsSerial = 703;
    increased = PrepareWrite(&countersteerLimiter, request);
    Require(Near(increased.command, -1.0f),
            "the remaining five degrees of manual countersteer must finish within three frames");

    // A full pendulum reversal retains smoothing but progresses by distance:
    // two normalized command units at 2 command/s take exactly one second.
    for (const int fps : {30, 60, 120}) {
        Limiter pendulumLimiter;
        pendulumLimiter.BeginSession(-1.0f, 800);
        const float frameDt = 1.0f / static_cast<float>(fps);
        float reversed = -1.0f;
        for (int frame = 1; frame <= fps; ++frame) {
            reversed = pendulumLimiter.AdvanceManual(
                1.0f, frameDt, 800 + static_cast<std::uint64_t>(frame));
        }
        Require(Near(reversed, 1.0f, 3.0e-5f),
                "a full manual pendulum reversal must complete in one second at every frame rate");
    }
}

void TestPrepareWriteNeutralHoldsLastAppliedCommand() {
    SessionLimiter limiter;
    auto request = ActiveRequest(0.0f, 300);
    request.smartCountersteerActive = true;
    request.forceCountersteer = true;
    request.automaticSide = 1;
    request.automaticCommand = -0.75f;
    auto automatic = PrepareWrite(&limiter, request);
    request.physicsSerial = 301;
    automatic = PrepareWrite(&limiter, request);

    // First perform a manual takeover so the value held during re-engagement
    // is an observable non-neutral command rather than the initial automatic
    // sample.
    request.physicsSerial = 302;
    request.rawCommand = 1.0f;
    request.currentManualInput = true;
    request.manualInputObserved = true;
    request.manualDirectionObserved = true;
    request.manualDirectionMask = kPositiveDirection;
    const auto manual = PrepareWrite(&limiter, request);
    Require(manual.write && manual.mode == WriteMode::Manual,
            "re-engagement setup must first establish a manual applied command");
    const float heldCommand = manual.command;

    // Disable smart/force for the neutral re-engagement frame, as the
    // controller does while it is waiting to reacquire automatic countersteer.
    request.smartCountersteerActive = false;
    request.forceCountersteer = false;
    request.rawCommand = 0.0f;
    request.holdNeutralCommand = true;
    request.currentManualInput = false;
    request.manualInputObserved = true;
    request.manualDirectionObserved = true;
    const auto neutral = PrepareWrite(&limiter, request);
    Require(neutral.write && neutral.mode == WriteMode::Manual &&
                Near(neutral.command, heldCommand),
            "neutral re-engagement must hold the last applied command after manual takeover");

    request.manualInputObserved = false;
    request.manualDirectionObserved = false;
    request.manualDirectionMask = 0;
    for (std::uint64_t serial = 303; serial <= 306; ++serial) {
        request.physicsSerial = serial;
        const auto neutralNext = PrepareWrite(&limiter, request);
        Require(neutralNext.write && neutralNext.mode == WriteMode::Manual &&
                    Near(neutralNext.command, heldCommand),
                "holding re-engagement across physics frames must not drift toward raw zero");
    }

    request.physicsSerial = 307;
    request.holdNeutralCommand = false;
    request.smartCountersteerActive = true;
    request.forceCountersteer = true;
    request.automaticCommand = -0.75f;
    const auto reacquired = PrepareWrite(&limiter, request);
    const float expected = MoveTowards(
        heldCommand, request.automaticCommand,
        kAutomaticRatePerSecond * kFrameDt);
    Require(reacquired.mode == WriteMode::Automatic &&
                Near(reacquired.command, expected),
            "automatic re-engagement must continue from the held command without a zero crossing");
}

void TestPrepareWriteSessionReset() {
    SessionLimiter limiter;
    auto request = ActiveRequest(0.8f, 400);
    request.smartCountersteerActive = true;
    request.forceCountersteer = true;
    request.automaticSide = 1;
    request.automaticCommand = -0.75f;
    (void)PrepareWrite(&limiter, request);
    Require(limiter.valid(), "an active write must initialize the limiter session");

    request.sessionActive = false;
    request.rawCommand = -0.35f;
    const auto inactive = PrepareWrite(&limiter, request);
    Require(!inactive.write && inactive.mode == WriteMode::Passthrough &&
                Near(inactive.command, -0.35f) && !limiter.valid(),
            "leaving drift mode must reset the limiter and pass raw steering through");

    request.sessionActive = true;
    request.physicsSerial = 1;
    request.sessionStartCommand = 0.25f;
    request.rawCommand = 0.25f;
    request.smartCountersteerActive = false;
    request.forceCountersteer = false;
    const auto restarted = PrepareWrite(&limiter, request);
    Require(!restarted.write && restarted.mode == WriteMode::Passthrough &&
                Near(restarted.command, 0.25f),
            "a new session must pass its trusted neutral sample through without stale trajectory");
}

void TestPrepareWriteWaitingAngleDoesNotSlewToZero() {
    SessionLimiter limiter;
    auto request = ActiveRequest(0.0f, 500);
    request.smartCountersteerActive = true;
    request.forceCountersteer = true;
    request.automaticSide = 1;
    request.automaticCommand = -0.75f;
    const auto firstAutomatic = PrepareWrite(&limiter, request);
    request.physicsSerial = 501;
    const auto secondAutomatic = PrepareWrite(&limiter, request);
    Require(secondAutomatic.mode == WriteMode::Automatic &&
                secondAutomatic.command < firstAutomatic.command,
            "the live session limiter must advance the restored automatic path");

    // The controller can fall back below the 15-degree acquisition angle while
    // the drift session remains active.  This is a true pass-through frame:
    // it must not consume a manual fixed-rate step.
    request.physicsSerial = 502;
    request.smartCountersteerActive = false;
    request.forceCountersteer = false;
    request.rawCommand = 0.0f;
    const auto waiting = PrepareWrite(&limiter, request);
    Require(!waiting.write && waiting.mode == WriteMode::Passthrough &&
                Near(waiting.command, 0.0f),
            "waiting-angle neutral input must pass through instead of slewing the wheel to zero");

    request.physicsSerial = 503;
    request.smartCountersteerActive = true;
    request.forceCountersteer = true;
    const auto reacquired = PrepareWrite(&limiter, request);
    Require(reacquired.mode == WriteMode::Automatic &&
                Near(reacquired.command,
                     -kAutomaticRatePerSecond * kFrameDt),
            "automatic re-acquisition must start from the visible pass-through command");
}

void TestPreviousCommandTracker() {
    PreviousCommandTracker history;
    history.Observe(0.0f, 10);
    history.Observe(1.0f, 11);
    Require(Near(history.PreviousCommand(11, -1.0f), 0.0f),
            "the first command in a physics serial must use the previous serial value");

    // Additional polls in the same serial update only the current sample.
    history.Observe(-1.0f, 11);
    Require(Near(history.PreviousCommand(11, -1.0f), 0.0f),
            "repeated input polls must not overwrite previous-frame history");

    history.Observe(0.5f, 12);
    Require(Near(history.PreviousCommand(12, -1.0f), -1.0f),
            "a new physics serial must roll current history forward exactly once");
    history.Reset();
    Require(Near(history.PreviousCommand(12, 0.25f), 0.25f),
            "reset must discard command history across a vehicle or cadence change");
}

}  // namespace

int main() {
    TestCommandHelpers();
    TestNeutralPollAfterManualTakeoverIsHeld();
    TestWaitingNeutralPassesThroughAndRetiresTrajectory();
    TestAutomaticOverrideImmediateMatrix();
    TestLimiterAutomaticRatePath();
    TestAutomaticTakeoverReanchorsAfterManualTransition();
    TestLimiterAutomaticFrameRateIndependence();
    TestManualTransitionStartsFromAppliedCommand();
    TestManualTransitionDurationIsIndependentOfDistance();
    TestManualTransitionSameSerialConsumesOneBudget();
    TestPrepareWriteAutomaticAndManualOwnership();
    TestPrepareWriteSameSerialRetargetPreservesAppliedCommand();
    TestPrepareWriteSameSerialModeChangePreservesAppliedCommand();
    TestPrepareWriteImmediateManualTakeover();
    TestManualCountersteerIncreaseAndPendulumReversalRates();
    TestPrepareWriteNeutralHoldsLastAppliedCommand();
    TestPrepareWriteSessionReset();
    TestPrepareWriteWaitingAngleDoesNotSlewToZero();
    TestPreviousCommandTracker();

    if (failures != 0) {
        std::cerr << failures << " steering response assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All steering response tests passed\n";
    return EXIT_SUCCESS;
}
