#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef NFSMW_ENABLE_INPUT_DIAGNOSTIC
#define NFSMW_ENABLE_INPUT_DIAGNOSTIC 0
#endif

#ifndef NFSMW_ENABLE_VEHICLE_DIAGNOSTIC
#define NFSMW_ENABLE_VEHICLE_DIAGNOSTIC 0
#endif

#ifndef NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC
#define NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC 0
#endif

#ifndef NFSMW_ENABLE_PHASE_DIAGNOSTIC
#define NFSMW_ENABLE_PHASE_DIAGNOSTIC 0
#endif

#ifndef NFSMW_ENABLE_ALPHA_BRIDGE
#define NFSMW_ENABLE_ALPHA_BRIDGE 0
#endif

#if (NFSMW_ENABLE_INPUT_DIAGNOSTIC + NFSMW_ENABLE_VEHICLE_DIAGNOSTIC + \
     NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC + NFSMW_ENABLE_PHASE_DIAGNOSTIC + \
     NFSMW_ENABLE_ALPHA_BRIDGE) > 1
#error "Diagnostics and the functional alpha require separate builds"
#endif

#include <nfsmw_sdk/nfsmw_sdk.h>
#include <nfsmw_sdk/scan.h>
#if NFSMW_ENABLE_INPUT_DIAGNOSTIC
#include <MinHook.h>
#endif

#include "drift_assist.hpp"
#include "drift_assist_ini.hpp"
#include "nfsmw_drift_target_profile.hpp"
#include "rear_wheel_steering.hpp"
#include "reforged_startup_gate.hpp"
#include "runtime_logging.hpp"
#include "vehicle_countersteer.hpp"
#if NFSMW_ENABLE_VEHICLE_DIAGNOSTIC
#include "vehicle_getter_diagnostic.hpp"
#endif
#if NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC
#include "coordinator_diagnostic.hpp"
#endif
#if NFSMW_ENABLE_PHASE_DIAGNOSTIC
#include "phase_timing_diagnostic.hpp"
#endif
#if NFSMW_ENABLE_ALPHA_BRIDGE
#include "alpha_bridge.hpp"
#endif

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>

static_assert(sizeof(void*) == 4,
              "The NFSMW drift host must be built as a 32-bit PE DLL");

namespace nfsmw_drift_asi {
namespace {

nfsmw_drift::DriftAssistController g_controller{};
vehicle_countersteer::Registry g_vehicleCountersteerMultipliers{};
rear_wheel_steering::Registry g_rearWheelSteeringVehicles{};

#if NFSMW_ENABLE_INPUT_DIAGNOSTIC
// This is the concrete ABI of SteeringWheelDevice::PollAllGameActionBindings
// in the measured PE32 target. ECX carries the object pointer; there are no
// stack arguments and the function returns with a plain `ret`. MSVC does not
// permit `__thiscall` on a free-function pointer type, so the detour/trampoline
// uses the standard x86 fastcall bridge: ECX is the object and the unused EDX
// slot is an explicit second parameter. This has the same register/stack ABI
// for this zero-argument target on both MSVC and MinGW.
using InputPollFn = void (NFSMW_FASTCALL*)(void* self, void* unusedEdx);

constexpr std::size_t kInputActionRowCount = 76;
constexpr std::size_t kLoggedInputRowCount = 10;
constexpr DWORD kInputDiagnosticLogIntervalMs = 250;

enum class InputSnapshotStatus {
    Ok,
    InvalidArguments,
    SelfUnreadable,
    VtableUnreadable,
    PollSlotMismatch,
    MirrorUnreadable,
    NonFiniteValue,
    AccessViolation,
};

std::atomic<InputPollFn> g_inputPollOriginal{nullptr};
std::atomic<std::uintptr_t> g_inputPollTarget{0};
std::atomic<DWORD> g_inputDiagnosticLastLogTick{0};
std::atomic<bool> g_inputDiagnosticFailureLogged{false};
std::atomic<bool> g_inputDiagnosticArmed{false};
#endif

void Log(const char* message) {
    if (message == nullptr) {
        return;
    }
    ::nfsmw_drift_asi::runtime_logging::DebugOutput("[nfsmw_drift_assist] ");
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(message);
    ::nfsmw_drift_asi::runtime_logging::DebugOutput("\n");
}

HMODULE SelfModule() {
    HMODULE module = nullptr;
    // Resolve the module from an address in this translation unit. This keeps
    // configuration lookup correct when the ASI file is renamed or loaded
    // from a directory different from the game's current working directory.
    const BOOL resolved = GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&SelfModule), &module);
    return resolved != FALSE ? module : nullptr;
}

std::string ModuleDirectory(HMODULE module) {
    if (module == nullptr) {
        return {};
    }

    char path[1024]{};
    const DWORD length = GetModuleFileNameA(
        module, path, static_cast<DWORD>(sizeof(path)));
    if (length == 0 || length >= sizeof(path)) {
        return {};
    }

    std::string directory(path, length);
    const std::string::size_type separator = directory.find_last_of("\\/");
    if (separator == std::string::npos) {
        return {};
    }
    directory.resize(separator + 1);
    return directory;
}

bool ReadConfigFile(const std::string& path, std::string& text) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }

    stream.seekg(0, std::ios::end);
    const std::streampos end = stream.tellg();
    if (end < 0 || end > static_cast<std::streamoff>(1024 * 1024)) {
        return false;
    }
    stream.seekg(0, std::ios::beg);
    text.assign(std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
}

const char* IniErrorName(nfsmw_drift::IniParseErrorCode error) {
    switch (error) {
    case nfsmw_drift::IniParseErrorCode::MalformedLine:
        return "malformed line";
    case nfsmw_drift::IniParseErrorCode::InvalidValue:
        return "invalid value";
    case nfsmw_drift::IniParseErrorCode::None:
    default:
        return "unknown error";
    }
}

bool LoadControllerConfig() {
    const HMODULE module = SelfModule();
    const std::string directory = ModuleDirectory(module);
    if (directory.empty()) {
        Log("could not resolve the ASI module directory; controller disabled");
        nfsmw_drift::AssistConfig disabled{};
        disabled.enabled = false;
        g_controller.setConfig(disabled);
        g_controller.reset();
        return false;
    }

    const std::string path = directory + "drift_assist.ini";
    std::string text;
    if (!ReadConfigFile(path, text)) {
        Log("drift_assist.ini was not found or could not be read; controller disabled");
        nfsmw_drift::AssistConfig disabled{};
        disabled.enabled = false;
        g_controller.setConfig(disabled);
        g_controller.reset();
        return false;
    }

    nfsmw_drift::AssistConfig config{};
    const nfsmw_drift::IniParseResult parsed =
        nfsmw_drift::ParseAssistConfigIni(text, config);
    if (!parsed) {
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "drift_assist.ini %s at line %llu; controller disabled",
                      IniErrorName(parsed.error),
                      static_cast<unsigned long long>(parsed.line));
        Log(message);
        config.enabled = false;
    }

    const nfsmw_drift::IniParseResult vehicleParsed =
        vehicle_countersteer::ParseIni(
            text, g_vehicleCountersteerMultipliers);
    if (!vehicleParsed) {
        char message[224]{};
        std::snprintf(
            message, sizeof(message),
            "drift_assist.ini vehicle multiplier section %s at line %llu; controller disabled",
            IniErrorName(vehicleParsed.error),
            static_cast<unsigned long long>(vehicleParsed.line));
        Log(message);
        config.enabled = false;
    }

#if NFSMW_ENABLE_REAR_WHEEL_STEERING
    const nfsmw_drift::IniParseResult rearSteeringParsed =
        rear_wheel_steering::ParseIni(text, g_rearWheelSteeringVehicles);
    if (!rearSteeringParsed) {
        char message[224]{};
        std::snprintf(
            message, sizeof(message),
            "drift_assist.ini rear-wheel steering section %s at line %llu; controller disabled",
            IniErrorName(rearSteeringParsed.error),
            static_cast<unsigned long long>(rearSteeringParsed.line));
        Log(message);
        config.enabled = false;
    }
