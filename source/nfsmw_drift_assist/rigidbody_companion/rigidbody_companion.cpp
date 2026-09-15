#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <nfsmw_sdk/nfsmw_sdk.h>

#include "asi_host/rigidbody_accel_experiment.hpp"
#include "drift_assist.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>

namespace nfsmw_drift_asi::rigidbody_companion {
namespace {

static_assert(sizeof(void*) == 4,
              "The rigid-body companion must be built as a 32-bit PE DLL");

using Vec3 = nfsmw_drift::Vec3;
using AccelerateFn = void (NFSMW_THISCALL*)(
    void* self, const Vec3* distribution, float amount);
using SimableGetPlayerFn = void* (NFSMW_THISCALL*)(void* self);
using SimablePredicateFn = bool (NFSMW_THISCALL*)(void* self);
using PlayerGetSimableFn = void* (NFSMW_THISCALL*)(void* self);

// This companion is intentionally pinned to the same measured speed.exe
// profile as the historical 0.9.5 ASI. It never patches the 0.9.5 image.
constexpr std::uintptr_t kExpectedImageBase = 0x00400000u;
constexpr std::uintptr_t kPVehicleInstancesRva = 0x005352B0u;
constexpr std::uintptr_t kPVehiclePrimaryVtableRva = 0x004AA9D8u;
constexpr std::uintptr_t kPVehicleSimableVtableRva = 0x004AA940u;
constexpr std::uintptr_t kRigidBodyVtableRva = 0x004AC880u;
constexpr std::uintptr_t kAccelerateRva = 0x00299E10u;
constexpr std::uintptr_t kSimableGetPlayerRva = 0x00286A20u;
constexpr std::uintptr_t kSimableIsPlayerRva = 0x00286A10u;
constexpr std::uintptr_t kSimableIsOwnedByPlayerRva = 0x00276FD0u;
constexpr std::uintptr_t kPlayerVtableRva = 0x004B0B10u;
constexpr std::uintptr_t kPlayerGetSimableRva = 0x002F8FB0u;

constexpr std::size_t kPVehicleInstanceLimit = 64;
constexpr std::size_t kPVehicleInstanceStride = 8;
constexpr std::size_t kPVehicleInstanceEnabledOffset = 4;
constexpr std::size_t kPVehicleReadableSize = 0x160;
constexpr std::size_t kPVehicleSimableOffset = 0x2C;
constexpr std::size_t kPVehicleDirtyOffset = 0x48;
constexpr std::size_t kPVehicleObjectTypeOffset = 0x60;
constexpr std::size_t kPVehicleRigidBodyOffset = 0x78;
constexpr std::size_t kPVehiclePlayerOffset = 0x84;
constexpr std::size_t kPVehicleSpeedOffset = 0x11C;
constexpr std::size_t kPVehicleSlipAngleOffset = 0x128;
constexpr std::size_t kPVehicleGroundedOffset = 0x130;
constexpr std::size_t kPVehicleInputOffset = 0xE8;

constexpr std::size_t kRigidBodyHolderOffset = 0x30;
constexpr std::size_t kRigidBodyModeOffset = 0x5D;
constexpr std::size_t kRigidBodyLinearVelocityOffset = 0x20;
constexpr std::size_t kRigidBodyRightVectorOffset = 0x70;
constexpr std::size_t kRigidBodyUpVectorOffset = 0x80;
constexpr std::size_t kRigidBodyForwardVectorOffset = 0x90;
constexpr std::size_t kRigidBodyAccelerateVtableSlot = 39;
constexpr std::size_t kSimableGetPlayerVtableSlot = 8;
constexpr std::size_t kSimableIsPlayerVtableSlot = 9;
constexpr std::size_t kSimableIsOwnedByPlayerVtableSlot = 10;
constexpr std::size_t kPlayerGetSimableVtableSlot = 1;
constexpr std::size_t kPInputControlsOffset = 0x50;
constexpr std::size_t kInputSteeringOffset = 0x04;
constexpr std::size_t kInputGasOffset = 0x14;
constexpr std::size_t kInputBrakeOffset = 0x18;
constexpr std::size_t kInputHandbrakeOffset = 0x1C;

constexpr std::uint8_t kAccelerateSignature[] = {
    0x8B, 0x41, 0x30, 0x8B, 0x00, 0x8A, 0x48, 0x5D,
    0x84, 0xC9, 0x75, 0x2A};

constexpr float kHandbrakeActivation = 0.50f;
constexpr float kMinimumDriftSpeedMps = 8.0f;
constexpr float kMinimumDriftAngleRad = nfsmw_drift::DegToRad(11.0f);
constexpr float kReleaseDriftAngleRad = nfsmw_drift::DegToRad(7.0f);
constexpr float kMinimumThrottle = 0.05f;
constexpr float kMaximumBrake = 0.05f;
constexpr float kInjectionIntervalSeconds = 0.008f;
constexpr float kDefaultDt = 1.0f / 60.0f;
constexpr float kMaximumDt = 0.05f;
constexpr double kSessionReleaseSeconds = 0.35;
constexpr DWORD kTelemetryIntervalMs = 500;
constexpr DWORD kFailureIntervalMs = 1000;

struct Identity {
    std::uintptr_t pvehicle = 0;
    std::uintptr_t player = 0;
    std::uintptr_t simable = 0;
    std::uintptr_t rigidBody = 0;
    std::uintptr_t holder = 0;
    std::uintptr_t inner = 0;
};

// The vehicle table can contain more than one slot referring to the same
// player object during a transition.  Those exact duplicates are harmless;
// a second distinct ownership tuple is still ambiguous and must be rejected.
bool SameOwnershipIdentity(const Identity& a, const Identity& b) {
    return a.pvehicle != 0 && a.pvehicle == b.pvehicle &&
           a.player == b.player && a.simable == b.simable &&
           a.rigidBody == b.rigidBody;
}

struct Candidate {
    Identity identity{};
    std::uint32_t grounded = 0;
};

struct Snapshot {
    Candidate candidate{};
    Vec3 right{};
    Vec3 up{};
    Vec3 forward{};
    Vec3 velocity{};
    float speedMps = 0.0f;
    float signedDriftAngleRad = 0.0f;
    float steering = 0.0f;
    float throttle = 0.0f;
    float brake = 0.0f;
    float handbrake = 0.0f;
};

struct SessionState {
    Identity identity{};
    bool active = false;
    float ramp = 0.0f;
    std::int64_t lastQpc = 0;
    std::int64_t lastInjectionQpc = 0;
    std::int64_t lastDriftQpc = 0;
};

struct CandidateCache {
    Candidate candidate{};
    bool valid = false;
    std::int64_t lastScanQpc = 0;
};

HMODULE g_selfModule = nullptr;
std::atomic<bool> g_started{false};
std::atomic<bool> g_hookInstalled{false};
std::atomic<bool> g_writerDisabled{false};
std::atomic<AccelerateFn> g_originalAccelerate{nullptr};
std::uintptr_t g_gameBase = 0;
std::int64_t g_qpcFrequency = 0;
SRWLOCK g_stateLock = SRWLOCK_INIT;
SessionState g_session{};
SRWLOCK g_candidateLock = SRWLOCK_INIT;
CandidateCache g_candidateCache{};
std::atomic<DWORD> g_lastTelemetryTick{0};
std::atomic<DWORD> g_lastFailureTick{0};
thread_local bool g_accelerateInFlight = false;

// Accelerate is a shared IRigidBody virtual entry and is called for traffic,
// world props, and the player vehicle.  Rescanning the fixed vehicle pool for
// every non-player call is needlessly expensive, so keep the last verified
// player candidate for a short interval.  The body/pvehicle links are still
// checked on every fast-path use, and a changed body or expired entry forces a
// fresh ownership scan.
constexpr double kCandidateRescanSeconds = 0.10;
constexpr DWORD kInitializationRetryWindowMs = 30000;
constexpr DWORD kInitializationRetryIntervalMs = 100;

void LogV(const char* format, va_list args) {
    if (format == nullptr) {
        return;
    }
    char message[1200]{};
    std::vsnprintf(message, sizeof(message), format, args);
    char line[1280]{};
    std::snprintf(line, sizeof(line), "[nfsmw_drift_rigidbody] %s\n", message);
    OutputDebugStringA(line);
}

void Log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    LogV(format, args);
    va_end(args);
}

void LogFailure(const char* format, ...) {
    const DWORD now = GetTickCount();
    DWORD previous = g_lastFailureTick.load(std::memory_order_relaxed);
    if (previous != 0 &&
        static_cast<DWORD>(now - previous) < kFailureIntervalMs) {
        return;
    }
    if (!g_lastFailureTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }
    va_list args;
    va_start(args, format);
    LogV(format, args);
    va_end(args);
}

bool AddAddress(std::uintptr_t base, std::size_t offset,
                std::uintptr_t* result) {
    if (result == nullptr || base > UINTPTR_MAX - offset) {
        return false;
    }
    *result = base + offset;
    return true;
}

bool ReadableRange(const void* address, std::size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(address);
    if (start > UINTPTR_MAX - (size - 1)) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT) {
        return false;
    }
    const DWORD protection = info.Protect & 0xFFu;
    if (protection == PAGE_NOACCESS || protection == PAGE_GUARD) {
        return false;
    }
    const std::uintptr_t regionStart =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    if (regionStart > UINTPTR_MAX - info.RegionSize) {
        return false;
    }
    const std::uintptr_t regionEnd = regionStart + info.RegionSize;
    return start >= regionStart && start + size <= regionEnd;
}

