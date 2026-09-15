#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "vehicle_getter_diagnostic.hpp"
#include "runtime_logging.hpp"

#include "nfsmw_drift_target_profile.hpp"

#include <nfsmw_sdk/nfsmw_sdk.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

namespace nfsmw_drift_asi::vehicle_getter_diagnostic {
namespace {

using WheelTractionFn = float (NFSMW_FASTCALL*)(void* self,
                                                void* unusedEdx,
                                                std::uint32_t index);
using DriveTorqueFn = float (NFSMW_FASTCALL*)(void* self, void* unusedEdx);

enum class GetterKind : std::uint32_t {
    Suspension = 1,
    Transmission = 2,
};

constexpr std::uintptr_t kPVehicleInstancesRva = 0x005352B0u;
constexpr std::size_t kPVehicleInstanceStride = 8;
constexpr std::size_t kPVehicleInstanceLimit = 64;
constexpr std::size_t kPVehicleReadableSize = 0x160;
constexpr std::size_t kSuspensionOffset = 0xF0;
constexpr std::size_t kTransmissionOffset = 0xFC;
constexpr std::size_t kWheelTractionSlot = 1;
constexpr std::size_t kDriveTorqueSlot = 8;
constexpr std::size_t kMaxHookRecords = 8;
constexpr std::size_t kMaxBindings = 64;
constexpr DWORD kDiscoveryIntervalMs = 500;
constexpr DWORD kLogIntervalMs = 500;
constexpr unsigned kOriginalWaitAttempts = 128;
constexpr float kMaximumDiagnosticFloatMagnitude = 100000000.0f;

std::atomic<std::uintptr_t> g_imageBase{0};
std::atomic<std::size_t> g_imageSize{0};
std::atomic<bool> g_configured{false};
std::atomic<DWORD> g_lastDiscoveryTick{0};
std::atomic<bool> g_discoveryInitialized{false};
std::atomic<DWORD> g_lastLogTick{0};

struct ObjectBinding {
    std::atomic<std::uintptr_t> object{0};
    std::atomic<std::uintptr_t> pvehicle{0};
    std::atomic<std::uint32_t> vehicleIndex{0xffffffffu};
    std::atomic<std::uint32_t> kind{0};
};

struct GetterStats {
    std::atomic<std::uintptr_t> target{0};
    std::atomic<std::uintptr_t> vtable{0};
    std::atomic<bool> attempted{false};
    std::atomic<bool> active{false};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> invalidCalls{0};
    std::atomic<std::uintptr_t> lastSelf{0};
    std::atomic<std::uintptr_t> lastObjectVtable{0};
    std::atomic<std::uintptr_t> lastPVehicle{0};
    std::atomic<std::uint32_t> lastVehicleIndex{0xffffffffu};
    std::atomic<std::uint32_t> lastArgument{0};
    std::atomic<std::uint32_t> lastValueBits{0};
    std::atomic<std::uint32_t> lastFlags{0};
    std::atomic<std::uint64_t> lastReportedCalls{0};
};

struct WheelRecord : GetterStats {
    std::unique_ptr<nfsmw::InlineHook<WheelTractionFn>> hook;
};

struct TorqueRecord : GetterStats {
    std::unique_ptr<nfsmw::InlineHook<DriveTorqueFn>> hook;
};

std::array<ObjectBinding, kMaxBindings> g_bindings{};
std::array<WheelRecord, kMaxHookRecords> g_wheelRecords{};
std::array<TorqueRecord, kMaxHookRecords> g_torqueRecords{};
std::mutex g_hookMutex;
std::mutex g_discoveryMutex;

std::atomic<bool> g_wheelChainFailureLogged{false};
std::atomic<bool> g_torqueChainFailureLogged{false};

void Log(const char* message) {
    if (message == nullptr) {
        return;
    }
    ::nfsmw_drift_asi::runtime_logging::DebugOutput("[nfsmw_drift_assist] ");
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(message);
    ::nfsmw_drift_asi::runtime_logging::DebugOutput("\n");
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

bool ValidVtable(std::uintptr_t vtable, std::size_t requiredSlot) {
    std::uintptr_t end = 0;
    const std::size_t bytes = (requiredSlot + 1) * sizeof(std::uintptr_t);
    if (!AddAddress(vtable, bytes, &end) ||
        !ImageRange(reinterpret_cast<const void*>(vtable), bytes)) {
        return false;
    }
    // Probe the first slot as well as the requested slot.  This rejects a
    // random readable pointer before any method target is considered.
    for (const std::size_t slot : {std::size_t{0}, requiredSlot}) {
        std::uintptr_t method = 0;
        std::uintptr_t slotAddress = 0;
        if (!AddAddress(vtable, slot * sizeof(std::uintptr_t), &slotAddress) ||
            !SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                             &method) ||
            !ExecutableAddress(reinterpret_cast<const void*>(method))) {
            return false;
        }
    }
    (void)end;
    return true;
}

bool ReadGetterTarget(std::uintptr_t object,
                      std::size_t slot,
                      std::uintptr_t* vtableOut,
                      std::uintptr_t* targetOut) {
    if (vtableOut == nullptr || targetOut == nullptr || object == 0 ||
        !SafeReadPointer(reinterpret_cast<const void*>(object), vtableOut) ||
        *vtableOut == 0 || !ValidVtable(*vtableOut, slot)) {
        return false;
    }
    std::uintptr_t slotAddress = 0;
    if (!AddAddress(*vtableOut, slot * sizeof(std::uintptr_t), &slotAddress) ||
        !SafeReadPointer(reinterpret_cast<const void*>(slotAddress), targetOut) ||
        !ExecutableAddress(reinterpret_cast<const void*>(*targetOut)) ||
        !ImageRange(reinterpret_cast<const void*>(*targetOut), 1)) {
        return false;
    }
    return true;
}

void PublishBinding(GetterKind kind,
                    std::uintptr_t object,
                    std::uintptr_t pvehicle,
                    std::uint32_t vehicleIndex) {
    if (object == 0) {
        return;
    }
    const std::uint32_t kindValue = static_cast<std::uint32_t>(kind);
    std::size_t freeSlot = kMaxBindings;
    for (std::size_t i = 0; i < g_bindings.size(); ++i) {
        const std::uintptr_t current =
            g_bindings[i].object.load(std::memory_order_acquire);
        if (current == object &&
            g_bindings[i].kind.load(std::memory_order_relaxed) == kindValue) {
            g_bindings[i].pvehicle.store(pvehicle, std::memory_order_relaxed);
            g_bindings[i].vehicleIndex.store(vehicleIndex,
                                             std::memory_order_relaxed);
            return;
        }
        if (current == 0 && freeSlot == kMaxBindings) {
            freeSlot = i;
        }
    }
    if (freeSlot == kMaxBindings) {
        return;
    }
    g_bindings[freeSlot].kind.store(kindValue, std::memory_order_relaxed);
    g_bindings[freeSlot].pvehicle.store(pvehicle, std::memory_order_relaxed);
    g_bindings[freeSlot].vehicleIndex.store(vehicleIndex,
                                             std::memory_order_relaxed);
    // Publish the object last so readers never observe a half-written row.
    g_bindings[freeSlot].object.store(object, std::memory_order_release);
}

bool LookupBinding(GetterKind kind,
                   std::uintptr_t object,
                   std::uintptr_t* pvehicleOut,
                   std::uint32_t* vehicleIndexOut) {
    const std::uint32_t kindValue = static_cast<std::uint32_t>(kind);
    for (const auto& binding : g_bindings) {
        if (binding.object.load(std::memory_order_acquire) != object ||
            binding.kind.load(std::memory_order_relaxed) != kindValue) {
            continue;
        }
        if (pvehicleOut != nullptr) {
            *pvehicleOut = binding.pvehicle.load(std::memory_order_relaxed);
        }
        if (vehicleIndexOut != nullptr) {
            *vehicleIndexOut =
                binding.vehicleIndex.load(std::memory_order_relaxed);
        }
        return true;
    }
    return false;
}

template <typename Record>
void RecordCall(Record& record,
                GetterKind kind,
                void* self,
                std::uint32_t argument,
                float value) {
    std::uintptr_t objectVtable = 0;
    std::uintptr_t target = record.target.load(std::memory_order_acquire);
    std::uintptr_t slotTarget = 0;
    bool objectReadable = false;
    bool slotMatches = false;
    if (self != nullptr &&
        SafeReadPointer(self, &objectVtable) && objectVtable != 0) {
        objectReadable =
            ReadGetterTarget(reinterpret_cast<std::uintptr_t>(self),
                             kind == GetterKind::Suspension ? kWheelTractionSlot
                                                            : kDriveTorqueSlot,
                             &objectVtable, &slotTarget);
        slotMatches = objectReadable && slotTarget == target;
    }

    std::uintptr_t pvehicle = 0;
    std::uint32_t vehicleIndex = 0xffffffffu;
    const bool bound = LookupBinding(kind, reinterpret_cast<std::uintptr_t>(self),
                                     &pvehicle, &vehicleIndex);
    const bool indexValid = kind == GetterKind::Transmission || argument < 8;
    const bool valueValid = std::isfinite(value) &&
                            std::fabs(value) <= kMaximumDiagnosticFloatMagnitude;
    std::uint32_t flags = 0;
    if (objectReadable) flags |= 1u;
    if (slotMatches) flags |= 2u;
    if (bound) flags |= 4u;
    if (indexValid) flags |= 8u;
    if (valueValid) flags |= 16u;

    std::uint32_t valueBits = 0;
    static_assert(sizeof(valueBits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&valueBits, &value, sizeof(valueBits));
    record.calls.fetch_add(1, std::memory_order_relaxed);
    if ((flags & 0x1fu) != 0x1fu) {
        record.invalidCalls.fetch_add(1, std::memory_order_relaxed);
    }
    record.lastSelf.store(reinterpret_cast<std::uintptr_t>(self),
                          std::memory_order_relaxed);
    record.lastObjectVtable.store(objectVtable, std::memory_order_relaxed);
    record.lastPVehicle.store(pvehicle, std::memory_order_relaxed);
    record.lastVehicleIndex.store(vehicleIndex, std::memory_order_relaxed);
    record.lastArgument.store(argument, std::memory_order_relaxed);
    record.lastValueBits.store(valueBits, std::memory_order_relaxed);
    record.lastFlags.store(flags, std::memory_order_relaxed);
}

template <typename Fn>
Fn WaitForOriginal(std::atomic<Fn>& original,
                   std::atomic<bool>& failureLogged,
                   const char* label) {
    Fn value = original.load(std::memory_order_acquire);
    for (unsigned attempt = 0; value == nullptr &&
                                attempt < kOriginalWaitAttempts;
         ++attempt) {
        SwitchToThread();
        value = original.load(std::memory_order_acquire);
    }
    if (value == nullptr &&
        !failureLogged.exchange(true, std::memory_order_relaxed)) {
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "%s getter diagnostic skipped: original trampoline unavailable",
                      label);
        Log(message);
    }
    return value;
}

// The original pointer is kept separately from the RAII object so detours can
// read it lock-free while MinHook publishes the trampoline during install.
std::array<std::atomic<WheelTractionFn>, kMaxHookRecords> g_wheelOriginal{};
std::array<std::atomic<DriveTorqueFn>, kMaxHookRecords> g_torqueOriginal{};

float DispatchWheelCall(std::size_t recordIndex,
                        void* self,
                        void* unusedEdx,
                        std::uint32_t index) {
    if (recordIndex >= g_wheelRecords.size()) {
        return 0.0f;
    }
    auto& record = g_wheelRecords[recordIndex];
    WheelTractionFn original =
        WaitForOriginal(g_wheelOriginal[recordIndex], g_wheelChainFailureLogged,
                        "suspension");
    if (original == nullptr) {
        return 0.0f;
    }
    const float value = original(self, unusedEdx, index);
    RecordCall(record, GetterKind::Suspension, self, index, value);
    return value;
}

float DispatchTorqueCall(std::size_t recordIndex,
                         void* self,
                         void* unusedEdx) {
    if (recordIndex >= g_torqueRecords.size()) {
        return 0.0f;
    }
    auto& record = g_torqueRecords[recordIndex];
    DriveTorqueFn original =
        WaitForOriginal(g_torqueOriginal[recordIndex], g_torqueChainFailureLogged,
                        "transmission");
    if (original == nullptr) {
        return 0.0f;
    }
    const float value = original(self, unusedEdx);
    RecordCall(record, GetterKind::Transmission, self, 0, value);
    return value;
}

#define NFSMW_DEFINE_WHEEL_DETOUR(n)                                          \
    float NFSMW_FASTCALL WheelDetour##n(void* self, void* unusedEdx,          \
                                        std::uint32_t index) {                 \
        return DispatchWheelCall(n, self, unusedEdx, index);                  \
    }
NFSMW_DEFINE_WHEEL_DETOUR(0)
NFSMW_DEFINE_WHEEL_DETOUR(1)
NFSMW_DEFINE_WHEEL_DETOUR(2)
NFSMW_DEFINE_WHEEL_DETOUR(3)
NFSMW_DEFINE_WHEEL_DETOUR(4)
NFSMW_DEFINE_WHEEL_DETOUR(5)
NFSMW_DEFINE_WHEEL_DETOUR(6)
NFSMW_DEFINE_WHEEL_DETOUR(7)
#undef NFSMW_DEFINE_WHEEL_DETOUR

#define NFSMW_DEFINE_TORQUE_DETOUR(n)                                         \
    float NFSMW_FASTCALL TorqueDetour##n(void* self, void* unusedEdx) {       \
        return DispatchTorqueCall(n, self, unusedEdx);                        \
    }
