#include "asi_host/reforged_startup_gate.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gate = nfsmw_drift::asi_host::reforged_startup_gate;

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++g_failures;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void WriteLe32(std::uint8_t* destination, std::uint32_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8u);
    destination[2] = static_cast<std::uint8_t>(value >> 16u);
    destination[3] = static_cast<std::uint8_t>(value >> 24u);
}

int HexNibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

void BuildValidFooter(
    std::array<std::uint8_t, gate::kFooterSize>* footer) {
    CHECK(footer != nullptr);
    if (footer == nullptr) return;
    footer->fill(0);
    std::memcpy(footer->data(), "NFSMWRF1", 8u);
    WriteLe32(footer->data() + 8u, 1u);
    WriteLe32(footer->data() + 12u,
              static_cast<std::uint32_t>(gate::kFooterSize));
    WriteLe32(footer->data() + 16u, gate::kVerificationCode);
    WriteLe32(footer->data() + 20u,
              static_cast<std::uint32_t>(gate::kOriginalExecutableSize));
    WriteLe32(footer->data() + 24u, gate::kOriginalPeChecksum);
    WriteLe32(footer->data() + 28u, 1u);
    for (std::size_t index = 0; index < 32u; ++index) {
        const int high = HexNibble(
            gate::kFooterOriginalCanonicalSha256[index * 2u]);
        const int low = HexNibble(
            gate::kFooterOriginalCanonicalSha256[index * 2u + 1u]);
        CHECK(high >= 0);
        CHECK(low >= 0);
        footer->at(32u + index) =
            static_cast<std::uint8_t>((high << 4) | low);
    }
}

bool IsUpperHex(const char* value, std::size_t size) {
    if (value == nullptr) return false;
    for (std::size_t index = 0; index < size; ++index) {
        if (HexNibble(value[index]) < 0) return false;
    }
    return true;
}

gate::DeviceComponents MakeDevice() {
    gate::DeviceComponents components{};
    components.hasSystemUuid = true;
    for (std::size_t index = 0; index < sizeof(components.systemUuid);
         ++index) {
        components.systemUuid[index] =
            static_cast<std::uint8_t>(0x80u + index);
    }
    strcpy_s(components.machineGuid,
             "  00112233-4455-6677-8899-aabbccddeeff\r\n");
    components.hasSystemVolumeSerial = true;
    components.systemVolumeSerial = 0x1234ABCDu;
    return components;
}

bool MakeTemporaryDirectory(std::wstring* output) {
    if (output == nullptr) return false;
    output->clear();
    wchar_t root[MAX_PATH] = {};
    const DWORD rootLength = GetTempPathW(MAX_PATH, root);
    if (rootLength == 0 || rootLength >= MAX_PATH) return false;
    wchar_t candidate[MAX_PATH] = {};
    if (GetTempFileNameW(root, L"SDF", 0, candidate) == 0) return false;
    if (!DeleteFileW(candidate)) return false;
    if (!CreateDirectoryW(candidate, nullptr)) return false;
    try {
        output->assign(candidate);
    } catch (...) {
        RemoveDirectoryW(candidate);
        return false;
    }
    return true;
}

std::wstring JoinPath(const std::wstring& directory, const wchar_t* leaf) {
    if (leaf == nullptr) return {};
    return directory + L"\\" + leaf;
}

bool ReadFileBytes(const std::wstring& path,
                   std::vector<std::uint8_t>* output) {
    if (output == nullptr) return false;
    output->clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) >
            gate::kMaxBindingJsonSize) {
        CloseHandle(file);
        return false;
    }
    try {
        output->resize(static_cast<std::size_t>(size.QuadPart));
    } catch (...) {
        CloseHandle(file);
        return false;
    }
    std::size_t total = 0;
    bool success = true;
    while (total < output->size()) {
        DWORD current = 0;
        success = ReadFile(
                      file, output->data() + total,
                      static_cast<DWORD>(output->size() - total), &current,
                      nullptr) != FALSE &&
                  current != 0;
        if (!success) break;
        total += current;
    }
    const bool closed = CloseHandle(file) != FALSE;
    if (!success || total != output->size() || !closed) {
        output->clear();
        return false;
    }
    return true;
}

