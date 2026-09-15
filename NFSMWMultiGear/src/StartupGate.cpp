#include "StartupGate.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include <cstdarg>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>

namespace startup_gate {
namespace {

constexpr std::size_t kChecksumOffset = 0x160u;
constexpr DWORD kRawSmbiosProvider = 0x52534D42u;  // 'RSMB'
constexpr unsigned kSystemUuidSource = 1u << 0;
constexpr unsigned kMachineGuidSource = 1u << 1;
constexpr unsigned kSystemVolumeSource = 1u << 2;
constexpr char kBindingEntropy[] =
    "NFSMWMultiGear.DeviceBinding.v2.local-machine";
constexpr char kBindingEnvelopePrefix[] =
    "{\r\n"
    "  \"schema\": 2,\r\n"
    "  \"encoding\": \"DPAPI-LOCAL-MACHINE-HEX\",\r\n"
    "  \"ciphertext_hex\": \"";
constexpr char kBindingEnvelopeSuffix[] = "\"\r\n}\r\n";
constexpr std::size_t kBindingPayloadCapacity = 1024u;
constexpr std::size_t kBindingCiphertextCapacity = 4096u;

void SetReason(char* reason, std::size_t reasonSize, const char* format, ...) {
    if (reason == nullptr || reasonSize == 0) return;
    va_list args;
    va_start(args, format);
    _vsnprintf_s(reason, reasonSize, _TRUNCATE, format, args);
    va_end(args);
}

std::uint32_t ReadLe32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

int HexNibble(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool DecodeHex(const char* text, std::uint8_t* output,
               std::size_t outputSize) noexcept {
    if (text == nullptr || output == nullptr) return false;
    for (std::size_t i = 0; i < outputSize; ++i) {
        const int high = HexNibble(text[i * 2]);
        const int low = HexNibble(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return text[outputSize * 2] == '\0';
}

bool EncodeHexBytes(const std::uint8_t* input, std::size_t inputSize,
                    char* output, std::size_t outputSize) noexcept {
    if (input == nullptr || output == nullptr || outputSize == 0 ||
        inputSize > (outputSize - 1u) / 2u) {
        return false;
    }
    constexpr char digits[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < inputSize; ++i) {
        output[i * 2u] = digits[input[i] >> 4u];
        output[i * 2u + 1u] = digits[input[i] & 0x0Fu];
    }
    output[inputSize * 2u] = '\0';
    return true;
}

bool DecodeHexBytes(const char* input, std::size_t inputSize,
                    std::vector<std::uint8_t>* output) {
    if (input == nullptr || output == nullptr || inputSize == 0 ||
        (inputSize & 1u) != 0 || inputSize / 2u > kBindingCiphertextCapacity) {
        return false;
    }
    output->assign(inputSize / 2u, 0u);
    for (std::size_t i = 0; i < output->size(); ++i) {
        const int high = HexNibble(input[i * 2u]);
        const int low = HexNibble(input[i * 2u + 1u]);
        // The writer uses uppercase hex; reject alternate spellings so the
        // envelope has one canonical representation.
        if (high < 0 || low < 0 ||
            (input[i * 2u] >= 'a' && input[i * 2u] <= 'f') ||
            (input[i * 2u + 1u] >= 'a' && input[i * 2u + 1u] <= 'f')) {
            output->clear();
            return false;
        }
        (*output)[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool ConstantTimeEqual(const std::uint8_t* left, std::size_t leftSize,
                       const std::uint8_t* right, std::size_t rightSize) {
    if (left == nullptr || right == nullptr || leftSize != rightSize) {
        return false;
    }
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < leftSize; ++i) {
        difference = static_cast<std::uint8_t>(difference | (left[i] ^ right[i]));
    }
    return difference == 0;
}

bool ProtectBindingPayload(const char* payload, std::size_t payloadSize,
                           std::vector<std::uint8_t>* ciphertext,
                           char* reason, std::size_t reasonSize) {
    if (payload == nullptr || payloadSize == 0 ||
        payloadSize > kBindingPayloadCapacity || payloadSize > MAXDWORD ||
        ciphertext == nullptr) {
        SetReason(reason, reasonSize, "device-binding payload is invalid");
        return false;
    }

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(payloadSize);
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(payload));
    DATA_BLOB entropy{};
    entropy.cbData = static_cast<DWORD>(sizeof(kBindingEntropy) - 1u);
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kBindingEntropy));
    DATA_BLOB protectedData{};
    if (!CryptProtectData(
            &input, L"NFSMWMultiGear device binding", &entropy, nullptr,
            nullptr,
            CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
            &protectedData)) {
        SetReason(reason, reasonSize,
                  "could not encrypt the device binding with DPAPI (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
        return false;
    }

    const bool valid = protectedData.pbData != nullptr &&
                       protectedData.cbData != 0 &&
                       protectedData.cbData <= kBindingCiphertextCapacity;
    if (valid) {
        ciphertext->assign(protectedData.pbData,
                           protectedData.pbData + protectedData.cbData);
    }
    if (protectedData.pbData != nullptr) {
        SecureZeroMemory(protectedData.pbData, protectedData.cbData);
        LocalFree(protectedData.pbData);
    }
    if (!valid) {
        SetReason(reason, reasonSize,
                  "DPAPI returned an invalid device-binding ciphertext");
        return false;
    }
    return true;
}

bool UnprotectBindingPayload(const std::uint8_t* ciphertext,
                             std::size_t ciphertextSize,
                             std::vector<std::uint8_t>* payload,
                             char* reason, std::size_t reasonSize) {
    if (ciphertext == nullptr || ciphertextSize == 0 ||
        ciphertextSize > kBindingCiphertextCapacity ||
        ciphertextSize > MAXDWORD || payload == nullptr) {
        SetReason(reason, reasonSize,
                  "encrypted device-binding payload is invalid");
        return false;
    }

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(ciphertextSize);
    input.pbData = reinterpret_cast<BYTE*>(
        const_cast<std::uint8_t*>(ciphertext));
    DATA_BLOB entropy{};
    entropy.cbData = static_cast<DWORD>(sizeof(kBindingEntropy) - 1u);
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kBindingEntropy));
    DATA_BLOB plainData{};
    LPWSTR description = nullptr;
    if (!CryptUnprotectData(&input, &description, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &plainData)) {
        SetReason(reason, reasonSize,
                  "could not decrypt the device binding with DPAPI (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
        return false;
    }

    const bool valid = plainData.pbData != nullptr && plainData.cbData != 0 &&
                       plainData.cbData <= kBindingPayloadCapacity;
    if (valid) {
        payload->assign(plainData.pbData, plainData.pbData + plainData.cbData);
    }
    if (description != nullptr) LocalFree(description);
    if (plainData.pbData != nullptr) {
        SecureZeroMemory(plainData.pbData, plainData.cbData);
        LocalFree(plainData.pbData);
    }
    if (!valid) {
        SetReason(reason, reasonSize,
                  "DPAPI returned an invalid device-binding plaintext");
        return false;
    }
    return true;
}

void EncodeHex(const std::uint8_t* digest, std::size_t digestSize,
               char* output) noexcept {
    constexpr char digits[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < digestSize; ++i) {
        output[i * 2] = digits[digest[i] >> 4u];
        output[i * 2 + 1] = digits[digest[i] & 0x0Fu];
    }
    output[digestSize * 2] = '\0';
}

bool BeginSha256(HCRYPTPROV* provider, HCRYPTHASH* hash) {
    if (provider == nullptr || hash == nullptr) return false;
    *provider = 0;
    *hash = 0;
    if (!CryptAcquireContextA(provider, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        return false;
    }
    if (!CryptCreateHash(*provider, CALG_SHA_256, 0, 0, hash)) {
        CryptReleaseContext(*provider, 0);
        *provider = 0;
        return false;
    }
    return true;
}

void EndHash(HCRYPTPROV provider, HCRYPTHASH hash) {
    if (hash != 0) CryptDestroyHash(hash);
    if (provider != 0) CryptReleaseContext(provider, 0);
}

bool FinishSha256(HCRYPTHASH hash, char output[65]) {
    std::uint8_t digest[32] = {};
    DWORD digestSize = sizeof(digest);
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0) ||
        digestSize != sizeof(digest)) {
        return false;
    }
    EncodeHex(digest, sizeof(digest), output);
    return true;
}

bool HashBytes(HCRYPTHASH hash, const void* data, std::size_t size) {
    if (size == 0) return true;
    if (data == nullptr || size > MAXDWORD) return false;
    return CryptHashData(hash, static_cast<const BYTE*>(data),
                         static_cast<DWORD>(size), 0) != FALSE;
}

bool HashField(HCRYPTHASH hash, const char* name, const void* data,
               std::size_t size) {
    if (name == nullptr || size > UINT32_MAX) return false;
    const std::size_t nameSize = std::strlen(name);
    if (nameSize > UINT32_MAX) return false;
    const std::uint32_t nameLength = static_cast<std::uint32_t>(nameSize);
    const std::uint32_t dataLength = static_cast<std::uint32_t>(size);
    return HashBytes(hash, &nameLength, sizeof(nameLength)) &&
           HashBytes(hash, name, nameSize) &&
           HashBytes(hash, &dataLength, sizeof(dataLength)) &&
           HashBytes(hash, data, size);
}

bool ReadFileAt(HANDLE file, std::uint64_t offset, void* destination,
                DWORD size) {
    if (file == INVALID_HANDLE_VALUE || destination == nullptr) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    DWORD total = 0;
    while (total < size) {
        DWORD current = 0;
        if (!ReadFile(file, static_cast<std::uint8_t*>(destination) + total,
                      size - total, &current, nullptr) ||
            current == 0) {
            return false;
        }
        total += current;
    }
    return true;
}

bool ComputeExecutableHashes(HANDLE file, char fullSha256[65],
                             char canonicalSha256[65], char* reason,
                             std::size_t reasonSize) {
    if (file == INVALID_HANDLE_VALUE) {
        SetReason(reason, reasonSize, "invalid Reforged executable handle");
        return false;
    }
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        SetReason(reason, reasonSize,
                  "could not rewind the Reforged executable");
        return false;
    }

    HCRYPTPROV fullProvider = 0;
    HCRYPTPROV canonicalProvider = 0;
    HCRYPTHASH fullHash = 0;
    HCRYPTHASH canonicalHash = 0;
    bool success = false;
    do {
        if (!BeginSha256(&fullProvider, &fullHash) ||
            !BeginSha256(&canonicalProvider, &canonicalHash)) {
            SetReason(reason, reasonSize, "could not initialize SHA-256");
            break;
        }

        std::uint8_t buffer[16 * 1024] = {};
        std::uint64_t offset = 0;
        for (;;) {
            DWORD bytesRead = 0;
            if (!ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr)) {
                SetReason(reason, reasonSize,
                          "could not read the Reforged executable");
                break;
            }
            if (bytesRead == 0) {
                if (offset != kStampedExecutableSize) {
                    SetReason(reason, reasonSize,
                              "the Reforged executable size changed while reading");
                    break;
                }
                if (!FinishSha256(fullHash, fullSha256) ||
                    !FinishSha256(canonicalHash, canonicalSha256)) {
                    SetReason(reason, reasonSize,
                              "could not finish the executable SHA-256");
                    break;
                }
                success = true;
                break;
            }

            if (!HashBytes(fullHash, buffer, bytesRead)) {
                SetReason(reason, reasonSize,
                          "could not hash the Reforged executable");
                break;
            }

            DWORD canonicalBytes = 0;
            if (offset < kOriginalExecutableSize) {
                const std::uint64_t remaining =
                    kOriginalExecutableSize - offset;
                canonicalBytes = static_cast<DWORD>(
                    remaining < bytesRead ? remaining : bytesRead);
                const std::uint64_t chunkEnd = offset + canonicalBytes;
                const std::uint64_t checksumEnd = kChecksumOffset + 4u;
                if (offset < checksumEnd && chunkEnd > kChecksumOffset) {
                    const std::size_t begin = static_cast<std::size_t>(
                        kChecksumOffset > offset ? kChecksumOffset - offset : 0);
                    const std::size_t end = static_cast<std::size_t>(
                        checksumEnd < chunkEnd ? checksumEnd - offset
                                               : canonicalBytes);
                    std::memset(buffer + begin, 0, end - begin);
                }
                if (!HashBytes(canonicalHash, buffer, canonicalBytes)) {
                    SetReason(reason, reasonSize,
                              "could not hash the canonical game image");
                    break;
                }
            }
            offset += bytesRead;
        }
    } while (false);

