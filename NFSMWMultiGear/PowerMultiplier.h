#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gear_power {

// GEAR_EFFICIENCY is the drivetrain term that scales delivered power without
// changing the gear ratio used for the engine-RPM/road-speed relationship.
inline bool ScaleEfficiency(float baseEfficiency, float powerMultiplier,
                            float* result) noexcept {
    if (result == nullptr || !std::isfinite(baseEfficiency) ||
        baseEfficiency <= 0.0f || !std::isfinite(powerMultiplier) ||
        powerMultiplier <= 0.0f) {
        return false;
    }
    const float scaled = baseEfficiency * powerMultiplier;
    if (!std::isfinite(scaled) || scaled <= 0.0f) return false;
    *result = scaled;
    return true;
}

// Build the two deliberately separate efficiency views used by Phase 4.
// publishedEfficiencies is safe for native tables and generic getters;
// poweredEfficiencies is diagnostic/runtime-only and must never be written
// back into the transmission layout.
inline bool BuildEfficiencyViews(float baseEfficiency,
                                 const float* powerMultipliers,
                                 std::size_t count,
                                 float* publishedEfficiencies,
                                 float* poweredEfficiencies) noexcept {
    if (powerMultipliers == nullptr || publishedEfficiencies == nullptr ||
        poweredEfficiencies == nullptr || count == 0 ||
        !std::isfinite(baseEfficiency) || baseEfficiency <= 0.0f) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        float powered = 0.0f;
        if (!ScaleEfficiency(baseEfficiency, powerMultipliers[i], &powered)) {
            return false;
        }
        publishedEfficiencies[i] = baseEfficiency;
        poweredEfficiencies[i] = powered;
    }
    return true;
}

// Static attribute views must stay neutral because the game derives shared
// transmission data by scanning the complete efficiency table. Apply a
// configured multiplier only to a live query for the actual current gear.
inline bool SelectRuntimeEfficiency(float baseEfficiency,
                                    float powerMultiplier,
                                    std::uint32_t queriedGear,
                                    std::int32_t currentGear,
                                    std::uint32_t firstPoweredGear,
                                    std::uint32_t lastPoweredGear,
                                    bool verifiedRuntimePowerCall,
                                    float* result) noexcept {
    if (result == nullptr || !std::isfinite(baseEfficiency) ||
        baseEfficiency <= 0.0f || !std::isfinite(powerMultiplier) ||
        powerMultiplier <= 0.0f || firstPoweredGear > lastPoweredGear) {
        return false;
    }

    *result = baseEfficiency;
    if (!verifiedRuntimePowerCall || currentGear < 0 ||
        queriedGear < firstPoweredGear ||
        queriedGear > lastPoweredGear ||
        queriedGear != static_cast<std::uint32_t>(currentGear)) {
        return true;
    }
    return ScaleEfficiency(baseEfficiency, powerMultiplier, result);
}

// 0x006A1480 returns a value proportional to 1 / (ratio * efficiency).
// Replace both denominator terms when using its native raw-9 result as a
// sidecar proxy.
inline bool ScaleRatioEfficiencyResult(float baseResult, float baseRatio,
                                       float baseEfficiency,
                                       float requestedRatio,
                                       float requestedEfficiency,
                                       float* result) noexcept {
    if (result == nullptr || !std::isfinite(baseResult) ||
        !std::isfinite(baseRatio) || baseRatio <= 0.0f ||
        !std::isfinite(baseEfficiency) || baseEfficiency <= 0.0f ||
        !std::isfinite(requestedRatio) || requestedRatio <= 0.0f ||
        !std::isfinite(requestedEfficiency) ||
        requestedEfficiency <= 0.0f) {
        return false;
    }
    const float ratioScaled = baseResult * (baseRatio / requestedRatio);
    const float scaled =
        ratioScaled * (baseEfficiency / requestedEfficiency);
    if (!std::isfinite(scaled)) return false;
    *result = scaled;
    return true;
}

}  // namespace gear_power
