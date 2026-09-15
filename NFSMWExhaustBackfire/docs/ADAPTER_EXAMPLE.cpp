/*
 * Illustrative adapter only. This file is intentionally not built until the
 * target speed.exe has verified marker/effect/audio symbols.
 */
#include "nfsmw_exhaust/PluginApi.hpp"

#include <cstdint>

namespace {

struct GameAdapter {
    // PVehicle enumeration, address profile, marker cache and audio backend.
};

std::uint32_t NFSW_EXHAUST_CALL getVehicleCount(void* user) {
    auto* adapter = static_cast<GameAdapter*>(user);
    (void)adapter;
    return 0;  // TODO: enumerate active PVehicle instances.
}

int NFSW_EXHAUST_CALL readVehicle(
    void* user, std::uint32_t index, NfswExhaustVehicleSnapshotC* output) {
    auto* adapter = static_cast<GameAdapter*>(user);
    (void)adapter;
    (void)index;
    (void)output;
    // TODO: IEngine RPM data, shift edge, and exact L/R world transforms.
    // A present marker needs x/y/z plus a normalized qx/qy/qz/qw quaternion.
    return 0;
}

int NFSW_EXHAUST_CALL spawnFlame(
    void* user, const NfswExhaustFlameRequestC* request) {
    auto* adapter = static_cast<GameAdapter*>(user);
    (void)adapter;
    (void)request;
    // TODO: call the verified exhaust particle function. Return 1 only when
    // the game accepted the request; audio is paired with accepted flames.
    // Returning 0 means no flame was created and allows a deadline-bound retry.
    return 0;
}

void NFSW_EXHAUST_CALL playAudio(
    void* user, const NfswExhaustAudioRequestC* request) {
    auto* adapter = static_cast<GameAdapter*>(user);
    (void)adapter;
    (void)request;
    // TODO: resolve request->assetId and submit positional audio.
}

void NFSW_EXHAUST_CALL setVanilla(void* user, std::uint32_t vehicleId,
                                  int enabled) {
    auto* adapter = static_cast<GameAdapter*>(user);
    (void)adapter;
    (void)vehicleId;
    (void)enabled;
    // TODO: suppress the stock EXHAUST backfire request only, never NOS.
}

void NFSW_EXHAUST_CALL logMessage(void* user, const char* message) {
    (void)user;
    (void)message;
}

}  // namespace

bool installExhaustController(GameAdapter* adapter) {
    const NfswExhaustCallbacks callbacks{
        NFSW_EXHAUST_API_VERSION, sizeof(NfswExhaustCallbacks),
        adapter, getVehicleCount, readVehicle, spawnFlame,
        playAudio, setVanilla, logMessage};
    return NFSW_Exhaust_RegisterCallbacks(&callbacks) != 0;
}

void uninstallExhaustController() {
    // Invoke on the game thread before GameAdapter or live vehicles teardown.
    NFSW_Exhaust_Shutdown();
}