#else
    const nfsmw_drift::IniParseResult rearSteeringParsed{};
    g_rearWheelSteeringVehicles = {};
#endif

    g_controller.setConfig(config);
    g_controller.reset();
    if (config.enabled) {
        char message[256]{};
        std::snprintf(
            message, sizeof(message),
            "drift_assist.ini loaded; controller configuration is enabled with %llu countersteer multiplier(s) and %llu rear-wheel steering vehicle(s)",
            static_cast<unsigned long long>(
                g_vehicleCountersteerMultipliers.count),
            static_cast<unsigned long long>(
                g_rearWheelSteeringVehicles.count));
        Log(message);
    } else {
        Log("drift_assist.ini loaded; controller is disabled by configuration");
    }
    return parsed.ok && vehicleParsed.ok && rearSteeringParsed.ok &&
           config.enabled;
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
    if (protection == PAGE_NOACCESS) {
        return false;
    }

    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t regionBegin =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + info.RegionSize;
    if (regionEnd < regionBegin || begin < regionBegin) {
        return false;
    }
    return size <= regionEnd - begin;
}

bool ExecutableAddress(const void* address) {
    if (!ReadableRange(address, 1)) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xffu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

// The legacy vehicle diagnostic still uses InlineHook, whose trampoline is
// published immediately after the detour is enabled. Keep its bounded wait
// local to that diagnostic; the input diagnostic publishes its trampoline
// before queuing the raw MinHook enable operation.
constexpr unsigned kHookOriginalWaitAttempts = 128;

#if NFSMW_ENABLE_INPUT_DIAGNOSTIC

// SteeringWheelDevice's poll method is slot 4 (the fifth virtual entry) in the
// measured v1.3 English executable. Keep this as an explicit ABI guard:
// accepting a different slot could mistake an unrelated object for the
// action poller and make the diagnostic read arbitrary memory.
constexpr std::size_t kInputPollVtableSlot = 4;

const char* InputSnapshotStatusName(InputSnapshotStatus status) {
    switch (status) {
        case InputSnapshotStatus::Ok:
            return "ok";
        case InputSnapshotStatus::InvalidArguments:
            return "invalid diagnostic arguments";
        case InputSnapshotStatus::SelfUnreadable:
            return "input object is unreadable";
        case InputSnapshotStatus::VtableUnreadable:
            return "input object vtable is unreadable";
        case InputSnapshotStatus::PollSlotMismatch:
            return "input object vtable slot 4 does not match the validated poll target";
        case InputSnapshotStatus::MirrorUnreadable:
            return "input action mirror at self+0x20 is unreadable";
        case InputSnapshotStatus::NonFiniteValue:
            return "input action mirror contains a non-finite value";
        case InputSnapshotStatus::AccessViolation:
            return "input object changed during the guarded snapshot";
    }
    return "unknown snapshot status";
}

InputSnapshotStatus SnapshotInputRows(void* self,
                                      float* loggedRows,
                                      const void** mirrorOut) {
    if (loggedRows == nullptr) {
        return InputSnapshotStatus::InvalidArguments;
    }
    if (self == nullptr ||
        // The game prologue reads self+0x24 before it reaches the mirror
        // write, so include the complete 0x00..0x27 object prefix here.
        !ReadableRange(self, 0x28)) {
        return InputSnapshotStatus::SelfUnreadable;
    }

    const auto expectedTarget = g_inputPollTarget.load(std::memory_order_acquire);
    if (expectedTarget == 0) {
        return InputSnapshotStatus::InvalidArguments;
    }

    float* mirror = nullptr;
#if defined(_MSC_VER)
    // Every object-owned dereference is inside one SEH region.  The game can
    // destroy or replace its input object while a mod callback is running;
    // a failed read must only skip this sample, never unwind into the game.
    __try {
#endif
        // Validate the object vtable before following any object-owned pointer.
        // The slot check prevents an unexpected `self` from being treated as
        // the game's SteeringWheelDevice instance.
        const auto selfBytes = reinterpret_cast<const unsigned char*>(self);
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(selfBytes);
        if (vtable == 0 ||
            !ReadableRange(reinterpret_cast<const void*>(vtable),
                           (kInputPollVtableSlot + 1) * sizeof(std::uintptr_t))) {
            return InputSnapshotStatus::VtableUnreadable;
        }
        const auto pollSlot = *reinterpret_cast<const std::uintptr_t*>(
            vtable + kInputPollVtableSlot * sizeof(std::uintptr_t));
        if (pollSlot != expectedTarget ||
            !ExecutableAddress(reinterpret_cast<const void*>(pollSlot))) {
            return InputSnapshotStatus::PollSlotMismatch;
        }

        // The original function stores its 76 aggregate action floats through
        // the pointer at self+0x20.  Keep the complete range check even though
        // only rows 0..9 are logged, so a truncated/corrupt mirror is never
        // sampled.
        mirror = *reinterpret_cast<float* const*>(selfBytes + 0x20);
        if (mirror == nullptr ||
            !ReadableRange(mirror, kInputActionRowCount * sizeof(float))) {
            return InputSnapshotStatus::MirrorUnreadable;
        }
        for (std::size_t index = 0; index < kInputActionRowCount; ++index) {
            const float value = mirror[index];
            if (!std::isfinite(value)) {
                return InputSnapshotStatus::NonFiniteValue;
            }
            if (index < kLoggedInputRowCount) {
                loggedRows[index] = value;
            }
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return InputSnapshotStatus::AccessViolation;
    }
#endif

    if (mirrorOut != nullptr) {
        *mirrorOut = mirror;
    }
    return InputSnapshotStatus::Ok;
}

void LogInputSnapshot(void* self, const void* mirror, const float* rows) {
    if (rows == nullptr) {
        return;
    }

    const DWORD now = GetTickCount();
    DWORD previous = g_inputDiagnosticLastLogTick.load(std::memory_order_relaxed);
    if (static_cast<DWORD>(now - previous) < kInputDiagnosticLogIntervalMs ||
        !g_inputDiagnosticLastLogTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }

    char message[768]{};
    std::snprintf(
        message, sizeof(message),
        "input diagnostic self=%p mirror=%p rows[0..9] "
        "r0(GAS)=%.3f r1(BRAKE)=%.3f r2(STEERLEFT)=%.3f "
        "r3(STEERRIGHT)=%.3f r4(HANDBRAKE)=%.3f "
        "r5(GAMEBREAKER)=%.3f r6(NOS)=%.3f r7(SHIFTDOWN)=%.3f "
        "r8(SHIFTUP)=%.3f r9(RESET)=%.3f",
        self, mirror, rows[0], rows[1], rows[2], rows[3], rows[4], rows[5],
        rows[6], rows[7], rows[8], rows[9]);
    Log(message);
}

void NFSMW_FASTCALL InputPollDetour(void* self, void* /*unusedEdx*/) {
    const InputPollFn original =
        g_inputPollOriginal.load(std::memory_order_acquire);
    // InstallInputDiagnosticHook publishes this non-null trampoline before the
    // disabled hook is ever queued for enable, and never clears it after an
    // ApplyQueued attempt. Every detoured poll therefore chains exactly once.
    original(self, nullptr);

    // During the narrow enable/rollback windows the hook is deliberately a
    // transparent pass-through. Only a fully installed diagnostic may sample.
    if (!g_inputDiagnosticArmed.load(std::memory_order_acquire)) {
        return;
    }

    float rows[kLoggedInputRowCount]{};
    const void* mirror = nullptr;
    const InputSnapshotStatus snapshotStatus =
        SnapshotInputRows(self, rows, &mirror);
    if (snapshotStatus != InputSnapshotStatus::Ok) {
        if (!g_inputDiagnosticFailureLogged.exchange(
                true, std::memory_order_relaxed)) {
            char message[256]{};
            std::snprintf(message, sizeof(message),
                          "input diagnostic skipped: %s",
                          InputSnapshotStatusName(snapshotStatus));
            Log(message);
        }
        return;
    }
    LogInputSnapshot(self, mirror, rows);
}

bool InstallInputDiagnosticHook(std::uintptr_t validatedTarget) {
    if (validatedTarget == 0 || !ExecutableAddress(
                                  reinterpret_cast<const void*>(validatedTarget))) {
        Log("input diagnostic disabled: validated poll target is not executable");
        return false;
    }

    g_inputDiagnosticArmed.store(false, std::memory_order_release);
    const MH_STATUS initializeStatus = MH_Initialize();
    if (initializeStatus != MH_OK &&
        initializeStatus != MH_ERROR_ALREADY_INITIALIZED) {
        char message[256]{};
        std::snprintf(
            message, sizeof(message),
            "input diagnostic disabled: MinHook initialization failed (%s)",
            MH_StatusToString(initializeStatus));
        Log(message);
        return false;
    }

    void* const target = reinterpret_cast<void*>(validatedTarget);
    void* originalRaw = nullptr;
    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void*>(&InputPollDetour), &originalRaw);
    if (createStatus != MH_OK || originalRaw == nullptr) {
        const MH_STATUS removeStatus =
            createStatus == MH_OK ? MH_RemoveHook(target)
                                  : MH_ERROR_NOT_CREATED;
        char message[320]{};
        std::snprintf(
            message, sizeof(message),
            "input diagnostic disabled: MinHook creation failed (%s); disabled cleanup=%s",
            MH_StatusToString(createStatus), MH_StatusToString(removeStatus));
        Log(message);
        return false;
    }

    // The hook is still disabled. Publish all state needed by the detour before
    // MinHook can redirect another thread to it.
    const InputPollFn original = reinterpret_cast<InputPollFn>(originalRaw);
    g_inputPollOriginal.store(original, std::memory_order_release);
    g_inputPollTarget.store(validatedTarget, std::memory_order_release);
    g_inputDiagnosticLastLogTick.store(0, std::memory_order_relaxed);
    g_inputDiagnosticFailureLogged.store(false, std::memory_order_relaxed);

    const MH_STATUS queueStatus = MH_QueueEnableHook(target);
    if (queueStatus != MH_OK) {
        // ApplyQueued has not run, so no thread can be inside this detour and
        // the disabled trampoline can still be removed safely.
        const MH_STATUS removeStatus = MH_RemoveHook(target);
        g_inputPollOriginal.store(nullptr, std::memory_order_release);
        g_inputPollTarget.store(0, std::memory_order_release);
        char message[320]{};
        std::snprintf(
            message, sizeof(message),
            "input diagnostic disabled: MinHook enable queue failed (%s); disabled cleanup=%s",
            MH_StatusToString(queueStatus), MH_StatusToString(removeStatus));
        Log(message);
        return false;
    }

    const MH_STATUS applyStatus = MH_ApplyQueued();
    if (applyStatus != MH_OK) {
        // ApplyQueued may have changed the target before reporting failure.
        // Retain the hook and published trampoline for process life; even if
        // rollback fails, any surviving detour stays a transparent chain-back.
        const MH_STATUS disableQueueStatus = MH_QueueDisableHook(target);
        const MH_STATUS rollbackStatus = MH_ApplyQueued();
        char message[384]{};
        std::snprintf(
            message, sizeof(message),
            "input diagnostic disabled: MinHook enable failed (%s); retained pass-through rollback queue=%s apply=%s",
            MH_StatusToString(applyStatus),
            MH_StatusToString(disableQueueStatus),
            MH_StatusToString(rollbackStatus));
        Log(message);
        // Report a handled load state so hosts that honor NFSMW_Load's return
        // value keep this module resident. Sampling remains disarmed, but any
        // in-flight or surviving detour always retains valid code/trampoline
        // storage for its transparent chain-back.
        return true;
    }

    g_inputDiagnosticArmed.store(true, std::memory_order_release);
    char message[160]{};
    std::snprintf(message, sizeof(message),
                  "read-only input diagnostic installed at 0x%p (ECX/fastcall bridge)",
                  reinterpret_cast<void*>(validatedTarget));
    Log(message);
    return true;
}

#endif

#if NFSMW_ENABLE_VEHICLE_DIAGNOSTIC

// ActiveComponents_TickAll is a verified cdecl(float) entry point in the
// measured speed.exe. The globals and vtables below are observations for
// that exact PE profile; they are used only for read-only diagnostics.
using VehicleTickFn = void (NFSMW_CDECL*)(float dt);

constexpr std::uintptr_t kActiveComponentsTickRva = 0x000BA940u;
constexpr std::uintptr_t kActiveVehicleArrayHeadRva = 0x00513E74u;
constexpr std::uintptr_t kActiveVehicleArrayCountRva = 0x00513E7Cu;
constexpr std::uintptr_t kPVehicleInstancesRva = 0x005352B0u;
constexpr std::uintptr_t kPlayerVehicleVtableRva = 0x004AC06Cu;
constexpr std::uintptr_t kAiVehicleVtableRva = 0x004AC0FCu;
constexpr std::uintptr_t kSubPhysicsVtableRva = 0x004AB6A0u;

constexpr std::size_t kVehicleEntryLimit = 64;
constexpr std::size_t kPVehicleInstanceLimit = 64;
constexpr std::size_t kPVehicleInstanceStride = 8;
constexpr std::size_t kPVehicleReadableSize = 0x160;
constexpr std::size_t kVehicleBodyReadableSize = 0x148;
constexpr std::size_t kVtableProbeSlots = 2;
constexpr std::size_t kRawFieldCount = 17; // body+0x100 .. body+0x140
constexpr std::size_t kMaxLoggedVehicleEntries = 16;
constexpr std::size_t kMaxLoggedPVehicleEntries = 16;
constexpr DWORD kVehicleDiagnosticLogIntervalMs = 500;
constexpr float kMaximumDiagnosticFloatMagnitude = 100000000.0f;

std::unique_ptr<nfsmw::InlineHook<VehicleTickFn>> g_vehicleTickHook;
std::atomic<VehicleTickFn> g_vehicleTickOriginal{nullptr};
std::atomic<std::uintptr_t> g_vehicleTickTarget{0};
std::atomic<std::uintptr_t> g_vehicleImageBase{0};
std::atomic<std::size_t> g_vehicleImageSize{0};
std::atomic<DWORD> g_vehicleDiagnosticLastLogTick{0};
std::atomic<bool> g_vehicleDiagnosticChainFailureLogged{false};

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

bool VehicleImageRange(const void* address, std::size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }

    const std::uintptr_t base =
        g_vehicleImageBase.load(std::memory_order_acquire);
    const std::size_t imageSize =
        g_vehicleImageSize.load(std::memory_order_acquire);
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    if (base == 0 || imageSize == 0 || begin < base) {
        return false;
    }
    const std::uintptr_t offset = begin - base;
    if (offset > imageSize || size > imageSize - offset) {
        return false;
    }
    return ReadableRange(address, size);
}