template <typename T>
bool SafeRead(const void* address, T* value) {
    if (value == nullptr || !ReadableRange(address, sizeof(T))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        *value = *reinterpret_cast<const T*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    *value = *reinterpret_cast<const T*>(address);
#endif
    return true;
}

template <typename T>
bool SafeRead(std::uintptr_t address, T* value) {
    return SafeRead(reinterpret_cast<const void*>(address), value);
}

bool SafeReadPointer(std::uintptr_t address, std::uintptr_t* value) {
    return SafeRead(reinterpret_cast<const void*>(address), value);
}

bool SafeReadFloat(std::uintptr_t address, float* value) {
    return SafeRead(reinterpret_cast<const void*>(address), value) &&
           std::isfinite(*value);
}

bool SafeReadVec3(std::uintptr_t address, Vec3* value) {
    return value != nullptr && SafeReadFloat(address, &value->x) &&
           SafeReadFloat(address + sizeof(float), &value->y) &&
           SafeReadFloat(address + 2 * sizeof(float), &value->z);
}

bool SafeReadBytes(std::uintptr_t address, const std::uint8_t* expected,
                   std::size_t size) {
    if (expected == nullptr || !ReadableRange(reinterpret_cast<const void*>(address),
                                               size)) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        return std::memcmp(reinterpret_cast<const void*>(address), expected,
                           size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return std::memcmp(reinterpret_cast<const void*>(address), expected, size) ==
           0;
#endif
}

bool SafeWritePointer(std::uintptr_t address, std::uintptr_t value) {
    if (!ReadableRange(reinterpret_cast<const void*>(address),
                       sizeof(std::uintptr_t))) {
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(value),
                        PAGE_READWRITE, &oldProtection)) {
        return false;
    }
    *reinterpret_cast<std::uintptr_t*>(address) = value;
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(address), sizeof(value),
                   oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address),
                          sizeof(value));
    std::uintptr_t readBack = 0;
    return SafeReadPointer(address, &readBack) && readBack == value;
}

