#include "GameAccess.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace game_access {
namespace {

using BStringHashFn = std::uint32_t(__cdecl*)(const char*);
using ClassGetDefinitionFn =
    const void*(__thiscall*)(const void* collectionClass, std::uint32_t key);

constexpr std::uint32_t kTransmissionClassKey = 0x07A7A3E5u;
constexpr std::uint32_t kDefaultCollectionKey = 0xEEC2271Au;
constexpr std::size_t kMaxCodeSignatureBytes = 32;

const std::uint8_t kBStringHashBytes[] = {
    0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x74, 0x23,
};

const std::uint8_t kClassGetDefinitionBytes[] = {
    0x83, 0xEC, 0x10, 0x8B, 0x44, 0x24, 0x14, 0x56,
};

void SetReason(char* reason, std::size_t reasonSize, const char* format, ...) {
    if (reason == nullptr || reasonSize == 0) return;
    va_list args;
    va_start(args, format);
    _vsnprintf_s(reason, reasonSize, _TRUNCATE, format, args);
    va_end(args);
}

bool HasReadableProtection(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const DWORD basic = protection & 0xFFu;
    return basic == PAGE_READONLY || basic == PAGE_READWRITE ||
           basic == PAGE_WRITECOPY || basic == PAGE_EXECUTE_READ ||
           basic == PAGE_EXECUTE_READWRITE ||
           basic == PAGE_EXECUTE_WRITECOPY;
}

bool HasWritableProtection(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const DWORD basic = protection & 0xFFu;
    return basic == PAGE_READWRITE || basic == PAGE_WRITECOPY ||
           basic == PAGE_EXECUTE_READWRITE ||
           basic == PAGE_EXECUTE_WRITECOPY;
}

bool HasRequiredProtection(const void* address, std::size_t size,
                           bool requireWritable) {
    if (address == nullptr || size == 0) return false;

    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
    if (size > UINTPTR_MAX - begin) return false;
    const std::uintptr_t end = begin + size;
    std::uintptr_t cursor = begin;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info,
                         sizeof(info)) != sizeof(info)) {
            return false;
        }
        const bool protectionOk =
            requireWritable ? HasWritableProtection(info.Protect)
                            : HasReadableProtection(info.Protect);
        if (info.State != MEM_COMMIT || !protectionOk) return false;

        const std::uintptr_t region =
            reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize > UINTPTR_MAX - region) return false;
        const std::uintptr_t next = region + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

bool ReadFileSize(const char* path, std::uint64_t* size) {
    HANDLE file = CreateFileA(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER value{};
    const BOOL ok = GetFileSizeEx(file, &value);
    CloseHandle(file);
    if (!ok || value.QuadPart < 0) return false;
    *size = static_cast<std::uint64_t>(value.QuadPart);
    return true;
}

bool ComputeFileMd5Impl(const char* path, char output[33]) {
    if (path == nullptr || output == nullptr) return false;
    output[0] = '\0';

    HANDLE file = INVALID_HANDLE_VALUE;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool success = false;
    BYTE buffer[16 * 1024] = {};
    BYTE digest[16] = {};

    do {
        file = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
        if (file == INVALID_HANDLE_VALUE) break;
        if (!CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_FULL,
                                  CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            break;
        }
        if (!CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)) break;

        for (;;) {
            DWORD bytesRead = 0;
            if (!ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr)) {
                break;
            }
            if (bytesRead == 0) {
                DWORD digestSize = sizeof(digest);
                if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize,
                                       0) ||
                    digestSize != sizeof(digest)) {
                    break;
                }

                constexpr char digits[] = "0123456789ABCDEF";
                for (std::size_t i = 0; i < sizeof(digest); ++i) {
                    output[i * 2] = digits[digest[i] >> 4];
                    output[i * 2 + 1] = digits[digest[i] & 0x0Fu];
                }
                output[32] = '\0';
                success = true;
                break;
            }
            if (!CryptHashData(hash, buffer, bytesRead, 0)) break;
        }
    } while (false);

    if (hash != 0) CryptDestroyHash(hash);
    if (provider != 0) CryptReleaseContext(provider, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!success) output[0] = '\0';
    return success;
}

bool HasExpectedBytes(std::uintptr_t address, const std::uint8_t* expected,
                      std::size_t size) {
    std::uint8_t actual[kMaxCodeSignatureBytes] = {};
    if (size > sizeof(actual) || !SafeRead(reinterpret_cast<const void*>(address),
                                           actual, size)) {
        return false;
    }
    return std::memcmp(actual, expected, size) == 0;
}

