#include "asi_host/driver_assist_hud.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void TestReferenceLayout() {
    const auto layout =
        nfsmw_drift_asi::driver_assist_hud::MakeLayout(1920, 1080);
    Require(layout.outer.left == 1565 && layout.outer.top == 960,
            "HUD should move 32 reference pixels left");
    Require(layout.outer.right == 1648 && layout.outer.bottom == 1038,
            "HUD reference size should be 83 by 78");
    Require(layout.rows[0].bottom < layout.rows[1].top &&
                layout.rows[1].bottom < layout.rows[2].top,
            "HUD rows should be separated");
}

void TestResolutionScaling() {
    const auto layout =
        nfsmw_drift_asi::driver_assist_hud::MakeLayout(1280, 720);
    Require(layout.outer.left == 1044 && layout.outer.top == 640,
            "720p HUD should preserve the screenshot anchor");
    Require(layout.outer.right <= 1280 && layout.outer.bottom <= 720,
            "HUD must remain inside the viewport");
    for (const auto& row : layout.rows) {
        Require(row.left >= layout.outer.left &&
                    row.right <= layout.outer.right &&
                    row.top >= layout.outer.top &&
                    row.bottom <= layout.outer.bottom,
                "HUD row must remain inside the outer panel");
    }
}

void TestVisualTimingAndOpacity() {
    using namespace nfsmw_drift_asi::driver_assist_hud;
    Require(kPanelOpacityAlpha == 166,
            "HUD panel opacity should round 65 percent to alpha 166");
    Require(kSolidActiveMilliseconds == 500,
            "working indicators should remain solid for half a second");
    Require(kNormalBlinkHalfPeriodMilliseconds <= 88 / 2,
            "normal HUD blinking must be at least twice as fast as 0.9.23");
    Require(kRecoveryTcsBlinkHalfPeriodMilliseconds <= 122 / 2,
            "recovery TCS blinking must be at least twice as fast as 0.9.23");
}

void TestHudVisibilityGate() {
    using namespace nfsmw_drift_asi::driver_assist_hud;
    Require(!ShouldShow(-2.0f, false),
            "HUD must remain hidden while reversing");
    Require(!ShouldShow(5.0f / 3.6f, false),
            "HUD must remain hidden at the five km/h boundary");
    Require(ShouldShow(5.01f / 3.6f, false),
            "HUD should appear above five km/h while moving forward");
    Require(ShouldShow(0.0f, true),
            "active launch control must expose its LC indicator at rest");
    Require(ShouldShow(3.0f / 3.6f, true),
            "active launch control must bypass the five km/h HUD gate");
    Require(!ShouldShow(-0.01f, true),
            "reverse motion must hide even an active LC indicator");

    Require(ShouldRender(true, true),
            "local visibility may render only with a visible native HUD");
    Require(!ShouldRender(true, false),
            "native HUD hiding must override the local visibility gate");
    Require(!ShouldRender(false, true),
            "native visibility must not bypass speed, reverse, or LC rules");
    Require(!ShouldRender(false, false),
            "HUD must remain hidden when both visibility layers reject it");

    Require(ShouldRenderWithNativeSample(true, true, false, false, false),
            "HUD-table visibility must provide a startup fallback before the first native feature sample");
    Require(!ShouldRenderWithNativeSample(true, false, false, false, false),
            "a hidden native HUD table must override the startup fallback");
    Require(ShouldRenderWithNativeSample(true, true, true, true, true),
            "a complete visible CustomHud-compatible sample must render");
    Require(!ShouldRenderWithNativeSample(true, true, true, false, true),
            "a disabled native HUD feature must hide the assist HUD");
    Require(!ShouldRenderWithNativeSample(true, true, true, true, false),
            "a hidden native FEngHud object must hide the assist HUD");
    Require(!ShouldRenderWithNativeSample(false, true, true, true, true),
            "native state must not bypass speed, reverse, or LC rules");
}

}  // namespace

int main() {
    TestReferenceLayout();
    TestResolutionScaling();
    TestVisualTimingAndOpacity();
    TestHudVisibilityGate();
    std::cout << "driver assist HUD tests passed\n";
    return 0;
}
