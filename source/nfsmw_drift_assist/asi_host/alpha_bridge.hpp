#pragma once

#include <cstddef>
#include <cstdint>

namespace nfsmw_drift {
class DriftAssistController;
}

namespace nfsmw_drift_asi::vehicle_countersteer {
struct Registry;
}

namespace nfsmw_drift_asi::rear_wheel_steering {
struct Registry;
}

namespace nfsmw_drift_asi::alpha_bridge {

// Installs the smart-countersteer/manual-yaw bridge. All targets must already
// have passed the common PE/signature gate in asi_main.cpp.
bool Install(std::uintptr_t inputPollTarget,
             std::uintptr_t activeComponentsTarget,
             std::uintptr_t angularVelocitySetterTarget,
             std::uintptr_t validatedTextStart,
             std::size_t validatedTextSize,
             nfsmw_drift::DriftAssistController* controller,
             const vehicle_countersteer::Registry* vehicleMultipliers,
             const rear_wheel_steering::Registry* rearSteeringVehicles);

bool IsArmed();

}  // namespace nfsmw_drift_asi::alpha_bridge