bool WriteFileBytes(const std::wstring& path,
                    const std::vector<std::uint8_t>& contents) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::size_t total = 0;
    bool success = true;
    while (total < contents.size()) {
        DWORD current = 0;
        success = WriteFile(
                      file, contents.data() + total,
                      static_cast<DWORD>(contents.size() - total), &current,
                      nullptr) != FALSE &&
                  current != 0;
        if (!success) break;
        total += current;
    }
    success = success && FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    return success && total == contents.size() && closed;
}

bool ContainsBytes(const std::vector<std::uint8_t>& haystack,
                   const char* needle) {
    if (needle == nullptr) return false;
    const std::size_t needleSize = std::strlen(needle);
    if (needleSize == 0) return true;
    if (needleSize > haystack.size()) return false;
    return std::search(haystack.begin(), haystack.end(), needle,
                       needle + needleSize) != haystack.end();
}

bool MutateCiphertextHex(std::vector<std::uint8_t>* json) {
    if (json == nullptr) return false;
    constexpr char marker[] = "\"ciphertext_hex\": \"";
    const auto markerBegin = std::search(json->begin(), json->end(), marker,
                                         marker + sizeof(marker) - 1u);
    if (markerBegin == json->end()) return false;
    const auto value = markerBegin + (sizeof(marker) - 1u);
    if (value == json->end()) return false;
    const char original = static_cast<char>(*value);
    if (HexNibble(original) < 0) return false;
    *value = static_cast<std::uint8_t>(original == '0' ? '1' : '0');
    return true;
}

