#pragma once

#include <cstddef>
#include <cstdint>

namespace nfsmw_drift_asi::coordinator_diagnostic {

// Install the optional read-only dispatcher probe after the common PE and
// AOB gates have validated the exact speed.exe image.  The probe never calls
// the discovered virtual target directly and never writes game memory.
bool Install(std::uintptr_t validatedTarget);

}  // namespace nfsmw_drift_asi::coordinator_diagnostic
