#include "NativeAudio.hpp"
#include "NativeLog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    nfsmw_exhaust::native_log::open("NativeAudioTests.log");
    if (argc != 2) {
        std::fprintf(stderr, "source directory argument is required\n");
        return 1;
    }
    constexpr std::size_t kOneSecondFrames = 48000;
    const float mainQuarter = nfsmw_exhaust::native_audio::testing::clipEnvelope(
        12000, kOneSecondFrames, 48000, true);
    const float mainHalf = nfsmw_exhaust::native_audio::testing::clipEnvelope(
        24000, kOneSecondFrames, 48000, true);
    const float mainThreeQuarter =
        nfsmw_exhaust::native_audio::testing::clipEnvelope(
            36000, kOneSecondFrames, 48000, true);
    const float mainEnd = nfsmw_exhaust::native_audio::testing::clipEnvelope(
        47999, kOneSecondFrames, 48000, true);
    const float backgroundHalf =
        nfsmw_exhaust::native_audio::testing::clipEnvelope(
            24000, kOneSecondFrames, 48000, false);
    if (std::fabs(mainQuarter - 1.0f) > 0.0001f ||
        std::fabs(mainHalf - 1.0f) > 0.0001f ||
        !(mainThreeQuarter > 0.0f && mainThreeQuarter < 1.0f) ||
        std::fabs(mainEnd) > 0.0001f ||
        std::fabs(backgroundHalf - 1.0f) > 0.0001f) {
        std::fprintf(stderr,
                     "main-only second-half fade envelope is incorrect\n");
        return 1;
    }
    if (std::fabs(nfsmw_exhaust::native_audio::testing::tonePitchMultiplier(
                      0.0f) - 0.892f) > 0.0001f ||
        std::fabs(nfsmw_exhaust::native_audio::testing::tonePitchMultiplier(
                      1.0f) - 1.0f) > 0.0001f ||
        std::fabs(nfsmw_exhaust::native_audio::testing::tonePitchMultiplier(
                      1.4f) - 1.12f) > 0.0001f ||
        std::fabs(nfsmw_exhaust::native_audio::testing::tonePitchMultiplier(
                      2.0f) - 1.30f) > 0.0001f) {
        std::fprintf(stderr, "per-car tone pitch mapping is incorrect\n");
        return 1;
    }
    if (std::fabs(nfsmw_exhaust::native_audio::testing::toneGainCompensation(
                      0.0f) - 1.4998f) > 0.0001f ||
        std::fabs(nfsmw_exhaust::native_audio::testing::toneGainCompensation(
                      0.1f) - 1.42881902875f) > 0.0001f ||
        std::fabs(nfsmw_exhaust::native_audio::testing::toneGainCompensation(
                      0.5f) - 1.19718671875f) > 0.0001f ||
        std::fabs(nfsmw_exhaust::native_audio::testing::toneGainCompensation(
                      1.0f) - 1.0f) > 0.0001f ||
        std::fabs(nfsmw_exhaust::native_audio::testing::toneGainCompensation(
                      2.0f) - 1.0f) > 0.0001f) {
        std::fprintf(stderr, "dark-tone loudness compensation is incorrect\n");
        return 1;
    }
    if (std::fabs(nfsmw_exhaust::native_audio::testing::toneWetMultiplier(
                      0.0f) - 0.80f) > 0.0001f ||
        std::fabs(nfsmw_exhaust::native_audio::testing::toneWetMultiplier(
                      0.5f) - 0.90f) > 0.0001f ||
        std::fabs(nfsmw_exhaust::native_audio::testing::toneWetMultiplier(
                      1.0f) - 1.0f) > 0.0001f ||
        std::fabs(nfsmw_exhaust::native_audio::testing::toneWetMultiplier(
                      2.0f) - 1.30f) > 0.0001f) {
        std::fprintf(stderr, "per-car tone reverb mapping is incorrect\n");
        return 1;
    }
    float neutralState = 0.0f;
    const float neutral =
        nfsmw_exhaust::native_audio::testing::shapeToneSample(
            0.5f, &neutralState, 1.0f, 48000);
    float darkState = 0.0f;
    float brightState = 0.0f;
    float dark = 0.0f;
    float bright = 0.0f;
    for (int index = 0; index < 64; ++index) {
        const float source = (index & 1) == 0 ? 0.5f : -0.5f;
        dark = nfsmw_exhaust::native_audio::testing::shapeToneSample(
            source, &darkState, 0.0f, 48000);
        bright = nfsmw_exhaust::native_audio::testing::shapeToneSample(
            source, &brightState, 2.0f, 48000);
    }
    if (neutral != 0.5f || neutralState != 0.0f ||
        std::fabs(dark) >= 0.20f || std::fabs(bright) <= 2.0f ||
        std::fabs(dark - bright) < 0.05f) {
        std::fprintf(stderr,
                     "neutral bypass or dark/bright tone shaping is incorrect\n");
        return 1;
    }
    std::string modulePath = argv[1];
    std::replace(modulePath.begin(), modulePath.end(), '/', '\\');
    modulePath += "\\NFSMWExhaustBackfire.asi";
    std::string manifestPath = argv[1];
    std::replace(manifestPath.begin(), manifestPath.end(), '/', '\\');
    manifestPath += "\\tests\\data\\NativeAudioManifest.ini";
    std::string incompleteManifestPath = argv[1];
    std::replace(incompleteManifestPath.begin(), incompleteManifestPath.end(),
                 '/', '\\');
    incompleteManifestPath += "\\tests\\data\\IncompleteAudio.ini";
    if (nfsmw_exhaust::native_audio::initialize(
            modulePath.c_str(), incompleteManifestPath.c_str())) {
        std::fprintf(stderr, "incomplete audio manifest was accepted\n");
        return 1;
    }
    if (!nfsmw_exhaust::native_audio::initialize(modulePath.c_str(),
                                                  manifestPath.c_str())) {
        std::fprintf(stderr, "native audio initialization failed\n");
        return 1;
    }
    if (!nfsmw_exhaust::native_audio::reverbReady()) {
        std::fprintf(stderr, "software ambience initialization failed\n");
        return 1;
    }
    constexpr std::size_t kFrames = 4096;
    std::vector<std::int16_t> gamePcm(kFrames * 2u, 1234);
    nfsmw_exhaust::native_audio::mixIntoGameBuffer(
        gamePcm.data(), kFrames, 2, 48000);
    if (!std::all_of(gamePcm.begin(), gamePcm.end(),
                     [](std::int16_t sample) { return sample == 1234; })) {
        std::fprintf(stderr, "inactive mixer modified the game PCM buffer\n");
        return 1;
    }
    nfsmw_exhaust::native_audio::Spatialization spatial{};
    spatial.gain = 0.85f;
    spatial.pan = 0.25f;
    spatial.lowPass = 1.0f;
    if (!nfsmw_exhaust::native_audio::play(
            "audio/backfire/./g1_01.wav", spatial)) {
        std::fprintf(stderr, "tone-shaped software voice enqueue failed\n");
        return 1;
    }

    std::fill(gamePcm.begin(), gamePcm.end(), 0);
    nfsmw_exhaust::native_audio::mixIntoGameBuffer(
        gamePcm.data(), kFrames, 2, 48000);
    if (std::all_of(gamePcm.begin(), gamePcm.end(),
                    [](std::int16_t sample) { return sample == 0; })) {
        std::fprintf(stderr, "queued voice did not reach the game PCM buffer\n");
        return 1;
    }
    bool stereoDifference = false;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        if (gamePcm[frame * 2u] != gamePcm[frame * 2u + 1u]) {
            stereoDifference = true;
            break;
        }
    }
    if (!stereoDifference) {
        std::fprintf(stderr, "spatial pan was not applied to game PCM\n");
        return 1;
    }

    nfsmw_exhaust::native_audio::setPlaybackAllowed(false);
    std::fill(gamePcm.begin(), gamePcm.end(), 0);
    nfsmw_exhaust::native_audio::mixIntoGameBuffer(
        gamePcm.data(), kFrames, 2, 48000);
    if (!std::all_of(gamePcm.begin(), gamePcm.end(),
                     [](std::int16_t sample) { return sample == 0; }) ||
        nfsmw_exhaust::native_audio::play(
            "audio/backfire/./g1_01.wav", spatial)) {
        std::fprintf(stderr,
                     "suspended mixer rendered or accepted a voice\n");
        return 1;
    }
    nfsmw_exhaust::native_audio::setPlaybackAllowed(true);
    if (!nfsmw_exhaust::native_audio::play(
            "audio/backfire/./g1_01.wav", spatial)) {
        std::fprintf(stderr, "resumed mixer rejected a new voice\n");
        return 1;
    }
    std::fill(gamePcm.begin(), gamePcm.end(), 0);
    nfsmw_exhaust::native_audio::mixIntoGameBuffer(
        gamePcm.data(), kFrames, 2, 48000);
    if (std::all_of(gamePcm.begin(), gamePcm.end(),
                    [](std::int16_t sample) { return sample == 0; })) {
        std::fprintf(stderr, "resumed mixer did not render a new voice\n");
        return 1;
    }

    for (int block = 0; block < 48; ++block) {
        std::fill(gamePcm.begin(), gamePcm.end(), 0);
        nfsmw_exhaust::native_audio::mixIntoGameBuffer(
            gamePcm.data(), kFrames, 2, 48000);
    }
    std::fill(gamePcm.begin(), gamePcm.end(), 0);
    nfsmw_exhaust::native_audio::mixIntoGameBuffer(
        gamePcm.data(), kFrames, 2, 48000);
    if (!std::all_of(gamePcm.begin(), gamePcm.end(),
                     [](std::int16_t sample) { return sample == 0; })) {
        std::fprintf(stderr, "finished software voice was not retired\n");
        return 1;
    }
    nfsmw_exhaust::native_audio::shutdown();
    std::puts("Native game PCM mixing and software ambience verified");
    return 0;
}
