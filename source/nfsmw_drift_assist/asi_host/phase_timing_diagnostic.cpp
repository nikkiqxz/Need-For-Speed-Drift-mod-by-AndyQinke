#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef NFSMW_ENABLE_PHASE_DIAGNOSTIC
#define NFSMW_ENABLE_PHASE_DIAGNOSTIC 0
#endif

#include "phase_timing_diagnostic.hpp"
#include "runtime_logging.hpp"

#include <nfsmw_sdk/nfsmw_sdk.h>
#include <MinHook.h>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace nfsmw_drift_asi::phase_timing_diagnostic {
namespace {

#if NFSMW_ENABLE_PHASE_DIAGNOSTIC

using PhaseFn = void (NFSMW_CDECL*)(float dt);

constexpr std::uintptr_t kActiveComponentsRva = 0x000BA940u;
constexpr std::uintptr_t kWorldPhysicsRva = 0x002E7A00u;
constexpr std::uintptr_t kPVehicleInstancesRva = 0x005352B0u;
constexpr std::uintptr_t kExpectedRigidBodyVtableRva = 0x004AC880u;

constexpr std::size_t kPVehicleInstanceLimit = 64;
constexpr std::size_t kPVehicleInstanceStride = 8;
constexpr std::size_t kPVehicleReadableSize = 0x88;
constexpr std::size_t kRigidBodyOffset = 0x78;
constexpr std::size_t kPlayerOffset = 0x84;
constexpr std::size_t kRigidBodyInnerHolderOffset = 0x30;
constexpr std::size_t kAngularVelocityOffset = 0x30;
constexpr DWORD kLogIntervalMs = 250;

enum SampleFlags : std::uint32_t {
    kTableReadable = 1u << 0,
    kScanComplete = 1u << 1,
    kUniquePlayer = 1u << 2,
    kPVehicleReadable = 1u << 3,
    kRigidBodyReadable = 1u << 4,
    kRigidBodyVtableMatches = 1u << 5,
    kHolderReadable = 1u << 6,
    kInnerReadable = 1u << 7,
    kAngularVelocityFinite = 1u << 8,
};

constexpr std::uint32_t kRequiredSampleFlags =
    kTableReadable | kScanComplete | kUniquePlayer | kPVehicleReadable |
    kRigidBodyReadable | kRigidBodyVtableMatches | kHolderReadable |
    kInnerReadable | kAngularVelocityFinite;

struct PlayerSample {
    std::uint32_t flags = 0;
    std::uint32_t playerCandidates = 0;
    std::uintptr_t pvehicle = 0;
    std::uintptr_t player = 0;
    std::uintptr_t rigidBody = 0;
    std::uintptr_t rigidBodyVtable = 0;
    std::uintptr_t holder = 0;
    std::uintptr_t inner = 0;
    std::uintptr_t angularVelocityAddress = 0;
    float angularVelocity[3]{};

