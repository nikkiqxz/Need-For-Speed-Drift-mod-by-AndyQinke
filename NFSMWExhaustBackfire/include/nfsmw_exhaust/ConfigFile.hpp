#pragma once

#include "Config.hpp"

#include <string>

namespace nfsmw_exhaust {

/* Small dependency-free INI reader for the plugin-side configuration. */
bool loadConfigFile(const char* path, ExhaustConfig* config,
                   std::string* error = nullptr);

}  // namespace nfsmw_exhaust