    EndHash(fullProvider, fullHash);
    EndHash(canonicalProvider, canonicalHash);
    return success;
}

bool ReadMachineGuid(char output[128]) {
    HKEY key = nullptr;
    LONG opened = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (opened != ERROR_SUCCESS) {
        opened = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                               "SOFTWARE\\Microsoft\\Cryptography", 0,
                               KEY_QUERY_VALUE, &key);
    }
    if (opened != ERROR_SUCCESS) return false;

    DWORD type = 0;
    DWORD size = 128;
    const LONG queried =
        RegQueryValueExA(key, "MachineGuid", nullptr, &type,
                         reinterpret_cast<BYTE*>(output), &size);
    RegCloseKey(key);
    if (queried != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size < 2 || size > 128) {
        output[0] = '\0';
        return false;
    }
    output[127] = '\0';
    return output[0] != '\0';
}

bool ReadSystemVolumeSerial(std::uint32_t* serial) {
    if (serial == nullptr) return false;
    char windowsDirectory[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryA(windowsDirectory, MAX_PATH);
    if (length < 3 || length >= MAX_PATH || windowsDirectory[1] != ':') {
        return false;
    }
    char root[] = {windowsDirectory[0], ':', '\\', '\0'};
    DWORD value = 0;
    if (!GetVolumeInformationA(root, nullptr, 0, &value, nullptr, nullptr,
                               nullptr, 0)) {
        return false;
    }
    *serial = static_cast<std::uint32_t>(value);
    return true;
}

bool IsUsableUuid(const std::uint8_t* uuid) {
    bool anyNonZero = false;
    bool anyNonFF = false;
    for (std::size_t i = 0; i < 16; ++i) {
        anyNonZero = anyNonZero || uuid[i] != 0;
        anyNonFF = anyNonFF || uuid[i] != 0xFFu;
    }
    return anyNonZero && anyNonFF;
}

bool ReadSystemUuid(std::uint8_t output[16]) {
    const UINT size = GetSystemFirmwareTable(kRawSmbiosProvider, 0, nullptr, 0);
    if (size < 12 || size > 4u * 1024u * 1024u) return false;
    std::vector<std::uint8_t> data(size);
    if (GetSystemFirmwareTable(kRawSmbiosProvider, 0, data.data(), size) !=
        size) {
        return false;
    }
    const std::uint32_t tableLength = ReadLe32(data.data() + 4);
    if (tableLength > size - 8u) return false;

    const std::size_t end = 8u + tableLength;
    std::size_t position = 8u;
    while (position + 4u <= end) {
        const std::uint8_t type = data[position];
        const std::uint8_t length = data[position + 1u];
        if (length < 4u || position + length > end) return false;
        if (type == 1u && length >= 0x19u) {
            const std::uint8_t* uuid = data.data() + position + 8u;
            if (IsUsableUuid(uuid)) {
                std::memcpy(output, uuid, 16u);
                return true;
            }
        }
        if (type == 127u) break;

        std::size_t next = position + length;
        while (next + 1u < end &&
               !(data[next] == 0 && data[next + 1u] == 0)) {
            ++next;
        }
        if (next + 1u >= end) return false;
        position = next + 2u;
    }
    return false;
}

bool QueryDeviceComponents(DeviceComponents* components, char* reason,
                           std::size_t reasonSize) {
    if (components == nullptr) {
        SetReason(reason, reasonSize, "device component destination is null");
        return false;
    }
    *components = {};
    components->hasSystemUuid = ReadSystemUuid(components->systemUuid);
    if (!components->hasSystemUuid) {
        SetReason(reason, reasonSize,
                  "could not read a usable SMBIOS system UUID");
        return false;
    }
    if (!ReadMachineGuid(components->machineGuid)) {
        SetReason(reason, reasonSize, "could not read Windows MachineGuid");
        return false;
    }
    components->hasSystemVolumeSerial =
        ReadSystemVolumeSerial(&components->systemVolumeSerial);
    if (!components->hasSystemVolumeSerial) {
        SetReason(reason, reasonSize,
                  "could not read the Windows system-volume serial");
        return false;
    }
    return true;
}

enum class ExistingBinding { Missing, Matches, Mismatch, Error };

ExistingBinding CheckExistingBinding(const char* path, const char* expected,
                                     std::size_t expectedSize, char* reason,
                                     std::size_t reasonSize) {
    const DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return ExistingBinding::Missing;
        }
        SetReason(reason, reasonSize,
                  "could not inspect the device-binding file (error %lu)",
                  static_cast<unsigned long>(error));
        return ExistingBinding::Error;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        SetReason(reason, reasonSize, "the device-binding path is a directory");
        return ExistingBinding::Error;
    }

    HANDLE file = CreateFileA(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetReason(reason, reasonSize,
                  "could not open the existing device-binding file");
        return ExistingBinding::Error;
    }
    LARGE_INTEGER size{};
    const bool sizeOk = GetFileSizeEx(file, &size) != FALSE &&
                        size.QuadPart == static_cast<LONGLONG>(expectedSize);
    std::vector<char> contents(sizeOk ? expectedSize : 0u);
    bool readOk = sizeOk;
    DWORD total = 0;
    while (readOk && total < expectedSize) {
        DWORD current = 0;
        readOk = ReadFile(file, contents.data() + total,
                          static_cast<DWORD>(expectedSize - total), &current,
                          nullptr) != FALSE &&
                 current != 0;
        total += current;
    }
    CloseHandle(file);

    if (!readOk || total != expectedSize ||
        std::memcmp(contents.data(), expected, expectedSize) != 0) {
        SetReason(reason, reasonSize,
                  "existing device binding does not match this computer");
        return ExistingBinding::Mismatch;
    }
    return ExistingBinding::Matches;
}