bool SafeReadPointer(const void* address, std::uintptr_t* value) {
    if (address == nullptr || value == nullptr ||
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
    if (address == nullptr || value == nullptr ||
        !ReadableRange(address, sizeof(float))) {
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
           std::fabs(*value) <= kMaximumDiagnosticFloatMagnitude;
}

bool SafeReadU32(const void* address, std::uint32_t* value) {
    if (address == nullptr || value == nullptr ||
        !ReadableRange(address, sizeof(std::uint32_t))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        *value = *reinterpret_cast<const std::uint32_t*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *value = *reinterpret_cast<const std::uint32_t*>(address);
#endif
    return true;
}

bool ValidDiagnosticVtable(std::uintptr_t vtable) {
    if (!VehicleImageRange(reinterpret_cast<const void*>(vtable),
                           kVtableProbeSlots * sizeof(std::uintptr_t))) {
        return false;
    }
    for (std::size_t slot = 0; slot < kVtableProbeSlots; ++slot) {
        std::uintptr_t method = 0;
        std::uintptr_t slotAddress = 0;
        if (!AddAddress(vtable, slot * sizeof(std::uintptr_t),
                        &slotAddress) ||
            !SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                             &method) ||
            !ExecutableAddress(reinterpret_cast<const void*>(method))) {
            return false;
        }
    }
    return true;
}

bool ReserveVehicleDiagnosticLog() {
    const DWORD now = GetTickCount();
    DWORD previous =
        g_vehicleDiagnosticLastLogTick.load(std::memory_order_relaxed);
    if (static_cast<DWORD>(now - previous) <
        kVehicleDiagnosticLogIntervalMs) {
        return false;
    }
    return g_vehicleDiagnosticLastLogTick.compare_exchange_strong(
        previous, now, std::memory_order_relaxed,
        std::memory_order_relaxed);
}

bool ReadVehicleRawFields(std::uintptr_t body, float* fields) {
    if (fields == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < kRawFieldCount; ++index) {
        std::uintptr_t address = 0;
        if (!AddAddress(body, 0x100u + index * sizeof(float), &address) ||
            !SafeReadFiniteFloat(reinterpret_cast<const void*>(address),
                                 &fields[index])) {
            return false;
        }
    }
    return true;
}

const char* VehicleKind(std::uintptr_t vtable, std::uintptr_t imageBase) {
    if (vtable == imageBase + kPlayerVehicleVtableRva) {
        return "player-candidate";
    }
    if (vtable == imageBase + kAiVehicleVtableRva) {
        return "ai-candidate";
    }
    return "other-vtable";
}

const char* SubPhysicsKind(std::uintptr_t vtable, std::uintptr_t imageBase) {
    if (vtable == imageBase + kSubPhysicsVtableRva) {
        return "known-subphysics";
    }
    return "other-subphysics";
}

bool ReadObjectVtable(std::uintptr_t object,
                      std::uintptr_t* vtableOut,
                      bool requireKnownMethods = false) {
    if (vtableOut == nullptr || object == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(object),
                       sizeof(std::uintptr_t)) ||
        !SafeReadPointer(reinterpret_cast<const void*>(object), vtableOut)) {
        return false;
    }
    if (*vtableOut == 0 ||
        !VehicleImageRange(reinterpret_cast<const void*>(*vtableOut),
                           kVtableProbeSlots * sizeof(std::uintptr_t))) {
        return false;
    }
    return !requireKnownMethods || ValidDiagnosticVtable(*vtableOut);
}

bool ReadObjectFieldPointer(std::uintptr_t object,
                            std::size_t offset,
                            std::uintptr_t* valueOut) {
    std::uintptr_t address = 0;
    return AddAddress(object, offset, &address) &&
           SafeReadPointer(reinterpret_cast<const void*>(address), valueOut);
}

void LogPVehicleInstances() {
    const std::uintptr_t imageBase =
        g_vehicleImageBase.load(std::memory_order_acquire);
    std::uintptr_t arrayAddress = 0;
    if (!AddAddress(imageBase, kPVehicleInstancesRva, &arrayAddress) ||
        !VehicleImageRange(reinterpret_cast<const void*>(arrayAddress),
                           sizeof(std::uintptr_t))) {
        Log("pvehicle diagnostic skipped: instance table address failed validation");
        return;
    }

    std::size_t occupiedCount = 0;
    std::size_t validCount = 0;
    std::size_t invalidCount = 0;
    std::size_t emittedCount = 0;
    for (std::size_t index = 0; index < kPVehicleInstanceLimit; ++index) {
        std::uintptr_t slotAddress = 0;
        std::uintptr_t pvehicle = 0;
        if (!AddAddress(arrayAddress, index * kPVehicleInstanceStride,
                        &slotAddress) ||
            !SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                             &pvehicle)) {
            ++invalidCount;
            break;
        }
        // PVehicle::GetInstancesCount() uses a null pointer as the table
        // terminator.  Stop here instead of probing past the known array.
        if (pvehicle == 0) {
            break;
        }
        ++occupiedCount;

        std::uintptr_t primaryVtable = 0;
        if (!ReadableRange(reinterpret_cast<const void*>(pvehicle),
                           kPVehicleReadableSize) ||
            !ReadObjectVtable(pvehicle, &primaryVtable)) {
            ++invalidCount;
            continue;
        }

        std::uintptr_t rigidBody = 0;
        std::uintptr_t player = 0;
        std::uintptr_t input = 0;
        std::uintptr_t suspension = 0;
        if (!ReadObjectFieldPointer(pvehicle, 0x78u, &rigidBody) ||
            !ReadObjectFieldPointer(pvehicle, 0x84u, &player) ||
            !ReadObjectFieldPointer(pvehicle, 0xE8u, &input) ||
            !ReadObjectFieldPointer(pvehicle, 0xF0u, &suspension)) {
            ++invalidCount;
            continue;
        }

        float speed = 0.0f;
        float slipAngle = 0.0f;
        float localX = 0.0f;
        float localY = 0.0f;
        float localZ = 0.0f;
        std::uint32_t wheelsOnGround = 0;
        std::uintptr_t speedAddress = 0;
        std::uintptr_t slipAddress = 0;
        std::uintptr_t wheelsAddress = 0;
        std::uintptr_t localAddress = 0;
        if (!AddAddress(pvehicle, 0x11Cu, &speedAddress) ||
            !AddAddress(pvehicle, 0x12Cu, &slipAddress) ||
            !AddAddress(pvehicle, 0x130u, &wheelsAddress) ||
            !AddAddress(pvehicle, 0x134u, &localAddress) ||
            !SafeReadFiniteFloat(reinterpret_cast<const void*>(speedAddress),
                                 &speed) ||
            !SafeReadFiniteFloat(reinterpret_cast<const void*>(slipAddress),
                                 &slipAngle) ||
            !SafeReadU32(reinterpret_cast<const void*>(wheelsAddress),
                         &wheelsOnGround) ||
            !SafeReadFiniteFloat(reinterpret_cast<const void*>(localAddress),
                                 &localX) ||
            !SafeReadFiniteFloat(reinterpret_cast<const void*>(localAddress + 4),
                                 &localY) ||
            !SafeReadFiniteFloat(reinterpret_cast<const void*>(localAddress + 8),
                                 &localZ)) {
            ++invalidCount;
            continue;
        }

        std::uintptr_t rigidBodyVtable = 0;
        std::uintptr_t inputVtable = 0;
        std::uintptr_t suspensionVtable = 0;
        const bool rigidBodyVtableValid =
            ReadObjectVtable(rigidBody, &rigidBodyVtable);
        const bool inputVtableValid = ReadObjectVtable(input, &inputVtable);
        const bool suspensionVtableValid =
            ReadObjectVtable(suspension, &suspensionVtable);
        ++validCount;

        if (emittedCount < kMaxLoggedPVehicleEntries) {
            char line[1152]{};
            std::snprintf(
                line, sizeof(line),
                "pvehicle[%llu] kind=%s pv=%p vtbl=%p rb=%p rbvtbl=%p "
                "player=%p input=%p inputvtbl=%p suspension=%p suspvtbl=%p "
                "speed=%.3f slip=%.3f grounded=%lu local=(%.3f %.3f %.3f) "
                "vtblChecks=%d/%d/%d",
                static_cast<unsigned long long>(index),
                VehicleKind(primaryVtable, imageBase),
                reinterpret_cast<void*>(pvehicle),
                reinterpret_cast<void*>(primaryVtable),
                reinterpret_cast<void*>(rigidBody),
                reinterpret_cast<void*>(rigidBodyVtable),
                reinterpret_cast<void*>(player), reinterpret_cast<void*>(input),
                reinterpret_cast<void*>(inputVtable),
                reinterpret_cast<void*>(suspension),
                reinterpret_cast<void*>(suspensionVtable),
                static_cast<double>(speed), static_cast<double>(slipAngle),
                static_cast<unsigned long>(wheelsOnGround),
                static_cast<double>(localX), static_cast<double>(localY),
                static_cast<double>(localZ), rigidBodyVtableValid ? 1 : 0,
                inputVtableValid ? 1 : 0, suspensionVtableValid ? 1 : 0);
            Log(line);
            ++emittedCount;
        }
    }

    char result[256]{};
    std::snprintf(result, sizeof(result),
                  "pvehicle diagnostic occupied=%llu valid=%llu invalid=%llu emitted=%llu%s",
                  static_cast<unsigned long long>(occupiedCount),
                  static_cast<unsigned long long>(validCount),
                  static_cast<unsigned long long>(invalidCount),
                  static_cast<unsigned long long>(emittedCount),
                  validCount > emittedCount ? " (entry log capped)" : "");
    Log(result);
}

