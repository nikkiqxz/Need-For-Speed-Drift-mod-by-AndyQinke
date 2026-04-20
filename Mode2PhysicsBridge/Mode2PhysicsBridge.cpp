#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cfloat>
#include <string>

namespace {

constexpr uintptr_t kAddrMinDriftBase = 0x008ABB7C;
constexpr uintptr_t kAddrMinSlipRad = 0x008ABB78;
constexpr uintptr_t kAddrFrictionScale = 0x00891050;
constexpr uintptr_t kAddrMaxSteeringAngle = 0x008AADE8;
constexpr uintptr_t kAddrSteeringScale = 0x008AB22C;

struct ParamConfig {
    const char* addrKey;
    const char* valueKey;
    uintptr_t address;
    float value;
    float original;
    bool hasOriginal;
};

struct Config {
    bool enabled;
    DWORD pollMs;
    bool restoreOnExit;
    std::string mode2ModuleName;
    uintptr_t mode2StateOffset;
    ParamConfig params[5];
    uintptr_t frictionScaleAddress;
    uintptr_t steerAngleAddress;
    int leftArrowKey;
    int rightArrowKey;
    int downArrowKey;
    float orbitDownComboHoldSeconds;
    float mode1OrbitAngles[31];
    float frictionScale1;
    float frictionScale2;
    float frictionScale3;
    float frictionScaleBrake;
    float mode2MaxAngles[21];
    float mode2MinSlipRadValue;
    float mode2MidSlipRadValue;
    float mode2FrictionScaleDrift3;
};

Config g_cfg{};
HANDLE g_thread = nullptr;
volatile bool g_running = true;
bool g_prevMode2Active = false;
bool g_frictionCaptured = false;
float g_frictionOriginal = 0.0f;
float g_comboHoldSeconds = 0.0f;

constexpr const char* kIniPath = ".\\NFSMWOrbitCamera.ini";
constexpr const char* kSection = "CAMERA";

uintptr_t ParseUint(const char* text, uintptr_t fallback) {
    if (!text || !text[0]) {
        return fallback;
    }
    char* end = nullptr;
    unsigned long long v = strtoull(text, &end, 0);
    if (end == text) {
        return fallback;
    }
    return static_cast<uintptr_t>(v);
}

uintptr_t ReadIniAddr(const char* key, uintptr_t fallback) {
    char buf[64] = {};
    GetPrivateProfileStringA(kSection, key, "", buf, static_cast<DWORD>(sizeof(buf)), kIniPath);
    return ParseUint(buf, fallback);
}

float ReadIniFloatText(const char* key, float fallback) {
    char buf[64] = {};
    GetPrivateProfileStringA(kSection, key, "", buf, static_cast<DWORD>(sizeof(buf)), kIniPath);
    if (!buf[0]) {
        return fallback;
    }
    char* end = nullptr;
    float v = strtof(buf, &end);
    if (end == buf) {
        return fallback;
    }
    return v;
}

bool SafeReadFloat(uintptr_t addr, float* outValue) {
    if (!addr || !outValue) {
        return false;
    }
    __try {
        *outValue = *reinterpret_cast<float*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadInt(uintptr_t addr, int* outValue) {
    if (!addr || !outValue) {
        return false;
    }
    __try {
        *outValue = *reinterpret_cast<int*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteFloat(uintptr_t addr, float value) {
    if (!addr) {
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    bool ok = true;
    __try {
        *reinterpret_cast<float*>(addr) = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DWORD temp = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), sizeof(float), oldProtect, &temp);
    return ok;
}

void LoadConfig() {
    g_cfg.enabled = GetPrivateProfileIntA(kSection, "Mode2PhysicsEnabled", 1, kIniPath) != 0;
    g_cfg.pollMs = static_cast<DWORD>(GetPrivateProfileIntA(kSection, "Mode2PhysicsPollMs", 10, kIniPath));
    if (g_cfg.pollMs < 1) {
        g_cfg.pollMs = 1;
    }
    g_cfg.restoreOnExit = GetPrivateProfileIntA(kSection, "Mode2PhysicsRestoreOnExit", 1, kIniPath) != 0;

    char moduleNameBuf[MAX_PATH] = {};
    GetPrivateProfileStringA(kSection, "Mode2PhysicsModuleName", "NFSMWOrbitCamera.asi", moduleNameBuf, MAX_PATH, kIniPath);
    g_cfg.mode2ModuleName = moduleNameBuf;
    g_cfg.mode2StateOffset = ReadIniAddr("Mode2PhysicsStateOffset", 0x3FF40);
    g_cfg.frictionScaleAddress = kAddrFrictionScale;
    g_cfg.steerAngleAddress = ReadIniAddr("FrontSteerSimableAngleAddress", 0x0089095C);
    g_cfg.leftArrowKey = GetPrivateProfileIntA(kSection, "LeftArrowKey", 37, kIniPath);
    g_cfg.rightArrowKey = GetPrivateProfileIntA(kSection, "RightArrowKey", 39, kIniPath);
    g_cfg.downArrowKey = GetPrivateProfileIntA(kSection, "DownArrowKey", 40, kIniPath);
    g_cfg.orbitDownComboHoldSeconds = ReadIniFloatText("FrontSteerOrbitDownComboHoldSeconds", 1.5f);
    g_cfg.frictionScale1 = ReadIniFloatText("frictionScale1", 1.0f);
    g_cfg.frictionScale2 = ReadIniFloatText("frictionScale2", 1.0f);
    g_cfg.frictionScale3 = ReadIniFloatText("frictionScale3", 1.0f);
    g_cfg.frictionScaleBrake = ReadIniFloatText("frictionScalebrake", 1.0f);
    g_cfg.mode2MinSlipRadValue = ReadIniFloatText("MinSlipRadValue", g_cfg.params[1].value);
    g_cfg.mode2MidSlipRadValue = ReadIniFloatText("MidSlipRadValue", g_cfg.mode2MinSlipRadValue);
    g_cfg.mode2FrictionScaleDrift3 = ReadIniFloatText("frictionScaledrift3", g_cfg.params[2].value);

    for (int i = 0; i < 31; ++i) {
        char key[64] = {};
        wsprintfA(key, "FrontSteerOrbitAngleConfig%d", i + 1);
        g_cfg.mode1OrbitAngles[i] = ReadIniFloatText(key, 0.0f);
    }

    g_cfg.mode2MaxAngles[0] = ReadIniFloatText("FrontSteerMaxAngleConfig1", 0.0f);
    g_cfg.mode2MaxAngles[1] = ReadIniFloatText("FrontSteerMaxAngleConfig1_1", 0.0f);
    g_cfg.mode2MaxAngles[2] = ReadIniFloatText("FrontSteerMaxAngleConfig1_2", 0.0f);
    g_cfg.mode2MaxAngles[3] = ReadIniFloatText("FrontSteerMaxAngleConfig1_3", 0.0f);
    g_cfg.mode2MaxAngles[4] = ReadIniFloatText("FrontSteerMaxAngleConfig1_4", 0.0f);
    g_cfg.mode2MaxAngles[5] = ReadIniFloatText("FrontSteerMaxAngleConfig1_5", 0.0f);
    g_cfg.mode2MaxAngles[6] = ReadIniFloatText("FrontSteerMaxAngleConfig1_6", 0.0f);
    g_cfg.mode2MaxAngles[7] = ReadIniFloatText("FrontSteerMaxAngleConfig1_7", 0.0f);
    g_cfg.mode2MaxAngles[8] = ReadIniFloatText("FrontSteerMaxAngleConfig1_8", 0.0f);
    g_cfg.mode2MaxAngles[9] = ReadIniFloatText("FrontSteerMaxAngleConfig1_9", 0.0f);
    g_cfg.mode2MaxAngles[10] = ReadIniFloatText("FrontSteerMaxAngleConfig2", 0.0f);
    g_cfg.mode2MaxAngles[11] = ReadIniFloatText("FrontSteerMaxAngleConfig2_1", 0.0f);
    g_cfg.mode2MaxAngles[12] = ReadIniFloatText("FrontSteerMaxAngleConfig2_2", 0.0f);
    g_cfg.mode2MaxAngles[13] = ReadIniFloatText("FrontSteerMaxAngleConfig2_3", 0.0f);
    g_cfg.mode2MaxAngles[14] = ReadIniFloatText("FrontSteerMaxAngleConfig2_4", 0.0f);
    g_cfg.mode2MaxAngles[15] = ReadIniFloatText("FrontSteerMaxAngleConfig2_5", 0.0f);
    g_cfg.mode2MaxAngles[16] = ReadIniFloatText("FrontSteerMaxAngleConfig2_6", 0.0f);
    g_cfg.mode2MaxAngles[17] = ReadIniFloatText("FrontSteerMaxAngleConfig2_7", 0.0f);
    g_cfg.mode2MaxAngles[18] = ReadIniFloatText("FrontSteerMaxAngleConfig2_8", 0.0f);
    g_cfg.mode2MaxAngles[19] = ReadIniFloatText("FrontSteerMaxAngleConfig2_9", 0.0f);
    g_cfg.mode2MaxAngles[20] = ReadIniFloatText("FrontSteerMaxAngleConfig3", 0.0f);

    g_cfg.params[0] = {"Mode2MinDriftBaseAddress", "Mode2MinDriftBaseValue", kAddrMinDriftBase, 0.0f, 0.0f, false};
    g_cfg.params[1] = {"Mode2MinSlipRadAddress", "Mode2MinSlipRadValue", kAddrMinSlipRad, 0.0f, 0.0f, false};
    g_cfg.params[2] = {"Mode2FrictionScaleAddress", "Mode2FrictionScaleValue", kAddrFrictionScale, 0.0f, 0.0f, false};
    g_cfg.params[3] = {"Mode2MaxSteeringAngleAddress", "Mode2MaxSteeringAngleValue", kAddrMaxSteeringAngle, 0.0f, 0.0f, false};
    g_cfg.params[4] = {"Mode2SteeringScaleAddress", "Mode2SteeringScaleValue", kAddrSteeringScale, 0.0f, 0.0f, false};

    for (auto& p : g_cfg.params) {
        p.value = ReadIniFloatText(p.valueKey, p.value);
    }
}

bool IsMode2Active() {
    HMODULE h = GetModuleHandleA(g_cfg.mode2ModuleName.c_str());
    if (!h) {
        return false;
    }
    uintptr_t stateAddr = reinterpret_cast<uintptr_t>(h) + g_cfg.mode2StateOffset;
    int state = 0;
    if (!SafeReadInt(stateAddr, &state)) {
        return false;
    }
    return state != 0;
}

void CaptureOriginalsIfNeeded() {
    for (auto& p : g_cfg.params) {
        if (!p.hasOriginal) {
            float cur = 0.0f;
            if (SafeReadFloat(p.address, &cur)) {
                p.original = cur;
                p.hasOriginal = true;
            }
        }
    }
}

void CaptureFrictionOriginalIfNeeded() {
    if (g_frictionCaptured) {
        return;
    }
    float cur = 0.0f;
    if (SafeReadFloat(g_cfg.frictionScaleAddress, &cur)) {
        g_frictionOriginal = cur;
        g_frictionCaptured = true;
    }
}

bool IsKeyDown(int vk) {
    if (vk <= 0) {
        return false;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

int FindClosestMode1ConfigIndex(float currentAngle) {
    int bestIdx = 0;
    float bestDiff = FLT_MAX;
    const float angleAbs = std::fabs(currentAngle);
    for (int i = 0; i < 31; ++i) {
        const float diff = std::fabs(std::fabs(g_cfg.mode1OrbitAngles[i]) - angleAbs);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIdx = i + 1;  // 1-based config index.
        }
    }
    return bestIdx;
}

int FindClosestMode2MaxConfigIndex(float currentAngle) {
    int bestIdx = 1;
    float bestDiff = FLT_MAX;
    const float angleAbs = std::fabs(currentAngle);
    for (int i = 0; i < 21; ++i) {
        const float diff = std::fabs(std::fabs(g_cfg.mode2MaxAngles[i]) - angleAbs);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIdx = i + 1;  // 1-based across [Config1 ... Config3], excludes Config4.
        }
    }
    return bestIdx;
}

void ApplyMode1FrictionByConfig() {
    CaptureFrictionOriginalIfNeeded();
    float steerAngle = 0.0f;
    if (!SafeReadFloat(g_cfg.steerAngleAddress, &steerAngle)) {
        return;
    }

    const bool leftDown = IsKeyDown(g_cfg.leftArrowKey);
    const bool rightDown = IsKeyDown(g_cfg.rightArrowKey);
    const bool downDown = IsKeyDown(g_cfg.downArrowKey);
    const bool comboPressed = downDown && (leftDown || rightDown);
    const float dt = static_cast<float>(g_cfg.pollMs) / 1000.0f;
    g_comboHoldSeconds = comboPressed ? (g_comboHoldSeconds + dt) : 0.0f;

    float targetFriction = g_frictionCaptured ? g_frictionOriginal : 1.0f;
    if (g_comboHoldSeconds >= g_cfg.orbitDownComboHoldSeconds) {
        targetFriction = g_cfg.frictionScaleBrake;
    } else {
        const int idx = FindClosestMode1ConfigIndex(steerAngle);
        if (idx >= 5 && idx <= 15) {
            targetFriction = g_cfg.frictionScale1;
        } else if (idx >= 16 && idx <= 25) {
            targetFriction = g_cfg.frictionScale2;
        } else if (idx >= 26 && idx <= 31) {
            targetFriction = g_cfg.frictionScale3;
        }
    }
    SafeWriteFloat(g_cfg.frictionScaleAddress, targetFriction);
}

void ApplyMode2Values() {
    for (const auto& p : g_cfg.params) {
        SafeWriteFloat(p.address, p.value);
    }
}

void ApplyMode2BucketAdjustments() {
    float steerAngle = 0.0f;
    if (!SafeReadFloat(g_cfg.steerAngleAddress, &steerAngle)) {
        return;
    }
    const int idx = FindClosestMode2MaxConfigIndex(steerAngle);

    if (idx >= 1 && idx <= 4) {
        // FrontSteerMaxAngleConfig1 ~ FrontSteerMaxAngleConfig1_3
        SafeWriteFloat(kAddrMinSlipRad, g_cfg.mode2MinSlipRadValue);
    } else if (idx >= 5 && idx <= 18) {
        // FrontSteerMaxAngleConfig1_4 ~ FrontSteerMaxAngleConfig2_7
        SafeWriteFloat(kAddrMinSlipRad, g_cfg.mode2MidSlipRadValue);
    } else if (idx >= 19 && idx <= 21) {
        // FrontSteerMaxAngleConfig2_8 ~ FrontSteerMaxAngleConfig3
        SafeWriteFloat(kAddrFrictionScale, g_cfg.mode2FrictionScaleDrift3);
    }
}

void RestoreOriginals() {
    if (!g_cfg.restoreOnExit) {
        return;
    }
    for (const auto& p : g_cfg.params) {
        if (p.hasOriginal) {
            SafeWriteFloat(p.address, p.original);
        }
    }
}

DWORD WINAPI WorkerThread(LPVOID) {
    LoadConfig();
    while (g_running) {
        if (g_cfg.enabled) {
            bool mode2Active = IsMode2Active();
            if (mode2Active) {
                if (!g_prevMode2Active) {
                    CaptureOriginalsIfNeeded();
                }
                ApplyMode2Values();
                ApplyMode2BucketAdjustments();
            } else {
                if (g_prevMode2Active) {
                    RestoreOriginals();
                    g_comboHoldSeconds = 0.0f;
                }
                ApplyMode1FrictionByConfig();
            }
            g_prevMode2Active = mode2Active;
        }
        Sleep(g_cfg.pollMs);
    }
    if (g_prevMode2Active) {
        RestoreOriginals();
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_running = true;
        g_thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running = false;
        if (g_thread) {
            WaitForSingleObject(g_thread, 500);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
    }
    return TRUE;
}
