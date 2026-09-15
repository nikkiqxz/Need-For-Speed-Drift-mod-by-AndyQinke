#include "NativeAudio.hpp"
#include "NativeLog.hpp"
#include "NitrousActivity.hpp"
#include "StartupGate.hpp"
#include "SunSetLighting.hpp"
#include "nfsmw_exhaust/PluginApi.hpp"
#include "nfsmw_exhaust/Types.hpp"

#include <MinHook.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using nfsmw_exhaust::native_log::write;

constexpr std::uintptr_t kImageBase = 0x00400000u;
constexpr std::uint32_t kExpectedTimestamp = 0x438E4C8Cu;
constexpr std::uint32_t kExpectedImageSize = 0x00693000u;
constexpr std::uint32_t kExpectedEntryRva = 0x003C4040u;
constexpr std::uint32_t kExpectedChecksum = 0x005DF89Du;

constexpr std::uintptr_t kGameFrameTick = 0x00663D30u;
constexpr std::uintptr_t kCarRenderConnOnLoaded = 0x00750B20u;
constexpr std::uintptr_t kCarRenderConnHandleFxEvent = 0x00739070u;
constexpr std::uintptr_t kEmitOneShot = 0x00744980u;
constexpr std::uintptr_t kUpdateEmitter = 0x00744A50u;
constexpr std::uintptr_t kAttributeLookup = 0x00454810u;
constexpr std::uintptr_t kVehicleTable = 0x009352B0u;
constexpr std::uintptr_t kPVehicleVtable = 0x008AA9D8u;
constexpr std::uintptr_t kEngineVtable = 0x008AB6E0u;
constexpr std::uintptr_t kTransmissionVtable = 0x008AB720u;
constexpr std::uintptr_t kRenderableVtable = 0x008ADCC8u;
constexpr std::uintptr_t kGetRpm = 0x006A03A0u;
constexpr std::uintptr_t kGetMaxRpm = 0x006A03B0u;
constexpr std::uintptr_t kGetRedline = 0x006A03D0u;
constexpr std::uintptr_t kGetGear = 0x006A0590u;
constexpr std::uintptr_t kIsGearChanging = 0x006A05A0u;
constexpr std::uintptr_t kGetModel = 0x006B8F30u;
constexpr std::uintptr_t kGetPositionMarker = 0x005016D0u;
constexpr std::uint32_t kLeftExhaustHash = 0xBCF8A18Bu;
constexpr std::uint32_t kRightExhaustHash = 0xBD7CF15Eu;
constexpr std::uint32_t kContinuousBackfireEffectAttributeHash = 0x60CEC115u;
constexpr std::uint32_t kEmitterTimeStepBits = 0x3C088889u;
constexpr std::uint64_t kPluginFlamePulseMs = 550u;
constexpr std::uint64_t kMultiOutletStepMs = 300u;
constexpr float kPluginFlameIntensity = 1.0f;

constexpr std::uint32_t kSunSetTimestamp1152 = 0x6A4A2A2Eu;
constexpr std::uint32_t kSunSetImageSize1152 = 0x00041000u;
constexpr std::uintptr_t kSunSetPopulateLightsRva1152 = 0x0000B410u;
constexpr std::uintptr_t kSunSetLightCountRva1152 = 0x00030C9Cu;
constexpr std::uintptr_t kSunSetLightBufferRva1152 = 0x00032830u;
constexpr std::uintptr_t kSunSetWeatherLightPowerRva1152 = 0x00030234u;
constexpr std::uintptr_t kSunSetExhaustLightRva1152 = 0x0003C678u;
constexpr std::size_t kSunSetLightCapacity = 512u;
constexpr std::int32_t kSunSetPlayerBrakeLightSource = 4;

constexpr std::size_t kEngineOffset = 0xF4u;
constexpr std::size_t kInputOffset = 0xE8u;
constexpr std::size_t kTransmissionOffset = 0xFCu;
constexpr std::size_t kRenderableOffset = 0x108u;
constexpr std::size_t kMaxVehicleSlots = 128u;
constexpr std::size_t kMaxConnections = 128u;
constexpr std::size_t kMaxExhaustMarkers = 16u;
constexpr std::size_t kMaxNitrousEmitters = 16u;
constexpr std::uint64_t kStatusLogIntervalMs = 1000u;
constexpr std::uintptr_t kCarRenderConnVtable = 0x008B5254u;

constexpr std::array<std::uint8_t, 16> kGameFrameTickBytes{{
    0xE8, 0x0B, 0x91, 0xDF, 0xFF, 0xE8, 0x16, 0x39,
    0x00, 0x00, 0xDB, 0x44, 0x24, 0x04, 0x51, 0xD8}};
constexpr std::array<std::uint8_t, 16> kCarRenderConnOnLoadedBytes{{
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC,
    0x54, 0x53, 0x56, 0x57, 0x8B, 0x7D, 0x08, 0x8B}};
constexpr std::array<std::uint8_t, 16> kCarRenderConnHandleFxEventBytes{{
    0x8B, 0x44, 0x24, 0x04, 0x83, 0xE8, 0x00, 0x74,
    0x22, 0x83, 0xE8, 0x03, 0x74, 0x10, 0x48, 0x75}};
constexpr std::array<std::uint8_t, 16> kEmitOneShotBytes{{
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF,
    0x68, 0x98, 0x0B, 0x88, 0x00, 0x64, 0xA1, 0x00}};
constexpr std::array<std::uint8_t, 16> kUpdateEmitterBytes{{
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF,
    0x68, 0xB8, 0x0B, 0x88, 0x00, 0x64, 0xA1, 0x00}};
constexpr std::array<std::uint8_t, 16> kAttributeLookupBytes{{
    0x8B, 0x49, 0x04, 0x85, 0xC9, 0x74, 0x05, 0xE9,
    0x74, 0xF9, 0xFF, 0xFF, 0x33, 0xC0, 0xC2, 0x08}};
constexpr std::array<std::uint8_t, 7> kGetRpmBytes{{
    0xD9, 0x81, 0x2C, 0x01, 0x00, 0x00, 0xC3}};
constexpr std::array<std::uint8_t, 10> kGetMaxRpmBytes{{
    0x8B, 0x81, 0xF8, 0x00, 0x00, 0x00, 0xD9, 0x40, 0x54, 0xC3}};
constexpr std::array<std::uint8_t, 10> kGetRedlineBytes{{
    0x8B, 0x81, 0xF8, 0x00, 0x00, 0x00, 0xD9, 0x40, 0x58, 0xC3}};
constexpr std::array<std::uint8_t, 4> kGetGearBytes{{
    0x8B, 0x41, 0x38, 0xC3}};
constexpr std::array<std::uint8_t, 22> kIsGearChangingBytes{{
    0xD9, 0x41, 0x3C, 0xD8, 0x1D, 0x68, 0x09, 0x89,
    0x00, 0xDF, 0xE0, 0xF6, 0xC4, 0x41, 0x75, 0x03,
    0xB0, 0x01, 0xC3, 0x32, 0xC0, 0xC3}};
constexpr std::array<std::uint8_t, 14> kGetModelBytes{{
    0x8D, 0x41, 0xB4, 0x85, 0xC0, 0x74, 0x04,
    0x8D, 0x41, 0x08, 0xC3, 0x33, 0xC0, 0xC3}};
constexpr std::array<std::uint8_t, 17> kGetPositionMarkerBytes{{
    0x8B, 0x49, 0x0C, 0x85, 0xC9, 0x74, 0x05, 0xE9, 0xE4,
    0xA3, 0xFF, 0xFF, 0x33, 0xC0, 0xC2, 0x04, 0x00}};
constexpr std::array<std::uint8_t, 9> kSunSetPopulateLightsPrefix1152{{
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x68}};
constexpr std::array<std::uint8_t, 13> kSunSetPopulateLightsSuffix1152{{
    0x33, 0xC4, 0x89, 0x44, 0x24, 0x64, 0x83,
    0x3D, 0x90, 0x5E, 0x92, 0x00, 0x06}};
constexpr std::array<std::uint8_t, 5> kSunSetPopulateLightsCall1152{{
    0xE8, 0xCF, 0xF5, 0xFF, 0xFF}};
constexpr std::array<std::uintptr_t, 7> kRenderableFunctions{{
    0x006BC260u, 0x006BBBA0u, 0x006BBBB0u, 0x006B90C0u,
    0x006B8F30u, 0x006B8F20u, 0x006BBBD0u}};

struct PositionMarkerData {
    std::uint32_t nameHash;
    std::int32_t integerParameter;
    float floatParameter0;
    float floatParameter1;
    float matrix[16];
};
static_assert(sizeof(PositionMarkerData) == 0x50,
              "unexpected position marker layout");

struct DriverControlsData {
    float banking;
    float steering;
    float steeringVertical;
    float strafeVertical;
    float strafeHorizontal;
    float gas;
    float brake;
    float handBrake;
    std::uint8_t actionButton;
    std::uint8_t nitrous;
    std::uint8_t padding[2];
};
static_assert(sizeof(DriverControlsData) == 0x24,
              "unexpected input controls layout");

struct MarkerProbe {
    void* pointer = nullptr;
    PositionMarkerData data{};
    bool valid = false;
};

struct ExhaustMarkerNode {
    void* next;
    float x;
    float y;
    float z;
    void* markerData;
};
static_assert(sizeof(ExhaustMarkerNode) == 0x14,
              "unexpected exhaust marker node layout");

struct MultiOutletSequence {
    std::uint32_t vehicleId = 0;
    std::uint32_t sequenceId = 0;
    std::uint64_t expiresAtMs = 0;
};

struct ConnectionRecord {
    void* connection = nullptr;
    void* renderInfo = nullptr;
    std::array<void*, kMaxExhaustMarkers> emitters{};
    std::array<std::uint32_t, kMaxExhaustMarkers> markerHashes{};
    std::array<std::array<float, 3>, kMaxExhaustMarkers> markerPositions{};
    std::array<std::uint32_t, kMaxExhaustMarkers> lastUpdateEffectKeys{};
    std::array<std::uint32_t, kMaxExhaustMarkers> lastStoredEffectKeys{};
    std::array<void*, kMaxExhaustMarkers> lastParticleInstances{};
    std::array<bool, kMaxExhaustMarkers> updateObserved{};
    std::array<std::uint64_t, kMaxExhaustMarkers> pluginPulseStartMs{};
    std::array<std::uint64_t, kMaxExhaustMarkers> pluginPulseUntilMs{};
    std::array<std::uint64_t, kMaxExhaustMarkers> pluginPulseLastUpdateMs{};
    std::array<std::uint32_t, kMaxExhaustMarkers> pluginPulseEffectKeys{};
    std::array<MultiOutletSequence, 8> multiOutletSequences{};
    std::size_t emitterCount = 0;
    std::array<void*, kMaxNitrousEmitters> nitrousEmitters{};
    std::size_t nitrousEmitterCount = 0;
    std::uint64_t lastNitrousUpdateMs = 0;
    std::uint32_t generation = 0;
};

struct VehicleTableEntry {
    void* vehicle;
    std::uint8_t enabled;
    std::uint8_t padding[3];
};
static_assert(sizeof(VehicleTableEntry) == 8, "unexpected vehicle table entry");

struct VehicleRecord {
    void* pointer = nullptr;
    std::uint32_t id = 0;
    std::int32_t previousGear = 0;
    bool previousGearValid = false;
    bool previousShift = false;
    bool seen = false;
    bool layoutLogged = false;
    bool suppressVanillaBackfire = false;
    std::uint64_t lastLogMs = 0;
};