void LogVehicleDiagnosticSnapshot(float dt) {
    if (!ReserveVehicleDiagnosticLog()) {
        return;
    }

    // The active-component list identifies the transient wrapper/body path;
    // the SDK instance table independently exposes the full PVehicle object.
    // Sampling both lets the next bridge match the player vehicle without
    // assuming that the wrapper itself is a PVehicle.
    LogPVehicleInstances();

    const std::uintptr_t imageBase =
        g_vehicleImageBase.load(std::memory_order_acquire);
    std::uintptr_t headAddress = 0;
    std::uintptr_t countAddress = 0;
    std::uintptr_t entries = 0;
    std::uintptr_t countValue = 0;
    if (!AddAddress(imageBase, kActiveVehicleArrayHeadRva, &headAddress) ||
        !AddAddress(imageBase, kActiveVehicleArrayCountRva, &countAddress) ||
        !VehicleImageRange(reinterpret_cast<const void*>(headAddress),
                           sizeof(std::uintptr_t)) ||
        !VehicleImageRange(reinterpret_cast<const void*>(countAddress),
                           sizeof(std::uint32_t)) ||
        !SafeReadPointer(reinterpret_cast<const void*>(headAddress), &entries) ||
        !SafeReadPointer(reinterpret_cast<const void*>(countAddress),
                         &countValue)) {
        Log("vehicle diagnostic skipped: active array globals failed validation");
        return;
    }

    if (countValue > kVehicleEntryLimit) {
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "vehicle diagnostic skipped: entry count %llu exceeds limit %llu",
                      static_cast<unsigned long long>(countValue),
                      static_cast<unsigned long long>(kVehicleEntryLimit));
        Log(message);
        return;
    }

    char summary[256]{};
    std::snprintf(summary, sizeof(summary),
                  "vehicle diagnostic dt=%.4f entries=%p count=%llu",
                  static_cast<double>(dt), reinterpret_cast<void*>(entries),
                  static_cast<unsigned long long>(countValue));
    Log(summary);

    if (countValue == 0) {
        return;
    }
    const std::size_t count = static_cast<std::size_t>(countValue);
    if (entries == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(entries),
                       count * sizeof(std::uintptr_t))) {
        Log("vehicle diagnostic skipped: active entry array is unreadable");
        return;
    }

    std::size_t validCount = 0;
    std::size_t invalidCount = 0;
    std::size_t emittedCount = 0;
    for (std::size_t index = 0; index < count; ++index) {
        std::uintptr_t entryAddress = 0;
        std::uintptr_t entry = 0;
        std::uintptr_t bodyAddress = 0;
        std::uintptr_t body = 0;
        if (!AddAddress(entries, index * sizeof(std::uintptr_t),
                        &entryAddress) ||
            !SafeReadPointer(reinterpret_cast<const void*>(entryAddress),
                             &entry) || entry == 0 ||
            !ReadableRange(reinterpret_cast<const void*>(entry), 0x0Cu) ||
            !AddAddress(entry, 0x08u, &bodyAddress) ||
            !SafeReadPointer(reinterpret_cast<const void*>(bodyAddress),
                             &body) || body == 0 ||
            !ReadableRange(reinterpret_cast<const void*>(body),
                           kVehicleBodyReadableSize)) {
            ++invalidCount;
            continue;
        }

        std::uintptr_t primaryVtable = 0;
        std::uintptr_t secondaryVtable = 0;
        std::uintptr_t subPhysics = 0;
        std::uintptr_t subPhysicsVtable = 0;
        std::uintptr_t address = 0;
        if (!SafeReadPointer(reinterpret_cast<const void*>(body),
                             &primaryVtable) ||
            !ValidDiagnosticVtable(primaryVtable) ||
            !AddAddress(body, 0x08u, &address) ||
            !SafeReadPointer(reinterpret_cast<const void*>(address),
                             &secondaryVtable) ||
            (secondaryVtable != 0 &&
             !ValidDiagnosticVtable(secondaryVtable)) ||
            !AddAddress(body, 0x98u, &address) ||
            !SafeReadPointer(reinterpret_cast<const void*>(address),
                             &subPhysics)) {
            ++invalidCount;
            continue;
        }
        if (subPhysics != 0 &&
            (!ReadableRange(reinterpret_cast<const void*>(subPhysics),
                            sizeof(std::uintptr_t)) ||
             !SafeReadPointer(reinterpret_cast<const void*>(subPhysics),
                              &subPhysicsVtable) ||
             !ValidDiagnosticVtable(subPhysicsVtable))) {
            ++invalidCount;
            continue;
        }

        float rawFields[kRawFieldCount]{};
        if (!ReadVehicleRawFields(body, rawFields)) {
            ++invalidCount;
            continue;
        }
        ++validCount;
        if (emittedCount >= kMaxLoggedVehicleEntries) {
            continue;
        }

        char line[1024]{};
        std::snprintf(
            line, sizeof(line),
            "vehicle[%llu] kind=%s entry=%p body=%p vtbl=%p secondary=%p "
            "sub=%p subkind=%s subvtbl=%p raw100..11c=[%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f]",
            static_cast<unsigned long long>(index),
            VehicleKind(primaryVtable, imageBase),
            reinterpret_cast<void*>(entry), reinterpret_cast<void*>(body),
            reinterpret_cast<void*>(primaryVtable),
            reinterpret_cast<void*>(secondaryVtable),
            reinterpret_cast<void*>(subPhysics),
            SubPhysicsKind(subPhysicsVtable, imageBase),
            reinterpret_cast<void*>(subPhysicsVtable),
            static_cast<double>(rawFields[0]), static_cast<double>(rawFields[1]),
            static_cast<double>(rawFields[2]), static_cast<double>(rawFields[3]),
            static_cast<double>(rawFields[4]), static_cast<double>(rawFields[5]),
            static_cast<double>(rawFields[6]), static_cast<double>(rawFields[7]));
        Log(line);

        std::snprintf(
            line, sizeof(line),
            "vehicle[%llu] raw120..140=[%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f]",
            static_cast<unsigned long long>(index),
            static_cast<double>(rawFields[8]), static_cast<double>(rawFields[9]),
            static_cast<double>(rawFields[10]), static_cast<double>(rawFields[11]),
            static_cast<double>(rawFields[12]), static_cast<double>(rawFields[13]),
            static_cast<double>(rawFields[14]), static_cast<double>(rawFields[15]),
            static_cast<double>(rawFields[16]));
        Log(line);
        ++emittedCount;
    }

    char result[224]{};
    std::snprintf(result, sizeof(result),
                  "vehicle diagnostic valid=%llu invalid=%llu emitted=%llu%s",
                  static_cast<unsigned long long>(validCount),
                  static_cast<unsigned long long>(invalidCount),
                  static_cast<unsigned long long>(emittedCount),
                  validCount > emittedCount ? " (entry log capped)" : "");
    Log(result);
}

