#include "nfsmw_exhaust/PluginApi.hpp"

#include "nfsmw_exhaust/ConfigFile.hpp"
#include "nfsmw_exhaust/ExhaustController.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace nfsmw_exhaust;

class CallbackBridge final : public IGameBridge {
public:
    void setCallbacks(const NfswExhaustCallbacks& callbacks) {
        callbacks_ = callbacks;
    }

    void clearCallbacks() {
        callbacks_ = NfswExhaustCallbacks{};
    }

    std::size_t collectVehicles(VehicleSnapshot* output,
                                std::size_t capacity) override {
        if (output == nullptr || capacity == 0 ||
            callbacks_.getVehicleCount == nullptr ||
            callbacks_.readVehicle == nullptr) {
            return 0;
        }
        const std::uint32_t reported =
            callbacks_.getVehicleCount(callbacks_.user);
        const std::size_t count =
            std::min<std::size_t>(reported, capacity);
        for (std::size_t i = 0; i < count; ++i) {
            NfswExhaustVehicleSnapshotC raw{};
            if (!callbacks_.readVehicle(callbacks_.user,
                                        static_cast<std::uint32_t>(i), &raw)) {
                output[i] = VehicleSnapshot{};
                continue;
            }
            output[i] = convert(raw);
        }
        return count;
    }

    bool spawnExhaustFlame(const FlameRequest& request) override {
        if (callbacks_.spawnFlame == nullptr) return false;
        NfswExhaustFlameRequestC raw{};
        raw.vehicleId = request.vehicleId;
        raw.scheduledAtMs = request.scheduledAtMs;
        raw.emittedAtMs = request.emittedAtMs;
        raw.effectVariant = request.effectVariant;
        raw.sequenceId = request.sequenceId;
        raw.side = static_cast<std::uint8_t>(request.side);
        raw.pattern = static_cast<std::uint8_t>(request.pattern);
        raw.effectId = request.effectId;
        raw.marker = toC(request.marker);
        return callbacks_.spawnFlame(callbacks_.user, &raw) != 0;
    }

    void playBackfireAudio(const AudioRequest& request) override {
        if (callbacks_.playAudio == nullptr) return;
        NfswExhaustAudioRequestC raw{};
        raw.vehicleId = request.vehicleId;
        raw.scheduledAtMs = request.scheduledAtMs;
        raw.emittedAtMs = request.emittedAtMs;
        raw.marker = toC(request.marker);
        raw.clipIndex = request.cue.clipIndex;
        raw.side = static_cast<std::uint8_t>(request.side);
        raw.assetId = request.cue.assetId;
        callbacks_.playAudio(callbacks_.user, &raw);
    }

    void setVanillaExhaustEnabled(VehicleId vehicleId,
                                  bool enabled) override {
        if (callbacks_.setVanillaExhaustEnabled != nullptr) {
            callbacks_.setVanillaExhaustEnabled(callbacks_.user, vehicleId,
                                                enabled ? 1 : 0);
        }
    }

    void log(const char* message) override {
        if (callbacks_.log != nullptr && message != nullptr) {
            callbacks_.log(callbacks_.user, message);
        }
    }

private:
    static ExhaustMarker convert(const NfswExhaustMarkerC& raw) {
        ExhaustMarker marker{};
        marker.present = raw.present != 0;
        marker.worldPosition = Vec3{raw.x, raw.y, raw.z};
        if (raw.qx != 0.0f || raw.qy != 0.0f || raw.qz != 0.0f ||
            raw.qw != 0.0f) {
            marker.worldOrientation =
                Quaternion{raw.qx, raw.qy, raw.qz, raw.qw};
        }
        return marker;
    }

    static NfswExhaustMarkerC toC(const ExhaustMarker& marker) {
        return NfswExhaustMarkerC{marker.present ? 1 : 0,
                                  marker.worldPosition.x,
                                  marker.worldPosition.y,
                                  marker.worldPosition.z,
                                  marker.worldOrientation.x,
                                  marker.worldOrientation.y,
                                  marker.worldOrientation.z,
                                  marker.worldOrientation.w};
    }

    static VehicleSnapshot convert(const NfswExhaustVehicleSnapshotC& raw) {
        VehicleSnapshot snapshot{};
        snapshot.id = raw.id;
        snapshot.valid = raw.valid != 0;
        snapshot.rpm = raw.rpm;
        snapshot.maxRpm = raw.maxRpm;
        snapshot.redlineRpm = raw.redlineRpm;
        snapshot.atRedline = raw.atRedline != 0;
        snapshot.atMaxRpm = raw.atMaxRpm != 0;
        snapshot.shiftInProgress = raw.shiftInProgress != 0;
        snapshot.shiftEvent = raw.shiftEvent != 0;
        snapshot.gearChanged = raw.gearChanged != 0;
        snapshot.shiftDirection =
            static_cast<ShiftDirection>(std::clamp(raw.shiftDirection, 0, 2));
        snapshot.gear = raw.gear;
        snapshot.driverControlsValid = raw.driverControlsValid != 0;
        snapshot.gasInput = raw.gasInput;
        snapshot.brakeInput = raw.brakeInput;
        snapshot.handBrakeInput = raw.handBrakeInput;
        snapshot.nitrousActive = raw.nitrousActive != 0;
        snapshot.leftExhaust = convert(raw.leftExhaust);
        snapshot.rightExhaust = convert(raw.rightExhaust);
        return snapshot;
    }