    bool valid() const {
        return (flags & kRequiredSampleFlags) == kRequiredSampleFlags;
    }
};

struct IdentityObservation {
    std::uint64_t generation = 0;
    bool changed = false;
};

struct IdentityState {
    bool present = false;
    std::uint64_t generation = 0;
    std::uintptr_t pvehicle = 0;
    std::uintptr_t player = 0;
    std::uintptr_t rigidBody = 0;
    std::uintptr_t holder = 0;
    std::uintptr_t inner = 0;
};

struct PairContext {
    bool pendingWorld = false;
    std::uint64_t pair = 0;
    std::uint64_t activeCall = 0;
    float activeDt = 0.0f;
};

std::atomic<PhaseFn> g_activeOriginal{nullptr};
std::atomic<PhaseFn> g_worldOriginal{nullptr};
std::atomic<std::uintptr_t> g_imageBase{0};
std::atomic<std::size_t> g_imageSize{0};
std::atomic<bool> g_armed{false};
std::atomic<std::uint64_t> g_activeCalls{0};
std::atomic<std::uint64_t> g_worldCalls{0};
std::atomic<std::uint64_t> g_pairSequence{0};
std::atomic<std::uint64_t> g_incompletePairs{0};
std::atomic<DWORD> g_lastLogTick{0};
SRWLOCK g_identityLock = SRWLOCK_INIT;
IdentityState g_identity{};
thread_local PairContext g_pairContext{};

void Log(const char* message) {
    if (message == nullptr) {
        return;
    }
    char line[1400]{};
    std::snprintf(line, sizeof(line), "[nfsmw_drift_assist] %s\n", message);
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(line);
}

bool AddAddress(std::uintptr_t base,
                std::size_t offset,
                std::uintptr_t* result) {
    if (result == nullptr ||
        base > static_cast<std::uintptr_t>(-1) - offset) {
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
    if (protection == PAGE_NOACCESS) {
        return false;
    }
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t regionBegin =
        reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + info.RegionSize;
    return regionEnd >= regionBegin && begin >= regionBegin &&
           size <= regionEnd - begin;
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

bool ImageRange(const void* address, std::size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    const std::size_t imageSize = g_imageSize.load(std::memory_order_acquire);
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    if (base == 0 || imageSize == 0 || begin < base) {
        return false;
    }
    const std::uintptr_t offset = begin - base;
    return offset <= imageSize && size <= imageSize - offset &&
           ReadableRange(address, size);
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
    return std::isfinite(*value);
}

bool ReadPointerField(std::uintptr_t object,
                      std::size_t offset,
                      std::uintptr_t* value) {
    std::uintptr_t address = 0;
    return object != 0 && AddAddress(object, offset, &address) &&
           SafeReadPointer(reinterpret_cast<const void*>(address), value);
}

void ReadFirstPlayerCandidate(std::uintptr_t pvehicle,
                              PlayerSample* sample) {
    if (sample == nullptr || sample->playerCandidates != 0) {
        return;
    }
    sample->pvehicle = pvehicle;
    if (!ReadableRange(reinterpret_cast<const void*>(pvehicle),
                       kPVehicleReadableSize)) {
        return;
    }
    sample->flags |= kPVehicleReadable;
    if (!ReadPointerField(pvehicle, kPlayerOffset, &sample->player) ||
        sample->player == 0 ||
        !ReadPointerField(pvehicle, kRigidBodyOffset, &sample->rigidBody) ||
        sample->rigidBody == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(sample->rigidBody),
                       kRigidBodyInnerHolderOffset +
                           sizeof(std::uintptr_t))) {
        return;
    }
    sample->flags |= kRigidBodyReadable;
    if (!SafeReadPointer(reinterpret_cast<const void*>(sample->rigidBody),
                         &sample->rigidBodyVtable)) {
        return;
    }
    const std::uintptr_t imageBase =
        g_imageBase.load(std::memory_order_acquire);
    if (sample->rigidBodyVtable !=
            imageBase + kExpectedRigidBodyVtableRva ||
        !ImageRange(reinterpret_cast<const void*>(sample->rigidBodyVtable),
                    sizeof(std::uintptr_t))) {
        return;
    }
    sample->flags |= kRigidBodyVtableMatches;

    if (!ReadPointerField(sample->rigidBody,
                          kRigidBodyInnerHolderOffset,
                          &sample->holder) ||
        sample->holder == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(sample->holder),
                       sizeof(std::uintptr_t))) {
        return;
    }
    sample->flags |= kHolderReadable;
    if (!SafeReadPointer(reinterpret_cast<const void*>(sample->holder),
                         &sample->inner) ||
        sample->inner == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(sample->inner),
                       kAngularVelocityOffset + 3 * sizeof(float)) ||
        !AddAddress(sample->inner, kAngularVelocityOffset,
                    &sample->angularVelocityAddress)) {
        return;
    }
    sample->flags |= kInnerReadable;
    for (std::size_t index = 0; index < 3; ++index) {
        if (!SafeReadFiniteFloat(
                reinterpret_cast<const void*>(
                    sample->angularVelocityAddress + index * sizeof(float)),
                &sample->angularVelocity[index])) {
            return;
        }
    }
    sample->flags |= kAngularVelocityFinite;
}