bool VerifyCodeSignature(std::uintptr_t address,
                         const std::uint8_t* expected,
                         std::size_t size,
                         const char* name,
                         char* reason,
                         std::size_t reasonSize) {
    if (HasExpectedBytes(address, expected, size)) return true;
    SetReason(reason, reasonSize, "%s bytes at 0x%08X do not match", name,
              static_cast<unsigned>(address));
    return false;
}

bool VerifyHasher() {
#if defined(_MSC_VER)
    __try {
        return BStringHash("transmission") == kTransmissionClassKey &&
               BStringHash("default") == kDefaultCollectionKey &&
               BStringHash("GEAR_RATIO") == kGearRatioKey &&
               BStringHash("GEAR_EFFICIENCY") == kGearEfficiencyKey;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return BStringHash("transmission") == kTransmissionClassKey &&
           BStringHash("default") == kDefaultCollectionKey &&
           BStringHash("GEAR_RATIO") == kGearRatioKey &&
           BStringHash("GEAR_EFFICIENCY") == kGearEfficiencyKey;
#endif
}

}  // namespace

const std::uint8_t kTransmissionCtorBytes[7] = {
    0x6A, 0xFF, 0x68, 0x58, 0xCB, 0x87, 0x00,
};

const std::uint8_t kPrivateCountBytes[5] = {
    0x0F, 0xB7, 0x41, 0x02, 0xC3,
};

const std::uint8_t kPrivateGetElementBytes[12] = {
    0x0F, 0xB7, 0x41, 0x02, 0x8B, 0x54,
    0x24, 0x04, 0x3B, 0xD0, 0x73, 0x3B,
};

const std::uint8_t kShiftGearBytes[23] = {
    0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x60, 0x01, 0x00,
    0x00, 0x57, 0xE8, 0x61, 0x08, 0xDC, 0xFF, 0x8B,
    0x7C, 0x24, 0x0C, 0x3B, 0xF8, 0x73, 0x51,
};

const std::uint8_t kGearRatioGetterBytes[14] = {
    0x56, 0x8B, 0xB1, 0x60, 0x01, 0x00, 0x00,
    0x8B, 0xCE, 0xE8, 0x72, 0x0E, 0xDC, 0xFF,
};

const std::uint8_t kGearEfficiencyGetterBytes[15] = {
    0x56, 0x8B, 0xB1, 0x60, 0x01, 0x00, 0x00, 0x8D,
    0x4E, 0x40, 0xE8, 0x41, 0x0E, 0xDC, 0xFF,
};

const std::uint8_t kGearRatioPointerBytes[17] = {
    0x56, 0x57, 0x8B, 0xF9, 0x8B, 0xB7, 0x60, 0x01, 0x00,
    0x00, 0x8B, 0xCE, 0xE8, 0xFF, 0x0D, 0xDC, 0xFF,
};

const std::uint8_t kEffectiveGearRatioBytes[14] = {
    0x56, 0x8B, 0xB1, 0x60, 0x01, 0x00, 0x00,
    0x8B, 0xCE, 0xE8, 0x62, 0x0D, 0xDC, 0xFF,
};

const std::uint8_t kTransmissionUpdateBytes[20] = {
    0x83, 0xEC, 0x0C, 0x56, 0x8B, 0x74, 0x24, 0x14, 0x83, 0xFE,
    0x01, 0x57, 0x8B, 0xF9, 0x0F, 0x8E, 0x03, 0x01, 0x00, 0x00,
};

const std::uint8_t kEffectiveRatioHelperBytes[17] = {
    0x56, 0x57, 0x8B, 0xF9, 0x8B, 0xB7, 0x60, 0x01, 0x00,
    0x00, 0x8B, 0xCE, 0xE8, 0x1F, 0xFA, 0xDB, 0xFF,
};

const std::uint8_t kAdditionalShiftDecisionBytes[15] = {
    0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x34, 0x8B, 0x01,
    0xFF, 0x50, 0x24, 0x84, 0xC0, 0x75, 0x0A,
};

