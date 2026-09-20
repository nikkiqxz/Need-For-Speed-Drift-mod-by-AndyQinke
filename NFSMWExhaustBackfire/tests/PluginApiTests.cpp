#include "nfsmw_exhaust/PluginApi.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct AdapterState {
    NfswExhaustVehicleSnapshotC vehicle{};
    std::uint32_t vehicleCount = 1;
    std::size_t flames = 0;
    std::size_t audio = 0;
    std::size_t vanillaCalls = 0;
    bool lastVanillaEnabled = true;
    bool requestSideAndOrientationValid = true;
    std::uint8_t lastFlameSide = 0xFFu;
    std::uint8_t lastFlamePattern = 0xFFu;
    std::uint32_t lastSequenceId = 0;
    std::string lastAssetId;
};

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t NFSW_EXHAUST_CALL getVehicleCount(void* user) {
    return user == nullptr ? 0 : static_cast<AdapterState*>(user)->vehicleCount;
}

int NFSW_EXHAUST_CALL readVehicle(
    void* user, std::uint32_t index, NfswExhaustVehicleSnapshotC* output) {
    if (user == nullptr || output == nullptr || index != 0) return 0;
    *output = static_cast<AdapterState*>(user)->vehicle;
    return 1;
}

int NFSW_EXHAUST_CALL spawnFlame(
    void* user, const NfswExhaustFlameRequestC* request) {
    if (user == nullptr || request == nullptr || !request->marker.present) {
        return 0;
    }
    auto* state = static_cast<AdapterState*>(user);
    ++state->flames;
    state->lastFlameSide = request->side;
    state->lastFlamePattern = request->pattern;
    state->lastSequenceId = request->sequenceId;
    const bool left = request->side == NFSW_EXHAUST_SIDE_LEFT;
    const bool right = request->side == NFSW_EXHAUST_SIDE_RIGHT;
    const bool orientationMatches =
        (left && request->marker.x == -1.0f &&
         request->marker.qz == 0.0f && request->marker.qw == 1.0f) ||
        (right && request->marker.x == 1.0f &&
         request->marker.qz == 1.0f && request->marker.qw == 0.0f);
    state->requestSideAndOrientationValid &= orientationMatches;
    return 1;
}

void NFSW_EXHAUST_CALL playAudio(
    void* user, const NfswExhaustAudioRequestC* request) {
    if (user == nullptr || request == nullptr) return;
    auto* state = static_cast<AdapterState*>(user);
    ++state->audio;
    const bool left = request->side == NFSW_EXHAUST_SIDE_LEFT;
    const bool right = request->side == NFSW_EXHAUST_SIDE_RIGHT;
    const bool orientationMatches =
        (left && request->marker.x == -1.0f &&
         request->marker.qz == 0.0f && request->marker.qw == 1.0f) ||
        (right && request->marker.x == 1.0f &&
         request->marker.qz == 1.0f && request->marker.qw == 0.0f);
    state->requestSideAndOrientationValid &=
        request->side == state->lastFlameSide && orientationMatches;
    state->lastAssetId = request->assetId == nullptr ? "" : request->assetId;
}

void NFSW_EXHAUST_CALL setVanilla(void* user, std::uint32_t,
                                  int enabled) {
    if (user != nullptr) {
        auto* state = static_cast<AdapterState*>(user);
        ++state->vanillaCalls;
        state->lastVanillaEnabled = enabled != 0;
    }
}

void NFSW_EXHAUST_CALL logMessage(void*, const char*) {}

