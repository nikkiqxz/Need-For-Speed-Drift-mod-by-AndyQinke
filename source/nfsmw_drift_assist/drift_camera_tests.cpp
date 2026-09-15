#include "asi_host/drift_camera.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void Require(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool Near(float actual, float expected, float tolerance = 1.0e-4f) {
    return std::fabs(actual - expected) <= tolerance;
}

float AdvanceForExactSecondsAtFrameRate(
    nfsmw_drift_asi::drift_camera::CameraTransitionState& state,
    float targetDegrees,
    float seconds,
    int fps) {
    using nfsmw_drift_asi::drift_camera::AdvanceAngleDegrees;
    const float dt = 1.0f / static_cast<float>(fps);
    const int wholeFrames = static_cast<int>(
        std::floor(static_cast<double>(seconds) *
                   static_cast<double>(fps)));
    float angle = state.currentDegrees;
    for (int frame = 0; frame < wholeFrames; ++frame) {
        angle = AdvanceAngleDegrees(state, targetDegrees, dt);
    }
    const float wholeFrameSeconds =
        static_cast<float>(wholeFrames) / static_cast<float>(fps);
    const float remainder = seconds - wholeFrameSeconds;
    if (remainder > 0.0f) {
        angle = AdvanceAngleDegrees(state, targetDegrees, remainder);
    }
    return angle;
}

void TestTransitionDurationsAreExplicitAndTargetSpecific() {
    using nfsmw_drift_asi::drift_camera::kFastTransitionDurationSeconds;
    using nfsmw_drift_asi::drift_camera::kLockedAngleDegrees;
    using nfsmw_drift_asi::drift_camera::kTransitionDurationSeconds;
    using nfsmw_drift_asi::drift_camera::TransitionDurationSeconds;

    Require(Near(kLockedAngleDegrees, 3.0f) &&
                Near(kTransitionDurationSeconds, 2.0f) &&
                Near(kFastTransitionDurationSeconds, 1.25f),
            "camera angle and transition durations must match the fixed tuning");
    Require(Near(TransitionDurationSeconds(0.0f, 3.0f), 2.0f) &&
                Near(TransitionDurationSeconds(0.0f, -3.0f), 2.0f),
            "a new entry from center must use the two-second duration");
    Require(Near(TransitionDurationSeconds(3.0f, 0.0f), 1.25f) &&
                Near(TransitionDurationSeconds(-3.0f, 0.0f), 1.25f),
            "returning to center must use the explicit 1.25-second duration");
    Require(Near(TransitionDurationSeconds(3.0f, -3.0f), 1.25f) &&
                Near(TransitionDurationSeconds(-3.0f, 3.0f), 1.25f),
            "switching active sides must use the explicit 1.25-second duration");
}

void TestTargetsFollowCommittedDriftSide() {
    using nfsmw_drift_asi::drift_camera::TargetAngleDegrees;
    Require(Near(TargetAngleDegrees(true, -1), 3.0f),
            "left drift must request the opposite camera-orbit sign");
    Require(Near(TargetAngleDegrees(true, 1), -3.0f),
            "right drift must request the opposite camera-orbit sign");
    Require(Near(TargetAngleDegrees(false, -1), 0.0f) &&
                Near(TargetAngleDegrees(false, 1), 0.0f) &&
                Near(TargetAngleDegrees(true, 0), 0.0f),
            "inactive or uncommitted drift state must center the camera");
}

void TestCenterToSideTakesTwoSecondsAtCommonFrameRates() {
    using nfsmw_drift_asi::drift_camera::CameraTransitionState;
    for (const float target : {-3.0f, 3.0f}) {
        for (const int fps : {30, 60, 120}) {
            CameraTransitionState state{};
            const float halfway = AdvanceForExactSecondsAtFrameRate(
                state, target, 1.0f, fps);
            Require(Near(halfway, target * 0.5f, 2.0e-3f),
                    "a center-to-side transition must be halfway after one second");
            const float angle = AdvanceForExactSecondsAtFrameRate(
                state, target, 1.0f, fps);
            Require(Near(angle, target, 2.0e-3f),
                    "a center-to-side transition must finish after two seconds");
        }
    }
}

