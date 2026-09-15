#include "HudGearDisplay.h"

#include "GameAccess.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace hud_gear_display {
namespace {

using SetFormattedTextFn = int(__cdecl*)(void* object, const char* format, ...);
using CustomHudDrawDigitFn = void(__thiscall*)(void* object, int atlasIndex,
                                               std::uint32_t color);

constexpr std::int32_t kFirstExtendedRawGear = 10;
constexpr std::int32_t kLastExtendedRawGear = 13;
constexpr std::int32_t kFirstSplitCustomHudRawGear = 11;

constexpr char kCustomHudModuleName[] = "CustomHud.asi";
constexpr std::uint64_t kCustomHudFileSize = 330752u;
constexpr char kCustomHudMd5[] = "2F5C7AB528C6E465879BEA71A0E75160";
constexpr std::uint32_t kCustomHudTimeDateStamp = 0x6323255Fu;
constexpr std::uint32_t kCustomHudImageSize = 0x00055000u;
constexpr std::uint32_t kCustomHudEntryPointRva = 0x00018B07u;
constexpr std::uintptr_t kCustomHudDrawDigitRva = 0x0000FD50u;
constexpr std::uintptr_t kCustomHudGearDrawCallRva = 0x000116CBu;
constexpr std::size_t kCustomHudSizeOffset = 0x10u;
constexpr std::size_t kCustomHudPositionXOffset = 0x14u;
constexpr std::size_t kCustomHudBackgroundColorOffset = 0x44u;
constexpr std::size_t kCustomHudSpriteOffset = 0x48u;
constexpr std::size_t kCustomHudSpriteOriginalWidthOffset = 0x3Cu;
constexpr std::size_t kCustomHudSpriteOriginalHeightOffset = 0x40u;
constexpr int kCustomHudBackgroundAtlasIndex = 9;

constexpr std::uint8_t kCustomHudDrawDigitBytes[] = {
    0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4,
    0xF8, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04,
    0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0x83, 0xEC,
    0x2C,
};

// HUD::Draw's gear-only foreground path. The earlier background call remains
// untouched, and this signature contains no load-address relocations.
constexpr std::uint8_t kCustomHudGearDrawCallBytes[] = {
    0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x76, 0x1C, 0x8B,
    0x40, 0x20, 0xFF, 0xD0, 0x8B, 0x4C, 0x24, 0x0C,
    0x56, 0x50, 0xE8, 0x80, 0xE6, 0xFF, 0xFF,
};
constexpr std::size_t kCustomHudGearDrawCallSignatureOffset = 18u;
constexpr std::uintptr_t kCustomHudGearDrawSequenceRva =
    kCustomHudGearDrawCallRva - kCustomHudGearDrawCallSignatureOffset;

static_assert(kCustomHudGearDrawCallSignatureOffset + 5u ==
                  sizeof(kCustomHudGearDrawCallBytes),
              "CustomHud call must end its signature");

// 0x0057A7BB: mov edx,[esi+60]; push edx; call GearCharacter; ...
//              push mappedCharacter; push "%c"; push widget;
//              call SetFormattedText; add esp,10h
constexpr std::uint8_t kRaceHudSequence[] = {
    0x8B, 0x56, 0x60, 0x52, 0xE8, 0xDC, 0xDF, 0xFE, 0xFF,
    0x0F, 0xBE, 0xC0, 0x50, 0x68, 0x7C, 0x0D, 0x8A, 0x00,
    0x51, 0xE8, 0x9D, 0xB5, 0xF9, 0xFF, 0x83, 0xC4, 0x10,
};

// 0x0057D217 is the equivalent drag-HUD path. It loads its gear widget from
// [esi+50] between the mapper and formatted-text calls.
constexpr std::uint8_t kDragHudSequence[] = {
    0x8B, 0x56, 0x60, 0x52, 0xE8, 0x80, 0xB5, 0xFE, 0xFF,
    0x8B, 0x4E, 0x50, 0x0F, 0xBE, 0xC0, 0x50,
    0x68, 0x7C, 0x0D, 0x8A, 0x00, 0x51,
    0xE8, 0x3E, 0x8B, 0xF9, 0xFF,
    0x8A, 0x46, 0x64, 0x83, 0xC4, 0x10,
};

constexpr std::uintptr_t kRaceHudSequenceAddress = 0x0057A7BBu;
constexpr std::uintptr_t kDragHudSequenceAddress = 0x0057D217u;
constexpr std::size_t kRaceCallOffset =
    game_access::kRaceHudGearTextCall - kRaceHudSequenceAddress;
constexpr std::size_t kDragCallOffset =
    game_access::kDragHudGearTextCall - kDragHudSequenceAddress;

static_assert(kRaceCallOffset + 5u <= sizeof(kRaceHudSequence),
              "race HUD call must be inside its signature");
static_assert(kDragCallOffset + 5u <= sizeof(kDragHudSequence),
              "drag HUD call must be inside its signature");

volatile LONG g_installState = 0;
volatile LONG g_customHudInstallState = 0;
CustomHudDrawDigitFn g_customHudDrawDigit = nullptr;

class PositionRestore {
public:
    PositionRestore(void* address, float value) noexcept
        : address_(address), value_(value) {}

