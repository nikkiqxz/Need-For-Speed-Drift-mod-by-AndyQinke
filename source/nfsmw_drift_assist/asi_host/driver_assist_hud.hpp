#pragma once

#include <cstdint>

namespace nfsmw_drift_asi::driver_assist_hud {

constexpr std::uint8_t kPanelOpacityAlpha = 166;
constexpr std::uint32_t kSolidActiveMilliseconds = 500;
constexpr std::uint32_t kNormalBlinkHalfPeriodMilliseconds = 44;
constexpr std::uint32_t kRecoveryTcsBlinkHalfPeriodMilliseconds = 61;
constexpr float kMinimumForwardHudSpeedMps = 5.0f / 3.6f;

constexpr bool ShouldShow(float longitudinalSpeedMps,
                          bool lcWorking) noexcept {
    return longitudinalSpeedMps >= 0.0f &&
           (longitudinalSpeedMps > kMinimumForwardHudSpeedMps ||
            lcWorking);
}

constexpr bool ShouldRender(bool localVisibilityAllowed,
                            bool nativeHudVisible) noexcept {
    return localVisibilityAllowed && nativeHudVisible;
}

constexpr bool ShouldRenderWithNativeSample(
    bool localVisibilityAllowed,
    bool nativeHudTableVisible,
    bool nativeFeatureSampleObserved,
    bool nativeFeatureEnabled,
    bool nativeHudObjectVisible) noexcept {
    if (!localVisibilityAllowed || !nativeHudTableVisible) return false;
    if (!nativeFeatureSampleObserved) return true;
    return nativeFeatureEnabled && nativeHudObjectVisible;
}

struct Rect {
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
};

struct Layout {
    Rect outer{};
    Rect rows[3]{};
    int pixel = 1;
};

Layout MakeLayout(std::uint32_t width, std::uint32_t height) noexcept;
void Publish(bool visible, bool absWorking, bool escWorking,
             bool escRecoveryWorking, bool tcsWorking,
             bool lcWorking,
             std::uint32_t nowMilliseconds) noexcept;
void Install() noexcept;

}  // namespace nfsmw_drift_asi::driver_assist_hud
