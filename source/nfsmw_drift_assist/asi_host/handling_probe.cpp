#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef NFSMW_ENABLE_ALPHA_BRIDGE
#define NFSMW_ENABLE_ALPHA_BRIDGE 0
#endif
#ifndef NFSMW_ENABLE_RWS_FORCE_TRACE
#define NFSMW_ENABLE_RWS_FORCE_TRACE 0
#endif
#ifndef NFSMW_ENABLE_REAR_WHEEL_STEERING
#define NFSMW_ENABLE_REAR_WHEEL_STEERING 0
#endif

#include "handling_probe.hpp"

#include "handling_probe_state.hpp"
#include "nfsmw_drift_target_profile.hpp"
#include "rear_wheel_steering.hpp"
#include "runtime_logging.hpp"

#include <nfsmw_sdk/nfsmw_sdk.h>
#if NFSMW_ENABLE_ALPHA_BRIDGE
#include <MinHook.h>
#endif

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace nfsmw_drift_asi::handling_probe {
namespace {

#if NFSMW_ENABLE_ALPHA_BRIDGE

using TireForceFn = float (NFSMW_THISCALL*)(
    void* self, float argument0, float argument1, float argument2,
    float argument3, float argument4);
using DriveTorqueFn = float (NFSMW_THISCALL*)(void* self);
#if NFSMW_ENABLE_RWS_FORCE_TRACE || NFSMW_ENABLE_REAR_WHEEL_STEERING
using FourWheelResultFn = void (NFSMW_THISCALL*)(void* self, void* result);
using WheelVisualFn = void (NFSMW_THISCALL*)(
    void* self, void* argument0, void* argument1, void* argument2);
using MatrixYawFn = void (NFSMW_CDECL*)(void* output, const void* input,
                                        std::int32_t angle);
#endif

constexpr std::size_t kPVehicleRigidBodyOffset = 0x78;
constexpr std::size_t kPVehiclePlayerOffset = 0x84;
constexpr std::size_t kPVehicleSimableOffset = 0x2C;
constexpr std::size_t kPVehicleSuspensionOffset = 0xF0;
constexpr std::size_t kPVehicleTransmissionOffset = 0xFC;
constexpr std::size_t kPVehicleRenderableOffset = 0x108;
constexpr std::size_t kPVehicleAttributeCollectionOffset = 0xD4;
constexpr std::size_t kPVehicleReadableSize = 0x160;
constexpr std::size_t kAttributeCollectionParentOffset = 0x10;
constexpr std::size_t kAttributeCollectionKeyOffset = 0x20;
constexpr std::uint32_t kMaximumAttributeParentDepth = 16;
constexpr std::size_t kRigidBodyHolderOffset = 0x30;
constexpr std::size_t kSuspensionWheelTableOffset = 0x168;
constexpr std::size_t kDriveTorqueVtableSlot = 8;
constexpr std::size_t kWheelNormalLoadOffset = 0xD4;
constexpr std::size_t kWheelLateralForceOffset = 0xD8;
constexpr std::size_t kWheelLongitudinalForceOffset = 0xDC;
constexpr std::size_t kWheelTractionStateOffset = 0x110;
#if NFSMW_ENABLE_RWS_FORCE_TRACE || NFSMW_ENABLE_REAR_WHEEL_STEERING
constexpr std::size_t kWheelWorldForceOffset = 0x5C;
#endif
constexpr std::size_t kWheelReadableSize =
    kWheelTractionStateOffset + sizeof(float);
constexpr DWORD kSampleIntervalMs = 100;
constexpr DWORD kLogIntervalMs = 1000;
constexpr float kMaximumObservedMagnitude = 100000000.0f;
constexpr std::uint8_t kRelativeCallOpcode = 0xE8u;
constexpr std::size_t kRelativeCallSize = 5;
constexpr std::size_t kTireForceSignatureSize = 25;
constexpr std::size_t kDriveTorqueSignatureSize = 4;
constexpr std::size_t kDiagnosticByteLimit = 32;
#if NFSMW_ENABLE_RWS_FORCE_TRACE || NFSMW_ENABLE_REAR_WHEEL_STEERING
constexpr std::uintptr_t kFourWheelResultRva = 0x002B6FB0u;
constexpr std::uintptr_t kFourWheelResultCallRva = 0x002B736Cu;
constexpr char kFourWheelResultSignature[] =
    "83 EC 18 53 55 8B 6C 24 24 8A 45 68 56 8B F1";
constexpr char kFourWheelResultCallSignature[] = "E8 3F FC FF FF";
constexpr std::size_t kResultSuspensionOffset = 0x88;
constexpr std::size_t kResultFirstSteeringOffset = 0x44;
constexpr std::uintptr_t kWheelVisualRva = 0x00346750u;
constexpr std::uintptr_t kWheelVisualCallRva = 0x0035660Du;
constexpr char kWheelVisualSignature[] =
    "55 8B EC 83 E4 F0 83 EC 64 53 56 8B 75 10 57 8B F9";
constexpr char kWheelVisualCallSignature[] = "E8 3E 01 FF FF";
constexpr std::size_t kWheelVisualSignatureSize = 17;
constexpr std::size_t kVisualOwnerOffset = 0x08;
constexpr std::size_t kVisualField44Offset = 0x44;
// The visual class embeds an Attrib::Instance at +0x14, so its Collection
// pointer occupies the second Instance field at +0x18.
constexpr std::size_t kVisualAttributeCollectionOffset = 0x18;
constexpr std::size_t kVisualReadableSize = 0x400;
constexpr std::size_t kVisualTraceSlotCount = 16;
constexpr std::uintptr_t kFourWheelSolverRva = 0x002AA020u;
constexpr std::uintptr_t kFourWheelSolverCallRva = 0x002B2086u;
constexpr std::uintptr_t kFourWheelSolverCallContextRva = 0x002B207Fu;
constexpr char kFourWheelSolverSignature[] =
    "81 EC A4 01 00 00 53 56 8B B4 24 B0 01 00 00 8B 86 C0 00 00 00";
constexpr char kFourWheelSolverCallContextSignature[] =
    "8D 4C 24 30 51 8B CE E8 95 7F FF FF BD 04 00 00 00";
constexpr std::size_t kFourWheelSolverSignatureSize = 21;
constexpr std::size_t kFourWheelSolverCallContextSize = 17;
constexpr std::size_t kFourWheelSolverCallOffset = 7;
constexpr std::uintptr_t kMatrixYawRva = 0x002BDD20u;
constexpr std::size_t kWheelDirectionOffset = 0x8C;
constexpr std::size_t kWheelContactNormalOffset = 0x3C;
constexpr std::size_t kRearMatrixSet0Offset = 0x1B0;
constexpr std::size_t kRearMatrixSet1Offset = 0x2B0;
constexpr std::size_t kWheelMatrixStride = 0x40;
constexpr std::size_t kMatrixBytes = 0x40;
constexpr DWORD kRearSteeringFreshnessMs = 250;
constexpr float kAngleUnitsPerRadian = 10430.3783504704527f;
constexpr std::uint32_t kNoMatchedOffset = 0xFFFFFFFFu;
constexpr std::uint32_t kVisualRelationSelfPvehicle = 1u << 0;
constexpr std::uint32_t kVisualRelationOwnerPvehicle = 1u << 1;
constexpr std::uint32_t kVisualRelationField44Pvehicle = 1u << 2;
constexpr std::uint32_t kVisualRelationSelfPlayer = 1u << 3;
constexpr std::uint32_t kVisualRelationOwnerPlayer = 1u << 4;
constexpr std::uint32_t kVisualRelationField44Player = 1u << 5;
constexpr std::uint32_t kVisualRelationSelfPhysicsComponent = 1u << 6;
constexpr std::uint32_t kVisualRelationOwnerPhysicsComponent = 1u << 7;
constexpr std::uint32_t kVisualRelationField44PhysicsComponent = 1u << 8;
constexpr std::uint32_t kVisualRelationSelfRenderable = 1u << 9;
constexpr std::uint32_t kVisualRelationOwnerRenderable = 1u << 10;
constexpr std::uint32_t kVisualRelationField44Renderable = 1u << 11;
constexpr std::uint32_t kVisualRelationSelfSimable = 1u << 12;
constexpr std::uint32_t kVisualRelationOwnerSimable = 1u << 13;
constexpr std::uint32_t kVisualRelationField44Simable = 1u << 14;
constexpr std::uint32_t kVisualRelationSelfRigidBody = 1u << 15;
constexpr std::uint32_t kVisualRelationOwnerRigidBody = 1u << 16;
constexpr std::uint32_t kVisualRelationField44RigidBody = 1u << 17;
constexpr std::uint32_t kVisualRelationSelfSuspension = 1u << 18;
constexpr std::uint32_t kVisualRelationOwnerSuspension = 1u << 19;
constexpr std::uint32_t kVisualRelationField44Suspension = 1u << 20;
constexpr std::uint32_t kVisualRelationSelfTransmission = 1u << 21;
constexpr std::uint32_t kVisualRelationOwnerTransmission = 1u << 22;
constexpr std::uint32_t kVisualRelationField44Transmission = 1u << 23;
constexpr std::uint32_t kVisualRelationCollectionPointer = 1u << 24;
constexpr std::uint32_t kVisualRelationCollectionKey = 1u << 25;
#endif
constexpr unsigned kMaximumPeSections = 96;

constexpr std::uint32_t kDiagnosticTireText = 1u << 0;
constexpr std::uint32_t kDiagnosticTireEntryAddress = 1u << 1;
constexpr std::uint32_t kDiagnosticTireEntryUnique = 1u << 2;
constexpr std::uint32_t kDiagnosticTireCallAddress = 1u << 3;
constexpr std::uint32_t kDiagnosticTireCallUnique = 1u << 4;
constexpr std::uint32_t kDiagnosticTireRel32Decode = 1u << 5;
constexpr std::uint32_t kDiagnosticTireRel32Target = 1u << 6;
constexpr std::uint32_t kDiagnosticTorqueText = 1u << 7;
constexpr std::uint32_t kDiagnosticTorqueEntryAddress = 1u << 8;
constexpr std::uint32_t kDiagnosticTorqueEntrySignature = 1u << 9;

struct PublishedWindow {
    std::atomic<bool> open{false};
    std::atomic<bool> wheelsValid{false};
    std::atomic<bool> transmissionValid{false};
    std::atomic<std::uint32_t> physicsThread{0};
    std::atomic<std::uint32_t> generation{0};
    std::atomic<std::uint32_t> groundedWheels{0};
    std::atomic<bool> driftActive{false};
    std::atomic<std::uint64_t> physicsSerial{0};
    std::atomic<std::uintptr_t> pvehicle{0};
    std::atomic<std::uintptr_t> player{0};
    std::atomic<std::uintptr_t> suspension{0};
    std::atomic<std::uintptr_t> transmission{0};
    std::array<std::atomic<std::uintptr_t>,
               handling_probe_state::kWheelCount>
        wheels{};
};

#if NFSMW_ENABLE_REAR_WHEEL_STEERING
struct PublishedRearSteering {
    std::atomic<bool> enabled{false};
    std::atomic<std::uintptr_t> pvehicle{0};
    std::atomic<std::uintptr_t> suspension{0};
    std::atomic<std::uint32_t> collectionKey{0};
    std::atomic<std::uint64_t> physicsSerial{0};
    std::atomic<std::uint32_t> angleBits{0};
    std::atomic<DWORD> publishTick{0};
};

struct RearSteeringSnapshot {
    std::uintptr_t pvehicle = 0;
    std::uintptr_t suspension = 0;
    std::uint32_t collectionKey = 0;
    std::uint64_t physicsSerial = 0;
    float angleRad = 0.0f;
};
#endif

struct CallContext {
    std::uint32_t generation = 0;
    std::uint32_t groundedWheels = 0;
    std::uint64_t physicsSerial = 0;
    bool driftActive = false;
    std::size_t wheelIndex = handling_probe_state::kWheelCount;
};

struct WheelFields {
    float normalLoad = 0.0f;
    float lateralForce = 0.0f;
    float longitudinalForce = 0.0f;
    float tractionState = 0.0f;
};

struct WheelStats {
    std::uint64_t calls = 0;
    std::uint64_t driftCalls = 0;
    std::uint64_t invalidSamples = 0;
    WheelFields before{};
    WheelFields after{};
    std::array<float, 5> arguments{};
    float returnValue = 0.0f;
    bool hasValue = false;
#if NFSMW_ENABLE_RWS_FORCE_TRACE
    std::array<float, 3> worldForce{};
    bool worldForceValid = false;
#endif
};

struct TorqueStats {
    std::uint64_t calls = 0;
    std::uint64_t driftCalls = 0;
    std::uint64_t positive = 0;
    std::uint64_t zero = 0;
    std::uint64_t negative = 0;
    std::uint64_t driftPositive = 0;
    std::uint64_t driftZero = 0;
    std::uint64_t driftNegative = 0;
    std::uint64_t invalidSamples = 0;
    float fieldValue = 0.0f;
    float returnValue = 0.0f;
    bool hasValue = false;
};

struct AggregateStats {
    std::uint32_t generation = 0;
    std::uint64_t sampledFrames = 0;
    std::array<WheelStats, handling_probe_state::kWheelCount> wheels{};
    TorqueStats torque{};
#if NFSMW_ENABLE_RWS_FORCE_TRACE
    std::uint64_t fourWheelResultCalls = 0;
    std::uint64_t fourWheelResultInvalid = 0;
    std::array<float, 4> resultBefore{};
    std::array<float, 4> resultAfter{};
    bool resultBeforeValid = false;
    bool resultValid = false;
#endif
};

#if NFSMW_ENABLE_RWS_FORCE_TRACE || NFSMW_ENABLE_REAR_WHEEL_STEERING
struct VisualTraceSlot {
    std::atomic<std::uintptr_t> self{0};
    std::atomic<std::uintptr_t> owner{0};
    std::atomic<std::uintptr_t> field44{0};
    std::atomic<std::uintptr_t> renderable{0};
    std::atomic<std::uintptr_t> visualCollection{0};
    std::atomic<std::uintptr_t> pvehicleCollection{0};
    std::atomic<std::uint32_t> visualCollectionKey{0};
    std::atomic<std::uint32_t> pvehicleCollectionKey{0};
    std::atomic<std::uint32_t> collectionPointerDepth{kNoMatchedOffset};
    std::atomic<std::uint32_t> collectionKeyDepth{kNoMatchedOffset};
    std::atomic<std::uint32_t> pvehicleSelfOffset{kNoMatchedOffset};
    std::atomic<std::uint32_t> pvehicleOwnerOffset{kNoMatchedOffset};
    std::atomic<std::uint32_t> pvehicleField44Offset{kNoMatchedOffset};
    std::atomic<std::uint32_t> visualPvehicleOffset{kNoMatchedOffset};
    std::atomic<std::uint32_t> visualRenderableOffset{kNoMatchedOffset};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint32_t> relationMask{0};
};
#endif

struct PatternScanSummary {
    std::size_t matches = 0;
    std::uintptr_t firstMatch = 0;
    bool expectedFound = false;
    bool scanned = false;
};

enum class RelativeCallFailure {
    kNone,
    kAddressOutOfBounds,
    kReadFault,
    kOpcodeMismatch,
    kTargetOverflow,
};

std::atomic<std::uintptr_t> g_imageBase{0};
std::atomic<std::size_t> g_imageSize{0};
std::atomic<std::uintptr_t> g_textStart{0};
std::atomic<std::size_t> g_textSize{0};
std::atomic<TireForceFn> g_tireOriginal{nullptr};
std::atomic<DriveTorqueFn> g_driveTorqueOriginal{nullptr};
#if NFSMW_ENABLE_RWS_FORCE_TRACE || NFSMW_ENABLE_REAR_WHEEL_STEERING
std::atomic<WheelVisualFn> g_wheelVisualOriginal{nullptr};
std::atomic<bool> g_wheelVisualArmed{false};
std::array<VisualTraceSlot, kVisualTraceSlotCount> g_visualTrace{};
#endif
#if NFSMW_ENABLE_RWS_FORCE_TRACE
std::atomic<FourWheelResultFn> g_fourWheelResultOriginal{nullptr};
std::atomic<bool> g_fourWheelResultArmed{false};
std::atomic<std::uint64_t> g_fourWheelRawCalls{0};
std::atomic<std::uintptr_t> g_fourWheelLastSelf{0};
std::atomic<std::uintptr_t> g_fourWheelLastSuspension{0};
#endif
#if NFSMW_ENABLE_REAR_WHEEL_STEERING
std::atomic<FourWheelResultFn> g_fourWheelSolverOriginal{nullptr};
std::atomic<bool> g_fourWheelSolverArmed{false};
PublishedRearSteering g_rearSteering{};
std::atomic<std::uint64_t> g_rearPhysicsWrites{0};
std::atomic<std::uint64_t> g_rearPhysicsRestores{0};
std::atomic<std::uint64_t> g_rearPhysicsRestoreFailures{0};
std::atomic<std::uint64_t> g_rearVisualWrites{0};
std::atomic<std::uint64_t> g_rearVisualRejected{0};
std::atomic<std::uint32_t> g_rearPhysicsInstallStage{0};
std::atomic<std::uint32_t> g_solverEntryMatches{0};
std::atomic<std::uint32_t> g_solverCallContextMatches{0};
std::atomic<std::uint32_t> g_decodedSolverRva{0};
std::atomic<int> g_rearPhysicsCreateStatus{-1};
std::atomic<int> g_rearPhysicsEnableStatus{-1};
#endif
std::atomic<std::uintptr_t> g_tireTarget{0};
std::atomic<std::uint64_t> g_tireRawCalls{0};
std::atomic<std::uintptr_t> g_tireLastSelf{0};
std::atomic<std::uintptr_t> g_driveTorqueTarget{0};
std::atomic<bool> g_tireArmed{false};
std::atomic<bool> g_driveTorqueArmed{false};
std::atomic<bool> g_driveTorqueProfileValid{false};
std::atomic<bool> g_driveTorqueHookAttempted{false};
std::atomic<bool> g_installAttempted{false};
std::atomic<std::uint32_t> g_validationDiagnosticMask{0};
std::atomic<std::uint32_t> g_samplingOwnerThread{0};
std::atomic<DWORD> g_lastSampleTick{0};
std::atomic<DWORD> g_lastLogTick{0};
PublishedWindow g_window{};
handling_probe_state::GenerationTracker g_generation{};
handling_probe_state::VehicleKey g_lastKey{};
PlayerFrame g_lastFrame{};
AggregateStats g_stats{};

void Log(const char* message) {
    if (message == nullptr) {
        return;
    }
    char line[1200]{};
    std::snprintf(line, sizeof(line),
                  "[nfsmw_drift_assist] %s\n", message);
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(line);
}

#if NFSMW_ENABLE_RWS_FORCE_TRACE
void ForceTraceFile(const char* message) {
    if (message == nullptr) return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&ForceTraceFile), &module)) {
        return;
    }
    char path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    char* separator = std::strrchr(path, '\\');
    if (separator == nullptr) return;
    constexpr char fileName[] = "\\RWS_TRUE_FORCE_TRACE_ONLY.log";
    if (static_cast<std::size_t>(separator - path) +
            sizeof(fileName) > sizeof(path)) {
        return;
    }
    std::memcpy(separator, fileName, sizeof(fileName));
    const HANDLE file = CreateFileA(
        path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    (void)WriteFile(file, message,
                    static_cast<DWORD>(std::strlen(message)), &written, nullptr);
    (void)WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}