    ~PositionRestore() {
        if (active_) {
            game_access::SafeWrite(address_, &value_, sizeof(value_));
        }
    }

    bool Restore() noexcept {
        if (!active_) return true;
        if (!game_access::SafeWrite(address_, &value_, sizeof(value_))) {
            return false;
        }
        active_ = false;
        return true;
    }

private:
    void* address_ = nullptr;
    float value_ = 0.0f;
    bool active_ = true;
};

void SetReason(char* reason, std::size_t reasonSize, const char* format, ...) {
    if (reason == nullptr || reasonSize == 0) return;
    va_list args;
    va_start(args, format);
    _vsnprintf_s(reason, reasonSize, _TRUNCATE, format, args);
    va_end(args);
}

bool Matches(std::uintptr_t address, const std::uint8_t* expected,
             std::size_t size) {
    std::uint8_t actual[sizeof(kDragHudSequence)] = {};
    if (size > sizeof(actual) ||
        !game_access::SafeRead(reinterpret_cast<const void*>(address), actual,
                              size)) {
        return false;
    }
    return std::memcmp(actual, expected, size) == 0;
}

bool WriteCode(std::uintptr_t address, const std::uint8_t* bytes,
               std::size_t size) {
    if (address == 0 || bytes == nullptr || size == 0) return false;

    void* target = reinterpret_cast<void*>(address);
    DWORD oldProtection = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE,
                        &oldProtection)) {
        return false;
    }

    std::memcpy(target, bytes, size);
    DWORD ignored = 0;
    const bool restored =
        VirtualProtect(target, size, oldProtection, &ignored) != FALSE;
    const bool flushed =
        FlushInstructionCache(GetCurrentProcess(), target, size) != FALSE;
    return restored && flushed;
}

bool WriteRelativeCallAtomically(std::uintptr_t callAddress,
                                 const std::uint8_t patch[5]) {
    if (patch == nullptr || patch[0] != 0xE8u ||
        callAddress > UINTPTR_MAX - 5u ||
        ((callAddress + 1u) & (alignof(LONG) - 1u)) != 0) {
        return false;
    }

    std::uint8_t opcode = 0;
    if (!game_access::SafeRead(reinterpret_cast<const void*>(callAddress),
                               &opcode, sizeof(opcode)) ||
        opcode != 0xE8u) {
        return false;
    }

    LONG displacement = 0;
    std::memcpy(&displacement, patch + 1, sizeof(displacement));
    void* const instruction = reinterpret_cast<void*>(callAddress);
    DWORD oldProtection = 0;
    if (!VirtualProtect(instruction, 5u, PAGE_EXECUTE_READWRITE,
                        &oldProtection)) {
        return false;
    }

    auto* const target = reinterpret_cast<volatile LONG*>(callAddress + 1u);
    InterlockedExchange(target, displacement);

    DWORD ignored = 0;
    const bool restored =
        VirtualProtect(instruction, 5u, oldProtection, &ignored) != FALSE;
    const bool flushed =
        FlushInstructionCache(GetCurrentProcess(), instruction, 5u) != FALSE;
    return restored && flushed;
}

