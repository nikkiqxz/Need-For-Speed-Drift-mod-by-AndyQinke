#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "drift_assist.hpp"
#include "drift_assist_ini.hpp"
#include "version_guard.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#ifndef NFSMW_ENABLE_UNVERIFIED_HOOKS
#define NFSMW_ENABLE_UNVERIFIED_HOOKS 0
#endif

namespace {

using nfsmw_drift::AssistConfig;
using nfsmw_drift::DriftAssistController;

HMODULE g_selfModule = nullptr;
std::atomic<bool> g_started{false};
DriftAssistController g_controller{};

void Log(const char* message) {
    if (message != nullptr) {
        OutputDebugStringA("[nfsmw_drift_assist] ");
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }
}

std::string ModuleDirectory() {
    char path[1024]{};
    const DWORD length = GetModuleFileNameA(
        g_selfModule, path, static_cast<DWORD>(sizeof(path)));
    if (length == 0 || length >= sizeof(path)) {
        return {};
    }

    std::string result(path, length);
    const std::string::size_type separator = result.find_last_of("\\/");
    if (separator == std::string::npos) {
        return {};
    }
    result.resize(separator + 1);
    return result;
}

bool ReadConfig(const std::string& path, std::string& text) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }

    stream.seekg(0, std::ios::end);
    const std::streampos end = stream.tellg();
    if (end < 0 || end > static_cast<std::streamoff>(1024 * 1024)) {
        return false;
    }
    stream.seekg(0, std::ios::beg);
    text.assign(std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
}

void LogFingerprint(const nfsmw_drift::asi_host::TargetFingerprint& fingerprint) {
    char line[256]{};
    std::snprintf(line, sizeof(line),
                  "target base=0x%p entry=0x%08lX fileSize=%llu known=%s",
                  reinterpret_cast<void*>(fingerprint.moduleBase),
                  static_cast<unsigned long>(fingerprint.entryPointVa),
                  static_cast<unsigned long long>(fingerprint.onDiskFileSize),
                  nfsmw_drift::asi_host::IsKnownV13English(fingerprint) ? "yes"
                                                                           : "no");
    Log(line);
}

void Initialize() {
    const nfsmw_drift::asi_host::TargetFingerprint fingerprint =
        nfsmw_drift::asi_host::InspectMainModule();
    LogFingerprint(fingerprint);
    if (!nfsmw_drift::asi_host::IsKnownV13English(fingerprint)) {
        Log("unsupported speed.exe; no hooks or memory writes were installed");
        Log(nfsmw_drift::asi_host::FirstFailure(fingerprint));
        return;
    }

    const std::string directory = ModuleDirectory();
    if (directory.empty()) {
        Log("could not resolve the ASI module directory; using controller defaults");
        return;
    }

    AssistConfig config{};
    const std::string configPath = directory + "drift_assist.ini";
    std::string configText;
    if (!ReadConfig(configPath, configText)) {
        Log("drift_assist.ini was not found/readable; controller remains disabled until a host hook is added");
        config.enabled = false;
        g_controller.setConfig(config);
        g_controller.reset();
        return;
    }

    const nfsmw_drift::IniParseResult parse =
        nfsmw_drift::ParseAssistConfigIni(configText, config);
    if (!parse) {
        char line[160]{};
        std::snprintf(line, sizeof(line),
                      "invalid INI at line %llu; controller disabled",
                      static_cast<unsigned long long>(parse.line));
        Log(line);
        config.enabled = false;
    }

    g_controller.setConfig(config);
    g_controller.reset();
    Log(config.enabled ? "configuration loaded" : "configuration loaded but disabled");

#if NFSMW_ENABLE_UNVERIFIED_HOOKS
    // This branch is a deliberate reminder for the next integration step.
    // It does not install a hook: fixed addresses must first pass per-site
    // AOB checks against the exact executable supplied by the user.
    Log("NFSMW_ENABLE_UNVERIFIED_HOOKS is ON, but no unverified hook is available in this host");
#else
    Log("bootstrap ready; input/physics hooks remain disabled pending target-specific validation");
#endif
}

DWORD WINAPI InitializationThread(LPVOID) {
    // Let the loader and the game's early initialization finish before any
    // inspection.  No game calls are made from DllMain itself.
    Sleep(2000);
    if (!g_started.exchange(true)) {
        Initialize();
    }
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = module;
        DisableThreadLibraryCalls(module);
        HANDLE worker = CreateThread(nullptr, 0, InitializationThread, nullptr, 0,
                                     nullptr);
        if (worker != nullptr) {
            CloseHandle(worker);
        }
    }
    return TRUE;
}
