#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

int __cdecl NFSW_Exhaust_NativeMain(HMODULE module);

static HMODULE g_module = NULL;
static volatile LONG g_started = 0;

static int run_once(void) {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return 0;
    return NFSW_Exhaust_NativeMain(g_module);
}

static DWORD WINAPI asi_worker(LPVOID context) {
    (void)context;
    Sleep(2000);
    run_once();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE thread;
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        thread = CreateThread(NULL, 0, asi_worker, NULL, 0, NULL);
        if (thread != NULL) CloseHandle(thread);
    }
    return TRUE;
}

__declspec(dllexport) int __cdecl BepInExNativePlugin_Load(void) {
    return run_once();
}

__declspec(dllexport) const char* BepInExNativePlugin_GUID =
    "nfsmw.exhaust.backfire.reforged";
__declspec(dllexport) const char* BepInExNativePlugin_Name =
    "NFSMW Exhaust Backfire";
__declspec(dllexport) const char* BepInExNativePlugin_Version = "1.1.15";

#ifdef __cplusplus
}
#endif
