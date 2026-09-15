#pragma once

#include <cmath>
#include <cstdint>

namespace nfsmw_drift_asi::drift_drive_torque {

constexpr std::uint32_t kFirstForwardGear = 2;
constexpr std::uint32_t kLastSupportedRawGear = 13;
constexpr float kDriftMultiplier = 3.0f;
constexpr float kMaximumOriginalTorque = 1000000.0f;

inline float Apply(float original, std::uint32_t rawGear,
                   bool currentPlayerDrift) noexcept {
    if (!currentPlayerDrift || rawGear < kFirstForwardGear ||
        rawGear > kLastSupportedRawGear || !std::isfinite(original) ||
        original <= 0.0f || original > kMaximumOriginalTorque) {
        return original;
    }
    return original * kDriftMultiplier;
}

}  // namespace nfsmw_drift_asi::drift_drive_torque