void RemoveFileIfPresent(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    CHECK((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        CHECK(DeleteFileW(path.c_str()) != FALSE);
    }
}

void RemoveDirectoryIfPresent(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    CHECK((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CHECK(RemoveDirectoryW(path.c_str()) != FALSE);
    }
}

void TestFooterValidation() {
    std::array<std::uint8_t, gate::kFooterSize> footer{};
    BuildValidFooter(&footer);
    char reason[256] = {};
    CHECK(gate::ValidateFooterBytes(footer.data(), footer.size(), reason,
                                    sizeof(reason)));
    CHECK(reason[0] == '\0');

    CHECK(!gate::ValidateFooterBytes(nullptr, footer.size(), reason,
                                     sizeof(reason)));
    CHECK(!gate::ValidateFooterBytes(footer.data(), footer.size() - 1u,
                                     reason, sizeof(reason)));

    BuildValidFooter(&footer);
    footer[16] ^= 1u;
    CHECK(!gate::ValidateFooterBytes(footer.data(), footer.size(), reason,
                                     sizeof(reason)));
    CHECK(reason[0] != '\0');

    BuildValidFooter(&footer);
    footer[63] ^= 1u;
    CHECK(!gate::ValidateFooterBytes(footer.data(), footer.size(), reason,
                                     sizeof(reason)));
}

void TestMachineGuidNormalization() {
    char normalized[128] = {};
    char reason[256] = {};
    CHECK(gate::NormalizeMachineGuidAscii(
        " \t00112233-4455-6677-8899-aabbccddeeff\r\n", normalized, reason,
        sizeof(reason)));
    CHECK(std::strcmp(normalized,
                      "00112233-4455-6677-8899-AABBCCDDEEFF") == 0);
    CHECK(reason[0] == '\0');

    char second[128] = {};
    CHECK(gate::NormalizeMachineGuidAscii(
        "00112233-4455-6677-8899-AABBCCDDEEFF", second, reason,
        sizeof(reason)));
    CHECK(std::strcmp(normalized, second) == 0);

    CHECK(!gate::NormalizeMachineGuidAscii(" \t\r\n", normalized, reason,
                                           sizeof(reason)));
    CHECK(!gate::NormalizeMachineGuidAscii("ABC DEF", normalized, reason,
                                           sizeof(reason)));
    const char nonAscii[] = {'A', static_cast<char>(0x80), '\0'};
    CHECK(!gate::NormalizeMachineGuidAscii(nonAscii, normalized, reason,
                                           sizeof(reason)));
    char unterminated[128];
    std::memset(unterminated, 'A', sizeof(unterminated));
    CHECK(!gate::NormalizeMachineGuidAscii(unterminated, normalized, reason,
                                           sizeof(reason)));
}

void TestConstantTimeComparison() {
    const std::uint8_t first[] = {1u, 2u, 3u};
    const std::uint8_t same[] = {1u, 2u, 3u};
    const std::uint8_t changed[] = {1u, 2u, 4u};
    CHECK(gate::ConstantTimeEqual(first, sizeof(first), same, sizeof(same)));
    CHECK(!gate::ConstantTimeEqual(first, sizeof(first), changed,
                                   sizeof(changed)));
    CHECK(!gate::ConstantTimeEqual(first, sizeof(first), same,
                                   sizeof(same) - 1u));
    CHECK(gate::ConstantTimeEqual(nullptr, 0, nullptr, 0));
    CHECK(!gate::ConstantTimeEqual(nullptr, 1u, same, sizeof(same)));
}

void TestUpperHex() {
    const std::uint8_t bytes[] = {0x00u, 0x01u, 0xABu, 0xFFu};
    char encoded[sizeof(bytes) * 2u + 1u] = {};
    char reason[256] = {};
    std::size_t written = 0;
    CHECK(gate::HexEncodeUpper(bytes, sizeof(bytes), encoded, sizeof(encoded),
                               &written, reason, sizeof(reason)));
    CHECK(written == 8u);
    CHECK(std::strcmp(encoded, "0001ABFF") == 0);
    CHECK(reason[0] == '\0');

    std::uint8_t decoded[sizeof(bytes)] = {};
    std::size_t decodedSize = 0;
    CHECK(gate::HexDecodeUpper(encoded, written, decoded, sizeof(decoded),
                               &decodedSize, reason, sizeof(reason)));
    CHECK(decodedSize == sizeof(bytes));
    CHECK(std::memcmp(decoded, bytes, sizeof(bytes)) == 0);

    char tiny[8] = {};
    CHECK(!gate::HexEncodeUpper(bytes, sizeof(bytes), tiny, sizeof(tiny),
                                &written, reason, sizeof(reason)));
    CHECK(!gate::HexDecodeUpper("0001abff", 8u, decoded, sizeof(decoded),
                                &decodedSize, reason, sizeof(reason)));
    CHECK(!gate::HexDecodeUpper("ABC", 3u, decoded, sizeof(decoded),
                                &decodedSize, reason, sizeof(reason)));
    CHECK(!gate::HexDecodeUpper(encoded, 8u, decoded, sizeof(decoded) - 1u,
                                &decodedSize, reason, sizeof(reason)));
}

void TestHardwareId() {
    gate::DeviceComponents components = MakeDevice();
    char first[65] = {};
    char second[65] = {};
    char changed[65] = {};
    char reason[256] = {};
    unsigned sourceMask = 0;
    CHECK(gate::ComputeHardwareId(components, first, &sourceMask, reason,
                                  sizeof(reason)));
    CHECK(std::strlen(first) == gate::kHardwareIdSize);
    CHECK(IsUpperHex(first, gate::kHardwareIdSize));
    CHECK(sourceMask == 0x7u);
    CHECK(gate::ComputeHardwareId(components, second, nullptr, reason,
                                  sizeof(reason)));
    CHECK(std::strcmp(first, second) == 0);

    strcpy_s(components.machineGuid,
             "00112233-4455-6677-8899-AABBCCDDEEFF");
    CHECK(gate::ComputeHardwareId(components, second, nullptr, reason,
                                  sizeof(reason)));
    CHECK(std::strcmp(first, second) == 0);

    ++components.systemVolumeSerial;
    CHECK(gate::ComputeHardwareId(components, changed, nullptr, reason,
                                  sizeof(reason)));
    CHECK(std::strcmp(first, changed) != 0);

    components = MakeDevice();
    components.hasSystemUuid = false;
    CHECK(!gate::ComputeHardwareId(components, changed, nullptr, reason,
                                   sizeof(reason)));
    components = MakeDevice();
    std::memset(components.systemUuid, 0, sizeof(components.systemUuid));
    CHECK(!gate::ComputeHardwareId(components, changed, nullptr, reason,
                                   sizeof(reason)));
    components = MakeDevice();
    components.machineGuid[0] = '\0';
    CHECK(!gate::ComputeHardwareId(components, changed, nullptr, reason,
                                   sizeof(reason)));
    components = MakeDevice();
    components.hasSystemVolumeSerial = false;
    CHECK(!gate::ComputeHardwareId(components, changed, nullptr, reason,
                                   sizeof(reason)));
}

void TestPlaintextPayload() {
    constexpr char hardwareId[] =
        "0123456789ABCDEF0123456789ABCDEF"
        "0123456789ABCDEF0123456789ABCDEF";
    char plaintext[gate::kMaxPlaintextSize] = {};
    char reason[256] = {};
    std::size_t written = 0;
    CHECK(gate::BuildPlaintextPayload(hardwareId, plaintext,
                                      sizeof(plaintext), &written, reason,
                                      sizeof(reason)));
    CHECK(written == std::strlen(plaintext));
    CHECK(std::strstr(plaintext, gate::kProductName) != nullptr);
    CHECK(std::strstr(plaintext, "Need For Speed MW-Reforged.exe") != nullptr);
    CHECK(std::strstr(plaintext, "\"verification_code\": 868086") !=
          nullptr);
    CHECK(std::strstr(plaintext, gate::kBindingExecutableSha256) != nullptr);
    if (std::strcmp(gate::kBindingExecutableSha256,
                    gate::kStampedExecutableSha256) != 0) {
        CHECK(std::strstr(plaintext, gate::kStampedExecutableSha256) ==
              nullptr);
    }
    CHECK(std::strstr(plaintext, "\"identity_source_mask\": 7") != nullptr);
    CHECK(std::strstr(plaintext, hardwareId) != nullptr);

    char invalidId[65] = {};
    std::memset(invalidId, 'a', 64u);
    CHECK(!gate::BuildPlaintextPayload(invalidId, plaintext,
                                       sizeof(plaintext), &written, reason,
                                       sizeof(reason)));
    char tiny[32] = {};
    CHECK(!gate::BuildPlaintextPayload(hardwareId, tiny, sizeof(tiny),
                                       &written, reason, sizeof(reason)));
}

void TestEncryptedEnvelope() {
    const std::uint8_t ciphertext[] = {0x00u, 0x01u, 0xABu, 0xCDu, 0xEFu};
    char json[1024] = {};
    char reason[256] = {};
    std::size_t jsonSize = 0;
    CHECK(gate::BuildEncryptedBindingJson(
        ciphertext, sizeof(ciphertext), json, sizeof(json), &jsonSize, reason,
        sizeof(reason)));
    CHECK(jsonSize == std::strlen(json));
    CHECK(std::strstr(
              json,
              "\"format\": \"DPAPI_LOCAL_MACHINE_HEX_V1\"") != nullptr);
    CHECK(std::strstr(json, "\"ciphertext_hex\": \"0001ABCDEF\"") !=
          nullptr);
    CHECK(std::strstr(json, "hardware_id") == nullptr);
    CHECK(std::strstr(json, gate::kBindingExecutableSha256) == nullptr);

    std::uint8_t parsed[sizeof(ciphertext)] = {};
    std::size_t parsedSize = 0;
    CHECK(gate::ParseEncryptedBindingJson(
        json, jsonSize, parsed, sizeof(parsed), &parsedSize, reason,
        sizeof(reason)));
    CHECK(parsedSize == sizeof(ciphertext));
    CHECK(std::memcmp(parsed, ciphertext, sizeof(ciphertext)) == 0);

    std::string lowercase(json, jsonSize);
    const std::size_t hexPosition = lowercase.find("0001ABCDEF");
    CHECK(hexPosition != std::string::npos);
    if (hexPosition != std::string::npos) {
        lowercase[hexPosition + 4u] = 'a';
        CHECK(!gate::ParseEncryptedBindingJson(
            lowercase.data(), lowercase.size(), parsed, sizeof(parsed),
            &parsedSize, reason, sizeof(reason)));
    }

    std::string nonCanonical(json, jsonSize);
    nonCanonical.push_back('\n');
    CHECK(!gate::ParseEncryptedBindingJson(
        nonCanonical.data(), nonCanonical.size(), parsed, sizeof(parsed),
        &parsedSize, reason, sizeof(reason)));
    CHECK(!gate::ParseEncryptedBindingJson(
        json, jsonSize, parsed, sizeof(parsed) - 1u, &parsedSize, reason,
        sizeof(reason)));
}

void TestDpapiRoundTrip() {
    constexpr char plaintext[] =
        "private-binding-payload-0123456789ABCDEF";
    char reason[256] = {};
    std::vector<std::uint8_t> ciphertext;
    CHECK(gate::ProtectPayloadForLocalMachine(
        reinterpret_cast<const std::uint8_t*>(plaintext),
        sizeof(plaintext) - 1u, &ciphertext, reason, sizeof(reason)));
    CHECK(!ciphertext.empty());
    CHECK(ciphertext.size() <= gate::kMaxCiphertextSize);

    std::vector<std::uint8_t> decrypted;
    if (!ciphertext.empty()) {
        CHECK(gate::UnprotectPayloadForLocalMachine(
            ciphertext.data(), ciphertext.size(), &decrypted, reason,
            sizeof(reason)));
        CHECK(gate::ConstantTimeEqual(
            decrypted.data(), decrypted.size(),
            reinterpret_cast<const std::uint8_t*>(plaintext),
            sizeof(plaintext) - 1u));

        std::vector<std::uint8_t> corrupted = ciphertext;
        corrupted[corrupted.size() / 2u] ^= 1u;
        std::vector<std::uint8_t> rejected;
        CHECK(!gate::UnprotectPayloadForLocalMachine(
            corrupted.data(), corrupted.size(), &rejected, reason,
            sizeof(reason)));
        CHECK(rejected.empty());
    }
    if (!decrypted.empty()) {
        SecureZeroMemory(decrypted.data(), decrypted.size());
    }
}

void TestBindingPath() {
    char reason[256] = {};
    std::wstring path;
    CHECK(gate::BuildBindingPath(
        L"C:\\MOSTWANTED\\speed.exe", &path, reason,
        sizeof(reason)));
    CHECK(path ==
          L"C:\\MOSTWANTED\\SCRIPTS\\"
          L"Slippery_Drifting_FlashFish_by_AndyQinke.device.json");
    CHECK(!gate::BuildBindingPath(
        L"C:\\MOSTWANTED\\speed.exe", nullptr, reason,
        sizeof(reason)));
    CHECK(!gate::BuildBindingPath(L"speed.exe", &path,
                                  reason, sizeof(reason)));
}

void TestBindingFile() {
    std::wstring directory;
    CHECK(MakeTemporaryDirectory(&directory));
    if (directory.empty()) return;
    const std::wstring path = JoinPath(directory, L"binding.json");
    constexpr char expected[] = "binding-payload-for-device-one";
    constexpr char otherDevice[] = "binding-payload-for-device-two";
    gate::BindingStatus status = gate::BindingStatus::Verified;
    char reason[256] = {};

    CHECK(gate::EnsureBindingFile(
        path.c_str(), reinterpret_cast<const std::uint8_t*>(expected),
        sizeof(expected) - 1u, &status, reason, sizeof(reason)));
    CHECK(status == gate::BindingStatus::Created);

    std::vector<std::uint8_t> original;
    CHECK(ReadFileBytes(path, &original));
    CHECK(!original.empty());
    CHECK(!ContainsBytes(original, expected));
    CHECK(ContainsBytes(
        original, "\"format\": \"DPAPI_LOCAL_MACHINE_HEX_V1\""));

    status = gate::BindingStatus::Created;
    CHECK(gate::EnsureBindingFile(
        path.c_str(), reinterpret_cast<const std::uint8_t*>(expected),
        sizeof(expected) - 1u, &status, reason, sizeof(reason)));
    CHECK(status == gate::BindingStatus::Verified);
    std::vector<std::uint8_t> afterVerify;
    CHECK(ReadFileBytes(path, &afterVerify));
    CHECK(afterVerify == original);

    CHECK(!gate::EnsureBindingFile(
        path.c_str(), reinterpret_cast<const std::uint8_t*>(otherDevice),
        sizeof(otherDevice) - 1u, &status, reason, sizeof(reason)));
    CHECK(reason[0] != '\0');
    std::vector<std::uint8_t> afterWrongDevice;
    CHECK(ReadFileBytes(path, &afterWrongDevice));
    CHECK(afterWrongDevice == original);

    std::vector<std::uint8_t> tampered = original;
    CHECK(MutateCiphertextHex(&tampered));
    CHECK(WriteFileBytes(path, tampered));
    CHECK(!gate::EnsureBindingFile(
        path.c_str(), reinterpret_cast<const std::uint8_t*>(expected),
        sizeof(expected) - 1u, &status, reason, sizeof(reason)));
    CHECK(reason[0] != '\0');
    std::vector<std::uint8_t> afterTamper;
    CHECK(ReadFileBytes(path, &afterTamper));
    CHECK(afterTamper == tampered);

    RemoveFileIfPresent(path);
    RemoveDirectoryIfPresent(directory);
}

void TestAutomaticScriptsCreation() {
    std::wstring directory;
    CHECK(MakeTemporaryDirectory(&directory));
    if (directory.empty()) return;
    const std::wstring executable = JoinPath(directory, gate::kExecutableName);
    const std::wstring scripts = JoinPath(directory, L"SCRIPTS");
    CHECK(GetFileAttributesW(scripts.c_str()) == INVALID_FILE_ATTRIBUTES);

    gate::BindingResult result{};
    char reason[512] = {};
    CHECK(gate::EnsureCurrentDeviceBinding(executable.c_str(), &result, reason,
                                           sizeof(reason)));
    CHECK(result.status == gate::BindingStatus::Created);
    CHECK(result.sourceMask == 0x7u);
    CHECK(result.path == JoinPath(scripts, gate::kBindingFileName));
    const DWORD scriptsAttributes = GetFileAttributesW(scripts.c_str());
    CHECK(scriptsAttributes != INVALID_FILE_ATTRIBUTES);
    CHECK((scriptsAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);

    std::vector<std::uint8_t> firstBinding;
    CHECK(ReadFileBytes(result.path, &firstBinding));
    CHECK(!ContainsBytes(firstBinding, "\"hardware_id\""));
    CHECK(!ContainsBytes(firstBinding, gate::kBindingExecutableSha256));
    CHECK(!ContainsBytes(firstBinding, "\"verification_code\""));

    const std::wstring bindingPath = result.path;
    CHECK(gate::EnsureCurrentDeviceBinding(executable.c_str(), &result, reason,
                                           sizeof(reason)));
    CHECK(result.status == gate::BindingStatus::Verified);
    CHECK(result.path == bindingPath);
    std::vector<std::uint8_t> secondBinding;
    CHECK(ReadFileBytes(bindingPath, &secondBinding));
    CHECK(secondBinding == firstBinding);

    RemoveFileIfPresent(bindingPath);
    RemoveDirectoryIfPresent(scripts);
    RemoveDirectoryIfPresent(directory);
}

bool FlipByteAt(const std::wstring& path, std::uint64_t offset) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    bool success = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
    std::uint8_t value = 0;
    DWORD transferred = 0;
    success = success &&
              ReadFile(file, &value, 1u, &transferred, nullptr) != FALSE &&
              transferred == 1u;
    position.QuadPart = static_cast<LONGLONG>(offset);
    success = success &&
              SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
    value ^= 1u;
    transferred = 0;
    success = success &&
              WriteFile(file, &value, 1u, &transferred, nullptr) != FALSE &&
              transferred == 1u && FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    return success && closed;
}

bool TruncateFile(const std::wstring& path, std::uint64_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(size);
    bool success = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
    success = success && SetEndOfFile(file) != FALSE;
    success = success && FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    return success && closed;
}

void TestRealExecutable(const wchar_t* source) {
    CHECK(source != nullptr);
    if (source == nullptr) return;
    char reason[512] = {};

    std::wstring directory;
    CHECK(MakeTemporaryDirectory(&directory));
    if (directory.empty()) return;
    const std::wstring copy = JoinPath(directory, gate::kExecutableName);
    CHECK(CopyFileW(source, copy.c_str(), TRUE) != FALSE);
    if (GetFileAttributesW(copy.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CHECK(gate::ValidateExecutable(copy.c_str(), reason, sizeof(reason)));
        CHECK(reason[0] == '\0');
        CHECK(FlipByteAt(copy, 0x1000u));
        CHECK(!gate::ValidateExecutable(copy.c_str(), reason, sizeof(reason)));
        CHECK(reason[0] != '\0');

    }

    RemoveFileIfPresent(copy);
    RemoveDirectoryIfPresent(directory);

    // The signed bytes are not enough when the old branded basename is used.
    std::wstring oldNameDirectory;
    CHECK(MakeTemporaryDirectory(&oldNameDirectory));
    if (!oldNameDirectory.empty()) {
        const std::wstring oldNameCopy = JoinPath(
            oldNameDirectory, L"Need For Speed MW-Reforged.exe");
        CHECK(CopyFileW(source, oldNameCopy.c_str(), TRUE) != FALSE);
        if (GetFileAttributesW(oldNameCopy.c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            CHECK(!gate::ValidateExecutable(oldNameCopy.c_str(), reason,
                                             sizeof(reason)));
            CHECK(reason[0] != '\0');
        }
        RemoveFileIfPresent(oldNameCopy);
        RemoveDirectoryIfPresent(oldNameDirectory);
    }

    // A plain speed.exe has no footer and must remain rejected. Truncating a
    // fresh verified copy reproduces the exact
    // original image without depending on an external sample path.
    std::wstring plainDirectory;
    CHECK(MakeTemporaryDirectory(&plainDirectory));
    if (!plainDirectory.empty()) {
        const std::wstring plainCopy =
            JoinPath(plainDirectory, gate::kExecutableName);
        CHECK(CopyFileW(source, plainCopy.c_str(), TRUE) != FALSE);
        if (GetFileAttributesW(plainCopy.c_str()) != INVALID_FILE_ATTRIBUTES) {
            CHECK(TruncateFile(plainCopy, gate::kOriginalExecutableSize));
            CHECK(!gate::ValidateExecutable(plainCopy.c_str(), reason,
                                             sizeof(reason)));
            CHECK(reason[0] != '\0');
        }
        RemoveFileIfPresent(plainCopy);
        RemoveDirectoryIfPresent(plainDirectory);
    }
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    TestFooterValidation();
    TestMachineGuidNormalization();
    TestConstantTimeComparison();
    TestUpperHex();
    TestHardwareId();
    TestPlaintextPayload();
    TestEncryptedEnvelope();
    TestDpapiRoundTrip();
    TestBindingPath();
    TestBindingFile();
    TestAutomaticScriptsCreation();
    if (argc >= 2) TestRealExecutable(argv[1]);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d Reforged startup-gate assertion(s) failed\n",
                     g_failures);
        return 1;
    }
    std::puts("Reforged startup-gate tests passed");
    return 0;
}