// 0x00401AE0 is the game's three-float clamp/interpolation helper.  Keep the
// complete first basic block, including the global-constant load, so a build
// with a changed calling convention or helper body is rejected before Phase1
// can call it directly.
const std::uint8_t kShiftCurveBytes[16] = {
    0xD9, 0x44, 0x24, 0x0C, 0xD8, 0x64, 0x24, 0x08,
    0xD8, 0x15, 0x18, 0x05, 0x89, 0x00, 0xDF, 0xE0,
};

// Exact v1.3-English-Collectors entry signatures for the high-gear
// consumers. Relative call/jump displacements are intentionally included:
// VerifySupportedBuild is only allowed to accept the matching executable
// image, and a changed displacement indicates that the detour ABI may have
// moved with the function body.
const std::uint8_t kGearRatioPairBytes[20] = {
    0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8, 0x01, 0x7C, 0x1D, 0x8B,
    0x54, 0x24, 0x08, 0x83, 0xFA, 0x01, 0x7E, 0x14, 0x3B, 0xD0,
};

const std::uint8_t kTransmissionShiftBytes[19] = {
    0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0xA8, 0x00, 0x00, 0x00, 0x57,
    0xE8, 0x41, 0x10, 0xDB, 0xFF, 0x8B, 0x7C, 0x24, 0x0C,
};

const std::uint8_t kTransmissionGetTopGearBytes[13] = {
    0x8B, 0x89, 0xA8, 0x00, 0x00, 0x00, 0xE8, 0x15, 0x27, 0xDA,
    0xFF, 0x48, 0xC3,
};

const std::uint8_t kAlternateTransmissionShiftBytes[19] = {
    0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0xA4, 0x00, 0x00, 0x00, 0x57,
    0xE8, 0x51, 0xF6, 0xDA, 0xFF, 0x8B, 0x7C, 0x24, 0x0C,
};

const std::uint8_t kAlternateTransmissionGetTopGearBytes[13] = {
    0x8B, 0x89, 0xA4, 0x00, 0x00, 0x00, 0xE8, 0xA5, 0x26, 0xDA,
    0xFF, 0x48, 0xC3,
};

const std::uint8_t kAutomaticShiftDecisionBytes[15] = {
    0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x34, 0x8B, 0x01, 0xFF, 0x50,
    0x24, 0x84, 0xC0, 0x75, 0x15,
};

const std::uint8_t kAutomaticShiftUpdateBytes[17] = {
    0x83, 0xEC, 0x14, 0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x68, 0x85,
    0xC9, 0x0F, 0x84, 0x92, 0x01, 0x00, 0x00,
};

const std::uint8_t kAutomaticShiftControllerBytes[17] = {
    0x83, 0xEC, 0x14, 0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x64, 0x85,
    0xC9, 0x0F, 0x84, 0x46, 0x01, 0x00, 0x00,
};

const std::uint8_t kDerivedShiftValueBytes[14] = {
    0x8B, 0x44, 0x24, 0x04, 0xD9, 0x84, 0x81, 0x9C,
    0x00, 0x00, 0x00, 0xC2, 0x04, 0x00,
};

const std::uint8_t kDerivedEfficiencyValueBytes[14] = {
    0x8B, 0x44, 0x24, 0x04, 0xD9, 0x84, 0x81, 0xC4,
    0x00, 0x00, 0x00, 0xC2, 0x04, 0x00,
};

const std::uint8_t kLayoutRatioGetterBytes[14] = {
    0x56, 0x8B, 0xB1, 0xF4, 0x00, 0x00, 0x00, 0x8B,
    0xCE, 0xE8, 0x42, 0x1A, 0xDB, 0xFF,
};

const std::uint8_t kLayoutEfficiencyGetterBytes[15] = {
    0x56, 0x8B, 0xB1, 0xF4, 0x00, 0x00, 0x00, 0x8D,
    0x4E, 0x40, 0xE8, 0x11, 0x1A, 0xDB, 0xFF,
};

const std::uint8_t kLayoutEffectiveRatioBytes[14] = {
    0x56, 0x8B, 0xB1, 0xF4, 0x00, 0x00, 0x00, 0x8B,
    0xCE, 0xE8, 0xD2, 0x19, 0xDB, 0xFF,
};

const std::uint8_t kTorqueEfficiencyRatioBytes[15] = {
    0x53, 0x56, 0x8B, 0xD9, 0x57, 0x8B, 0xBB, 0xF4,
    0x00, 0x00, 0x00, 0x8D, 0x4F, 0x40, 0xE8,
};