void NFSMW_CDECL VehicleTickDetour(float dt) {
    VehicleTickFn original =
        g_vehicleTickOriginal.load(std::memory_order_acquire);
    for (unsigned attempt = 0; original == nullptr &&
                              attempt < kHookOriginalWaitAttempts; ++attempt) {
        SwitchToThread();
        original = g_vehicleTickOriginal.load(std::memory_order_acquire);
    }
    if (original == nullptr) {
        if (!g_vehicleDiagnosticChainFailureLogged.exchange(
                true, std::memory_order_relaxed)) {
            Log("vehicle diagnostic skipped: original tick trampoline is unavailable");
        }
        return;
    }

    // Observe the post-update state. No vehicle object or physics field is
    // written, and no game virtual function is called from this detour.
    original(dt);
    vehicle_getter_diagnostic::OnVehicleTick();
    LogVehicleDiagnosticSnapshot(dt);
}

bool InstallVehicleDiagnosticHook(std::uintptr_t validatedTarget) {
    HMODULE module = GetModuleHandleA(nullptr);
    uintptr_t moduleBase = 0;
    std::size_t imageSize = 0;
    if (module == nullptr || !nfsmw_main_module_range(&moduleBase, &imageSize) ||
        moduleBase != reinterpret_cast<std::uintptr_t>(module) ||
        imageSize == 0 ||
        validatedTarget != moduleBase + kActiveComponentsTickRva ||
        !ExecutableAddress(reinterpret_cast<const void*>(validatedTarget))) {
        Log("vehicle diagnostic disabled: validated tick target or image range is invalid");
        return false;
    }

    g_vehicleImageBase.store(moduleBase, std::memory_order_release);
    g_vehicleImageSize.store(imageSize, std::memory_order_release);
    if (!vehicle_getter_diagnostic::Configure(moduleBase, imageSize)) {
        g_vehicleTickTarget.store(0, std::memory_order_release);
        g_vehicleImageBase.store(0, std::memory_order_release);
        g_vehicleImageSize.store(0, std::memory_order_release);
        return false;
    }
    g_vehicleTickTarget.store(validatedTarget, std::memory_order_release);
    auto hook = std::make_unique<nfsmw::InlineHook<VehicleTickFn>>(
        validatedTarget, &VehicleTickDetour);
    const VehicleTickFn original = hook->original();
    if (!hook->installed() || original == nullptr) {
        Log("vehicle diagnostic disabled: MinHook installation failed");
        g_vehicleTickTarget.store(0, std::memory_order_release);
        g_vehicleImageBase.store(0, std::memory_order_release);
        g_vehicleImageSize.store(0, std::memory_order_release);
        return false;
    }

    g_vehicleTickOriginal.store(original, std::memory_order_release);
    g_vehicleTickHook = std::move(hook);
    char message[192]{};
    std::snprintf(message, sizeof(message),
                  "read-only vehicle diagnostic installed at 0x%p (cdecl float)",
                  reinterpret_cast<void*>(validatedTarget));
    Log(message);
    return true;
}

