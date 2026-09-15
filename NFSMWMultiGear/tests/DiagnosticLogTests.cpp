#include "../src/DiagnosticLog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,   \
                         __LINE__, #condition);                              \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

}  // namespace

int main() {
    char temporaryDirectory[MAX_PATH] = {};
    CHECK(GetTempPathA(MAX_PATH, temporaryDirectory) != 0);

    char path[MAX_PATH] = {};
    CHECK(sprintf_s(path, "%sNFSMWMultiGear-disabled-%lu.log",
                    temporaryDirectory,
                    static_cast<unsigned long>(GetCurrentProcessId())) > 0);
    DeleteFileA(path);

    CHECK(diagnostic_log::Open(path));
    diagnostic_log::Write("this must not be emitted");
    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetLastError() == ERROR_FILE_NOT_FOUND);

    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) DeleteFileA(path);
    if (g_failures != 0) return 1;
    std::puts("Disabled diagnostic log tests passed");
    return 0;
}
