#pragma once

#include <cstdint>

namespace nfsmw_drift_asi::phase_timing_diagnostic {

// Installs the two read-only phase hooks as one fail-closed unit. Both
// addresses must already have passed the PE profile, unique-signature, and
// .text checks in the host before this function is called.
bool Install(std::uintptr_t activeComponentsTarget,
             std::uintptr_t worldPhysicsTarget);

}  // namespace nfsmw_drift_asi::phase_timing_diagnostic
