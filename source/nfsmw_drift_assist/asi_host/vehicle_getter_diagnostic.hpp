#pragma once

#include <cstddef>
#include <cstdint>

namespace nfsmw_drift_asi::vehicle_getter_diagnostic {

// Configure the exact, already profile-validated speed.exe image.  No hook is
// installed until this gate succeeds.
bool Configure(std::uintptr_t imageBase, std::size_t imageSize);

// Discover suspension/transmission getter implementations from the PVehicle
// instance table, install read-only call-count detours, and emit a bounded
// frequency snapshot.  This function is intended to run after the vehicle
// tick's original function has returned.
void OnVehicleTick();

}  // namespace nfsmw_drift_asi::vehicle_getter_diagnostic