void TestReturnToCenterTakesOnePointTwoFiveSeconds() {
    using nfsmw_drift_asi::drift_camera::AdvanceAngleDegrees;
    using nfsmw_drift_asi::drift_camera::CameraTransitionState;
    for (const float start : {-3.0f, 3.0f}) {
        for (const int fps : {30, 60, 120}) {
            CameraTransitionState state{};
            AdvanceAngleDegrees(state, start, 2.0f);
            const float atRetarget =
                AdvanceAngleDegrees(state, 0.0f, 0.0f);
            Require(Near(atRetarget, start) &&
                        Near(state.durationSeconds, 1.25f),
                    "returning to center must retarget without jumping and use 1.25 seconds");
            const float halfway = AdvanceForExactSecondsAtFrameRate(
                state, 0.0f, 0.625f, fps);
            Require(Near(halfway, start * 0.5f, 2.0e-3f),
                    "a return-to-center transition must be halfway after 0.625 seconds");
            const float angle = AdvanceForExactSecondsAtFrameRate(
                state, 0.0f, 0.625f, fps);
            Require(Near(angle, 0.0f, 2.0e-3f),
                    "a return-to-center transition must finish after 1.25 seconds");
        }
    }
}

void TestPendulumSideChangeTakesOnePointTwoFiveSeconds() {
    using nfsmw_drift_asi::drift_camera::AdvanceAngleDegrees;
    using nfsmw_drift_asi::drift_camera::CameraTransitionState;
    for (const float start : {-3.0f, 3.0f}) {
        for (const int fps : {30, 60, 120}) {
            CameraTransitionState state{};
            AdvanceAngleDegrees(state, start, 2.0f);
            const float target = -start;
            const float atRetarget =
                AdvanceAngleDegrees(state, target, 0.0f);
            Require(Near(atRetarget, start) &&
                        Near(state.durationSeconds, 1.25f),
                    "switching sides must retarget without jumping and use 1.25 seconds");
            const float halfway = AdvanceForExactSecondsAtFrameRate(
                state, target, 0.625f, fps);
            Require(Near(halfway, 0.0f, 3.0e-3f),
                    "a side-to-side transition must cross center after 0.625 seconds");
            const float angle = AdvanceForExactSecondsAtFrameRate(
                state, target, 0.625f, fps);
            Require(Near(angle, target, 3.0e-3f),
                    "a side-to-side transition must finish after 1.25 seconds");
        }
    }
}

void TestSmoothstepEasesInAndOut() {
    using nfsmw_drift_asi::drift_camera::AdvanceAngleDegrees;
    using nfsmw_drift_asi::drift_camera::CameraTransitionState;
    CameraTransitionState state{};
    const float quarter = AdvanceAngleDegrees(state, 3.0f, 0.5f);
    const float halfway = AdvanceAngleDegrees(state, 3.0f, 0.5f);
    const float threeQuarters = AdvanceAngleDegrees(state, 3.0f, 0.5f);
    const float finished = AdvanceAngleDegrees(state, 3.0f, 0.5f);

    Require(Near(quarter, 0.46875f) && Near(halfway, 1.5f) &&
                Near(threeQuarters, 2.53125f) && Near(finished, 3.0f),
            "the two-second entry must follow cubic smoothstep checkpoints");
    Require((quarter - 0.0f) < (halfway - quarter) &&
                (finished - threeQuarters) <
                    (threeQuarters - halfway),
            "camera angular speed must ease in and out");

    const float fastQuarter =
        AdvanceAngleDegrees(state, -3.0f, 0.3125f);
    const float fastHalfway =
        AdvanceAngleDegrees(state, -3.0f, 0.3125f);
    const float fastThreeQuarters =
        AdvanceAngleDegrees(state, -3.0f, 0.3125f);
    const float fastFinished =
        AdvanceAngleDegrees(state, -3.0f, 0.3125f);
    Require(Near(fastQuarter, 2.0625f) && Near(fastHalfway, 0.0f) &&
                Near(fastThreeQuarters, -2.0625f) &&
                Near(fastFinished, -3.0f),
            "the 1.25-second side change must preserve cubic smoothstep easing");
}

void TestFastTransitionsFinishOnFirstFrameAtOrAfterDeadline() {
    using nfsmw_drift_asi::drift_camera::AdvanceAngleDegrees;
    using nfsmw_drift_asi::drift_camera::CameraTransitionState;
    using nfsmw_drift_asi::drift_camera::kFastTransitionDurationSeconds;
    for (const float target : {0.0f, -3.0f}) {
        for (const int fps : {30, 60, 120}) {
            CameraTransitionState state{};
            AdvanceAngleDegrees(state, 3.0f, 2.0f);
            AdvanceAngleDegrees(state, target, 0.0f);

            const float dt = 1.0f / static_cast<float>(fps);
            const int completionFrame = static_cast<int>(std::ceil(
                static_cast<double>(kFastTransitionDurationSeconds) *
                static_cast<double>(fps)));
            float angle = state.currentDegrees;
            for (int frame = 0; frame < completionFrame - 1; ++frame) {
                angle = AdvanceAngleDegrees(state, target, dt);
            }
            Require(state.elapsedSeconds < state.durationSeconds,
                    "a fast transition must remain active before its 1.25-second deadline");
            angle = AdvanceAngleDegrees(state, target, dt);
            Require(Near(state.elapsedSeconds, 1.25f) &&
                        Near(angle, target),
                    "a fast transition must finish on the first frame at or after 1.25 seconds");
        }
    }
}

