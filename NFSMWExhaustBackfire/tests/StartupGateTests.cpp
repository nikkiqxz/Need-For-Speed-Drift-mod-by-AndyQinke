#include "StartupGate.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dpapi.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gate = nfsmw_exhaust::startup_gate;

namespace {

int g_failures = 0;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #expression);                              \
            ++g_failures;                                                     \
        }                                                                     \
    } while (false)

constexpr char kHardwareId[] =
    "0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF";
constexpr char kEntropy[] = "NFSMW.CGPhysicsFix.DeviceAuthority.v1";

std::wstring join(const std::wstring& left, const wchar_t* right) {
    return left + L"\\" + right;
}

bool makeTemporaryDirectory(std::wstring* output) {
    wchar_t temp[MAX_PATH] = {};
    wchar_t name[MAX_PATH] = {};
    if (output == nullptr || GetTempPathW(MAX_PATH, temp) == 0 ||
        GetTempFileNameW(temp, L"nxb", 0, name) == 0 ||
        DeleteFileW(name) == FALSE || CreateDirectoryW(name, nullptr) == FALSE) {
        return false;
    }
    *output = name;
    return true;
}

bool writeFile(const std::wstring& path, const void* data, std::size_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = size <= MAXDWORD &&
        WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) != FALSE &&
        written == size && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return ok;
}

std::string buildPayload(const char* authorityHash) {
    std::array<char, gate::kMaxPlaintextSize> buffer{};
    const int length = _snprintf_s(
        buffer.data(), buffer.size(), _TRUNCATE,
        "{\r\n"
        "  \"schema\": 1,\r\n"
        "  \"product\": \"NFSMW.CGPhysicsFix\",\r\n"
        "  \"authority_module\": \"NFSMW.CGPhysicsFix.asi\",\r\n"
        "  \"authority_module_sha256\": \"%s\",\r\n"
        "  \"validation_status\": \"verified\",\r\n"
        "  \"executable\": \"speed.exe\",\r\n"
        "  \"verification_code\": 868086,\r\n"
        "  \"executable_sha256\": \"%s\",\r\n"
        "  \"executable_canonical_sha256\": \"%s\",\r\n"
        "  \"hardware_id_algorithm\": \"SHA-256\",\r\n"
        "  \"identity_source_mask\": 7,\r\n"
        "  \"hardware_id\": \"%s\"\r\n"
        "}\r\n",
        authorityHash, gate::kExpectedExecutableSha256,
        gate::kCanonicalExecutableSha256, kHardwareId);
    return length > 0 ? std::string(buffer.data(), static_cast<std::size_t>(length))
                      : std::string{};
}

bool protect(const std::string& plaintext,
             std::vector<std::uint8_t>* ciphertext) {
    DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1u),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"test", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE |
                              CRYPTPROTECT_UI_FORBIDDEN,
                          &output)) {
        return false;
    }
    ciphertext->assign(output.pbData, output.pbData + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

std::string envelope(const std::vector<std::uint8_t>& ciphertext) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result =
        "{\r\n"
        "  \"schema\": 1,\r\n"
        "  \"product\": \"NFSMW.CGPhysicsFix\",\r\n"
        "  \"format\": \"DPAPI_LOCAL_MACHINE_HEX_V1\",\r\n"
        "  \"ciphertext_hex\": \"";
    for (std::uint8_t value : ciphertext) {
        result.push_back(digits[value >> 4u]);
        result.push_back(digits[value & 0x0Fu]);
    }
    result += "\"\r\n}\r\n";
    return result;
}

gate::ValidationStatusV1 makeStatus(const char* moduleHash,
                                    const std::wstring& bindingPath) {
    gate::ValidationStatusV1 status{};
    status.structSize = sizeof(status);
    status.abiVersion = 1u;
    status.state = static_cast<long>(gate::AuthorityState::Verified);
    status.bindingSchema = 1u;
    status.deviceSourceMask = gate::kRequiredSourceMask;
    strcpy_s(status.authorityModuleSha256, moduleHash);
    strcpy_s(status.executableSha256, gate::kExpectedExecutableSha256);
    wcscpy_s(status.bindingPath, bindingPath.c_str());
    return status;
}

