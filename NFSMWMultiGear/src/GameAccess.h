#pragma once

#include <cstddef>
#include <cstdint>

namespace game_access {

constexpr std::uintptr_t kImageBase = 0x00400000u;
constexpr std::uint64_t kReferenceFileSize = 6029312u;
constexpr char kReferenceMd5[] = "C0516B485065FABDD69579816B5DF763";
constexpr std::uint64_t kV13EnglishCollectorsFileSize = 6135808u;
constexpr char kV13EnglishCollectorsMd5[] =
    "C5C5DBED69AF10A43A6037FD312384F2";
constexpr char kV13EnglishCollectorsSha256[] =
    "36FB81DB38469BABF15E9EDFD40959CB22609DB9005A51D4250BBC86BBCF3DD9";
constexpr std::uint64_t kReforgedFileSize = 6135872u;
constexpr char kReforgedMd5[] = "97F5A7DFC1CE8F6CCDB6A812C583C3AA";
constexpr std::uint32_t kV13EnglishCollectorsTimeDateStamp = 0x438E4C8Cu;
constexpr std::uint32_t kV13EnglishCollectorsImageSize = 0x00693000u;
constexpr std::uint32_t kV13EnglishCollectorsEntryPointRva = 0x003C4040u;

constexpr std::uintptr_t kBStringHash = 0x005CC240u;
constexpr std::uintptr_t kTransmissionCtor = 0x006A5590u;
constexpr std::uintptr_t kClassGetDefinition = 0x00457380u;
constexpr std::uintptr_t kPrivateCount = 0x00452940u;
constexpr std::uintptr_t kPrivateGetElement = 0x00453890u;
constexpr std::uintptr_t kShiftGear = 0x006920D0u;

// Phase 4 transmission entry points.  These consumers address native gear
// storage directly and therefore need dedicated hooks for sidecar gears.
constexpr std::uintptr_t kGearRatioGetter = 0x00691AC0u;
constexpr std::uintptr_t kGearEfficiencyGetter = 0x00691AF0u;
constexpr std::uintptr_t kGearRatioPointer = 0x00691B30u;
constexpr std::uintptr_t kEffectiveGearRatio = 0x00691BD0u;
constexpr std::uintptr_t kTransmissionUpdate = 0x00691F90u;
constexpr std::uintptr_t kEffectiveRatioHelper = 0x00692F10u;
constexpr std::uintptr_t kAdditionalShiftDecision = 0x00693010u;
// Shared clamp/interpolation helper used by the sidecar shift-decision path.
// Phase1 calls this address directly, so it is included in the build
// signature gate just like every hooked entry point.
constexpr std::uintptr_t kShiftCurve = 0x00401AE0u;

// High-gear consumers found in the alternate transmission/automatic paths.
// These functions index fixed native arrays or expose the transmission's
// virtual gear count, so callers that virtualize raw indices 10..13 must
// either proxy them or deliberately leave them untouched for unconfigured
// layouts.
constexpr std::uintptr_t kGearRatioPair = 0x00693340u;
constexpr std::uintptr_t kTransmissionShift = 0x006A18F0u;
constexpr std::uintptr_t kTransmissionGetTopGear = 0x006B0220u;
// Alternate transmission object used by the secondary drivetrain path.  It
// keeps the ratio Private header at +0xA4 (the primary object uses +0xA8).
constexpr std::uintptr_t kAlternateTransmissionShift = 0x006A32E0u;
constexpr std::uintptr_t kAlternateTransmissionGetTopGear = 0x006B0290u;
constexpr std::uintptr_t kAutomaticShiftDecision = 0x006A0D90u;
constexpr std::uintptr_t kAutomaticShiftUpdate = 0x006A1740u;
constexpr std::uintptr_t kAutomaticShiftController = 0x006A3180u;

// HUD gear text path. Both race HUD variants leave the raw gear argument on
// the stack while calling the game's single-character mapper, then pass that
// same stack frame to the formatted-text helper. The two call sites are
// redirected by HudGearDisplay without changing the mapper globally.
constexpr std::uintptr_t kHudGearCharacter = 0x005687A0u;
constexpr std::uintptr_t kHudSetFormattedText = 0x00515D70u;
constexpr std::uintptr_t kRaceHudGearTextCall = 0x0057A7CEu;
constexpr std::uintptr_t kDragHudGearTextCall = 0x0057D22Du;

// Phase 4 sidecar consumers. These are small helpers used by the alternate
// drivetrain/automatic-shift paths; they read the in-layout arrays directly
// instead of going through the public transmission accessors.
constexpr std::uintptr_t kDerivedShiftValue = 0x00691C50u;
constexpr std::uintptr_t kDerivedEfficiencyValue = 0x00691C60u;
constexpr std::uintptr_t kLayoutRatioGetter = 0x006A0EF0u;
constexpr std::uintptr_t kLayoutEfficiencyGetter = 0x006A0F20u;
constexpr std::uintptr_t kLayoutEffectiveRatio = 0x006A0F60u;
constexpr std::uintptr_t kTorqueEfficiencyRatio = 0x006A1480u;
constexpr std::uintptr_t kControllerRatioGetter = 0x006A29B0u;
constexpr std::uintptr_t kControllerEfficiencyGetter = 0x006A29E0u;
constexpr std::uintptr_t kControllerEffectiveRatio = 0x006A2A10u;

constexpr std::uint32_t kGearRatioKey = 0x17144A84u;
constexpr std::uint32_t kGearEfficiencyKey = 0x5250EB92u;

constexpr std::size_t kWrapperCollectionOffset = 0x04u;
constexpr std::size_t kCollectionParentOffset = 0x10u;
constexpr std::size_t kCollectionClassOffset = 0x14u;
constexpr std::size_t kCollectionLayoutOffset = 0x18u;
constexpr std::size_t kCollectionKeyOffset = 0x20u;

// Attrib::Private stores inline data after this header. Bit 15 in metadata
// inserts an additional eight-byte prefix before the first element.
struct PrivateHeader {
    std::uint16_t capacity;
    std::uint16_t count;
    std::uint16_t elementSize;
    std::uint16_t metadata;
};

static_assert(sizeof(PrivateHeader) == 8, "MW Attrib::Private must be 8 bytes");

constexpr std::uint16_t kPrivateHasExtraPrefix = 0x8000u;

constexpr std::size_t PrivateDataOffset(
    const PrivateHeader& header) noexcept {
    return sizeof(PrivateHeader) +
           ((header.metadata & kPrivateHasExtraPrefix) != 0 ? 8u : 0u);
}

struct AttributeDefinition {
    std::uint32_t key;
    std::uint32_t type;
    std::uint16_t offset;
    std::uint16_t size;
    std::uint16_t maxCount;
    std::uint8_t flags;
    std::uint8_t alignment;
};

static_assert(sizeof(AttributeDefinition) == 16,
              "MW Attrib::Definition must be 16 bytes");

constexpr std::uint8_t kDefinitionArray = 1u << 0;
constexpr std::uint8_t kDefinitionInLayout = 1u << 1;

struct ExecutableFingerprint {
    char path[260] = {};
    std::uintptr_t loadedBase = 0;
    std::uint64_t fileSize = 0;
    std::uint32_t timeDateStamp = 0;
    std::uint32_t imageSize = 0;
    std::uint32_t preferredImageBase = 0;
    std::uint32_t entryPointRva = 0;
    std::uint16_t machine = 0;
    std::uint16_t optionalHeaderMagic = 0;
    char md5[33] = {};
};

extern const std::uint8_t kTransmissionCtorBytes[7];
extern const std::uint8_t kPrivateCountBytes[5];
extern const std::uint8_t kPrivateGetElementBytes[12];
extern const std::uint8_t kShiftGearBytes[23];
extern const std::uint8_t kGearRatioGetterBytes[14];
extern const std::uint8_t kGearEfficiencyGetterBytes[15];
extern const std::uint8_t kGearRatioPointerBytes[17];
extern const std::uint8_t kEffectiveGearRatioBytes[14];
extern const std::uint8_t kTransmissionUpdateBytes[20];
extern const std::uint8_t kEffectiveRatioHelperBytes[17];
extern const std::uint8_t kAdditionalShiftDecisionBytes[15];
extern const std::uint8_t kShiftCurveBytes[16];
extern const std::uint8_t kGearRatioPairBytes[20];
extern const std::uint8_t kTransmissionShiftBytes[19];
extern const std::uint8_t kTransmissionGetTopGearBytes[13];
extern const std::uint8_t kAlternateTransmissionShiftBytes[19];
extern const std::uint8_t kAlternateTransmissionGetTopGearBytes[13];
extern const std::uint8_t kAutomaticShiftDecisionBytes[15];
extern const std::uint8_t kAutomaticShiftUpdateBytes[17];
extern const std::uint8_t kAutomaticShiftControllerBytes[17];
extern const std::uint8_t kDerivedShiftValueBytes[14];
extern const std::uint8_t kDerivedEfficiencyValueBytes[14];
extern const std::uint8_t kLayoutRatioGetterBytes[14];
extern const std::uint8_t kLayoutEfficiencyGetterBytes[15];
extern const std::uint8_t kLayoutEffectiveRatioBytes[14];
extern const std::uint8_t kTorqueEfficiencyRatioBytes[15];
extern const std::uint8_t kControllerRatioGetterBytes[14];
extern const std::uint8_t kControllerEfficiencyGetterBytes[15];
extern const std::uint8_t kControllerEffectiveRatioBytes[14];

enum class BuildProfile {
    Unsupported,
    ReferenceC051,
    V13EnglishCollectors,
    ReforgedC5C5,
};

bool SafeRead(const void* address, void* destination, std::size_t size);
bool IsWritable(const void* address, std::size_t size);
bool SafeWrite(void* address, const void* source, std::size_t size);

bool ComputeFileMd5(const char* path, char output[33]);

template <typename T>
bool ReadField(const void* base, std::size_t offset, T* destination) {
    if (base == nullptr || destination == nullptr) return false;
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(base);
    if (offset > UINTPTR_MAX - value) return false;
    return SafeRead(reinterpret_cast<const void*>(value + offset),
                    destination, sizeof(T));
}

std::uint32_t BStringHash(const char* text);

bool ReadAttributeDefinition(const void* collectionClass, std::uint32_t key,
                             AttributeDefinition* definition,
                             const void** definitionAddress);

bool QueryExecutableFingerprint(ExecutableFingerprint* fingerprint,
                                char* reason, std::size_t reasonSize);

BuildProfile IdentifyBuildProfile(
    const ExecutableFingerprint& fingerprint) noexcept;
const char* BuildProfileName(BuildProfile profile) noexcept;
bool IsPhase3ProfileSupported(BuildProfile profile) noexcept;

// Phase 3 is restricted to the exact C5C5 profile and validates every
// game-code address it calls or hooks before MinHook changes any entry point.
bool VerifySupportedBuild(const ExecutableFingerprint& fingerprint,
                          BuildProfile* profile,
                          char* reason, std::size_t reasonSize);
bool VerifySupportedBuild(BuildProfile* profile,
                          char* reason, std::size_t reasonSize);

}  // namespace game_access
