#include "version_guard.hpp"

#include <windows.h>
#include <winnt.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace nfsmw_drift::asi_host {
namespace {

constexpr std::uintptr_t kPreferredImageBase = 0x00400000u;

bool EndsWithSpeedExe(const char* path, std::size_t length) {
    constexpr char kName[] = "speed.exe";
    constexpr std::size_t kNameLength = sizeof(kName) - 1;
    if (path == nullptr || length < kNameLength) {
        return false;
    }

    const char* tail = path + length - kNameLength;
    for (std::size_t index = 0; index < kNameLength; ++index) {
        char value = tail[index];
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
        if (value != kName[index]) {
            return false;
        }
    }
    return true;
}

void ReadOnDiskSize(TargetFingerprint& result) {
    if (result.modulePath[0] == '\0') {
        return;
    }

    HANDLE file = CreateFileA(result.modulePath, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 0) {
        result.fileSizeKnown = true;
        result.onDiskFileSize = static_cast<std::uint64_t>(size.QuadPart);
        result.fileSize = result.onDiskFileSize == kV13EnglishFileSize;
    }
    CloseHandle(file);
}

} // namespace

TargetFingerprint InspectMainModule() {
    TargetFingerprint result{};
    HMODULE module = GetModuleHandleA(nullptr);
    if (module == nullptr) {
        return result;
    }

    result.moduleBase = reinterpret_cast<std::uintptr_t>(module);
    result.loadedBase = result.moduleBase == kPreferredImageBase;

    char* path = result.modulePath;
    const DWORD pathLength = GetModuleFileNameA(
        module, path, static_cast<DWORD>(sizeof(result.modulePath)));
    if (pathLength != 0 && pathLength < sizeof(result.modulePath)) {
        result.fileName = EndsWithSpeedExe(path, pathLength);
    }
    ReadOnDiskSize(result);

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        dos->e_lfanew > 0x100000) {
        return result;
    }
    result.dosHeader = true;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return result;
    }
    result.peHeader = true;
    result.i386 = nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386;
    result.pe32 = nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    if (!result.pe32) {
        return result;
    }

    result.preferredImageBase =
        nt->OptionalHeader.ImageBase == kPreferredImageBase;
    result.entryPointRva = nt->OptionalHeader.AddressOfEntryPoint;
    const std::uintptr_t entryPoint = result.moduleBase + result.entryPointRva;
    result.entryPointVa = static_cast<std::uint32_t>(entryPoint);
    result.entryPoint = result.entryPointVa == kV13EnglishEntryPointVa;
    result.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    return result;
}

bool IsKnownV13English(const TargetFingerprint& fingerprint) {
    return fingerprint.dosHeader && fingerprint.peHeader &&
           fingerprint.pe32 && fingerprint.i386 && fingerprint.loadedBase &&
           fingerprint.preferredImageBase && fingerprint.fileName &&
           fingerprint.entryPoint && fingerprint.fileSizeKnown &&
           fingerprint.fileSize;
}

const char* FirstFailure(const TargetFingerprint& fingerprint) {
    if (!fingerprint.dosHeader) return "DOS header";
    if (!fingerprint.peHeader) return "PE header";
    if (!fingerprint.pe32) return "PE32 format";
    if (!fingerprint.i386) return "i386 machine";
    if (!fingerprint.loadedBase) return "loaded image base";
    if (!fingerprint.preferredImageBase) return "preferred image base";
    if (!fingerprint.fileName) return "speed.exe file name";
    if (!fingerprint.entryPoint) return "entry point";
    if (!fingerprint.fileSizeKnown) return "on-disk file size (unreadable)";
    if (!fingerprint.fileSize) return "v1.3 English file size";
    return "unknown";
}

} // namespace nfsmw_drift::asi_host
