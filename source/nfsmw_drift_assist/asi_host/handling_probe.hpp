#pragma once

#include <cstddef>
#include <cstdint>

namespace nfsmw_drift_asi::handling_probe {

struct PlayerFrame {
    std::uintptr_t pvehicle = 0;
    std::uintptr_t player = 0;
    std::uintptr_t rigidBody = 0;
    std::uintptr_t rigidBodyHolder = 0;
    std::uintptr_t rigidBodyInner = 0;
    std::uint64_t physicsSerial = 0;
    std::uint32_t physicsThread = 0;
    std::uint32_t groundedWheels = 0;
    bool driftActive = false;
    float speedMps = 0.0f;
    float steeringCommand = 0.0f;
    std::uint32_t collectionKey = 0;
    bool driverAssistActive = false;
};

struct RearWheelSteeringStats {
    bool visualHookArmed = false;
    bool physicsHookArmed = false;
    std::uint64_t visualWrites = 0;
    std::uint64_t visualRejected = 0;
    std::uint64_t physicsWrites = 0;
    std::uint64_t physicsRestores = 0;
    std::uint64_t physicsRestoreFailures = 0;
    std::uint32_t physicsInstallStage = 0;
    std::uint32_t solverEntryMatches = 0;
    std::uint32_t solverCallContextMatches = 0;
    std::uint32_t decodedSolverRva = 0;
    int physicsHookCreateStatus = -1;
    int physicsHookEnableStatus = -1;
};

// Each probe channel is optional. A failed validation or hook installation
// disables only that read-only channel and never affects the primary bridge.
// textStart/textSize must be the exact range accepted by the common profile
// gate; the probe deliberately does not parse the PE section table again.
bool Install(std::uintptr_t imageBase,
             std::size_t imageSize,
             std::uintptr_t textStart,
             std::size_t textSize);

// Sampling is deliberately sparse; non-sampled frames remain pass-through.
bool WantsFrame(std::uint32_t physicsThread);
void BeginFrame(const PlayerFrame& frame);
void InvalidateFrame();
void EndFrame();

// Publishes the already validated, player-only rear-steering command to the
// visual and tire-direction writers. Passing enabled=false invalidates it.
void PublishRearWheelSteering(std::uintptr_t pvehicle,
                              std::uintptr_t suspension,
                              std::uint32_t collectionKey,
                              std::uint64_t physicsSerial,
                              float angleRad,
                              bool enabled);
RearWheelSteeringStats GetRearWheelSteeringStats();

}  // namespace nfsmw_drift_asi::handling_probe
