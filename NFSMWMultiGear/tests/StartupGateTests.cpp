#include "../src/StartupGate.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

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

void BuildValidFooter(std::uint8_t footer[startup_gate::kFooterSize]) {
    std::memset(footer, 0, startup_gate::kFooterSize);
    std::memcpy(footer, "NFSMWRF1", 8u);
    WriteLe32(footer + 8u, 1u);
    WriteLe32(footer + 12u,
              static_cast<std::uint32_t>(startup_gate::kFooterSize));
    WriteLe32(footer + 16u, startup_gate::kVerificationCode);
    WriteLe32(footer + 20u,
              static_cast<std::uint32_t>(
                  startup_gate::kOriginalExecutableSize));
    WriteLe32(footer + 24u, startup_gate::kOriginalPeChecksum);
    WriteLe32(footer + 28u, 1u);
    for (std::size_t i = 0; i < 32u; ++i) {
        const int high = HexNibble(startup_gate::kFooterOriginalCanonicalSha256[i * 2]);
        const int low =
            HexNibble(startup_gate::kFooterOriginalCanonicalSha256[i * 2 + 1]);
        footer[32u + i] =
            static_cast<std::uint8_t>((high << 4) | low);
    }
}

void TestFooterValidation() {
    std::uint8_t footer[startup_gate::kFooterSize] = {};
    BuildValidFooter(footer);
    char reason[256] = {};
    CHECK(startup_gate::ValidateFooterBytes(
        footer, sizeof(footer), reason, sizeof(reason)));
    CHECK(reason[0] == '\0');

    CHECK(!startup_gate::ValidateFooterBytes(
        footer, sizeof(footer) - 1u, reason, sizeof(reason)));
    CHECK(reason[0] != '\0');

    BuildValidFooter(footer);
    footer[16] ^= 1u;
    CHECK(!startup_gate::ValidateFooterBytes(
        footer, sizeof(footer), reason, sizeof(reason)));

    BuildValidFooter(footer);
    footer[63] ^= 1u;
    CHECK(!startup_gate::ValidateFooterBytes(
        footer, sizeof(footer), reason, sizeof(reason)));
}

void TestBindingPath() {
    char path[MAX_PATH] = {};
    char reason[256] = {};
    CHECK(startup_gate::BuildBindingPath(
        "C:\\MOSTWANTED\\Need For Speed MW-Reforged.exe", path,
        sizeof(path), reason, sizeof(reason)));
    CHECK(std::strcmp(
              path,
              "C:\\MOSTWANTED\\SCRIPTS\\NFSMWMultiGear.device.json") == 0);
    CHECK(!startup_gate::BuildBindingPath(
        "Need For Speed MW-Reforged.exe", path, sizeof(path), reason,
        sizeof(reason)));
    char tiny[8] = {};
    CHECK(!startup_gate::BuildBindingPath(
        "C:\\MOSTWANTED\\Need For Speed MW-Reforged.exe", tiny,
        sizeof(tiny), reason, sizeof(reason)));
}

startup_gate::DeviceComponents MakeDevice() {
    startup_gate::DeviceComponents components{};
    components.hasSystemUuid = true;
    for (std::size_t i = 0; i < sizeof(components.systemUuid); ++i) {
        components.systemUuid[i] = static_cast<std::uint8_t>(0x80u + i);
    }
    strcpy_s(components.machineGuid,
             "00112233-4455-6677-8899-AABBCCDDEEFF");
    components.hasSystemVolumeSerial = true;
    components.systemVolumeSerial = 0x1234ABCDu;
    return components;
}

void TestDeviceId() {
    startup_gate::DeviceComponents components = MakeDevice();
    char first[65] = {};
    char second[65] = {};
    char changed[65] = {};
    char reason[256] = {};
    unsigned sourceMask = 0;
    CHECK(startup_gate::ComputeDeviceId(
        components, first, &sourceMask, reason, sizeof(reason)));
    CHECK(std::strlen(first) == 64u);
    CHECK(sourceMask == 0x7u);
    CHECK(startup_gate::ComputeDeviceId(
        components, second, nullptr, reason, sizeof(reason)));
    CHECK(std::strcmp(first, second) == 0);

    ++components.systemVolumeSerial;
    CHECK(startup_gate::ComputeDeviceId(
        components, changed, nullptr, reason, sizeof(reason)));
    CHECK(std::strcmp(first, changed) != 0);

    components = MakeDevice();
    for (std::size_t i = 0; i < sizeof(components.systemUuid); ++i) {
        components.systemUuid[i] = static_cast<std::uint8_t>(i + 1u);
    }
    CHECK(startup_gate::ComputeDeviceId(
        components, changed, &sourceMask, reason, sizeof(reason)));
    CHECK(sourceMask == 0x7u);
    CHECK(std::strcmp(first, changed) != 0);

    components = MakeDevice();
    components.hasSystemUuid = false;
    CHECK(!startup_gate::ComputeDeviceId(
        components, changed, nullptr, reason, sizeof(reason)));
    components = MakeDevice();
    components.machineGuid[0] = '\0';
    CHECK(!startup_gate::ComputeDeviceId(
        components, changed, nullptr, reason, sizeof(reason)));
    components = MakeDevice();
    components.hasSystemVolumeSerial = false;
    CHECK(!startup_gate::ComputeDeviceId(
        components, changed, nullptr, reason, sizeof(reason)));
}

