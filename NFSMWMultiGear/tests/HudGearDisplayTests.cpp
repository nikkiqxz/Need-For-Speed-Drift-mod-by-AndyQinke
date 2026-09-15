#include "../src/HudGearDisplay.h"

#include <cstdint>
#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++g_failures;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void TestPresentationSelection() {
    const hud_gear_display::Presentation reverse =
        hud_gear_display::SelectPresentation(0, 'R');
    CHECK(!reverse.numeric);
    CHECK(reverse.value == 'R');

    const hud_gear_display::Presentation neutral =
        hud_gear_display::SelectPresentation(1, 'N');
    CHECK(!neutral.numeric);
    CHECK(neutral.value == 'N');

    const hud_gear_display::Presentation eighth =
        hud_gear_display::SelectPresentation(9, '8');
    CHECK(!eighth.numeric);
    CHECK(eighth.value == '8');

    for (std::int32_t raw = 10; raw <= 13; ++raw) {
        const hud_gear_display::Presentation extended =
            hud_gear_display::SelectPresentation(raw, 'N');
        CHECK(extended.numeric);
        CHECK(extended.value == raw - 1);
    }

    const hud_gear_display::Presentation unexpected =
        hud_gear_display::SelectPresentation(14, 'N');
    CHECK(!unexpected.numeric);
    CHECK(unexpected.value == 'N');
}

void TestRelativeCallEncoding() {
    std::uint8_t patch[5] = {};
    CHECK(hud_gear_display::EncodeRelativeCall(0x1000u, 0x2000u, patch));
    CHECK(patch[0] == 0xE8u);
    CHECK(patch[1] == 0xFBu);
    CHECK(patch[2] == 0x0Fu);
    CHECK(patch[3] == 0x00u);
    CHECK(patch[4] == 0x00u);

    CHECK(hud_gear_display::EncodeRelativeCall(0x2000u, 0x1000u, patch));
    CHECK(patch[0] == 0xE8u);
    CHECK(patch[1] == 0xFBu);
    CHECK(patch[2] == 0xEFu);
    CHECK(patch[3] == 0xFFu);
    CHECK(patch[4] == 0xFFu);

    CHECK(!hud_gear_display::EncodeRelativeCall(UINTPTR_MAX - 4u, 0u,
                                                patch));
    CHECK(!hud_gear_display::EncodeRelativeCall(0u, UINTPTR_MAX, patch));
    CHECK(!hud_gear_display::EncodeRelativeCall(0x1000u, 0x2000u, nullptr));
}

void TestCustomHudPresentation() {
    for (std::int32_t raw = 0; raw <= 10; ++raw) {
        const hud_gear_display::CustomHudPresentation unchanged =
            hud_gear_display::SelectCustomHudPresentation(raw, raw);
        CHECK(!unchanged.split);
    }

    const hud_gear_display::CustomHudPresentation tenth =
        hud_gear_display::SelectCustomHudPresentation(11, 11);
    CHECK(tenth.split);
    CHECK(tenth.leadingAtlasIndex == 2);
    CHECK(tenth.trailingAtlasIndex == 1);

    const hud_gear_display::CustomHudPresentation eleventh =
        hud_gear_display::SelectCustomHudPresentation(12, 12);
    CHECK(eleventh.split);
    CHECK(eleventh.leadingAtlasIndex == 2);
    CHECK(eleventh.trailingAtlasIndex == 2);

    const hud_gear_display::CustomHudPresentation twelfth =
        hud_gear_display::SelectCustomHudPresentation(13, 0);
    CHECK(twelfth.split);
    CHECK(twelfth.leadingAtlasIndex == 2);
    CHECK(twelfth.trailingAtlasIndex == 3);

    CHECK(!hud_gear_display::SelectCustomHudPresentation(11, 9).split);
    CHECK(!hud_gear_display::SelectCustomHudPresentation(12, 11).split);
    CHECK(!hud_gear_display::SelectCustomHudPresentation(13, 13).split);
    CHECK(!hud_gear_display::SelectCustomHudPresentation(14, 0).split);
}

void TestCustomHudPosition() {
    const float glyphWidth =
        hud_gear_display::CustomHudGlyphWidth(27.5f, 1368u, 114u);
    CHECK(std::fabs(glyphWidth - 27.5f) < 0.0001f);
    const float position =
        hud_gear_display::CustomHudLeadingPositionX(202.0f, glyphWidth);
    CHECK(std::fabs(position - 229.5f) < 0.0001f);
    CHECK(hud_gear_display::CustomHudLeadingPositionX(-4.0f, 0.0f) ==
          -4.0f);
    CHECK(hud_gear_display::CustomHudGlyphWidth(0.0f, 1368u, 114u) ==
          0.0f);
    CHECK(hud_gear_display::CustomHudGlyphWidth(27.5f, 0u, 114u) ==
          0.0f);
    CHECK(hud_gear_display::CustomHudGlyphWidth(27.5f, 1368u, 0u) ==
          0.0f);
}

}  // namespace

int main() {
    TestPresentationSelection();
    TestRelativeCallEncoding();
    TestCustomHudPresentation();
    TestCustomHudPosition();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d HUD gear display assertion(s) failed\n",
                     g_failures);
        return 1;
    }

    std::puts("HUD gear display tests passed");
    return 0;
}