bool WriteComplete(HANDLE file, const char* contents, std::size_t size) {
    std::size_t total = 0;
    while (total < size) {
        const DWORD remaining = static_cast<DWORD>(size - total);
        DWORD written = 0;
        if (!WriteFile(file, contents + total, remaining, &written, nullptr) ||
            written == 0) {
            return false;
        }
        total += written;
    }
    return true;
}

bool BuildBindingPayloadInternal(const char* deviceId, std::uint32_t schema,
                                 char* output, std::size_t outputSize,
                                 std::size_t* written, char* reason,
                                 std::size_t reasonSize) noexcept {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    if (written != nullptr) *written = 0;
    if (deviceId == nullptr || output == nullptr || outputSize == 0 ||
        std::strlen(deviceId) != 64u ||
        (schema != 1u && schema != kBindingSchema)) {
        SetReason(reason, reasonSize,
                  "device ID, schema, or JSON destination is invalid");
        return false;
    }
    for (std::size_t i = 0; i < 64u; ++i) {
        const char value = deviceId[i];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'A' && value <= 'F'))) {
            SetReason(reason, reasonSize,
                      "device ID is not uppercase SHA-256");
            return false;
        }
    }

    const int length = _snprintf_s(
        output, outputSize, _TRUNCATE,
        "{\r\n"
        "  \"schema\": %u,\r\n"
        "  \"product\": \"NFSMWMultiGear\",\r\n"
        "  \"executable\": \"%s\",\r\n"
        "  \"verification_code\": %u,\r\n"
        "  \"executable_sha256\": \"%s\",\r\n"
        "  \"hardware_id_algorithm\": \"SHA-256\",\r\n"
        "  \"identity_source_mask\": 7,\r\n"
        "  \"hardware_id\": \"%s\"\r\n"
        "}\r\n",
        static_cast<unsigned>(schema), kExecutableName,
        static_cast<unsigned>(kVerificationCode),
        kBindingExecutableSha256, deviceId);
    if (length < 0) {
        SetReason(reason, reasonSize,
                  "device-binding JSON buffer is too small");
        return false;
    }
    if (written != nullptr) *written = static_cast<std::size_t>(length);
    return true;
}