struct CachedVehicle {
    NfswExhaustVehicleSnapshotC snapshot{};
    void* pointer = nullptr;
    void* connection = nullptr;
    float rawRpm = 0.0f;
    float rawRedline = 0.0f;
    float rawMaxRpm = 0.0f;
};

struct VehicleProbe {
    void* pvehicle = nullptr;
    std::uintptr_t pvehicleVtable = 0;
    void* input = nullptr;
    std::uintptr_t inputVtable = 0;
    void* engine = nullptr;
    std::uintptr_t engineVtable = 0;
    void* transmission = nullptr;
    std::uintptr_t transmissionVtable = 0;
    void* renderable = nullptr;
    std::uintptr_t renderableVtable = 0;
    std::array<std::uintptr_t, 7> renderableFunctions{};
    void* model = nullptr;
    MarkerProbe leftMarker{};
    MarkerProbe rightMarker{};
    void* renderableOwner = nullptr;
    std::array<std::uint32_t, 50> renderableOwnerWords{};
    bool renderableOwnerReadable = false;
};

struct ResolvedVehicle {
    void* pvehicle = nullptr;
    void* input = nullptr;
    void* engine = nullptr;
    void* transmission = nullptr;
};

std::array<VehicleRecord, kMaxVehicleSlots> g_records{};
std::array<CachedVehicle, kMaxVehicleSlots> g_cached{};
std::array<ConnectionRecord, kMaxConnections> g_connections{};
std::uint32_t g_cachedCount = 0;
std::uint32_t g_nextVehicleId = 1;
std::uint32_t g_connectionGeneration = 0;
std::uint64_t g_lastVehicleScanFailureLogMs = 0;
std::uint32_t g_outletRandomState = 0xA53C9E17u;
HMODULE g_pluginModule = nullptr;
using GameFrameTickFn = void(__cdecl*)(std::uint32_t elapsedMs);
using FloatGetterFn = float(__thiscall*)(void* object);
using GearGetterFn = std::uint32_t(__thiscall*)(void* object);
using BoolGetterFn = bool(__thiscall*)(void* object);
using ModelGetterFn = void*(__thiscall*)(void* object);
using ControlsGetterFn = void*(__thiscall*)(void* object);
using PositionMarkerFn = void*(__thiscall*)(void* model,
                                            std::uint32_t markerHash);
using AttributeLookupFn = void*(__thiscall*)(void* collection,
                                             std::uint32_t attributeHash,
                                             std::uint32_t index);
using CarRenderConnOnLoadedFn = void(__thiscall*)(void* connection,
                                                  void* renderInfo);
using CarRenderConnHandleFxEventFn = void(__thiscall*)(void* connection,
                                                       std::uint32_t eventCode);
using EmitOneShotFn = void(__thiscall*)(void* emitter, const void* parentMatrix,
                                        std::uint32_t effectKey, float intensity,
                                        const void* velocity);
using UpdateEmitterFn = void(__thiscall*)(void* emitter, const void* parentMatrix,
                                         std::uint32_t effectKey,
                                         std::uint32_t parameter,
                                         float intensity,
                                         const void* velocity);
using SunSetPopulateLightsFn = void(__cdecl*)();
GameFrameTickFn g_originalGameFrameTick = nullptr;
CarRenderConnOnLoadedFn g_originalCarRenderConnOnLoaded = nullptr;
CarRenderConnHandleFxEventFn g_originalCarRenderConnHandleFxEvent = nullptr;
EmitOneShotFn g_originalEmitOneShot = nullptr;
UpdateEmitterFn g_originalUpdateEmitter = nullptr;
SunSetPopulateLightsFn g_originalSunSetPopulateLights = nullptr;

enum class SunSetHookState : std::uint8_t { Waiting, Installed, Unsupported };
SunSetHookState g_sunSetHookState = SunSetHookState::Waiting;
std::uintptr_t g_sunSetLightCount = 0;
std::uintptr_t g_sunSetLightBuffer = 0;
std::uintptr_t g_sunSetWeatherLightPower = 0;
std::uintptr_t g_sunSetExhaustLight = 0;

bool isReadable(const void* address, std::size_t size) noexcept {
    if (address == nullptr || size == 0) return false;
    auto current = reinterpret_cast<std::uintptr_t>(address);
    if (size - 1u > UINTPTR_MAX - current) return false;
    const std::uintptr_t end = current + size;
    while (current < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &region,
                         sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        const std::uintptr_t regionEnd =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress) +
            static_cast<std::uintptr_t>(region.RegionSize);
        if (regionEnd <= current) return false;
        current = std::min(end, regionEnd);
    }
    return true;
}