bool ExecutableAddress(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info,
                     sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT) {
        return false;
    }
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

float VecLength(Vec3 value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

bool FiniteBoundedVec3(Vec3 value, float maximum) {
    const float length = VecLength(value);
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(length) &&
           length <= maximum;
}

Vec3 Normalize(Vec3 value, Vec3 fallback) {
    const float length = VecLength(value);
    if (!std::isfinite(length) || length < 1.0e-5f) {
        return fallback;
    }
    return {value.x / length, value.y / length, value.z / length};
}

float DotLocal(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 CrossLocal(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

double QpcSeconds(std::int64_t ticks) {
    if (g_qpcFrequency <= 0) {
        return 0.0;
    }
    return static_cast<double>(ticks) /
           static_cast<double>(g_qpcFrequency);
}

bool ReadGameProfile(std::uintptr_t* baseOut) {
    HMODULE module = GetModuleHandleA(nullptr);
    if (module == nullptr ||
        reinterpret_cast<std::uintptr_t>(module) != kExpectedImageBase) {
        LogFailure("profile rejected: speed.exe image base is not 0x00400000");
        return false;
    }
    char path[MAX_PATH]{};
    const DWORD pathLength =
        GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
    if (pathLength < 9 || pathLength >= sizeof(path)) {
        LogFailure("profile rejected: speed.exe path is unavailable");
        return false;
    }
    const char* tail = path + pathLength - 9;
    const char expectedName[] = "speed.exe";
    for (std::size_t i = 0; i < 9; ++i) {
        char actual = tail[i];
        if (actual >= 'A' && actual <= 'Z') {
            actual = static_cast<char>(actual - 'A' + 'a');
        }
        if (actual != expectedName[i]) {
            LogFailure("profile rejected: main module is not speed.exe");
            return false;
        }
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    std::int32_t ntOffset = 0;
    if (!SafeRead(reinterpret_cast<const char*>(module) + 0x3C, &ntOffset) ||
        ntOffset <= 0 || ntOffset > 0x1000) {
        LogFailure("profile rejected: DOS header is unreadable");
        return false;
    }
    (void)dos;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        reinterpret_cast<const char*>(module) + ntOffset);
    std::uint32_t signature = 0;
    std::uint16_t machine = 0;
    std::uint16_t optionalMagic = 0;
    std::uint32_t imageSize = 0;
    if (!SafeRead(&nt->Signature, &signature) ||
        !SafeRead(&nt->FileHeader.Machine, &machine) ||
        !SafeRead(&nt->OptionalHeader.Magic, &optionalMagic) ||
        !SafeRead(&nt->OptionalHeader.SizeOfImage, &imageSize) ||
        signature != IMAGE_NT_SIGNATURE || machine != IMAGE_FILE_MACHINE_I386 ||
        optionalMagic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || imageSize == 0) {
        LogFailure("profile rejected: PE32 header contract failed");
        return false;
    }

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
    const std::uintptr_t accelerate = base + kAccelerateRva;
    if (!ExecutableAddress(accelerate) ||
        !SafeReadBytes(accelerate, kAccelerateSignature,
                       sizeof(kAccelerateSignature))) {
        LogFailure("profile rejected: IRigidBody::Accelerate signature mismatch");
        return false;
    }
    if (baseOut != nullptr) {
        *baseOut = base;
    }
    return true;
}

bool ValidateMethod(std::uintptr_t vtable, std::size_t slot,
                    std::uintptr_t expected) {
    std::uintptr_t slotAddress = 0;
    std::uintptr_t target = 0;
    return AddAddress(vtable, slot * sizeof(std::uintptr_t), &slotAddress) &&
           SafeReadPointer(slotAddress, &target) && target == expected &&
           ExecutableAddress(target);
}

bool ReadPlayerOwnership(std::uintptr_t pvehicle, Identity* identity) {
    if (identity == nullptr) {
        return false;
    }
    *identity = {};
    std::uintptr_t simable = 0;
    std::uintptr_t player = 0;
    if (!AddAddress(pvehicle, kPVehicleSimableOffset, &simable) ||
        !SafeReadPointer(simable, &simable) || simable == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(simable), sizeof(void*))) {
        return false;
    }
    std::uintptr_t simableVtable = 0;
    if (!SafeReadPointer(simable, &simableVtable) ||
        simableVtable != g_gameBase + kPVehicleSimableVtableRva) {
        return false;
    }
    if (!ValidateMethod(simableVtable, kSimableGetPlayerVtableSlot,
                        g_gameBase + kSimableGetPlayerRva) ||
        !ValidateMethod(simableVtable, kSimableIsPlayerVtableSlot,
                        g_gameBase + kSimableIsPlayerRva) ||
        !ValidateMethod(simableVtable, kSimableIsOwnedByPlayerVtableSlot,
                        g_gameBase + kSimableIsOwnedByPlayerRva)) {
        return false;
    }
    std::uintptr_t getPlayerAddress = 0;
    std::uintptr_t isPlayerAddress = 0;
    std::uintptr_t isOwnedAddress = 0;
    if (!AddAddress(simableVtable,
                    kSimableGetPlayerVtableSlot * sizeof(std::uintptr_t),
                    &getPlayerAddress) ||
        !AddAddress(simableVtable,
                    kSimableIsPlayerVtableSlot * sizeof(std::uintptr_t),
                    &isPlayerAddress) ||
        !AddAddress(simableVtable,
                    kSimableIsOwnedByPlayerVtableSlot * sizeof(std::uintptr_t),
                    &isOwnedAddress)) {
        return false;
    }
    SimableGetPlayerFn getPlayer = nullptr;
    SimablePredicateFn isPlayer = nullptr;
    SimablePredicateFn isOwned = nullptr;
    if (!SafeReadPointer(getPlayerAddress,
                         reinterpret_cast<std::uintptr_t*>(&getPlayer)) ||
        !SafeReadPointer(isPlayerAddress,
                         reinterpret_cast<std::uintptr_t*>(&isPlayer)) ||
        !SafeReadPointer(isOwnedAddress,
                         reinterpret_cast<std::uintptr_t*>(&isOwned))) {
        return false;
    }
    bool playerPredicate = false;
    bool ownedPredicate = false;
#if defined(_MSC_VER)
    __try {
        player = reinterpret_cast<std::uintptr_t>(getPlayer(
            reinterpret_cast<void*>(simable)));
        playerPredicate = isPlayer(reinterpret_cast<void*>(simable));
        ownedPredicate = isOwned(reinterpret_cast<void*>(simable));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    player = reinterpret_cast<std::uintptr_t>(
        getPlayer(reinterpret_cast<void*>(simable)));
    playerPredicate = isPlayer(reinterpret_cast<void*>(simable));
    ownedPredicate = isOwned(reinterpret_cast<void*>(simable));
#endif
    std::uintptr_t reportedPlayer = 0;
    if (!SafeReadPointer(pvehicle + kPVehiclePlayerOffset, &reportedPlayer) ||
        reportedPlayer == 0 || player == 0 || player != reportedPlayer ||
        !playerPredicate || !ownedPredicate) {
        return false;
    }

    std::uintptr_t playerVtable = 0;
    if (!SafeReadPointer(player, &playerVtable) ||
        playerVtable != g_gameBase + kPlayerVtableRva ||
        !ValidateMethod(playerVtable, kPlayerGetSimableVtableSlot,
                        g_gameBase + kPlayerGetSimableRva)) {
        return false;
    }
    std::uintptr_t playerGetSimableAddress = 0;
    if (!AddAddress(playerVtable,
                    kPlayerGetSimableVtableSlot * sizeof(std::uintptr_t),
                    &playerGetSimableAddress)) {
        return false;
    }
    PlayerGetSimableFn playerGetSimable = nullptr;
    if (!SafeReadPointer(
            playerGetSimableAddress,
            reinterpret_cast<std::uintptr_t*>(&playerGetSimable))) {
        return false;
    }
    void* reverseSimable = nullptr;
#if defined(_MSC_VER)
    __try {
        reverseSimable = playerGetSimable(reinterpret_cast<void*>(player));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    reverseSimable = playerGetSimable(reinterpret_cast<void*>(player));
#endif
    if (reinterpret_cast<std::uintptr_t>(reverseSimable) != simable) {
        return false;
    }
    identity->pvehicle = pvehicle;
    identity->player = player;
    identity->simable = simable;
    return true;
}

bool ReadPlayerCandidate(Candidate* candidate) {
    if (candidate == nullptr) {
        return false;
    }
    *candidate = {};
    const std::uintptr_t table = g_gameBase + kPVehicleInstancesRva;
    if (!ReadableRange(reinterpret_cast<const void*>(table),
                       kPVehicleInstanceLimit * kPVehicleInstanceStride)) {
        return false;
    }
    const std::uintptr_t expectedPVehicleVtable =
        g_gameBase + kPVehiclePrimaryVtableRva;
    const std::uintptr_t expectedRigidBodyVtable =
        g_gameBase + kRigidBodyVtableRva;
    bool foundAny = false;
    Candidate found{};
    for (std::size_t index = 0; index < kPVehicleInstanceLimit; ++index) {
        const std::uintptr_t entry =
            table + index * kPVehicleInstanceStride;
        std::uintptr_t pvehicle = 0;
        std::uint8_t enabled = 0;
        if (!SafeReadPointer(entry, &pvehicle) ||
            !SafeRead(reinterpret_cast<const void*>(
                          entry + kPVehicleInstanceEnabledOffset),
                      &enabled)) {
            continue;
        }
        if (enabled == 0 || enabled > 1 || pvehicle == 0 ||
            !ReadableRange(reinterpret_cast<const void*>(pvehicle),
                           kPVehicleReadableSize)) {
            continue;
        }
        std::uintptr_t pvehicleVtable = 0;
        if (!SafeReadPointer(pvehicle, &pvehicleVtable) ||
            pvehicleVtable != expectedPVehicleVtable) {
            continue;
        }
        // PVehicle::mDirty is a byte at +0x48.  Reading a wider word here
        // would consume adjacent fields and reject every valid vehicle.
        std::uint8_t dirty = 1;
        std::uint32_t objectType = 0;
        std::uintptr_t rigidBody = 0;
        if (!SafeRead(reinterpret_cast<const void*>(
                          pvehicle + kPVehicleDirtyOffset),
                      &dirty) ||
            !SafeRead(reinterpret_cast<const void*>(
                          pvehicle + kPVehicleObjectTypeOffset),
                      &objectType) ||
            !SafeReadPointer(pvehicle + kPVehicleRigidBodyOffset,
                             &rigidBody) ||
            dirty != 0 || objectType == 0 || rigidBody == 0) {
            continue;
        }
        std::uintptr_t rigidBodyVtable = 0;
        if (!SafeReadPointer(rigidBody, &rigidBodyVtable) ||
            rigidBodyVtable != expectedRigidBodyVtable) {
            continue;
        }
        Identity identity{};
        if (!ReadPlayerOwnership(pvehicle, &identity)) {
            continue;
        }
        identity.rigidBody = rigidBody;
        std::uint32_t grounded = 0;
        if (!SafeRead(reinterpret_cast<const void*>(
                          pvehicle + kPVehicleGroundedOffset),
                      &grounded) ||
            grounded > 4) {
            continue;
        }
        if (!foundAny) {
            found.identity = identity;
            found.grounded = grounded;
            foundAny = true;
            continue;
        }

        // A loading/vehicle-transition window may expose the same player
        // object in multiple table slots.  De-duplicate that exact tuple;
        // reject only a genuinely different player candidate.
        if (!SameOwnershipIdentity(found.identity, identity)) {
            return false;
        }
        // Keep the newest grounded value when duplicate slots disagree.
        found.grounded = grounded;
    }
    if (!foundAny) {
        return false;
    }
    *candidate = found;
    return true;
}

bool CandidateBodyLinkCurrent(const Candidate& candidate,
                              std::uintptr_t body) {
    if (body == 0 || candidate.identity.pvehicle == 0 ||
        candidate.identity.player == 0 || candidate.identity.simable == 0 ||
        candidate.identity.rigidBody != body) {
        return false;
    }
    std::uintptr_t rigidBody = 0;
    std::uintptr_t player = 0;
    std::uintptr_t simable = 0;
    std::uint8_t dirty = 1;
    std::uint32_t objectType = 0;
    return SafeReadPointer(candidate.identity.pvehicle +
                               kPVehicleRigidBodyOffset,
                           &rigidBody) &&
           SafeReadPointer(candidate.identity.pvehicle +
                               kPVehiclePlayerOffset,
                           &player) &&
           SafeReadPointer(candidate.identity.pvehicle +
                               kPVehicleSimableOffset,
                           &simable) &&
           SafeRead(reinterpret_cast<const void*>(candidate.identity.pvehicle +
                                                   kPVehicleDirtyOffset),
                    &dirty) &&
           SafeRead(reinterpret_cast<const void*>(candidate.identity.pvehicle +
                                                   kPVehicleObjectTypeOffset),
                    &objectType) &&
           rigidBody == body && player == candidate.identity.player &&
           simable == candidate.identity.simable && dirty == 0 &&
           objectType != 0;
}

bool ResolveCandidateForBody(std::uintptr_t body, std::int64_t nowQpc,
                             Candidate* candidate) {
    if (candidate == nullptr || body == 0) {
        return false;
    }

    Candidate cached{};
    bool cacheValid = false;
    std::int64_t lastScanQpc = 0;
    AcquireSRWLockShared(&g_candidateLock);
    cached = g_candidateCache.candidate;
    cacheValid = g_candidateCache.valid;
    lastScanQpc = g_candidateCache.lastScanQpc;
    ReleaseSRWLockShared(&g_candidateLock);

    const bool cacheFresh =
        lastScanQpc > 0 && nowQpc >= lastScanQpc &&
        QpcSeconds(nowQpc - lastScanQpc) < kCandidateRescanSeconds;
    if (cacheFresh) {
        if (!cacheValid) {
            return false;
        }
        if (cached.identity.rigidBody != body ||
            !CandidateBodyLinkCurrent(cached, body)) {
            return false;
        }
        *candidate = cached;
        return true;
    }

    // Serialize the relatively rare pool scan, then re-check in case another
    // physics call refreshed the cache while this thread was waiting.
    AcquireSRWLockExclusive(&g_candidateLock);
    const bool exclusiveCacheFresh =
        g_candidateCache.lastScanQpc > 0 &&
        nowQpc >= g_candidateCache.lastScanQpc &&
        QpcSeconds(nowQpc - g_candidateCache.lastScanQpc) <
            kCandidateRescanSeconds;
    if (exclusiveCacheFresh) {
        cached = g_candidateCache.candidate;
        cacheValid = g_candidateCache.valid;
        ReleaseSRWLockExclusive(&g_candidateLock);
        if (!cacheValid) {
            return false;
        }
        if (cached.identity.rigidBody != body ||
            !CandidateBodyLinkCurrent(cached, body)) {
            return false;
        }
        *candidate = cached;
        return true;
    }

    Candidate fresh{};
    const bool found = ReadPlayerCandidate(&fresh);
    g_candidateCache.lastScanQpc = nowQpc;
    g_candidateCache.valid = found;
    g_candidateCache.candidate = found ? fresh : Candidate{};
    ReleaseSRWLockExclusive(&g_candidateLock);
    if (!found || fresh.identity.rigidBody != body ||
        !CandidateBodyLinkCurrent(fresh, body)) {
        return false;
    }
    *candidate = fresh;
    return true;
}

bool ReadSnapshot(const Candidate& candidate, Snapshot* snapshot) {
    if (snapshot == nullptr || candidate.identity.pvehicle == 0 ||
        candidate.identity.rigidBody == 0) {
        return false;
    }
    *snapshot = {};
    snapshot->candidate = candidate;
    const std::uintptr_t pvehicle = candidate.identity.pvehicle;
    const std::uintptr_t rigidBody = candidate.identity.rigidBody;
    std::uint32_t grounded = 0;
    if (!SafeRead(reinterpret_cast<const void*>(pvehicle +
                                                kPVehicleGroundedOffset),
                  &grounded) ||
        grounded > 4) {
        return false;
    }
    snapshot->candidate.grounded = grounded;
    std::uintptr_t holder = 0;
    std::uintptr_t inner = 0;
    if (!SafeReadPointer(rigidBody + kRigidBodyHolderOffset, &holder) ||
        holder == 0 || !SafeReadPointer(holder, &inner) || inner == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(inner), 0xA0)) {
        return false;
    }
    std::uint8_t mode = 0xFF;
    if (!SafeRead(reinterpret_cast<const void*>(inner + kRigidBodyModeOffset),
                  &mode) ||
        mode != 0 ||
        !SafeReadVec3(inner + kRigidBodyLinearVelocityOffset,
                      &snapshot->velocity) ||
        !SafeReadVec3(inner + kRigidBodyRightVectorOffset, &snapshot->right) ||
        !SafeReadVec3(inner + kRigidBodyUpVectorOffset, &snapshot->up) ||
        !SafeReadVec3(inner + kRigidBodyForwardVectorOffset,
                      &snapshot->forward) ||
        !FiniteBoundedVec3(snapshot->velocity, 1000.0f) ||
        !FiniteBoundedVec3(snapshot->right, 2.0f) ||
        !FiniteBoundedVec3(snapshot->up, 2.0f) ||
        !FiniteBoundedVec3(snapshot->forward, 2.0f)) {
        return false;
    }

    // Validate the raw basis before normalization.  Normalizing first would
    // make a zero or badly scaled vector appear unit length and defeat the
    // profile gate.  The tolerances match the measured vehicle-reader
    // contract used by the historical bridge.
    const float rightLength = VecLength(snapshot->right);
    const float upLength = VecLength(snapshot->up);
    const float forwardLength = VecLength(snapshot->forward);
    if (!std::isfinite(rightLength) || !std::isfinite(upLength) ||
        !std::isfinite(forwardLength) || rightLength < 0.85f ||
        rightLength > 1.15f || upLength < 0.85f || upLength > 1.15f ||
        forwardLength < 0.85f || forwardLength > 1.15f) {
        return false;
    }
    snapshot->right = Normalize(snapshot->right, {1.0f, 0.0f, 0.0f});
    snapshot->up = Normalize(snapshot->up, {0.0f, 1.0f, 0.0f});
    snapshot->forward = Normalize(snapshot->forward, {0.0f, 0.0f, 1.0f});
    if (std::fabs(DotLocal(snapshot->right, snapshot->up)) > 0.15f ||
        std::fabs(DotLocal(snapshot->right, snapshot->forward)) > 0.15f ||
        std::fabs(DotLocal(snapshot->up, snapshot->forward)) > 0.15f ||
        DotLocal(CrossLocal(snapshot->right, snapshot->up),
                 snapshot->forward) < 0.75f) {
        return false;
    }
    snapshot->speedMps = VecLength(snapshot->velocity);
    const float longitudinal = DotLocal(snapshot->velocity, snapshot->forward);
    if (!std::isfinite(snapshot->speedMps) || snapshot->speedMps < 0.0f ||
        longitudinal <= 0.0f) {
        return false;
    }
    const Vec3 cross = CrossLocal(snapshot->forward, snapshot->velocity);
    snapshot->signedDriftAngleRad =
        std::atan2(DotLocal(cross, snapshot->up), longitudinal);

    std::uintptr_t input = 0;
    if (!SafeReadPointer(pvehicle + kPVehicleInputOffset, &input) ||
        input == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(input + kPInputControlsOffset),
                       kInputHandbrakeOffset + sizeof(float))) {
        return false;
    }
    if (!SafeReadFloat(input + kPInputControlsOffset + kInputSteeringOffset,
                       &snapshot->steering) ||
        !SafeReadFloat(input + kPInputControlsOffset + kInputGasOffset,
                       &snapshot->throttle) ||
        !SafeReadFloat(input + kPInputControlsOffset + kInputBrakeOffset,
                       &snapshot->brake) ||
        !SafeReadFloat(input + kPInputControlsOffset + kInputHandbrakeOffset,
                       &snapshot->handbrake)) {
        return false;
    }
    if (snapshot->steering < -1.05f || snapshot->steering > 1.05f ||
        snapshot->throttle < -0.05f || snapshot->throttle > 1.05f ||
        snapshot->brake < -0.05f || snapshot->brake > 1.05f ||
        snapshot->handbrake < -0.05f || snapshot->handbrake > 1.05f) {
        return false;
    }
    snapshot->candidate.identity.holder = holder;
    snapshot->candidate.identity.inner = inner;
    return true;
}

bool SameIdentity(const Identity& a, const Identity& b) {
    return a.pvehicle != 0 && a.pvehicle == b.pvehicle &&
           a.player == b.player && a.simable == b.simable &&
           a.rigidBody == b.rigidBody;
}

void ResetSessionLocked() {
    g_session = SessionState{};
}

bool ShouldInjectLocked(const Snapshot& snapshot, std::int64_t nowQpc,
                        float* dtOut, float* rampOut) {
    if (dtOut == nullptr || rampOut == nullptr) {
        return false;
    }
    if (!SameIdentity(g_session.identity, snapshot.candidate.identity)) {
        g_session = SessionState{};
        g_session.identity = snapshot.candidate.identity;
        g_session.lastQpc = nowQpc;
    }
    const double elapsed =
        g_session.lastQpc > 0
            ? std::max(0.0, QpcSeconds(nowQpc - g_session.lastQpc))
            : static_cast<double>(kDefaultDt);
    const float dt = std::clamp(static_cast<float>(elapsed), 0.001f,
                                kMaximumDt);
    g_session.lastQpc = nowQpc;

    const bool speedReady = snapshot.speedMps >= kMinimumDriftSpeedMps;
    const bool grounded = snapshot.candidate.grounded >= 2 &&
                          snapshot.candidate.grounded <= 4;
    const bool driftEvidence =
        std::fabs(snapshot.signedDriftAngleRad) >= kMinimumDriftAngleRad;
    if (!g_session.active && speedReady && grounded &&
        snapshot.handbrake >= kHandbrakeActivation) {
        g_session.active = true;
        g_session.ramp = 0.0f;
        g_session.lastDriftQpc = nowQpc;
    }
    if (driftEvidence) {
        g_session.lastDriftQpc = nowQpc;
    }
    if (g_session.active && !snapshot.handbrake &&
        g_session.lastDriftQpc > 0 &&
        QpcSeconds(nowQpc - g_session.lastDriftQpc) >=
            kSessionReleaseSeconds &&
        std::fabs(snapshot.signedDriftAngleRad) < kReleaseDriftAngleRad) {
        g_session.active = false;
    }

    const bool demand =
        g_session.active && speedReady && grounded && driftEvidence &&
        snapshot.throttle >= kMinimumThrottle &&
        snapshot.throttle <= 1.0f && snapshot.brake >= 0.0f &&
        snapshot.brake <= kMaximumBrake;
    g_session.ramp = rigidbody_accel::AdvanceRamp(
        g_session.ramp, demand, dt, 1.0f, 0.25f);
    *dtOut = dt;
    *rampOut = g_session.ramp;

    if (!demand || g_session.lastInjectionQpc > 0) {
        if (g_session.lastInjectionQpc > 0 &&
            QpcSeconds(nowQpc - g_session.lastInjectionQpc) <
                kInjectionIntervalSeconds) {
            return false;
        }
    }
    return demand;
}

void LogTelemetry(const Snapshot& snapshot,
                  const rigidbody_accel::Plan& plan,
                  const rigidbody_accel::AppliedDeltaVerification& applied,
                  bool writeAttempted, bool writeOk) {
    const DWORD now = GetTickCount();
    DWORD previous = g_lastTelemetryTick.load(std::memory_order_relaxed);
    if (previous != 0 &&
        static_cast<DWORD>(now - previous) < kTelemetryIntervalMs &&
        !writeAttempted) {
        return;
    }
    if (!g_lastTelemetryTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return;
    }
    Log("mode=%s accepted=%d pvehicle=%p rigid_body=%p speed_mps=%.3f "
        "drift_deg=%.2f throttle=%.3f brake=%.3f grounded=%u ramp=%.3f "
        "target_accel=%.3f delta_v=%.5f before=(%.3f,%.3f,%.3f) "
        "after=(%.3f,%.3f,%.3f) actual_delta=%.5f projected=%.5f "
        "orthogonal=%.5f write_attempted=%d write_ok=%d",
        writeAttempted ? "write" : "idle", plan.accepted ? 1 : 0,
        reinterpret_cast<void*>(snapshot.candidate.identity.pvehicle),
        reinterpret_cast<void*>(snapshot.candidate.identity.rigidBody),
        static_cast<double>(snapshot.speedMps),
        static_cast<double>(snapshot.signedDriftAngleRad * 180.0f /
                            nfsmw_drift::kPi),
        static_cast<double>(snapshot.throttle), static_cast<double>(snapshot.brake),
        snapshot.candidate.grounded, static_cast<double>(plan.speedScale),
        static_cast<double>(plan.targetAccelerationMps2),
        static_cast<double>(plan.deltaVMps),
        static_cast<double>(applied.beforeVelocity.x),
        static_cast<double>(applied.beforeVelocity.y),
        static_cast<double>(applied.beforeVelocity.z),
        static_cast<double>(applied.afterVelocity.x),
        static_cast<double>(applied.afterVelocity.y),
        static_cast<double>(applied.afterVelocity.z),
        static_cast<double>(applied.actualDeltaVMps),
        static_cast<double>(applied.projectedDeltaVMps),
        static_cast<double>(applied.orthogonalResidualMps),
        writeAttempted ? 1 : 0, writeOk ? 1 : 0);
}

void MaybeInject(std::uintptr_t body) {
    if (!g_hookInstalled.load(std::memory_order_acquire) ||
        g_writerDisabled.load(std::memory_order_acquire)) {
        return;
    }
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter)) {
        LogFailure("frame rejected: QueryPerformanceCounter failed");
        return;
    }
    Candidate candidate{};
    if (!ResolveCandidateForBody(body, counter.QuadPart, &candidate)) {
        return;
    }
    Snapshot snapshot{};
    if (!ReadSnapshot(candidate, &snapshot)) {
        LogFailure("frame rejected: player vehicle snapshot is invalid");
        return;
    }

    float dt = kDefaultDt;
    float ramp = 0.0f;
    rigidbody_accel::Plan plan{};
    {
        AcquireSRWLockExclusive(&g_stateLock);
        if (!ShouldInjectLocked(snapshot, counter.QuadPart, &dt, &ramp)) {
            ReleaseSRWLockExclusive(&g_stateLock);
            return;
        }
        rigidbody_accel::Inputs input{};
        input.enabled = true;
        input.sessionActive = true;
        input.identityStable = true;
        input.physicsStable = true;
        input.grounded = snapshot.candidate.grounded >= 2;
        input.throttle = std::clamp(snapshot.throttle, 0.0f, 1.0f);
        input.brake = std::clamp(snapshot.brake, 0.0f, 1.0f);
        input.dt = dt;
        input.forward = snapshot.forward;
        input.linearVelocity = snapshot.velocity;
        input.targetAccelerationMps2 =
            rigidbody_accel::kConfiguredTargetAccelerationMps2;
        input.ramp = ramp;
        plan = rigidbody_accel::MakePlan(input);
        if (plan.accepted) {
            g_session.lastInjectionQpc = counter.QuadPart;
        }
        ReleaseSRWLockExclusive(&g_stateLock);
    }
    if (!plan.accepted) {
        rigidbody_accel::AppliedDeltaVerification empty{};
        LogTelemetry(snapshot, plan, empty, false, false);
        return;
    }

    std::uintptr_t velocityAddress = 0;
    if (!AddAddress(snapshot.candidate.identity.inner,
                    kRigidBodyLinearVelocityOffset, &velocityAddress)) {
        LogFailure("write rejected: velocity address overflowed");
        return;
    }
    Vec3 before{};
    if (!SafeReadVec3(velocityAddress, &before)) {
        LogFailure("write rejected: velocity read before Accelerate failed");
        return;
    }
    const AccelerateFn original =
        g_originalAccelerate.load(std::memory_order_acquire);
    if (original == nullptr) {
        return;
    }
    bool called = false;
#if defined(_MSC_VER)
    __try {
        original(reinterpret_cast<void*>(body), &plan.direction, plan.deltaVMps);
        called = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        called = false;
    }
#else
    original(reinterpret_cast<void*>(body), &plan.direction, plan.deltaVMps);
    called = true;
#endif
    Vec3 after{};
    rigidbody_accel::AppliedDeltaVerification applied{};
    if (called && SafeReadVec3(velocityAddress, &after)) {
        applied = rigidbody_accel::VerifyAppliedDelta(
            before, after, plan.direction, plan.deltaVMps);
    }
    const bool writeOk = called && applied.accepted;
    if (!writeOk) {
        g_writerDisabled.store(true, std::memory_order_release);
        Log("writer disabled: Accelerate call/read-back did not satisfy strict "
            "delta verification (called=%d accepted=%d)",
            called ? 1 : 0, applied.accepted ? 1 : 0);
    }
    LogTelemetry(snapshot, plan, applied, true, writeOk);
}