void testStatusValidation() {
    char reason[256] = {};
    CHECK(!gate::VerifyLoadedAuthority(reason, sizeof(reason)));
    CHECK(!gate::ValidateAuthorityStatus(nullptr, reason, sizeof(reason)));

    gate::ValidationStatusV1 status = makeStatus(
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        L"C:\\MOSTWANTED\\SCRIPTS\\NFSMW.CGPhysicsFix.device.json");
    CHECK(gate::ValidateAuthorityStatus(&status, reason, sizeof(reason)));

    status.state = static_cast<long>(gate::AuthorityState::NotStarted);
    CHECK(!gate::ValidateAuthorityStatus(&status, reason, sizeof(reason)));
    status.state = static_cast<long>(gate::AuthorityState::Validating);
    CHECK(!gate::ValidateAuthorityStatus(&status, reason, sizeof(reason)));
    status.state = static_cast<long>(gate::AuthorityState::Failed);
    CHECK(!gate::ValidateAuthorityStatus(&status, reason, sizeof(reason)));
    status.state = static_cast<long>(gate::AuthorityState::Verified);
    status.abiVersion = 2u;
    CHECK(!gate::ValidateAuthorityStatus(&status, reason, sizeof(reason)));
    status.abiVersion = 1u;
    status.structSize = sizeof(status) - 1u;
    CHECK(!gate::ValidateAuthorityStatus(&status, reason, sizeof(reason)));
    status.structSize = sizeof(status);
    status.bindingSchema = 2u;
    CHECK(!gate::ValidateAuthorityStatus(&status, reason, sizeof(reason)));
    status.bindingSchema = 1u;
    status.deviceSourceMask = 3u;
    CHECK(!gate::ValidateAuthorityStatus(&status, reason, sizeof(reason)));
    status.deviceSourceMask = gate::kRequiredSourceMask;
    strcpy_s(status.executableSha256,
             "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
    CHECK(!gate::ValidateAuthorityStatus(&status, reason, sizeof(reason)));
    strcpy_s(status.executableSha256, gate::kExpectedExecutableSha256);
    strcpy_s(status.failureReason, "authority rejected host");
    CHECK(!gate::ValidateAuthorityStatus(&status, reason, sizeof(reason)));
}

void testStrictParsers() {
    char reason[256] = {};
    constexpr char hash[] =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    const std::string payload = buildPayload(hash);
    CHECK(gate::ValidateDecryptedPayload(
        reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size(),
        hash, reason, sizeof(reason)));
    std::string changed = payload;
    changed[changed.find("verified")] = 'V';
    CHECK(!gate::ValidateDecryptedPayload(
        reinterpret_cast<const std::uint8_t*>(changed.data()), changed.size(),
        hash, reason, sizeof(reason)));

    const std::vector<std::uint8_t> bytes{0x00u, 0xABu, 0xFFu};
    const std::string json = envelope(bytes);
    std::array<std::uint8_t, 16> parsed{};
    std::size_t parsedSize = 0;
    CHECK(gate::ParseEncryptedBindingJson(
        json.data(), json.size(), parsed.data(), parsed.size(), &parsedSize,
        reason, sizeof(reason)));
    CHECK(parsedSize == bytes.size() &&
          std::memcmp(parsed.data(), bytes.data(), bytes.size()) == 0);
    std::string lowercase = json;
    lowercase[lowercase.find("ABFF")] = 'a';
    CHECK(!gate::ParseEncryptedBindingJson(
        lowercase.data(), lowercase.size(), parsed.data(), parsed.size(),
        &parsedSize, reason, sizeof(reason)));
    std::string trailing = json + "\n";
    CHECK(!gate::ParseEncryptedBindingJson(
        trailing.data(), trailing.size(), parsed.data(), parsed.size(),
        &parsedSize, reason, sizeof(reason)));
}

void testSharedAuthorityIntegration() {
    std::wstring root;
    CHECK(makeTemporaryDirectory(&root));
    if (root.empty()) return;
    const std::wstring scripts = join(root, L"SCRIPTS");
    CHECK(CreateDirectoryW(scripts.c_str(), nullptr) != FALSE);
    const std::wstring modulePath = join(scripts, gate::kAuthorityModuleName);
    const std::wstring bindingPath = join(scripts, gate::kBindingFileName);
    const std::wstring executablePath = join(root, L"speed.exe");
    constexpr char moduleBytes[] = "NFSMW validation authority test binary";
    CHECK(writeFile(modulePath, moduleBytes, sizeof(moduleBytes) - 1u));

    char reason[512] = {};
    char moduleHash[65] = {};
    CHECK(gate::ComputeFileSha256(modulePath.c_str(), moduleHash, reason,
                                  sizeof(reason)));
    std::vector<std::uint8_t> ciphertext;
    CHECK(protect(buildPayload(moduleHash), &ciphertext));
    std::string json = envelope(ciphertext);
    CHECK(writeFile(bindingPath, json.data(), json.size()));
    gate::ValidationStatusV1 status = makeStatus(moduleHash, bindingPath);
    CHECK(gate::VerifyAuthoritySnapshot(&status, modulePath.c_str(),
                                        executablePath.c_str(), reason,
                                        sizeof(reason)));

    gate::ValidationStatusV1 wrongHash = status;
    wrongHash.authorityModuleSha256[0] =
        wrongHash.authorityModuleSha256[0] == 'A' ? 'B' : 'A';
    CHECK(!gate::VerifyAuthoritySnapshot(&wrongHash, modulePath.c_str(),
                                         executablePath.c_str(), reason,
                                         sizeof(reason)));

    gate::ValidationStatusV1 escaped = status;
    const std::wstring escapedPath = join(root, L"escaped.json");
    wcscpy_s(escaped.bindingPath, escapedPath.c_str());
    CHECK(!gate::VerifyAuthoritySnapshot(&escaped, modulePath.c_str(),
                                         executablePath.c_str(), reason,
                                         sizeof(reason)));

    const std::wstring otherExecutable =
        L"C:\\other-install\\speed.exe";
    CHECK(!gate::VerifyAuthoritySnapshot(&status, modulePath.c_str(),
                                         otherExecutable.c_str(), reason,
                                         sizeof(reason)));

    std::string tampered = json;
    const std::size_t cipherStart = tampered.find("ciphertext_hex");
    const std::size_t hex = tampered.find('"', cipherStart + 16u) + 1u;
    tampered[hex] = tampered[hex] == 'A' ? 'B' : 'A';
    CHECK(writeFile(bindingPath, tampered.data(), tampered.size()));
    CHECK(!gate::VerifyAuthoritySnapshot(&status, modulePath.c_str(),
                                         executablePath.c_str(), reason,
                                         sizeof(reason)));

    std::vector<std::uint8_t> invalidCiphertext{1u, 2u, 3u, 4u};
    const std::string invalidDpapi = envelope(invalidCiphertext);
    CHECK(writeFile(bindingPath, invalidDpapi.data(), invalidDpapi.size()));
    CHECK(!gate::VerifyAuthoritySnapshot(&status, modulePath.c_str(),
                                         executablePath.c_str(), reason,
                                         sizeof(reason)));

    constexpr char otherHash[] =
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC";
    ciphertext.clear();
    CHECK(protect(buildPayload(otherHash), &ciphertext));
    json = envelope(ciphertext);
    CHECK(writeFile(bindingPath, json.data(), json.size()));
    CHECK(!gate::VerifyAuthoritySnapshot(&status, modulePath.c_str(),
                                         executablePath.c_str(), reason,
                                         sizeof(reason)));

    DeleteFileW(bindingPath.c_str());
    DeleteFileW(modulePath.c_str());
    RemoveDirectoryW(scripts.c_str());
    RemoveDirectoryW(root.c_str());
}

}  // namespace

int main() {
    testStatusValidation();
    testStrictParsers();
    testSharedAuthorityIntegration();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d shared-authority assertion(s) failed\n",
                     g_failures);
        return 1;
    }
    std::puts("Shared NFSMW validation authority consumer verified");
    return 0;
}