const std::uint8_t kControllerRatioGetterBytes[14] = {
    0x56, 0x8B, 0xB1, 0xF0, 0x00, 0x00, 0x00, 0x8B,
    0xCE, 0xE8, 0x82, 0xFF, 0xDA, 0xFF,
};

const std::uint8_t kControllerEfficiencyGetterBytes[15] = {
    0x56, 0x8B, 0xB1, 0xF0, 0x00, 0x00, 0x00, 0x8D,
    0x4E, 0x40, 0xE8, 0x51, 0xFF, 0xDA, 0xFF,
};

const std::uint8_t kControllerEffectiveRatioBytes[14] = {
    0x56, 0x8B, 0xB1, 0xF0, 0x00, 0x00, 0x00, 0x8B,
    0xCE, 0xE8, 0x22, 0xFF, 0xDA, 0xFF,
};

static_assert(sizeof(kShiftGearBytes) <= kMaxCodeSignatureBytes,
              "code-signature buffer is too small");
static_assert(sizeof(kGearRatioGetterBytes) <= kMaxCodeSignatureBytes &&
                  sizeof(kGearEfficiencyGetterBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kGearRatioPointerBytes) <= kMaxCodeSignatureBytes &&
                  sizeof(kEffectiveGearRatioBytes) <= kMaxCodeSignatureBytes &&
                  sizeof(kTransmissionUpdateBytes) <= kMaxCodeSignatureBytes &&
                  sizeof(kEffectiveRatioHelperBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kAdditionalShiftDecisionBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kShiftCurveBytes) <= kMaxCodeSignatureBytes &&
                  sizeof(kGearRatioPairBytes) <= kMaxCodeSignatureBytes &&
                  sizeof(kTransmissionShiftBytes) <= kMaxCodeSignatureBytes &&
                  sizeof(kTransmissionGetTopGearBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kAlternateTransmissionShiftBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kAlternateTransmissionGetTopGearBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kAutomaticShiftDecisionBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kAutomaticShiftUpdateBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kAutomaticShiftControllerBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kDerivedShiftValueBytes) <= kMaxCodeSignatureBytes &&
                  sizeof(kDerivedEfficiencyValueBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kLayoutRatioGetterBytes) <= kMaxCodeSignatureBytes &&
                  sizeof(kLayoutEfficiencyGetterBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kLayoutEffectiveRatioBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kTorqueEfficiencyRatioBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kControllerRatioGetterBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kControllerEfficiencyGetterBytes) <=
                      kMaxCodeSignatureBytes &&
                  sizeof(kControllerEffectiveRatioBytes) <=
                      kMaxCodeSignatureBytes,
              "Phase 4 code-signature buffer is too small");

bool ComputeFileMd5(const char* path, char output[33]) {
    return ComputeFileMd5Impl(path, output);
}

