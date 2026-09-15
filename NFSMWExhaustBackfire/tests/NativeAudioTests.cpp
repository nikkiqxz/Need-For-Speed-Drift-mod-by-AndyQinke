#include "NativeAudio.hpp"
#include "NativeLog.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    nfsmw_exhaust::native_log::open("NativeAudioTests.log");
    if (argc != 2) {
        std::fprintf(stderr, "source directory argument is required\n");
        return 1;
    }
    std::string modulePath = argv[1];
    std::replace(modulePath.begin(), modulePath.end(), '/', '\\');
    modulePath += "\\NFSMWExhaustBackfire.asi";
    if (!nfsmw_exhaust::native_audio::initialize(modulePath.c_str())) {
        std::fprintf(stderr, "native audio initialization failed\n");
        return 1;
    }
    if (!nfsmw_exhaust::native_audio::reverbReady()) {
        std::fprintf(stderr, "XAudio2 reverb submix initialization failed\n");
        return 1;
    }
    nfsmw_exhaust::native_audio::Spatialization spatial{};
    spatial.gain = 0.85f;
    spatial.pan = 0.25f;
    spatial.lowPass = 1.0f;
    if (!nfsmw_exhaust::native_audio::play(
            "audio/backfire/g1_01.wav", spatial)) {
        std::fprintf(stderr, "tone-shaped source voice playback failed\n");
        return 1;
    }
    std::puts("Native audio and device-rate reverb initialized");
    return 0;
}
