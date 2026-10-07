#include "StartupGate.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace nfsmw_exhaust::startup_gate {
namespace {

constexpr char kDpapiEntropy[] =
    "NFSMW.CGPhysicsFix.DeviceAuthority.v1";
constexpr char kBindingJsonPrefix[] =
    "{\r\n"
    "  \"schema\": 1,\r\n"
    "  \"product\": \"NFSMW.CGPhysicsFix\",\r\n"
    "  \"format\": \"DPAPI_LOCAL_MACHINE_HEX_V1\",\r\n"
    "  \"ciphertext_hex\": \"";
constexpr char kBindingJsonSuffix[] = "\"\r\n}\r\n";
constexpr char kHardwareIdMarker[] = "  \"hardware_id\": \"";

void clearReason(char* reason, std::size_t reasonSize) noexcept {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
}

void setReason(char* reason, std::size_t reasonSize,
               const char* format, ...) noexcept {
    if (reason == nullptr || reasonSize == 0) return;
    va_list args;
    va_start(args, format);
    _vsnprintf_s(reason, reasonSize, _TRUNCATE, format, args);
    va_end(args);
}

bool isUpperSha256(const char* value) noexcept {
    if (value == nullptr || strnlen_s(value, 65u) != 64u) return false;
    for (std::size_t index = 0; index < 64u; ++index) {
        const char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

int upperHexNibble(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool hashesEqual(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) return false;
    unsigned difference = 0;
    for (std::size_t index = 0; index < 64u; ++index) {
        difference |= static_cast<unsigned>(
            static_cast<unsigned char>(left[index]) ^
            static_cast<unsigned char>(right[index]));
    }
    return difference == 0 && left[64] == '\0' && right[64] == '\0';
}

bool canonicalPath(const wchar_t* input, std::wstring* output) {
    if (input == nullptr || input[0] == L'\0' || output == nullptr) return false;
    const DWORD required = GetFullPathNameW(input, 0, nullptr, nullptr);
    if (required == 0 || required > 32768u) return false;
    std::vector<wchar_t> buffer(required);
    const DWORD written = GetFullPathNameW(input, required, buffer.data(), nullptr);
    if (written == 0 || written >= required) return false;
    output->assign(buffer.data(), written);
    while (output->size() > 3u &&
           (output->back() == L'\\' || output->back() == L'/')) {
        output->pop_back();
    }
    return true;
}

bool samePath(const std::wstring& left, const std::wstring& right) noexcept {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                right.c_str(), static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

bool expectedInstallationPaths(const wchar_t* modulePath,
                               const wchar_t* executablePath,
                               std::wstring* bindingPath) {
    std::wstring canonicalModule;
    std::wstring canonicalExecutable;
    if (!canonicalPath(modulePath, &canonicalModule) ||
        !canonicalPath(executablePath, &canonicalExecutable) ||
        bindingPath == nullptr)
        return false;
    const std::size_t executableSlash =
        canonicalExecutable.find_last_of(L"\\/");
    if (executableSlash == std::wstring::npos) return false;
    const std::wstring scripts =
        canonicalExecutable.substr(0, executableSlash) + L"\\SCRIPTS";
    const std::wstring expectedModule =
        scripts + L"\\" + kAuthorityModuleName;
    if (!samePath(canonicalModule, expectedModule)) return false;
    *bindingPath = scripts + L"\\" + kBindingFileName;
    return true;
}

bool readFile(const wchar_t* path, std::size_t maximum,
              std::vector<std::uint8_t>* output, char* reason,
              std::size_t reasonSize) {
    if (path == nullptr || output == nullptr) return false;
    output->clear();
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        setReason(reason, reasonSize, "could not open shared binding (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
        return false;
    }
    LARGE_INTEGER size{};
    bool success = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0 &&
                   static_cast<std::uint64_t>(size.QuadPart) <= maximum;
    if (success) {
        try {
            output->resize(static_cast<std::size_t>(size.QuadPart));
        } catch (...) {
            success = false;
        }
    }
    DWORD read = 0;
    if (success) {
        success = ReadFile(file, output->data(),
                           static_cast<DWORD>(output->size()), &read,
                           nullptr) != FALSE && read == output->size();
    }
    CloseHandle(file);
    if (!success) {
        output->clear();
        setReason(reason, reasonSize,
                  "shared binding is empty, oversized, or unreadable");
    }
    return success;
}

bool decryptPayload(const std::uint8_t* ciphertext, std::size_t ciphertextSize,
                    std::vector<std::uint8_t>* plaintext, char* reason,
                    std::size_t reasonSize) {
    if (ciphertext == nullptr || ciphertextSize == 0 ||
        ciphertextSize > kMaxCiphertextSize || ciphertextSize > MAXDWORD ||
        plaintext == nullptr) {
        setReason(reason, reasonSize, "DPAPI ciphertext input is invalid");
        return false;
    }
    plaintext->clear();
    DATA_BLOB input{static_cast<DWORD>(ciphertextSize),
                    const_cast<BYTE*>(ciphertext)};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kDpapiEntropy) - 1u),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kDpapiEntropy))};
    DATA_BLOB output{};
    LPWSTR description = nullptr;
    if (!CryptUnprotectData(&input, &description, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        setReason(reason, reasonSize,
                  "DPAPI could not decrypt shared binding (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
        return false;
    }
    if (description != nullptr) LocalFree(description);
    const bool valid = output.pbData != nullptr && output.cbData != 0 &&
                       output.cbData <= kMaxPlaintextSize;
    if (valid) {
        try {
            plaintext->assign(output.pbData, output.pbData + output.cbData);
        } catch (...) {
            plaintext->clear();
        }
    }
    if (output.pbData != nullptr) {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
    }
    if (!valid || plaintext->empty()) {
        setReason(reason, reasonSize, "DPAPI returned an invalid plaintext");
        return false;
    }
    return true;
}

using GetStatusFn = const ValidationStatusV1*(__cdecl*)();

bool copyAuthorityStatus(GetStatusFn getStatus,
                         ValidationStatusV1* snapshot) noexcept {
    if (getStatus == nullptr || snapshot == nullptr) return false;
    __try {
        const ValidationStatusV1* status = getStatus();
        if (status == nullptr || status->structSize < sizeof(*snapshot))
            return false;
        std::memcpy(snapshot, status, sizeof(*snapshot));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool ValidateAuthorityStatus(const ValidationStatusV1* status, char* reason,
                             std::size_t reasonSize) noexcept {
    clearReason(reason, reasonSize);
    if (status == nullptr) {
        setReason(reason, reasonSize, "validation authority export is missing");
        return false;
    }
    if (status->structSize < sizeof(ValidationStatusV1) ||
        status->abiVersion != 1u) {
        setReason(reason, reasonSize, "validation authority ABI is incompatible");
        return false;
    }
    if (status->state != static_cast<long>(AuthorityState::Verified)) {
        setReason(reason, reasonSize,
                  "validation authority is not verified (state %ld)",
                  status->state);
        return false;
    }
    if (status->bindingSchema != 1u ||
        status->deviceSourceMask != kRequiredSourceMask) {
        setReason(reason, reasonSize, "validation authority metadata is invalid");
        return false;
    }
    if (status->failureReason[0] != '\0' ||
        std::memchr(status->failureReason, '\0',
                    sizeof(status->failureReason)) == nullptr) {
        setReason(reason, reasonSize, "validation authority reported a failure");
        return false;
    }
    if (!isUpperSha256(status->authorityModuleSha256) ||
        !isUpperSha256(status->executableSha256) ||
        !hashesEqual(status->executableSha256,
                     kExpectedExecutableSha256)) {
        setReason(reason, reasonSize, "validation authority hashes are invalid");
        return false;
    }
    if (status->bindingPath[0] == L'\0' ||
        std::wmemchr(status->bindingPath, L'\0',
                     std::size(status->bindingPath)) == nullptr) {
        setReason(reason, reasonSize,
                  "validation authority binding path is invalid");
        return false;
    }
    return true;
}

bool ParseEncryptedBindingJson(const char* json, std::size_t jsonSize,
                               std::uint8_t* ciphertext,
                               std::size_t ciphertextCapacity,
                               std::size_t* ciphertextSize, char* reason,
                               std::size_t reasonSize) noexcept {
    clearReason(reason, reasonSize);
    if (ciphertextSize != nullptr) *ciphertextSize = 0;
    constexpr std::size_t prefixSize = sizeof(kBindingJsonPrefix) - 1u;
    constexpr std::size_t suffixSize = sizeof(kBindingJsonSuffix) - 1u;
    if (json == nullptr || ciphertext == nullptr ||
        jsonSize <= prefixSize + suffixSize || jsonSize > kMaxBindingJsonSize ||
        std::memcmp(json, kBindingJsonPrefix, prefixSize) != 0 ||
        std::memcmp(json + jsonSize - suffixSize, kBindingJsonSuffix,
                    suffixSize) != 0) {
        setReason(reason, reasonSize, "shared binding JSON is not canonical");
        return false;
    }
    const std::size_t hexSize = jsonSize - prefixSize - suffixSize;
    if (hexSize == 0 || (hexSize & 1u) != 0 ||
        hexSize / 2u > kMaxCiphertextSize ||
        ciphertextCapacity < hexSize / 2u) {
        setReason(reason, reasonSize,
                  "shared binding ciphertext size is invalid");
        return false;
    }
    for (std::size_t index = 0; index < hexSize / 2u; ++index) {
        const int high = upperHexNibble(json[prefixSize + index * 2u]);
        const int low = upperHexNibble(json[prefixSize + index * 2u + 1u]);
        if (high < 0 || low < 0) {
            setReason(reason, reasonSize,
                      "shared binding ciphertext is not uppercase hexadecimal");
            return false;
        }
        ciphertext[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    if (ciphertextSize != nullptr) *ciphertextSize = hexSize / 2u;
    return true;
}

bool ValidateDecryptedPayload(const std::uint8_t* plaintext,
                              std::size_t plaintextSize,
                              const char* authorityModuleSha256, char* reason,
                              std::size_t reasonSize) noexcept {
    clearReason(reason, reasonSize);
    if (plaintext == nullptr || plaintextSize == 0 ||
        plaintextSize >= kMaxPlaintextSize ||
        !isUpperSha256(authorityModuleSha256)) {
        setReason(reason, reasonSize,
                  "shared binding plaintext input is invalid");
        return false;
    }
    // DPAPI payloads are not NUL-terminated. Copy to a bounded buffer before
    // locating the hardware ID field used to reconstruct the exact document.
    std::array<char, kMaxPlaintextSize> text{};
    std::memcpy(text.data(), plaintext, plaintextSize);
    const char* marker = std::strstr(text.data(), kHardwareIdMarker);
    if (marker == nullptr) {
        setReason(reason, reasonSize, "shared binding hardware ID is missing");
        return false;
    }
    marker += sizeof(kHardwareIdMarker) - 1u;
    if (static_cast<std::size_t>(marker - text.data()) + 64u > plaintextSize) {
        setReason(reason, reasonSize,
                  "shared binding hardware ID is truncated");
        return false;
    }
    char hardwareId[65] = {};
    std::memcpy(hardwareId, marker, 64u);
    if (!isUpperSha256(hardwareId)) {
        setReason(reason, reasonSize, "shared binding hardware ID is invalid");
        return false;
    }
    std::array<char, kMaxPlaintextSize> expected{};
    const int length = _snprintf_s(
        expected.data(), expected.size(), _TRUNCATE,
        "{\r\n"
        "  \"schema\": 1,\r\n"
        "  \"product\": \"NFSMW.CGPhysicsFix\",\r\n"
        "  \"authority_module\": \"NFSMW.CGPhysicsFix.asi\",\r\n"
        "  \"authority_module_sha256\": \"%s\",\r\n"
        "  \"validation_status\": \"verified\",\r\n"
        "  \"executable\": \"speed.exe\",\r\n"
        "  \"verification_code\": %u,\r\n"
        "  \"executable_sha256\": \"%s\",\r\n"
        "  \"executable_canonical_sha256\": \"%s\",\r\n"
        "  \"hardware_id_algorithm\": \"SHA-256\",\r\n"
        "  \"identity_source_mask\": %u,\r\n"
        "  \"hardware_id\": \"%s\"\r\n"
        "}\r\n",
        authorityModuleSha256, static_cast<unsigned>(kVerificationCode),
        kExpectedExecutableSha256, kCanonicalExecutableSha256,
        static_cast<unsigned>(kRequiredSourceMask), hardwareId);
    SecureZeroMemory(hardwareId, sizeof(hardwareId));
    SecureZeroMemory(text.data(), text.size());
    if (length < 0 || static_cast<std::size_t>(length) != plaintextSize ||
        std::memcmp(expected.data(), plaintext, plaintextSize) != 0) {
        SecureZeroMemory(expected.data(), expected.size());
        setReason(reason, reasonSize,
                  "shared binding payload fields do not match");
        return false;
    }
    SecureZeroMemory(expected.data(), expected.size());
    return true;
}

bool ComputeFileSha256(const wchar_t* filePath, char output[65], char* reason,
                       std::size_t reasonSize) noexcept {
    clearReason(reason, reasonSize);
    if (output != nullptr) output[0] = '\0';
    if (filePath == nullptr || filePath[0] == L'\0' || output == nullptr) {
        setReason(reason, reasonSize, "authority file hash input is invalid");
        return false;
    }
    HANDLE file = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        setReason(reason, reasonSize,
                  "could not open authority module for SHA-256");
        return false;
    }
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool success = CryptAcquireContextW(&provider, nullptr, nullptr,
                                        PROV_RSA_AES,
                                        CRYPT_VERIFYCONTEXT) != FALSE &&
                   CryptCreateHash(provider, CALG_SHA_256, 0, 0,
                                   &hash) != FALSE;
    std::array<std::uint8_t, 16u * 1024u> buffer{};
    while (success) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr)) {
            success = false;
            break;
        }
        if (read == 0) break;
        success = CryptHashData(hash, buffer.data(), read, 0) != FALSE;
    }
    std::array<std::uint8_t, 32> digest{};
    DWORD digestSize = static_cast<DWORD>(digest.size());
    success = success &&
              CryptGetHashParam(hash, HP_HASHVAL, digest.data(),
                                &digestSize, 0) != FALSE &&
              digestSize == digest.size();
    if (hash != 0) CryptDestroyHash(hash);
    if (provider != 0) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    SecureZeroMemory(buffer.data(), buffer.size());
    if (!success) {
        setReason(reason, reasonSize,
                  "could not calculate authority SHA-256");
        return false;
    }
    constexpr char digits[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < digest.size(); ++index) {
        output[index * 2u] = digits[digest[index] >> 4u];
        output[index * 2u + 1u] = digits[digest[index] & 0x0Fu];
    }
    output[64] = '\0';
    SecureZeroMemory(digest.data(), digest.size());
    return true;
}

bool VerifyAuthoritySnapshot(const ValidationStatusV1* status,
                             const wchar_t* loadedAuthorityPath,
                             const wchar_t* gameExecutablePath, char* reason,
                             std::size_t reasonSize) noexcept {
    clearReason(reason, reasonSize);
    try {
        if (!ValidateAuthorityStatus(status, reason, reasonSize)) return false;
        std::wstring expectedPath;
        std::wstring reportedPath;
        if (!expectedInstallationPaths(loadedAuthorityPath, gameExecutablePath,
                                       &expectedPath) ||
            !canonicalPath(status->bindingPath, &reportedPath) ||
            !samePath(expectedPath, reportedPath)) {
            setReason(reason, reasonSize,
                      "shared binding path escaped the authority SCRIPTS directory");
            return false;
        }
        char diskHash[65] = {};
        if (!ComputeFileSha256(loadedAuthorityPath, diskHash, reason,
                               reasonSize) ||
            !hashesEqual(diskHash, status->authorityModuleSha256)) {
            SecureZeroMemory(diskHash, sizeof(diskHash));
            setReason(reason, reasonSize,
                      "authority module SHA-256 does not match export");
            return false;
        }
        std::vector<std::uint8_t> json;
        std::vector<std::uint8_t> ciphertext(kMaxCiphertextSize);
        std::vector<std::uint8_t> plaintext;
        std::size_t ciphertextSize = 0;
        const bool success =
            readFile(reportedPath.c_str(), kMaxBindingJsonSize, &json,
                     reason, reasonSize) &&
            ParseEncryptedBindingJson(
                reinterpret_cast<const char*>(json.data()), json.size(),
                ciphertext.data(), ciphertext.size(), &ciphertextSize,
                reason, reasonSize) &&
            decryptPayload(ciphertext.data(), ciphertextSize, &plaintext,
                           reason, reasonSize) &&
            ValidateDecryptedPayload(plaintext.data(), plaintext.size(),
                                     diskHash, reason, reasonSize);
        SecureZeroMemory(ciphertext.data(), ciphertext.size());
        if (!plaintext.empty())
            SecureZeroMemory(plaintext.data(), plaintext.size());
        SecureZeroMemory(diskHash, sizeof(diskHash));
        return success;
    } catch (...) {
        setReason(reason, reasonSize,
                  "shared validation consumer failed unexpectedly");
        return false;
    }
}

bool VerifyLoadedAuthority(char* reason, std::size_t reasonSize) noexcept {
    clearReason(reason, reasonSize);
    HMODULE module = GetModuleHandleW(kAuthorityModuleName);
    if (module == nullptr) {
        setReason(reason, reasonSize, "NFSMW.CGPhysicsFix.asi is not loaded");
        return false;
    }
    const auto getStatus = reinterpret_cast<GetStatusFn>(
        GetProcAddress(module, kAuthorityExportName));
    if (getStatus == nullptr) {
        setReason(reason, reasonSize, "validation authority export is missing");
        return false;
    }
    ValidationStatusV1 snapshot{};
    if (!copyAuthorityStatus(getStatus, &snapshot)) {
        setReason(reason, reasonSize,
                  "validation authority status is unreadable");
        return false;
    }
    std::vector<wchar_t> modulePath(32768u);
    const DWORD length = GetModuleFileNameW(
        module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) {
        setReason(reason, reasonSize,
                  "could not resolve validation authority path");
        return false;
    }
    std::vector<wchar_t> executablePath(32768u);
    const DWORD executableLength = GetModuleFileNameW(
        nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (executableLength == 0 || executableLength >= executablePath.size()) {
        setReason(reason, reasonSize, "could not resolve game executable path");
        return false;
    }
    return VerifyAuthoritySnapshot(&snapshot, modulePath.data(),
                                   executablePath.data(), reason, reasonSize);
}

}  // namespace nfsmw_exhaust::startup_gate