PlayerSample CapturePlayerSample() {
    PlayerSample sample{};
    const std::uintptr_t imageBase =
        g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t table = 0;
    if (!AddAddress(imageBase, kPVehicleInstancesRva, &table) ||
        !ImageRange(reinterpret_cast<const void*>(table),
                    kPVehicleInstanceLimit * kPVehicleInstanceStride)) {
        return sample;
    }
    sample.flags |= kTableReadable;

    bool scanComplete = true;
    for (std::size_t index = 0; index < kPVehicleInstanceLimit; ++index) {
        std::uintptr_t slot = 0;
        std::uintptr_t pvehicle = 0;
        if (!AddAddress(table, index * kPVehicleInstanceStride, &slot) ||
            !SafeReadPointer(reinterpret_cast<const void*>(slot), &pvehicle)) {
            scanComplete = false;
            break;
        }
        if (pvehicle == 0) {
            break;
        }
        if (!ReadableRange(reinterpret_cast<const void*>(pvehicle),
                           kPVehicleReadableSize)) {
            scanComplete = false;
            continue;
        }
        std::uintptr_t player = 0;
        if (!ReadPointerField(pvehicle, kPlayerOffset, &player)) {
            scanComplete = false;
            continue;
        }
        if (player == 0) {
            continue;
        }
        if (sample.playerCandidates == 0) {
            ReadFirstPlayerCandidate(pvehicle, &sample);
            // ReadFirstPlayerCandidate intentionally runs before this count is
            // incremented so it only captures the first non-null mPlayer row.
            sample.player = player;
        }
        ++sample.playerCandidates;
    }
    if (scanComplete) {
        sample.flags |= kScanComplete;
    }
    if (sample.playerCandidates == 1) {
        sample.flags |= kUniquePlayer;
    }
    return sample;
}

IdentityObservation ObserveIdentity(const PlayerSample& sample) {
    IdentityObservation observation{};
    AcquireSRWLockExclusive(&g_identityLock);
    if (sample.valid()) {
        const bool changed = !g_identity.present ||
                             g_identity.pvehicle != sample.pvehicle ||
                             g_identity.player != sample.player ||
                             g_identity.rigidBody != sample.rigidBody ||
                             g_identity.holder != sample.holder ||
                             g_identity.inner != sample.inner;
        if (changed) {
            ++g_identity.generation;
            g_identity.present = true;
            g_identity.pvehicle = sample.pvehicle;
            g_identity.player = sample.player;
            g_identity.rigidBody = sample.rigidBody;
            g_identity.holder = sample.holder;
            g_identity.inner = sample.inner;
        }
        observation.changed = changed;
    } else if ((sample.flags & (kTableReadable | kScanComplete)) ==
                   (kTableReadable | kScanComplete) &&
               sample.playerCandidates == 0 && g_identity.present) {
        ++g_identity.generation;
        g_identity.present = false;
        g_identity.pvehicle = 0;
        g_identity.player = 0;
        g_identity.rigidBody = 0;
        g_identity.holder = 0;
        g_identity.inner = 0;
        observation.changed = true;
    }
    observation.generation = g_identity.generation;
    ReleaseSRWLockExclusive(&g_identityLock);
    return observation;
}

