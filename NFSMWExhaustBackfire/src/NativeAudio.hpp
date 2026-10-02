#pragma once

#include <cstddef>
#include <cstdint>

namespace nfsmw_exhaust::native_audio {

struct Spatialization {
    float gain = 1.0f;
    float pan = 0.0f;
    float lowPass = 1.0f;
    float toneCharacter = 1.0f;
};

bool initialize(const char* modulePath, const char* audioManifestPath) noexcept;
void shutdown() noexcept;
void setPlaybackAllowed(bool allowed) noexcept;
bool play(const char* assetId, const Spatialization& spatial) noexcept;
bool reverbReady() noexcept;
void mixIntoGameBuffer(std::int16_t* samples, std::size_t frameCount,
                       std::uint32_t channels,
                       std::uint32_t sampleRate) noexcept;

namespace testing {
float clipEnvelope(std::size_t frame, std::size_t frameCount,
                   std::uint32_t sampleRate, bool mainClip) noexcept;
float tonePitchMultiplier(float toneCharacter) noexcept;
float toneGainCompensation(float toneCharacter) noexcept;
float toneWetMultiplier(float toneCharacter) noexcept;
float shapeToneSample(float source, float* lowBandState,
                      float toneCharacter, std::uint32_t sampleRate) noexcept;
}

}  // namespace nfsmw_exhaust::native_audio