bool safeRead(const void* address, void* destination, std::size_t size) noexcept {
    if (destination == nullptr || !isReadable(address, size)) return false;
    __try {
        std::memcpy(destination, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool isWritable(void* address, std::size_t size) noexcept {
    if (address == nullptr || size == 0) return false;
    auto current = reinterpret_cast<std::uintptr_t>(address);
    if (size - 1u > UINTPTR_MAX - current) return false;
    const std::uintptr_t end = current + size;
    while (current < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &region,
                         sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        const DWORD protection = region.Protect & 0xFFu;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY) {
            return false;
        }
        const std::uintptr_t regionEnd =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress) +
            static_cast<std::uintptr_t>(region.RegionSize);
        if (regionEnd <= current) return false;
        current = std::min(end, regionEnd);
    }
    return true;
}

bool safeWrite(void* address, const void* source, std::size_t size) noexcept {
    if (source == nullptr || !isWritable(address, size)) return false;
    __try {
        std::memcpy(address, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool readValue(const void* base, std::size_t offset, T* output) noexcept {
    if (base == nullptr || output == nullptr) return false;
    const auto value = reinterpret_cast<std::uintptr_t>(base);
    if (offset > UINTPTR_MAX - value) return false;
    return safeRead(reinterpret_cast<const void*>(value + offset), output,
                    sizeof(T));
}

template <std::size_t Size>
bool hasBytes(std::uintptr_t address,
              const std::array<std::uint8_t, Size>& expected) noexcept {
    std::array<std::uint8_t, Size> actual{};
    return safeRead(reinterpret_cast<const void*>(address), actual.data(), Size) &&
           actual == expected;
}

bool verifyHost(wchar_t path[MAX_PATH]) noexcept {
    HMODULE host = GetModuleHandleA(nullptr);
    if (host == nullptr || reinterpret_cast<std::uintptr_t>(host) != kImageBase) {
        write("GATE_REJECT loaded image base is not 0x%08X",
              static_cast<unsigned>(kImageBase));
        return false;
    }
    const DWORD pathSize = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (pathSize == 0 || pathSize >= MAX_PATH) {
        write("GATE_REJECT could not resolve main executable path");
        return false;
    }
    char reason[192] = {};
    if (!nfsmw_exhaust::startup_gate::ValidateExecutable(
            path, reason, sizeof(reason))) {
        write("GATE_REJECT %s", reason[0] == '\0' ? "validation failed" : reason);
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!safeRead(host, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 0x1000 ||
        !safeRead(reinterpret_cast<const std::uint8_t*>(host) + dos.e_lfanew,
                  &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.OptionalHeader.ImageBase != kImageBase ||
        nt.FileHeader.TimeDateStamp != kExpectedTimestamp ||
        nt.OptionalHeader.SizeOfImage != kExpectedImageSize ||
        nt.OptionalHeader.AddressOfEntryPoint != kExpectedEntryRva ||
        nt.OptionalHeader.CheckSum != kExpectedChecksum) {
        write("GATE_REJECT PE32 header does not match the target build");
        return false;
    }

    if (!hasBytes(kGameFrameTick, kGameFrameTickBytes) ||
        !hasBytes(kCarRenderConnOnLoaded, kCarRenderConnOnLoadedBytes) ||
        !hasBytes(kCarRenderConnHandleFxEvent,
                  kCarRenderConnHandleFxEventBytes) ||
        !hasBytes(kEmitOneShot, kEmitOneShotBytes) ||
        !hasBytes(kUpdateEmitter, kUpdateEmitterBytes) ||
        !hasBytes(kAttributeLookup, kAttributeLookupBytes) ||
        !hasBytes(kGetRpm, kGetRpmBytes) ||
        !hasBytes(kGetMaxRpm, kGetMaxRpmBytes) ||
        !hasBytes(kGetRedline, kGetRedlineBytes) ||
        !hasBytes(kGetGear, kGetGearBytes) ||
        !hasBytes(kIsGearChanging, kIsGearChangingBytes) ||
        !hasBytes(kGetModel, kGetModelBytes) ||
        !hasBytes(kGetPositionMarker, kGetPositionMarkerBytes)) {
        write("GATE_REJECT one or more required code signatures changed");
        return false;
    }
    nfsmw_exhaust::startup_gate::BindingResult binding{};
    if (!nfsmw_exhaust::startup_gate::EnsureCurrentDeviceBinding(
            path, &binding, reason, sizeof(reason))) {
        write("GATE_REJECT device binding: %s",
              reason[0] == '\0' ? "validation failed" : reason);
        return false;
    }
    write("GATE_ACCEPT path='%ls' verificationCode=%u sha256=%s "
          "deviceBinding=%s sourceMask=0x%X",
          path,
          static_cast<unsigned>(
              nfsmw_exhaust::startup_gate::kVerificationCode),
          nfsmw_exhaust::startup_gate::kStampedExecutableSha256,
          binding.status ==
                  nfsmw_exhaust::startup_gate::BindingStatus::Created
              ? "created"
              : "verified",
          binding.sourceMask);
    return true;
}

bool callModelGetter(void* renderable, void** output) noexcept {
    if (renderable == nullptr || output == nullptr) return false;
    __try {
        *output = reinterpret_cast<ModelGetterFn>(kGetModel)(renderable);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *output = nullptr;
        return false;
    }
}

bool queryMarker(void* model, std::uint32_t hash,
                 MarkerProbe* output) noexcept {
    if (model == nullptr || output == nullptr) return false;
    *output = {};
    __try {
        output->pointer =
            reinterpret_cast<PositionMarkerFn>(kGetPositionMarker)(model, hash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output->pointer = nullptr;
        return false;
    }
    if (output->pointer == nullptr) return true;
    if (!safeRead(output->pointer, &output->data, sizeof(output->data)) ||
        output->data.nameHash != hash) {
        output->pointer = nullptr;
        return false;
    }
    for (float value : output->data.matrix) {
        if (!std::isfinite(value)) {
            output->pointer = nullptr;
            return false;
        }
    }
    output->valid = true;
    return true;
}

void probeMarkers(VehicleProbe* probe) noexcept {
    if (probe == nullptr || probe->renderable == nullptr ||
        probe->renderableVtable != kRenderableVtable ||
        probe->renderableFunctions != kRenderableFunctions) {
        return;
    }
    if (!callModelGetter(probe->renderable, &probe->model)) return;
    const auto expectedModel = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(probe->renderable) + 8u);
    if (probe->model != expectedModel || !isReadable(probe->model, 0x10u)) {
        probe->model = nullptr;
        return;
    }
    queryMarker(probe->model, kLeftExhaustHash, &probe->leftMarker);
    queryMarker(probe->model, kRightExhaustHash, &probe->rightMarker);
}

bool callFloat(std::uintptr_t function, void* object, float* output) noexcept {
    if (object == nullptr || output == nullptr) return false;
    __try {
        *output = reinterpret_cast<FloatGetterFn>(function)(object);
        return std::isfinite(*output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callGear(void* object, std::uint32_t* output) noexcept {
    if (object == nullptr || output == nullptr) return false;
    __try {
        *output = reinterpret_cast<GearGetterFn>(kGetGear)(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callShift(void* object, bool* output) noexcept {
    if (object == nullptr || output == nullptr) return false;
    __try {
        *output = reinterpret_cast<BoolGetterFn>(kIsGearChanging)(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool callDriverControls(void* input, DriverControlsData* output) noexcept {
    if (input == nullptr || output == nullptr) return false;
    std::uintptr_t vtable = 0;
    ControlsGetterFn getter = nullptr;
    if (!safeRead(input, &vtable, sizeof(vtable)) ||
        vtable < kImageBase || vtable >= kImageBase + kExpectedImageSize ||
        !readValue(reinterpret_cast<const void*>(vtable),
                   2u * sizeof(void*), &getter)) {
        return false;
    }
    const std::uintptr_t function = reinterpret_cast<std::uintptr_t>(getter);
    if (function < kImageBase || function >= kImageBase + kExpectedImageSize) {
        return false;
    }
    void* controls = nullptr;
    __try {
        controls = getter(input);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    DriverControlsData candidate{};
    if (!safeRead(controls, &candidate, sizeof(candidate)) ||
        !std::isfinite(candidate.gas) || !std::isfinite(candidate.brake) ||
        !std::isfinite(candidate.handBrake) || candidate.gas < -0.05f ||
        candidate.gas > 1.25f || candidate.brake < -0.05f ||
        candidate.brake > 1.25f || candidate.handBrake < -0.05f ||
        candidate.handBrake > 1.25f) {
        return false;
    }
    candidate.gas = std::clamp(candidate.gas, 0.0f, 1.0f);
    candidate.brake = std::clamp(candidate.brake, 0.0f, 1.0f);
    candidate.handBrake = std::clamp(candidate.handBrake, 0.0f, 1.0f);
    *output = candidate;
    return true;
}

bool readAttributeValue(void* connection, std::uint32_t attributeHash,
                        std::uint32_t* output) noexcept {
    if (connection == nullptr || output == nullptr) return false;
    void* entry = nullptr;
    __try {
        auto* collection = reinterpret_cast<void*>(
            reinterpret_cast<std::uintptr_t>(connection) + 0x14u);
        entry = reinterpret_cast<AttributeLookupFn>(kAttributeLookup)(
            collection, attributeHash, 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return entry != nullptr && readValue(entry, 0x4u, output) && *output != 0;
}

VehicleRecord* recordFor(void* vehicle) noexcept {
    for (auto& record : g_records) {
        if (record.pointer == vehicle) return &record;
    }
    for (auto& record : g_records) {
        if (record.pointer == nullptr) {
            record = {};
            record.pointer = vehicle;
            record.id = g_nextVehicleId++;
            if (g_nextVehicleId == 0) g_nextVehicleId = 1;
            write("VEHICLE_ATTACH id=%u pointer=%p", record.id, vehicle);
            return &record;
        }
    }
    return nullptr;
}

void normalizeRpm(float rawRpm, float rawRedline, float rawMaxRpm,
                  float* rpm, float* redline, float* maxRpm) noexcept {
    const float scale = rawMaxRpm > 0.0f && rawMaxRpm < 100.0f ? 1000.0f : 1.0f;
    *rpm = rawRpm * scale;
    *redline = rawRedline * scale;
    *maxRpm = rawMaxRpm * scale;
}

bool probePVehicle(void* raw, VehicleProbe* probe) noexcept {
    if (raw == nullptr || probe == nullptr) return false;
    *probe = {};
    probe->pvehicle = raw;
    safeRead(raw, &probe->pvehicleVtable, sizeof(probe->pvehicleVtable));
    readValue(raw, kInputOffset, &probe->input);
    readValue(raw, kEngineOffset, &probe->engine);
    readValue(raw, kTransmissionOffset, &probe->transmission);
    readValue(raw, kRenderableOffset, &probe->renderable);
    safeRead(probe->input, &probe->inputVtable, sizeof(probe->inputVtable));
    safeRead(probe->engine, &probe->engineVtable, sizeof(probe->engineVtable));
    safeRead(probe->transmission, &probe->transmissionVtable,
             sizeof(probe->transmissionVtable));
    safeRead(probe->renderable, &probe->renderableVtable,
             sizeof(probe->renderableVtable));
    if (probe->renderableVtable != 0) {
        safeRead(reinterpret_cast<const void*>(probe->renderableVtable),
                 probe->renderableFunctions.data(),
                 sizeof(probe->renderableFunctions));
    }
    if (probe->renderable != nullptr &&
        reinterpret_cast<std::uintptr_t>(probe->renderable) >= 0x4Cu) {
        probe->renderableOwner = reinterpret_cast<void*>(
            reinterpret_cast<std::uintptr_t>(probe->renderable) - 0x4Cu);
        probe->renderableOwnerReadable =
            safeRead(probe->renderableOwner,
                     probe->renderableOwnerWords.data(),
                     sizeof(probe->renderableOwnerWords));
    }
    probeMarkers(probe);

    return probe->pvehicleVtable == kPVehicleVtable &&
           probe->engineVtable == kEngineVtable &&
           probe->transmissionVtable == kTransmissionVtable;
}

void logRenderableOwnerWords(std::uint32_t vehicleId,
                             const VehicleProbe& probe) noexcept {
    if (!probe.renderableOwnerReadable) {
        write("RENDERABLE_OWNER id=%u owner=%p unreadable", vehicleId,
              probe.renderableOwner);
        return;
    }
    for (std::size_t first = 0; first < probe.renderableOwnerWords.size();
         first += 13) {
        char values[512] = {};
        std::size_t used = 0;
        const std::size_t end =
            std::min(first + 13, probe.renderableOwnerWords.size());
        for (std::size_t index = first; index < end; ++index) {
            const int count = std::snprintf(
                values + used, sizeof(values) - used, "%s%02X=%08X",
                index == first ? "" : " ",
                static_cast<unsigned>(index * sizeof(std::uint32_t)),
                static_cast<unsigned>(probe.renderableOwnerWords[index]));
            if (count < 0 || static_cast<std::size_t>(count) >=
                                 sizeof(values) - used) {
                break;
            }
            used += static_cast<std::size_t>(count);
        }
        write("RENDERABLE_OWNER id=%u owner=%p fields[%s]", vehicleId,
              probe.renderableOwner, values);
    }
}

bool resolveVehicle(void* raw, ResolvedVehicle* resolved,
                    VehicleProbe* probe) noexcept {
    if (resolved == nullptr || probe == nullptr) return false;
    *resolved = {};
    if (!probePVehicle(raw, probe)) return false;
    resolved->pvehicle = raw;
    resolved->input = probe->input;
    resolved->engine = probe->engine;
    resolved->transmission = probe->transmission;
    return true;
}

void populateExhaustMarkers(const VehicleProbe& probe,
                            NfswExhaustVehicleSnapshotC* snapshot) noexcept;
const ConnectionRecord* findConnection(void* connection) noexcept;
bool connectionOwnsEmitter(const ConnectionRecord& record,
                           void* emitter) noexcept;

void logVehicleResolveFailure(
    std::size_t index, const VehicleProbe& probe,
    std::uint64_t nowMs) noexcept {
    if (index != 0 ||
        (g_lastVehicleScanFailureLogMs != 0 &&
         nowMs - g_lastVehicleScanFailureLogMs < kStatusLogIntervalMs)) {
        return;
    }
    g_lastVehicleScanFailureLogMs = nowMs;
    write("PVEHICLE_SCAN slot=0 ptr=%p vt=%p "
          "engine(+0xF4)=%p evt=%p transmission(+0xFC)=%p tvt=%p "
          "renderable(+0x108)=%p rvt=%p",
          probe.pvehicle, reinterpret_cast<void*>(probe.pvehicleVtable),
          probe.engine, reinterpret_cast<void*>(probe.engineVtable),
          probe.transmission,
          reinterpret_cast<void*>(probe.transmissionVtable), probe.renderable,
          reinterpret_cast<void*>(probe.renderableVtable));
}

void scanVehicles(std::uint64_t nowMs) noexcept {
    for (auto& record : g_records) record.seen = false;
    g_cachedCount = 0;

    for (std::size_t index = 0; index < kMaxVehicleSlots; ++index) {
        VehicleTableEntry entry{};
        const auto address = reinterpret_cast<const void*>(
            kVehicleTable + index * sizeof(VehicleTableEntry));
        if (!safeRead(address, &entry, sizeof(entry))) {
            write("VEHICLE_SCAN stopped at unreadable table slot %zu", index);
            break;
        }
        if (entry.vehicle == nullptr) break;

        ResolvedVehicle vehicle{};
        VehicleProbe probe{};
        if (!resolveVehicle(entry.vehicle, &vehicle, &probe)) {
            logVehicleResolveFailure(index, probe, nowMs);
            continue;
        }

        float rawRpm = 0.0f;
        float rawRedline = 0.0f;
        float rawMaxRpm = 0.0f;
        std::uint32_t rawGear = 0;
        bool shifting = false;
        if (!callFloat(kGetRpm, vehicle.engine, &rawRpm) ||
            !callFloat(kGetRedline, vehicle.engine, &rawRedline) ||
            !callFloat(kGetMaxRpm, vehicle.engine, &rawMaxRpm) ||
            !callGear(vehicle.transmission, &rawGear) ||
            !callShift(vehicle.transmission, &shifting)) {
            continue;
        }
        DriverControlsData controls{};
        const bool controlsValid =
            callDriverControls(vehicle.input, &controls);

        VehicleRecord* record = recordFor(vehicle.pvehicle);
        if (record == nullptr || g_cachedCount >= g_cached.size()) break;
        record->seen = true;
        if (!record->layoutLogged) {
            logRenderableOwnerWords(record->id, probe);
            record->layoutLogged = true;
        }
        CachedVehicle& cached = g_cached[g_cachedCount++];
        cached = {};
        cached.pointer = vehicle.pvehicle;
        cached.rawRpm = rawRpm;
        cached.rawRedline = rawRedline;
        cached.rawMaxRpm = rawMaxRpm;

        auto& snapshot = cached.snapshot;
        snapshot.id = record->id;
        snapshot.valid = entry.enabled != 0 ? 1 : 0;
        normalizeRpm(rawRpm, rawRedline, rawMaxRpm, &snapshot.rpm,
                     &snapshot.redlineRpm, &snapshot.maxRpm);
        snapshot.gear = static_cast<std::int32_t>(rawGear);
        snapshot.driverControlsValid = controlsValid ? 1 : 0;
        snapshot.gasInput = controls.gas;
        snapshot.brakeInput = controls.brake;
        snapshot.handBrakeInput = controls.handBrake;
        snapshot.nitrousActive = 0;
        snapshot.shiftInProgress = shifting ? 1 : 0;
        snapshot.shiftEvent = shifting && !record->previousShift ? 1 : 0;
        snapshot.gearChanged =
            record->previousGearValid && snapshot.gear != record->previousGear
                ? 1
                : 0;
        if (snapshot.gearChanged) {
            snapshot.shiftDirection = snapshot.gear > record->previousGear ? 1 : 2;
        }
        snapshot.atRedline =
            snapshot.redlineRpm > 0.0f &&
                    snapshot.rpm >= snapshot.redlineRpm * 0.995f
                ? 1
                : 0;
        snapshot.atMaxRpm =
            snapshot.maxRpm > 0.0f &&
                    snapshot.rpm >= snapshot.maxRpm * 0.995f
                ? 1
                : 0;
        populateExhaustMarkers(probe, &snapshot);
        void* connection = nullptr;
        const ConnectionRecord* connectionRecord = nullptr;
        if (readValue(probe.renderable, 0x38u, &connection)) {
            connectionRecord = findConnection(connection);
            if (connectionRecord != nullptr) {
                snapshot.nitrousActive =
                    nfsmw_exhaust::native_adapter::isNitrousEmitterActive(
                        connectionRecord->nitrousEmitterCount,
                        connectionRecord->lastNitrousUpdateMs, nowMs)
                        ? 1
                        : 0;
                }
        }
        if (connectionRecord != nullptr) {
            cached.connection = connection;
        }

        const bool event = snapshot.shiftEvent != 0 || snapshot.gearChanged != 0;
        if (event || record->lastLogMs == 0 ||
            nowMs - record->lastLogMs >= kStatusLogIntervalMs) {
            write("VEHICLE id=%u pvehicle=%p pvt=%p enabled=%u "
                  "input=%p ivt=%p engine=%p evt=%p transmission=%p tvt=%p "
                  "renderable=%p rvt=%p model=%p "
                  "rfn=[%p,%p,%p,%p,%p,%p,%p] "
                  "markerQuery=L%d@%p/R%d@%p "
                  "localPos=L(%.3f,%.3f,%.3f)/R(%.3f,%.3f,%.3f) "
                  "raw[rpm=%.6g redline=%.6g max=%.6g] "
                  "normalized[rpm=%.1f redline=%.1f max=%.1f] gear=%d "
                  "shifting=%d shiftEdge=%d gearChanged=%d direction=%d "
                  "controls=%d gas=%.3f brake=%.3f handbrake=%.3f "
                  "nosButton=%d nosFx=%d "
                  "markers=L%d/R%d markerPos=L(%.3f,%.3f,%.3f)/R(%.3f,%.3f,%.3f) "
                  "diagnostic=1",
                  snapshot.id, vehicle.pvehicle,
                  reinterpret_cast<void*>(probe.pvehicleVtable),
                  static_cast<unsigned>(entry.enabled),
                  vehicle.input, reinterpret_cast<void*>(probe.inputVtable),
                  vehicle.engine, reinterpret_cast<void*>(probe.engineVtable),
                  vehicle.transmission,
                  reinterpret_cast<void*>(probe.transmissionVtable),
                  probe.renderable,
                  reinterpret_cast<void*>(probe.renderableVtable),
                  probe.model,
                  reinterpret_cast<void*>(probe.renderableFunctions[0]),
                  reinterpret_cast<void*>(probe.renderableFunctions[1]),
                  reinterpret_cast<void*>(probe.renderableFunctions[2]),
                  reinterpret_cast<void*>(probe.renderableFunctions[3]),
                  reinterpret_cast<void*>(probe.renderableFunctions[4]),
                  reinterpret_cast<void*>(probe.renderableFunctions[5]),
                  reinterpret_cast<void*>(probe.renderableFunctions[6]),
                  probe.leftMarker.valid ? 1 : 0, probe.leftMarker.pointer,
                  probe.rightMarker.valid ? 1 : 0, probe.rightMarker.pointer,
                  static_cast<double>(probe.leftMarker.data.matrix[12]),
                  static_cast<double>(probe.leftMarker.data.matrix[13]),
                  static_cast<double>(probe.leftMarker.data.matrix[14]),
                  static_cast<double>(probe.rightMarker.data.matrix[12]),
                  static_cast<double>(probe.rightMarker.data.matrix[13]),
                  static_cast<double>(probe.rightMarker.data.matrix[14]),
                  static_cast<double>(rawRpm), static_cast<double>(rawRedline),
                  static_cast<double>(rawMaxRpm), static_cast<double>(snapshot.rpm),
                  static_cast<double>(snapshot.redlineRpm),
                  static_cast<double>(snapshot.maxRpm), snapshot.gear,
                  snapshot.shiftInProgress, snapshot.shiftEvent,
                  snapshot.gearChanged, snapshot.shiftDirection,
                  snapshot.driverControlsValid,
                  static_cast<double>(snapshot.gasInput),
                  static_cast<double>(snapshot.brakeInput),
                  static_cast<double>(snapshot.handBrakeInput),
                  controlsValid && controls.nitrous != 0 ? 1 : 0,
                  snapshot.nitrousActive,
                  snapshot.leftExhaust.present, snapshot.rightExhaust.present,
                  static_cast<double>(snapshot.leftExhaust.x),
                  static_cast<double>(snapshot.leftExhaust.y),
                  static_cast<double>(snapshot.leftExhaust.z),
                  static_cast<double>(snapshot.rightExhaust.x),
                  static_cast<double>(snapshot.rightExhaust.y),
                  static_cast<double>(snapshot.rightExhaust.z));
            record->lastLogMs = nowMs;
        }
        record->previousGear = snapshot.gear;
        record->previousGearValid = true;
        record->previousShift = shifting;
    }

    for (auto& record : g_records) {
        if (record.pointer != nullptr && !record.seen) {
            write("VEHICLE_DETACH id=%u pointer=%p", record.id, record.pointer);
            record = {};
        }
    }
}

std::uint32_t NFSW_EXHAUST_CALL getVehicleCount(void*) {
    scanVehicles(GetTickCount64());
    return g_cachedCount;
}

int NFSW_EXHAUST_CALL readVehicle(void*, std::uint32_t index,
                                  NfswExhaustVehicleSnapshotC* output) {
    if (output == nullptr || index >= g_cachedCount) return 0;
    *output = g_cached[index].snapshot;
    return 1;
}

CachedVehicle* cachedVehicleForId(std::uint32_t vehicleId) noexcept {
    for (std::uint32_t index = 0; index < g_cachedCount; ++index) {
        if (g_cached[index].snapshot.id == vehicleId) return &g_cached[index];
    }
    return nullptr;
}

bool cachedVehicleAllowsBackfire(const CachedVehicle& vehicle) noexcept {
    return vehicle.snapshot.valid != 0 &&
           vehicle.snapshot.leftExhaust.present != 0 &&
           vehicle.snapshot.rightExhaust.present != 0 &&
           std::isfinite(vehicle.snapshot.rpm) &&
           vehicle.snapshot.rpm > nfsmw_exhaust::kMinimumBackfireRpm;
}

bool connectionAllowsBackfire(const ConnectionRecord& connection) noexcept {
    for (std::uint32_t index = 0; index < g_cachedCount; ++index) {
        if (g_cached[index].connection == connection.connection) {
            return cachedVehicleAllowsBackfire(g_cached[index]);
        }
    }
    return false;
}

void clearPluginFlamePulses(ConnectionRecord& connection) noexcept {
    connection.pluginPulseStartMs.fill(0);
    connection.pluginPulseUntilMs.fill(0);
    connection.pluginPulseLastUpdateMs.fill(0);
    connection.pluginPulseEffectKeys.fill(0);
    connection.multiOutletSequences.fill({});
}

VehicleRecord* vehicleRecordForId(std::uint32_t vehicleId) noexcept {
    for (auto& record : g_records) {
        if (record.pointer != nullptr && record.id == vehicleId) return &record;
    }
    return nullptr;
}

bool shouldSuppressStockBackfire(void* connection,
                                 std::uint32_t* vehicleId) noexcept {
    if (connection == nullptr || findConnection(connection) == nullptr) {
        return false;
    }
    for (std::uint32_t index = 0; index < g_cachedCount; ++index) {
        const CachedVehicle& cached = g_cached[index];
        if (cached.connection != connection || cached.snapshot.valid == 0 ||
            cached.snapshot.leftExhaust.present == 0 ||
            cached.snapshot.rightExhaust.present == 0) {
            continue;
        }
        VehicleRecord* record = vehicleRecordForId(cached.snapshot.id);
        if (record == nullptr || record->pointer != cached.pointer ||
            !record->suppressVanillaBackfire) {
            return false;
        }
        if (vehicleId != nullptr) *vehicleId = cached.snapshot.id;
        return true;
    }
    return false;
}

std::uint32_t nextOutletRandom() noexcept {
    g_outletRandomState ^= g_outletRandomState << 13u;
    g_outletRandomState ^= g_outletRandomState >> 17u;
    g_outletRandomState ^= g_outletRandomState << 5u;
    return g_outletRandomState;
}

void shuffleOutletIndices(std::array<std::size_t, kMaxExhaustMarkers>& indices,
                          std::size_t count) noexcept {
    while (count > 1) {
        const std::size_t selected = nextOutletRandom() % count;
        std::swap(indices[count - 1u], indices[selected]);
        --count;
    }
}

std::size_t collectOutletIndices(
    const ConnectionRecord& connection, std::uint32_t markerHash,
    std::array<std::size_t, kMaxExhaustMarkers>* output) noexcept {
    if (output == nullptr) return 0;
    std::size_t count = 0;
    for (std::size_t index = 0; index < connection.emitterCount; ++index) {
        if (connection.markerHashes[index] == markerHash &&
            connectionOwnsEmitter(connection, connection.emitters[index])) {
            (*output)[count++] = index;
        }
    }
    return count;
}

bool armOutlet(ConnectionRecord& connection, std::size_t index,
               std::uint32_t effectKey, std::uint64_t dueMs,
               std::uint64_t nowMs) noexcept {
    if (index >= connection.emitterCount || effectKey == 0 ||
        !connectionOwnsEmitter(connection, connection.emitters[index])) {
        return false;
    }
    connection.pluginPulseStartMs[index] = dueMs;
    connection.pluginPulseUntilMs[index] = dueMs + kPluginFlamePulseMs;
    connection.pluginPulseEffectKeys[index] = effectKey;
    connection.pluginPulseLastUpdateMs[index] = 0;
    if (dueMs > nowMs) return true;

    void* velocity = nullptr;
    const auto* parentMatrix = reinterpret_cast<const void*>(
        reinterpret_cast<std::uintptr_t>(connection.connection) + 0x330u);
    if (!readValue(connection.connection, 0x38u, &velocity) ||
        !isReadable(parentMatrix, sizeof(float) * 16u)) {
        return false;
    }
    __try {
        g_originalUpdateEmitter(connection.emitters[index], parentMatrix,
                                effectKey, kEmitterTimeStepBits,
                                kPluginFlameIntensity, velocity);
        connection.pluginPulseLastUpdateMs[index] = nowMs;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        connection.pluginPulseStartMs[index] = 0;
        connection.pluginPulseUntilMs[index] = 0;
        connection.pluginPulseEffectKeys[index] = 0;
        return false;
    }
}

MultiOutletSequence* findMultiOutletSequence(ConnectionRecord& connection,
                                              std::uint32_t vehicleId,
                                              std::uint32_t sequenceId,
                                              std::uint64_t nowMs) noexcept {
    for (auto& sequence : connection.multiOutletSequences) {
        if (sequence.expiresAtMs < nowMs) sequence = {};
        if (sequence.vehicleId == vehicleId &&
            sequence.sequenceId == sequenceId && sequenceId != 0) {
            return &sequence;
        }
    }
    return nullptr;
}

void rememberMultiOutletSequence(ConnectionRecord& connection,
                                 std::uint32_t vehicleId,
                                 std::uint32_t sequenceId,
                                 std::uint64_t expiresAtMs) noexcept {
    MultiOutletSequence* slot = &connection.multiOutletSequences[0];
    for (auto& sequence : connection.multiOutletSequences) {
        if (sequence.vehicleId == 0 ||
            sequence.expiresAtMs < slot->expiresAtMs) {
            slot = &sequence;
        }
    }
    *slot = MultiOutletSequence{vehicleId, sequenceId, expiresAtMs};
}

int NFSW_EXHAUST_CALL spawnFlame(void*,
                                 const NfswExhaustFlameRequestC* request) {
    if (request == nullptr || g_originalUpdateEmitter == nullptr ||
        (request->side != NFSW_EXHAUST_SIDE_LEFT &&
         request->side != NFSW_EXHAUST_SIDE_RIGHT)) {
        return 0;
    }
    CachedVehicle* vehicle = cachedVehicleForId(request->vehicleId);
    if (vehicle == nullptr || vehicle->connection == nullptr ||
        !cachedVehicleAllowsBackfire(*vehicle)) {
        return 0;
    }
    const ConnectionRecord* foundConnection = findConnection(vehicle->connection);
    if (foundConnection == nullptr) return 0;
    ConnectionRecord* connection = nullptr;
    for (auto& candidate : g_connections) {
        if (&candidate == foundConnection) {
            connection = &candidate;
            break;
        }
    }
    if (connection == nullptr) return 0;

    std::uint32_t effectKey = 0;
    void* velocity = nullptr;
    auto* parentMatrix = reinterpret_cast<const void*>(
        reinterpret_cast<std::uintptr_t>(connection->connection) + 0x330u);
    if (!readAttributeValue(connection->connection,
                            kContinuousBackfireEffectAttributeHash,
                            &effectKey) ||
        !readValue(connection->connection, 0x38u, &velocity) ||
        !isReadable(parentMatrix, sizeof(float) * 16u)) {
        write("LIVE_FLAME_REJECT vehicle=%u reason=unresolved_context",
              request->vehicleId);
        return 0;
    }

    std::array<std::size_t, kMaxExhaustMarkers> left{};
    std::array<std::size_t, kMaxExhaustMarkers> right{};
    const std::size_t leftCount =
        collectOutletIndices(*connection, kLeftExhaustHash, &left);
    const std::size_t rightCount =
        collectOutletIndices(*connection, kRightExhaustHash, &right);
    if (leftCount == 0 || rightCount == 0) return 0;

    const std::uint64_t nowMs = GetTickCount64();
    const bool multiOutlet = leftCount + rightCount >= 4u;
    const bool simultaneous =
        request->pattern == NFSW_EXHAUST_FLAME_SIMULTANEOUS;
    const bool sequential =
        request->pattern == NFSW_EXHAUST_FLAME_SEQUENTIAL;
    unsigned armedOutlets = 0;

    if (multiOutlet && sequential && request->sequenceId != 0) {
        if (findMultiOutletSequence(*connection, request->vehicleId,
                                    request->sequenceId, nowMs) != nullptr) {
            return 1;
        }
        shuffleOutletIndices(left, leftCount);
        shuffleOutletIndices(right, rightCount);
        std::size_t leftIndex = 0;
        std::size_t rightIndex = 0;
        bool chooseLeft = request->side == NFSW_EXHAUST_SIDE_LEFT;
        std::uint64_t dueMs = nowMs;
        while (leftIndex < leftCount || rightIndex < rightCount) {
            std::size_t outlet = 0;
            if ((chooseLeft && leftIndex < leftCount) ||
                rightIndex >= rightCount) {
                outlet = left[leftIndex++];
            } else {
                outlet = right[rightIndex++];
            }
            if (!armOutlet(*connection, outlet, effectKey, dueMs, nowMs)) {
                return 0;
            }
            ++armedOutlets;
            dueMs += kMultiOutletStepMs;
            chooseLeft = !chooseLeft;
        }
        rememberMultiOutletSequence(*connection, request->vehicleId,
                                    request->sequenceId,
                                    dueMs + kPluginFlamePulseMs);
    } else {
        auto& outlets = request->side == NFSW_EXHAUST_SIDE_LEFT ? left : right;
        const std::size_t outletCount =
            request->side == NFSW_EXHAUST_SIDE_LEFT ? leftCount : rightCount;
        shuffleOutletIndices(outlets, outletCount);
        for (std::size_t position = 0; position < outletCount; ++position) {
            const std::uint64_t dueMs =
                multiOutlet && simultaneous && position > 0
                    ? nowMs + kMultiOutletStepMs
                    : nowMs;
            if (!armOutlet(*connection, outlets[position], effectKey, dueMs,
                           nowMs)) {
                return 0;
            }
            ++armedOutlets;
        }
    }
    if (armedOutlets == 0) return 0;

    const unsigned long long lateness =
        request->emittedAtMs >= request->scheduledAtMs
            ? request->emittedAtMs - request->scheduledAtMs
            : 0u;
    write("LIVE_FLAME vehicle=%u side=%s outlets=%u mode=PULSE duration=%llums "
          "intensity=%.2f effectKey=%08X scheduled=%llu emitted=%llu "
          "lateness=%llu",
          request->vehicleId,
          request->side == NFSW_EXHAUST_SIDE_LEFT ? "LEFT" : "RIGHT",
          armedOutlets,
          static_cast<unsigned long long>(kPluginFlamePulseMs),
          static_cast<double>(kPluginFlameIntensity),
          static_cast<unsigned>(effectKey),
          static_cast<unsigned long long>(request->scheduledAtMs),
          static_cast<unsigned long long>(request->emittedAtMs), lateness);
    return 1;
}

void servicePluginFlamePulses(std::uint64_t nowMs) noexcept {
    if (g_originalUpdateEmitter == nullptr) return;
    for (auto& connection : g_connections) {
        bool hasScheduledPulse = false;
        for (std::size_t index = 0; index < connection.emitterCount; ++index) {
            if (connection.pluginPulseEffectKeys[index] == 0) continue;
            if (connection.pluginPulseUntilMs[index] < nowMs) {
                connection.pluginPulseStartMs[index] = 0;
                connection.pluginPulseUntilMs[index] = 0;
                connection.pluginPulseLastUpdateMs[index] = 0;
                connection.pluginPulseEffectKeys[index] = 0;
                continue;
            }
            hasScheduledPulse = true;
        }
        if (findConnection(connection.connection) == nullptr) {
            continue;
        }
        if (!hasScheduledPulse) {
            continue;
        }
        if (!connectionAllowsBackfire(connection)) {
            clearPluginFlamePulses(connection);
            continue;
        }
        void* velocity = nullptr;
        auto* parentMatrix = reinterpret_cast<const void*>(
            reinterpret_cast<std::uintptr_t>(connection.connection) + 0x330u);
        if (!readValue(connection.connection, 0x38u, &velocity) ||
            !isReadable(parentMatrix, sizeof(float) * 16u)) {
            continue;
        }
        for (std::size_t index = 0; index < connection.emitterCount; ++index) {
            if (connection.pluginPulseEffectKeys[index] == 0) continue;
            if (connection.pluginPulseStartMs[index] > nowMs ||
                connection.pluginPulseLastUpdateMs[index] == nowMs ||
                !connectionOwnsEmitter(connection, connection.emitters[index])) {
                continue;
            }
            __try {
                g_originalUpdateEmitter(
                    connection.emitters[index], parentMatrix,
                    connection.pluginPulseEffectKeys[index],
                    kEmitterTimeStepBits, kPluginFlameIntensity, velocity);
                connection.pluginPulseLastUpdateMs[index] = nowMs;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                connection.pluginPulseStartMs[index] = 0;
                connection.pluginPulseUntilMs[index] = 0;
                connection.pluginPulseEffectKeys[index] = 0;
                write("LIVE_FLAME_PULSE_STOP conn=%p emitter=%p "
                      "reason=exception",
                      connection.connection, connection.emitters[index]);
            }
        }
    }
}

bool sunSetHasLightAtPosition(
    std::int32_t count,
    const nfsmw_exhaust::sunset_lighting::SpotLight& light) noexcept {
    using nfsmw_exhaust::sunset_lighting::SpotLightModel32;
    for (std::int32_t index = 0; index < count; ++index) {
        SpotLightModel32 existing{};
        const auto address = reinterpret_cast<const void*>(
            g_sunSetLightBuffer +
            static_cast<std::uintptr_t>(index) * sizeof(SpotLightModel32));
        if (!safeRead(address, &existing, sizeof(existing))) return true;
        if (nfsmw_exhaust::sunset_lighting::samePosition(existing.light,
                                                         light)) {
            return true;
        }
    }
    return false;
}

void appendPluginFlameLightsToSunSet() noexcept {
    using nfsmw_exhaust::sunset_lighting::SpotLight;
    using nfsmw_exhaust::sunset_lighting::SpotLightModel32;
    using nfsmw_exhaust::sunset_lighting::Vec3;

    if (g_sunSetHookState != SunSetHookState::Installed) return;

    std::int32_t lightCount = 0;
    SpotLight configured{};
    float weatherLightPower = 0.0f;
    if (!safeRead(reinterpret_cast<const void*>(g_sunSetLightCount),
                  &lightCount, sizeof(lightCount)) ||
        lightCount < 0 ||
        lightCount > static_cast<std::int32_t>(kSunSetLightCapacity) ||
        !safeRead(reinterpret_cast<const void*>(g_sunSetExhaustLight),
                  &configured, sizeof(configured)) ||
        !safeRead(reinterpret_cast<const void*>(g_sunSetWeatherLightPower),
                  &weatherLightPower, sizeof(weatherLightPower))) {
        return;
    }

    const std::uint64_t nowMs = GetTickCount64();
    for (const auto& connection : g_connections) {
        if (lightCount >= static_cast<std::int32_t>(kSunSetLightCapacity)) {
            break;
        }
        if (connection.connection == nullptr ||
            !connectionAllowsBackfire(connection)) {
            continue;
        }

        std::array<float, 16> vehicleMatrix{};
        const auto* matrixAddress = reinterpret_cast<const void*>(
            reinterpret_cast<std::uintptr_t>(connection.connection) + 0x330u);
        if (!safeRead(matrixAddress, vehicleMatrix.data(),
                      sizeof(vehicleMatrix))) {
            continue;
        }

        for (std::size_t index = 0; index < connection.emitterCount;
             ++index) {
            if (lightCount >=
                static_cast<std::int32_t>(kSunSetLightCapacity)) {
                break;
            }
            if (connection.pluginPulseEffectKeys[index] == 0 ||
                connection.pluginPulseStartMs[index] > nowMs ||
                connection.pluginPulseUntilMs[index] < nowMs ||
                (connection.markerHashes[index] != kLeftExhaustHash &&
                 connection.markerHashes[index] != kRightExhaustHash) ||
                !connectionOwnsEmitter(connection,
                                       connection.emitters[index])) {
                continue;
            }

            SpotLight transformed{};
            const auto& position = connection.markerPositions[index];
            if (!nfsmw_exhaust::sunset_lighting::transformExhaustLight(
                    configured, Vec3{position[0], position[1], position[2]},
                    vehicleMatrix.data(), weatherLightPower, &transformed) ||
                (transformed.color.x == 0.0f &&
                 transformed.color.y == 0.0f &&
                 transformed.color.z == 0.0f) ||
                sunSetHasLightAtPosition(lightCount, transformed)) {
                continue;
            }

            SpotLightModel32 model{};
            model.light = transformed;
            model.source = kSunSetPlayerBrakeLightSource;
            model.flareIntensity = 1.0f;
            const auto destination = reinterpret_cast<void*>(
                g_sunSetLightBuffer +
                static_cast<std::uintptr_t>(lightCount) * sizeof(model));
            if (!safeWrite(destination, &model, sizeof(model))) return;
            ++lightCount;
        }
    }
    safeWrite(reinterpret_cast<void*>(g_sunSetLightCount), &lightCount,
              sizeof(lightCount));
}

void __cdecl sunSetPopulateLightsHook() {
    g_originalSunSetPopulateLights();
    appendPluginFlameLightsToSunSet();
}

bool matchesSunSet1152(HMODULE module) noexcept {
    if (module == nullptr) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!safeRead(module, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 0x1000 ||
        !safeRead(reinterpret_cast<const std::uint8_t*>(module) +
                      dos.e_lfanew,
                  &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.FileHeader.TimeDateStamp != kSunSetTimestamp1152 ||
        nt.OptionalHeader.SizeOfImage != kSunSetImageSize1152) {
        return false;
    }

    const std::uintptr_t function = base + kSunSetPopulateLightsRva1152;
    return hasBytes(function, kSunSetPopulateLightsPrefix1152) &&
           hasBytes(function + 14u, kSunSetPopulateLightsSuffix1152) &&
           hasBytes(base + 0x0000BE3Cu, kSunSetPopulateLightsCall1152) &&
           isWritable(reinterpret_cast<void*>(
                          base + kSunSetLightCountRva1152),
                      sizeof(std::int32_t)) &&
           isWritable(reinterpret_cast<void*>(
                          base + kSunSetLightBufferRva1152),
                      sizeof(nfsmw_exhaust::sunset_lighting::SpotLightModel32) *
                          kSunSetLightCapacity) &&
           isReadable(reinterpret_cast<const void*>(
                          base + kSunSetExhaustLightRva1152),
                      sizeof(nfsmw_exhaust::sunset_lighting::SpotLight)) &&
           isReadable(reinterpret_cast<const void*>(
                          base + kSunSetWeatherLightPowerRva1152),
                      sizeof(float));
}

void tryInstallSunSetLightingHook() noexcept {
    if (g_sunSetHookState != SunSetHookState::Waiting) return;
    HMODULE module = GetModuleHandleA("SunSet.asi");
    if (module == nullptr) return;
    if (!matchesSunSet1152(module)) {
        g_sunSetHookState = SunSetHookState::Unsupported;
        write("SUNSET_LIGHT unsupported SunSet build; integration disabled");
        return;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto function = base + kSunSetPopulateLightsRva1152;
    const MH_STATUS created = MH_CreateHook(
        reinterpret_cast<void*>(function),
        reinterpret_cast<void*>(&sunSetPopulateLightsHook),
        reinterpret_cast<void**>(&g_originalSunSetPopulateLights));
    if (created != MH_OK) {
        g_sunSetHookState = SunSetHookState::Unsupported;
        write("SUNSET_LIGHT hook creation failed status=%d",
              static_cast<int>(created));
        return;
    }
    const MH_STATUS enabled = MH_EnableHook(reinterpret_cast<void*>(function));
    if (enabled != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(function));
        g_originalSunSetPopulateLights = nullptr;
        g_sunSetHookState = SunSetHookState::Unsupported;
        write("SUNSET_LIGHT hook enable failed status=%d",
              static_cast<int>(enabled));
        return;
    }

    g_sunSetLightCount = base + kSunSetLightCountRva1152;
    g_sunSetLightBuffer = base + kSunSetLightBufferRva1152;
    g_sunSetWeatherLightPower =
        base + kSunSetWeatherLightPowerRva1152;
    g_sunSetExhaustLight = base + kSunSetExhaustLightRva1152;
    g_sunSetHookState = SunSetHookState::Installed;
    write("SUNSET_LIGHT installed module=%p populate=%p buffer=%p "
          "config=%p",
          module, reinterpret_cast<void*>(function),
          reinterpret_cast<void*>(g_sunSetLightBuffer),
          reinterpret_cast<void*>(g_sunSetExhaustLight));
}

nfsmw_exhaust::native_audio::Spatialization audioSpatialization(
    const NfswExhaustAudioRequestC& request, float* distanceOut) noexcept {
    nfsmw_exhaust::native_audio::Spatialization result{};
    if (distanceOut != nullptr) *distanceOut = 0.0f;
    CachedVehicle* source = cachedVehicleForId(request.vehicleId);
    CachedVehicle* listener = nullptr;
    for (std::uint32_t index = 0; index < g_cachedCount; ++index) {
        if (g_cached[index].snapshot.valid != 0 &&
            g_cached[index].connection != nullptr) {
            listener = &g_cached[index];
            break;
        }
    }
    if (source == nullptr || listener == nullptr ||
        source->connection == nullptr || listener->connection == nullptr) {
        result.pan = request.side == NFSW_EXHAUST_SIDE_LEFT ? -0.18f : 0.18f;
        return result;
    }

    std::array<float, 16> sourceMatrix{};
    std::array<float, 16> listenerMatrix{};
    const auto* sourceAddress = reinterpret_cast<const void*>(
        reinterpret_cast<std::uintptr_t>(source->connection) + 0x330u);
    const auto* listenerAddress = reinterpret_cast<const void*>(
        reinterpret_cast<std::uintptr_t>(listener->connection) + 0x330u);
    if (!safeRead(sourceAddress, sourceMatrix.data(), sizeof(sourceMatrix)) ||
        !safeRead(listenerAddress, listenerMatrix.data(),
                  sizeof(listenerMatrix))) {
        result.pan = request.side == NFSW_EXHAUST_SIDE_LEFT ? -0.18f : 0.18f;
        return result;
    }

    const float localX = request.marker.x;
    const float localY = request.marker.y;
    const float localZ = request.marker.z;
    const float sourceX = localX * sourceMatrix[0] +
                          localY * sourceMatrix[4] +
                          localZ * sourceMatrix[8] + sourceMatrix[12];
    const float sourceY = localX * sourceMatrix[1] +
                          localY * sourceMatrix[5] +
                          localZ * sourceMatrix[9] + sourceMatrix[13];
    const float sourceZ = localX * sourceMatrix[2] +
                          localY * sourceMatrix[6] +
                          localZ * sourceMatrix[10] + sourceMatrix[14];
    const float deltaX = sourceX - listenerMatrix[12];
    const float deltaY = sourceY - listenerMatrix[13];
    const float deltaZ = sourceZ - listenerMatrix[14];
    const float distance =
        std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
    if (!std::isfinite(distance)) return result;

    const float lateralLength = std::sqrt(
        listenerMatrix[4] * listenerMatrix[4] +
        listenerMatrix[5] * listenerMatrix[5] +
        listenerMatrix[6] * listenerMatrix[6]);
    if (distance > 0.001f && lateralLength > 0.001f) {
        const float lateral =
            -(deltaX * listenerMatrix[4] + deltaY * listenerMatrix[5] +
              deltaZ * listenerMatrix[6]) /
            lateralLength;
        result.pan = std::clamp((lateral / distance) * 0.60f, -0.55f, 0.55f);
    }
    if (source != listener) {
        const float scaled = distance / 22.0f;
        result.gain = std::clamp(1.0f / (1.0f + scaled * scaled),
                                 0.04f, 1.0f);
        result.lowPass = 0.55f + 0.45f * result.gain;
    }
    if (distanceOut != nullptr) *distanceOut = distance;
    return result;
}

void NFSW_EXHAUST_CALL playAudio(void*,
                                 const NfswExhaustAudioRequestC* request) {
    if (request == nullptr) return;
    const CachedVehicle* source = cachedVehicleForId(request->vehicleId);
    if (source == nullptr || !cachedVehicleAllowsBackfire(*source)) return;
    float distance = 0.0f;
    const auto spatial = audioSpatialization(*request, &distance);
    const bool played =
        nfsmw_exhaust::native_audio::play(request->assetId, spatial);
    write("LIVE_AUDIO vehicle=%u side=%s clip=%u asset='%s' "
          "played=%d gain=%.3f pan=%.3f lowpass=%.3f distance=%.2f "
          "scheduled=%llu emitted=%llu",
          request->vehicleId,
          request->side == NFSW_EXHAUST_SIDE_LEFT ? "LEFT" : "RIGHT",
          static_cast<unsigned>(request->clipIndex) + 1u,
          request->assetId == nullptr ? "<null>" : request->assetId,
          played ? 1 : 0,
          static_cast<double>(spatial.gain),
          static_cast<double>(spatial.pan),
          static_cast<double>(spatial.lowPass),
          static_cast<double>(distance),
          static_cast<unsigned long long>(request->scheduledAtMs),
          static_cast<unsigned long long>(request->emittedAtMs));
}

void NFSW_EXHAUST_CALL setVanilla(void*, std::uint32_t vehicleId, int enabled) {
    VehicleRecord* record = vehicleRecordForId(vehicleId);
    if (record != nullptr) {
        record->suppressVanillaBackfire = enabled == 0;
    }
    write("VANILLA_STATE vehicle=%u requestedEnabled=%d suppression=%d "
          "applied=%d",
          vehicleId, enabled,
          record != nullptr && record->suppressVanillaBackfire ? 1 : 0,
          record != nullptr ? 1 : 0);
}

void NFSW_EXHAUST_CALL coreLog(void*, const char* message) {
    write("CORE %s", message == nullptr ? "<null>" : message);
}

void __cdecl gameFrameTickHook(std::uint32_t elapsedMs) {
    g_originalGameFrameTick(elapsedMs);
    tryInstallSunSetLightingHook();
    const std::uint64_t nowMs = GetTickCount64();
    NFSW_Exhaust_OnFrame(nowMs);
    servicePluginFlamePulses(nowMs);
}

ConnectionRecord* connectionRecordFor(void* connection) noexcept {
    for (auto& record : g_connections) {
        if (record.connection == connection) return &record;
    }
    for (auto& record : g_connections) {
        if (record.connection == nullptr) return &record;
    }
    auto* oldest = &g_connections[0];
    for (auto& record : g_connections) {
        if (record.generation < oldest->generation) oldest = &record;
    }
    return oldest;
}

std::size_t collectListNodes(void* sentinel, void** output,
                             std::size_t capacity) noexcept {
    if (sentinel == nullptr || output == nullptr || capacity == 0) return 0;
    void* node = nullptr;
    if (!safeRead(sentinel, &node, sizeof(node))) return 0;
    std::size_t count = 0;
    while (node != nullptr && node != sentinel && count < capacity) {
        for (std::size_t index = 0; index < count; ++index) {
            if (output[index] == node) return count;
        }
        output[count++] = node;
        void* next = nullptr;
        if (!safeRead(node, &next, sizeof(next))) break;
        node = next;
    }
    return count;
}

const char* markerSide(std::uint32_t hash) noexcept {
    if (hash == kLeftExhaustHash) return "LEFT";
    if (hash == kRightExhaustHash) return "RIGHT";
    return "UNKNOWN";
}

void traceExhaustMapping(void* connection, void* renderInfo) noexcept {
    auto* record = connectionRecordFor(connection);
    if (record == nullptr) return;
    *record = {};
    record->connection = connection;
    record->renderInfo = renderInfo;
    ++g_connectionGeneration;
    if (g_connectionGeneration == 0) ++g_connectionGeneration;
    record->generation = g_connectionGeneration;

    auto* markerSentinel = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(renderInfo) + 0x1ACu);
    auto* emitterSentinel = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(connection) + 0x3E4u);
    auto* nitrousEmitterSentinel = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(connection) + 0x3ECu);
    std::array<void*, kMaxExhaustMarkers> markerNodes{};
    std::array<void*, kMaxExhaustMarkers> emitterNodes{};
    std::array<void*, kMaxNitrousEmitters> nitrousEmitterNodes{};
    const std::size_t markerCount = collectListNodes(
        markerSentinel, markerNodes.data(), markerNodes.size());
    const std::size_t emitterCount = collectListNodes(
        emitterSentinel, emitterNodes.data(), emitterNodes.size());
    const std::size_t nitrousEmitterCount = collectListNodes(
        nitrousEmitterSentinel, nitrousEmitterNodes.data(),
        nitrousEmitterNodes.size());
    record->emitterCount = emitterCount;
    record->nitrousEmitterCount = nitrousEmitterCount;
    std::copy_n(nitrousEmitterNodes.begin(), nitrousEmitterCount,
                record->nitrousEmitters.begin());

    write("EXHAUST_LIST conn=%p renderInfo=%p markers=%u emitters=%u "
          "nitrousEmitters=%u",
          connection, renderInfo, static_cast<unsigned>(markerCount),
          static_cast<unsigned>(emitterCount),
          static_cast<unsigned>(nitrousEmitterCount));
    for (std::size_t index = 0; index < emitterCount; ++index) {
        record->emitters[index] = emitterNodes[index];
        std::uint32_t hash = 0;
        ExhaustMarkerNode markerNode{};
        PositionMarkerData marker{};
        std::array<float, 16> emitterMatrix{};
        bool markerReadable = false;
        bool matrixMatch = false;
        if (index < markerCount &&
            safeRead(markerNodes[index], &markerNode, sizeof(markerNode)) &&
            markerNode.markerData != nullptr &&
            safeRead(markerNode.markerData, &marker, sizeof(marker))) {
            markerReadable = true;
            hash = marker.nameHash;
            matrixMatch = safeRead(
                              reinterpret_cast<const std::uint8_t*>(
                                  emitterNodes[index]) +
                                  0x10u,
                              emitterMatrix.data(), sizeof(emitterMatrix)) &&
                          std::memcmp(emitterMatrix.data(), marker.matrix,
                                      sizeof(marker.matrix)) == 0;
        }
        record->markerHashes[index] = hash;
        if (markerReadable) {
            record->markerPositions[index] = {{
                markerNode.x, markerNode.y, markerNode.z}};
        }
        write("EXHAUST_MAP conn=%p index=%u markerNode=%p markerData=%p "
              "hash=%08X side=%s emitter=%p markerReadable=%d matrixMatch=%d "
              "nodePos=(%.3f,%.3f,%.3f) matrixTrow=(%.3f,%.3f,%.3f) "
              "matrixTcol=(%.3f,%.3f,%.3f)",
              connection, static_cast<unsigned>(index),
              index < markerCount ? markerNodes[index] : nullptr,
              markerReadable ? markerNode.markerData : nullptr,
              static_cast<unsigned>(hash), markerSide(hash), emitterNodes[index],
              markerReadable ? 1 : 0, matrixMatch ? 1 : 0,
              markerReadable ? markerNode.x : 0.0f,
              markerReadable ? markerNode.y : 0.0f,
              markerReadable ? markerNode.z : 0.0f,
              markerReadable ? marker.matrix[12] : 0.0f,
              markerReadable ? marker.matrix[13] : 0.0f,
              markerReadable ? marker.matrix[14] : 0.0f,
              markerReadable ? marker.matrix[3] : 0.0f,
              markerReadable ? marker.matrix[7] : 0.0f,
              markerReadable ? marker.matrix[11] : 0.0f);
    }
    if (markerCount != emitterCount) {
        write("EXHAUST_MAP_INCOMPLETE conn=%p markers=%u emitters=%u",
              connection, static_cast<unsigned>(markerCount),
              static_cast<unsigned>(emitterCount));
    }
}

const ConnectionRecord* findConnection(void* connection) noexcept {
    if (connection == nullptr) return nullptr;
    for (const auto& record : g_connections) {
        if (record.connection != connection) continue;
        std::uintptr_t vtable = 0;
        void* renderInfo = nullptr;
        if (safeRead(record.connection, &vtable, sizeof(vtable)) &&
            vtable == kCarRenderConnVtable &&
            readValue(record.connection, 0x44u, &renderInfo) &&
            renderInfo == record.renderInfo) {
            return &record;
        }
    }
    return nullptr;
}

void populateExhaustMarkers(const VehicleProbe& probe,
                            NfswExhaustVehicleSnapshotC* snapshot) noexcept {
    if (snapshot == nullptr || probe.renderable == nullptr) return;
    void* connection = nullptr;
    if (!readValue(probe.renderable, 0x38u, &connection)) return;
    const ConnectionRecord* record = findConnection(connection);
    if (record == nullptr) return;

    for (std::size_t index = 0; index < record->emitterCount; ++index) {
        NfswExhaustMarkerC* target = nullptr;
        if (record->markerHashes[index] == kLeftExhaustHash) {
            target = &snapshot->leftExhaust;
        } else if (record->markerHashes[index] == kRightExhaustHash) {
            target = &snapshot->rightExhaust;
        }
        if (target == nullptr || target->present != 0) continue;
        target->present = 1;
        target->x = record->markerPositions[index][0];
        target->y = record->markerPositions[index][1];
        target->z = record->markerPositions[index][2];
        target->qw = 1.0f;
    }
}

bool connectionOwnsEmitter(const ConnectionRecord& record,
                           void* emitter) noexcept {
    std::uintptr_t vtable = 0;
    void* renderInfo = nullptr;
    if (!safeRead(record.connection, &vtable, sizeof(vtable)) ||
        vtable != kCarRenderConnVtable ||
        !readValue(record.connection, 0x44u, &renderInfo) ||
        renderInfo != record.renderInfo) {
        return false;
    }
    auto* sentinel = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(record.connection) + 0x3E4u);
    std::array<void*, kMaxExhaustMarkers> nodes{};
    const std::size_t count =
        collectListNodes(sentinel, nodes.data(), nodes.size());
    return std::find(nodes.begin(), nodes.begin() + count, emitter) !=
           nodes.begin() + count;
}

bool connectionOwnsNitrousEmitter(const ConnectionRecord& record,
                                  void* emitter) noexcept {
    std::uintptr_t vtable = 0;
    void* renderInfo = nullptr;
    if (!safeRead(record.connection, &vtable, sizeof(vtable)) ||
        vtable != kCarRenderConnVtable ||
        !readValue(record.connection, 0x44u, &renderInfo) ||
        renderInfo != record.renderInfo) {
        return false;
    }
    auto* sentinel = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(record.connection) + 0x3ECu);
    std::array<void*, kMaxNitrousEmitters> nodes{};
    const std::size_t count =
        collectListNodes(sentinel, nodes.data(), nodes.size());
    return std::find(nodes.begin(), nodes.begin() + count, emitter) !=
           nodes.begin() + count;
}

ConnectionRecord* findEmitterOwner(void* emitter,
                                   std::size_t* outputIndex) noexcept {
    ConnectionRecord* best = nullptr;
    std::size_t bestIndex = 0;
    for (auto& record : g_connections) {
        for (std::size_t index = 0; index < record.emitterCount; ++index) {
            if (record.emitters[index] == emitter &&
                (best == nullptr || record.generation > best->generation) &&
                connectionOwnsEmitter(record, emitter)) {
                best = &record;
                bestIndex = index;
            }
        }
    }
    if (best != nullptr && outputIndex != nullptr) *outputIndex = bestIndex;
    return best;
}

ConnectionRecord* findNitrousEmitterOwner(void* emitter) noexcept {
    ConnectionRecord* best = nullptr;
    for (auto& record : g_connections) {
        for (std::size_t index = 0; index < record.nitrousEmitterCount;
             ++index) {
            if (record.nitrousEmitters[index] == emitter &&
                (best == nullptr || record.generation > best->generation) &&
                connectionOwnsNitrousEmitter(record, emitter)) {
                best = &record;
            }
        }
    }
    return best;
}

void __fastcall carRenderConnOnLoadedHook(void* connection, void*,
                                          void* renderInfo) {
    g_originalCarRenderConnOnLoaded(connection, renderInfo);
    std::uintptr_t connectionVtable = 0;
    void* storedRenderInfo = nullptr;
    void* exhaustFirst = nullptr;
    void* nitrousFirst = nullptr;
    void* renderInfoExhaustFirst = nullptr;
    void* renderInfoNitrousFirst = nullptr;
    safeRead(connection, &connectionVtable, sizeof(connectionVtable));
    readValue(connection, 0x44u, &storedRenderInfo);
    readValue(connection, 0x3E4u, &exhaustFirst);
    readValue(connection, 0x3ECu, &nitrousFirst);
    readValue(renderInfo, 0x1ACu, &renderInfoExhaustFirst);
    readValue(renderInfo, 0x1A4u, &renderInfoNitrousFirst);
    write("RENDER_CONN_LOADED conn=%p vt=%p argRenderInfo=%p storedRenderInfo=%p "
          "emitters=exhaust:%p nitrous:%p markers=exhaust:%p nitrous:%p",
          connection, reinterpret_cast<void*>(connectionVtable), renderInfo,
          storedRenderInfo, exhaustFirst, nitrousFirst, renderInfoExhaustFirst,
          renderInfoNitrousFirst);
    if (connectionVtable == kCarRenderConnVtable && renderInfo != nullptr &&
        storedRenderInfo == renderInfo) {
        traceExhaustMapping(connection, renderInfo);
    }
}

void __fastcall carRenderConnHandleFxEventHook(void* connection, void*,
                                               std::uint32_t eventCode) {
    std::uint32_t flagsBefore = 0;
    std::uint32_t flagsAfter = 0;
    void* renderInfo = nullptr;
    readValue(connection, 0x3F8u, &flagsBefore);
    readValue(connection, 0x44u, &renderInfo);
    std::uint32_t vehicleId = 0;
    if ((eventCode == 0u || eventCode == 3u || eventCode == 4u) &&
        shouldSuppressStockBackfire(connection, &vehicleId)) {
        write("FX_EVENT_SUPPRESS vehicle=%u conn=%p renderInfo=%p code=%u "
              "flags=%08X",
              vehicleId, connection, renderInfo, eventCode, flagsBefore);
        return;
    }
    g_originalCarRenderConnHandleFxEvent(connection, eventCode);
    readValue(connection, 0x3F8u, &flagsAfter);
    if (eventCode == 0u || eventCode == 3u || eventCode == 4u) {
        write("FX_EVENT_FORWARD conn=%p renderInfo=%p code=%u flags=%08X->%08X",
              connection, renderInfo, eventCode, flagsBefore, flagsAfter);
    }
}

void __fastcall emitOneShotHook(void* emitter, void*, const void* parentMatrix,
                                std::uint32_t effectKey, float intensity,
                                const void* velocity) {
    std::size_t index = 0;
    const ConnectionRecord* owner = findEmitterOwner(emitter, &index);
    if (owner != nullptr) {
        std::uint32_t vehicleId = 0;
        if (shouldSuppressStockBackfire(owner->connection, &vehicleId)) {
            write("EMIT_ONE_SHOT_SUPPRESS vehicle=%u conn=%p emitter=%p "
                  "index=%u hash=%08X side=%s effectKey=%08X",
                  vehicleId, owner->connection, emitter,
                  static_cast<unsigned>(index),
                  static_cast<unsigned>(owner->markerHashes[index]),
                  markerSide(owner->markerHashes[index]),
                  static_cast<unsigned>(effectKey));
            return;
        }
        write("EMIT_ONE_SHOT_FORWARD conn=%p emitter=%p index=%u hash=%08X "
              "side=%s effectKey=%08X intensity=%.3f parentMatrix=%p velocity=%p",
              owner->connection, emitter, static_cast<unsigned>(index),
              static_cast<unsigned>(owner->markerHashes[index]),
              markerSide(owner->markerHashes[index]),
              static_cast<unsigned>(effectKey), intensity, parentMatrix, velocity);
    }
    g_originalEmitOneShot(emitter, parentMatrix, effectKey, intensity, velocity);
}

void __fastcall updateEmitterHook(void* emitter, void*,
                                  const void* parentMatrix,
                                  std::uint32_t effectKey,
                                  std::uint32_t parameter, float intensity,
                                  const void* velocity) {
    std::size_t index = 0;
    ConnectionRecord* owner = findEmitterOwner(emitter, &index);
    ConnectionRecord* nitrousOwner =
        owner == nullptr ? findNitrousEmitterOwner(emitter) : nullptr;
    void* particleBefore = nullptr;
    std::uint32_t storedBefore = 0;
    const bool beforeReadable =
        owner != nullptr && readValue(emitter, 0x50u, &particleBefore) &&
        readValue(emitter, 0x54u, &storedBefore);

    g_originalUpdateEmitter(emitter, parentMatrix, effectKey, parameter,
                            intensity, velocity);
    if (nitrousOwner != nullptr && effectKey != 0 && intensity > 0.0f) {
        nitrousOwner->lastNitrousUpdateMs = GetTickCount64();
        return;
    }
    if (owner == nullptr) return;

    void* particleAfter = nullptr;
    std::uint32_t storedAfter = 0;
    const bool afterReadable = readValue(emitter, 0x50u, &particleAfter) &&
                               readValue(emitter, 0x54u, &storedAfter);
    const bool changed =
        !owner->updateObserved[index] ||
        owner->lastUpdateEffectKeys[index] != effectKey ||
        owner->lastParticleInstances[index] != particleAfter ||
        owner->lastStoredEffectKeys[index] != storedAfter ||
        particleBefore != particleAfter || storedBefore != storedAfter;
    if (changed) {
        write("EMIT_CONTINUOUS_FORWARD conn=%p emitter=%p index=%u hash=%08X "
              "side=%s effectKey=%08X parameter=%08X intensity=%.3f "
              "particle=%p->%p stored=%08X->%08X readable=%d/%d "
              "parentMatrix=%p velocity=%p",
              owner->connection, emitter, static_cast<unsigned>(index),
              static_cast<unsigned>(owner->markerHashes[index]),
              markerSide(owner->markerHashes[index]),
              static_cast<unsigned>(effectKey),
              static_cast<unsigned>(parameter),
              static_cast<double>(intensity), particleBefore, particleAfter,
              static_cast<unsigned>(storedBefore),
              static_cast<unsigned>(storedAfter), beforeReadable ? 1 : 0,
              afterReadable ? 1 : 0, parentMatrix, velocity);
    }
    owner->updateObserved[index] = true;
    owner->lastUpdateEffectKeys[index] = effectKey;
    owner->lastParticleInstances[index] = particleAfter;
    owner->lastStoredEffectKeys[index] = storedAfter;
}

bool appendPath(char path[MAX_PATH], const char* fileName) noexcept {
    char* slash = std::strrchr(path, '\\');
    if (slash == nullptr) return false;
    const std::size_t directoryLength = static_cast<std::size_t>(slash - path + 1);
    const std::size_t fileLength = std::strlen(fileName);
    if (directoryLength + fileLength >= MAX_PATH) return false;
    std::memcpy(path + directoryLength, fileName, fileLength + 1);
    return true;
}

bool registerCore() noexcept {
    const NfswExhaustCallbacks callbacks{
        NFSW_EXHAUST_API_VERSION,
        sizeof(NfswExhaustCallbacks),
        nullptr,
        getVehicleCount,
        readVehicle,
        spawnFlame,
        playAudio,
        setVanilla,
        coreLog,
    };
    if (!NFSW_Exhaust_RegisterCallbacks(&callbacks)) {
        write("INIT_FAIL core callback registration failed");
        return false;
    }

    char modulePath[MAX_PATH] = {};
    const DWORD modulePathSize =
        GetModuleFileNameA(g_pluginModule, modulePath, MAX_PATH);
    if (modulePathSize == 0 || modulePathSize >= MAX_PATH) {
        write("CONFIG module path unavailable; built-in defaults active");
        return true;
    }
    char configPath[MAX_PATH] = {};
    char audioPath[MAX_PATH] = {};
    std::strcpy(configPath, modulePath);
    std::strcpy(audioPath, modulePath);
    if (appendPath(configPath, "NFSMWExhaustBackfire.ini")) {
        write("CONFIG main path='%s' loaded=%d", configPath,
              NFSW_Exhaust_LoadConfig(configPath));
    }
    if (appendPath(audioPath, "BackfireAudio.ini")) {
        write("CONFIG audio path='%s' loaded=%d", audioPath,
              NFSW_Exhaust_LoadAudioManifest(audioPath));
    }
    nfsmw_exhaust::native_audio::initialize(modulePath);
    return true;
}

bool installHooks() noexcept {
    const MH_STATUS initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        write("HOOK_FAIL MH_Initialize status=%d", static_cast<int>(initialized));
        return false;
    }
    const MH_STATUS created = MH_CreateHook(
        reinterpret_cast<void*>(kGameFrameTick),
        reinterpret_cast<void*>(&gameFrameTickHook),
        reinterpret_cast<void**>(&g_originalGameFrameTick));
    if (created != MH_OK) {
        write("HOOK_FAIL MH_CreateHook status=%d", static_cast<int>(created));
        return false;
    }
    const MH_STATUS loadedCreated = MH_CreateHook(
        reinterpret_cast<void*>(kCarRenderConnOnLoaded),
        reinterpret_cast<void*>(&carRenderConnOnLoadedHook),
        reinterpret_cast<void**>(&g_originalCarRenderConnOnLoaded));
    const MH_STATUS eventCreated = MH_CreateHook(
        reinterpret_cast<void*>(kCarRenderConnHandleFxEvent),
        reinterpret_cast<void*>(&carRenderConnHandleFxEventHook),
        reinterpret_cast<void**>(&g_originalCarRenderConnHandleFxEvent));
    const MH_STATUS emitCreated = MH_CreateHook(
        reinterpret_cast<void*>(kEmitOneShot),
        reinterpret_cast<void*>(&emitOneShotHook),
        reinterpret_cast<void**>(&g_originalEmitOneShot));
    const MH_STATUS updateCreated = MH_CreateHook(
        reinterpret_cast<void*>(kUpdateEmitter),
        reinterpret_cast<void*>(&updateEmitterHook),
        reinterpret_cast<void**>(&g_originalUpdateEmitter));
    if (loadedCreated != MH_OK || eventCreated != MH_OK || emitCreated != MH_OK ||
        updateCreated != MH_OK) {
        write("HOOK_FAIL render hooks create loaded=%d event=%d emit=%d update=%d",
              static_cast<int>(loadedCreated), static_cast<int>(eventCreated),
              static_cast<int>(emitCreated), static_cast<int>(updateCreated));
        MH_RemoveHook(reinterpret_cast<void*>(kGameFrameTick));
        if (loadedCreated == MH_OK)
            MH_RemoveHook(reinterpret_cast<void*>(kCarRenderConnOnLoaded));
        if (eventCreated == MH_OK)
            MH_RemoveHook(reinterpret_cast<void*>(kCarRenderConnHandleFxEvent));
        if (emitCreated == MH_OK)
            MH_RemoveHook(reinterpret_cast<void*>(kEmitOneShot));
        if (updateCreated == MH_OK)
            MH_RemoveHook(reinterpret_cast<void*>(kUpdateEmitter));
        return false;
    }
    const std::array<std::uintptr_t, 5> hooks{{
        kGameFrameTick, kCarRenderConnOnLoaded, kCarRenderConnHandleFxEvent,
        kEmitOneShot, kUpdateEmitter}};
    std::size_t enabledCount = 0;
    for (; enabledCount < hooks.size(); ++enabledCount) {
        const MH_STATUS enabled =
            MH_EnableHook(reinterpret_cast<void*>(hooks[enabledCount]));
        if (enabled != MH_OK) {
            write("HOOK_FAIL enable address=0x%08X status=%d",
                  static_cast<unsigned>(hooks[enabledCount]),
                  static_cast<int>(enabled));
            break;
        }
    }
    if (enabledCount != hooks.size()) {
        while (enabledCount > 0) {
            --enabledCount;
            MH_DisableHook(reinterpret_cast<void*>(hooks[enabledCount]));
        }
        for (const auto hook : hooks)
            MH_RemoveHook(reinterpret_cast<void*>(hook));
        return false;
    }
    write("HOOK_OK GameFrameTick address=0x%08X trampoline=%p",
          static_cast<unsigned>(kGameFrameTick), g_originalGameFrameTick);
    write("HOOK_OK CarRenderConn::OnLoaded address=0x%08X trampoline=%p",
          static_cast<unsigned>(kCarRenderConnOnLoaded),
          g_originalCarRenderConnOnLoaded);
    write("HOOK_OK CarRenderConn::HandleFxEvent address=0x%08X trampoline=%p",
          static_cast<unsigned>(kCarRenderConnHandleFxEvent),
          g_originalCarRenderConnHandleFxEvent);
    write("HOOK_OK EmitOneShot address=0x%08X trampoline=%p",
          static_cast<unsigned>(kEmitOneShot), g_originalEmitOneShot);
    write("HOOK_OK UpdateEmitter address=0x%08X trampoline=%p",
          static_cast<unsigned>(kUpdateEmitter), g_originalUpdateEmitter);
    return true;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl
NFSW_Exhaust_NativeMain(HMODULE module) {
    g_pluginModule = module;
    g_outletRandomState = static_cast<std::uint32_t>(GetTickCount64()) ^
                          GetCurrentProcessId() ^
                          static_cast<std::uint32_t>(
                              reinterpret_cast<std::uintptr_t>(module));
    if (g_outletRandomState == 0) g_outletRandomState = 0xA53C9E17u;
#if NFSMW_EXHAUST_ENABLE_LOGGING
    char modulePath[MAX_PATH] = {};
    const DWORD modulePathSize = GetModuleFileNameA(module, modulePath, MAX_PATH);
    if (modulePathSize > 0 && modulePathSize < MAX_PATH &&
        appendPath(modulePath, "NFSMWExhaustBackfire.log")) {
        nfsmw_exhaust::native_log::open(modulePath);
    }
#endif

    write("INIT replacement mode: paired pulse flames, spatial XAudio2, and "
          "NOS edge audio enabled; stock exhaust backfire suppressed after "
          "marker validation");
    wchar_t executablePath[MAX_PATH] = {};
    if (!verifyHost(executablePath)) return -1;
    if (!registerCore()) return -1;
    if (!installHooks()) {
        NFSW_Exhaust_Shutdown();
        return -1;
    }
    write("MARKER_HASH LEFT_EXHAUST=0x%08X RIGHT_EXHAUST=0x%08X",
          kLeftExhaustHash, kRightExhaustHash);
    write("INIT_OK waiting for live PVehicle instances");
    return 0;
}
