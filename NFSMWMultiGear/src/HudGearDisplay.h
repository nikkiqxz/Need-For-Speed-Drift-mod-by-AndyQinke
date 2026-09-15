#pragma once

#include <cstddef>
#include <cstdint>

namespace hud_gear_display {

struct Presentation {
    bool numeric = false;
    int value = 0;
};

struct CustomHudPresentation {
    bool split = false;
    int leadingAtlasIndex = 0;
    int trailingAtlasIndex = 0;
};

enum class CustomHudInstallResult {
    Installed,
    NotLoaded,
    UnsupportedBuild,
    Failed,
};

// Native raw indices 10..13 are forward gears 9..12. All native indices and
// unexpected values retain the character selected by the game's mapper.
Presentation SelectPresentation(std::int32_t rawGear,
                                int legacyCharacter) noexcept;

// CustomHud v1.8.2 maps reverse/neutral into the last two slots of its
// single-glyph atlas. Raw gears 11..13 therefore need two numeric glyphs.
// The incoming atlas index is checked so background and unrelated digit draws
// retain their original behavior.
CustomHudPresentation SelectCustomHudPresentation(
    std::int32_t rawGear, int incomingAtlasIndex) noexcept;

// CustomHud positions glyphs from the right edge. Derive one atlas cell's
// rendered width, then keep the trailing digit at the configured position and
// move the leading digit left by exactly that amount.
float CustomHudGlyphWidth(float configuredSize,
                          std::uint32_t atlasWidth,
                          std::uint32_t atlasHeight) noexcept;
float CustomHudLeadingPositionX(float originalPositionX,
                                float glyphWidth) noexcept;

// Encodes a five-byte x86 CALL rel32 instruction. Exposed for deterministic
// offline coverage of the code-patch arithmetic.
bool EncodeRelativeCall(std::uintptr_t callAddress,
                        std::uintptr_t targetAddress,
                        std::uint8_t patch[5]) noexcept;

// Installs the two narrowly-scoped HUD call-site redirects after the exact
// executable profile has been verified. Failure leaves both sites unchanged.
bool Install(char* reason, std::size_t reasonSize);

// Installs compatibility for the exact user-tested CustomHud v1.8.2 binary.
// Missing or unrecognized versions are reported without modifying the module.
CustomHudInstallResult InstallCustomHud(char* reason,
                                        std::size_t reasonSize);

}  // namespace hud_gear_display
