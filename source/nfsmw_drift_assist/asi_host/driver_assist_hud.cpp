#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "driver_assist_hud.hpp"
#include "runtime_logging.hpp"

#include <MinHook.h>

#include <d3d9.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace nfsmw_drift_asi::driver_assist_hud {
namespace {

constexpr std::uintptr_t kD3d9DevicePointerAddress = 0x00982BDCu;
constexpr std::uintptr_t kNativeHudTableAddress = 0x0092FD94u;
constexpr std::uintptr_t kDetermineHudFeaturesAddress = 0x0057CA60u;
constexpr std::uintptr_t kFEngHudIsVisibleAddress = 0x005A0C20u;
constexpr std::uintptr_t kIsDragRaceAddress = 0x004D6150u;
constexpr std::size_t kD3d9PresentVtableSlot = 17;
constexpr std::size_t kD3d9EndSceneVtableSlot = 42;
constexpr std::uint32_t kVisibleBit = 1u << 0;
constexpr std::uint32_t kAbsBit = 1u << 1;
constexpr std::uint32_t kEscBit = 1u << 2;
constexpr std::uint32_t kEscRecoveryBit = 1u << 3;
constexpr std::uint32_t kTcsBit = 1u << 4;
constexpr std::uint32_t kLcBit = 1u << 5;
constexpr D3DCOLOR WithPanelOpacity(D3DCOLOR rgb) noexcept {
    return (static_cast<D3DCOLOR>(kPanelOpacityAlpha) << 24) |
           (rgb & 0x00FFFFFFu);
}

constexpr D3DCOLOR kOuterGray = WithPanelOpacity(0x00777777u);
constexpr D3DCOLOR kInactiveGray = WithPanelOpacity(0x00292929u);
constexpr D3DCOLOR kActiveWhite = WithPanelOpacity(0x00FFFFFFu);
constexpr D3DCOLOR kActiveRed = WithPanelOpacity(0x00E02020u);
constexpr D3DCOLOR kOpaqueWhite = 0xFFFFFFFFu;
constexpr D3DCOLOR kBlack = 0xFF000000u;
constexpr std::size_t kMaximumVertices = 2048;

using EndSceneFn = HRESULT (WINAPI*)(IDirect3DDevice9*);
using PresentFn = HRESULT (WINAPI*)(IDirect3DDevice9*, const RECT*,
                                    const RECT*, HWND, const RGNDATA*);
using DetermineHudFeaturesFn = std::uint64_t (__thiscall*)(void*, int);
using FEngHudIsVisibleFn = bool (__thiscall*)(void*);
using IsDragRaceFn = bool (__stdcall*)();

struct HudVertex {
    float x;
    float y;
    float z;
    float rhw;
    D3DCOLOR color;
};

struct VertexBatch {
    HudVertex vertices[kMaximumVertices]{};
    std::size_t count = 0;
};

std::atomic<std::uint32_t> g_stateBits{0};
std::atomic<std::uint32_t> g_absStart{0};
std::atomic<std::uint32_t> g_escStart{0};
std::atomic<std::uint32_t> g_tcsStart{0};
std::atomic<EndSceneFn> g_originalEndScene{nullptr};
std::atomic<PresentFn> g_originalPresent{nullptr};
std::atomic<DetermineHudFeaturesFn> g_originalDetermineHudFeatures{nullptr};
std::atomic<void*> g_nativeHudObject{nullptr};
std::atomic<bool> g_nativeFeatureSampleObserved{false};
std::atomic<bool> g_nativeFeatureEnabled{false};
std::atomic<bool> g_installStarted{false};
std::atomic<bool> g_presentHookInstalled{false};
std::atomic<bool> g_presentEnteredLogged{false};
std::atomic<bool> g_endSceneEnteredLogged{false};
std::atomic<bool> g_firstFrameLogged{false};
std::atomic<bool> g_drawFailureLogged{false};
std::atomic<bool> g_renderDisabled{false};
#if !NFSMW_RELEASE_SILENT
SRWLOCK g_logLock = SRWLOCK_INIT;
#endif

void Log(const char* message) noexcept {
#if NFSMW_RELEASE_SILENT
    (void)message;
#else
    if (message == nullptr) return;
    ::nfsmw_drift_asi::runtime_logging::DebugOutput("[nfsmw_drift_assist] ");
    ::nfsmw_drift_asi::runtime_logging::DebugOutput(message);
    ::nfsmw_drift_asi::runtime_logging::DebugOutput("\n");

    AcquireSRWLockExclusive(&g_logLock);
    HMODULE module = nullptr;
    char modulePath[MAX_PATH]{};
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&g_stateBits), &module) != FALSE &&
        GetModuleFileNameA(module, modulePath, MAX_PATH) != 0) {
        char* const slash = std::strrchr(modulePath, '\\');
        if (slash != nullptr) {
            *(slash + 1) = '\0';
            constexpr char kLogName[] =
                "Slippery_Drifting_FlashFish_by_AndyQinke.hud.log";
            const std::size_t prefixLength = std::strlen(modulePath);
            if (prefixLength + sizeof(kLogName) <= MAX_PATH) {
                std::memcpy(modulePath + prefixLength, kLogName,
                            sizeof(kLogName));
                HANDLE file = CreateFileA(
                    modulePath, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    char line[640]{};
                    const int count = std::snprintf(
                        line, sizeof(line),
                        "[%10lu] [nfsmw_drift_assist] %s\r\n",
                        static_cast<unsigned long>(GetTickCount()), message);
                    if (count > 0) {
                        DWORD written = 0;
                        WriteFile(file, line,
                                  static_cast<DWORD>(std::min<int>(
                                      count, static_cast<int>(sizeof(line) - 1))),
                                  &written, nullptr);
                    }
                    CloseHandle(file);
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_logLock);
#endif
}

bool IsReadable(const void* pointer, std::size_t size) noexcept {
    if (pointer == nullptr || size == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0 ||
        (info.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    const auto regionStart = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    return start >= regionStart && size <= info.RegionSize &&
           start - regionStart <= info.RegionSize - size;
}

bool IsExecutable(const void* pointer) noexcept {
    if (pointer == nullptr) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

IDirect3DDevice9* ReadGameDevice() noexcept {
#if defined(_MSC_VER)
    __try {
#endif
        auto* const slot = reinterpret_cast<IDirect3DDevice9* const*>(
            kD3d9DevicePointerAddress);
        if (!IsReadable(slot, sizeof(*slot))) return nullptr;
        return *slot;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#endif
}

std::uint64_t __fastcall DetermineHudFeaturesDetour(
    void* object, void*, int requestedFeatures) noexcept {
    const DetermineHudFeaturesFn original =
        g_originalDetermineHudFeatures.load(std::memory_order_acquire);
    if (original == nullptr) return 0;

    const std::uint64_t result = original(object, requestedFeatures);
    bool enabled = false;
#if defined(_MSC_VER)
    __try {
#endif
        const auto isDragRace = reinterpret_cast<IsDragRaceFn>(
            kIsDragRaceAddress);
        const auto isHudVisible = reinterpret_cast<FEngHudIsVisibleFn>(
            kFEngHudIsVisibleAddress);
        if (object != nullptr && !isDragRace() &&
            (result & (std::uint64_t{1} << 1)) != 0) {
            enabled = isHudVisible(object);
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        enabled = false;
    }
#endif
    g_nativeHudObject.store(object, std::memory_order_release);
    g_nativeFeatureEnabled.store(enabled, std::memory_order_release);
    g_nativeFeatureSampleObserved.store(true, std::memory_order_release);
    return result;
}

bool IsNativeHudVisible() noexcept {
    bool hudTableVisible = false;
    bool objectVisible = false;
#if defined(_MSC_VER)
    __try {
#endif
        const auto* const hudTable = reinterpret_cast<const std::uint8_t*>(
            kNativeHudTableAddress);
        hudTableVisible = IsReadable(hudTable, sizeof(*hudTable)) &&
                          *hudTable != 0;
        void* const object =
            g_nativeHudObject.load(std::memory_order_acquire);
        if (object != nullptr && IsReadable(object, sizeof(void*))) {
            const auto isHudVisible = reinterpret_cast<FEngHudIsVisibleFn>(
                kFEngHudIsVisibleAddress);
            objectVisible = isHudVisible(object);
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hudTableVisible = false;
        objectVisible = false;
    }
#endif
    return ShouldRenderWithNativeSample(
        true, hudTableVisible,
        g_nativeFeatureSampleObserved.load(std::memory_order_acquire),
        g_nativeFeatureEnabled.load(std::memory_order_acquire),
        objectVisible);
}

void InstallNativeHudTracking() noexcept {
    void* originalRaw = nullptr;
    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<void*>(kDetermineHudFeaturesAddress),
        reinterpret_cast<void*>(&DetermineHudFeaturesDetour), &originalRaw);
    MH_STATUS enable = MH_ERROR_NOT_CREATED;
    if (create == MH_OK && originalRaw != nullptr) {
        g_originalDetermineHudFeatures.store(
            reinterpret_cast<DetermineHudFeaturesFn>(originalRaw),
            std::memory_order_release);
        enable = MH_EnableHook(
            reinterpret_cast<void*>(kDetermineHudFeaturesAddress));
    }
    const bool installed = create == MH_OK && originalRaw != nullptr &&
        (enable == MH_OK || enable == MH_ERROR_ENABLED);
    if (!installed) {
        g_originalDetermineHudFeatures.store(nullptr,
                                              std::memory_order_release);
        char message[256]{};
        std::snprintf(message, sizeof(message),
                      "driver-assist native HUD lifecycle hook failed create=%s enable=%s; HUD-table fallback remains active",
                      MH_StatusToString(create), MH_StatusToString(enable));
        Log(message);
        return;
    }
    Log("driver-assist native HUD lifecycle synchronized with CustomHud logic");
}

void AddRect(VertexBatch* batch, const Rect& rect, D3DCOLOR color) noexcept {
    if (batch == nullptr || rect.right <= rect.left ||
        rect.bottom <= rect.top || batch->count + 6 > kMaximumVertices) {
        return;
    }
    const float left = static_cast<float>(rect.left) - 0.5f;
    const float top = static_cast<float>(rect.top) - 0.5f;
    const float right = static_cast<float>(rect.right) - 0.5f;
    const float bottom = static_cast<float>(rect.bottom) - 0.5f;
    const HudVertex topLeft{left, top, 0.0f, 1.0f, color};
    const HudVertex topRight{right, top, 0.0f, 1.0f, color};
    const HudVertex bottomLeft{left, bottom, 0.0f, 1.0f, color};
    const HudVertex bottomRight{right, bottom, 0.0f, 1.0f, color};
    batch->vertices[batch->count++] = topLeft;
    batch->vertices[batch->count++] = topRight;
    batch->vertices[batch->count++] = bottomRight;
    batch->vertices[batch->count++] = topLeft;
    batch->vertices[batch->count++] = bottomRight;
    batch->vertices[batch->count++] = bottomLeft;
}

void AddPanelChrome(VertexBatch* batch, const Layout& layout,
                    D3DCOLOR color) noexcept {
    const Rect& outer = layout.outer;
    const Rect& first = layout.rows[0];
    const Rect& second = layout.rows[1];
    const Rect& third = layout.rows[2];
    AddRect(batch, {outer.left, outer.top, outer.right, first.top}, color);
    AddRect(batch, {outer.left, third.bottom, outer.right, outer.bottom}, color);
    AddRect(batch, {outer.left, first.top, first.left, third.bottom}, color);
    AddRect(batch, {first.right, first.top, outer.right, third.bottom}, color);
    AddRect(batch, {first.left, first.bottom, first.right, second.top}, color);
    AddRect(batch, {second.left, second.bottom, second.right, third.top}, color);
}

std::uint8_t GlyphRow(char character, int row) noexcept {
    static constexpr std::uint8_t kA[7] = {
        0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static constexpr std::uint8_t kB[7] = {
        0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    static constexpr std::uint8_t kC[7] = {
        0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
    static constexpr std::uint8_t kE[7] = {
        0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static constexpr std::uint8_t kS[7] = {
        0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static constexpr std::uint8_t kT[7] = {
        0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static constexpr std::uint8_t kL[7] = {
        0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    if (row < 0 || row >= 7) return 0;
    switch (character) {
        case 'A': return kA[row];
        case 'B': return kB[row];
        case 'C': return kC[row];
        case 'E': return kE[row];
        case 'S': return kS[row];
        case 'T': return kT[row];
        case 'L': return kL[row];
        default: return 0;
    }
}

void AddLabel(VertexBatch* batch, const Rect& rowRect,
              const char label[4], int pixel, D3DCOLOR color) noexcept {
    const int glyphWidth = 5 * pixel;
    const int gap = 2 * pixel;
    int characterCount = 0;
    while (characterCount < 3 && label[characterCount] != '\0' &&
           label[characterCount] != ' ') {
        ++characterCount;
    }
    const int totalWidth = characterCount > 0
        ? characterCount * glyphWidth + (characterCount - 1) * gap
        : 0;
    const int totalHeight = 7 * pixel;
    const long startX = rowRect.left +
        std::max<long>(0, (rowRect.right - rowRect.left - totalWidth) / 2);
    const long startY = rowRect.top +
        std::max<long>(0, (rowRect.bottom - rowRect.top - totalHeight) / 2);
    for (int characterIndex = 0; characterIndex < characterCount;
         ++characterIndex) {
        for (int glyphRow = 0; glyphRow < 7; ++glyphRow) {
            const std::uint8_t bits = GlyphRow(label[characterIndex], glyphRow);
            for (int column = 0; column < 5; ++column) {
                if ((bits & (1u << (4 - column))) == 0) continue;
                const long x = startX +
                    characterIndex * (glyphWidth + gap) + column * pixel;
                const long y = startY + glyphRow * pixel;
                AddRect(batch, {x, y, x + pixel, y + pixel}, color);
            }
        }
    }
}

D3DCOLOR WorkingBackground(std::uint32_t now, std::uint32_t start,
                           std::uint32_t halfPeriod) noexcept {
    const std::uint32_t age = now - start;
    if (age < kSolidActiveMilliseconds) return kActiveWhite;
    const std::uint32_t phase =
        (age - kSolidActiveMilliseconds) / std::max(1u, halfPeriod);
    return (phase & 1u) == 0 ? kActiveWhite : kActiveRed;
}

void AddIndicator(VertexBatch* batch, const Rect& rect, const char label[4],
                   bool working, std::uint32_t start, std::uint32_t now,
                   std::uint32_t halfPeriod, bool blinking,
                   int pixel) noexcept {
    const D3DCOLOR background = working
        ? (blinking ? WorkingBackground(now, start, halfPeriod)
                    : kActiveWhite)
        : kInactiveGray;
    AddRect(batch, rect, background);
    AddLabel(batch, rect, label, pixel, working ? kBlack : kOpaqueWhite);
}

bool DrawBatch(IDirect3DDevice9* device, const VertexBatch& batch) noexcept {
    if (device == nullptr || batch.count == 0 || batch.count % 3 != 0) {
        return false;
    }
    IDirect3DStateBlock9* savedState = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &savedState)) ||
        savedState == nullptr) {
        return false;
    }
    device->SetTexture(0, nullptr);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,
                           D3DCOLORWRITEENABLE_RED |
                           D3DCOLORWRITEENABLE_GREEN |
                           D3DCOLORWRITEENABLE_BLUE |
                           D3DCOLORWRITEENABLE_ALPHA);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    const HRESULT drawResult = device->DrawPrimitiveUP(
        D3DPT_TRIANGLELIST, static_cast<UINT>(batch.count / 3),
        batch.vertices, sizeof(HudVertex));
    const HRESULT restoreResult = savedState->Apply();
    savedState->Release();
    return SUCCEEDED(drawResult) && SUCCEEDED(restoreResult);
}

void Render(IDirect3DDevice9* device, const char* renderPath) noexcept {
    if (device == nullptr ||
        g_renderDisabled.load(std::memory_order_acquire)) return;
    const std::uint32_t bits = g_stateBits.load(std::memory_order_acquire);
    if (!ShouldRender((bits & kVisibleBit) != 0,
                      IsNativeHudVisible())) return;

#if defined(_MSC_VER)
    __try {
#endif
        D3DVIEWPORT9 viewport{};
        if (FAILED(device->GetViewport(&viewport)) || viewport.Width < 640 ||
            viewport.Height < 480) return;
        const Layout layout = MakeLayout(viewport.Width, viewport.Height);
        VertexBatch batch{};
        AddPanelChrome(&batch, layout, kOuterGray);
        const std::uint32_t now = GetTickCount();
        AddIndicator(&batch, layout.rows[0], "ABS", (bits & kAbsBit) != 0,
                     g_absStart.load(std::memory_order_acquire), now,
                     kNormalBlinkHalfPeriodMilliseconds, true, layout.pixel);
        AddIndicator(&batch, layout.rows[1], "ESC", (bits & kEscBit) != 0,
                     g_escStart.load(std::memory_order_acquire), now,
                     kNormalBlinkHalfPeriodMilliseconds,
                     (bits & kEscRecoveryBit) != 0, layout.pixel);
        const bool lcWorking = (bits & kLcBit) != 0;
        AddIndicator(&batch, layout.rows[2], lcWorking ? "LC " : "TCS",
                     lcWorking || (bits & kTcsBit) != 0,
                     g_tcsStart.load(std::memory_order_acquire), now,
                     (bits & kEscRecoveryBit) != 0
                          ? kRecoveryTcsBlinkHalfPeriodMilliseconds
                          : kNormalBlinkHalfPeriodMilliseconds,
                     true, layout.pixel);
        if (!DrawBatch(device, batch)) {
            if (!g_drawFailureLogged.exchange(true,
                                              std::memory_order_acq_rel)) {
                Log("driver-assist HUD draw failed; vehicle control remains active");
            }
            return;
        }
        if (!g_firstFrameLogged.exchange(true, std::memory_order_acq_rel)) {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                          "driver-assist HUD first frame rendered path=%s viewport=%ux%u vertices=%u",
                          renderPath != nullptr ? renderPath : "unknown",
                          static_cast<unsigned>(viewport.Width),
                          static_cast<unsigned>(viewport.Height),
                          static_cast<unsigned>(batch.count));
            Log(message);
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_renderDisabled.store(true, std::memory_order_release);
        Log("driver-assist HUD disabled after D3D9 exception; vehicle control remains active");
    }
#endif
}

HRESULT WINAPI EndSceneDetour(IDirect3DDevice9* device) noexcept {
    if (!g_endSceneEnteredLogged.exchange(true, std::memory_order_acq_rel)) {
        char message[160]{};
        std::snprintf(message, sizeof(message),
                      "driver-assist HUD EndScene callback entered state_bits=0x%02X",
                      static_cast<unsigned>(
                          g_stateBits.load(std::memory_order_acquire)));
        Log(message);
    }
    if (!g_presentHookInstalled.load(std::memory_order_acquire)) {
        Render(device, "EndScene-fallback");
    }
    const EndSceneFn original =
        g_originalEndScene.load(std::memory_order_acquire);
    return original != nullptr ? original(device) : D3D_OK;
}

HRESULT WINAPI PresentDetour(IDirect3DDevice9* device,
                             const RECT* sourceRect,
                             const RECT* destinationRect,
                             HWND destinationWindow,
                             const RGNDATA* dirtyRegion) noexcept {
    if (!g_presentEnteredLogged.exchange(true, std::memory_order_acq_rel)) {
        char message[160]{};
        std::snprintf(message, sizeof(message),
                      "driver-assist HUD Present callback entered state_bits=0x%02X",
                      static_cast<unsigned>(
                          g_stateBits.load(std::memory_order_acquire)));
        Log(message);
    }
    bool beganScene = false;
#if defined(_MSC_VER)
    __try {
#endif
        if (device != nullptr) {
            const HRESULT beginResult = device->BeginScene();
            beganScene = SUCCEEDED(beginResult);
            Render(device, beganScene ? "Present" : "Present-existing-scene");
            if (beganScene) {
                device->EndScene();
            }
        }
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_renderDisabled.store(true, std::memory_order_release);
        Log("driver-assist HUD disabled after Present exception; vehicle control remains active");
    }
#endif
    const PresentFn original =
        g_originalPresent.load(std::memory_order_acquire);
    return original != nullptr
        ? original(device, sourceRect, destinationRect,
                   destinationWindow, dirtyRegion)
        : D3D_OK;
}

DWORD WINAPI InstallWorker(void*) noexcept {
    InstallNativeHudTracking();
    bool waitingLogged = false;
    for (;;) {
        IDirect3DDevice9* const device = ReadGameDevice();
        if (device == nullptr || !IsReadable(device, sizeof(void*))) {
            if (!waitingLogged) {
                Log("driver-assist HUD waiting for D3D9 device");
                waitingLogged = true;
            }
            Sleep(50);
            continue;
        }
        void** vtable = nullptr;
#if defined(_MSC_VER)
        __try {
#endif
            vtable = *reinterpret_cast<void***>(device);
#if defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            vtable = nullptr;
        }
#endif
        if (!IsReadable(vtable,
                        (kD3d9EndSceneVtableSlot + 1) * sizeof(void*)) ||
            !IsExecutable(vtable[kD3d9EndSceneVtableSlot])) {
            Sleep(50);
            continue;
        }

        void* const presentTarget = vtable[kD3d9PresentVtableSlot];
        void* const endSceneTarget = vtable[kD3d9EndSceneVtableSlot];
        void* presentOriginalRaw = nullptr;
        const MH_STATUS presentCreate = MH_CreateHook(
            presentTarget, reinterpret_cast<void*>(&PresentDetour),
            &presentOriginalRaw);
        MH_STATUS presentEnable = MH_ERROR_NOT_CREATED;
        bool presentVerified = false;
        if (presentCreate == MH_OK && presentOriginalRaw != nullptr) {
            g_originalPresent.store(
                reinterpret_cast<PresentFn>(presentOriginalRaw),
                std::memory_order_release);
            presentEnable = MH_EnableHook(presentTarget);
            presentVerified = presentEnable == MH_OK ||
                              presentEnable == MH_ERROR_ENABLED;
        }
        if (!presentVerified) {
            if (presentCreate == MH_OK) {
                MH_RemoveHook(presentTarget);
            }
            g_originalPresent.store(nullptr, std::memory_order_release);
        }
        g_presentHookInstalled.store(presentVerified,
                                     std::memory_order_release);

        void* endSceneOriginalRaw = nullptr;
        const MH_STATUS endSceneCreate = MH_CreateHook(
            endSceneTarget, reinterpret_cast<void*>(&EndSceneDetour),
            &endSceneOriginalRaw);
        MH_STATUS endSceneEnable = MH_ERROR_NOT_CREATED;
        bool endSceneVerified = false;
        if (endSceneCreate == MH_OK && endSceneOriginalRaw != nullptr) {
            g_originalEndScene.store(
                reinterpret_cast<EndSceneFn>(endSceneOriginalRaw),
                std::memory_order_release);
            endSceneEnable = MH_EnableHook(endSceneTarget);
            endSceneVerified = endSceneEnable == MH_OK ||
                               endSceneEnable == MH_ERROR_ENABLED;
        }
        if (!endSceneVerified) {
            if (endSceneCreate == MH_OK) {
                MH_RemoveHook(endSceneTarget);
            }
            g_originalEndScene.store(nullptr, std::memory_order_release);
        }
        if (!presentVerified && !endSceneVerified) {
            char failure[320]{};
            std::snprintf(
                failure, sizeof(failure),
                "driver-assist HUD function hooks failed Present=%s/%s EndScene=%s/%s; retrying",
                MH_StatusToString(presentCreate),
                MH_StatusToString(presentEnable),
                MH_StatusToString(endSceneCreate),
                MH_StatusToString(endSceneEnable));
            Log(failure);
            Sleep(500);
            continue;
        }
        char message[384]{};
        std::snprintf(message, sizeof(message),
                      "driver-assist HUD function hooks installed Present=%d(%s/%s) EndScene=%d(%s/%s) device=%p present_target=%p endscene_target=%p",
                      presentVerified ? 1 : 0,
                      MH_StatusToString(presentCreate),
                      MH_StatusToString(presentEnable),
                      endSceneVerified ? 1 : 0,
                      MH_StatusToString(endSceneCreate),
                      MH_StatusToString(endSceneEnable),
                      static_cast<void*>(device), presentTarget,
                      endSceneTarget);
        Log(message);
        return 0;
    }
}

}  // namespace

void Publish(bool visible, bool absWorking, bool escWorking,
             bool escRecoveryWorking, bool tcsWorking,
             bool lcWorking,
             std::uint32_t nowMilliseconds) noexcept {
    std::uint32_t next = visible ? kVisibleBit : 0u;
    if (visible && absWorking) next |= kAbsBit;
    if (visible && escWorking) next |= kEscBit;
    if (visible && escRecoveryWorking) next |= kEscRecoveryBit;
    if (visible && tcsWorking) next |= kTcsBit;
    if (visible && lcWorking) next |= kLcBit;
    const std::uint32_t previous =
        g_stateBits.load(std::memory_order_acquire);
    if ((next & kAbsBit) != 0 && (previous & kAbsBit) == 0) {
        g_absStart.store(nowMilliseconds, std::memory_order_release);
    }
    if ((next & kEscBit) != 0 && (previous & kEscBit) == 0) {
        g_escStart.store(nowMilliseconds, std::memory_order_release);
    }
    if ((next & (kTcsBit | kLcBit)) != 0 &&
        (previous & (kTcsBit | kLcBit)) == 0) {
        g_tcsStart.store(nowMilliseconds, std::memory_order_release);
    }
    g_stateBits.store(next, std::memory_order_release);
}

void Install() noexcept {
    g_renderDisabled.store(false, std::memory_order_release);
    if (g_installStarted.exchange(true, std::memory_order_acq_rel)) return;
    Log("driver-assist HUD 1.1.0 CustomHud-synchronized native-visibility DXVK function-hook installer starting");
    HANDLE thread = CreateThread(nullptr, 0, &InstallWorker, nullptr, 0, nullptr);
    if (thread == nullptr) {
        g_installStarted.store(false, std::memory_order_release);
        Log("driver-assist HUD worker creation failed; vehicle control remains active");
        return;
    }
    CloseHandle(thread);
    Log("driver-assist ABS/ESC/TCS/LC HUD installation armed");
}

}  // namespace nfsmw_drift_asi::driver_assist_hud