#endif

bool OnDiskFileSizeMatches(HMODULE module) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(
        module, path,
        static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (length == 0 || length >= sizeof(path) / sizeof(path[0])) {
        return false;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    const bool readable = GetFileSizeEx(file, &size) != FALSE &&
                          size.QuadPart >= 0;
    CloseHandle(file);
    return readable &&
           static_cast<unsigned long long>(size.QuadPart) ==
               target_profile::kExpectedFileSize;
}

bool Reject(const char* reason) {
    ::nfsmw_drift_asi::runtime_logging::DebugOutput("[nfsmw_drift_assist] target rejected: ");
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(reason);
    ::nfsmw_drift_asi::runtime_logging::DebugOutput("\n");
    return false;
}

bool ValidatePeProfile(HMODULE module,
                       const IMAGE_NT_HEADERS32*& ntHeaders,
                       std::size_t& imageSize) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    if (module == nullptr || base != target_profile::kExpectedImageBase) {
        return Reject("image base mismatch");
    }
    if (!ReadableRange(module, sizeof(IMAGE_DOS_HEADER))) {
        return Reject("DOS header is not readable");
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0x40 ||
        dos->e_lfanew > 0x00100000) {
        return Reject("invalid DOS header");
    }

    const auto ntAddress = base + static_cast<std::uintptr_t>(dos->e_lfanew);
    if (ntAddress < base ||
        !ReadableRange(reinterpret_cast<const void*>(ntAddress),
                       sizeof(IMAGE_NT_HEADERS32))) {
        return Reject("PE header is not readable");
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(ntAddress);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != target_profile::kExpectedMachine ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.ImageBase != target_profile::kExpectedImageBase ||
        nt->OptionalHeader.AddressOfEntryPoint != target_profile::kExpectedEntryRva ||
        (target_profile::kExpectedSizeOfImage != 0 &&
         nt->OptionalHeader.SizeOfImage != target_profile::kExpectedSizeOfImage)) {
        return Reject("PE32 version profile mismatch");
    }

    imageSize = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
    if (imageSize == 0 ||
        target_profile::kExpectedEntryRva >= imageSize ||
        !ExecutableAddress(reinterpret_cast<const void*>(
            base + target_profile::kExpectedEntryRva))) {
        return Reject("entry point is outside executable image");
    }
    ntHeaders = nt;
    return true;
}

bool RejectAnchor(const char* label, const char* reason) {
    char message[256]{};
    std::snprintf(message, sizeof(message), "%s anchor %s", label, reason);
    return Reject(message);
}