// A virtual thiscall target puts `self` in ECX and leaves the remaining
// arguments on the stack.  MSVC does not permit __thiscall on a free function,
// so use the x86 fastcall bridge: ECX is still `self`, EDX is an explicit
// unused slot, and distribution/amount retain their stack positions.
void NFSMW_FASTCALL AccelerateDetour(void* self, void* /*unusedEdx*/,
                                     const Vec3* distribution, float amount) {
    const AccelerateFn original =
        g_originalAccelerate.load(std::memory_order_acquire);
    if (g_accelerateInFlight) {
        // The game's implementation can call another virtual acceleration
        // path while integrating a body.  Preserve that nested call, but do
        // not recursively run candidate discovery or inject a second boost.
        if (original != nullptr) {
            original(self, distribution, amount);
        }
        return;
    }
    struct ReentryScope {
        bool& flag;
        explicit ReentryScope(bool& value) : flag(value) { flag = true; }
        ~ReentryScope() { flag = false; }
    } reentry(g_accelerateInFlight);
    if (original != nullptr) {
        original(self, distribution, amount);
    }
    if (self != nullptr && !g_writerDisabled.load(std::memory_order_acquire)) {
        MaybeInject(reinterpret_cast<std::uintptr_t>(self));
    }
}

bool InstallVtableHook() {
    const std::uintptr_t vtable = g_gameBase + kRigidBodyVtableRva;
    const std::uintptr_t slotAddress =
        vtable + kRigidBodyAccelerateVtableSlot * sizeof(std::uintptr_t);
    std::uintptr_t target = 0;
    if (!SafeReadPointer(slotAddress, &target) ||
        target != g_gameBase + kAccelerateRva || !ExecutableAddress(target)) {
        Log("hook rejected: IRigidBody vtable slot 39 is not the measured Accelerate");
        return false;
    }
    g_originalAccelerate.store(reinterpret_cast<AccelerateFn>(target),
                               std::memory_order_release);
    const std::uintptr_t detour =
        reinterpret_cast<std::uintptr_t>(&AccelerateDetour);
    if (!SafeWritePointer(slotAddress, detour)) {
        g_originalAccelerate.store(nullptr, std::memory_order_release);
        Log("hook rejected: vtable slot 39 could not be written");
        return false;
    }
    g_hookInstalled.store(true, std::memory_order_release);
    Log("installed vtable-only companion hook slot=39 target=%p detour=%p; "
        "historical 0.9.5 input/physics hooks remain untouched",
        reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour));
    return true;
}

}  // namespace
}  // namespace nfsmw_drift_asi::rigidbody_companion