bool ReadRawGear(std::int32_t* rawGear) {
    if (rawGear == nullptr) return false;

    std::uintptr_t vehicle = 0;
    if (!game_access::SafeRead(reinterpret_cast<const void*>(0x0092FD98u),
                               &vehicle, sizeof(vehicle)) ||
        vehicle == 0 || vehicle > UINTPTR_MAX - 0x2C4u) {
        return false;
    }

    std::uintptr_t drivetrain = 0;
    if (!game_access::SafeRead(reinterpret_cast<const void*>(vehicle + 0x2C4u),
                               &drivetrain, sizeof(drivetrain)) ||
        drivetrain == 0 || drivetrain > UINTPTR_MAX - 0x60u) {
        return false;
    }
    return game_access::SafeRead(
        reinterpret_cast<const void*>(drivetrain + 0x60u), rawGear,
        sizeof(*rawGear));
}

void __fastcall CustomHudGearDrawProxy(void* object, void*, int atlasIndex,
                                       std::uint32_t color) {
    const CustomHudDrawDigitFn drawDigit = g_customHudDrawDigit;
    if (drawDigit == nullptr) return;

    std::int32_t rawGear = 0;
    if (!ReadRawGear(&rawGear)) {
        drawDigit(object, atlasIndex, color);
        return;
    }

    const CustomHudPresentation presentation =
        SelectCustomHudPresentation(rawGear, atlasIndex);
    if (!presentation.split) {
        drawDigit(object, atlasIndex, color);
        return;
    }

    if (object == nullptr) return;

    float size = 0.0f;
    float originalPositionX = 0.0f;
    std::uint32_t backgroundColor = 0;
    std::uintptr_t sprite = 0;
    std::uint32_t atlasWidth = 0;
    std::uint32_t atlasHeight = 0;
    const auto* bytes = static_cast<const std::uint8_t*>(object);
    if (!game_access::SafeRead(bytes + kCustomHudSizeOffset, &size,
                               sizeof(size)) ||
        !game_access::SafeRead(bytes + kCustomHudPositionXOffset,
                               &originalPositionX,
                               sizeof(originalPositionX)) ||
        !game_access::SafeRead(bytes + kCustomHudBackgroundColorOffset,
                               &backgroundColor,
                               sizeof(backgroundColor)) ||
        !game_access::SafeRead(bytes + kCustomHudSpriteOffset, &sprite,
                               sizeof(sprite)) ||
        sprite == 0 ||
        sprite > UINTPTR_MAX - kCustomHudSpriteOriginalHeightOffset ||
        !game_access::SafeRead(
            reinterpret_cast<const void*>(
                sprite + kCustomHudSpriteOriginalWidthOffset),
            &atlasWidth, sizeof(atlasWidth)) ||
        !game_access::SafeRead(
            reinterpret_cast<const void*>(
                sprite + kCustomHudSpriteOriginalHeightOffset),
            &atlasHeight, sizeof(atlasHeight)) ||
        !std::isfinite(size) || size <= 0.0f ||
        !std::isfinite(originalPositionX)) {
        drawDigit(object, atlasIndex, color);
        return;
    }

    const float glyphWidth =
        CustomHudGlyphWidth(size, atlasWidth, atlasHeight);
    const float leadingPositionX =
        CustomHudLeadingPositionX(originalPositionX, glyphWidth);
    if (!std::isfinite(glyphWidth) || glyphWidth <= 0.0f ||
        !std::isfinite(leadingPositionX)) {
        drawDigit(object, atlasIndex, color);
        return;
    }

    auto* mutableBytes = static_cast<std::uint8_t*>(object);
    void* const positionAddress =
        mutableBytes + kCustomHudPositionXOffset;
    if (!game_access::SafeWrite(positionAddress, &leadingPositionX,
                                sizeof(leadingPositionX))) {
        drawDigit(object, atlasIndex, color);
        return;
    }

    PositionRestore restorePosition(positionAddress, originalPositionX);
    if (backgroundColor != 0) {
        drawDigit(object, kCustomHudBackgroundAtlasIndex, backgroundColor);
    }
    drawDigit(object, presentation.leadingAtlasIndex, color);
    if (restorePosition.Restore()) {
        drawDigit(object, presentation.trailingAtlasIndex, color);
    }
}