bool BuildEncryptedEnvelope(const char* payload, std::size_t payloadSize,
                            char* output, std::size_t outputSize,
                            std::size_t* written, char* reason,
                            std::size_t reasonSize) {
    if (written != nullptr) *written = 0;
    if (output == nullptr || outputSize == 0) {
        SetReason(reason, reasonSize,
                  "encrypted binding destination is invalid");
        return false;
    }

    std::vector<std::uint8_t> ciphertext;
    if (!ProtectBindingPayload(payload, payloadSize, &ciphertext, reason,
                               reasonSize)) {
        return false;
    }
    const std::size_t prefixSize = sizeof(kBindingEnvelopePrefix) - 1u;
    const std::size_t suffixSize = sizeof(kBindingEnvelopeSuffix) - 1u;
    const std::size_t hexSize = ciphertext.size() * 2u;
    const std::size_t required = prefixSize + hexSize + suffixSize;
    if (required >= outputSize) {
        SecureZeroMemory(ciphertext.data(), ciphertext.size());
        SetReason(reason, reasonSize,
                  "encrypted binding JSON buffer is too small");
        return false;
    }

    std::memcpy(output, kBindingEnvelopePrefix, prefixSize);
    if (!EncodeHexBytes(ciphertext.data(), ciphertext.size(),
                        output + prefixSize, outputSize - prefixSize)) {
        SecureZeroMemory(ciphertext.data(), ciphertext.size());
        SetReason(reason, reasonSize,
                  "could not encode the device-binding ciphertext");
        return false;
    }
    SecureZeroMemory(ciphertext.data(), ciphertext.size());
    std::memcpy(output + prefixSize + hexSize, kBindingEnvelopeSuffix,
                suffixSize);
    output[required] = '\0';
    if (written != nullptr) *written = required;
    return true;
}