NfswExhaustVehicleSnapshotC makeVehicle() {
    NfswExhaustVehicleSnapshotC vehicle{};
    vehicle.id = 42;
    vehicle.valid = 1;
    vehicle.rpm = 9000.0f;
    vehicle.maxRpm = 9000.0f;
    vehicle.redlineRpm = 8800.0f;
    vehicle.atMaxRpm = 1;
    vehicle.gear = 1;
    vehicle.leftExhaust =
        NfswExhaustMarkerC{1, -1.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 0.0f, 0.0f};
    vehicle.rightExhaust =
        NfswExhaustMarkerC{1, 1.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 1.0f, 0.0f};
    return vehicle;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string root = argc > 1 ? argv[1] : ".";
        AdapterState state{};
        state.vehicle = makeVehicle();
        const NfswExhaustCallbacks callbacks{
            NFSW_EXHAUST_API_VERSION, sizeof(NfswExhaustCallbacks),
            &state, getVehicleCount, readVehicle, spawnFlame,
            playAudio, setVanilla, logMessage};

        NfswExhaustCallbacks invalidCallbacks = callbacks;
        invalidCallbacks.apiVersion = 0;
        expect(NFSW_Exhaust_RegisterCallbacks(&invalidCallbacks) == 0,
               "an incompatible callback ABI version must be rejected");
        invalidCallbacks = callbacks;
        invalidCallbacks.setVanillaExhaustEnabled = nullptr;
        expect(NFSW_Exhaust_RegisterCallbacks(&invalidCallbacks) == 0,
               "vanilla suppression callback must be required");
        expect(NFSW_Exhaust_RegisterCallbacks(&callbacks) == 1,
               "callback registration must succeed");
        expect(NFSW_Exhaust_LoadConfig(
                   (root + "/tests/data/Deterministic.ini").c_str()) == 1,
               "C API configuration load must succeed");
        expect(NFSW_Exhaust_LoadAudioManifest(
                   (root + "/config/BackfireAudio.ini").c_str()) == 1,
               "C API audio manifest load must succeed");
        expect(NFSW_Exhaust_LoadConfig(
                   (root + "/tests/data/Deterministic.ini").c_str()) == 1,
               "configuration reload must preserve a valid audio manifest");

        NFSW_Exhaust_OnFrame(0);
        state.vehicle.shiftEvent = 1;
        state.vehicle.gear = 2;
        NFSW_Exhaust_OnFrame(10);
        state.vehicle.shiftEvent = 0;
        for (std::uint64_t nowMs = 11; nowMs <= 510; ++nowMs) {
            NFSW_Exhaust_OnFrame(nowMs);
        }

        expect(state.flames >= 4 && state.flames <= 8,
               "C API must deliver the configured burst");
        expect(state.audio == state.flames,
               "C API must pair each accepted flame with one sound");
        expect(state.lastAssetId.find("audio/backfire/") == 0,
               "C API must expose the loaded audio asset IDs");
        expect(state.requestSideAndOrientationValid,
               "C API must preserve marker orientation and exhaust side");
        expect(!state.lastVanillaEnabled,
               "C API must request vanilla exhaust suppression");

        NFSW_Exhaust_Reset();
        expect(state.lastVanillaEnabled,
               "C API reset must restore vanilla exhaust");

        NFSW_Exhaust_OnFrame(520);
        expect(!state.lastVanillaEnabled,
               "frame processing after reset must suppress vanilla again");
        state.vehicleCount = 0;
        const std::size_t callsBeforeDisappearance = state.vanillaCalls;
        NFSW_Exhaust_OnFrame(521);
        expect(state.vanillaCalls == callsBeforeDisappearance,
               "a disappeared vehicle ID must not be passed back to adapter");

        state.vehicle = makeVehicle();
        state.vehicle.id = 43;
        state.vehicleCount = 1;
        NFSW_Exhaust_OnFrame(522);
        expect(!state.lastVanillaEnabled,
               "a new live vehicle must request vanilla suppression");
        NFSW_Exhaust_Shutdown();
        expect(state.lastVanillaEnabled,
               "shutdown must restore live tracked vehicles");
        const std::size_t callsAfterShutdown = state.vanillaCalls;
        NFSW_Exhaust_OnFrame(523);
        expect(state.vanillaCalls == callsAfterShutdown,
               "shutdown must stop frame processing and release callbacks");
        expect(std::string(NFSW_Exhaust_Version()) == "1.1.15",
               "C API version must match the release");
    } catch (const std::exception& error) {
        std::cerr << "NFSMWExhaustBackfire API tests failed: " << error.what()
                  << '\n';
        return 1;
    }
    std::cout << "NFSMWExhaustBackfire API tests passed\n";
    return 0;
}
