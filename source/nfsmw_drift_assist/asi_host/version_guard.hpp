#pragma once

#include <cstdint>

namespace nfsmw_drift::asi_host {

// These values identify the retail v1.3 English executable used by the
// public NFSMW address tables.  They are deliberately kept in the guard, not
// in the controller, so an unknown repack cannot receive memory writes.
constexpr std::uint32_t kV13EnglishEntryPointVa = 0x007C4040u;
constexpr std::uint64_t kV13EnglishFileSize = 6029312ull; // 0x5C0000

struct TargetFingerprint {
    bool dosHeader = false;
    bool peHeader = false;
    bool pe32 = false;
    bool i386 = false;
    bool loadedBase = false;
    bool preferredImageBase = false;
    bool fileName = false;
    bool entryPoint = false;
    bool fileSizeKnown = false;
    bool fileSize = false;

    std::uintptr_t moduleBase = 0;
    std::uint32_t entryPointRva = 0;
    std::uint32_t entryPointVa = 0;
    std::uint32_t sizeOfImage = 0;
    std::uint64_t onDiskFileSize = 0;
    char modulePath[1024]{};
};

// Inspect the process main module without changing process state.
TargetFingerprint InspectMainModule();

// Returns true only for the known v1.3 English fingerprint.  Every fixed
// address writer must be gated by this result (and by its own AOB check).
bool IsKnownV13English(const TargetFingerprint& fingerprint);

// Human-readable first failure for the bootstrap log.
const char* FirstFailure(const TargetFingerprint& fingerprint);

} // namespace nfsmw_drift::asi_host