void TestBindingJson() {
    constexpr char deviceId[] =
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    char json[startup_gate::kBindingJsonCapacity] = {};
    char reason[256] = {};
    std::size_t written = 0;
    CHECK(startup_gate::BuildBindingJson(
        deviceId, json, sizeof(json), &written, reason, sizeof(reason)));
    CHECK(written == std::strlen(json));
    CHECK(std::strstr(json, "\"schema\": 2") != nullptr);
    CHECK(std::strstr(json,
                     "\"encoding\": \"DPAPI-LOCAL-MACHINE-HEX\"") !=
          nullptr);
    CHECK(std::strstr(json, "\"ciphertext_hex\": \"") != nullptr);
    // The outer file must not expose the payload or the device digest.
    CHECK(std::strstr(json, "NFSMWMultiGear") == nullptr);
    CHECK(std::strstr(json, startup_gate::kExecutableName) == nullptr);
    CHECK(std::strstr(json, startup_gate::kBindingExecutableSha256) == nullptr);
    CHECK(std::strstr(json, deviceId) == nullptr);

    constexpr char marker[] = "\"ciphertext_hex\": \"";
    const char* begin = std::strstr(json, marker);
    CHECK(begin != nullptr);
    if (begin != nullptr) {
        begin += std::strlen(marker);
        const char* end = std::strchr(begin, '\"');
        CHECK(end != nullptr);
        if (end != nullptr) {
            const std::size_t hexSize =
                static_cast<std::size_t>(end - begin);
            CHECK(hexSize != 0u && (hexSize & 1u) == 0u);
            for (std::size_t i = 0; i < hexSize; ++i) {
                const char value = begin[i];
                CHECK((value >= '0' && value <= '9') ||
                      (value >= 'A' && value <= 'F'));
            }
        }
    }

    char payload[1024] = {};
    std::size_t payloadSize = 0;
    CHECK(startup_gate::BuildBindingPayload(
        deviceId, startup_gate::kBindingSchema, payload, sizeof(payload),
        &payloadSize, reason, sizeof(reason)));
    CHECK(std::strstr(payload, "\"schema\": 2") != nullptr);

    char legacy[1024] = {};
    std::size_t legacySize = 0;
    CHECK(startup_gate::BuildLegacyBindingJson(
        deviceId, legacy, sizeof(legacy), &legacySize, reason,
        sizeof(reason)));
    CHECK(std::strstr(legacy, "\"schema\": 1") != nullptr);
    CHECK(std::strstr(legacy, deviceId) != nullptr);

    char invalidId[65] = {};
    std::memset(invalidId, 'a', 64u);
    CHECK(!startup_gate::BuildBindingJson(
        invalidId, json, sizeof(json), &written, reason, sizeof(reason)));
    char tiny[16] = {};
    CHECK(!startup_gate::BuildBindingJson(
        deviceId, tiny, sizeof(tiny), &written, reason, sizeof(reason)));
}

bool MakeTemporaryDirectory(char output[MAX_PATH]) {
    char root[MAX_PATH] = {};
    const DWORD rootLength = GetTempPathA(MAX_PATH, root);
    if (rootLength == 0 || rootLength >= MAX_PATH) return false;
    if (GetTempFileNameA(root, "NMG", 0, output) == 0) return false;
    if (!DeleteFileA(output)) return false;
    return CreateDirectoryA(output, nullptr) != FALSE;
}

bool ReadExactFile(const char* path, char* output, DWORD size) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD read = 0;
    const bool result = ReadFile(file, output, size, &read, nullptr) != FALSE &&
                        read == size;
    CloseHandle(file);
    return result;
}