#endif

const char* TextRangeFailureReason(
    handling_probe_state::TextRangeFailure failure) {
    using handling_probe_state::TextRangeFailure;
    switch (failure) {
        case TextRangeFailure::None:
            return "none";
        case TextRangeFailure::ImageUnavailable:
            return "image-unavailable";
        case TextRangeFailure::TextUnavailable:
            return "text-unavailable";
        case TextRangeFailure::StartBeforeImage:
            return "text-start-before-image";
        case TextRangeFailure::StartOutsideImage:
            return "text-start-outside-image";
        case TextRangeFailure::SpanOutsideImage:
            return "text-span-outside-image";
    }
    return "unknown";
}

bool AddAddress(std::uintptr_t base,
                std::size_t offset,
                std::uintptr_t* result) {
    if (result == nullptr ||
        base > std::numeric_limits<std::uintptr_t>::max() - offset) {
        return false;
    }
    *result = base + offset;
    return true;
}

bool ReadableRange(const void* address, std::size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xffu;
    if (protection != PAGE_READONLY &&
        protection != PAGE_READWRITE &&
        protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY) {
        return false;
    }
    const std::uintptr_t begin =
        reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t regionBegin =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + info.RegionSize;
    return regionEnd >= regionBegin && begin >= regionBegin &&
           size <= regionEnd - begin;
}

bool ImageRange(const void* address, std::size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    const std::size_t imageSize =
        g_imageSize.load(std::memory_order_acquire);
    const std::uintptr_t begin =
        reinterpret_cast<std::uintptr_t>(address);
    if (base == 0 || imageSize == 0 || begin < base) {
        return false;
    }
    const std::uintptr_t offset = begin - base;
    return offset <= imageSize && size <= imageSize - offset &&
           ReadableRange(address, size);
}