NFSMW_DEFINE_TORQUE_DETOUR(0)
NFSMW_DEFINE_TORQUE_DETOUR(1)
NFSMW_DEFINE_TORQUE_DETOUR(2)
NFSMW_DEFINE_TORQUE_DETOUR(3)
NFSMW_DEFINE_TORQUE_DETOUR(4)
NFSMW_DEFINE_TORQUE_DETOUR(5)
NFSMW_DEFINE_TORQUE_DETOUR(6)
NFSMW_DEFINE_TORQUE_DETOUR(7)
#undef NFSMW_DEFINE_TORQUE_DETOUR

constexpr std::array<WheelTractionFn, kMaxHookRecords> kWheelDetours = {
    &WheelDetour0, &WheelDetour1, &WheelDetour2, &WheelDetour3,
    &WheelDetour4, &WheelDetour5, &WheelDetour6, &WheelDetour7};
constexpr std::array<DriveTorqueFn, kMaxHookRecords> kTorqueDetours = {
    &TorqueDetour0, &TorqueDetour1, &TorqueDetour2, &TorqueDetour3,
    &TorqueDetour4, &TorqueDetour5, &TorqueDetour6, &TorqueDetour7};

template <typename RecordArray>
int FindRecord(const RecordArray& records, std::uintptr_t target) {
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].target.load(std::memory_order_acquire) == target) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

