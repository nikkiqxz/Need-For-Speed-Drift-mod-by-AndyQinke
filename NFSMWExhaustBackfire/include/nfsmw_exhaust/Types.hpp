#pragma once

#include <cstdint>

namespace nfsmw_exhaust {

inline constexpr float kMinimumBackfireRpm = 4000.0f;

using VehicleId = std::uint32_t;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct ExhaustMarker {
    bool present = false;
    Vec3 worldPosition{};
    Quaternion worldOrientation{};
};

enum class ExhaustSide : std::uint8_t {
    Left = 0,
    Right = 1,
};

enum class FlamePattern : std::uint8_t {
    Standalone = 0,
    Simultaneous = 1,
    Sequential = 2,
};

enum class ShiftDirection : std::uint8_t {
    None = 0,
    Up = 1,
    Down = 2,
};

/*
 * This is the only vehicle state the timing core needs. The game adapter is
 * responsible for obtaining it from PVehicle/IEngine and for resolving the
 * two model markers.
 */
struct VehicleSnapshot {
    VehicleId id = 0;
    bool valid = false;

    /* Absolute revolutions per minute, not the game's possible kRPM value. */
    float rpm = 0.0f;
    float maxRpm = 0.0f;
    float redlineRpm = 0.0f;
    bool atRedline = false;
    bool atMaxRpm = false;

    bool shiftInProgress = false;
    bool shiftEvent = false;
    bool gearChanged = false;
    ShiftDirection shiftDirection = ShiftDirection::None;
    std::int32_t gear = 0;

    bool driverControlsValid = false;
    float gasInput = 0.0f;
    float brakeInput = 0.0f;
    float handBrakeInput = 0.0f;
    bool nitrousActive = false;

    ExhaustMarker leftExhaust{};
    ExhaustMarker rightExhaust{};

    bool hasBothExhaustMarkers() const noexcept {
        return leftExhaust.present && rightExhaust.present;
    }
};

struct AudioCue {
    std::uint8_t clipIndex = 0;
    const char* assetId = nullptr;
};

struct FlameRequest {
    VehicleId vehicleId = 0;
    std::uint64_t scheduledAtMs = 0;
    std::uint64_t emittedAtMs = 0;
    std::uint32_t effectVariant = 0;
    std::uint32_t sequenceId = 0;
    const char* effectId = nullptr;
    ExhaustSide side = ExhaustSide::Left;
    FlamePattern pattern = FlamePattern::Standalone;
    ExhaustMarker marker{};
};

struct AudioRequest {
    VehicleId vehicleId = 0;
    std::uint64_t scheduledAtMs = 0;
    std::uint64_t emittedAtMs = 0;
    ExhaustSide side = ExhaustSide::Left;
    ExhaustMarker marker{};
    AudioCue cue{};
};

}  // namespace nfsmw_exhaust
