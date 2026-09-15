#pragma once

#include "drift_assist.hpp"

#include <algorithm>
#include <cmath>

namespace nfsmw_drift_asi::drift_camera {

constexpr float kLockedAngleDegrees = 3.0f;
constexpr float kTransitionDurationSeconds = 2.0f;
constexpr float kFastTransitionDurationSeconds = 1.25f;
constexpr float kMaximumVisibleFrameStepSeconds = 0.10f;

struct CameraTransitionState {
    float startDegrees = 0.0f;
    float currentDegrees = 0.0f;
    float targetDegrees = 0.0f;
    float elapsedSeconds = 0.0f;
    float durationSeconds = kTransitionDurationSeconds;
};

inline float VisibleFrameStepSeconds(float elapsedSeconds) {
    return std::isfinite(elapsedSeconds)
               ? nfsmw_drift::Clamp(
                     elapsedSeconds, 0.0f,
                     kMaximumVisibleFrameStepSeconds)
               : 0.0f;
}

inline float TargetAngleDegrees(bool active, int driftSide) {
    return active && (driftSide == -1 || driftSide == 1)
               ? -static_cast<float>(driftSide) * kLockedAngleDegrees
               : 0.0f;
}

inline float TransitionDurationSeconds(float previousTargetDegrees,
                                       float targetDegrees) {
    const bool switchesSides =
        (previousTargetDegrees < 0.0f && targetDegrees > 0.0f) ||
        (previousTargetDegrees > 0.0f && targetDegrees < 0.0f);
    return targetDegrees == 0.0f || switchesSides
               ? kFastTransitionDurationSeconds
               : kTransitionDurationSeconds;
}

inline float AdvanceAngleDegrees(CameraTransitionState& state,
                                 float targetDegrees,
                                 float elapsedSeconds) {
    if (!std::isfinite(state.startDegrees) ||
        !std::isfinite(state.currentDegrees) ||
        !std::isfinite(state.targetDegrees) ||
        !std::isfinite(state.elapsedSeconds) ||
        !std::isfinite(state.durationSeconds) ||
        state.durationSeconds <= 0.0f ||
        !std::isfinite(targetDegrees) || !std::isfinite(elapsedSeconds)) {
        state = CameraTransitionState{};
        return 0.0f;
    }

    if (targetDegrees != state.targetDegrees) {
        state.startDegrees = state.currentDegrees;
        state.durationSeconds =
            TransitionDurationSeconds(state.targetDegrees, targetDegrees);
        state.targetDegrees = targetDegrees;
        state.elapsedSeconds = 0.0f;
    }

    state.elapsedSeconds = std::min(
        state.elapsedSeconds + std::max(elapsedSeconds, 0.0f),
        state.durationSeconds);
    const float progress = nfsmw_drift::Clamp(
        state.elapsedSeconds / state.durationSeconds, 0.0f, 1.0f);
    const float easedProgress =
        progress * progress * (3.0f - 2.0f * progress);
    state.currentDegrees =
        state.startDegrees +
        (state.targetDegrees - state.startDegrees) * easedProgress;
    if (progress >= 1.0f) {
        state.currentDegrees = state.targetDegrees;
    }
    return state.currentDegrees;
}

inline nfsmw_drift::Vec3 RotateHorizontalFrom(
    nfsmw_drift::Vec3 from,
    nfsmw_drift::Vec3 to,
    float angleDegrees) {
    if (!std::isfinite(from.x) || !std::isfinite(from.y) ||
        !std::isfinite(from.z) || !std::isfinite(to.x) ||
        !std::isfinite(to.y) || !std::isfinite(to.z) ||
        !std::isfinite(angleDegrees)) {
        return from;
    }
    const float angleRad = angleDegrees * (nfsmw_drift::kPi / 180.0f);
    const float cosine = std::cos(angleRad);
    const float sine = std::sin(angleRad);
    const float x = from.x - to.x;
    const float y = from.y - to.y;
    return {
        cosine * x - sine * y + to.x,
        sine * x + cosine * y + to.y,
        from.z,
    };
}

}  // namespace nfsmw_drift_asi::drift_camera
