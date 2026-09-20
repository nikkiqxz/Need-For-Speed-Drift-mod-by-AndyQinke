#include "NativeLog.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace nfsmw_exhaust::native_log {
#if NFSMW_EXHAUST_ENABLE_LOGGING
namespace {

FILE* g_file = nullptr;
CRITICAL_SECTION g_lock{};
bool g_lockReady = false;

}  // namespace

bool open(const char* path) noexcept {
    if (path == nullptr || *path == '\0') return false;
    if (!InitializeCriticalSectionAndSpinCount(&g_lock, 4000)) return false;
    g_lockReady = true;
    if (fopen_s(&g_file, path, "wb") != 0) {
        DeleteCriticalSection(&g_lock);
        g_lockReady = false;
        return false;
    }
    write("NFSMW Exhaust Backfire native adapter v1.1.15");
    return true;
}

void write(const char* format, ...) noexcept {
    if (format == nullptr) return;

    char message[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[1200] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[%02u:%02u:%02u.%03u] %s\r\n",
                static_cast<unsigned>(now.wHour),
                static_cast<unsigned>(now.wMinute),
                static_cast<unsigned>(now.wSecond),
                static_cast<unsigned>(now.wMilliseconds), message);

    if (g_lockReady) EnterCriticalSection(&g_lock);
    OutputDebugStringA(line);
    if (g_file != nullptr) {
        fputs(line, g_file);
        fflush(g_file);
    }
    if (g_lockReady) LeaveCriticalSection(&g_lock);
}

#else

bool open(const char*) noexcept {
    return false;
}

void write(const char*, ...) noexcept {}

#endif

}  // namespace nfsmw_exhaust::native_log
