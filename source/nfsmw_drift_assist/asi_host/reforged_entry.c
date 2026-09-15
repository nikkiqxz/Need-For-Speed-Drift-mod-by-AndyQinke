/*
 * Unified entry shim for the signed Reforged image launched as speed.exe.
 *
 * ASI loaders enter through DllMain and run the plugin on a delayed worker.
 * BepInEx NativeBootstrap calls BepInExNativePlugin_Load directly. The
 * interlocked state ensures the plugin runs at most once when both are
 * present, while still allowing a failed early attempt to be retried.
 */

#include <nfsmw_sdk/platform.h>

#ifndef NFSMW_RELEASE_SILENT
#define NFSMW_RELEASE_SILENT 0
#endif

#if NFSMW_RELEASE_SILENT
#define NFSMW_DEBUG_OUTPUT(message) ((void)(message))
#else
#define NFSMW_DEBUG_OUTPUT(message) OutputDebugStringA(message)
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern int (*nfsmw_plugin_main_ptr)(void);

/* 0 = idle/retryable, 1 = plugin_main in progress, 2 = completed. */
static volatile LONG g_nfsmw_drift_ran = 0;
static HMODULE g_nfsmw_drift_module = NULL;

static int nfsmw_drift_ascii_iequal(wchar_t actual, wchar_t expected) {
    if (actual >= L'A' && actual <= L'Z')
        actual = (wchar_t)(actual - L'A' + L'a');
    if (expected >= L'A' && expected <= L'Z')
        expected = (wchar_t)(expected - L'A' + L'a');
    return actual == expected;
}

static int nfsmw_drift_validate_module_name(HMODULE module,
                                             const wchar_t *expected_name) {
    /* Keep this ceiling in sync with the C++ startup gate. */
    wchar_t path[32768];
    DWORD length;
    const wchar_t *base_name;
    DWORD base_name_length;
    DWORD expected_length;
    DWORD index;

    if (module == NULL || expected_name == NULL)
        return 0;
    length = GetModuleFileNameW(module, path,
                                (DWORD)(sizeof(path) / sizeof(path[0])));
    if (length == 0 || length >= (DWORD)(sizeof(path) / sizeof(path[0])))
        return 0;

    base_name = path;
    for (index = 0; index < length; ++index) {
        if (path[index] == L'\\' || path[index] == L'/')
            base_name = path + index + 1;
    }
    base_name_length = length - (DWORD)(base_name - path);
    expected_length = 0;
    while (expected_name[expected_length] != L'\0') {
        if (expected_length == 32767u)
            return 0;
        ++expected_length;
    }
    if (base_name_length != expected_length)
        return 0;
    for (index = 0; index < expected_length; ++index) {
        if (!nfsmw_drift_ascii_iequal(base_name[index], expected_name[index]))
            return 0;
    }
    return 1;
}

static HMODULE nfsmw_drift_self_module(void) {
    HMODULE module = g_nfsmw_drift_module;
    if (module == NULL) {
        (void)GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void *)&nfsmw_drift_self_module, &module);
    }
    return module;
}

static int nfsmw_drift_validate_plugin_name(void) {
    static const wchar_t expected_name[] =
        L"Slippery_Drifting_FlashFish_by_AndyQinke.asi";
    return nfsmw_drift_validate_module_name(nfsmw_drift_self_module(),
                                             expected_name);
}

static int nfsmw_drift_validate_reforged_exe(void) {
    static const wchar_t expected_name[] =
        L"speed.exe";
    HMODULE module = GetModuleHandleW(NULL);

    /* HMODULE is the image's actual runtime load base. */
    if (module == NULL || (uintptr_t)module != (uintptr_t)0x00400000u)
        return 0;
    return nfsmw_drift_validate_module_name(module, expected_name);
}

static int nfsmw_drift_run_once(const char *via) {
    if (!nfsmw_drift_validate_plugin_name()) {
        NFSMW_DEBUG_OUTPUT(
            "[nfsmw_drift_assist] unsupported plugin filename - refusing\n");
        return 1;
    }
    if (!nfsmw_drift_validate_reforged_exe()) {
        NFSMW_DEBUG_OUTPUT(
            "[nfsmw_drift_assist] unsupported executable name or image base - refusing\n");
        return 1;
    }
    /* Reserve the call only after the lightweight host check succeeds.  A
       transient early-loader failure must leave the gate retryable. */
    if (InterlockedCompareExchange(&g_nfsmw_drift_ran, 1, 0) != 0)
        return 0;
    NFSMW_DEBUG_OUTPUT("[nfsmw_drift_assist] plugin starting via ");
    NFSMW_DEBUG_OUTPUT(via);
    NFSMW_DEBUG_OUTPUT("\n");
    const int result = nfsmw_plugin_main_ptr ? nfsmw_plugin_main_ptr() : -1;
    if (result == 0) {
        InterlockedExchange(&g_nfsmw_drift_ran, 2);
    } else {
        /* The C++ gate can fail transiently while the game/loader settles.
           Its hook installers clean up pre-enable failures, so retrying is
           safe and avoids a permanent no-op after an early BepInEx call. */
        InterlockedExchange(&g_nfsmw_drift_ran, 0);
    }
    return result;
}

#ifndef NFSMW_ASI_INIT_DELAY_MS
#define NFSMW_ASI_INIT_DELAY_MS 2000
#endif

static DWORD WINAPI nfsmw_drift_asi_worker(LPVOID context) {
    (void)context;
    Sleep(NFSMW_ASI_INIT_DELAY_MS);
    /* Retry boundedly after a transient profile/device/loader failure.  A
       successful call moves the state to one and subsequent attempts return
       immediately. */
    for (unsigned attempt = 0; attempt < 15u; ++attempt) {
        const int result = nfsmw_drift_run_once("ASI");
        if (result == 1 ||
            InterlockedCompareExchange(&g_nfsmw_drift_ran, 0, 0) == 2)
            break;
        Sleep(1000);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE worker;
        g_nfsmw_drift_module = instance;
        DisableThreadLibraryCalls(instance);
        worker = CreateThread(NULL, 0, nfsmw_drift_asi_worker, NULL, 0, NULL);
        if (worker != NULL)
            CloseHandle(worker);
    }
    return TRUE;
}

NFSMW_EXPORT int BepInExNativePlugin_Load(void) {
    return nfsmw_drift_run_once("BepInEx");
}

NFSMW_EXPORT const char *BepInExNativePlugin_GUID =
    "nfsmw.slippery-drifting-flashfish.reforged";
NFSMW_EXPORT const char *BepInExNativePlugin_Name =
    "Slippery Drifting FlashFish by AndyQinke";
NFSMW_EXPORT const char *BepInExNativePlugin_Version = "1.1.0";

#ifdef __cplusplus
}
#endif
