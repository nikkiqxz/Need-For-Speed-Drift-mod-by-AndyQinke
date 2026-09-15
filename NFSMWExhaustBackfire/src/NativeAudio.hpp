#pragma once

namespace nfsmw_exhaust::native_audio {

struct Spatialization {
    float gain = 1.0f;
    float pan = 0.0f;
    float lowPass = 1.0f;
};

bool initialize(const char* modulePath) noexcept;
bool play(const char* assetId, const Spatialization& spatial) noexcept;
bool reverbReady() noexcept;

}  // namespace nfsmw_exhaust::native_audio