template <typename RecordArray>
bool HasTarget(const RecordArray& records, std::uintptr_t target) {
    return FindRecord(records, target) >= 0;
}

template <typename RecordArray>
int FindFreeRecord(const RecordArray& records) {
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (!records[i].attempted.load(std::memory_order_acquire)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int InstallWheelHook(std::uintptr_t target, std::uintptr_t vtable) {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    const int existing = FindRecord(g_wheelRecords, target);
    if (existing >= 0) {
        g_wheelRecords[static_cast<std::size_t>(existing)].vtable.store(
            vtable, std::memory_order_release);
        return existing;
    }
    const int index = FindFreeRecord(g_wheelRecords);
    if (index < 0) {
        Log("suspension getter diagnostic capacity exhausted");
        return -1;
    }
    auto& record = g_wheelRecords[static_cast<std::size_t>(index)];
    record.target.store(target, std::memory_order_release);
    record.vtable.store(vtable, std::memory_order_release);
    record.attempted.store(true, std::memory_order_release);
    auto hook = std::make_unique<nfsmw::InlineHook<WheelTractionFn>>(
        target, kWheelDetours[static_cast<std::size_t>(index)]);
    const WheelTractionFn original = hook->original();
    if (!hook->installed() || original == nullptr) {
        Log("suspension getter diagnostic hook installation failed; target skipped");
        return -1;
    }
    g_wheelOriginal[static_cast<std::size_t>(index)].store(
        original, std::memory_order_release);
    record.hook = std::move(hook);
    record.active.store(true, std::memory_order_release);
    return index;
}

int InstallTorqueHook(std::uintptr_t target, std::uintptr_t vtable) {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    const int existing = FindRecord(g_torqueRecords, target);
    if (existing >= 0) {
        g_torqueRecords[static_cast<std::size_t>(existing)].vtable.store(
            vtable, std::memory_order_release);
        return existing;
    }
    const int index = FindFreeRecord(g_torqueRecords);
    if (index < 0) {
        Log("transmission getter diagnostic capacity exhausted");
        return -1;
    }
    auto& record = g_torqueRecords[static_cast<std::size_t>(index)];
    record.target.store(target, std::memory_order_release);
    record.vtable.store(vtable, std::memory_order_release);
    record.attempted.store(true, std::memory_order_release);
    auto hook = std::make_unique<nfsmw::InlineHook<DriveTorqueFn>>(
        target, kTorqueDetours[static_cast<std::size_t>(index)]);
    const DriveTorqueFn original = hook->original();
    if (!hook->installed() || original == nullptr) {
        Log("transmission getter diagnostic hook installation failed; target skipped");
        return -1;
    }
    g_torqueOriginal[static_cast<std::size_t>(index)].store(
        original, std::memory_order_release);
    record.hook = std::move(hook);
    record.active.store(true, std::memory_order_release);
    return index;
}

void LogDiscovery(GetterKind kind,
                  std::size_t vehicleIndex,
                  std::uintptr_t pvehicle,
                  std::uintptr_t object,
                  std::uintptr_t vtable,
                  std::uintptr_t target,
                  int hookIndex) {
    char message[384]{};
    const char* label = kind == GetterKind::Suspension ? "suspension" : "transmission";
    const std::size_t slot = kind == GetterKind::Suspension ? kWheelTractionSlot
                                                             : kDriveTorqueSlot;
    std::snprintf(message, sizeof(message),
                  "getter diagnostic discovered kind=%s slot=%llu vehicle_index=%llu "
                  "pv=%p object=%p "
                  "vtable=%p target=%p hook=%s[%d]",
                  label, static_cast<unsigned long long>(slot),
                  static_cast<unsigned long long>(vehicleIndex),
                  reinterpret_cast<void*>(pvehicle),
                  reinterpret_cast<void*>(object),
                  reinterpret_cast<void*>(vtable),
                  reinterpret_cast<void*>(target), hookIndex >= 0 ? "active" : "skipped",
                  hookIndex);
    Log(message);
}

void DiscoverGetters() {
    const std::uintptr_t imageBase =
        g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t tableAddress = 0;
    if (!AddAddress(imageBase, kPVehicleInstancesRva, &tableAddress) ||
        !ImageRange(reinterpret_cast<const void*>(tableAddress),
                    sizeof(std::uintptr_t))) {
        Log("getter diagnostic skipped: PVehicle instance table is unreadable");
        return;
    }

    for (std::size_t index = 0; index < kPVehicleInstanceLimit; ++index) {
        std::uintptr_t slotAddress = 0;
        std::uintptr_t pvehicle = 0;
        if (!AddAddress(tableAddress, index * kPVehicleInstanceStride,
                        &slotAddress) ||
            !SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                             &pvehicle)) {
            break;
        }
        if (pvehicle == 0) {
            break;
        }
        if (!ReadableRange(reinterpret_cast<const void*>(pvehicle),
                           kPVehicleReadableSize)) {
            continue;
        }

        for (const auto spec : {
                 std::pair<GetterKind, std::size_t>{GetterKind::Suspension,
                                                    kSuspensionOffset},
                 std::pair<GetterKind, std::size_t>{GetterKind::Transmission,
                                                    kTransmissionOffset}}) {
            std::uintptr_t fieldAddress = 0;
            std::uintptr_t object = 0;
            if (!AddAddress(pvehicle, spec.second, &fieldAddress) ||
                !SafeReadPointer(reinterpret_cast<const void*>(fieldAddress),
                                 &object) ||
                object == 0) {
                continue;
            }
            const std::size_t getterSlot =
                spec.first == GetterKind::Suspension ? kWheelTractionSlot
                                                      : kDriveTorqueSlot;
            std::uintptr_t vtable = 0;
            std::uintptr_t target = 0;
            if (!ReadGetterTarget(object, getterSlot, &vtable, &target)) {
                continue;
            }
            PublishBinding(spec.first, object, pvehicle,
                           static_cast<std::uint32_t>(index));
            const bool newTarget =
                spec.first == GetterKind::Suspension
                    ? !HasTarget(g_wheelRecords, target)
                    : !HasTarget(g_torqueRecords, target);
            const int hookIndex =
                spec.first == GetterKind::Suspension
                    ? InstallWheelHook(target, vtable)
                    : InstallTorqueHook(target, vtable);
            // Emit discovery only when a target is first observed.  Existing
            // records are still refreshed through the binding table above.
            if (newTarget) {
                LogDiscovery(spec.first, index, pvehicle, object, vtable, target,
                             hookIndex);
            }
        }
    }
}

float BitsToFloat(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

template <typename Record>
void LogRecord(const char* label,
               std::size_t slot,
               Record& record,
               DWORD elapsedMs) {
    if (!record.attempted.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint64_t total = record.calls.load(std::memory_order_relaxed);
    const std::uint64_t previous =
        record.lastReportedCalls.exchange(total, std::memory_order_relaxed);
    const std::uint64_t delta = total - previous;
    const double rate = elapsedMs == 0
                            ? 0.0
                            : static_cast<double>(delta) * 1000.0 /
                                  static_cast<double>(elapsedMs);
    const std::uint32_t flags = record.lastFlags.load(std::memory_order_relaxed);
    const float value = BitsToFloat(
        record.lastValueBits.load(std::memory_order_relaxed));
    char message[640]{};
    std::snprintf(
        message, sizeof(message),
        "getter diagnostic kind=%s slot=%llu target=%p active=%d calls_total=%llu "
        "calls_delta=%llu rate_hz=%.2f invalid_total=%llu last_self=%p "
        "last_pvehicle=%p vehicle_index=%lu arg=%lu value=%.5f flags=0x%02lX",
        label, static_cast<unsigned long long>(slot),
        reinterpret_cast<void*>(record.target.load(std::memory_order_relaxed)),
        record.active.load(std::memory_order_relaxed) ? 1 : 0,
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(delta), rate,
        static_cast<unsigned long long>(
            record.invalidCalls.load(std::memory_order_relaxed)),
        reinterpret_cast<void*>(record.lastSelf.load(std::memory_order_relaxed)),
        reinterpret_cast<void*>(
            record.lastPVehicle.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(
            record.lastVehicleIndex.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(record.lastArgument.load(std::memory_order_relaxed)),
        static_cast<double>(value), static_cast<unsigned long>(flags));
    Log(message);
}

void LogRates() {
    const DWORD now = GetTickCount();
    DWORD previous = g_lastLogTick.load(std::memory_order_relaxed);
    if (previous != 0 && static_cast<DWORD>(now - previous) < kLogIntervalMs) {
        return;
    }
    if (!g_lastLogTick.compare_exchange_strong(previous, now,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
        return;
    }
    const DWORD elapsed = previous == 0 ? kLogIntervalMs : now - previous;
    for (std::size_t i = 0; i < g_wheelRecords.size(); ++i) {
        LogRecord("suspension", kWheelTractionSlot, g_wheelRecords[i], elapsed);
    }
    for (std::size_t i = 0; i < g_torqueRecords.size(); ++i) {
        LogRecord("transmission", kDriveTorqueSlot, g_torqueRecords[i], elapsed);
    }
}

}  // namespace

bool Configure(std::uintptr_t imageBase, std::size_t imageSize) {
    const bool profileMatches =
        imageBase == static_cast<std::uintptr_t>(target_profile::kExpectedImageBase) &&
        imageSize != 0 &&
        (target_profile::kExpectedSizeOfImage == 0 ||
         imageSize == static_cast<std::size_t>(target_profile::kExpectedSizeOfImage));
    if (!profileMatches) {
        g_configured.store(false, std::memory_order_release);
        Log("getter diagnostic disabled: PE profile/image range mismatch");
        return false;
    }
    g_imageBase.store(imageBase, std::memory_order_release);
    g_imageSize.store(imageSize, std::memory_order_release);
    g_configured.store(true, std::memory_order_release);
    return true;
}

void OnVehicleTick() {
    if (!g_configured.load(std::memory_order_acquire)) {
        return;
    }
    bool claim = false;
    {
        std::lock_guard<std::mutex> lock(g_discoveryMutex);
        const DWORD now = GetTickCount();
        const DWORD previous =
            g_lastDiscoveryTick.load(std::memory_order_relaxed);
        const bool uninitialized =
            !g_discoveryInitialized.load(std::memory_order_relaxed);
        if (uninitialized ||
            static_cast<DWORD>(now - previous) >= kDiscoveryIntervalMs) {
            g_lastDiscoveryTick.store(now, std::memory_order_relaxed);
            g_discoveryInitialized.store(true, std::memory_order_release);
            claim = true;
        }
    }
    if (claim) {
        DiscoverGetters();
    }
    LogRates();
}

}  // namespace nfsmw_drift_asi::vehicle_getter_diagnostic
