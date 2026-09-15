#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef NFSMW_RELEASE_SILENT
#define NFSMW_RELEASE_SILENT 0
#endif

namespace nfsmw_drift_asi::runtime_logging {

constexpr bool kEnabled = NFSMW_RELEASE_SILENT == 0;

inline void DebugOutput(const char* message) noexcept {
#if NFSMW_RELEASE_SILENT
    (void)message;
#else
    if (message != nullptr) {
        ::OutputDebugStringA(message);
    }
#endif
}

}  // namespace nfsmw_drift_asi::runtime_logging
