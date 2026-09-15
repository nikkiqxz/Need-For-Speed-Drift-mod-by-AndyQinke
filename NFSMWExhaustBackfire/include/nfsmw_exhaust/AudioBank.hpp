#pragma once

#include "Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nfsmw_exhaust {

/*
 * The files are deliberately only identifiers at this stage. Replacing the
 * paths in the manifest does not require changing the timing core or the DLL
 * ABI. The engine adapter decides how an assetId is loaded and played.
 */
class AudioBank {
public:
    static constexpr std::size_t kClipCount = 12;

    AudioBank();

    const char* assetId(std::size_t clipIndex) const noexcept;
    AudioCue at(std::size_t clipIndex) const noexcept;
    bool loadManifest(const char* path, std::string* error = nullptr);

private:
    std::array<std::string, kClipCount> assetIds_{};
};

}  // namespace nfsmw_exhaust