bool ExecutableImageAddress(const void* address) {
    if (!ImageRange(address, 1)) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xffu;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool ModuleHeaders(const IMAGE_NT_HEADERS32** ntHeaders) {
    if (ntHeaders == nullptr) {
        return false;
    }
    *ntHeaders = nullptr;
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    if (!ImageRange(reinterpret_cast<const void*>(base),
                    sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return false;
    }
    std::uintptr_t ntAddress = 0;
    if (!AddAddress(base, static_cast<std::size_t>(dos->e_lfanew),
                    &ntAddress) ||
        !ImageRange(reinterpret_cast<const void*>(ntAddress),
                    sizeof(IMAGE_NT_HEADERS32))) {
        return false;
    }
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS32*>(ntAddress);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.SizeOfOptionalHeader !=
            sizeof(IMAGE_OPTIONAL_HEADER32) ||
        nt->FileHeader.NumberOfSections == 0 ||
        nt->FileHeader.NumberOfSections > kMaximumPeSections) {
        return false;
    }
    *ntHeaders = nt;
    return true;
}

bool SectionTable(const IMAGE_NT_HEADERS32* nt,
                  const IMAGE_SECTION_HEADER** sectionsOut,
                  unsigned* countOut) {
    if (nt == nullptr || sectionsOut == nullptr || countOut == nullptr) {
        return false;
    }
    *sectionsOut = nullptr;
    *countOut = 0;
    const unsigned count = nt->FileHeader.NumberOfSections;
    if (count == 0 || count > kMaximumPeSections ||
        count > std::numeric_limits<std::size_t>::max() /
                    sizeof(IMAGE_SECTION_HEADER)) {
        return false;
    }
    std::uintptr_t optionalHeader = 0;
    std::uintptr_t sectionAddress = 0;
    if (!AddAddress(reinterpret_cast<std::uintptr_t>(nt),
                    offsetof(IMAGE_NT_HEADERS32, OptionalHeader),
                    &optionalHeader) ||
        !AddAddress(optionalHeader, nt->FileHeader.SizeOfOptionalHeader,
                    &sectionAddress)) {
        return false;
    }
    const std::size_t sectionBytes =
        static_cast<std::size_t>(count) * sizeof(IMAGE_SECTION_HEADER);
    if (!ImageRange(reinterpret_cast<const void*>(sectionAddress),
                    sectionBytes)) {
        return false;
    }
    *sectionsOut =
        reinterpret_cast<const IMAGE_SECTION_HEADER*>(sectionAddress);
    *countOut = count;
    return true;
}

bool ReadOnlyImageRange(const void* address, std::size_t size) {
    if (!ImageRange(address, size)) {
        return false;
    }
    const IMAGE_NT_HEADERS32* nt = nullptr;
    if (!ModuleHeaders(&nt)) {
        return false;
    }
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    const std::uintptr_t begin =
        reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t offset = begin - base;
    const std::size_t imageSize =
        g_imageSize.load(std::memory_order_acquire);
    const IMAGE_SECTION_HEADER* sections = nullptr;
    unsigned sectionCount = 0;
    if (!SectionTable(nt, &sections, &sectionCount)) {
        return false;
    }
    for (unsigned index = 0; index < sectionCount; ++index) {
        const std::size_t virtualSize =
            static_cast<std::size_t>(sections[index].Misc.VirtualSize);
        const std::size_t rawSize =
            static_cast<std::size_t>(sections[index].SizeOfRawData);
        const std::size_t span =
            virtualSize > rawSize ? virtualSize : rawSize;
        const std::size_t sectionBegin =
            static_cast<std::size_t>(sections[index].VirtualAddress);
        if (span == 0 || sectionBegin >= imageSize ||
            span > imageSize - sectionBegin) {
            return false;
        }
        if (offset < sectionBegin || offset - sectionBegin > span ||
            size > span - (offset - sectionBegin)) {
            continue;
        }
        const DWORD characteristics = sections[index].Characteristics;
        return (characteristics & IMAGE_SCN_MEM_READ) != 0 &&
               (characteristics & IMAGE_SCN_MEM_WRITE) == 0 &&
               (characteristics & IMAGE_SCN_MEM_EXECUTE) == 0;
    }
    return false;
}

bool TextSectionRange(std::uintptr_t* startOut,
                      std::size_t* sizeOut) {
    if (startOut == nullptr || sizeOut == nullptr) {
        return false;
    }
    *startOut = 0;
    *sizeOut = 0;
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    const std::size_t imageSize =
        g_imageSize.load(std::memory_order_acquire);
    const std::uintptr_t textStart =
        g_textStart.load(std::memory_order_acquire);
    const std::size_t textSize =
        g_textSize.load(std::memory_order_acquire);
    if (handling_probe_state::ValidateTextRange(
            base, imageSize, textStart, textSize) !=
        handling_probe_state::TextRangeFailure::None) {
        return false;
    }
    *startOut = textStart;
    *sizeOut = textSize;
    return true;
}

bool TextSectionContains(std::uintptr_t address, std::size_t size) {
    std::uintptr_t textStart = 0;
    std::size_t textSize = 0;
    if (address == 0 || size == 0 ||
        !TextSectionRange(&textStart, &textSize) ||
        address < textStart) {
        return false;
    }
    const std::uintptr_t offset = address - textStart;
    return offset <= textSize && size <= textSize - offset;
}

bool SafeReadImageBytes(
    std::uintptr_t address,
    std::size_t requested,
    std::array<std::uint8_t, kDiagnosticByteLimit>* bytes,
    std::size_t* countOut) {
    if (bytes == nullptr || countOut == nullptr) {
        return false;
    }
    *countOut = 0;
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    const std::size_t imageSize =
        g_imageSize.load(std::memory_order_acquire);
    if (address == 0 || requested == 0 || base == 0 ||
        imageSize == 0 || address < base) {
        return false;
    }
    const std::uintptr_t offset = address - base;
    if (offset >= imageSize) {
        return false;
    }
    if (requested > kDiagnosticByteLimit) {
        return false;
    }
    const std::size_t count = requested;
    if (count == 0 || count > imageSize - offset ||
        !ImageRange(reinterpret_cast<const void*>(address), count)) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        std::memcpy(bytes->data(), reinterpret_cast<const void*>(address),
                    count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(bytes->data(), reinterpret_cast<const void*>(address),
                count);
#endif
    *countOut = count;
    return true;
}

void FormatImageBytes(std::uintptr_t address,
                      std::size_t requested,
                      char* output,
                      std::size_t outputSize) {
    if (output == nullptr || outputSize == 0) {
        return;
    }
    output[0] = '\0';
    std::array<std::uint8_t, kDiagnosticByteLimit> bytes{};
    std::size_t count = 0;
    if (!SafeReadImageBytes(address, requested, &bytes, &count)) {
        std::snprintf(output, outputSize, "unavailable");
        return;
    }
    std::size_t used = 0;
    for (std::size_t index = 0; index < count && used < outputSize;
         ++index) {
        const int written = std::snprintf(
            output + used, outputSize - used, "%s%02X",
            index == 0 ? "" : " ",
            static_cast<unsigned>(bytes[index]));
        if (written < 0 ||
            static_cast<std::size_t>(written) >= outputSize - used) {
            output[outputSize - 1] = '\0';
            return;
        }
        used += static_cast<std::size_t>(written);
    }
}

bool ClaimValidationDiagnostic(std::uint32_t diagnostic) {
    return (g_validationDiagnosticMask.fetch_or(
                diagnostic, std::memory_order_relaxed) &
            diagnostic) == 0;
}

void LogAddressValidationFailureOnce(std::uint32_t diagnostic,
                                     const char* channel,
                                     const char* stage,
                                     const char* reason,
                                     std::uintptr_t rva,
                                     std::uintptr_t address,
                                     std::size_t inspectBytes) {
    if (!ClaimValidationDiagnostic(diagnostic)) {
        return;
    }
    char actual[128]{};
    FormatImageBytes(address, inspectBytes, actual, sizeof(actual));
    char message[640]{};
    std::snprintf(
        message, sizeof(message),
        "handling probe validation channel=%s result=failed stage=%s reason=%s expected_rva=0x%08llX expected_address=%p actual_bytes=[%s]",
        channel, stage, reason,
        static_cast<unsigned long long>(rva),
        reinterpret_cast<void*>(address), actual);
    Log(message);
}

const char* PatternFailureReason(const PatternScanSummary& summary) {
    if (!summary.scanned) {
        return "scan-unavailable";
    }
    if (summary.matches == 0) {
        return "signature-not-found";
    }
    if (summary.matches == 1 && !summary.expectedFound) {
        return "signature-at-other-address";
    }
    return "signature-not-unique";
}

void LogPatternValidationFailureOnce(
    std::uint32_t diagnostic,
    const char* channel,
    const char* stage,
    std::uintptr_t rva,
    std::uintptr_t address,
    std::size_t inspectBytes,
    const char* signature,
    const PatternScanSummary& summary) {
    if (!ClaimValidationDiagnostic(diagnostic)) {
        return;
    }
    char actual[128]{};
    FormatImageBytes(address, inspectBytes, actual, sizeof(actual));
    char message[800]{};
    std::snprintf(
        message, sizeof(message),
        "handling probe validation channel=%s result=failed stage=%s reason=%s expected_rva=0x%08llX expected_address=%p matches=%llu expected_found=%d first_match=%p expected_aob=\"%.96s\" actual_bytes=[%s]",
        channel, stage, PatternFailureReason(summary),
        static_cast<unsigned long long>(rva),
        reinterpret_cast<void*>(address),
        static_cast<unsigned long long>(summary.matches),
        summary.expectedFound ? 1 : 0,
        reinterpret_cast<void*>(summary.firstMatch),
        signature == nullptr ? "" : signature, actual);
    Log(message);
}

void LogExactSignatureFailureOnce(std::uint32_t diagnostic,
                                  const char* channel,
                                  const char* stage,
                                  std::uintptr_t rva,
                                  std::uintptr_t address,
                                  std::size_t inspectBytes,
                                  const char* signature) {
    if (!ClaimValidationDiagnostic(diagnostic)) {
        return;
    }
    char actual[128]{};
    FormatImageBytes(address, inspectBytes, actual, sizeof(actual));
    char message[640]{};
    std::snprintf(
        message, sizeof(message),
        "handling probe validation channel=%s result=failed stage=%s reason=bytes-mismatch expected_rva=0x%08llX expected_address=%p expected_aob=\"%.96s\" actual_bytes=[%s]",
        channel, stage, static_cast<unsigned long long>(rva),
        reinterpret_cast<void*>(address),
        signature == nullptr ? "" : signature, actual);
    Log(message);
}

const char* RelativeCallFailureReason(RelativeCallFailure failure) {
    switch (failure) {
        case RelativeCallFailure::kAddressOutOfBounds:
            return "call-site-aob";
        case RelativeCallFailure::kReadFault:
            return "read-fault";
        case RelativeCallFailure::kOpcodeMismatch:
            return "opcode-not-e8";
        case RelativeCallFailure::kTargetOverflow:
            return "decoded-target-overflow";
        case RelativeCallFailure::kNone:
            return "none";
    }
    return "unknown";
}

void LogRelativeCallFailureOnce(std::uint32_t diagnostic,
                                RelativeCallFailure failure,
                                std::uintptr_t callSite) {
    if (!ClaimValidationDiagnostic(diagnostic)) {
        return;
    }
    char actual[128]{};
    FormatImageBytes(callSite, kRelativeCallSize, actual,
                     sizeof(actual));
    char message[560]{};
    std::snprintf(
        message, sizeof(message),
        "handling probe validation channel=tire-force result=failed stage=rel32-decode reason=%s call_site=%p actual_bytes=[%s]",
        RelativeCallFailureReason(failure),
        reinterpret_cast<void*>(callSite), actual);
    Log(message);
}

void LogRelativeTargetMismatchOnce(std::uintptr_t callSite,
                                   std::uintptr_t expected,
                                   std::uintptr_t decoded) {
    if (!ClaimValidationDiagnostic(kDiagnosticTireRel32Target)) {
        return;
    }
    char actual[128]{};
    FormatImageBytes(callSite, kRelativeCallSize, actual,
                     sizeof(actual));
    char message[640]{};
    std::snprintf(
        message, sizeof(message),
        "handling probe validation channel=tire-force result=failed stage=rel32-target reason=target-mismatch call_site=%p expected_target=%p decoded_target=%p actual_bytes=[%s]",
        reinterpret_cast<void*>(callSite),
        reinterpret_cast<void*>(expected),
        reinterpret_cast<void*>(decoded), actual);
    Log(message);
}

bool SafeReadPointer(const void* address, std::uintptr_t* value) {
    if (value == nullptr ||
        !ReadableRange(address, sizeof(std::uintptr_t))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        *value = *reinterpret_cast<const std::uintptr_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *value = *reinterpret_cast<const std::uintptr_t*>(address);
#endif
    return true;
}

bool SafeReadFiniteFloat(const void* address, float* value) {
    if (value == nullptr || !ReadableRange(address, sizeof(float))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        *value = *reinterpret_cast<const float*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *value = *reinterpret_cast<const float*>(address);
#endif
    return std::isfinite(*value) &&
           std::fabs(*value) <= kMaximumObservedMagnitude;
}

bool PatternMatchesAt(std::uintptr_t target,
                      const char* signature,
                      std::size_t maximumBytes) {
    if (target == 0 || signature == nullptr || signature[0] == '\0' ||
        !ImageRange(reinterpret_cast<const void*>(target), maximumBytes)) {
        return false;
    }
    return nfsmw_aob_scan_range(target, maximumBytes, signature) == target;
}

bool UniquePatternAt(std::uintptr_t expected,
                     const char* signature,
                     PatternScanSummary* summary) {
    if (summary == nullptr) {
        return false;
    }
    *summary = PatternScanSummary{};
    std::uintptr_t textStart = 0;
    std::size_t textSize = 0;
    if (expected == 0 || signature == nullptr || signature[0] == '\0' ||
        !TextSectionRange(&textStart, &textSize) ||
        expected < textStart || expected - textStart >= textSize ||
        textSize >
            std::numeric_limits<std::uintptr_t>::max() - textStart) {
        return false;
    }
    summary->scanned = true;
    const std::uintptr_t end = textStart + textSize;
    std::uintptr_t cursor = textStart;
    while (cursor < end) {
        const std::uintptr_t match = nfsmw_aob_scan_range(
            cursor, static_cast<std::size_t>(end - cursor), signature);
        if (match == 0 || match < cursor || match >= end) {
            break;
        }
        if (summary->matches == 0) {
            summary->firstMatch = match;
        }
        ++summary->matches;
        summary->expectedFound = summary->expectedFound || match == expected;
        if (match == std::numeric_limits<std::uintptr_t>::max()) {
            break;
        }
        cursor = match + 1;
    }
    return summary->matches == 1 && summary->expectedFound;
}

bool DecodeRelativeCall(std::uintptr_t callSite,
                        std::uintptr_t* target,
                        RelativeCallFailure* failureOut) {
    if (failureOut != nullptr) {
        *failureOut = RelativeCallFailure::kNone;
    }
    if (target == nullptr ||
        !ImageRange(reinterpret_cast<const void*>(callSite),
                    kRelativeCallSize)) {
        if (failureOut != nullptr) {
            *failureOut = RelativeCallFailure::kAddressOutOfBounds;
        }
        return false;
    }
    std::uint8_t opcode = 0;
    std::int32_t displacement = 0;
#if defined(_MSC_VER)
    __try {
        opcode = *reinterpret_cast<const std::uint8_t*>(callSite);
        displacement = *reinterpret_cast<const std::int32_t*>(callSite + 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (failureOut != nullptr) {
            *failureOut = RelativeCallFailure::kReadFault;
        }
        return false;
    }
#else
    opcode = *reinterpret_cast<const std::uint8_t*>(callSite);
    std::memcpy(&displacement,
                reinterpret_cast<const void*>(callSite + 1),
                sizeof(displacement));
#endif
    if (opcode != kRelativeCallOpcode) {
        if (failureOut != nullptr) {
            *failureOut = RelativeCallFailure::kOpcodeMismatch;
        }
        return false;
    }
    const std::int64_t resolved =
        static_cast<std::int64_t>(callSite) + kRelativeCallSize +
        static_cast<std::int64_t>(displacement);
    if (resolved < 0 ||
        static_cast<std::uint64_t>(resolved) >
            std::numeric_limits<std::uintptr_t>::max()) {
        if (failureOut != nullptr) {
            *failureOut = RelativeCallFailure::kTargetOverflow;
        }
        return false;
    }
    *target = static_cast<std::uintptr_t>(resolved);
    return true;
}

bool ResolveProfileCodeAddress(std::uintptr_t rva,
                               std::size_t requiredBytes,
                               std::uintptr_t* target,
                               const char** failureReason) {
    if (target == nullptr || failureReason == nullptr) {
        return false;
    }
    *target = 0;
    *failureReason = "internal-output-null";
    const std::uintptr_t base =
        g_imageBase.load(std::memory_order_acquire);
    if (rva == 0) {
        *failureReason = "profile-rva-zero";
        return false;
    }
    if (!AddAddress(base, static_cast<std::size_t>(rva), target)) {
        *failureReason = "address-overflow";
        return false;
    }
    const std::size_t imageSize =
        g_imageSize.load(std::memory_order_acquire);
    if (base == 0 || imageSize == 0 || *target < base) {
        *failureReason = "image-aob";
        return false;
    }
    const std::uintptr_t offset = *target - base;
    if (offset > imageSize || requiredBytes > imageSize - offset) {
        *failureReason = "image-aob";
        return false;
    }
    if (!ReadableRange(reinterpret_cast<const void*>(*target),
                       requiredBytes)) {
        *failureReason = "range-unreadable";
        return false;
    }
    if (!TextSectionContains(*target, requiredBytes)) {
        *failureReason = "text-aob";
        return false;
    }
    if (!ExecutableImageAddress(reinterpret_cast<const void*>(*target))) {
        *failureReason = "page-not-executable";
        return false;
    }
    *failureReason = "none";
    return true;
}

bool ValidateTireTarget(std::uintptr_t* targetOut) {
    if (targetOut == nullptr) {
        return false;
    }
    *targetOut = 0;
    std::uintptr_t target = 0;
    std::uintptr_t callSite = 0;
    std::uintptr_t decodedTarget = 0;
    std::uintptr_t textStart = 0;
    std::size_t textSize = 0;
    if (!TextSectionRange(&textStart, &textSize)) {
        const std::uintptr_t base =
            g_imageBase.load(std::memory_order_acquire);
        (void)AddAddress(base,
                         static_cast<std::size_t>(
                             target_profile::kTireForceRva),
                         &target);
        LogAddressValidationFailureOnce(
            kDiagnosticTireText, "tire-force", "text-contract",
            "validated-range-unavailable", target_profile::kTireForceRva,
            target, kTireForceSignatureSize);
        return false;
    }
    (void)textStart;
    (void)textSize;

    const char* entryAddressFailure = nullptr;
    const bool entryAddressValid = ResolveProfileCodeAddress(
        target_profile::kTireForceRva, kTireForceSignatureSize,
        &target, &entryAddressFailure);
    if (!entryAddressValid) {
        LogAddressValidationFailureOnce(
            kDiagnosticTireEntryAddress, "tire-force",
            "entry-address", entryAddressFailure,
            target_profile::kTireForceRva, target,
            kTireForceSignatureSize);
    }

    const char* callAddressFailure = nullptr;
    const bool callAddressValid = ResolveProfileCodeAddress(
        target_profile::kTireForceCallRva, kRelativeCallSize,
        &callSite, &callAddressFailure);
    if (!callAddressValid) {
        LogAddressValidationFailureOnce(
            kDiagnosticTireCallAddress, "tire-force",
            "call-site-address", callAddressFailure,
            target_profile::kTireForceCallRva, callSite,
            kRelativeCallSize);
    }

    PatternScanSummary entrySummary{};
    const bool entryUnique =
        entryAddressValid &&
        UniquePatternAt(target, target_profile::kTireForceSignature,
                        &entrySummary);
    if (entryAddressValid && !entryUnique) {
        LogPatternValidationFailureOnce(
            kDiagnosticTireEntryUnique, "tire-force",
            "entry-unique", target_profile::kTireForceRva, target,
            kTireForceSignatureSize,
            target_profile::kTireForceSignature, entrySummary);
    }

    PatternScanSummary callSummary{};
    const bool callUnique =
        callAddressValid &&
        UniquePatternAt(callSite,
                        target_profile::kTireForceCallSignature,
                        &callSummary);
    if (callAddressValid && !callUnique) {
        LogPatternValidationFailureOnce(
            kDiagnosticTireCallUnique, "tire-force",
            "call-site-unique", target_profile::kTireForceCallRva,
            callSite, kRelativeCallSize,
            target_profile::kTireForceCallSignature, callSummary);
    }

    RelativeCallFailure relativeFailure = RelativeCallFailure::kNone;
    const bool relativeDecoded =
        callAddressValid &&
        DecodeRelativeCall(callSite, &decodedTarget, &relativeFailure);
    if (callAddressValid && !relativeDecoded) {
        LogRelativeCallFailureOnce(kDiagnosticTireRel32Decode,
                                   relativeFailure, callSite);
    }
    const bool relativeTargetMatches =
        relativeDecoded && entryAddressValid && decodedTarget == target;
    if (relativeDecoded && entryAddressValid &&
        !relativeTargetMatches) {
        LogRelativeTargetMismatchOnce(callSite, target, decodedTarget);
    }

    if (!entryAddressValid || !callAddressValid || !entryUnique ||
        !callUnique || !relativeDecoded || !relativeTargetMatches) {
        return false;
    }
    *targetOut = target;
    return true;
}

bool ValidateDriveTorqueTarget(std::uintptr_t* targetOut) {
    if (targetOut == nullptr) {
        return false;
    }
    *targetOut = 0;
    std::uintptr_t target = 0;
    std::uintptr_t textStart = 0;
    std::size_t textSize = 0;
    if (!TextSectionRange(&textStart, &textSize)) {
        const std::uintptr_t base =
            g_imageBase.load(std::memory_order_acquire);
        (void)AddAddress(base,
                         static_cast<std::size_t>(
                             target_profile::kDriveTorqueRva),
                         &target);
        LogAddressValidationFailureOnce(
            kDiagnosticTorqueText, "drive-torque", "text-contract",
            "validated-range-unavailable", target_profile::kDriveTorqueRva,
            target, kDriveTorqueSignatureSize);
        return false;
    }
    (void)textStart;
    (void)textSize;

    const char* entryAddressFailure = nullptr;
    if (!ResolveProfileCodeAddress(
            target_profile::kDriveTorqueRva,
            kDriveTorqueSignatureSize, &target,
            &entryAddressFailure)) {
        LogAddressValidationFailureOnce(
            kDiagnosticTorqueEntryAddress, "drive-torque",
            "entry-address", entryAddressFailure,
            target_profile::kDriveTorqueRva, target,
            kDriveTorqueSignatureSize);
        return false;
    }
    if (!PatternMatchesAt(target,
                          target_profile::kDriveTorqueSignature,
                          kDriveTorqueSignatureSize)) {
        LogExactSignatureFailureOnce(
            kDiagnosticTorqueEntrySignature, "drive-torque",
            "entry-signature", target_profile::kDriveTorqueRva,
            target, kDriveTorqueSignatureSize,
            target_profile::kDriveTorqueSignature);
        return false;
    }
    *targetOut = target;
    return true;
}

#if NFSMW_ENABLE_RWS_FORCE_TRACE || NFSMW_ENABLE_REAR_WHEEL_STEERING
bool ReadOwnedPointer(std::uintptr_t owner,
                      std::size_t offset,
                      std::uintptr_t* value);
bool ContextStillOpen(const CallContext& context);

#if NFSMW_ENABLE_RWS_FORCE_TRACE
bool ValidateFourWheelResultTarget(std::uintptr_t* targetOut) {
    if (targetOut == nullptr) return false;
    *targetOut = 0;
    std::uintptr_t target = 0;
    std::uintptr_t callSite = 0;
    const char* failure = nullptr;
    if (!ResolveProfileCodeAddress(kFourWheelResultRva,
                                   15,
                                   &target, &failure) ||
        !ResolveProfileCodeAddress(kFourWheelResultCallRva,
                                   5,
                                   &callSite, &failure)) {
        Log("RWS trace result channel disabled: entry or call outside verified image text");
        return false;
    }
    PatternScanSummary entrySummary{};
    PatternScanSummary callSummary{};
    std::uintptr_t decodedTarget = 0;
    RelativeCallFailure decodeFailure = RelativeCallFailure::kNone;
    if (!PatternMatchesAt(target, kFourWheelResultSignature, 15) ||
        !PatternMatchesAt(callSite, kFourWheelResultCallSignature, 5) ||
        !UniquePatternAt(target, kFourWheelResultSignature, &entrySummary) ||
        !UniquePatternAt(callSite, kFourWheelResultCallSignature,
                         &callSummary) ||
        !DecodeRelativeCall(callSite, &decodedTarget, &decodeFailure) ||
        decodedTarget != target) {
        Log("RWS trace result channel disabled: signature, uniqueness or sole call target mismatch");
        return false;
    }
    *targetOut = target;
    return true;
}
#endif

bool ValidateWheelVisualTarget(std::uintptr_t* targetOut) {
    if (targetOut == nullptr) return false;
    *targetOut = 0;
    std::uintptr_t target = 0;
    std::uintptr_t callSite = 0;
    const char* failure = nullptr;
    if (!ResolveProfileCodeAddress(kWheelVisualRva,
                                   kWheelVisualSignatureSize,
                                   &target, &failure) ||
        !ResolveProfileCodeAddress(kWheelVisualCallRva,
                                   kRelativeCallSize,
                                   &callSite, &failure)) {
        Log("RWS visual trace disabled: entry or call outside verified image text");
        return false;
    }
    PatternScanSummary entrySummary{};
    PatternScanSummary callSummary{};
    std::uintptr_t decodedTarget = 0;
    RelativeCallFailure decodeFailure = RelativeCallFailure::kNone;
    if (!PatternMatchesAt(target, kWheelVisualSignature,
                          kWheelVisualSignatureSize) ||
        !PatternMatchesAt(callSite, kWheelVisualCallSignature,
                          kRelativeCallSize) ||
        !UniquePatternAt(target, kWheelVisualSignature, &entrySummary) ||
        !UniquePatternAt(callSite, kWheelVisualCallSignature,
                         &callSummary) ||
        !DecodeRelativeCall(callSite, &decodedTarget, &decodeFailure) ||
        decodedTarget != target) {
        Log("RWS visual trace disabled: signature, uniqueness or sole call target mismatch");
        return false;
    }
    *targetOut = target;
    return true;
}

#if NFSMW_ENABLE_REAR_WHEEL_STEERING
bool ValidateFourWheelSolverTarget(std::uintptr_t* targetOut) {
    if (targetOut == nullptr) return false;
    *targetOut = 0;
    g_rearPhysicsInstallStage.store(1, std::memory_order_relaxed);
    std::uintptr_t target = 0;
    std::uintptr_t callContext = 0;
    std::uintptr_t callSite = 0;
    const char* failure = nullptr;
    if (!ResolveProfileCodeAddress(kFourWheelSolverRva,
                                   kFourWheelSolverSignatureSize,
                                   &target, &failure) ||
        !ResolveProfileCodeAddress(kFourWheelSolverCallRva,
                                   kRelativeCallSize,
                                   &callSite, &failure) ||
        !ResolveProfileCodeAddress(kFourWheelSolverCallContextRva,
                                   kFourWheelSolverCallContextSize,
                                   &callContext, &failure) ||
        callContext + kFourWheelSolverCallOffset != callSite) {
        Log("rear tire-direction writer disabled: solver entry or call is outside verified image text");
        return false;
    }
    g_rearPhysicsInstallStage.store(2, std::memory_order_relaxed);
    PatternScanSummary entrySummary{};
    PatternScanSummary callContextSummary{};
    std::uintptr_t decodedTarget = 0;
    RelativeCallFailure decodeFailure = RelativeCallFailure::kNone;
    if (!PatternMatchesAt(target, kFourWheelSolverSignature,
                          kFourWheelSolverSignatureSize)) {
        g_rearPhysicsInstallStage.store(3, std::memory_order_relaxed);
        Log("rear tire-direction writer disabled: solver entry signature mismatch");
        return false;
    }
    if (!PatternMatchesAt(callContext, kFourWheelSolverCallContextSignature,
                          kFourWheelSolverCallContextSize)) {
        g_rearPhysicsInstallStage.store(4, std::memory_order_relaxed);
        Log("rear tire-direction writer disabled: solver call-context signature mismatch");
        return false;
    }
    const bool entryUnique =
        UniquePatternAt(target, kFourWheelSolverSignature, &entrySummary);
    g_solverEntryMatches.store(
        static_cast<std::uint32_t>(entrySummary.matches),
        std::memory_order_relaxed);
    if (!entryUnique) {
        g_rearPhysicsInstallStage.store(5, std::memory_order_relaxed);
        Log("rear tire-direction writer disabled: solver entry signature is not unique");
        return false;
    }
    const bool contextUnique = UniquePatternAt(
        callContext, kFourWheelSolverCallContextSignature,
        &callContextSummary);
    g_solverCallContextMatches.store(
        static_cast<std::uint32_t>(callContextSummary.matches),
        std::memory_order_relaxed);
    if (!contextUnique) {
        g_rearPhysicsInstallStage.store(6, std::memory_order_relaxed);
        Log("rear tire-direction writer disabled: solver call context is not unique");
        return false;
    }
    if (!DecodeRelativeCall(callSite, &decodedTarget, &decodeFailure)) {
        g_rearPhysicsInstallStage.store(7, std::memory_order_relaxed);
        Log("rear tire-direction writer disabled: solver relative call cannot be decoded");
        return false;
    }
    const std::uintptr_t imageBase =
        g_imageBase.load(std::memory_order_acquire);
    if (decodedTarget >= imageBase &&
        decodedTarget - imageBase <=
            std::numeric_limits<std::uint32_t>::max()) {
        g_decodedSolverRva.store(
            static_cast<std::uint32_t>(decodedTarget - imageBase),
            std::memory_order_relaxed);
    }
    if (decodedTarget != target) {
        g_rearPhysicsInstallStage.store(8, std::memory_order_relaxed);
        Log("rear tire-direction writer disabled: solver relative call target mismatch");
        return false;
    }
    g_rearPhysicsInstallStage.store(9, std::memory_order_relaxed);
    *targetOut = target;
    return true;
}

bool ValidateMatrixYawTarget(MatrixYawFn* targetOut) {
    if (targetOut == nullptr) return false;
    *targetOut = nullptr;
    std::uintptr_t target = 0;
    const char* failure = nullptr;
    constexpr char signature[] =
        "55 8B EC 83 E4 F0 83 EC 40 8B 45 10 50 8D 4C 24 04";
    if (!ResolveProfileCodeAddress(kMatrixYawRva, 17, &target, &failure) ||
        !PatternMatchesAt(target, signature, 17) ||
        !ExecutableImageAddress(reinterpret_cast<const void*>(target))) {
        Log("rear visual writer disabled: matrix yaw helper validation failed");
        return false;
    }
    *targetOut = reinterpret_cast<MatrixYawFn>(target);
    return true;
}
#endif

std::uintptr_t ReadPointerField(std::uintptr_t object,
                                std::size_t offset) {
    std::uintptr_t value = 0;
    if (object == 0 ||
        !SafeReadPointer(reinterpret_cast<const void*>(object + offset),
                         &value)) {
        return 0;
    }
    return value;
}

std::uint32_t FirstAlignedReferenceOffset(std::uintptr_t object,
                                          std::size_t bytes,
                                          std::uintptr_t target) {
    if (object == 0 || target == 0) return kNoMatchedOffset;
    for (std::size_t offset = 0;
         offset + sizeof(std::uintptr_t) <= bytes;
         offset += sizeof(std::uintptr_t)) {
        if (ReadPointerField(object, offset) == target) {
            return static_cast<std::uint32_t>(offset);
        }
    }
    return kNoMatchedOffset;
}

std::uint32_t ReadCollectionKey(std::uintptr_t collection) {
    std::uintptr_t raw = 0;
    if (collection == 0 ||
        !SafeReadPointer(reinterpret_cast<const void*>(
                             collection + kAttributeCollectionKeyOffset),
                         &raw)) {
        return 0;
    }
    return static_cast<std::uint32_t>(raw);
}

void MatchCollectionChain(std::uintptr_t visualCollection,
                          std::uint32_t visualKey,
                          std::uintptr_t pvehicleCollection,
                          std::uint32_t* pointerDepth,
                          std::uint32_t* keyDepth) {
    if (pointerDepth == nullptr || keyDepth == nullptr) return;
    *pointerDepth = kNoMatchedOffset;
    *keyDepth = kNoMatchedOffset;
    std::uintptr_t current = pvehicleCollection;
    for (std::uint32_t depth = 0;
         current != 0 && depth < kMaximumAttributeParentDepth;
         ++depth) {
        if (*pointerDepth == kNoMatchedOffset && current == visualCollection) {
            *pointerDepth = depth;
        }
        if (*keyDepth == kNoMatchedOffset && visualKey != 0 &&
            ReadCollectionKey(current) == visualKey) {
            *keyDepth = depth;
        }
        const std::uintptr_t parent =
            ReadPointerField(current, kAttributeCollectionParentOffset);
        if (parent == current) break;
        current = parent;
    }
}

#if NFSMW_ENABLE_REAR_WHEEL_STEERING
float RearBitsToFloat(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint32_t RearFloatToBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool SnapshotRearSteering(RearSteeringSnapshot* snapshot) {
    if (snapshot == nullptr ||
        !g_rearSteering.enabled.load(std::memory_order_acquire)) {
        return false;
    }
    RearSteeringSnapshot candidate{};
    candidate.pvehicle =
        g_rearSteering.pvehicle.load(std::memory_order_relaxed);
    candidate.suspension =
        g_rearSteering.suspension.load(std::memory_order_relaxed);
    candidate.collectionKey =
        g_rearSteering.collectionKey.load(std::memory_order_relaxed);
    candidate.physicsSerial =
        g_rearSteering.physicsSerial.load(std::memory_order_relaxed);
    candidate.angleRad = RearBitsToFloat(
        g_rearSteering.angleBits.load(std::memory_order_relaxed));
    const DWORD tick =
        g_rearSteering.publishTick.load(std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    constexpr float maximumAngleRad = 15.0f * 3.14159265358979323846f / 180.0f;
    if (!g_rearSteering.enabled.load(std::memory_order_acquire) ||
        candidate.pvehicle == 0 || candidate.suspension < 0x4Cu ||
        candidate.collectionKey == 0 || candidate.physicsSerial == 0 ||
        tick == 0 || static_cast<DWORD>(now - tick) >
                         kRearSteeringFreshnessMs ||
        !std::isfinite(candidate.angleRad) ||
        std::fabs(candidate.angleRad) > maximumAngleRad) {
        return false;
    }
    std::uintptr_t currentSuspension = 0;
    if (!ReadOwnedPointer(candidate.pvehicle, kPVehicleSuspensionOffset,
                          &currentSuspension) ||
        currentSuspension != candidate.suspension) {
        return false;
    }
    *snapshot = candidate;
    return true;
}

bool WritableRange(void* address, std::size_t size) {
    if (address == nullptr || size == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xffu;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY) {
        return false;
    }
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t regionBegin =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + info.RegionSize;
    return regionEnd >= regionBegin && begin >= regionBegin &&
           size <= regionEnd - begin;
}

bool ReadVec3(std::uintptr_t address, std::array<float, 3>* value) {
    if (value == nullptr ||
        !ReadableRange(reinterpret_cast<const void*>(address),
                       3 * sizeof(float))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        std::memcpy(value->data(), reinterpret_cast<const void*>(address),
                    3 * sizeof(float));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(value->data(), reinterpret_cast<const void*>(address),
                3 * sizeof(float));
#endif
    for (float component : *value) {
        if (!std::isfinite(component) || std::fabs(component) > 4.0f) {
            return false;
        }
    }
    return true;
}

bool WriteVec3(std::uintptr_t address, const std::array<float, 3>& value) {
    if (!WritableRange(reinterpret_cast<void*>(address),
                       3 * sizeof(float))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        std::memcpy(reinterpret_cast<void*>(address), value.data(),
                    3 * sizeof(float));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(reinterpret_cast<void*>(address), value.data(),
                3 * sizeof(float));
#endif
    return true;
}

void ApplyRearSteeringVisual(std::uintptr_t self) {
    RearSteeringSnapshot snapshot{};
    if (!SnapshotRearSteering(&snapshot)) return;
    const std::uintptr_t visualCollection =
        ReadPointerField(self, kVisualAttributeCollectionOffset);
    const std::uint32_t visualKey = ReadCollectionKey(visualCollection);
    const std::uintptr_t pvehicleCollection =
        ReadPointerField(snapshot.pvehicle, kPVehicleAttributeCollectionOffset);
    std::uint32_t pointerDepth = kNoMatchedOffset;
    std::uint32_t keyDepth = kNoMatchedOffset;
    MatchCollectionChain(visualCollection, visualKey, pvehicleCollection,
                         &pointerDepth, &keyDepth);
    if (visualKey != snapshot.collectionKey ||
        keyDepth == kNoMatchedOffset) {
        g_rearVisualRejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    const MatrixYawFn rotate =
        reinterpret_cast<MatrixYawFn>(base + kMatrixYawRva);
    const std::int32_t angle = static_cast<std::int32_t>(
        std::lround(-snapshot.angleRad * kAngleUnitsPerRadian));
    const std::array<std::size_t, 4> offsets = {
        kRearMatrixSet0Offset,
        kRearMatrixSet0Offset + kWheelMatrixStride,
        kRearMatrixSet1Offset,
        kRearMatrixSet1Offset + kWheelMatrixStride};
    for (const std::size_t offset : offsets) {
        void* matrix = reinterpret_cast<void*>(self + offset);
        if (!WritableRange(matrix, kMatrixBytes)) return;
    }
    for (const std::size_t offset : offsets) {
        auto* matrix = reinterpret_cast<float*>(self + offset);
        const std::array<float, 7> affine = {
            matrix[3], matrix[7], matrix[11], matrix[12],
            matrix[13], matrix[14], matrix[15]};
        rotate(matrix, matrix, angle);
        matrix[3] = affine[0];
        matrix[7] = affine[1];
        matrix[11] = affine[2];
        matrix[12] = affine[3];
        matrix[13] = affine[4];
        matrix[14] = affine[5];
        matrix[15] = affine[6];
        g_rearVisualWrites.fetch_add(1, std::memory_order_relaxed);
    }
}

void NFSMW_FASTCALL FourWheelSolverDetour(void* self,
                                          void* /*unusedEdx*/,
                                          void* result) {
    const FourWheelResultFn original =
        g_fourWheelSolverOriginal.load(std::memory_order_acquire);
    RearSteeringSnapshot snapshot{};
    std::array<std::uintptr_t, 2> rearWheels{};
    std::array<std::array<float, 3>, 2> originalDirections{};
    std::array<bool, 2> changed{};
    const bool playerSolver = SnapshotRearSteering(&snapshot) &&
        reinterpret_cast<std::uintptr_t>(self) == snapshot.suspension - 0x4Cu;
    if (playerSolver) {
        for (std::size_t rear = 0; rear < 2; ++rear) {
            const std::size_t wheelIndex = rear + 2;
            const std::uintptr_t slot = snapshot.suspension +
                kSuspensionWheelTableOffset + wheelIndex * sizeof(void*);
            std::uintptr_t wheel = 0;
            std::array<float, 3> direction{};
            std::array<float, 3> normal{};
            std::array<float, 3> rotated{};
            if (SafeReadPointer(reinterpret_cast<const void*>(slot), &wheel) &&
                wheel != 0 &&
                ReadVec3(wheel + kWheelDirectionOffset, &direction) &&
                ReadVec3(wheel + kWheelContactNormalOffset, &normal) &&
                rear_wheel_steering::RotateDirectionAroundAxis(
                    direction, normal, snapshot.angleRad, rotated) &&
                WriteVec3(wheel + kWheelDirectionOffset, rotated)) {
                rearWheels[rear] = wheel;
                originalDirections[rear] = direction;
                changed[rear] = true;
                g_rearPhysicsWrites.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    original(self, result);
    for (std::size_t rear = 0; rear < changed.size(); ++rear) {
        if (!changed[rear]) continue;
        if (WriteVec3(rearWheels[rear] + kWheelDirectionOffset,
                      originalDirections[rear])) {
            g_rearPhysicsRestores.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_rearPhysicsRestoreFailures.fetch_add(1,
                                                    std::memory_order_relaxed);
            g_rearSteering.enabled.store(false, std::memory_order_release);
        }
    }
}
#endif

std::uint32_t VisualRelationMask(std::uintptr_t self,
                                 std::uintptr_t owner,
                                 std::uintptr_t field44,
                                 std::uintptr_t renderable,
                                 std::uintptr_t simable,
                                 std::uintptr_t rigidBody,
                                 std::uintptr_t suspension,
                                 std::uintptr_t transmission,
                                 std::uintptr_t visualCollection,
                                 std::uint32_t visualCollectionKey,
                                 std::uintptr_t pvehicleCollection,
                                 std::uint32_t collectionPointerDepth,
                                 std::uint32_t collectionKeyDepth) {
    const std::uintptr_t pvehicle =
        g_window.pvehicle.load(std::memory_order_relaxed);
    const std::uintptr_t player =
        g_window.player.load(std::memory_order_relaxed);
    const std::uintptr_t physicsComponent =
        suspension >= 0x4Cu ? suspension - 0x4Cu : 0;
    std::uint32_t mask = 0;
    if (pvehicle != 0) {
        if (self == pvehicle) mask |= kVisualRelationSelfPvehicle;
        if (owner == pvehicle) mask |= kVisualRelationOwnerPvehicle;
        if (field44 == pvehicle) mask |= kVisualRelationField44Pvehicle;
    }
    if (player != 0) {
        if (self == player) mask |= kVisualRelationSelfPlayer;
        if (owner == player) mask |= kVisualRelationOwnerPlayer;
        if (field44 == player) mask |= kVisualRelationField44Player;
    }
    if (physicsComponent != 0) {
        if (self == physicsComponent) {
            mask |= kVisualRelationSelfPhysicsComponent;
        }
        if (owner == physicsComponent) {
            mask |= kVisualRelationOwnerPhysicsComponent;
        }
        if (field44 == physicsComponent) {
            mask |= kVisualRelationField44PhysicsComponent;
        }
    }
    const auto addRelations = [self, owner, field44, &mask](
                                  std::uintptr_t target,
                                  std::uint32_t selfBit,
                                  std::uint32_t ownerBit,
                                  std::uint32_t field44Bit) {
        if (target == 0) return;
        if (self == target) mask |= selfBit;
        if (owner == target) mask |= ownerBit;
        if (field44 == target) mask |= field44Bit;
    };
    addRelations(renderable, kVisualRelationSelfRenderable,
                 kVisualRelationOwnerRenderable,
                 kVisualRelationField44Renderable);
    addRelations(simable, kVisualRelationSelfSimable,
                 kVisualRelationOwnerSimable,
                 kVisualRelationField44Simable);
    addRelations(rigidBody, kVisualRelationSelfRigidBody,
                 kVisualRelationOwnerRigidBody,
                 kVisualRelationField44RigidBody);
    addRelations(suspension, kVisualRelationSelfSuspension,
                 kVisualRelationOwnerSuspension,
                 kVisualRelationField44Suspension);
    addRelations(transmission, kVisualRelationSelfTransmission,
                 kVisualRelationOwnerTransmission,
                 kVisualRelationField44Transmission);
    if (visualCollection != 0 && visualCollection == pvehicleCollection &&
        collectionPointerDepth != kNoMatchedOffset) {
        mask |= kVisualRelationCollectionPointer;
    }
    if (visualCollectionKey != 0 &&
        collectionKeyDepth != kNoMatchedOffset) {
        mask |= kVisualRelationCollectionKey;
    }
    return mask;
}

void RecordVisualCandidate(std::uintptr_t self) {
    std::uintptr_t owner = 0;
    std::uintptr_t field44 = 0;
    (void)SafeReadPointer(reinterpret_cast<const void*>(
                              self + kVisualOwnerOffset),
                          &owner);
    (void)SafeReadPointer(reinterpret_cast<const void*>(
                              self + kVisualField44Offset),
                          &field44);
    const std::uintptr_t pvehicle =
        g_window.pvehicle.load(std::memory_order_relaxed);
    const std::uintptr_t renderable =
        ReadPointerField(pvehicle, kPVehicleRenderableOffset);
    const std::uintptr_t simable =
        ReadPointerField(pvehicle, kPVehicleSimableOffset);
    const std::uintptr_t rigidBody =
        ReadPointerField(pvehicle, kPVehicleRigidBodyOffset);
    const std::uintptr_t suspension =
        ReadPointerField(pvehicle, kPVehicleSuspensionOffset);
    const std::uintptr_t transmission =
        ReadPointerField(pvehicle, kPVehicleTransmissionOffset);
    const std::uintptr_t visualCollection =
        ReadPointerField(self, kVisualAttributeCollectionOffset);
    const std::uintptr_t pvehicleCollection =
        ReadPointerField(pvehicle, kPVehicleAttributeCollectionOffset);
    const std::uint32_t visualCollectionKey =
        ReadCollectionKey(visualCollection);
    const std::uint32_t pvehicleCollectionKey =
        ReadCollectionKey(pvehicleCollection);
    std::uint32_t collectionPointerDepth = kNoMatchedOffset;
    std::uint32_t collectionKeyDepth = kNoMatchedOffset;
    MatchCollectionChain(visualCollection, visualCollectionKey,
                         pvehicleCollection, &collectionPointerDepth,
                         &collectionKeyDepth);
    for (VisualTraceSlot& slot : g_visualTrace) {
        std::uintptr_t known = slot.self.load(std::memory_order_relaxed);
        if (known == 0 &&
            slot.self.compare_exchange_strong(
                known, self, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            known = self;
        }
        if (known == self) {
            slot.owner.store(owner, std::memory_order_relaxed);
            slot.field44.store(field44, std::memory_order_relaxed);
            slot.renderable.store(renderable, std::memory_order_relaxed);
            slot.visualCollection.store(visualCollection,
                                        std::memory_order_relaxed);
            slot.pvehicleCollection.store(pvehicleCollection,
                                          std::memory_order_relaxed);
            slot.visualCollectionKey.store(visualCollectionKey,
                                           std::memory_order_relaxed);
            slot.pvehicleCollectionKey.store(pvehicleCollectionKey,
                                             std::memory_order_relaxed);
            slot.collectionPointerDepth.store(collectionPointerDepth,
                                              std::memory_order_relaxed);
            slot.collectionKeyDepth.store(collectionKeyDepth,
                                          std::memory_order_relaxed);
            slot.pvehicleSelfOffset.store(
                FirstAlignedReferenceOffset(pvehicle, kPVehicleReadableSize,
                                            self),
                std::memory_order_relaxed);
            slot.pvehicleOwnerOffset.store(
                FirstAlignedReferenceOffset(pvehicle, kPVehicleReadableSize,
                                            owner),
                std::memory_order_relaxed);
            slot.pvehicleField44Offset.store(
                FirstAlignedReferenceOffset(pvehicle, kPVehicleReadableSize,
                                            field44),
                std::memory_order_relaxed);
            slot.visualPvehicleOffset.store(
                FirstAlignedReferenceOffset(self, kVisualReadableSize,
                                            pvehicle),
                std::memory_order_relaxed);
            slot.visualRenderableOffset.store(
                FirstAlignedReferenceOffset(self, kVisualReadableSize,
                                            renderable),
                std::memory_order_relaxed);
            slot.relationMask.fetch_or(
                VisualRelationMask(
                    self, owner, field44, renderable, simable, rigidBody,
                    suspension, transmission, visualCollection,
                    visualCollectionKey, pvehicleCollection,
                    collectionPointerDepth, collectionKeyDepth),
                std::memory_order_relaxed);
            slot.calls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void NFSMW_FASTCALL WheelVisualDetour(void* self,
                                      void* /*unusedEdx*/,
                                      void* argument0,
                                      void* argument1,
                                      void* argument2) {
    const WheelVisualFn original =
        g_wheelVisualOriginal.load(std::memory_order_acquire);
    original(self, argument0, argument1, argument2);
    if (g_wheelVisualArmed.load(std::memory_order_acquire) && self != nullptr) {
#if NFSMW_ENABLE_REAR_WHEEL_STEERING
        ApplyRearSteeringVisual(reinterpret_cast<std::uintptr_t>(self));
#endif
#if NFSMW_ENABLE_RWS_FORCE_TRACE
        RecordVisualCandidate(reinterpret_cast<std::uintptr_t>(self));
#endif
    }
}

#if NFSMW_ENABLE_RWS_FORCE_TRACE
bool ReadFourSteeringSlots(std::uintptr_t result,
                           std::array<float, 4>* angles) {
    if (result == 0 || angles == nullptr ||
        !ReadableRange(reinterpret_cast<const void*>(result),
                       kResultFirstSteeringOffset + 4 * sizeof(float))) {
        return false;
    }
    for (std::size_t index = 0; index < angles->size(); ++index) {
        if (!SafeReadFiniteFloat(
                reinterpret_cast<const void*>(
                    result + kResultFirstSteeringOffset +
                    index * sizeof(float)),
                &(*angles)[index])) {
            return false;
        }
    }
    return true;
}

void NFSMW_FASTCALL FourWheelResultDetour(void* self,
                                          void* /*unusedEdx*/,
                                          void* result) {
    const FourWheelResultFn original =
        g_fourWheelResultOriginal.load(std::memory_order_acquire);
    const std::uintptr_t component = reinterpret_cast<std::uintptr_t>(self);
    const std::uintptr_t output = reinterpret_cast<std::uintptr_t>(result);
    g_fourWheelRawCalls.fetch_add(1, std::memory_order_relaxed);
    g_fourWheelLastSelf.store(component, std::memory_order_relaxed);
    CallContext context{};
    std::uintptr_t suspension = 0;
    const bool candidate =
        g_fourWheelResultArmed.load(std::memory_order_acquire) &&
        g_window.open.load(std::memory_order_acquire) &&
        g_window.wheelsValid.load(std::memory_order_relaxed) &&
        g_window.physicsThread.load(std::memory_order_relaxed) ==
            GetCurrentThreadId() &&
        ReadOwnedPointer(component, kResultSuspensionOffset, &suspension) &&
        suspension != 0 &&
        suspension == g_window.suspension.load(std::memory_order_relaxed);
    g_fourWheelLastSuspension.store(suspension, std::memory_order_relaxed);
    std::array<float, 4> before{};
    const bool beforeValid =
        candidate && ReadFourSteeringSlots(output, &before);
    if (candidate) {
        context.generation =
            g_window.generation.load(std::memory_order_relaxed);
        context.physicsSerial =
            g_window.physicsSerial.load(std::memory_order_relaxed);
    }
    original(self, result);
    if (!candidate || !ContextStillOpen(context) ||
        g_stats.generation != context.generation) {
        return;
    }
    ++g_stats.fourWheelResultCalls;
    std::array<float, 4> after{};
    const bool afterValid = ReadFourSteeringSlots(output, &after);
    if (!afterValid) {
        ++g_stats.fourWheelResultInvalid;
        return;
    }
    g_stats.resultBefore = before;
    g_stats.resultAfter = after;
    g_stats.resultBeforeValid = beforeValid;
    g_stats.resultValid = true;
}
#endif
#endif

bool ValidObjectVtable(std::uintptr_t object,
                       std::size_t requiredSlot,
                       std::uintptr_t requiredTarget,
                       bool requireExactTarget) {
    std::uintptr_t vtable = 0;
    if (object == 0 ||
        !SafeReadPointer(reinterpret_cast<const void*>(object), &vtable) ||
        vtable == 0 ||
        !ReadOnlyImageRange(
            reinterpret_cast<const void*>(vtable),
            (requiredSlot + 1) * sizeof(std::uintptr_t))) {
        return false;
    }
    std::uintptr_t slotAddress = 0;
    std::uintptr_t slotTarget = 0;
    return AddAddress(vtable, requiredSlot * sizeof(std::uintptr_t),
                      &slotAddress) &&
           SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                           &slotTarget) &&
           ExecutableImageAddress(reinterpret_cast<const void*>(slotTarget)) &&
           (!requireExactTarget || slotTarget == requiredTarget);
}

bool ReadOwnedPointer(std::uintptr_t owner,
                      std::size_t offset,
                      std::uintptr_t* value) {
    std::uintptr_t address = 0;
    return AddAddress(owner, offset, &address) &&
           SafeReadPointer(reinterpret_cast<const void*>(address), value);
}

bool ReadWheelFieldsValidated(std::uintptr_t wheel, WheelFields* fields) {
    if (fields == nullptr || wheel == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(wheel),
                       kWheelReadableSize)) {
        return false;
    }
    return SafeReadFiniteFloat(
               reinterpret_cast<const void*>(
                   wheel + kWheelNormalLoadOffset),
               &fields->normalLoad) &&
           SafeReadFiniteFloat(
               reinterpret_cast<const void*>(
                   wheel + kWheelLateralForceOffset),
               &fields->lateralForce) &&
           SafeReadFiniteFloat(
               reinterpret_cast<const void*>(
                   wheel + kWheelLongitudinalForceOffset),
               &fields->longitudinalForce) &&
           SafeReadFiniteFloat(
               reinterpret_cast<const void*>(
                   wheel + kWheelTractionStateOffset),
               &fields->tractionState);
}

bool ReadWheelFieldsNoQuery(std::uintptr_t wheel, WheelFields* fields) {
    if (fields == nullptr || wheel == 0) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
#endif
        fields->normalLoad = *reinterpret_cast<const float*>(
            wheel + kWheelNormalLoadOffset);
        fields->lateralForce = *reinterpret_cast<const float*>(
            wheel + kWheelLateralForceOffset);
        fields->longitudinalForce = *reinterpret_cast<const float*>(
            wheel + kWheelLongitudinalForceOffset);
        fields->tractionState = *reinterpret_cast<const float*>(
            wheel + kWheelTractionStateOffset);
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
    return std::isfinite(fields->normalLoad) &&
           std::isfinite(fields->lateralForce) &&
           std::isfinite(fields->longitudinalForce) &&
           std::isfinite(fields->tractionState) &&
           std::fabs(fields->normalLoad) <= kMaximumObservedMagnitude &&
           std::fabs(fields->lateralForce) <= kMaximumObservedMagnitude &&
           std::fabs(fields->longitudinalForce) <=
               kMaximumObservedMagnitude &&
           std::fabs(fields->tractionState) <= kMaximumObservedMagnitude;
}

bool ReadFloatNoQuery(std::uintptr_t address, float* value) {
    if (value == nullptr || address == 0) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        *value = *reinterpret_cast<const float*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *value = *reinterpret_cast<const float*>(address);
#endif
    return std::isfinite(*value) &&
           std::fabs(*value) <= kMaximumObservedMagnitude;
}

bool ResolveOwnership(const PlayerFrame& frame,
                      handling_probe_state::VehicleKey* key,
                      bool* wheelsValid,
                      bool* transmissionValid) {
    if (key == nullptr || wheelsValid == nullptr ||
        transmissionValid == nullptr || frame.pvehicle == 0 ||
        frame.player == 0 || frame.rigidBody == 0 ||
        frame.rigidBodyHolder == 0 || frame.rigidBodyInner == 0 ||
        frame.physicsSerial == 0 || frame.physicsThread == 0 ||
        frame.groundedWheels > handling_probe_state::kWheelCount) {
        return false;
    }
    *key = handling_probe_state::VehicleKey{};
    *wheelsValid = false;
    *transmissionValid = false;

    std::uintptr_t currentPlayer = 0;
    std::uintptr_t currentRigidBody = 0;
    std::uintptr_t currentHolder = 0;
    std::uintptr_t currentInner = 0;
    if (!ReadOwnedPointer(frame.pvehicle, kPVehiclePlayerOffset,
                          &currentPlayer) ||
        !ReadOwnedPointer(frame.pvehicle, kPVehicleRigidBodyOffset,
                          &currentRigidBody) ||
        !ReadOwnedPointer(frame.rigidBody, kRigidBodyHolderOffset,
                          &currentHolder) ||
        currentHolder == 0 ||
        !SafeReadPointer(reinterpret_cast<const void*>(currentHolder),
                         &currentInner) ||
        currentPlayer != frame.player ||
        currentRigidBody != frame.rigidBody ||
        currentHolder != frame.rigidBodyHolder ||
        currentInner != frame.rigidBodyInner) {
        return false;
    }
    key->pvehicle = frame.pvehicle;
    key->player = frame.player;
    key->rigidBody = frame.rigidBody;
    key->rigidBodyHolder = frame.rigidBodyHolder;
    key->rigidBodyInner = frame.rigidBodyInner;

    std::uintptr_t suspension = 0;
    if (g_tireArmed.load(std::memory_order_acquire) &&
        ReadOwnedPointer(frame.pvehicle, kPVehicleSuspensionOffset,
                         &suspension) &&
        suspension != 0 &&
        ValidObjectVtable(suspension, 0, 0, false)) {
        bool allWheelsValid = true;
        std::array<std::uintptr_t,
                   handling_probe_state::kWheelCount>
            wheels{};
        for (std::size_t index = 0;
             index < handling_probe_state::kWheelCount; ++index) {
            std::uintptr_t wheelAddress = 0;
            WheelFields initialFields{};
            if (!AddAddress(suspension,
                            kSuspensionWheelTableOffset +
                                index * sizeof(std::uintptr_t),
                            &wheelAddress) ||
                !SafeReadPointer(
                    reinterpret_cast<const void*>(wheelAddress),
                    &wheels[index]) ||
                wheels[index] == 0 ||
                !ReadWheelFieldsValidated(wheels[index], &initialFields)) {
                allWheelsValid = false;
                break;
            }
        }
        if (allWheelsValid &&
            handling_probe_state::HasDistinctCompleteWheelSet(wheels)) {
            key->suspension = suspension;
            key->wheels = wheels;
            *wheelsValid = true;
        }
    }

    std::uintptr_t transmission = 0;
    const std::uintptr_t driveTarget =
        g_driveTorqueTarget.load(std::memory_order_acquire);
    if (g_driveTorqueProfileValid.load(std::memory_order_acquire) &&
        ReadOwnedPointer(frame.pvehicle, kPVehicleTransmissionOffset,
                         &transmission) &&
        transmission != 0 && driveTarget != 0 &&
        ValidObjectVtable(transmission, kDriveTorqueVtableSlot,
                          driveTarget, true)) {
        key->transmission = transmission;
        *transmissionValid = true;
    }
    return *wheelsValid || *transmissionValid;
}

void CloseWindow() {
    g_window.open.store(false, std::memory_order_release);
}

bool SnapshotWheelContext(std::uintptr_t wheel,
                          CallContext* context) {
    if (context == nullptr ||
        !g_tireArmed.load(std::memory_order_acquire) ||
        !g_window.open.load(std::memory_order_acquire) ||
        !g_window.wheelsValid.load(std::memory_order_relaxed) ||
        g_window.physicsThread.load(std::memory_order_relaxed) !=
            GetCurrentThreadId()) {
        return false;
    }
    for (std::size_t index = 0;
         index < handling_probe_state::kWheelCount; ++index) {
        if (g_window.wheels[index].load(std::memory_order_relaxed) != wheel) {
            continue;
        }
        context->generation =
            g_window.generation.load(std::memory_order_relaxed);
        context->groundedWheels =
            g_window.groundedWheels.load(std::memory_order_relaxed);
        context->physicsSerial =
            g_window.physicsSerial.load(std::memory_order_relaxed);
        context->driftActive =
            g_window.driftActive.load(std::memory_order_relaxed);
        context->wheelIndex = index;
        return context->generation != 0 && context->physicsSerial != 0;
    }
    return false;
}

bool SnapshotTorqueContext(std::uintptr_t transmission,
                           CallContext* context) {
    if (context == nullptr ||
        !g_driveTorqueArmed.load(std::memory_order_acquire) ||
        !g_window.open.load(std::memory_order_acquire) ||
        !g_window.transmissionValid.load(std::memory_order_relaxed) ||
        g_window.physicsThread.load(std::memory_order_relaxed) !=
            GetCurrentThreadId() ||
        g_window.transmission.load(std::memory_order_relaxed) !=
            transmission) {
        return false;
    }
    context->generation =
        g_window.generation.load(std::memory_order_relaxed);
    context->groundedWheels =
        g_window.groundedWheels.load(std::memory_order_relaxed);
    context->physicsSerial =
        g_window.physicsSerial.load(std::memory_order_relaxed);
    context->driftActive =
        g_window.driftActive.load(std::memory_order_relaxed);
    return context->generation != 0 && context->physicsSerial != 0;
}

bool ContextStillOpen(const CallContext& context) {
    return g_window.open.load(std::memory_order_acquire) &&
           g_window.generation.load(std::memory_order_relaxed) ==
               context.generation &&
           g_window.physicsSerial.load(std::memory_order_relaxed) ==
               context.physicsSerial &&
           g_window.physicsThread.load(std::memory_order_relaxed) ==
               GetCurrentThreadId();
}

void RecordWheel(const CallContext& context,
                 const WheelFields& before,
                 const WheelFields& after,
                 const std::array<float, 5>& arguments,
                 float returnValue,
                 bool valid) {
    if (context.wheelIndex >= g_stats.wheels.size() ||
        g_stats.generation != context.generation) {
        return;
    }
    WheelStats& stats = g_stats.wheels[context.wheelIndex];
    ++stats.calls;
    if (context.driftActive) {
        ++stats.driftCalls;
    }
    if (!valid) {
        ++stats.invalidSamples;
        return;
    }
    stats.before = before;
    stats.after = after;
    stats.arguments = arguments;
    stats.returnValue = returnValue;
    stats.hasValue = true;
}

void RecordTorque(const CallContext& context,
                  float fieldValue,
                  float returnValue,
                  bool valid) {
    if (g_stats.generation != context.generation) {
        return;
    }
    TorqueStats& stats = g_stats.torque;
    ++stats.calls;
    if (context.driftActive) {
        ++stats.driftCalls;
    }
    if (!valid) {
        ++stats.invalidSamples;
        return;
    }
    const handling_probe_state::TorqueSign sign =
        handling_probe_state::ClassifyTorque(returnValue);
    switch (sign) {
        case handling_probe_state::TorqueSign::Positive:
            ++stats.positive;
            if (context.driftActive) ++stats.driftPositive;
            break;
        case handling_probe_state::TorqueSign::Zero:
            ++stats.zero;
            if (context.driftActive) ++stats.driftZero;
            break;
        case handling_probe_state::TorqueSign::Negative:
            ++stats.negative;
            if (context.driftActive) ++stats.driftNegative;
            break;
    }
    stats.fieldValue = fieldValue;
    stats.returnValue = returnValue;
    stats.hasValue = true;
}

float NFSMW_FASTCALL TireForceDetour(
    void* self, void* /*unusedEdx*/, float argument0, float argument1,
    float argument2, float argument3, float argument4) {
    const TireForceFn original =
        g_tireOriginal.load(std::memory_order_acquire);
    CallContext context{};
    WheelFields before{};
    const std::uintptr_t wheel = reinterpret_cast<std::uintptr_t>(self);
    g_tireRawCalls.fetch_add(1, std::memory_order_relaxed);
    g_tireLastSelf.store(wheel, std::memory_order_relaxed);
    const bool selected = SnapshotWheelContext(wheel, &context);
    const bool beforeValid =
        selected && ReadWheelFieldsNoQuery(wheel, &before);

    const float result = original(
        self, argument0, argument1, argument2, argument3, argument4);

    if (selected && ContextStillOpen(context)) {
        WheelFields after{};
        const bool afterValid = ReadWheelFieldsNoQuery(wheel, &after);
        const bool resultValid =
            std::isfinite(result) &&
            std::fabs(result) <= kMaximumObservedMagnitude;
        const std::array<float, 5> arguments = {
            argument0, argument1, argument2, argument3, argument4};
        bool argumentsValid = true;
        for (const float argument : arguments) {
            argumentsValid = argumentsValid && std::isfinite(argument) &&
                             std::fabs(argument) <=
                                 kMaximumObservedMagnitude;
        }
        RecordWheel(context, before, after, arguments, result,
                    beforeValid && afterValid && resultValid &&
                        argumentsValid);
    }
    return result;
}

float NFSMW_FASTCALL DriveTorqueDetour(void* self,
                                       void* /*unusedEdx*/) {
    const DriveTorqueFn original =
        g_driveTorqueOriginal.load(std::memory_order_acquire);
    CallContext context{};
    const std::uintptr_t transmission =
        reinterpret_cast<std::uintptr_t>(self);
    const bool selected = SnapshotTorqueContext(transmission, &context);
    float fieldValue = 0.0f;
    const bool fieldValid =
        selected && ReadFloatNoQuery(transmission + 0x34, &fieldValue);

    const float result = original(self);

    if (selected && ContextStillOpen(context)) {
        const bool resultValid =
            std::isfinite(result) &&
            std::fabs(result) <= kMaximumObservedMagnitude;
        RecordTorque(context, fieldValue, result,
                     fieldValid && resultValid);
    }
    return result;
}

template <typename Fn>
bool InstallChannel(std::uintptr_t target,
                    void* detour,
                    std::atomic<Fn>& originalStorage,
                    std::atomic<bool>& armedStorage,
                    const char* label,
                    std::atomic<int>* createStatusStorage = nullptr,
                    std::atomic<int>* enableStatusStorage = nullptr,
                    std::atomic<std::uint32_t>* installStageStorage = nullptr) {
    void* originalRaw = nullptr;
    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<void*>(target), detour, &originalRaw);
    if (createStatusStorage != nullptr) {
        createStatusStorage->store(static_cast<int>(create),
                                   std::memory_order_relaxed);
    }
    if (create != MH_OK || originalRaw == nullptr) {
        if (installStageStorage != nullptr) {
            installStageStorage->store(10, std::memory_order_relaxed);
        }
        char message[320]{};
        std::snprintf(message, sizeof(message),
                      "handling probe channel=%s disabled: hook creation failed (%s)",
                      label, MH_StatusToString(create));
        Log(message);
        return false;
    }

    // Publish the trampoline before enabling. If enable partially fails, the
    // retained detour remains a permanent pass-through with armed=false.
    originalStorage.store(reinterpret_cast<Fn>(originalRaw),
                          std::memory_order_release);
    armedStorage.store(false, std::memory_order_release);
    const MH_STATUS enable =
        MH_EnableHook(reinterpret_cast<void*>(target));
    if (enableStatusStorage != nullptr) {
        enableStatusStorage->store(static_cast<int>(enable),
                                   std::memory_order_relaxed);
    }
    if (enable != MH_OK) {
        if (installStageStorage != nullptr) {
            installStageStorage->store(11, std::memory_order_relaxed);
        }
        char message[384]{};
        std::snprintf(
            message, sizeof(message),
            "handling probe channel=%s disabled: hook enable failed (%s); retained pass-through trampoline",
            label, MH_StatusToString(enable));
        Log(message);
        return false;
    }
    armedStorage.store(true, std::memory_order_release);
    if (installStageStorage != nullptr) {
        installStageStorage->store(12, std::memory_order_relaxed);
    }
    char message[240]{};
    std::snprintf(message, sizeof(message),
                  "handling probe channel=%s armed target=%p read_only=1",
                  label, reinterpret_cast<void*>(target));
    Log(message);
    return true;
}

void PublishWindow(const PlayerFrame& frame,
                   const handling_probe_state::VehicleKey& key,
                   std::uint32_t generation,
                   bool wheelsValid,
                   bool transmissionValid) {
    CloseWindow();
    g_window.wheelsValid.store(wheelsValid, std::memory_order_relaxed);
    g_window.transmissionValid.store(transmissionValid,
                                     std::memory_order_relaxed);
    g_window.physicsThread.store(frame.physicsThread,
                                 std::memory_order_relaxed);
    g_window.generation.store(generation, std::memory_order_relaxed);
    g_window.groundedWheels.store(frame.groundedWheels,
                                  std::memory_order_relaxed);
    g_window.driftActive.store(frame.driftActive,
                               std::memory_order_relaxed);
    g_window.physicsSerial.store(frame.physicsSerial,
                                 std::memory_order_relaxed);
    g_window.pvehicle.store(key.pvehicle, std::memory_order_relaxed);
    g_window.player.store(key.player, std::memory_order_relaxed);
    g_window.suspension.store(key.suspension,
                              std::memory_order_relaxed);
    g_window.transmission.store(key.transmission,
                                std::memory_order_relaxed);
    for (std::size_t index = 0;
         index < handling_probe_state::kWheelCount; ++index) {
        g_window.wheels[index].store(key.wheels[index],
                                     std::memory_order_relaxed);
    }
    g_window.open.store(true, std::memory_order_release);
}

void LogAggregate() {
    const DWORD now = GetTickCount();
    DWORD previous = g_lastLogTick.load(std::memory_order_relaxed);
    if (previous != 0 &&
        static_cast<DWORD>(now - previous) < kLogIntervalMs) {
        return;
    }
    if (!g_lastLogTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }

    char summary[640]{};
    std::snprintf(
        summary, sizeof(summary),
        "handling_probe generation=%lu serial=%llu thread=%lu drift=%d grounded=%lu sampled_frames=%llu player=%p pvehicle=%p suspension=%p transmission=%p wheel_channel=%d torque_channel=%d",
        static_cast<unsigned long>(g_stats.generation),
        static_cast<unsigned long long>(g_lastFrame.physicsSerial),
        static_cast<unsigned long>(g_lastFrame.physicsThread),
        g_lastFrame.driftActive ? 1 : 0,
        static_cast<unsigned long>(g_lastFrame.groundedWheels),
        static_cast<unsigned long long>(g_stats.sampledFrames),
        reinterpret_cast<void*>(g_lastKey.player),
        reinterpret_cast<void*>(g_lastKey.pvehicle),
        reinterpret_cast<void*>(g_lastKey.suspension),
        reinterpret_cast<void*>(g_lastKey.transmission),
        g_tireArmed.load(std::memory_order_relaxed) ? 1 : 0,
        g_driveTorqueArmed.load(std::memory_order_relaxed) ? 1 : 0);
    Log(summary);
#if NFSMW_ENABLE_RWS_FORCE_TRACE
    char traceHeader[700]{};
    std::snprintf(traceHeader, sizeof(traceHeader),
                  "TRACE_ONLY_V3 serial=%llu pvehicle=%p key=%08lX speed_kmh=%.2f steer=%.4f driver_assist=%d drift=%d grounded=%lu tire_raw_calls=%llu tire_last_self=%p; rear_force_writes=0 visual_writes=0",
                  static_cast<unsigned long long>(g_lastFrame.physicsSerial),
                  reinterpret_cast<void*>(g_lastFrame.pvehicle),
                  static_cast<unsigned long>(g_lastFrame.collectionKey),
                  static_cast<double>(g_lastFrame.speedMps * 3.6f),
                  static_cast<double>(g_lastFrame.steeringCommand),
                  g_lastFrame.driverAssistActive ? 1 : 0,
                  g_lastFrame.driftActive ? 1 : 0,
                  static_cast<unsigned long>(g_lastFrame.groundedWheels),
                  static_cast<unsigned long long>(
                      g_tireRawCalls.load(std::memory_order_relaxed)),
                  reinterpret_cast<void*>(
                      g_tireLastSelf.load(std::memory_order_relaxed)));
    ForceTraceFile(traceHeader);
    char resultLine[620]{};
    std::snprintf(resultLine, sizeof(resultLine),
                  "four_wheel_result raw_calls=%llu last_self=%p last_suspension_at_88=%p player_suspension=%p selected_calls=%llu invalid=%llu before_valid=%d after_valid=%d before=(%.5f %.5f %.5f %.5f) after=(%.5f %.5f %.5f %.5f) CANDIDATE_ONLY_NOT_PROVEN_RENDER_SINK",
                  static_cast<unsigned long long>(
                      g_fourWheelRawCalls.load(std::memory_order_relaxed)),
                  reinterpret_cast<void*>(
                      g_fourWheelLastSelf.load(std::memory_order_relaxed)),
                  reinterpret_cast<void*>(
                      g_fourWheelLastSuspension.load(
                          std::memory_order_relaxed)),
                  reinterpret_cast<void*>(g_lastKey.suspension),
                  static_cast<unsigned long long>(g_stats.fourWheelResultCalls),
                  static_cast<unsigned long long>(g_stats.fourWheelResultInvalid),
                  g_stats.resultBeforeValid ? 1 : 0,
                  g_stats.resultValid ? 1 : 0,
                  static_cast<double>(g_stats.resultBefore[0]),
                  static_cast<double>(g_stats.resultBefore[1]),
                  static_cast<double>(g_stats.resultBefore[2]),
                  static_cast<double>(g_stats.resultBefore[3]),
                  static_cast<double>(g_stats.resultAfter[0]),
                  static_cast<double>(g_stats.resultAfter[1]),
                  static_cast<double>(g_stats.resultAfter[2]),
                  static_cast<double>(g_stats.resultAfter[3]));
    ForceTraceFile(resultLine);
    for (std::size_t index = 0; index < g_visualTrace.size(); ++index) {
        const VisualTraceSlot& slot = g_visualTrace[index];
        const std::uintptr_t visualSelf =
            slot.self.load(std::memory_order_relaxed);
        if (visualSelf == 0) continue;
        char visualLine[960]{};
        std::snprintf(
            visualLine, sizeof(visualLine),
            "visual_candidate_v3 slot=%llu self=%p owner_at_08=%p field_at_44=%p renderable_at_pv108=%p calls=%llu relation_mask=%08lX visual_collection=%p:%08lX pvehicle_collection=%p:%08lX collection_match_depth=(ptr:%lu key:%lu) pvehicle_ref_offsets=(self:%08lX owner:%08lX field44:%08lX) visual_ref_offsets=(pvehicle:%08lX renderable:%08lX) TRACE_ONLY_NO_MATRIX_WRITES",
            static_cast<unsigned long long>(index),
            reinterpret_cast<void*>(visualSelf),
            reinterpret_cast<void*>(
                slot.owner.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(
                slot.field44.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(
                slot.renderable.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                slot.calls.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(
                slot.relationMask.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(
                slot.visualCollection.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(
                slot.visualCollectionKey.load(std::memory_order_relaxed)),
            reinterpret_cast<void*>(
                slot.pvehicleCollection.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(
                slot.pvehicleCollectionKey.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(
                slot.collectionPointerDepth.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(
                slot.collectionKeyDepth.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(
                slot.pvehicleSelfOffset.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(
                slot.pvehicleOwnerOffset.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(
                slot.pvehicleField44Offset.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(
                slot.visualPvehicleOffset.load(std::memory_order_relaxed)),
            static_cast<unsigned long>(
                slot.visualRenderableOffset.load(std::memory_order_relaxed)));
        ForceTraceFile(visualLine);
    }
#endif

    for (std::size_t index = 0; index < g_stats.wheels.size(); ++index) {
        const WheelStats& wheel = g_stats.wheels[index];
        char message[900]{};
        std::snprintf(
            message, sizeof(message),
            "handling_probe wheel=%llu axle=%s object=%p calls=%llu drift_calls=%llu invalid=%llu D4=%.5f D8_before=%.5f D8_after=%.5f DC_before=%.5f DC_after=%.5f traction110=%.5f return=%.5f args=(%.5f %.5f %.5f %.5f %.5f) value_ok=%d",
            static_cast<unsigned long long>(index),
            handling_probe_state::IsFrontWheel(index) ? "front" : "rear",
            reinterpret_cast<void*>(g_lastKey.wheels[index]),
            static_cast<unsigned long long>(wheel.calls),
            static_cast<unsigned long long>(wheel.driftCalls),
            static_cast<unsigned long long>(wheel.invalidSamples),
            static_cast<double>(wheel.after.normalLoad),
            static_cast<double>(wheel.before.lateralForce),
            static_cast<double>(wheel.after.lateralForce),
            static_cast<double>(wheel.before.longitudinalForce),
            static_cast<double>(wheel.after.longitudinalForce),
            static_cast<double>(wheel.after.tractionState),
            static_cast<double>(wheel.returnValue),
            static_cast<double>(wheel.arguments[0]),
            static_cast<double>(wheel.arguments[1]),
            static_cast<double>(wheel.arguments[2]),
            static_cast<double>(wheel.arguments[3]),
            static_cast<double>(wheel.arguments[4]),
            wheel.hasValue ? 1 : 0);
        Log(message);
#if NFSMW_ENABLE_RWS_FORCE_TRACE
        char traceLine[760]{};
        std::snprintf(traceLine, sizeof(traceLine),
                      "wheel=%llu calls=%llu valid=%d args=(%.5f %.5f %.5f %.5f %.5f) return=%.5f normal=%.5f lateral_before_after=(%.5f %.5f) longitudinal_before_after=(%.5f %.5f) world_force=(%.5f %.5f %.5f) world_force_valid=%d",
                      static_cast<unsigned long long>(index),
                      static_cast<unsigned long long>(wheel.calls),
                      wheel.hasValue ? 1 : 0,
                      static_cast<double>(wheel.arguments[0]),
                      static_cast<double>(wheel.arguments[1]),
                      static_cast<double>(wheel.arguments[2]),
                      static_cast<double>(wheel.arguments[3]),
                      static_cast<double>(wheel.arguments[4]),
                      static_cast<double>(wheel.returnValue),
                      static_cast<double>(wheel.after.normalLoad),
                      static_cast<double>(wheel.before.lateralForce),
                      static_cast<double>(wheel.after.lateralForce),
                      static_cast<double>(wheel.before.longitudinalForce),
                      static_cast<double>(wheel.after.longitudinalForce),
                      static_cast<double>(wheel.worldForce[0]),
                      static_cast<double>(wheel.worldForce[1]),
                      static_cast<double>(wheel.worldForce[2]),
                      wheel.worldForceValid ? 1 : 0);
        ForceTraceFile(traceLine);
#endif
    }

    const TorqueStats& torque = g_stats.torque;
    char torqueMessage[720]{};
    std::snprintf(
        torqueMessage, sizeof(torqueMessage),
        "handling_probe torque object=%p calls=%llu drift_calls=%llu sign_pos_zero_neg=%llu/%llu/%llu drift_pos_zero_neg=%llu/%llu/%llu invalid=%llu field34=%.5f return=%.5f value_ok=%d",
        reinterpret_cast<void*>(g_lastKey.transmission),
        static_cast<unsigned long long>(torque.calls),
        static_cast<unsigned long long>(torque.driftCalls),
        static_cast<unsigned long long>(torque.positive),
        static_cast<unsigned long long>(torque.zero),
        static_cast<unsigned long long>(torque.negative),
        static_cast<unsigned long long>(torque.driftPositive),
        static_cast<unsigned long long>(torque.driftZero),
        static_cast<unsigned long long>(torque.driftNegative),
        static_cast<unsigned long long>(torque.invalidSamples),
        static_cast<double>(torque.fieldValue),
        static_cast<double>(torque.returnValue), torque.hasValue ? 1 : 0);
    Log(torqueMessage);
}

#endif  // NFSMW_ENABLE_ALPHA_BRIDGE

}  // namespace

bool Install(std::uintptr_t imageBase,
             std::size_t imageSize,
             std::uintptr_t textStart,
             std::size_t textSize) {
#if !NFSMW_ENABLE_ALPHA_BRIDGE
    (void)imageBase;
    (void)imageSize;
    (void)textStart;
    (void)textSize;
    return false;
#else
    if (g_installAttempted.exchange(true, std::memory_order_acq_rel)) {
        return g_tireArmed.load(std::memory_order_acquire) ||
#if NFSMW_ENABLE_RWS_FORCE_TRACE || NFSMW_ENABLE_REAR_WHEEL_STEERING
               g_wheelVisualArmed.load(std::memory_order_acquire) ||
#endif
               g_driveTorqueArmed.load(std::memory_order_acquire);
    }
    if (imageBase == 0 || imageSize == 0 ||
        imageBase != target_profile::kExpectedImageBase) {
        Log("handling probe disabled: main-image profile is unavailable");
        return false;
    }
    const handling_probe_state::TextRangeFailure textFailure =
        handling_probe_state::ValidateTextRange(
            imageBase, imageSize, textStart, textSize);
    if (textFailure != handling_probe_state::TextRangeFailure::None) {
        char message[512]{};
        std::snprintf(
            message, sizeof(message),
            "handling probe validation channel=all result=failed stage=text-contract reason=%s image_base=%p image_size=0x%llX text_start=%p text_size=0x%llX",
            TextRangeFailureReason(textFailure),
            reinterpret_cast<void*>(imageBase),
            static_cast<unsigned long long>(imageSize),
            reinterpret_cast<void*>(textStart),
            static_cast<unsigned long long>(textSize));
        Log(message);
        Log("handling probe disabled: common text-range contract is unavailable");
        return false;
    }
    g_imageBase.store(imageBase, std::memory_order_release);
    g_imageSize.store(imageSize, std::memory_order_release);
    g_textStart.store(textStart, std::memory_order_release);
    g_textSize.store(textSize, std::memory_order_release);

    const MH_STATUS initialize = MH_Initialize();
    if (initialize != MH_OK &&
        initialize != MH_ERROR_ALREADY_INITIALIZED) {
        char message[256]{};
        std::snprintf(message, sizeof(message),
                      "handling probe disabled: MinHook initialization failed (%s)",
                      MH_StatusToString(initialize));
        Log(message);
        return false;
    }

    std::uintptr_t tireTarget = 0;
    if (ValidateTireTarget(&tireTarget)) {
        g_tireTarget.store(tireTarget, std::memory_order_release);
        InstallChannel(tireTarget,
                       reinterpret_cast<void*>(&TireForceDetour),
                       g_tireOriginal, g_tireArmed, "tire-force");
    } else {
        Log("handling probe channel=tire-force disabled: strict validation failed; see preceding validation diagnostics");
    }
#if NFSMW_ENABLE_RWS_FORCE_TRACE
    std::uintptr_t resultTarget = 0;
    if (g_tireArmed.load(std::memory_order_acquire) &&
        ValidateFourWheelResultTarget(&resultTarget)) {
        InstallChannel(resultTarget,
                       reinterpret_cast<void*>(&FourWheelResultDetour),
                       g_fourWheelResultOriginal,
                       g_fourWheelResultArmed, "four-wheel-result-trace");
    } else {
        Log("RWS trace result channel disabled; tire force remains read-only");
    }
#endif
#if NFSMW_ENABLE_RWS_FORCE_TRACE || NFSMW_ENABLE_REAR_WHEEL_STEERING
    std::uintptr_t visualTarget = 0;
#if NFSMW_ENABLE_REAR_WHEEL_STEERING
    MatrixYawFn matrixYaw = nullptr;
#endif
    if (ValidateWheelVisualTarget(&visualTarget)) {
#if NFSMW_ENABLE_REAR_WHEEL_STEERING
        if (ValidateMatrixYawTarget(&matrixYaw)) {
#endif
        InstallChannel(visualTarget,
                       reinterpret_cast<void*>(&WheelVisualDetour),
                       g_wheelVisualOriginal,
                       g_wheelVisualArmed,
#if NFSMW_ENABLE_REAR_WHEEL_STEERING
                       "rear-wheel-visual-writer");
        }
#else
                       "wheel-visual-trace");
#endif
    } else {
        Log("RWS visual trace channel disabled; physics traces remain read-only");
    }
#endif
#if NFSMW_ENABLE_REAR_WHEEL_STEERING
    std::uintptr_t solverTarget = 0;
    if (ValidateFourWheelSolverTarget(&solverTarget)) {
        InstallChannel(solverTarget,
                       reinterpret_cast<void*>(&FourWheelSolverDetour),
                       g_fourWheelSolverOriginal,
                       g_fourWheelSolverArmed,
                       "rear-tire-direction-writer",
                       &g_rearPhysicsCreateStatus,
                       &g_rearPhysicsEnableStatus,
                       &g_rearPhysicsInstallStage);
    } else {
        Log("rear tire-direction writer disabled: strict validation failed");
    }
#endif
#if NFSMW_ENABLE_RWS_FORCE_TRACE
    ForceTraceFile("RWS TRUE STEERING EXPERIMENT v3: TRACE ONLY; NO REAR STEERING, TIRE FORCE OR MODEL WRITES; NOT A RELEASE");
#endif

    std::uintptr_t torqueTarget = 0;
    if (ValidateDriveTorqueTarget(&torqueTarget)) {
        g_driveTorqueTarget.store(torqueTarget, std::memory_order_release);
        g_driveTorqueProfileValid.store(true, std::memory_order_release);
        Log("handling probe channel=drive-torque pending: profile entry validated; waiting for player transmission vtable slot 8");
    } else {
        Log("handling probe channel=drive-torque disabled: strict validation failed; see preceding validation diagnostics");
    }

    const bool any = g_tireArmed.load(std::memory_order_acquire) ||
#if NFSMW_ENABLE_RWS_FORCE_TRACE || NFSMW_ENABLE_REAR_WHEEL_STEERING
                     g_wheelVisualArmed.load(std::memory_order_acquire) ||
#endif
#if NFSMW_ENABLE_REAR_WHEEL_STEERING
                     g_fourWheelSolverArmed.load(std::memory_order_acquire) ||
#endif
                     g_driveTorqueProfileValid.load(
                         std::memory_order_acquire);
    Log(any
            ? "handling probe available: sparse player-only readback; no tire-force or torque writes"
            : "handling probe unavailable; accepted steering, yaw, and camera channels remain active");
    return any;
#endif
}

bool WantsFrame(std::uint32_t physicsThread) {
#if !NFSMW_ENABLE_ALPHA_BRIDGE
    (void)physicsThread;
    return false;
#else
    if (physicsThread == 0 ||
        (!g_tireArmed.load(std::memory_order_acquire) &&
#if NFSMW_ENABLE_RWS_FORCE_TRACE || NFSMW_ENABLE_REAR_WHEEL_STEERING
         !g_wheelVisualArmed.load(std::memory_order_acquire) &&
#endif
         !g_driveTorqueProfileValid.load(std::memory_order_acquire))) {
        return false;
    }
    const DWORD now = GetTickCount();
    DWORD previous = g_lastSampleTick.load(std::memory_order_relaxed);
    if (previous != 0 &&
        static_cast<DWORD>(now - previous) < kSampleIntervalMs) {
        return false;
    }
    if (!g_lastSampleTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return false;
    }
    std::uint32_t noOwner = 0;
    if (!g_samplingOwnerThread.compare_exchange_strong(
            noOwner, physicsThread, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return false;
    }
    CloseWindow();
    return true;
#endif
}

void BeginFrame(const PlayerFrame& frame) {
#if !NFSMW_ENABLE_ALPHA_BRIDGE
    (void)frame;
#else
    CloseWindow();
    handling_probe_state::VehicleKey key{};
    bool wheelsValid = false;
    bool transmissionValid = false;
    if (!ResolveOwnership(frame, &key, &wheelsValid,
                          &transmissionValid)) {
        InvalidateFrame();
        return;
    }
    if (transmissionValid &&
        !g_driveTorqueArmed.load(std::memory_order_acquire)) {
        bool notAttempted = false;
        if (g_driveTorqueHookAttempted.compare_exchange_strong(
                notAttempted, true, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            const std::uintptr_t target =
                g_driveTorqueTarget.load(std::memory_order_acquire);
            std::uintptr_t freshTarget = 0;
            const bool targetStillValid =
                ValidateDriveTorqueTarget(&freshTarget) &&
                freshTarget == target;
            if (!targetStillValid) {
                Log("handling probe channel=drive-torque disabled: target changed before delayed hook installation");
            }
            const bool installed =
                targetStillValid &&
                InstallChannel(
                    target,
                    reinterpret_cast<void*>(&DriveTorqueDetour),
                    g_driveTorqueOriginal, g_driveTorqueArmed,
                    "drive-torque");
            if (!installed) {
                g_driveTorqueProfileValid.store(
                    false, std::memory_order_release);
                transmissionValid = false;
                key.transmission = 0;
            }
        }
    }
    if (!wheelsValid && !transmissionValid) {
        InvalidateFrame();
        return;
    }
    const std::uint32_t generation = g_generation.Observe(key);
    if (generation == 0) {
        InvalidateFrame();
        return;
    }
    if (g_stats.generation != generation) {
        g_stats = AggregateStats{};
        g_stats.generation = generation;
        char message[560]{};
        std::snprintf(
            message, sizeof(message),
            "handling_probe vehicle_generation=%lu player=%p pvehicle=%p rigid_body=%p holder=%p inner=%p suspension=%p transmission=%p wheels=%p,%p,%p,%p ownership_ok=1",
            static_cast<unsigned long>(generation),
            reinterpret_cast<void*>(key.player),
            reinterpret_cast<void*>(key.pvehicle),
            reinterpret_cast<void*>(key.rigidBody),
            reinterpret_cast<void*>(key.rigidBodyHolder),
            reinterpret_cast<void*>(key.rigidBodyInner),
            reinterpret_cast<void*>(key.suspension),
            reinterpret_cast<void*>(key.transmission),
            reinterpret_cast<void*>(key.wheels[0]),
            reinterpret_cast<void*>(key.wheels[1]),
            reinterpret_cast<void*>(key.wheels[2]),
            reinterpret_cast<void*>(key.wheels[3]));
        Log(message);
    }
    ++g_stats.sampledFrames;
    g_lastKey = key;
    g_lastFrame = frame;
    PublishWindow(frame, key, generation, wheelsValid,
                  transmissionValid);
#endif
}

void InvalidateFrame() {
#if NFSMW_ENABLE_ALPHA_BRIDGE
    if (g_samplingOwnerThread.load(std::memory_order_acquire) !=
        GetCurrentThreadId()) {
        return;
    }
    CloseWindow();
    g_generation.Invalidate();
    g_lastKey = handling_probe_state::VehicleKey{};
    g_lastFrame = PlayerFrame{};
    g_stats = AggregateStats{};
#endif
}

void EndFrame() {
#if NFSMW_ENABLE_ALPHA_BRIDGE
    const std::uint32_t thread = GetCurrentThreadId();
    if (g_samplingOwnerThread.load(std::memory_order_acquire) != thread) {
        return;
    }
#if NFSMW_ENABLE_RWS_FORCE_TRACE
    if (g_window.open.load(std::memory_order_acquire) &&
        g_window.wheelsValid.load(std::memory_order_relaxed) &&
        g_window.physicsThread.load(std::memory_order_relaxed) == thread &&
        g_window.physicsSerial.load(std::memory_order_relaxed) ==
            g_lastFrame.physicsSerial &&
        g_window.generation.load(std::memory_order_relaxed) ==
            g_stats.generation) {
        for (std::size_t index = 0; index < g_stats.wheels.size(); ++index) {
            WheelStats& stats = g_stats.wheels[index];
            const std::uintptr_t wheel =
                g_window.wheels[index].load(std::memory_order_relaxed);
            if (stats.calls == 0 || wheel != g_lastKey.wheels[index]) continue;
            bool valid = true;
            for (std::size_t axis = 0; axis < stats.worldForce.size(); ++axis) {
                valid = ReadFloatNoQuery(
                    wheel + kWheelWorldForceOffset + axis * sizeof(float),
                    &stats.worldForce[axis]) && valid;
            }
            stats.worldForceValid = valid;
        }
    }
#endif
    CloseWindow();
    if (g_generation.valid() && g_stats.generation != 0) {
        LogAggregate();
    }
    g_samplingOwnerThread.store(0, std::memory_order_release);
#endif
}

void PublishRearWheelSteering(std::uintptr_t pvehicle,
                              std::uintptr_t suspension,
                              std::uint32_t collectionKey,
                              std::uint64_t physicsSerial,
                              float angleRad,
                              bool enabled) {
#if NFSMW_ENABLE_ALPHA_BRIDGE && NFSMW_ENABLE_REAR_WHEEL_STEERING
    g_rearSteering.enabled.store(false, std::memory_order_release);
    if (!enabled || pvehicle == 0 || suspension < 0x4Cu ||
        collectionKey == 0 || physicsSerial == 0 ||
        !std::isfinite(angleRad)) {
        g_rearSteering.publishTick.store(0, std::memory_order_relaxed);
        return;
    }
    g_rearSteering.pvehicle.store(pvehicle, std::memory_order_relaxed);
    g_rearSteering.suspension.store(suspension,
                                     std::memory_order_relaxed);
    g_rearSteering.collectionKey.store(collectionKey,
                                        std::memory_order_relaxed);
    g_rearSteering.physicsSerial.store(physicsSerial,
                                        std::memory_order_relaxed);
    g_rearSteering.angleBits.store(RearFloatToBits(angleRad),
                                    std::memory_order_relaxed);
    g_rearSteering.publishTick.store(GetTickCount(),
                                      std::memory_order_relaxed);
    g_rearSteering.enabled.store(true, std::memory_order_release);
#else
    (void)pvehicle;
    (void)suspension;
    (void)collectionKey;
    (void)physicsSerial;
    (void)angleRad;
    (void)enabled;
#endif
}

RearWheelSteeringStats GetRearWheelSteeringStats() {
    RearWheelSteeringStats stats{};
#if NFSMW_ENABLE_ALPHA_BRIDGE && NFSMW_ENABLE_REAR_WHEEL_STEERING
    stats.visualHookArmed =
        g_wheelVisualArmed.load(std::memory_order_acquire);
    stats.physicsHookArmed =
        g_fourWheelSolverArmed.load(std::memory_order_acquire);
    stats.visualWrites =
        g_rearVisualWrites.load(std::memory_order_relaxed);
    stats.visualRejected =
        g_rearVisualRejected.load(std::memory_order_relaxed);
    stats.physicsWrites =
        g_rearPhysicsWrites.load(std::memory_order_relaxed);
    stats.physicsRestores =
        g_rearPhysicsRestores.load(std::memory_order_relaxed);
    stats.physicsRestoreFailures =
        g_rearPhysicsRestoreFailures.load(std::memory_order_relaxed);
    stats.physicsInstallStage =
        g_rearPhysicsInstallStage.load(std::memory_order_relaxed);
    stats.solverEntryMatches =
        g_solverEntryMatches.load(std::memory_order_relaxed);
    stats.solverCallContextMatches =
        g_solverCallContextMatches.load(std::memory_order_relaxed);
    stats.decodedSolverRva =
        g_decodedSolverRva.load(std::memory_order_relaxed);
    stats.physicsHookCreateStatus =
        g_rearPhysicsCreateStatus.load(std::memory_order_relaxed);
    stats.physicsHookEnableStatus =
        g_rearPhysicsEnableStatus.load(std::memory_order_relaxed);
#endif
    return stats;
}

}  // namespace nfsmw_drift_asi::handling_probe