NFSMW_PLUGIN_DECLARE("NFSMW Drift Assist Rigidbody Companion",
                     "0.9.5-rigidbody20", "OpenAI")

NFSMW_PLUGIN_MAIN() {
    using namespace nfsmw_drift_asi::rigidbody_companion;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
        return NFSMW_OK;
    }
    if (!nfsmw_validate_speed_exe()) {
        Log("companion disabled: SDK speed.exe gate failed");
        return NFSMW_FAIL;
    }
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        Log("companion disabled: performance-counter frequency unavailable");
        return NFSMW_FAIL;
    }
    g_qpcFrequency = frequency.QuadPart;
    const DWORD retryStart = GetTickCount();
    bool installed = false;
    for (;;) {
        std::uintptr_t profileBase = 0;
        if (ReadGameProfile(&profileBase)) {
            g_gameBase = profileBase;
            if (InstallVtableHook()) {
                installed = true;
                break;
            }
        }
        const DWORD elapsed =
            static_cast<DWORD>(GetTickCount() - retryStart);
        if (elapsed >= kInitializationRetryWindowMs) {
            break;
        }
        Sleep(kInitializationRetryIntervalMs);
    }
    if (!installed) {
        Log("companion disabled: profile/vtable did not become ready within "
            "%lu ms", static_cast<unsigned long>(
                          kInitializationRetryWindowMs));
        return NFSMW_FAIL;
    }
    Log("companion active: nominal target=5.25 m/s^2, effective strength=80%% "
        "(4.20 m/s^2 at <=70 km/h); speed fade 70/125/150/170 km/h; "
        "strict before/after verification enabled");
    return NFSMW_OK;
}