void TestMidEntrySideRetargetUsesFastDurationWithoutJumping() {
    using nfsmw_drift_asi::drift_camera::AdvanceAngleDegrees;
    using nfsmw_drift_asi::drift_camera::CameraTransitionState;
    for (const float entryTarget : {-3.0f, 3.0f}) {
        for (const int fps : {30, 60, 120}) {
            CameraTransitionState state{};
            const float dt = 1.0f / static_cast<float>(fps);
            float beforeRetarget = 0.0f;
            for (int frame = 0; frame < fps; ++frame) {
                beforeRetarget =
                    AdvanceAngleDegrees(state, entryTarget, dt);
            }

            const float target = -entryTarget;
            const float atRetarget =
                AdvanceAngleDegrees(state, target, 0.0f);
            Require(Near(atRetarget, beforeRetarget),
                    "changing sides mid-entry must not jump the displayed angle");
            Require(Near(state.durationSeconds, 1.25f),
                    "changing active sides mid-entry must use 1.25 seconds");

            const float halfway = AdvanceForExactSecondsAtFrameRate(
                state, target, 0.625f, fps);
            Require(Near(halfway,
                         (beforeRetarget + target) * 0.5f,
                         3.0e-3f),
                    "a side retarget must start from the displayed angle");
            const float angle = AdvanceForExactSecondsAtFrameRate(
                state, target, 0.625f, fps);
            Require(Near(angle, target, 3.0e-3f),
                    "a mid-entry side retarget must finish after 1.25 seconds");
        }
    }
}

void TestMidEntryCenterRetargetUsesFastDurationWithoutJumping() {
    using nfsmw_drift_asi::drift_camera::AdvanceAngleDegrees;
    using nfsmw_drift_asi::drift_camera::CameraTransitionState;
    for (const float entryTarget : {-3.0f, 3.0f}) {
        for (const int fps : {30, 60, 120}) {
            CameraTransitionState state{};
            const float dt = 1.0f / static_cast<float>(fps);
            float beforeRetarget = 0.0f;
            for (int frame = 0; frame < fps; ++frame) {
                beforeRetarget =
                    AdvanceAngleDegrees(state, entryTarget, dt);
            }

            const float atRetarget =
                AdvanceAngleDegrees(state, 0.0f, 0.0f);
            Require(Near(atRetarget, beforeRetarget),
                    "centering mid-entry must not jump the displayed angle");
            Require(Near(state.durationSeconds, 1.25f),
                    "centering mid-entry must use 1.25 seconds");

            const float halfway = AdvanceForExactSecondsAtFrameRate(
                state, 0.0f, 0.625f, fps);
            Require(Near(halfway, beforeRetarget * 0.5f, 3.0e-3f),
                    "a center retarget must start from the displayed angle");
            const float angle = AdvanceForExactSecondsAtFrameRate(
                state, 0.0f, 0.625f, fps);
            Require(Near(angle, 0.0f, 3.0e-3f),
                    "a mid-entry center retarget must finish after 1.25 seconds");
        }
    }
}

void TestUnchangedTargetDoesNotRestartTransition() {
    using nfsmw_drift_asi::drift_camera::AdvanceAngleDegrees;
    using nfsmw_drift_asi::drift_camera::CameraTransitionState;
    CameraTransitionState state{};

    const float halfway =
        AdvanceAngleDegrees(state, 3.0f, 1.0f);
    const float finished =
        AdvanceAngleDegrees(state, 3.0f, 1.0f);

    Require(Near(halfway, 1.5f) && Near(finished, 3.0f),
            "publishing an unchanged target must not restart its transition clock");
}

void TestNewEntryAfterCenterRetargetUsesTwoSeconds() {
    using nfsmw_drift_asi::drift_camera::AdvanceAngleDegrees;
    using nfsmw_drift_asi::drift_camera::CameraTransitionState;
    CameraTransitionState state{};

    AdvanceAngleDegrees(state, 3.0f, 2.0f);
    AdvanceAngleDegrees(state, 0.0f, 0.6f);
    const float beforeNewEntry = state.currentDegrees;
    const float atRetarget = AdvanceAngleDegrees(state, -3.0f, 0.0f);
    const float halfway = AdvanceAngleDegrees(state, -3.0f, 1.0f);
    const float finished = AdvanceAngleDegrees(state, -3.0f, 1.0f);

    Require(Near(atRetarget, beforeNewEntry),
            "a new drift entry during return-to-center must not jump the camera");
    Require(Near(state.durationSeconds, 2.0f) &&
                Near(halfway, (beforeNewEntry - 3.0f) * 0.5f,
                     2.0e-3f) &&
                Near(finished, -3.0f, 2.0e-3f),
            "a new entry after a center target must use the normal two-second duration");
}