ExistingBinding CheckEncryptedBinding(const char* path,
                                      const char* expectedPayload,
                                      std::size_t expectedPayloadSize,
                                      char* reason,
                                      std::size_t reasonSize) {
    const DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return ExistingBinding::Missing;
        }
        SetReason(reason, reasonSize,
                  "could not inspect the device-binding file (error %lu)",
                  static_cast<unsigned long>(error));
        return ExistingBinding::Error;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        SetReason(reason, reasonSize, "the device-binding path is a directory");
        return ExistingBinding::Error;
    }

    HANDLE file = CreateFileA(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetReason(reason, reasonSize,
                  "could not open the existing device-binding file");
        return ExistingBinding::Error;
    }
    LARGE_INTEGER fileSize{};
    const std::size_t prefixSize = sizeof(kBindingEnvelopePrefix) - 1u;
    const std::size_t suffixSize = sizeof(kBindingEnvelopeSuffix) - 1u;
    const std::size_t maximumSize =
        prefixSize + kBindingCiphertextCapacity * 2u + suffixSize;
    const bool sizeOk =
        GetFileSizeEx(file, &fileSize) != FALSE && fileSize.QuadPart > 0 &&
        static_cast<std::uint64_t>(fileSize.QuadPart) <= maximumSize;
    std::vector<char> contents(
        sizeOk ? static_cast<std::size_t>(fileSize.QuadPart) : 0u);
    bool readOk = sizeOk;
    DWORD total = 0;
    while (readOk && total < contents.size()) {
        DWORD current = 0;
        readOk =
            ReadFile(file, contents.data() + total,
                     static_cast<DWORD>(contents.size() - total), &current,
                     nullptr) != FALSE &&
            current != 0;
        total += current;
    }
    CloseHandle(file);
    if (!readOk || total != contents.size()) {
        SetReason(reason, reasonSize,
                  "could not read the encrypted device-binding file");
        return ExistingBinding::Error;
    }
    if (contents.size() <= prefixSize + suffixSize ||
        std::memcmp(contents.data(), kBindingEnvelopePrefix, prefixSize) != 0 ||
        std::memcmp(contents.data() + contents.size() - suffixSize,
                    kBindingEnvelopeSuffix, suffixSize) != 0) {
        SetReason(reason, reasonSize,
                  "existing device binding is not a schema-2 encrypted envelope");
        return ExistingBinding::Mismatch;
    }

    const std::size_t hexSize = contents.size() - prefixSize - suffixSize;
    std::vector<std::uint8_t> ciphertext;
    if (!DecodeHexBytes(contents.data() + prefixSize, hexSize, &ciphertext)) {
        SetReason(reason, reasonSize,
                  "device-binding ciphertext is not canonical uppercase hex");
        return ExistingBinding::Mismatch;
    }
    std::vector<std::uint8_t> payload;
    if (!UnprotectBindingPayload(ciphertext.data(), ciphertext.size(),
                                 &payload, reason, reasonSize)) {
        SecureZeroMemory(ciphertext.data(), ciphertext.size());
        return ExistingBinding::Mismatch;
    }
    SecureZeroMemory(ciphertext.data(), ciphertext.size());
    const bool matches = ConstantTimeEqual(
        payload.data(), payload.size(),
        reinterpret_cast<const std::uint8_t*>(expectedPayload),
        expectedPayloadSize);
    SecureZeroMemory(payload.data(), payload.size());
    if (!matches) {
        SetReason(reason, reasonSize,
                  "existing encrypted device binding does not match this computer");
        return ExistingBinding::Mismatch;
    }
    return ExistingBinding::Matches;
}

enum class PublishResult { Published, DestinationExists, Error };

PublishResult PublishBindingFile(const char* path, const char* contents,
                                 std::size_t size, bool replaceExisting,
                                 char* reason, std::size_t reasonSize) {
    char temporaryPath[MAX_PATH] = {};
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD createError = ERROR_FILE_EXISTS;
    for (unsigned attempt = 0; attempt < 8u; ++attempt) {
        if (_snprintf_s(temporaryPath, sizeof(temporaryPath), _TRUNCATE,
                        "%s.tmp.%08lX.%08lX.%08lX.%u", path,
                        static_cast<unsigned long>(GetCurrentProcessId()),
                        static_cast<unsigned long>(GetCurrentThreadId()),
                        static_cast<unsigned long>(GetTickCount()), attempt) <
            0) {
            SetReason(reason, reasonSize,
                      "temporary binding path is too long");
            return PublishResult::Error;
        }
        file = CreateFileA(temporaryPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        createError = GetLastError();
        if (createError != ERROR_FILE_EXISTS &&
            createError != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        SetReason(reason, reasonSize,
                  "could not create the temporary device binding (error %lu)",
                  static_cast<unsigned long>(createError));
        return PublishResult::Error;
    }
    const bool wrote = WriteComplete(file, contents, size) &&
                       FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!wrote) {
        DeleteFileA(temporaryPath);
        SetReason(reason, reasonSize,
                  "could not write the temporary device binding");
        return PublishResult::Error;
    }

    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replaceExisting) flags |= MOVEFILE_REPLACE_EXISTING;
    if (MoveFileExA(temporaryPath, path, flags)) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return PublishResult::Published;
    }
    const DWORD moveError = GetLastError();
    DeleteFileA(temporaryPath);
    if (moveError == ERROR_ALREADY_EXISTS || moveError == ERROR_FILE_EXISTS) {
        return PublishResult::DestinationExists;
    }
    SetReason(reason, reasonSize,
              "could not publish the device binding (error %lu)",
              static_cast<unsigned long>(moveError));
    return PublishResult::Error;
}

}  // namespace

