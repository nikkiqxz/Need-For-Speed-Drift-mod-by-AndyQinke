#pragma once

#include <cstddef>
#include <cstdint>

namespace nfsmw_exhaust::native_adapter {

inline constexpr std::uint64_t kNitrousActivityGraceMs = 180u;

inline bool isNitrousEmitterActive(std::size_t emitterCount,
                                   std::uint64_t lastUpdateMs,
                                   std::uint64_t nowMs) noexcept {
    return emitterCount != 0 && lastUpdateMs != 0 && nowMs >= lastUpdateMs &&
           nowMs - lastUpdateMs <= kNitrousActivityGraceMs;
}

}  // namespace nfsmw_exhaust::native_adapter