bool ReadModulePe32(HMODULE module, IMAGE_NT_HEADERS32* nt) {
    if (module == nullptr || nt == nullptr) return false;

    IMAGE_DOS_HEADER dos{};
    if (!game_access::SafeRead(module, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 0x1000) {
        return false;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    return game_access::SafeRead(base + dos.e_lfanew, nt, sizeof(*nt)) &&
           nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
}

int __cdecl HudGearTextProxy(void* object, const char* legacyFormat,
                             int legacyCharacter, std::int32_t rawGear) {
    const SetFormattedTextFn setText =
        reinterpret_cast<SetFormattedTextFn>(
            game_access::kHudSetFormattedText);
    const Presentation presentation =
        SelectPresentation(rawGear, legacyCharacter);
    if (presentation.numeric) {
        return setText(object, "%d", presentation.value);
    }
    return setText(object, legacyFormat, presentation.value);
}

bool RestoreOriginalCalls() {
    const bool race = WriteCode(game_access::kRaceHudGearTextCall,
                                kRaceHudSequence + kRaceCallOffset, 5u);
    const bool drag = WriteCode(game_access::kDragHudGearTextCall,
                                kDragHudSequence + kDragCallOffset, 5u);
    return race && drag;
}

}  // namespace

Presentation SelectPresentation(std::int32_t rawGear,
                                int legacyCharacter) noexcept {
    if (rawGear >= kFirstExtendedRawGear &&
        rawGear <= kLastExtendedRawGear) {
        return {true, static_cast<int>(rawGear - 1)};
    }
    return {false, legacyCharacter};
}

CustomHudPresentation SelectCustomHudPresentation(
    std::int32_t rawGear, int incomingAtlasIndex) noexcept {
    if (rawGear < kFirstSplitCustomHudRawGear ||
        rawGear > kLastExtendedRawGear) {
        return {};
    }

    const int expectedAtlasIndex = rawGear > 12 ? 0 : rawGear;
    if (incomingAtlasIndex != expectedAtlasIndex) return {};

    // Atlas slots are one-based: index 1 is digit 0, index 2 is digit 1.
    return {true, 2, static_cast<int>(rawGear - 10)};
}

float CustomHudGlyphWidth(float configuredSize,
                          std::uint32_t atlasWidth,
                          std::uint32_t atlasHeight) noexcept {
    if (!std::isfinite(configuredSize) || configuredSize <= 0.0f ||
        atlasWidth == 0 || atlasHeight == 0) {
        return 0.0f;
    }
    return (static_cast<float>(atlasWidth) / 12.0f) /
           static_cast<float>(atlasHeight) * configuredSize;
}

float CustomHudLeadingPositionX(float originalPositionX,
                                float glyphWidth) noexcept {
    return originalPositionX + glyphWidth;
}

bool EncodeRelativeCall(std::uintptr_t callAddress,
                        std::uintptr_t targetAddress,
                        std::uint8_t patch[5]) noexcept {
    if (patch == nullptr || callAddress > UINTPTR_MAX - 5u) return false;

    const std::int64_t displacement =
        static_cast<std::int64_t>(targetAddress) -
        static_cast<std::int64_t>(callAddress + 5u);
    if (displacement < INT32_MIN || displacement > INT32_MAX) return false;

    const std::int32_t relative = static_cast<std::int32_t>(displacement);
    patch[0] = 0xE8;
    std::memcpy(patch + 1, &relative, sizeof(relative));
    return true;
}

bool Install(char* reason, std::size_t reasonSize) {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';

    const LONG previous = InterlockedCompareExchange(&g_installState, 1, 0);
    if (previous == 2) return true;
    if (previous != 0) {
        SetReason(reason, reasonSize, "HUD gear display install is busy");
        return false;
    }

    if (!Matches(kRaceHudSequenceAddress, kRaceHudSequence,
                 sizeof(kRaceHudSequence))) {
        SetReason(reason, reasonSize,
                  "race HUD gear sequence at 0x%08X does not match",
                  static_cast<unsigned>(kRaceHudSequenceAddress));
        InterlockedExchange(&g_installState, 0);
        return false;
    }
    if (!Matches(kDragHudSequenceAddress, kDragHudSequence,
                 sizeof(kDragHudSequence))) {
        SetReason(reason, reasonSize,
                  "drag HUD gear sequence at 0x%08X does not match",
                  static_cast<unsigned>(kDragHudSequenceAddress));
        InterlockedExchange(&g_installState, 0);
        return false;
    }

    const std::uintptr_t proxy =
        reinterpret_cast<std::uintptr_t>(&HudGearTextProxy);
    std::uint8_t racePatch[5] = {};
    std::uint8_t dragPatch[5] = {};
    if (!EncodeRelativeCall(game_access::kRaceHudGearTextCall, proxy,
                            racePatch) ||
        !EncodeRelativeCall(game_access::kDragHudGearTextCall, proxy,
                            dragPatch)) {
        SetReason(reason, reasonSize,
                  "HUD gear proxy is outside x86 CALL rel32 range");
        InterlockedExchange(&g_installState, 0);
        return false;
    }

    if (!WriteCode(game_access::kRaceHudGearTextCall, racePatch,
                   sizeof(racePatch))) {
        const bool restored = RestoreOriginalCalls();
        SetReason(reason, reasonSize,
                  "could not patch race HUD call at 0x%08X; rollback=%s",
                  static_cast<unsigned>(game_access::kRaceHudGearTextCall),
                  restored ? "complete" : "partial");
        InterlockedExchange(&g_installState, 0);
        return false;
    }
    if (!WriteCode(game_access::kDragHudGearTextCall, dragPatch,
                   sizeof(dragPatch))) {
        const bool restored = RestoreOriginalCalls();
        SetReason(reason, reasonSize,
                  "could not patch drag HUD call at 0x%08X; rollback=%s",
                  static_cast<unsigned>(game_access::kDragHudGearTextCall),
                  restored ? "complete" : "partial");
        InterlockedExchange(&g_installState, 0);
        return false;
    }

    if (!Matches(game_access::kRaceHudGearTextCall, racePatch,
                 sizeof(racePatch)) ||
        !Matches(game_access::kDragHudGearTextCall, dragPatch,
                 sizeof(dragPatch))) {
        const bool restored = RestoreOriginalCalls();
        SetReason(reason, reasonSize,
                  "HUD gear call verification failed; rollback=%s",
                  restored ? "complete" : "partial");
        InterlockedExchange(&g_installState, 0);
        return false;
    }

    InterlockedExchange(&g_installState, 2);
    return true;
}

CustomHudInstallResult InstallCustomHud(char* reason,
                                        std::size_t reasonSize) {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';

    const LONG previous =
        InterlockedCompareExchange(&g_customHudInstallState, 1, 0);
    if (previous == 2) return CustomHudInstallResult::Installed;
    if (previous != 0) {
        SetReason(reason, reasonSize,
                  "CustomHud gear display install is busy");
        return CustomHudInstallResult::Failed;
    }

    HMODULE module = GetModuleHandleA(kCustomHudModuleName);
    if (module == nullptr) {
        SetReason(reason, reasonSize, "%s is not loaded", kCustomHudModuleName);
        InterlockedExchange(&g_customHudInstallState, 0);
        return CustomHudInstallResult::NotLoaded;
    }

    char path[MAX_PATH] = {};
    const DWORD pathLength =
        GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
    char md5[33] = {};
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (pathLength == 0 || pathLength >= sizeof(path) ||
        !GetFileAttributesExA(path, GetFileExInfoStandard, &attributes) ||
        attributes.nFileSizeHigh != 0 ||
        attributes.nFileSizeLow != kCustomHudFileSize ||
        !game_access::ComputeFileMd5(path, md5) ||
        _stricmp(md5, kCustomHudMd5) != 0) {
        SetReason(reason, reasonSize,
                  "loaded CustomHud is not the supported v1.8.2 binary "
                  "(required size=%llu md5=%s; actual size=%llu md5=%s)",
                  static_cast<unsigned long long>(kCustomHudFileSize),
                  kCustomHudMd5,
                  (static_cast<unsigned long long>(attributes.nFileSizeHigh)
                   << 32u) | attributes.nFileSizeLow,
                  md5[0] != '\0' ? md5 : "unavailable");
        InterlockedExchange(&g_customHudInstallState, 0);
        return CustomHudInstallResult::UnsupportedBuild;
    }

    IMAGE_NT_HEADERS32 nt{};
    if (!ReadModulePe32(module, &nt) ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.FileHeader.TimeDateStamp != kCustomHudTimeDateStamp ||
        nt.OptionalHeader.SizeOfImage != kCustomHudImageSize ||
        nt.OptionalHeader.AddressOfEntryPoint != kCustomHudEntryPointRva) {
        SetReason(reason, reasonSize,
                  "loaded CustomHud PE profile does not match v1.8.2");
        InterlockedExchange(&g_customHudInstallState, 0);
        return CustomHudInstallResult::UnsupportedBuild;
    }

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
    if (base > UINTPTR_MAX - kCustomHudGearDrawSequenceRva ||
        base > UINTPTR_MAX - kCustomHudGearDrawCallRva ||
        base > UINTPTR_MAX - kCustomHudDrawDigitRva) {
        SetReason(reason, reasonSize, "CustomHud RVA overflow");
        InterlockedExchange(&g_customHudInstallState, 0);
        return CustomHudInstallResult::Failed;
    }

    const std::uintptr_t drawDigit = base + kCustomHudDrawDigitRva;
    const std::uintptr_t callSequence =
        base + kCustomHudGearDrawSequenceRva;
    const std::uintptr_t callAddress = base + kCustomHudGearDrawCallRva;
    if (!Matches(drawDigit, kCustomHudDrawDigitBytes,
                 sizeof(kCustomHudDrawDigitBytes)) ||
        !Matches(callSequence, kCustomHudGearDrawCallBytes,
                 sizeof(kCustomHudGearDrawCallBytes))) {
        SetReason(reason, reasonSize,
                  "CustomHud v1.8.2 drawing signatures do not match");
        InterlockedExchange(&g_customHudInstallState, 0);
        return CustomHudInstallResult::UnsupportedBuild;
    }

    std::uint8_t patch[5] = {};
    if (!EncodeRelativeCall(
            callAddress,
            reinterpret_cast<std::uintptr_t>(&CustomHudGearDrawProxy),
            patch)) {
        SetReason(reason, reasonSize,
                  "CustomHud gear proxy is outside x86 CALL rel32 range");
        InterlockedExchange(&g_customHudInstallState, 0);
        return CustomHudInstallResult::Failed;
    }

    g_customHudDrawDigit = reinterpret_cast<CustomHudDrawDigitFn>(drawDigit);
    if (!WriteRelativeCallAtomically(callAddress, patch) ||
        !Matches(callAddress, patch, sizeof(patch))) {
        const bool restored = WriteRelativeCallAtomically(
            callAddress, kCustomHudGearDrawCallBytes +
                             kCustomHudGearDrawCallSignatureOffset);
        g_customHudDrawDigit = nullptr;
        SetReason(reason, reasonSize,
                  "could not patch CustomHud gear call at RVA 0x%05X; "
                  "rollback=%s",
                  static_cast<unsigned>(kCustomHudGearDrawCallRva),
                  restored ? "complete" : "partial");
        InterlockedExchange(&g_customHudInstallState, 0);
        return CustomHudInstallResult::Failed;
    }

    SetReason(reason, reasonSize,
              "CustomHud v1.8.2 size=%llu md5=%s callRva=0x%05X",
              static_cast<unsigned long long>(kCustomHudFileSize),
              kCustomHudMd5,
              static_cast<unsigned>(kCustomHudGearDrawCallRva));
    InterlockedExchange(&g_customHudInstallState, 2);
    return CustomHudInstallResult::Installed;
}

}  // namespace hud_gear_display