bool ValidateFooterBytes(const std::uint8_t* footer, std::size_t size,
                         char* reason, std::size_t reasonSize) noexcept {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    constexpr char magic[] = "NFSMWRF1";
    if (footer == nullptr || size != kFooterSize) {
        SetReason(reason, reasonSize, "Reforged footer is missing or truncated");
        return false;
    }
    if (std::memcmp(footer, magic, 8u) != 0 || ReadLe32(footer + 8u) != 1u ||
        ReadLe32(footer + 12u) != kFooterSize ||
        ReadLe32(footer + 16u) != kVerificationCode ||
        ReadLe32(footer + 20u) != kOriginalExecutableSize ||
        ReadLe32(footer + 24u) != kOriginalPeChecksum ||
        ReadLe32(footer + 28u) != 1u) {
        SetReason(reason, reasonSize,
                  "Reforged footer fields or verification code are invalid");
        return false;
    }
    std::uint8_t expectedDigest[32] = {};
    if (!DecodeHex(kFooterOriginalCanonicalSha256, expectedDigest,
                   sizeof(expectedDigest)) ||
        std::memcmp(footer + 32u, expectedDigest, sizeof(expectedDigest)) != 0) {
        SetReason(reason, reasonSize,
                  "Reforged footer original-image digest is invalid");
        return false;
    }
    return true;
}

bool ValidateExecutable(const char* executablePath, std::uint64_t fileSize,
                        const char* md5, char* reason,
                        std::size_t reasonSize) {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    if (executablePath == nullptr || md5 == nullptr) {
        SetReason(reason, reasonSize, "executable validation input is null");
        return false;
    }
    const char* slash = std::strrchr(executablePath, '\\');
    const char* forwardSlash = std::strrchr(executablePath, '/');
    if (forwardSlash != nullptr && (slash == nullptr || forwardSlash > slash)) {
        slash = forwardSlash;
    }
    const char* baseName = slash == nullptr ? executablePath : slash + 1;
    if (_stricmp(baseName, kExecutableName) != 0 &&
        _stricmp(baseName, "speed.exe") != 0) {
        SetReason(reason, reasonSize,
                  "main executable is '%s', expected '%s' or 'speed.exe'",
                  baseName, kExecutableName);
        return false;
    }
    if (fileSize != kStampedExecutableSize ||
        _stricmp(md5, kStampedExecutableMd5) != 0) {
        SetReason(reason, reasonSize,
                  "Reforged executable fingerprint mismatch: size=%llu md5=%s",
                  static_cast<unsigned long long>(fileSize), md5);
        return false;
    }

    HANDLE file = CreateFileA(executablePath, GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetReason(reason, reasonSize, "could not open the Reforged executable");
        return false;
    }
    LARGE_INTEGER actualSize{};
    std::uint8_t footer[kFooterSize] = {};
    char fullSha256[65] = {};
    char canonicalSha256[65] = {};
    bool valid = false;
    do {
        if (!GetFileSizeEx(file, &actualSize) || actualSize.QuadPart < 0 ||
            static_cast<std::uint64_t>(actualSize.QuadPart) != fileSize ||
            fileSize != kStampedExecutableSize) {
            SetReason(reason, reasonSize,
                      "the Reforged executable size changed during validation");
            break;
        }
        if (!ReadFileAt(file, kOriginalExecutableSize, footer,
                        static_cast<DWORD>(sizeof(footer)))) {
            SetReason(reason, reasonSize, "could not read the Reforged footer");
            break;
        }
        if (!ValidateFooterBytes(footer, sizeof(footer), reason, reasonSize)) {
            break;
        }
        if (!ComputeExecutableHashes(file, fullSha256, canonicalSha256, reason,
                                     reasonSize)) {
            break;
        }
        valid = true;
    } while (false);
    CloseHandle(file);
    if (!valid) {
        return false;
    }
    if (_stricmp(fullSha256, kStampedExecutableSha256) != 0) {
        SetReason(reason, reasonSize,
                  "stamped executable SHA-256 mismatch: %s", fullSha256);
        return false;
    }
    if (_stricmp(canonicalSha256, kExpandedCanonicalSha256) != 0) {
        SetReason(reason, reasonSize,
                  "original game-image SHA-256 mismatch: %s",
                  canonicalSha256);
        return false;
    }
    return true;
}

bool BuildBindingPath(const char* executablePath, char* output,
                      std::size_t outputSize, char* reason,
                      std::size_t reasonSize) noexcept {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    if (executablePath == nullptr || output == nullptr || outputSize == 0) {
        SetReason(reason, reasonSize, "binding-path input is null");
        return false;
    }
    const char* slash = std::strrchr(executablePath, '\\');
    const char* forwardSlash = std::strrchr(executablePath, '/');
    if (forwardSlash != nullptr && (slash == nullptr || forwardSlash > slash)) {
        slash = forwardSlash;
    }
    if (slash == nullptr || slash == executablePath) {
        SetReason(reason, reasonSize,
                  "could not resolve the executable directory");
        return false;
    }
    const std::size_t directoryLength =
        static_cast<std::size_t>(slash - executablePath);
    if (directoryLength > static_cast<std::size_t>(INT_MAX) ||
        _snprintf_s(output, outputSize, _TRUNCATE, "%.*s\\SCRIPTS\\%s",
                    static_cast<int>(directoryLength), executablePath,
                    kBindingFileName) < 0) {
        SetReason(reason, reasonSize, "device-binding path is too long");
        return false;
    }
    return true;
}