bool ReservePairLog(std::uint64_t* pairOut) {
    if (pairOut == nullptr) {
        return false;
    }
    const DWORD now = GetTickCount();
    DWORD previous = g_lastLogTick.load(std::memory_order_relaxed);
    if (static_cast<DWORD>(now - previous) < kLogIntervalMs ||
        !g_lastLogTick.compare_exchange_strong(
            previous, now, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
        return false;
    }
    *pairOut = g_pairSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    return true;
}

bool SameAngularVelocityObject(const PlayerSample& left,
                               const PlayerSample& right) {
    return left.valid() && right.valid() &&
           left.pvehicle == right.pvehicle &&
           left.rigidBody == right.rigidBody &&
           left.inner == right.inner;
}

void LogPhase(const char* phase,
              std::uint64_t pair,
              std::uint64_t anchorCall,
              float dt,
              const PlayerSample& sample,
              const IdentityObservation& identity,
              const PlayerSample* before) {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const bool deltaValid =
        before != nullptr && SameAngularVelocityObject(*before, sample);
    const double deltaX = deltaValid
                              ? static_cast<double>(sample.angularVelocity[0] -
                                                    before->angularVelocity[0])
                              : 0.0;
    const double deltaY = deltaValid
                              ? static_cast<double>(sample.angularVelocity[1] -
                                                    before->angularVelocity[1])
                              : 0.0;
    const double deltaZ = deltaValid
                              ? static_cast<double>(sample.angularVelocity[2] -
                                                    before->angularVelocity[2])
                              : 0.0;
    char line[1280]{};
    std::snprintf(
        line, sizeof(line),
        "phase timing pair=%llu phase=%s call=%llu active_total=%llu "
        "world_total=%llu incomplete=%llu thread=%lu dt=%.7f dt_finite=%d "
        "qpc=%lld generation=%llu changed=%d candidates=%lu pv=%p player=%p "
        "rb=%p rbvtbl=%p holder=%p inner=%p omega_addr=%p "
        "omega=(%.7f %.7f %.7f) delta=(%.7f %.7f %.7f) delta_valid=%d "
        "flags=0x%03lX valid=%d",
        static_cast<unsigned long long>(pair), phase,
        static_cast<unsigned long long>(anchorCall),
        static_cast<unsigned long long>(
            g_activeCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_worldCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_incompletePairs.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<double>(dt), std::isfinite(dt) ? 1 : 0,
        static_cast<long long>(qpc.QuadPart),
        static_cast<unsigned long long>(identity.generation),
        identity.changed ? 1 : 0,
        static_cast<unsigned long>(sample.playerCandidates),
        reinterpret_cast<void*>(sample.pvehicle),
        reinterpret_cast<void*>(sample.player),
        reinterpret_cast<void*>(sample.rigidBody),
        reinterpret_cast<void*>(sample.rigidBodyVtable),
        reinterpret_cast<void*>(sample.holder),
        reinterpret_cast<void*>(sample.inner),
        reinterpret_cast<void*>(sample.angularVelocityAddress),
        static_cast<double>(sample.angularVelocity[0]),
        static_cast<double>(sample.angularVelocity[1]),
        static_cast<double>(sample.angularVelocity[2]), deltaX, deltaY, deltaZ,
        deltaValid ? 1 : 0, static_cast<unsigned long>(sample.flags),
        sample.valid() ? 1 : 0);
    Log(line);
}

void ReportIncompletePair() {
    if (!g_pairContext.pendingWorld) {
        return;
    }
    const std::uint64_t pair = g_pairContext.pair;
    const std::uint64_t activeCall = g_pairContext.activeCall;
    const float activeDt = g_pairContext.activeDt;
    // Consume before logging so a reentrant hook cannot report the same pair
    // twice or mistake it for a newly selected pair.
    g_pairContext.pendingWorld = false;
    const std::uint64_t incomplete =
        g_incompletePairs.fetch_add(1, std::memory_order_relaxed) + 1;
    char line[320]{};
    std::snprintf(
        line, sizeof(line),
        "phase timing pair=%llu incomplete=1 active_call=%llu "
        "thread=%lu active_dt=%.7f incomplete_total=%llu",
        static_cast<unsigned long long>(pair),
        static_cast<unsigned long long>(activeCall),
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<double>(activeDt),
        static_cast<unsigned long long>(incomplete));
    Log(line);
}

void NFSMW_CDECL ActiveComponentsDetour(float dt) {
    const std::uint64_t call =
        g_activeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const PhaseFn original =
        g_activeOriginal.load(std::memory_order_acquire);
    if (!g_armed.load(std::memory_order_acquire)) {
        original(dt);
        return;
    }

    ReportIncompletePair();
    std::uint64_t pair = 0;
    const bool selected = ReservePairLog(&pair);
    if (!selected) {
        original(dt);
        return;
    }
    const PlayerSample before = CapturePlayerSample();
    const IdentityObservation beforeIdentity = ObserveIdentity(before);
    g_pairContext.pendingWorld = true;
    g_pairContext.pair = pair;
    g_pairContext.activeCall = call;
    g_pairContext.activeDt = dt;
    LogPhase("active.before", pair, call, dt, before, beforeIdentity,
             nullptr);

    original(dt);

    const PlayerSample after = CapturePlayerSample();
    const IdentityObservation afterIdentity = ObserveIdentity(after);
    LogPhase("active.after", pair, call, dt, after, afterIdentity,
             &before);
}

void NFSMW_CDECL WorldPhysicsDetour(float dt) {
    const std::uint64_t call =
        g_worldCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const PhaseFn original =
        g_worldOriginal.load(std::memory_order_acquire);
    if (!g_armed.load(std::memory_order_acquire)) {
        original(dt);
        return;
    }

    const bool selected = g_pairContext.pendingWorld;
    if (!selected) {
        original(dt);
        return;
    }
    const std::uint64_t pair = g_pairContext.pair;
    // Claim the pair before any capture, logging, or original call. A nested
    // world call must not reuse it, and a nested active call may establish a
    // new pending pair without the outer call clearing that state on return.
    g_pairContext.pendingWorld = false;
    const PlayerSample before = CapturePlayerSample();
    const IdentityObservation beforeIdentity = ObserveIdentity(before);
    LogPhase("world.before", pair, call, dt, before, beforeIdentity,
             nullptr);

    original(dt);

    const PlayerSample after = CapturePlayerSample();
    const IdentityObservation afterIdentity = ObserveIdentity(after);
    LogPhase("world.after", pair, call, dt, after, afterIdentity,
             &before);
}

bool FindTextRange(std::uintptr_t moduleBase,
                   std::size_t imageSize,
                   std::uintptr_t* textBeginOut,
                   std::size_t* textSizeOut) {
    if (moduleBase == 0 || imageSize == 0 || textBeginOut == nullptr ||
        textSizeOut == nullptr ||
        !ReadableRange(reinterpret_cast<const void*>(moduleBase),
                       sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }
    const auto* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return false;
    }
    std::uintptr_t ntAddress = 0;
    if (!AddAddress(moduleBase, static_cast<std::size_t>(dos->e_lfanew),
                    &ntAddress) ||
        !ReadableRange(reinterpret_cast<const void*>(ntAddress),
                       sizeof(IMAGE_NT_HEADERS32))) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(ntAddress);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.NumberOfSections == 0 ||
        nt->FileHeader.NumberOfSections > 96) {
        return false;
    }
    const auto* section = IMAGE_FIRST_SECTION(nt);
    const std::size_t sectionBytes =
        static_cast<std::size_t>(nt->FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (!ReadableRange(section, sectionBytes)) {
        return false;
    }
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections;
         ++index) {
        if (std::memcmp(section[index].Name, ".text", 5) != 0) {
            continue;
        }
        const std::size_t virtualSize =
            static_cast<std::size_t>(section[index].Misc.VirtualSize);
        const std::size_t rawSize =
            static_cast<std::size_t>(section[index].SizeOfRawData);
        const std::size_t span = virtualSize > rawSize ? virtualSize : rawSize;
        const std::size_t rva =
            static_cast<std::size_t>(section[index].VirtualAddress);
        if (span == 0 || rva >= imageSize || span > imageSize - rva) {
            return false;
        }
        *textBeginOut = moduleBase + rva;
        *textSizeOut = span;
        return true;
    }
    return false;
}

bool InText(std::uintptr_t address,
            std::uintptr_t textBegin,
            std::size_t textSize) {
    return address >= textBegin &&
           address - textBegin < textSize &&
           ExecutableAddress(reinterpret_cast<const void*>(address));
}

#endif  // NFSMW_ENABLE_PHASE_DIAGNOSTIC

}  // namespace

