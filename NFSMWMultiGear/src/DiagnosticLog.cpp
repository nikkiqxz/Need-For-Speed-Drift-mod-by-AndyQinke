#include "DiagnosticLog.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace diagnostic_log {

#ifdef NFSMW_MULTIGEAR_DISABLE_LOG

bool Open(const char*) {
    return true;
}

void Write(const char*, ...) {}

#else

namespace {

FILE* g_file = nullptr;
CRITICAL_SECTION g_lock{};
bool g_lockReady = false;

void Emit(const char* text) {
    if (g_lockReady) EnterCriticalSection(&g_lock);

    SYSTEMTIME now{};
    GetLocalTime(&now);

    char line[1024] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[%02u:%02u:%02u.%03u] %s\r\n",
                static_cast<unsigned>(now.wHour),
                static_cast<unsigned>(now.wMinute),
                static_cast<unsigned>(now.wSecond),
                static_cast<unsigned>(now.wMilliseconds), text);

    OutputDebugStringA(line);
    if (g_file != nullptr) {
        fputs(line, g_file);
        fflush(g_file);
    }

    if (g_lockReady) LeaveCriticalSection(&g_lock);
}

}  // namespace

bool Open(const char* path) {
    if (path == nullptr || *path == '\0') return false;
    if (!InitializeCriticalSectionAndSpinCount(&g_lock, 4000)) return false;
    g_lockReady = true;
    if (fopen_s(&g_file, path, "wb") != 0) {
        g_file = nullptr;
        g_lockReady = false;
        DeleteCriticalSection(&g_lock);
        return false;
    }
    Emit("NFSMW MultiGear Phase 4 sidecar multi-gear prototype");
    return true;
}

void Write(const char* format, ...) {
    char text[768] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    Emit(text);
}

#endif

}  // namespace diagnostic_log