void TestLongCallbackGapCannotJumpTheCamera() {
    using nfsmw_drift_asi::drift_camera::AdvanceAngleDegrees;
    using nfsmw_drift_asi::drift_camera::CameraTransitionState;
    using nfsmw_drift_asi::drift_camera::VisibleFrameStepSeconds;
    CameraTransitionState state{};
    const float frameStep = VisibleFrameStepSeconds(10.0f);
    const float angle = AdvanceAngleDegrees(state, 3.0f, frameStep);
    Require(Near(frameStep, 0.10f) && angle > 0.0f && angle < 0.10f,
            "a long loading or camera-callback gap must resume with one bounded visible step");
    Require(Near(VisibleFrameStepSeconds(-1.0f), 0.0f),
            "a negative camera interval must not advance the transition");
}

void TestHorizontalRotationMatchesOrbitCameraConvention() {
    using nfsmw_drift_asi::drift_camera::RotateHorizontalFrom;
    // The verified game LookAt call supplies a Z-up vector. A rear camera is
    // therefore orbited in X/Y while its world height remains unchanged.
    const nfsmw_drift::Vec3 from{0.0f, -10.0f, 4.0f};
    const nfsmw_drift::Vec3 to{0.0f, 0.0f, 1.0f};
    const nfsmw_drift::Vec3 leftView =
        RotateHorizontalFrom(from, to, 90.0f);
    const nfsmw_drift::Vec3 rightView =
        RotateHorizontalFrom(from, to, -90.0f);
    Require(Near(leftView.x, 10.0f) && Near(leftView.y, 0.0f) &&
                Near(leftView.z, 4.0f),
            "positive offset must orbit the eye right in X/Y so the view turns left");
    Require(Near(rightView.x, -10.0f) && Near(rightView.y, 0.0f) &&
                Near(rightView.z, 4.0f),
            "negative offset must mirror the Z-up horizontal orbit while preserving height");

    const nfsmw_drift::Vec3 lockedLeft =
        RotateHorizontalFrom(from, to, 3.0f);
    const nfsmw_drift::Vec3 lockedRight =
        RotateHorizontalFrom(from, to, -3.0f);
    const float originalRadiusSquared = 100.0f;
    Require(Near(lockedLeft.z, from.z) && Near(lockedRight.z, from.z) &&
                Near(lockedLeft.x * lockedLeft.x +
                         lockedLeft.y * lockedLeft.y,
                     originalRadiusSquared, 2.0e-3f) &&
                Near(lockedRight.x * lockedRight.x +
                         lockedRight.y * lockedRight.y,
                     originalRadiusSquared, 2.0e-3f),
            "the three-degree Z-up orbit must preserve height and horizontal radius");
    Require(lockedLeft.x > 0.0f && lockedRight.x < 0.0f &&
                Near(lockedLeft.x, -lockedRight.x) &&
                Near(lockedLeft.y, lockedRight.y),
            "the locked-angle Z-up orbit must mirror cleanly across drift sides");
}

}  // namespace

int main() {
    TestTransitionDurationsAreExplicitAndTargetSpecific();
    TestTargetsFollowCommittedDriftSide();
    TestCenterToSideTakesTwoSecondsAtCommonFrameRates();
    TestReturnToCenterTakesOnePointTwoFiveSeconds();
    TestPendulumSideChangeTakesOnePointTwoFiveSeconds();
    TestSmoothstepEasesInAndOut();
    TestFastTransitionsFinishOnFirstFrameAtOrAfterDeadline();
    TestMidEntrySideRetargetUsesFastDurationWithoutJumping();
    TestMidEntryCenterRetargetUsesFastDurationWithoutJumping();
    TestUnchangedTargetDoesNotRestartTransition();
    TestNewEntryAfterCenterRetargetUsesTwoSeconds();
    TestLongCallbackGapCannotJumpTheCamera();
    TestHorizontalRotationMatchesOrbitCameraConvention();
    if (failures != 0) {
        std::cerr << failures << " drift camera assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All drift camera tests passed\n";
    return EXIT_SUCCESS;
}