bool Install(std::uintptr_t activeComponentsTarget,
             std::uintptr_t worldPhysicsTarget) {
#if !NFSMW_ENABLE_PHASE_DIAGNOSTIC
    (void)activeComponentsTarget;
    (void)worldPhysicsTarget;
    return false;
#else
    HMODULE module = GetModuleHandleA(nullptr);
    std::uintptr_t moduleBase = 0;
    std::size_t imageSize = 0;
    if (module == nullptr ||
        !nfsmw_main_module_range(&moduleBase, &imageSize) ||
        moduleBase != reinterpret_cast<std::uintptr_t>(module) ||
        imageSize == 0 ||
        activeComponentsTarget != moduleBase + kActiveComponentsRva ||
        worldPhysicsTarget != moduleBase + kWorldPhysicsRva) {
        Log("phase timing diagnostic disabled: validated targets or image range are invalid");
        return false;
    }

    g_imageBase.store(moduleBase, std::memory_order_release);
    g_imageSize.store(imageSize, std::memory_order_release);
    std::uintptr_t textBegin = 0;
    std::size_t textSize = 0;
    std::uintptr_t pvehicleTable = 0;
    const bool layoutValid =
        FindTextRange(moduleBase, imageSize, &textBegin, &textSize) &&
        InText(activeComponentsTarget, textBegin, textSize) &&
        InText(worldPhysicsTarget, textBegin, textSize) &&
        AddAddress(moduleBase, kPVehicleInstancesRva, &pvehicleTable) &&
        ImageRange(reinterpret_cast<const void*>(pvehicleTable),
                   kPVehicleInstanceLimit * kPVehicleInstanceStride) &&
        ImageRange(reinterpret_cast<const void*>(
                       moduleBase + kExpectedRigidBodyVtableRva),
                   sizeof(std::uintptr_t));
    if (!layoutValid) {
        Log("phase timing diagnostic disabled: .text or read-only layout validation failed");
        g_imageBase.store(0, std::memory_order_release);
        g_imageSize.store(0, std::memory_order_release);
        return false;
    }

    g_armed.store(false, std::memory_order_release);
    const MH_STATUS initializeStatus = MH_Initialize();
    if (initializeStatus != MH_OK &&
        initializeStatus != MH_ERROR_ALREADY_INITIALIZED) {
        char message[256]{};
        std::snprintf(message, sizeof(message),
                      "phase timing diagnostic disabled: MinHook initialization failed (%s)",
                      MH_StatusToString(initializeStatus));
        Log(message);
        g_imageBase.store(0, std::memory_order_release);
        g_imageSize.store(0, std::memory_order_release);
        return false;
    }

    void* const activeTarget =
        reinterpret_cast<void*>(activeComponentsTarget);
    void* const worldTarget = reinterpret_cast<void*>(worldPhysicsTarget);
    void* activeOriginalRaw = nullptr;
    void* worldOriginalRaw = nullptr;
    const MH_STATUS activeCreateStatus = MH_CreateHook(
        activeTarget, reinterpret_cast<void*>(&ActiveComponentsDetour),
        &activeOriginalRaw);
    if (activeCreateStatus != MH_OK || activeOriginalRaw == nullptr) {
        const MH_STATUS activeRemoveStatus =
            activeCreateStatus == MH_OK ? MH_RemoveHook(activeTarget)
                                        : MH_ERROR_NOT_CREATED;
        char message[320]{};
        std::snprintf(
            message, sizeof(message),
            "phase timing diagnostic disabled: ActiveComponents MinHook creation failed (%s); disabled cleanup=%s",
            MH_StatusToString(activeCreateStatus),
            MH_StatusToString(activeRemoveStatus));
        Log(message);
        g_imageBase.store(0, std::memory_order_release);
        g_imageSize.store(0, std::memory_order_release);
        return false;
    }

    const MH_STATUS worldCreateStatus = MH_CreateHook(
        worldTarget, reinterpret_cast<void*>(&WorldPhysicsDetour),
        &worldOriginalRaw);
    if (worldCreateStatus != MH_OK || worldOriginalRaw == nullptr) {
        const MH_STATUS worldRemoveStatus =
            worldCreateStatus == MH_OK ? MH_RemoveHook(worldTarget)
                                       : MH_ERROR_NOT_CREATED;
        const MH_STATUS activeRemoveStatus = MH_RemoveHook(activeTarget);
        char message[384]{};
        std::snprintf(
            message, sizeof(message),
            "phase timing diagnostic disabled: world-physics MinHook creation failed (%s); disabled cleanup active=%s world=%s",
            MH_StatusToString(worldCreateStatus),
            MH_StatusToString(activeRemoveStatus),
            MH_StatusToString(worldRemoveStatus));
        Log(message);
        g_imageBase.store(0, std::memory_order_release);
        g_imageSize.store(0, std::memory_order_release);
        return false;
    }

    const PhaseFn activeOriginal =
        reinterpret_cast<PhaseFn>(activeOriginalRaw);
    const PhaseFn worldOriginal =
        reinterpret_cast<PhaseFn>(worldOriginalRaw);
    // Both hooks are still disabled. Publish both trampolines before either
    // target can enter its detour; the pointers remain valid for process life
    // once MH_ApplyQueued has been attempted.
    g_activeOriginal.store(activeOriginal, std::memory_order_release);
    g_worldOriginal.store(worldOriginal, std::memory_order_release);

    AcquireSRWLockExclusive(&g_identityLock);
    g_identity = IdentityState{};
    ReleaseSRWLockExclusive(&g_identityLock);
    g_activeCalls.store(0, std::memory_order_relaxed);
    g_worldCalls.store(0, std::memory_order_relaxed);
    g_pairSequence.store(0, std::memory_order_relaxed);
    g_incompletePairs.store(0, std::memory_order_relaxed);
    g_lastLogTick.store(0, std::memory_order_relaxed);

    const MH_STATUS activeQueueStatus = MH_QueueEnableHook(activeTarget);
    const MH_STATUS worldQueueStatus = MH_QueueEnableHook(worldTarget);
    if (activeQueueStatus != MH_OK || worldQueueStatus != MH_OK) {
        const MH_STATUS activeRemoveStatus = MH_RemoveHook(activeTarget);
        const MH_STATUS worldRemoveStatus = MH_RemoveHook(worldTarget);
        g_activeOriginal.store(nullptr, std::memory_order_release);
        g_worldOriginal.store(nullptr, std::memory_order_release);
        char message[448]{};
        std::snprintf(
            message, sizeof(message),
            "phase timing diagnostic disabled: MinHook queue failed active=%s world=%s; disabled cleanup active=%s world=%s",
            MH_StatusToString(activeQueueStatus),
            MH_StatusToString(worldQueueStatus),
            MH_StatusToString(activeRemoveStatus),
            MH_StatusToString(worldRemoveStatus));
        Log(message);
        g_imageBase.store(0, std::memory_order_release);
        g_imageSize.store(0, std::memory_order_release);
        return false;
    }

    const MH_STATUS applyStatus = MH_ApplyQueued();
    if (applyStatus != MH_OK) {
        // ApplyQueued can fail after enabling one target. Keep both published
        // trampolines and both MinHook entries resident even if rollback also
        // fails: any surviving detour remains a permanent pass-through, and no
        // in-flight call can observe freed trampoline storage.
        g_armed.store(false, std::memory_order_release);
        const MH_STATUS activeDisableQueueStatus =
            MH_QueueDisableHook(activeTarget);
        const MH_STATUS worldDisableQueueStatus =
            MH_QueueDisableHook(worldTarget);
        const MH_STATUS rollbackStatus = MH_ApplyQueued();
        char message[448]{};
        std::snprintf(
            message, sizeof(message),
            "phase timing diagnostic disabled: atomic MinHook enable failed (%s); retained pass-through rollback active=%s world=%s apply=%s",
            MH_StatusToString(applyStatus),
            MH_StatusToString(activeDisableQueueStatus),
            MH_StatusToString(worldDisableQueueStatus),
            MH_StatusToString(rollbackStatus));
        Log(message);
        return false;
    }

    g_armed.store(true, std::memory_order_release);

    char message[320]{};
    std::snprintf(
        message, sizeof(message),
        "read-only phase timing diagnostic installed active=%p world=%p (cdecl float; two signatures and .text validated)",
        reinterpret_cast<void*>(activeComponentsTarget),
        reinterpret_cast<void*>(worldPhysicsTarget));
    Log(message);
    return true;
#endif
}

}  // namespace nfsmw_drift_asi::phase_timing_diagnostic