bool ReadFileBytes(const char* path, std::vector<char>* output) {
    if (path == nullptr || output == nullptr) return false;
    output->clear();
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool sizeOk = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
    if (!sizeOk || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    output->assign(static_cast<std::size_t>(size.QuadPart), 0);
    DWORD total = 0;
    bool readOk = true;
    while (readOk && total < output->size()) {
        DWORD current = 0;
        readOk = ReadFile(file, output->data() + total,
                          static_cast<DWORD>(output->size() - total), &current,
                          nullptr) != FALSE &&
                 current != 0;
        total += current;
    }
    CloseHandle(file);
    return readOk && total == output->size();
}

bool WriteFileBytes(const char* path, const char* contents, std::size_t size) {
    if (path == nullptr || contents == nullptr || size > MAXDWORD) return false;
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::size_t total = 0;
    bool writeOk = true;
    while (writeOk && total < size) {
        DWORD current = 0;
        writeOk = WriteFile(file, contents + total,
                            static_cast<DWORD>(size - total), &current,
                            nullptr) != FALSE &&
                  current != 0;
        total += current;
    }
    const bool closed = CloseHandle(file) != FALSE;
    return writeOk && closed && total == size;
}

bool ContainsBytes(const std::vector<char>& contents, const char* needle) {
    if (needle == nullptr) return false;
    const std::size_t needleSize = std::strlen(needle);
    if (needleSize == 0 || needleSize > contents.size()) return false;
    for (std::size_t i = 0; i + needleSize <= contents.size(); ++i) {
        if (std::memcmp(contents.data() + i, needle, needleSize) == 0) {
            return true;
        }
    }
    return false;
}

void TestEncryptedBindingFile() {
    constexpr char deviceId[] =
        "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    constexpr char otherDeviceId[] =
        "FEDCBA9876543210FEDCBA9876543210FEDCBA9876543210FEDCBA9876543210";
    char payload[1024] = {};
    char legacy[1024] = {};
    char otherLegacy[1024] = {};
    char reason[256] = {};
    std::size_t payloadSize = 0;
    std::size_t legacySize = 0;
    std::size_t otherLegacySize = 0;
    CHECK(startup_gate::BuildBindingPayload(
        deviceId, startup_gate::kBindingSchema, payload, sizeof(payload),
        &payloadSize, reason, sizeof(reason)));
    CHECK(startup_gate::BuildLegacyBindingJson(
        deviceId, legacy, sizeof(legacy), &legacySize, reason,
        sizeof(reason)));
    CHECK(startup_gate::BuildLegacyBindingJson(
        otherDeviceId, otherLegacy, sizeof(otherLegacy), &otherLegacySize,
        reason, sizeof(reason)));

    char directory[MAX_PATH] = {};
    CHECK(MakeTemporaryDirectory(directory));
    if (directory[0] == '\0') return;

    char path[MAX_PATH] = {};
    CHECK(sprintf_s(path, "%s\\encrypted.json", directory) > 0);
    startup_gate::BindingStatus status = startup_gate::BindingStatus::Verified;
    CHECK(startup_gate::EnsureEncryptedBindingFile(
        path, payload, payloadSize, legacy, legacySize, &status, reason,
        sizeof(reason)));
    CHECK(status == startup_gate::BindingStatus::Created);

    std::vector<char> contents;
    CHECK(ReadFileBytes(path, &contents));
    CHECK(contents.size() > 0u && contents.size() < startup_gate::kBindingJsonCapacity);
    CHECK(!ContainsBytes(contents, "hardware_id"));
    CHECK(!ContainsBytes(contents, deviceId));
    CHECK(ContainsBytes(contents, "ciphertext_hex"));

    CHECK(startup_gate::EnsureEncryptedBindingFile(
        path, payload, payloadSize, legacy, legacySize, &status, reason,
        sizeof(reason)));
    CHECK(status == startup_gate::BindingStatus::Verified);

    // A single altered ciphertext nibble must fail closed.
    const char marker[] = "\"ciphertext_hex\": \"";
    std::size_t markerOffset = contents.size();
    for (std::size_t i = 0; i + sizeof(marker) - 1u <= contents.size(); ++i) {
        if (std::memcmp(contents.data() + i, marker, sizeof(marker) - 1u) == 0) {
            markerOffset = i + sizeof(marker) - 1u;
            break;
        }
    }
    CHECK(markerOffset < contents.size());
    if (markerOffset < contents.size()) {
        contents[markerOffset] = contents[markerOffset] == '0' ? '1' : '0';
        CHECK(WriteFileBytes(path, contents.data(), contents.size()));
        CHECK(!startup_gate::EnsureEncryptedBindingFile(
            path, payload, payloadSize, legacy, legacySize, &status, reason,
            sizeof(reason)));
    }
    CHECK(DeleteFileA(path) != FALSE);

    // The exact v1 plaintext is migrated once, but a different v1 binding is
    // never overwritten.
    char legacyPath[MAX_PATH] = {};
    CHECK(sprintf_s(legacyPath, "%s\\legacy.json", directory) > 0);
    CHECK(startup_gate::EnsureBindingFile(
        legacyPath, legacy, legacySize, &status, reason, sizeof(reason)));
    CHECK(startup_gate::EnsureEncryptedBindingFile(
        legacyPath, payload, payloadSize, legacy, legacySize, &status, reason,
        sizeof(reason)));
    CHECK(status == startup_gate::BindingStatus::Migrated);
    contents.clear();
    CHECK(ReadFileBytes(legacyPath, &contents));
    CHECK(!ContainsBytes(contents, "hardware_id"));
    CHECK(startup_gate::EnsureEncryptedBindingFile(
        legacyPath, payload, payloadSize, legacy, legacySize, &status, reason,
        sizeof(reason)));
    CHECK(status == startup_gate::BindingStatus::Verified);

    char mismatchPath[MAX_PATH] = {};
    CHECK(sprintf_s(mismatchPath, "%s\\mismatch.json", directory) > 0);
    CHECK(startup_gate::EnsureBindingFile(
        mismatchPath, otherLegacy, otherLegacySize, &status, reason,
        sizeof(reason)));
    std::vector<char> mismatchBefore;
    CHECK(ReadFileBytes(mismatchPath, &mismatchBefore));
    CHECK(!startup_gate::EnsureEncryptedBindingFile(
        mismatchPath, payload, payloadSize, legacy, legacySize, &status, reason,
        sizeof(reason)));
    std::vector<char> mismatchAfter;
    CHECK(ReadFileBytes(mismatchPath, &mismatchAfter));
    CHECK(mismatchBefore == mismatchAfter);

    CHECK(DeleteFileA(legacyPath) != FALSE);
    CHECK(DeleteFileA(mismatchPath) != FALSE);
    CHECK(RemoveDirectoryA(directory) != FALSE);
}

void TestBindingFile() {
    char directory[MAX_PATH] = {};
    CHECK(MakeTemporaryDirectory(directory));
    if (directory[0] == '\0') return;

    char path[MAX_PATH] = {};
    CHECK(sprintf_s(path, "%s\\binding.json", directory) > 0);
    constexpr char expected[] = "one";
    constexpr char mismatch[] = "two";
    startup_gate::BindingStatus status = startup_gate::BindingStatus::Verified;
    char reason[256] = {};
    CHECK(startup_gate::EnsureBindingFile(
        path, expected, sizeof(expected) - 1u, &status, reason,
        sizeof(reason)));
    CHECK(status == startup_gate::BindingStatus::Created);
    CHECK(startup_gate::EnsureBindingFile(
        path, expected, sizeof(expected) - 1u, &status, reason,
        sizeof(reason)));
    CHECK(status == startup_gate::BindingStatus::Verified);
    CHECK(!startup_gate::EnsureBindingFile(
        path, mismatch, sizeof(mismatch) - 1u, &status, reason,
        sizeof(reason)));

    char actual[sizeof(expected)] = {};
    CHECK(ReadExactFile(path, actual,
                        static_cast<DWORD>(sizeof(expected) - 1u)));
    CHECK(std::memcmp(actual, expected, sizeof(expected) - 1u) == 0);
    CHECK(DeleteFileA(path) != FALSE);
    CHECK(RemoveDirectoryA(directory) != FALSE);
}

void TestCurrentDeviceBinding() {
    char directory[MAX_PATH] = {};
    CHECK(MakeTemporaryDirectory(directory));
    if (directory[0] == '\0') return;

    char scripts[MAX_PATH] = {};
    char executable[MAX_PATH] = {};
    CHECK(sprintf_s(scripts, "%s\\SCRIPTS", directory) > 0);
    CHECK(CreateDirectoryA(scripts, nullptr) != FALSE);
    CHECK(sprintf_s(executable, "%s\\%s", directory,
                    startup_gate::kExecutableName) > 0);

    startup_gate::BindingResult result{};
    char reason[256] = {};
    CHECK(startup_gate::EnsureCurrentDeviceBinding(
        executable, &result, reason, sizeof(reason)));
    CHECK(result.status == startup_gate::BindingStatus::Created);
    CHECK(result.sourceMask == 0x7u);
    CHECK(startup_gate::EnsureCurrentDeviceBinding(
        executable, &result, reason, sizeof(reason)));
    CHECK(result.status == startup_gate::BindingStatus::Verified);

    CHECK(DeleteFileA(result.path) != FALSE);
    CHECK(RemoveDirectoryA(scripts) != FALSE);
    CHECK(RemoveDirectoryA(directory) != FALSE);
}

bool CopyForExecutableTest(const char* source, const char* directory,
                           const char* name,
                           char destination[MAX_PATH]) {
    if (sprintf_s(destination, MAX_PATH, "%s\\%s", directory, name) <= 0) {
        return false;
    }
    return CopyFileA(source, destination, FALSE) != FALSE;
}

void TestRealExecutable(const char* source) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    CHECK(GetFileAttributesExA(source, GetFileExInfoStandard, &attributes) !=
          FALSE);
    const std::uint64_t fileSize =
        (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32u) |
        attributes.nFileSizeLow;
    char reason[512] = {};
    CHECK(startup_gate::ValidateExecutable(
        source, fileSize, startup_gate::kStampedExecutableMd5, reason,
        sizeof(reason)));

    char directory[MAX_PATH] = {};
    CHECK(MakeTemporaryDirectory(directory));
    if (directory[0] == '\0') return;
    char copy[MAX_PATH] = {};
    CHECK(CopyForExecutableTest(source, directory,
                                startup_gate::kExecutableName, copy));
    if (copy[0] != '\0') {
        HANDLE file = CreateFileA(copy, GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER position{};
            position.QuadPart = 0x1000;
            CHECK(SetFilePointerEx(file, position, nullptr, FILE_BEGIN) !=
                  FALSE);
            std::uint8_t value = 0;
            DWORD transferred = 0;
            CHECK(ReadFile(file, &value, 1, &transferred, nullptr) != FALSE);
            CHECK(transferred == 1u);
            position.QuadPart = 0x1000;
            CHECK(SetFilePointerEx(file, position, nullptr, FILE_BEGIN) !=
                  FALSE);
            value ^= 1u;
            transferred = 0;
            CHECK(WriteFile(file, &value, 1, &transferred, nullptr) != FALSE);
            CHECK(transferred == 1u);
            CHECK(CloseHandle(file) != FALSE);
            CHECK(!startup_gate::ValidateExecutable(
                copy, fileSize, startup_gate::kStampedExecutableMd5, reason,
                sizeof(reason)));
        }
        CHECK(DeleteFileA(copy) != FALSE);
    }
    CHECK(RemoveDirectoryA(directory) != FALSE);

    CHECK(MakeTemporaryDirectory(directory));
    if (directory[0] != '\0') {
        char speedCopy[MAX_PATH] = {};
        CHECK(CopyForExecutableTest(source, directory, "speed.exe", speedCopy));
        CHECK(startup_gate::ValidateExecutable(
            speedCopy, fileSize, startup_gate::kStampedExecutableMd5, reason,
            sizeof(reason)));
        CHECK(DeleteFileA(speedCopy) != FALSE);
        CHECK(RemoveDirectoryA(directory) != FALSE);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--ensure-current-binding") == 0) {
        const char* executable =
            argc >= 3 ? argv[2]
                      : "C:\\MOSTWANTED\\Need For Speed MW-Reforged.exe";
        startup_gate::BindingResult result{};
        char reason[512] = {};
        const bool success = startup_gate::EnsureCurrentDeviceBinding(
            executable, &result, reason, sizeof(reason));
        if (success) {
            const char* status = "verified";
            if (result.status == startup_gate::BindingStatus::Created) {
                status = "created";
            } else if (result.status == startup_gate::BindingStatus::Migrated) {
                status = "migrated";
            }
            std::printf("device binding %s: %s\n", status, result.path);
            return 0;
        }
        std::fprintf(stderr, "device binding failed: %s\n", reason);
        return 1;
    }
    TestFooterValidation();
    TestBindingPath();
    TestDeviceId();
    TestBindingJson();
    TestBindingFile();
    TestEncryptedBindingFile();
    TestCurrentDeviceBinding();
    if (argc >= 2) TestRealExecutable(argv[1]);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d StartupGate assertion(s) failed\n", g_failures);
        return 1;
    }
    std::puts("StartupGate tests passed");
    return 0;
}
