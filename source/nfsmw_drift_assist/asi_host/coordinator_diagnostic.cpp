#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC
#define NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC 0
#endif

#include "coordinator_diagnostic.hpp"
#include "runtime_logging.hpp"

#include <nfsmw_sdk/nfsmw_sdk.h>
#include <nfsmw_sdk/scan.h>

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace nfsmw_drift_asi::coordinator_diagnostic {
namespace {

#if NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC

// WorldPhysicsDispatch_MainThreadCoordinator has no stack parameters.  The
// function loads DAT_009885C8 and dispatches its virtual slot +0x44, then
// performs the remaining world update calls.
using DispatchFn = void (NFSMW_CDECL*)();

constexpr std::uintptr_t kDispatchRva = 0x0035AAD0u;
constexpr std::uintptr_t kCoordinatorGlobalRva = 0x005885C8u;
constexpr std::uintptr_t kExpectedPrimaryVtableRva = 0x004B0E18u;
// DAT_009885C8 is published as the +0x48 secondary subobject.  Its final
// runtime vtable is PTR_LAB_008B0D78; the primary vtable remains on the full
// object at (DAT_009885C8 - 0x48).
constexpr std::uintptr_t kExpectedSubobjectVtableRva = 0x004B0D78u;
constexpr std::size_t kCoordinatorVtableSlot = 0x44u / sizeof(std::uintptr_t);
constexpr std::size_t kCoordinatorSubobjectOffset = 0x48u;
constexpr DWORD kLogIntervalMs = 500;
constexpr unsigned kOriginalWaitAttempts = 128;

std::unique_ptr<nfsmw::InlineHook<DispatchFn>> g_dispatchHook;
std::atomic<DispatchFn> g_dispatchOriginal{nullptr};
std::atomic<std::uintptr_t> g_imageBase{0};
std::atomic<std::size_t> g_imageSize{0};
std::atomic<std::uintptr_t> g_textRva{0};
std::atomic<std::size_t> g_textSize{0};
std::atomic<std::uintptr_t> g_dispatchTarget{0};
std::atomic<std::uint64_t> g_dispatchCalls{0};
std::atomic<DWORD> g_lastLogTick{0};
std::atomic<std::uint64_t> g_lastLogCalls{0};
std::atomic<std::uintptr_t> g_lastLoggedSlotTarget{0};
std::atomic<bool> g_chainFailureLogged{false};
std::atomic<bool> g_readFailureLogged{false};

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

bool TextAddress(std::uintptr_t address) {
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    const std::size_t imageSize = g_imageSize.load(std::memory_order_acquire);
    const std::uintptr_t textRva = g_textRva.load(std::memory_order_acquire);
    const std::size_t textSize = g_textSize.load(std::memory_order_acquire);
    if (base == 0 || imageSize == 0 || address < base) {
        return false;
    }
    const std::uintptr_t offset = address - base;
    if (offset >= imageSize || textSize == 0 || offset < textRva ||
        offset - textRva >= textSize) {
        return false;
    }
    return
           ExecutableAddress(reinterpret_cast<const void*>(address));
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

bool SafeReadBytes(const void* address, void* value, std::size_t size) {
    if (address == nullptr || value == nullptr || size == 0 ||
        !ReadableRange(address, size)) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        std::memcpy(value, address, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(value, address, size);
#endif
    return true;
}

bool FindTextSection(std::uintptr_t moduleBase,
                     std::size_t imageSize,
                     std::uintptr_t* textRvaOut,
                     std::size_t* textSizeOut) {
    if (moduleBase == 0 || imageSize == 0 || textRvaOut == nullptr ||
        textSizeOut == nullptr) {
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    if (!SafeReadBytes(reinterpret_cast<const void*>(moduleBase), &dos,
                       sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
        return false;
    }

    std::uintptr_t ntAddress = 0;
    if (!AddAddress(moduleBase, static_cast<std::size_t>(dos.e_lfanew),
                    &ntAddress)) {
        return false;
    }
    DWORD signature = 0;
    if (!SafeReadBytes(reinterpret_cast<const void*>(ntAddress), &signature,
                       sizeof(signature)) ||
        signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    IMAGE_FILE_HEADER fileHeader{};
    const std::uintptr_t fileHeaderAddress = ntAddress + sizeof(DWORD);
    if (!SafeReadBytes(reinterpret_cast<const void*>(fileHeaderAddress),
                       &fileHeader, sizeof(fileHeader)) ||
        fileHeader.NumberOfSections == 0 || fileHeader.NumberOfSections > 96) {
        return false;
    }

    const std::uintptr_t sectionAddress =
        fileHeaderAddress + sizeof(IMAGE_FILE_HEADER) +
        static_cast<std::uintptr_t>(fileHeader.SizeOfOptionalHeader);
    const std::size_t sectionBytes =
        static_cast<std::size_t>(fileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (sectionAddress < moduleBase || sectionAddress - moduleBase > imageSize ||
        sectionBytes > imageSize - (sectionAddress - moduleBase) ||
        !ReadableRange(reinterpret_cast<const void*>(sectionAddress),
                       sectionBytes)) {
        return false;
    }

    for (unsigned index = 0; index < fileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER section{};
        const std::uintptr_t address =
            sectionAddress + static_cast<std::uintptr_t>(index) *
                                 sizeof(IMAGE_SECTION_HEADER);
        if (!SafeReadBytes(reinterpret_cast<const void*>(address), &section,
                           sizeof(section))) {
            return false;
        }
        if (std::memcmp(section.Name, ".text", 5) != 0) {
            continue;
        }

        const std::size_t virtualSize =
            static_cast<std::size_t>(section.Misc.VirtualSize);
        const std::size_t rawSize =
            static_cast<std::size_t>(section.SizeOfRawData);
        const std::size_t span = virtualSize > rawSize ? virtualSize : rawSize;
        const std::size_t rva = static_cast<std::size_t>(section.VirtualAddress);
        if (span == 0 || rva >= imageSize || span > imageSize - rva) {
            return false;
        }
        *textRvaOut = static_cast<std::uintptr_t>(rva);
        *textSizeOut = span;
        return true;
    }
    return false;
}

struct Snapshot {
    std::uintptr_t globalAddress = 0;
    // The value stored at DAT_009885C8.  In this target it is the address of
    // the coordinator's +0x48 secondary subobject, not the primary object.
    std::uintptr_t subobject = 0;
    std::uintptr_t fullObject = 0;
    std::uintptr_t primaryVtable = 0;
    std::uintptr_t subobjectVtable = 0;
    std::uintptr_t slotTarget = 0;
    bool globalReadable = false;
    bool subobjectReadable = false;
    bool primaryVtableReadable = false;
    bool subobjectVtableReadable = false;
    bool slotTargetInText = false;
    bool primaryVtableMatches = false;
    bool subobjectVtableMatches = false;
    bool fullObjectReadable = false;
};

bool ReadSnapshot(Snapshot* snapshot) {
    if (snapshot == nullptr) {
        return false;
    }
    *snapshot = Snapshot{};

    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    std::uintptr_t globalAddress = 0;
    if (base == 0 ||
        !AddAddress(base, kCoordinatorGlobalRva, &globalAddress) ||
        !ImageRange(reinterpret_cast<const void*>(globalAddress),
                    sizeof(std::uintptr_t)) ||
        !SafeReadPointer(reinterpret_cast<const void*>(globalAddress),
                         &snapshot->subobject)) {
        return false;
    }
    snapshot->globalAddress = globalAddress;
    snapshot->globalReadable = true;
    if (snapshot->subobject == 0 ||
        !ReadableRange(reinterpret_cast<const void*>(snapshot->subobject),
                       sizeof(std::uintptr_t))) {
        return true;
    }
    snapshot->subobjectReadable = true;

    if (!SafeReadPointer(reinterpret_cast<const void*>(snapshot->subobject),
                         &snapshot->subobjectVtable)) {
        return true;
    }
    const std::size_t subobjectSlotBytes =
        (kCoordinatorVtableSlot + 1) * sizeof(std::uintptr_t);
    snapshot->subobjectVtableReadable =
        snapshot->subobjectVtable != 0 &&
        ImageRange(reinterpret_cast<const void*>(snapshot->subobjectVtable),
                   subobjectSlotBytes);
    if (!snapshot->subobjectVtableReadable) {
        return true;
    }

    std::uintptr_t slotAddress = 0;
    if (!AddAddress(snapshot->subobjectVtable,
                    kCoordinatorVtableSlot * sizeof(std::uintptr_t),
                    &slotAddress) ||
        !SafeReadPointer(reinterpret_cast<const void*>(slotAddress),
                         &snapshot->slotTarget)) {
        return true;
    }
    snapshot->slotTargetInText = TextAddress(snapshot->slotTarget);
    snapshot->subobjectVtableMatches =
        snapshot->subobjectVtable == base + kExpectedSubobjectVtableRva;

    if (snapshot->subobject >= kCoordinatorSubobjectOffset) {
        snapshot->fullObject = snapshot->subobject -
                               kCoordinatorSubobjectOffset;
        if (ReadableRange(reinterpret_cast<const void*>(snapshot->fullObject),
                          sizeof(std::uintptr_t)) &&
            SafeReadPointer(reinterpret_cast<const void*>(snapshot->fullObject),
                            &snapshot->primaryVtable)) {
            snapshot->fullObjectReadable = true;
            snapshot->primaryVtableMatches =
                snapshot->primaryVtable == base + kExpectedPrimaryVtableRva;
            snapshot->primaryVtableReadable =
                ImageRange(reinterpret_cast<const void*>(snapshot->primaryVtable),
                           sizeof(std::uintptr_t));
        }
    }
    return true;
}

void LogSnapshotIfDue() {
    Snapshot snapshot{};
    if (!ReadSnapshot(&snapshot)) {
        if (!g_readFailureLogged.exchange(true, std::memory_order_relaxed)) {
            Log("coordinator diagnostic skipped: singleton/vtable read failed");
        }
        return;
    }

    const std::uint64_t calls =
        g_dispatchCalls.load(std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    const DWORD previousTick = g_lastLogTick.load(std::memory_order_relaxed);
    const std::uintptr_t previousTarget =
        g_lastLoggedSlotTarget.load(std::memory_order_relaxed);
    const bool targetChanged = snapshot.slotTarget != previousTarget;
    const bool intervalElapsed =
        static_cast<DWORD>(now - previousTick) >= kLogIntervalMs;
    if (!targetChanged && !intervalElapsed) {
        return;
    }

    if (intervalElapsed) {
        DWORD expected = previousTick;
        if (!g_lastLogTick.compare_exchange_strong(
                expected, now, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return;
        }
    }
    g_lastLoggedSlotTarget.store(snapshot.slotTarget, std::memory_order_relaxed);

    const std::uint64_t previousCalls =
        g_lastLogCalls.exchange(calls, std::memory_order_relaxed);
    const DWORD elapsed = static_cast<DWORD>(now - previousTick);
    const double frequency = elapsed == 0
                                 ? 0.0
                                 : static_cast<double>(calls - previousCalls) *
                                       1000.0 / static_cast<double>(elapsed);
    const std::uintptr_t base = g_imageBase.load(std::memory_order_acquire);
    const auto rva = [base](std::uintptr_t address) -> unsigned long {
        if (base == 0 || address < base) {
            return 0;
        }
        return static_cast<unsigned long>(address - base);
    };

    char line[1024]{};
    std::snprintf(
        line, sizeof(line),
        "coordinator diagnostic calls=%llu freq=%.2fHz global=%p value=%p full=%p "
        "subVtable=%p(vrva=0x%08lX expected=%d) primaryVtable=%p(vrva=0x%08lX expected=%d) "
        "slot17=%p(rva=0x%08lX text=%d) valid=%d",
        static_cast<unsigned long long>(calls), frequency,
        reinterpret_cast<void*>(snapshot.globalAddress),
        reinterpret_cast<void*>(snapshot.subobject),
        reinterpret_cast<void*>(snapshot.fullObject),
        reinterpret_cast<void*>(snapshot.subobjectVtable),
        rva(snapshot.subobjectVtable),
        snapshot.subobjectVtableMatches ? 1 : 0,
        reinterpret_cast<void*>(snapshot.primaryVtable),
        rva(snapshot.primaryVtable), snapshot.primaryVtableMatches ? 1 : 0,
        reinterpret_cast<void*>(snapshot.slotTarget), rva(snapshot.slotTarget),
        snapshot.slotTargetInText ? 1 : 0,
        snapshot.globalReadable && snapshot.subobjectReadable &&
                snapshot.subobjectVtableReadable &&
                snapshot.fullObjectReadable && snapshot.primaryVtableReadable &&
                snapshot.slotTargetInText
            ? 1
            : 0);
    Log(line);
}

void NFSMW_CDECL DispatchDetour() {
    g_dispatchCalls.fetch_add(1, std::memory_order_relaxed);
    // Capture the slot before the original function invokes it.  This is a
    // read-only observation; the unknown target is never called by the mod.
    LogSnapshotIfDue();

    DispatchFn original =
        g_dispatchOriginal.load(std::memory_order_acquire);
    for (unsigned attempt = 0; original == nullptr &&
                                attempt < kOriginalWaitAttempts; ++attempt) {
        SwitchToThread();
        original = g_dispatchOriginal.load(std::memory_order_acquire);
    }
    if (original == nullptr) {
        if (!g_chainFailureLogged.exchange(true, std::memory_order_relaxed)) {
            Log("coordinator diagnostic skipped: original dispatcher trampoline is unavailable");
        }
        return;
    }
    original();
}

#endif  // NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC

}  // namespace

bool Install(std::uintptr_t validatedTarget) {
#if !NFSMW_ENABLE_COORDINATOR_DIAGNOSTIC
    (void)validatedTarget;
    return false;
#else
    HMODULE module = GetModuleHandleA(nullptr);
    std::uintptr_t moduleBase = 0;
    std::size_t imageSize = 0;
    if (module == nullptr || !nfsmw_main_module_range(&moduleBase, &imageSize) ||
        moduleBase != reinterpret_cast<std::uintptr_t>(module) ||
        imageSize == 0 ||
        validatedTarget != moduleBase + kDispatchRva ||
        !ExecutableAddress(reinterpret_cast<const void*>(validatedTarget))) {
        Log("coordinator diagnostic disabled: validated dispatcher target or image range is invalid");
        return false;
    }

    std::uintptr_t textRva = 0;
    std::size_t textSize = 0;
    if (!FindTextSection(moduleBase, imageSize, &textRva, &textSize)) {
        Log("coordinator diagnostic disabled: .text section could not be validated");
        return false;
    }
    const std::uintptr_t targetOffset = validatedTarget - moduleBase;
    if (targetOffset < textRva || targetOffset - textRva >= textSize) {
        Log("coordinator diagnostic disabled: dispatcher anchor is outside .text");
        return false;
    }

    g_imageBase.store(moduleBase, std::memory_order_release);
    g_imageSize.store(imageSize, std::memory_order_release);
    g_textRva.store(textRva, std::memory_order_release);
    g_textSize.store(textSize, std::memory_order_release);
    g_dispatchTarget.store(validatedTarget, std::memory_order_release);

    auto hook = std::make_unique<nfsmw::InlineHook<DispatchFn>>(
        validatedTarget, &DispatchDetour);
    const DispatchFn original = hook->original();
    if (!hook->installed() || original == nullptr) {
        Log("coordinator diagnostic disabled: MinHook installation failed");
        g_dispatchTarget.store(0, std::memory_order_release);
        g_textRva.store(0, std::memory_order_release);
        g_textSize.store(0, std::memory_order_release);
        g_imageBase.store(0, std::memory_order_release);
        g_imageSize.store(0, std::memory_order_release);
        return false;
    }

    g_dispatchOriginal.store(original, std::memory_order_release);
    g_dispatchHook = std::move(hook);
    char message[192]{};
    std::snprintf(message, sizeof(message),
                  "read-only coordinator diagnostic installed at 0x%p (cdecl no-args; slot17 is observed only)",
                  reinterpret_cast<void*>(validatedTarget));
    Log(message);
    return true;
#endif
}

}  // namespace nfsmw_drift_asi::coordinator_diagnostic