bool SafeRead(const void* address, void* destination, std::size_t size) {
    if (address == nullptr || destination == nullptr || size == 0) return false;
    if (!HasRequiredProtection(address, size, false)) return false;

#if defined(_MSC_VER)
    __try {
        std::memcpy(destination, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination, address, size);
    return true;
#endif
}

bool IsWritable(const void* address, std::size_t size) {
    return HasRequiredProtection(address, size, true);
}

bool SafeWrite(void* address, const void* source, std::size_t size) {
    if (address == nullptr || source == nullptr || size == 0 ||
        !IsWritable(address, size)) {
        return false;
    }

#if defined(_MSC_VER)
    __try {
        std::memcpy(address, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(address, source, size);
    return true;
#endif
}

std::uint32_t BStringHash(const char* text) {
    if (text == nullptr || *text == '\0') return 0;
    return reinterpret_cast<BStringHashFn>(kBStringHash)(text);
}

bool ReadAttributeDefinition(const void* collectionClass, std::uint32_t key,
                             AttributeDefinition* definition,
                             const void** definitionAddress) {
    if (definitionAddress != nullptr) *definitionAddress = nullptr;
    if (collectionClass == nullptr || definition == nullptr) return false;

    const void* address = nullptr;
#if defined(_MSC_VER)
    __try {
        address = reinterpret_cast<ClassGetDefinitionFn>(kClassGetDefinition)(
            collectionClass, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    address = reinterpret_cast<ClassGetDefinitionFn>(kClassGetDefinition)(
        collectionClass, key);
#endif
    if (address == nullptr || !SafeRead(address, definition,
                                        sizeof(*definition))) {
        return false;
    }
    if (definitionAddress != nullptr) *definitionAddress = address;
    return true;
}

bool QueryExecutableFingerprint(ExecutableFingerprint* fingerprint,
                                char* reason, std::size_t reasonSize) {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    if (fingerprint == nullptr) {
        SetReason(reason, reasonSize, "fingerprint destination is null");
        return false;
    }
    *fingerprint = {};

    HMODULE module = GetModuleHandleA(nullptr);
    if (module == nullptr) {
        SetReason(reason, reasonSize, "GetModuleHandleA(NULL) failed");
        return false;
    }
    fingerprint->loadedBase = reinterpret_cast<std::uintptr_t>(module);

    const DWORD length =
        GetModuleFileNameA(nullptr, fingerprint->path,
                           static_cast<DWORD>(sizeof(fingerprint->path)));
    if (length == 0 || length >= sizeof(fingerprint->path)) {
        SetReason(reason, reasonSize, "could not resolve the main-module path");
        return false;
    }
    if (!ReadFileSize(fingerprint->path, &fingerprint->fileSize)) {
        SetReason(reason, reasonSize, "could not read the main-module file size");
        return false;
    }
    if (!ComputeFileMd5(fingerprint->path, fingerprint->md5)) {
        SetReason(reason, reasonSize, "could not calculate the main-module MD5");
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    if (!SafeRead(module, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 0x1000) {
        SetReason(reason, reasonSize, "invalid main-module DOS header");
        return false;
    }

    IMAGE_NT_HEADERS32 nt{};
    if (!SafeRead(reinterpret_cast<const std::uint8_t*>(module) + dos.e_lfanew,
                  &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        SetReason(reason, reasonSize, "main module does not have a PE32 header");
        return false;
    }

    fingerprint->timeDateStamp = nt.FileHeader.TimeDateStamp;
    fingerprint->imageSize = nt.OptionalHeader.SizeOfImage;
    fingerprint->preferredImageBase = nt.OptionalHeader.ImageBase;
    fingerprint->entryPointRva = nt.OptionalHeader.AddressOfEntryPoint;
    fingerprint->machine = nt.FileHeader.Machine;
    fingerprint->optionalHeaderMagic = nt.OptionalHeader.Magic;
    return true;
}

BuildProfile IdentifyBuildProfile(
    const ExecutableFingerprint& fingerprint) noexcept {
    if (fingerprint.fileSize == kReferenceFileSize &&
        _stricmp(fingerprint.md5, kReferenceMd5) == 0) {
        return BuildProfile::ReferenceC051;
    }
    if (fingerprint.fileSize == kV13EnglishCollectorsFileSize &&
        _stricmp(fingerprint.md5, kV13EnglishCollectorsMd5) == 0) {
        return BuildProfile::V13EnglishCollectors;
    }
    if (fingerprint.fileSize == kReforgedFileSize &&
        _stricmp(fingerprint.md5, kReforgedMd5) == 0) {
        return BuildProfile::ReforgedC5C5;
    }
    return BuildProfile::Unsupported;
}

const char* BuildProfileName(BuildProfile profile) noexcept {
    switch (profile) {
        case BuildProfile::ReferenceC051:
            return "reference-C051";
        case BuildProfile::V13EnglishCollectors:
            return "v1.3-English-Collectors-C5C5";
        case BuildProfile::ReforgedC5C5:
            return "Reforged-C5C5-868086";
        default:
            return "unsupported";
    }
}

bool IsPhase3ProfileSupported(BuildProfile profile) noexcept {
    return profile == BuildProfile::ReforgedC5C5;
}

bool VerifySupportedBuild(const ExecutableFingerprint& fingerprint,
                          BuildProfile* profile,
                          char* reason, std::size_t reasonSize) {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    if (profile != nullptr) *profile = BuildProfile::Unsupported;
    if (fingerprint.loadedBase != kImageBase) {
        SetReason(reason, reasonSize,
                  "main module base is 0x%08X, expected 0x%08X",
                  static_cast<unsigned>(fingerprint.loadedBase),
                  static_cast<unsigned>(kImageBase));
        return false;
    }
    if (fingerprint.machine != IMAGE_FILE_MACHINE_I386 ||
        fingerprint.optionalHeaderMagic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        fingerprint.preferredImageBase != kImageBase) {
        SetReason(reason, reasonSize, "main module is not the expected i386 image");
        return false;
    }

    const BuildProfile identified = IdentifyBuildProfile(fingerprint);
    if (identified == BuildProfile::Unsupported) {
        SetReason(reason, reasonSize,
                  "unsupported executable fingerprint: size=%llu md5=%s",
                  static_cast<unsigned long long>(fingerprint.fileSize),
                  fingerprint.md5);
        return false;
    }

    const char* baseName = std::strrchr(fingerprint.path, '\\');
    baseName = baseName == nullptr ? fingerprint.path : baseName + 1;
    const char* expectedName = identified == BuildProfile::ReforgedC5C5
                                   ? "Need For Speed MW-Reforged.exe"
                                   : "speed.exe";
    const bool alternateReforgedName =
        identified == BuildProfile::ReforgedC5C5 &&
        _stricmp(baseName, "speed.exe") == 0;
    if (_stricmp(baseName, expectedName) != 0 && !alternateReforgedName) {
        SetReason(reason, reasonSize,
                  "main module is '%s', expected '%s' or 'speed.exe'",
                  baseName, expectedName);
        return false;
    }

    if (!IsPhase3ProfileSupported(identified)) {
        SetReason(reason, reasonSize,
                  "multi-gear hooks do not support profile '%s'",
                  BuildProfileName(identified));
        return false;
    }

    if ((identified == BuildProfile::V13EnglishCollectors ||
         identified == BuildProfile::ReforgedC5C5) &&
        (fingerprint.timeDateStamp !=
             kV13EnglishCollectorsTimeDateStamp ||
         fingerprint.imageSize != kV13EnglishCollectorsImageSize ||
         fingerprint.entryPointRva != kV13EnglishCollectorsEntryPointRva)) {
        SetReason(reason, reasonSize,
                  "v1.3 profile PE fields changed: timestamp=0x%08X "
                  "imageSize=0x%08X entryRva=0x%08X",
                  static_cast<unsigned>(fingerprint.timeDateStamp),
                  static_cast<unsigned>(fingerprint.imageSize),
                  static_cast<unsigned>(fingerprint.entryPointRva));
        return false;
    }

    if (!VerifyCodeSignature(kBStringHash, kBStringHashBytes,
                             sizeof(kBStringHashBytes), "bStringHash", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kTransmissionCtor, kTransmissionCtorBytes,
                             sizeof(kTransmissionCtorBytes),
                             "transmission constructor", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kClassGetDefinition, kClassGetDefinitionBytes,
                             sizeof(kClassGetDefinitionBytes),
                             "Class::GetDefinition", reason, reasonSize) ||
        !VerifyCodeSignature(kPrivateCount, kPrivateCountBytes,
                             sizeof(kPrivateCountBytes), "Private::Count",
                             reason, reasonSize) ||
        !VerifyCodeSignature(kPrivateGetElement, kPrivateGetElementBytes,
                             sizeof(kPrivateGetElementBytes),
                             "Private::GetElement", reason, reasonSize) ||
        !VerifyCodeSignature(kShiftGear, kShiftGearBytes,
                             sizeof(kShiftGearBytes), "ShiftGear", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kGearRatioGetter, kGearRatioGetterBytes,
                             sizeof(kGearRatioGetterBytes),
                             "gear-ratio getter", reason, reasonSize) ||
        !VerifyCodeSignature(kGearEfficiencyGetter,
                             kGearEfficiencyGetterBytes,
                             sizeof(kGearEfficiencyGetterBytes),
                             "gear-efficiency getter", reason, reasonSize) ||
        !VerifyCodeSignature(kGearRatioPointer, kGearRatioPointerBytes,
                             sizeof(kGearRatioPointerBytes),
                             "gear-ratio pointer helper", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kEffectiveGearRatio,
                             kEffectiveGearRatioBytes,
                             sizeof(kEffectiveGearRatioBytes),
                             "effective gear-ratio getter", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kTransmissionUpdate,
                             kTransmissionUpdateBytes,
                             sizeof(kTransmissionUpdateBytes),
                             "transmission update", reason, reasonSize) ||
        !VerifyCodeSignature(kEffectiveRatioHelper,
                             kEffectiveRatioHelperBytes,
                             sizeof(kEffectiveRatioHelperBytes),
                             "effective-ratio helper", reason, reasonSize) ||
        !VerifyCodeSignature(kAdditionalShiftDecision,
                             kAdditionalShiftDecisionBytes,
                             sizeof(kAdditionalShiftDecisionBytes),
                             "additional shift decision", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kShiftCurve, kShiftCurveBytes,
                             sizeof(kShiftCurveBytes),
                             "shift curve helper", reason, reasonSize) ||
        !VerifyCodeSignature(kGearRatioPair, kGearRatioPairBytes,
                             sizeof(kGearRatioPairBytes),
                             "gear-ratio pair helper", reason, reasonSize) ||
        !VerifyCodeSignature(kTransmissionShift, kTransmissionShiftBytes,
                             sizeof(kTransmissionShiftBytes),
                             "transmission shift", reason, reasonSize) ||
        !VerifyCodeSignature(kTransmissionGetTopGear,
                             kTransmissionGetTopGearBytes,
                             sizeof(kTransmissionGetTopGearBytes),
                             "transmission top-gear getter", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kAlternateTransmissionShift,
                             kAlternateTransmissionShiftBytes,
                             sizeof(kAlternateTransmissionShiftBytes),
                             "alternate transmission shift", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kAlternateTransmissionGetTopGear,
                             kAlternateTransmissionGetTopGearBytes,
                             sizeof(kAlternateTransmissionGetTopGearBytes),
                             "alternate transmission top-gear getter", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kAutomaticShiftDecision,
                             kAutomaticShiftDecisionBytes,
                             sizeof(kAutomaticShiftDecisionBytes),
                             "automatic shift decision", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kAutomaticShiftUpdate,
                             kAutomaticShiftUpdateBytes,
                             sizeof(kAutomaticShiftUpdateBytes),
                             "automatic shift update", reason, reasonSize) ||
        !VerifyCodeSignature(kAutomaticShiftController,
                             kAutomaticShiftControllerBytes,
                             sizeof(kAutomaticShiftControllerBytes),
                             "automatic shift controller", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kDerivedShiftValue, kDerivedShiftValueBytes,
                             sizeof(kDerivedShiftValueBytes),
                             "derived shift value", reason, reasonSize) ||
        !VerifyCodeSignature(kDerivedEfficiencyValue,
                             kDerivedEfficiencyValueBytes,
                             sizeof(kDerivedEfficiencyValueBytes),
                             "derived efficiency value", reason, reasonSize) ||
        !VerifyCodeSignature(kLayoutRatioGetter, kLayoutRatioGetterBytes,
                             sizeof(kLayoutRatioGetterBytes),
                             "layout ratio getter", reason, reasonSize) ||
        !VerifyCodeSignature(kLayoutEfficiencyGetter,
                             kLayoutEfficiencyGetterBytes,
                             sizeof(kLayoutEfficiencyGetterBytes),
                             "layout efficiency getter", reason, reasonSize) ||
        !VerifyCodeSignature(kLayoutEffectiveRatio,
                             kLayoutEffectiveRatioBytes,
                             sizeof(kLayoutEffectiveRatioBytes),
                             "layout effective ratio", reason, reasonSize) ||
        !VerifyCodeSignature(kTorqueEfficiencyRatio,
                             kTorqueEfficiencyRatioBytes,
                             sizeof(kTorqueEfficiencyRatioBytes),
                             "torque efficiency-ratio helper", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kControllerRatioGetter,
                             kControllerRatioGetterBytes,
                             sizeof(kControllerRatioGetterBytes),
                             "controller ratio getter", reason, reasonSize) ||
        !VerifyCodeSignature(kControllerEfficiencyGetter,
                             kControllerEfficiencyGetterBytes,
                             sizeof(kControllerEfficiencyGetterBytes),
                             "controller efficiency getter", reason,
                             reasonSize) ||
        !VerifyCodeSignature(kControllerEffectiveRatio,
                             kControllerEffectiveRatioBytes,
                             sizeof(kControllerEffectiveRatioBytes),
                             "controller effective ratio", reason,
                             reasonSize)) {
        return false;
    }
    if (!VerifyHasher()) {
        SetReason(reason, reasonSize, "bStringHash self-test failed");
        return false;
    }

    if (profile != nullptr) *profile = identified;
    return true;
}

bool VerifySupportedBuild(BuildProfile* profile,
                          char* reason, std::size_t reasonSize) {
    ExecutableFingerprint fingerprint{};
    if (!QueryExecutableFingerprint(&fingerprint, reason, reasonSize)) {
        return false;
    }
    return VerifySupportedBuild(fingerprint, profile, reason, reasonSize);
}

}  // namespace game_access