    NfswExhaustCallbacks callbacks_{};
};

CallbackBridge g_bridge;
std::unique_ptr<ExhaustController> g_controller;
bool g_callbacksRegistered = false;
std::string g_audioManifestPath;

void ensureController() {
    if (!g_controller) g_controller = std::make_unique<ExhaustController>(g_bridge);
}

void logFailure(const char* message) noexcept {
    try {
        g_bridge.log(message);
    } catch (...) {
    }
}

}  // namespace

extern "C" NFSW_EXHAUST_EXPORT int NFSW_EXHAUST_CALL
NFSW_Exhaust_RegisterCallbacks(const NfswExhaustCallbacks* callbacks) {
    if (callbacks == nullptr ||
        callbacks->apiVersion != NFSW_EXHAUST_API_VERSION ||
        callbacks->structSize < sizeof(NfswExhaustCallbacks) ||
        callbacks->getVehicleCount == nullptr ||
        callbacks->readVehicle == nullptr || callbacks->spawnFlame == nullptr ||
        callbacks->playAudio == nullptr ||
        callbacks->setVanillaExhaustEnabled == nullptr) {
        return 0;
    }
    try {
        if (g_controller) g_controller->reset();
        g_bridge.setCallbacks(*callbacks);
        ensureController();
        g_callbacksRegistered = true;
        return 1;
    } catch (const std::exception& error) {
        logFailure(error.what());
    } catch (...) {
        logFailure("callback registration failed");
    }
    return 0;
}

extern "C" NFSW_EXHAUST_EXPORT int NFSW_EXHAUST_CALL
NFSW_Exhaust_LoadConfig(const char* path) {
    try {
        ensureController();
        ExhaustConfig candidate = g_controller->config();
        std::string error;
        if (!loadConfigFile(path, &candidate, &error)) {
            if (!error.empty()) logFailure(error.c_str());
            return 0;
        }
        auto replacement =
            std::make_unique<ExhaustController>(g_bridge, candidate);
        if (!g_audioManifestPath.empty()) {
            if (!replacement->loadAudioManifest(g_audioManifestPath.c_str(),
                                                &error)) {
                if (!error.empty()) logFailure(error.c_str());
                return 0;
            }
        }
        /* Commit only after both files load; old live state remains on error. */
        g_controller->reset();
        g_controller = std::move(replacement);
        return 1;
    } catch (const std::exception& error) {
        logFailure(error.what());
    } catch (...) {
        logFailure("configuration load failed");
    }
    return 0;
}

extern "C" NFSW_EXHAUST_EXPORT int NFSW_EXHAUST_CALL
NFSW_Exhaust_LoadAudioManifest(const char* path) {
    try {
        ensureController();
        std::string error;
        if (!g_controller->loadAudioManifest(path, &error)) {
            if (!error.empty()) logFailure(error.c_str());
            return 0;
        }
        g_audioManifestPath = path == nullptr ? "" : path;
        return 1;
    } catch (const std::exception& error) {
        logFailure(error.what());
    } catch (...) {
        logFailure("audio manifest load failed");
    }
    return 0;
}

extern "C" NFSW_EXHAUST_EXPORT void NFSW_EXHAUST_CALL
NFSW_Exhaust_OnFrame(std::uint64_t nowMs) {
    if (!g_callbacksRegistered) return;
    try {
        ensureController();
        g_controller->tick(nowMs);
    } catch (const std::exception& error) {
        logFailure(error.what());
    } catch (...) {
        logFailure("exhaust frame update failed");
    }
}

extern "C" NFSW_EXHAUST_EXPORT void NFSW_EXHAUST_CALL
NFSW_Exhaust_Reset() {
    try {
        if (g_controller) g_controller->reset();
    } catch (const std::exception& error) {
        logFailure(error.what());
    } catch (...) {
        logFailure("exhaust reset failed");
    }
}

extern "C" NFSW_EXHAUST_EXPORT void NFSW_EXHAUST_CALL
NFSW_Exhaust_Shutdown() {
    try {
        if (g_controller) g_controller->reset();
    } catch (const std::exception& error) {
        logFailure(error.what());
    } catch (...) {
        logFailure("exhaust shutdown failed while restoring vanilla state");
    }

    /* ExhaustController destruction is callback-free. Drop the controller
     * before releasing the callback table it references. */
    g_controller.reset();
    g_callbacksRegistered = false;
    g_audioManifestPath.clear();
    g_bridge.clearCallbacks();
}

extern "C" NFSW_EXHAUST_EXPORT const char* NFSW_EXHAUST_CALL
NFSW_Exhaust_Version() {
    return "1.1.14";
}
