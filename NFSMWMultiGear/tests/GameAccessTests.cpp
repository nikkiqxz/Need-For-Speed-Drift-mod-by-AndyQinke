#include "../src/GameAccess.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++g_failures;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void TestFileMd5() {
    char temporaryDirectory[MAX_PATH] = {};
    const DWORD directoryLength =
        GetTempPathA(static_cast<DWORD>(sizeof(temporaryDirectory)),
                     temporaryDirectory);
    CHECK(directoryLength != 0);
    CHECK(directoryLength < sizeof(temporaryDirectory));
    if (directoryLength == 0 || directoryLength >= sizeof(temporaryDirectory)) {
        return;
    }

    char path[MAX_PATH] = {};
    CHECK(GetTempFileNameA(temporaryDirectory, "NMG", 0, path) != 0);
    if (path[0] == '\0') return;

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) {
        constexpr char contents[] = "abc";
        DWORD bytesWritten = 0;
        const BOOL wrote = WriteFile(file, contents, 3, &bytesWritten, nullptr);
        CHECK(wrote != FALSE);
        CHECK(bytesWritten == 3);
        CHECK(CloseHandle(file) != FALSE);

        if (wrote != FALSE && bytesWritten == 3) {
            char digest[33] = {};
            CHECK(game_access::ComputeFileMd5(path, digest));
            CHECK(std::strcmp(digest,
                              "900150983CD24FB0D6963F7D28E17F72") == 0);
        }
    }

    CHECK(DeleteFileA(path) != FALSE);
}

void TestSafeReadValidAddress() {
    constexpr std::uint32_t expected = 0x1234ABCDu;
    std::uint32_t actual = 0;

    CHECK(game_access::SafeRead(&expected, &actual, sizeof(actual)));
    CHECK(actual == expected);
}

void TestSafeReadInvalidAddress() {
    std::uint32_t destination = 0xFFFFFFFFu;
    const void* invalid =
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(1));

    CHECK(!game_access::SafeRead(invalid, &destination, sizeof(destination)));
}

void TestSafeWrite() {
    std::uint32_t destination = 0;
    constexpr std::uint32_t expected = 0x89ABCDEFu;
    CHECK(game_access::IsWritable(&destination, sizeof(destination)));
    CHECK(game_access::SafeWrite(&destination, &expected, sizeof(expected)));
    CHECK(destination == expected);

    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                              PAGE_READWRITE);
    CHECK(page != nullptr);
    if (page == nullptr) return;

    *static_cast<std::uint32_t*>(page) = 0x12345678u;
    DWORD previous = 0;
    const BOOL protectedReadOnly =
        VirtualProtect(page, 4096, PAGE_READONLY, &previous);
    CHECK(protectedReadOnly != FALSE);
    if (protectedReadOnly != FALSE) {
        CHECK(!game_access::IsWritable(page, sizeof(expected)));
        CHECK(!game_access::SafeWrite(page, &expected, sizeof(expected)));
        CHECK(*static_cast<const std::uint32_t*>(page) == 0x12345678u);
    }
    CHECK(VirtualFree(page, 0, MEM_RELEASE) != FALSE);
}

void TestBuildProfileIdentification() {
    game_access::ExecutableFingerprint fingerprint{};

    fingerprint.fileSize = game_access::kReferenceFileSize;
    strcpy_s(fingerprint.md5, game_access::kReferenceMd5);
    CHECK(game_access::IdentifyBuildProfile(fingerprint) ==
          game_access::BuildProfile::ReferenceC051);

    fingerprint.fileSize = game_access::kV13EnglishCollectorsFileSize;
    strcpy_s(fingerprint.md5, game_access::kV13EnglishCollectorsMd5);
    CHECK(game_access::IdentifyBuildProfile(fingerprint) ==
          game_access::BuildProfile::V13EnglishCollectors);
    CHECK(std::strcmp(game_access::BuildProfileName(
                          game_access::BuildProfile::V13EnglishCollectors),
                      "v1.3-English-Collectors-C5C5") == 0);
    CHECK(!game_access::IsPhase3ProfileSupported(
        game_access::BuildProfile::V13EnglishCollectors));
    fingerprint.fileSize = game_access::kReforgedFileSize;
    strcpy_s(fingerprint.md5, game_access::kReforgedMd5);
    CHECK(game_access::IdentifyBuildProfile(fingerprint) ==
          game_access::BuildProfile::ReforgedC5C5);
    CHECK(std::strcmp(game_access::BuildProfileName(
                          game_access::BuildProfile::ReforgedC5C5),
                      "Reforged-C5C5-868086") == 0);
    CHECK(game_access::IsPhase3ProfileSupported(
        game_access::BuildProfile::ReforgedC5C5));
    CHECK(!game_access::IsPhase3ProfileSupported(
        game_access::BuildProfile::ReferenceC051));
    CHECK(!game_access::IsPhase3ProfileSupported(
        game_access::BuildProfile::Unsupported));

    fingerprint.md5[0] = '0';
    CHECK(game_access::IdentifyBuildProfile(fingerprint) ==
          game_access::BuildProfile::Unsupported);
}

void TestPrivateDataOffset() {
    game_access::PrivateHeader header{};
    CHECK(game_access::PrivateDataOffset(header) == 8u);

    header.metadata = game_access::kPrivateHasExtraPrefix;
    CHECK(game_access::PrivateDataOffset(header) == 16u);

    header.metadata = 0xFFFFu;
    CHECK(game_access::PrivateDataOffset(header) == 16u);
}

}  // namespace

int main() {
    TestFileMd5();
    TestSafeReadValidAddress();
    TestSafeReadInvalidAddress();
    TestSafeWrite();
    TestBuildProfileIdentification();
    TestPrivateDataOffset();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d GameAccess assertion(s) failed\n", g_failures);
        return 1;
    }

    std::puts("GameAccess tests passed");
    return 0;
}