bool ComputeDeviceId(const DeviceComponents& components, char output[65],
                     unsigned* sourceMask, char* reason,
                     std::size_t reasonSize) {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    if (output == nullptr || !components.hasSystemUuid ||
        !IsUsableUuid(components.systemUuid) ||
        components.machineGuid[0] == '\0' ||
        !components.hasSystemVolumeSerial) {
        SetReason(reason, reasonSize,
                  "required device identity components are missing");
        return false;
    }
    const std::size_t machineGuidSize =
        strnlen_s(components.machineGuid, sizeof(components.machineGuid));
    if (machineGuidSize == 0 ||
        machineGuidSize == sizeof(components.machineGuid)) {
        SetReason(reason, reasonSize, "Windows MachineGuid is malformed");
        return false;
    }

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!BeginSha256(&provider, &hash)) {
        SetReason(reason, reasonSize,
                  "could not initialize the device SHA-256");
        return false;
    }
    const char domain[] = "NFSMWMultiGear.DeviceBinding.v1";
    bool success = HashField(hash, "domain", domain, sizeof(domain) - 1u) &&
                   HashField(hash, "verification-code", &kVerificationCode,
                             sizeof(kVerificationCode));
    const unsigned sources =
        kSystemUuidSource | kMachineGuidSource | kSystemVolumeSource;
    success = success &&
              HashField(hash, "smbios-system-uuid", components.systemUuid,
                        sizeof(components.systemUuid)) &&
              HashField(hash, "windows-machine-guid", components.machineGuid,
                        machineGuidSize) &&
              HashField(hash, "system-volume-serial",
                        &components.systemVolumeSerial,
                        sizeof(components.systemVolumeSerial)) &&
              FinishSha256(hash, output);
    EndHash(provider, hash);
    if (!success) {
        SetReason(reason, reasonSize, "could not calculate the device SHA-256");
        if (output != nullptr) output[0] = '\0';
        return false;
    }
    if (sourceMask != nullptr) *sourceMask = sources;
    return true;
}

bool BuildBindingPayload(const char* deviceId, std::uint32_t schema,
                         char* output, std::size_t outputSize,
                         std::size_t* written, char* reason,
                         std::size_t reasonSize) noexcept {
    return BuildBindingPayloadInternal(deviceId, schema, output, outputSize,
                                       written, reason, reasonSize);
}

bool BuildBindingJson(const char* deviceId, char* output,
                      std::size_t outputSize, std::size_t* written,
                      char* reason, std::size_t reasonSize) noexcept {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    if (written != nullptr) *written = 0;
    if (deviceId == nullptr || output == nullptr || outputSize == 0) {
        SetReason(reason, reasonSize,
                  "device ID or JSON destination is invalid");
        return false;
    }

    char payload[kBindingPayloadCapacity] = {};
    std::size_t payloadSize = 0;
    if (!BuildBindingPayloadInternal(deviceId, kBindingSchema, payload,
                                     sizeof(payload), &payloadSize, reason,
                                     reasonSize)) {
        return false;
    }
    try {
        return BuildEncryptedEnvelope(payload, payloadSize, output, outputSize,
                                       written, reason, reasonSize);
    } catch (...) {
        SetReason(reason, reasonSize,
                  "could not allocate encrypted device-binding buffers");
        return false;
    }
}

bool BuildLegacyBindingJson(const char* deviceId, char* output,
                            std::size_t outputSize, std::size_t* written,
                            char* reason, std::size_t reasonSize) noexcept {
    return BuildBindingPayloadInternal(deviceId, 1u, output, outputSize,
                                       written, reason, reasonSize);
}