bool FindTextSection(const IMAGE_NT_HEADERS32* ntHeaders,
                     std::uintptr_t moduleBase,
                     std::size_t imageSize,
                     std::uintptr_t* textRvaOut,
                     std::size_t* textSizeOut) {
    if (ntHeaders == nullptr || moduleBase == 0 || imageSize == 0 ||
        textRvaOut == nullptr || textSizeOut == nullptr ||
        ntHeaders->FileHeader.NumberOfSections == 0 ||
        ntHeaders->FileHeader.NumberOfSections > 96) {
        return false;
    }
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(ntHeaders);
    const std::size_t sectionBytes =
        static_cast<std::size_t>(ntHeaders->FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    const std::uintptr_t sectionAddress =
        reinterpret_cast<std::uintptr_t>(sections);
    if (sectionAddress < moduleBase ||
        sectionAddress - moduleBase > imageSize ||
        sectionBytes > imageSize - (sectionAddress - moduleBase) ||
        !ReadableRange(sections, sectionBytes)) {
        return false;
    }
    for (unsigned index = 0; index < ntHeaders->FileHeader.NumberOfSections;
         ++index) {
        if (std::memcmp(sections[index].Name, ".text", 5) != 0) {
            continue;
        }
        const std::size_t virtualSize =
            static_cast<std::size_t>(sections[index].Misc.VirtualSize);
        const std::size_t rawSize =
            static_cast<std::size_t>(sections[index].SizeOfRawData);
        const std::size_t span = virtualSize > rawSize ? virtualSize : rawSize;
        const std::size_t rva =
            static_cast<std::size_t>(sections[index].VirtualAddress);
        if (span == 0 || rva >= imageSize || span > imageSize - rva) {
            return false;
        }
        *textRvaOut = static_cast<std::uintptr_t>(rva);
        *textSizeOut = span;
        return true;
    }
    return false;
}

bool ValidateUniqueAnchor(HMODULE module,
                          std::size_t imageSize,
                          std::uintptr_t textRva,
                          std::size_t textSize,
                          std::uintptr_t signatureRva,
                          const char* signature,
                          const char* label,
                          std::uintptr_t* validatedTargetOut) {
    // A zero RVA or empty pattern is intentionally a hard refusal. Every
    // phase anchor is scanned across the full image, must be unique, and must
    // resolve to the configured address inside the executable .text section.
    if (signatureRva == 0 || signature == nullptr || signature[0] == '\0') {
        return RejectAnchor(label, "signature is not configured");
    }
    if (signatureRva >= imageSize || signatureRva < textRva ||
        signatureRva - textRva >= textSize) {
        return RejectAnchor(label, "RVA is outside .text");
    }

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
    const std::uintptr_t expected = base + signatureRva;
    if (expected < base ||
        !ExecutableAddress(reinterpret_cast<const void*>(expected))) {
        return RejectAnchor(label, "address is not executable");
    }

    std::uintptr_t moduleBase = 0;
    std::size_t moduleLength = 0;
    if (!nfsmw_main_module_range(&moduleBase, &moduleLength) ||
        moduleBase != base || moduleLength != imageSize ||
        moduleLength > static_cast<std::size_t>(UINTPTR_MAX - moduleBase)) {
        return RejectAnchor(label, "module range changed during validation");
    }

    std::size_t matchCount = 0;
    bool expectedFound = false;
    std::uintptr_t cursor = moduleBase;
    const std::uintptr_t end = moduleBase + moduleLength;
    while (cursor < end) {
        const std::uintptr_t match = nfsmw_aob_scan_range(
            cursor, static_cast<std::size_t>(end - cursor), signature);
        if (match == 0 || match < cursor || match >= end) {
            break;
        }
        ++matchCount;
        expectedFound = expectedFound || match == expected;
        if (match == UINTPTR_MAX) {
            break;
        }
        cursor = match + 1;
    }
    if (matchCount != 1 || !expectedFound) {
        return RejectAnchor(label, "signature is missing, shifted, or duplicated");
    }
    if (validatedTargetOut != nullptr) {
        *validatedTargetOut = expected;
    }
    return true;
}

#if NFSMW_ENABLE_ALPHA_BRIDGE
constexpr std::uintptr_t kRigidBodyVtableRva = 0x004AC880u;
constexpr std::size_t kSetAngularVelocityVtableSlot = 25;

bool SafeReadProfilePointer(const void* address, std::uintptr_t* value) {
    if (address == nullptr || value == nullptr ||
        !ReadableRange(address, sizeof(*value))) {
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

bool RangeIsInReadOnlyDataSection(const IMAGE_NT_HEADERS32* ntHeaders,
                                  std::uintptr_t moduleBase,
                                  std::size_t imageSize,
                                  std::uintptr_t address,
                                  std::size_t size) {
    if (ntHeaders == nullptr || moduleBase == 0 || imageSize == 0 ||
        address < moduleBase || size == 0 ||
        address - moduleBase > imageSize ||
        size > imageSize - (address - moduleBase)) {
        return false;
    }
    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(ntHeaders);
    const std::size_t sectionBytes =
        static_cast<std::size_t>(ntHeaders->FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (!ReadableRange(sections, sectionBytes)) {
        return false;
    }
    const std::uintptr_t rva = address - moduleBase;
    for (unsigned index = 0; index < ntHeaders->FileHeader.NumberOfSections;
         ++index) {
        const std::size_t virtualSize =
            static_cast<std::size_t>(sections[index].Misc.VirtualSize);
        const std::size_t rawSize =
            static_cast<std::size_t>(sections[index].SizeOfRawData);
        const std::size_t span = virtualSize > rawSize ? virtualSize : rawSize;
        const std::uintptr_t sectionRva = sections[index].VirtualAddress;
        const DWORD flags = sections[index].Characteristics;
        if (span == 0 || rva < sectionRva || rva - sectionRva > span ||
            size > span - static_cast<std::size_t>(rva - sectionRva)) {
            continue;
        }
        return (flags & IMAGE_SCN_MEM_READ) != 0 &&
               (flags & IMAGE_SCN_MEM_WRITE) == 0 &&
               (flags & IMAGE_SCN_MEM_EXECUTE) == 0 &&
               ReadableRange(reinterpret_cast<const void*>(address), size);
    }
    return false;
}

bool ValidateAngularVelocitySetterContract(
    HMODULE module,
    const IMAGE_NT_HEADERS32* ntHeaders,
    std::size_t imageSize,
    std::uintptr_t validatedSetter) {
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
    const std::uintptr_t vtable = base + kRigidBodyVtableRva;
    const std::size_t vtableBytes =
        (kSetAngularVelocityVtableSlot + 1) * sizeof(std::uintptr_t);
    if (validatedSetter == 0 || validatedSetter < base ||
        validatedSetter - base !=
            target_profile::kAngularVelocitySetterRva ||
        !RangeIsInReadOnlyDataSection(ntHeaders, base, imageSize, vtable,
                                      vtableBytes)) {
        return Reject("rigid-body setter vtable is outside read-only image data");
    }
    std::uintptr_t slotAddress = 0;
    if (vtable > UINTPTR_MAX -
                     kSetAngularVelocityVtableSlot * sizeof(std::uintptr_t)) {
        return Reject("rigid-body setter slot address overflowed");
    }
    slotAddress = vtable +
                  kSetAngularVelocityVtableSlot * sizeof(std::uintptr_t);
    std::uintptr_t slotTarget = 0;
    if (!SafeReadProfilePointer(reinterpret_cast<const void*>(slotAddress),
                                &slotTarget) ||
        slotTarget != validatedSetter) {
        return Reject("rigid-body vtable slot 25 does not match the validated setter");
    }
    return true;
}
#endif

bool ValidateTarget(std::uintptr_t* validatedTargetOut,
                    std::uintptr_t* validatedPhaseWorldTargetOut,
                    std::uintptr_t* validatedAngularVelocitySetterOut,
                    std::uintptr_t* validatedTextStartOut,
                    std::size_t* validatedTextSizeOut) {
    if (validatedTextStartOut == nullptr ||
        validatedTextSizeOut == nullptr) {
        return Reject("validated text-range output is unavailable");
    }
    *validatedTextStartOut = 0;
    *validatedTextSizeOut = 0;
    HMODULE module = GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS32* ntHeaders = nullptr;
    std::size_t imageSize = 0;
    if (!ValidatePeProfile(module, ntHeaders, imageSize)) {
        return false;
    }
    if (!OnDiskFileSizeMatches(module)) {
        return Reject("on-disk Reforged executable size mismatch");
    }
    std::uintptr_t textRva = 0;
    std::size_t textSize = 0;
    if (!FindTextSection(ntHeaders, reinterpret_cast<std::uintptr_t>(module),
                         imageSize, &textRva, &textSize)) {
        return Reject(".text section could not be validated");
    }
    std::uintptr_t textStart = 0;
    const std::uintptr_t moduleBase =
        reinterpret_cast<std::uintptr_t>(module);
    if (textRva >
        std::numeric_limits<std::uintptr_t>::max() - moduleBase) {
        return Reject(".text section address overflow");
    }
    textStart = moduleBase + textRva;
    if (!ValidateUniqueAnchor(
            module, imageSize, textRva, textSize,
            target_profile::kRequiredSignatureRva,
            target_profile::kRequiredSignature, "primary",
            validatedTargetOut)) {
        return false;
    }
#if NFSMW_ENABLE_PHASE_DIAGNOSTIC || NFSMW_ENABLE_ALPHA_BRIDGE
    if (!ValidateUniqueAnchor(
            module, imageSize, textRva, textSize,
            target_profile::kPhaseWorldSignatureRva,
            target_profile::kPhaseWorldSignature, "phase-world",
            validatedPhaseWorldTargetOut)) {
        return false;
    }
    if (validatedTargetOut != nullptr &&
        validatedPhaseWorldTargetOut != nullptr &&
        *validatedTargetOut == *validatedPhaseWorldTargetOut) {
        return Reject("two-anchor targets unexpectedly alias");
    }
#else
    if (validatedPhaseWorldTargetOut != nullptr) {
        *validatedPhaseWorldTargetOut = 0;
    }
#endif
#if NFSMW_ENABLE_ALPHA_BRIDGE
    if (!ValidateUniqueAnchor(
            module, imageSize, textRva, textSize,
            target_profile::kAngularVelocitySetterRva,
            target_profile::kAngularVelocitySetterSignature,
            "angular-velocity-setter",
            validatedAngularVelocitySetterOut)) {
        return false;
    }
    if (validatedAngularVelocitySetterOut == nullptr ||
        !ValidateAngularVelocitySetterContract(
            module, ntHeaders, imageSize,
            *validatedAngularVelocitySetterOut)) {
        return false;
    }
    if ((validatedTargetOut != nullptr &&
         *validatedAngularVelocitySetterOut == *validatedTargetOut) ||
        (validatedPhaseWorldTargetOut != nullptr &&
         *validatedAngularVelocitySetterOut ==
             *validatedPhaseWorldTargetOut)) {
        return Reject("validated alpha targets unexpectedly alias");
    }
#else
    if (validatedAngularVelocitySetterOut != nullptr) {
        *validatedAngularVelocitySetterOut = 0;
    }
#endif
    *validatedTextStartOut = textStart;
    *validatedTextSizeOut = textSize;
    return true;
}

} // namespace
} // namespace nfsmw_drift_asi

NFSMW_PLUGIN_DECLARE("Slippery_Drifting_FlashFish_by_AndyQinke",
                     "1.2.1", "AndyQinke")

NFSMW_PLUGIN_MAIN() {
    namespace startup_gate =
        nfsmw_drift::asi_host::reforged_startup_gate;
    startup_gate::BindingResult binding{};
    char gateReason[512]{};
    std::wstring verifiedExecutablePath;
    // The file/footer gate must complete before any in-memory profile check,
    // and the device file is intentionally deferred until every runtime
    // anchor has passed.  This prevents an unsupported process from leaving a
    // plausible-looking binding behind.
    if (!startup_gate::ValidateCurrentProcess(
            &verifiedExecutablePath, gateReason, sizeof(gateReason))) {
        ::nfsmw_drift_asi::runtime_logging::DebugOutput("[nfsmw_drift_assist] startup gate rejected: ");
        ::nfsmw_drift_asi::runtime_logging::DebugOutput(gateReason[0] != '\0' ? gateReason
                                                : "unspecified failure");
        ::nfsmw_drift_asi::runtime_logging::DebugOutput(
            "; no configuration was loaded and no hooks were installed\n");
        return NFSMW_FAIL;
    }
    std::uintptr_t validatedTarget = 0;
    std::uintptr_t validatedPhaseWorldTarget = 0;
    std::uintptr_t validatedAngularVelocitySetter = 0;
    std::uintptr_t validatedTextStart = 0;
    std::size_t validatedTextSize = 0;
    if (!nfsmw_drift_asi::ValidateTarget(
            &validatedTarget, &validatedPhaseWorldTarget,
            &validatedAngularVelocitySetter, &validatedTextStart,
            &validatedTextSize)) {
        // No input, attitude, tire, or drivetrain hook is installed on a
        // rejected profile.  Returning NFSMW_FAIL keeps the loader diagnostic
        // meaningful while the SDK entry shim remains one-shot.
        return NFSMW_FAIL;
    }

    // Re-read the on-disk image immediately before creating or inspecting the
    // binding.  The process image is immutable for normal launches, but this
    // second check closes the practical path-replacement window between the
    // first file validation and the runtime profile inspection.
    if (!startup_gate::ValidateExecutable(
            verifiedExecutablePath.c_str(), gateReason, sizeof(gateReason)) ||
        !startup_gate::EnsureCurrentDeviceBinding(
            verifiedExecutablePath.c_str(), &binding, gateReason,
            sizeof(gateReason))) {
        ::nfsmw_drift_asi::runtime_logging::DebugOutput("[nfsmw_drift_assist] device-binding gate rejected: ");
        ::nfsmw_drift_asi::runtime_logging::DebugOutput(gateReason[0] != '\0' ? gateReason
                                                 : "unspecified failure");
        ::nfsmw_drift_asi::runtime_logging::DebugOutput(
            "; no configuration was loaded and no hooks were installed\n");
        return NFSMW_FAIL;
    }
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(
        binding.status == startup_gate::BindingStatus::Created
            ? "[nfsmw_drift_assist] Reforged executable verified; encrypted device binding created\n"
            : "[nfsmw_drift_assist] Reforged executable and encrypted device binding verified\n");

    // Configuration is loaded only after the executable profile has passed.
    // The controller remains fail-closed until a separately verified vehicle
    // bridge supplies VehicleState samples and installs its hooks.
    const bool controllerEnabled = nfsmw_drift_asi::LoadControllerConfig();
    (void)controllerEnabled;

#if NFSMW_ENABLE_INPUT_DIAGNOSTIC
    if (!nfsmw_drift_asi::InstallInputDiagnosticHook(validatedTarget)) {
        // Pre-enable failures leave no detour behind and may fail the load.
        return NFSMW_FAIL;
    }
    // A handled post-Apply failure also reaches here with sampling disarmed;
    // returning NFSMW_OK keeps any retained pass-through detour resident.
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(
        "[nfsmw_drift_assist] input diagnostic is read-only; no game values are written\n");
#elif NFSMW_ENABLE_VEHICLE_DIAGNOSTIC
    if (!nfsmw_drift_asi::InstallVehicleDiagnosticHook(validatedTarget)) {
        // A diagnostic failure must leave the game untouched. The target
        // profile remains fail-closed when MinHook cannot be installed.
        return NFSMW_FAIL;
    }
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(
        "[nfsmw_drift_assist] vehicle diagnostic is read-only; no game values are written\n");
#elif NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC
    if (!nfsmw_drift_asi::coordinator_diagnostic::Install(validatedTarget)) {
        // A diagnostic failure must leave the game untouched. The target
        // profile remains fail-closed when MinHook cannot be installed.
        return NFSMW_FAIL;
    }
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(
        "[nfsmw_drift_assist] coordinator diagnostic is read-only; no game values are written\n");
#elif NFSMW_ENABLE_PHASE_DIAGNOSTIC
    if (!nfsmw_drift_asi::phase_timing_diagnostic::Install(
            validatedTarget, validatedPhaseWorldTarget)) {
        // The two hooks are one diagnostic unit. Install() rolls both back if
        // either MinHook operation or the defensive layout check fails.
        return NFSMW_FAIL;
    }
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(
        "[nfsmw_drift_assist] phase timing diagnostic is read-only; no game values are written\n");
#elif NFSMW_ENABLE_ALPHA_BRIDGE
    if (!controllerEnabled ||
        !nfsmw_drift_asi::alpha_bridge::Install(
            validatedTarget, validatedPhaseWorldTarget,
            validatedAngularVelocitySetter, validatedTextStart,
            validatedTextSize,
            &nfsmw_drift_asi::g_controller,
            &nfsmw_drift_asi::g_vehicleCountersteerMultipliers,
            &nfsmw_drift_asi::g_rearWheelSteeringVehicles)) {
        return NFSMW_FAIL;
    }
    if (!nfsmw_drift_asi::alpha_bridge::IsArmed()) {
        ::nfsmw_drift_asi::runtime_logging::DebugOutput(
            "[nfsmw_drift_assist] functional alpha is disarmed; retained hooks are pass-through only\n");
        return NFSMW_OK;
    }
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(
        "[nfsmw_drift_assist] Slippery_Drifting_FlashFish_by_AndyQinke 1.2.1 is active with 15-degree activation, "
         "per-car-scaled 55-degree base countersteer, restored 0.9.5 automatic execution, immediate "
         "any-direction manual ownership arbitration, "
        "countersteer execution, and restored distance-proportional 0.9.5 manual steering response, plus a 75-percent "
        "manual-yaw envelope with a two-second same-side ramp and bounded opposite-side "
        "recovery, 60-sample initial and 3-sample recovery probation, an inverted three-degree "
        "camera with a 2.0-second entry and 1.25-second return or side changes, pendulum side "
        "transitions, and enabled-slot-aware player-vehicle qualification; front-grip and "
        "driven-wheel-power writers remain disabled; the integrated rigid-body acceleration "
         "writer uses a 5.25 m/s^2 nominal target at a 61.2-percent release scale, with "
         "12.5-percent applied-wheel-angle countersteer deflection, total-speed attenuation, and strict "
        "before/after verification; a continuous 0.2-second handbrake hold selects higher-priority "
        "velocity-opposed deceleration, rising linearly from 15 percent at 100 km/h to a maximum "
        "1.15 times the acceleration strength at 180 km/h; non-drift driving assistance is active by default, "
        "suspends during drift, and returns 0.2 seconds after drift with half-second delayed fading ABS, "
        "gentle over-80-km/h straight-line ESC and angle-scaled recovery ESC, probabilistic gear-aware TCS, "
        "a three-second active-drift wheel-contact grace window, and a verified D3D9 ABS/ESC/TCS/LC HUD below the tachometer that preserves its speed/reverse/LC gates and also follows native race-HUD visibility\n");
#else
    (void)validatedTarget;
    (void)validatedPhaseWorldTarget;
    (void)validatedAngularVelocitySetter;
#endif

#if NFSMW_ENABLE_ALPHA_BRIDGE
    ::nfsmw_drift_asi::runtime_logging::DebugOutput("[nfsmw_drift_assist] profile accepted; integrated Slippery_Drifting_FlashFish_by_AndyQinke bridge installed\n");
#else
    ::nfsmw_drift_asi::runtime_logging::DebugOutput("[nfsmw_drift_assist] profile accepted; game bridge is not installed\n");
#endif
    return NFSMW_OK;
}
