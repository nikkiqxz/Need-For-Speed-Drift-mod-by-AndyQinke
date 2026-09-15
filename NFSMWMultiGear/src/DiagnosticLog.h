#pragma once

namespace diagnostic_log {

// Diagnostic builds open a new log for the game session. Formal builds define
// NFSMW_MULTIGEAR_DISABLE_LOG, making Open/Write successful no-ops.
bool Open(const char* path);

// Diagnostic builds flush each line immediately. Formal builds emit nothing.
void Write(const char* format, ...);

}  // namespace diagnostic_log
