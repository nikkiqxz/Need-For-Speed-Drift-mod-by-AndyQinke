/* Project-local ASI entry shim. Accept both the original speed.exe name and
 * the branded Reforged name, then let the strict footer/device gate verify the
 * actual executable bytes before any hooks are installed. */

#include <nfsmw_sdk/platform.h>

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NFSMW_MULTIGEAR_VERSION
#define NFSMW_MULTIGEAR_VERSION "dev"
#endif

extern int (*nfsmw_plugin_main_ptr)(void);

static volatile LONG g_nfsmw_ran = 0;

static int nfsmw_validate_reforged_host(void) {
    HMODULE module = GetModuleHandleA(NULL);
    if (!module) return 0;

    DWORD nt_offset = *(DWORD*)((BYTE*)module + 0x3C);
    DWORD image_base = *(DWORD*)((BYTE*)module + nt_offset + 0x34);
    if (image_base != NFSMW_IMAGE_BASE) return 0;

    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (length == 0 || length >= sizeof(path)) return 0;

    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    return _stricmp(base, "Need For Speed MW-Reforged.exe") == 0 ||
           _stricmp(base, "speed.exe") == 0;
}

static int nfsmw_run_once(const char* via) {
    if (InterlockedCompareExchange(&g_nfsmw_ran, 1, 0) != 0) return 0;
    if (!nfsmw_validate_reforged_host()) {
#ifndef NFSMW_MULTIGEAR_DISABLE_LOG
        OutputDebugStringA(
            "[NFSMWMultiGear] unexpected main executable - refusing\n");
#endif
        return 1;
    }
#ifndef NFSMW_MULTIGEAR_DISABLE_LOG
    OutputDebugStringA("[NFSMWMultiGear] plugin starting via ");
    OutputDebugStringA(via);
    OutputDebugStringA("\n");
#else
    (void)via;
#endif
    return nfsmw_plugin_main_ptr ? nfsmw_plugin_main_ptr() : -1;
}

#ifndef NFSMW_ASI_INIT_DELAY_MS
#define NFSMW_ASI_INIT_DELAY_MS 2000
#endif

static DWORD WINAPI nfsmw_asi_worker(LPVOID context) {
    (void)context;
    Sleep(NFSMW_ASI_INIT_DELAY_MS);
    nfsmw_run_once("ASI");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE thread;
        DisableThreadLibraryCalls(instance);
        thread = CreateThread(NULL, 0, nfsmw_asi_worker, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

NFSMW_EXPORT int BepInExNativePlugin_Load(void) {
    return nfsmw_run_once("BepInEx");
}

NFSMW_EXPORT const char* BepInExNativePlugin_GUID =
    "nfsmw.multigear.reforged";
NFSMW_EXPORT const char* BepInExNativePlugin_Name = "NFSMW MultiGear";
NFSMW_EXPORT const char* BepInExNativePlugin_Version =
    NFSMW_MULTIGEAR_VERSION;

#ifdef __cplusplus
}
#endif