bool EnsureBindingFile(const char* path, const char* expected,
                       std::size_t expectedSize, BindingStatus* status,
                       char* reason, std::size_t reasonSize) {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    if (path == nullptr || expected == nullptr || expectedSize == 0 ||
        expectedSize > MAXDWORD || status == nullptr) {
        SetReason(reason, reasonSize, "device-binding input is invalid");
        return false;
    }

    ExistingBinding existing =
        CheckExistingBinding(path, expected, expectedSize, reason, reasonSize);
    if (existing == ExistingBinding::Matches) {
        *status = BindingStatus::Verified;
        return true;
    }
    if (existing != ExistingBinding::Missing) return false;

    char temporaryPath[MAX_PATH] = {};
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD createError = ERROR_FILE_EXISTS;
    for (unsigned attempt = 0; attempt < 8u; ++attempt) {
        if (_snprintf_s(temporaryPath, sizeof(temporaryPath), _TRUNCATE,
                        "%s.tmp.%08lX.%08lX.%08lX.%u", path,
                        static_cast<unsigned long>(GetCurrentProcessId()),
                        static_cast<unsigned long>(GetCurrentThreadId()),
                        static_cast<unsigned long>(GetTickCount()), attempt) <
            0) {
            SetReason(reason, reasonSize, "temporary binding path is too long");
            return false;
        }
        file = CreateFileA(temporaryPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        createError = GetLastError();
        if (createError != ERROR_FILE_EXISTS &&
            createError != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        SetReason(reason, reasonSize,
                  "could not create the temporary device binding (error %lu)",
                  static_cast<unsigned long>(createError));
        return false;
    }
    const bool wrote = WriteComplete(file, expected, expectedSize) &&
                       FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!wrote) {
        DeleteFileA(temporaryPath);
        SetReason(reason, reasonSize,
                  "could not write the temporary device binding");
        return false;
    }

    if (MoveFileExA(temporaryPath, path, MOVEFILE_WRITE_THROUGH)) {
        *status = BindingStatus::Created;
        return true;
    }
    const DWORD moveError = GetLastError();
    DeleteFileA(temporaryPath);
    if (moveError == ERROR_ALREADY_EXISTS || moveError == ERROR_FILE_EXISTS) {
        existing = CheckExistingBinding(path, expected, expectedSize, reason,
                                        reasonSize);
        if (existing == ExistingBinding::Matches) {
            *status = BindingStatus::Verified;
            return true;
        }
        return false;
    }
    SetReason(reason, reasonSize,
              "could not publish the device binding (error %lu)",
              static_cast<unsigned long>(moveError));
    return false;
}

bool EnsureEncryptedBindingFile(const char* path, const char* expectedPayload,
                                std::size_t expectedPayloadSize,
                                const char* legacyExpected,
                                std::size_t legacyExpectedSize,
                                BindingStatus* status, char* reason,
                                std::size_t reasonSize) {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    if (path == nullptr || expectedPayload == nullptr ||
        expectedPayloadSize == 0 ||
        expectedPayloadSize > kBindingPayloadCapacity ||
        expectedPayloadSize > MAXDWORD || status == nullptr ||
        ((legacyExpected == nullptr) != (legacyExpectedSize == 0)) ||
        legacyExpectedSize > kBindingPayloadCapacity ||
        legacyExpectedSize > MAXDWORD) {
        SetReason(reason, reasonSize,
                  "encrypted device-binding input is invalid");
        return false;
    }

    ExistingBinding existing = CheckEncryptedBinding(
        path, expectedPayload, expectedPayloadSize, reason, reasonSize);
    bool migrating = false;
    if (existing == ExistingBinding::Matches) {
        *status = BindingStatus::Verified;
        return true;
    }
    if (existing == ExistingBinding::Missing) {
        // A missing file is the normal first-run path.
    } else if (existing == ExistingBinding::Mismatch &&
               legacyExpected != nullptr) {
        char legacyReason[256] = {};
        const ExistingBinding legacy = CheckExistingBinding(
            path, legacyExpected, legacyExpectedSize, legacyReason,
            sizeof(legacyReason));
        if (legacy == ExistingBinding::Matches) {
            migrating = true;
        } else {
            if (legacy == ExistingBinding::Error && legacyReason[0] != '\0') {
                SetReason(reason, reasonSize, "%s", legacyReason);
            } else {
                SetReason(reason, reasonSize,
                          "existing device binding does not match this computer");
            }
            return false;
        }
    } else {
        return false;
    }

    char encrypted[kBindingJsonCapacity] = {};
    std::size_t encryptedSize = 0;
    if (!BuildEncryptedEnvelope(expectedPayload, expectedPayloadSize,
                                encrypted, sizeof(encrypted), &encryptedSize,
                                reason, reasonSize)) {
        return false;
    }
    const PublishResult published = PublishBindingFile(
        path, encrypted, encryptedSize, migrating, reason, reasonSize);
    SecureZeroMemory(encrypted, sizeof(encrypted));
    if (published == PublishResult::Published) {
        if (migrating) {
            char verifyReason[256] = {};
            if (CheckEncryptedBinding(path, expectedPayload,
                                      expectedPayloadSize, verifyReason,
                                      sizeof(verifyReason)) !=
                ExistingBinding::Matches) {
                SetReason(reason, reasonSize,
                          "published device binding could not be verified");
                return false;
            }
            *status = BindingStatus::Migrated;
        } else {
            *status = BindingStatus::Created;
        }
        return true;
    }
    if (published == PublishResult::DestinationExists) {
        char verifyReason[256] = {};
        if (CheckEncryptedBinding(path, expectedPayload, expectedPayloadSize,
                                  verifyReason, sizeof(verifyReason)) ==
            ExistingBinding::Matches) {
            *status = BindingStatus::Verified;
            return true;
        }
        SetReason(reason, reasonSize,
                  "another device binding was published concurrently");
    }
    return false;
}

bool EnsureCurrentDeviceBinding(const char* executablePath,
                                BindingResult* result, char* reason,
                                std::size_t reasonSize) {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
    if (result == nullptr) {
        SetReason(reason, reasonSize, "device-binding result is null");
        return false;
    }
    try {
        *result = {};
        if (!BuildBindingPath(executablePath, result->path, sizeof(result->path),
                              reason, reasonSize)) {
            return false;
        }

        DeviceComponents components{};
        if (!QueryDeviceComponents(&components, reason, reasonSize)) {
            return false;
        }
        char deviceId[65] = {};
        if (!ComputeDeviceId(components, deviceId, &result->sourceMask, reason,
                             reasonSize)) {
            SecureZeroMemory(&components, sizeof(components));
            return false;
        }
        SecureZeroMemory(&components, sizeof(components));

        char payload[kBindingPayloadCapacity] = {};
        char legacyPayload[kBindingPayloadCapacity] = {};
        std::size_t payloadSize = 0;
        std::size_t legacyPayloadSize = 0;
        if (!BuildBindingPayloadInternal(deviceId, kBindingSchema, payload,
                                         sizeof(payload), &payloadSize, reason,
                                         reasonSize) ||
            !BuildBindingPayloadInternal(deviceId, 1u, legacyPayload,
                                         sizeof(legacyPayload), &legacyPayloadSize,
                                         reason, reasonSize)) {
            SecureZeroMemory(deviceId, sizeof(deviceId));
            return false;
        }
        SecureZeroMemory(deviceId, sizeof(deviceId));
        const bool bound = EnsureEncryptedBindingFile(
            result->path, payload, payloadSize, legacyPayload, legacyPayloadSize,
            &result->status, reason, reasonSize);
        SecureZeroMemory(payload, sizeof(payload));
        SecureZeroMemory(legacyPayload, sizeof(legacyPayload));
        return bound;
    } catch (...) {
        // Device binding is a startup gate. Allocation/DPAPI failures must
        // disable the plugin cleanly instead of escaping into the game thread.
        SecureZeroMemory(result, sizeof(*result));
        SetReason(reason, reasonSize,
                  "unexpected device-binding exception; plugin disabled");
        return false;
    }
}

}  // namespace startup_gate
