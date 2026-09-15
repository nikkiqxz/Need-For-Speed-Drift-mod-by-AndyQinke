#include "NativeAudio.hpp"

#include "NativeLog.hpp"

#include <windows.h>
#include <xaudio2.h>
#include <xaudio2fx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nfsmw_exhaust::native_audio {
namespace {

constexpr std::size_t kClipCount = 12;
constexpr std::size_t kVoiceCount = 32;
constexpr std::uint64_t kMaxWaveBytes = 16u * 1024u * 1024u;
constexpr float kBaseGain = 0.999306f;
constexpr float kReverbGain = 0.52f;
constexpr float kDirectPanScale = 0.70f;
constexpr float kWetPanScale = 0.18f;
constexpr float kTonePitchRatio = 0.86f;
constexpr float kToneLowPassCeiling = 0.82f;
constexpr float kToneOutputTrim = 0.78f;
constexpr float kBassCutoffHz = 380.0f;
constexpr float kBassBodyMix = 0.80f;
constexpr float kMetalCenterHz = 1450.0f;
constexpr float kMetalQ = 3.0f;
constexpr float kMetalGainDb = 7.0f;
constexpr float kMetalRingCenterHz = 2650.0f;
constexpr float kMetalRingQ = 4.0f;
constexpr float kMetalRingGainDb = 4.5f;
constexpr std::array<const char*, kClipCount> kClipAssetIds{{
    "audio/backfire/g1_01.wav", "audio/backfire/g1_02.wav",
    "audio/backfire/g1_03.wav", "audio/backfire/g2_01.wav",
    "audio/backfire/g2_02.wav", "audio/backfire/g2_03.wav",
    "audio/backfire/g3_01.wav", "audio/backfire/g3_02.wav",
    "audio/backfire/g3_03.wav", "audio/backfire/g4_01.wav",
    "audio/backfire/g4_02.wav", "audio/backfire/g4_03.wav"}};

struct BiquadCoefficients {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
};

BiquadCoefficients makePeakingBiquad(float sampleRate, float centerHz,
                                     float q, float gainDb) noexcept {
    const float amplitude = std::pow(10.0f, gainDb / 40.0f);
    const float omega =
        2.0f * 3.14159265358979323846f * centerHz / sampleRate;
    const float alpha = std::sin(omega) / (2.0f * q);
    const float a0 = 1.0f + alpha / amplitude;
    return {(1.0f + alpha * amplitude) / a0,
            (-2.0f * std::cos(omega)) / a0,
            (1.0f - alpha * amplitude) / a0,
            (-2.0f * std::cos(omega)) / a0,
            (1.0f - alpha / amplitude) / a0};
}

struct Clip {
    std::string assetId;
    WAVEFORMATEX format{};
    std::vector<std::uint8_t> samples;
};

struct VoiceSlot {
    IXAudio2SourceVoice* voice = nullptr;
    std::uint64_t sequence = 0;
};

std::array<Clip, kClipCount> g_clips{};
std::array<VoiceSlot, kVoiceCount> g_voices{};
IXAudio2* g_engine = nullptr;
IXAudio2MasteringVoice* g_masterVoice = nullptr;
IXAudio2SubmixVoice* g_reverbVoice = nullptr;
std::uint64_t g_sequence = 0;
bool g_initialized = false;
bool g_ready = false;
UINT32 g_outputChannels = 0;
bool g_reverbReady = false;

std::uint16_t readU16(const std::uint8_t* data) noexcept {
    std::uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::uint32_t readU32(const std::uint8_t* data) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool shapePcm16Tone(Clip* clip) noexcept {
    if (clip == nullptr || clip->format.wFormatTag != WAVE_FORMAT_PCM ||
        clip->format.wBitsPerSample != 16 || clip->format.nChannels == 0 ||
        clip->format.nBlockAlign != clip->format.nChannels * 2u ||
        clip->samples.size() % clip->format.nBlockAlign != 0) {
        return false;
    }

    const float sampleRate =
        static_cast<float>(clip->format.nSamplesPerSec);
    const float alpha = 1.0f -
        std::exp(-2.0f * 3.14159265358979323846f * kBassCutoffHz /
                 sampleRate);
    const BiquadCoefficients metalBody = makePeakingBiquad(
        sampleRate, kMetalCenterHz, kMetalQ, kMetalGainDb);
    const BiquadCoefficients metalRing = makePeakingBiquad(
        sampleRate, kMetalRingCenterHz, kMetalRingQ, kMetalRingGainDb);
    std::vector<float> lowBand(clip->format.nChannels, 0.0f);
    std::array<std::vector<float>, 2> input1;
    std::array<std::vector<float>, 2> input2;
    std::array<std::vector<float>, 2> output1;
    std::array<std::vector<float>, 2> output2;
    for (std::size_t filter = 0; filter < 2; ++filter) {
        input1[filter].resize(clip->format.nChannels, 0.0f);
        input2[filter].resize(clip->format.nChannels, 0.0f);
        output1[filter].resize(clip->format.nChannels, 0.0f);
        output2[filter].resize(clip->format.nChannels, 0.0f);
    }
    const std::array<BiquadCoefficients, 2> metalFilters{{metalBody,
                                                          metalRing}};
    const std::size_t sampleCount = clip->samples.size() / sizeof(std::int16_t);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        std::int16_t pcm = 0;
        std::memcpy(&pcm, clip->samples.data() + index * sizeof(pcm),
                    sizeof(pcm));
        const std::size_t channel = index % clip->format.nChannels;
        const float dry = static_cast<float>(pcm) / 32768.0f;
        lowBand[channel] += alpha * (dry - lowBand[channel]);
        float metal = dry;
        for (std::size_t filter = 0; filter < metalFilters.size(); ++filter) {
            const auto& coefficients = metalFilters[filter];
            const float filtered =
                coefficients.b0 * metal +
                coefficients.b1 * input1[filter][channel] +
                coefficients.b2 * input2[filter][channel] -
                coefficients.a1 * output1[filter][channel] -
                coefficients.a2 * output2[filter][channel];
            input2[filter][channel] = input1[filter][channel];
            input1[filter][channel] = metal;
            output2[filter][channel] = output1[filter][channel];
            output1[filter][channel] = filtered;
            metal = filtered;
        }
        const float shaped = std::clamp(
            (metal + kBassBodyMix * lowBand[channel]) * kToneOutputTrim,
            -1.0f, 1.0f);
        const auto output = static_cast<std::int16_t>(
            std::lround(shaped * (shaped < 0.0f ? 32768.0f : 32767.0f)));
        std::memcpy(clip->samples.data() + index * sizeof(output), &output,
                    sizeof(output));
    }
    return true;
}

bool hasFourCc(const std::uint8_t* data, const char expected[5]) noexcept {
    return std::memcmp(data, expected, 4) == 0;
}

bool readFileBytes(const char* path, std::vector<std::uint8_t>* output) noexcept {
    if (path == nullptr || output == nullptr) return false;
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size) != FALSE &&
                           size.QuadPart >= 12 &&
                           static_cast<std::uint64_t>(size.QuadPart) <=
                               kMaxWaveBytes;
    if (!validSize) {
        CloseHandle(file);
        return false;
    }

    output->resize(static_cast<std::size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const bool read = ReadFile(file, output->data(),
                               static_cast<DWORD>(output->size()), &bytesRead,
                               nullptr) != FALSE &&
                      bytesRead == output->size();
    CloseHandle(file);
    if (!read) output->clear();
    return read;
}

bool loadWave(const char* path, Clip* clip) noexcept {
    if (clip == nullptr) return false;
    std::vector<std::uint8_t> file;
    if (!readFileBytes(path, &file) || !hasFourCc(file.data(), "RIFF") ||
        !hasFourCc(file.data() + 8, "WAVE")) {
        return false;
    }

    bool foundFormat = false;
    bool foundData = false;
    std::size_t offset = 12;
    while (offset + 8 <= file.size()) {
        const std::uint8_t* header = file.data() + offset;
        const std::uint32_t chunkSize = readU32(header + 4);
        const std::size_t payload = offset + 8;
        if (payload > file.size() || chunkSize > file.size() - payload) {
            return false;
        }

        if (hasFourCc(header, "fmt ") && chunkSize >= 16) {
            const std::uint8_t* format = file.data() + payload;
            clip->format.wFormatTag = readU16(format);
            clip->format.nChannels = readU16(format + 2);
            clip->format.nSamplesPerSec = readU32(format + 4);
            clip->format.nAvgBytesPerSec = readU32(format + 8);
            clip->format.nBlockAlign = readU16(format + 12);
            clip->format.wBitsPerSample = readU16(format + 14);
            clip->format.cbSize = chunkSize >= 18 ? readU16(format + 16) : 0;
            foundFormat = true;
        } else if (hasFourCc(header, "data") && chunkSize != 0) {
            clip->samples.assign(file.begin() + payload,
                                 file.begin() + payload + chunkSize);
            foundData = true;
        }

        const std::size_t padded = static_cast<std::size_t>(chunkSize) +
                                   (chunkSize & 1u);
        if (padded > file.size() - payload) break;
        offset = payload + padded;
    }

    const bool valid = foundFormat && foundData &&
                       clip->format.wFormatTag == WAVE_FORMAT_PCM &&
                       clip->format.nChannels != 0 &&
                       clip->format.nSamplesPerSec != 0 &&
                       clip->format.nBlockAlign != 0 &&
                       clip->format.wBitsPerSample != 0 &&
                       clip->samples.size() <= UINT32_MAX;
    return valid && shapePcm16Tone(clip);
}

bool resolveAssetPath(const char* modulePath, const char* assetId,
                      char output[MAX_PATH]) noexcept {
    if (modulePath == nullptr || assetId == nullptr || output == nullptr ||
        std::strstr(assetId, "..") != nullptr ||
        std::strchr(assetId, ':') != nullptr || *assetId == '\\' ||
        *assetId == '/') {
        return false;
    }
    const char* slash = std::strrchr(modulePath, '\\');
    if (slash == nullptr) return false;
    const std::size_t directoryLength =
        static_cast<std::size_t>(slash - modulePath + 1);
    const std::size_t assetLength = std::strlen(assetId);
    if (directoryLength + assetLength >= MAX_PATH) return false;
    std::memcpy(output, modulePath, directoryLength);
    for (std::size_t index = 0; index < assetLength; ++index) {
        output[directoryLength + index] =
            assetId[index] == '/' ? '\\' : assetId[index];
    }
    output[directoryLength + assetLength] = '\0';
    return true;
}

void destroyVoice(VoiceSlot* slot) noexcept {
    if (slot == nullptr || slot->voice == nullptr) return;
    slot->voice->Stop(0);
    slot->voice->FlushSourceBuffers();
    slot->voice->DestroyVoice();
    *slot = {};
}

VoiceSlot* acquireVoice() noexcept {
    for (auto& slot : g_voices) {
        if (slot.voice == nullptr) return &slot;
        XAUDIO2_VOICE_STATE state{};
        slot.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0) {
            destroyVoice(&slot);
            return &slot;
        }
    }
    auto* oldest = &g_voices[0];
    for (auto& slot : g_voices) {
        if (slot.sequence < oldest->sequence) oldest = &slot;
    }
    destroyVoice(oldest);
    return oldest;
}

const Clip* findClip(const char* assetId) noexcept {
    if (assetId == nullptr) return nullptr;
    for (const auto& clip : g_clips) {
        if (_stricmp(clip.assetId.c_str(), assetId) == 0) return &clip;
    }
    return nullptr;
}

bool initializeReverb(UINT32 sampleRate) noexcept {
    IUnknown* effect = nullptr;
    if (FAILED(XAudio2CreateReverb(&effect)) || effect == nullptr) return false;

    XAUDIO2_EFFECT_DESCRIPTOR descriptor{};
    descriptor.InitialState = TRUE;
    descriptor.OutputChannels = 2;
    descriptor.pEffect = effect;
    XAUDIO2_EFFECT_CHAIN chain{};
    chain.EffectCount = 1;
    chain.pEffectDescriptors = &descriptor;
    const HRESULT createResult = g_engine->CreateSubmixVoice(
        &g_reverbVoice, 2, sampleRate, 0, 0, nullptr, &chain);
    effect->Release();
    if (FAILED(createResult) || g_reverbVoice == nullptr) return false;

    XAUDIO2FX_REVERB_I3DL2_PARAMETERS preset =
        XAUDIO2FX_I3DL2_PRESET_ALLEY;
    preset.WetDryMix = 100.0f;
    preset.Room = -1300;
    preset.RoomHF = -2300;
    preset.DecayTime = 0.95f;
    preset.Reflections = -500;
    preset.ReflectionsDelay = 0.015f;
    preset.Reverb = -850;
    preset.ReverbDelay = 0.025f;
    preset.Diffusion = 68.0f;
    preset.Density = 75.0f;
    XAUDIO2FX_REVERB_PARAMETERS parameters{};
    ReverbConvertI3DL2ToNative(&preset, &parameters);
    if (FAILED(g_reverbVoice->SetEffectParameters(
            0, &parameters, sizeof(parameters))) ||
        FAILED(g_reverbVoice->SetVolume(kReverbGain))) {
        g_reverbVoice->DestroyVoice();
        g_reverbVoice = nullptr;
        return false;
    }

    std::vector<float> matrix(2u * g_outputChannels, 0.0f);
    if (g_outputChannels == 1) {
        matrix[0] = 0.5f;
        matrix[1] = 0.5f;
    } else {
        matrix[0] = 0.55f;
        matrix[g_outputChannels + 1u] = 0.55f;
        if (g_outputChannels == 4) {
            matrix[2] = 0.82f;
            matrix[g_outputChannels + 3u] = 0.82f;
        } else if (g_outputChannels >= 6) {
            matrix[2] = 0.18f;
            matrix[g_outputChannels + 2u] = 0.18f;
            matrix[4] = 0.82f;
            matrix[g_outputChannels + 5u] = 0.82f;
            if (g_outputChannels >= 8) {
                matrix[6] = 0.62f;
                matrix[g_outputChannels + 7u] = 0.62f;
            }
        }
    }
    if (FAILED(g_reverbVoice->SetOutputMatrix(
            g_masterVoice, 2, g_outputChannels, matrix.data()))) {
        g_reverbVoice->DestroyVoice();
        g_reverbVoice = nullptr;
        return false;
    }
    return true;
}

}  // namespace

bool initialize(const char* modulePath) noexcept {
    if (g_initialized) return g_ready;
    g_initialized = true;

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        native_log::write("AUDIO_INIT_FAIL CoInitializeEx hr=0x%08X",
                          static_cast<unsigned>(comResult));
        return false;
    }

    const HRESULT engineResult = XAudio2Create(&g_engine, 0);
    if (FAILED(engineResult) || g_engine == nullptr) {
        native_log::write("AUDIO_INIT_FAIL XAudio2Create hr=0x%08X",
                          static_cast<unsigned>(engineResult));
        return false;
    }
    const HRESULT masterResult = g_engine->CreateMasteringVoice(&g_masterVoice);
    if (FAILED(masterResult) || g_masterVoice == nullptr) {
        native_log::write("AUDIO_INIT_FAIL CreateMasteringVoice hr=0x%08X",
                          static_cast<unsigned>(masterResult));
        return false;
    }
    XAUDIO2_VOICE_DETAILS masterDetails{};
    g_masterVoice->GetVoiceDetails(&masterDetails);
    g_outputChannels = masterDetails.InputChannels;
    if (g_outputChannels == 0) {
        native_log::write("AUDIO_INIT_FAIL mastering voice has no channels");
        return false;
    }
    g_reverbReady = initializeReverb(masterDetails.InputSampleRate);

    for (std::size_t clipIndex = 0; clipIndex < kClipAssetIds.size();
         ++clipIndex) {
        const char* assetId = kClipAssetIds[clipIndex];
        Clip& clip = g_clips[clipIndex];
        clip.assetId = assetId;
        char path[MAX_PATH] = {};
        if (!resolveAssetPath(modulePath, assetId, path) ||
            !loadWave(path, &clip)) {
            native_log::write("AUDIO_INIT_FAIL invalid WAV asset='%s'",
                              assetId);
            return false;
        }
    }

    g_ready = true;
    const auto& format = g_clips[0].format;
    native_log::write(
        "AUDIO_INIT_OK backend=XAudio2 clips=%u voices=%u "
        "format=%uHz/%ubit/%uch outputChannels=%u spatial=1 volume=%.5f "
        "directPanScale=%.2f reverb=%d reverbGain=%.2f preset=TRACK_SHORT "
        "tone=pitch%.2f/bass%.2f@%.0fHz/metal%.0fHz+%.1fdB/Q%.1f/"
        "ring%.0fHz+%.1fdB/Q%.1f/trim%.2f/lowpass%.2f",
        static_cast<unsigned>(g_clips.size()),
        static_cast<unsigned>(g_voices.size()), format.nSamplesPerSec,
        format.wBitsPerSample, format.nChannels, g_outputChannels,
        static_cast<double>(kBaseGain), static_cast<double>(kDirectPanScale),
        g_reverbReady ? 1 : 0, static_cast<double>(kReverbGain),
        static_cast<double>(kTonePitchRatio),
        static_cast<double>(kBassBodyMix), static_cast<double>(kBassCutoffHz),
        static_cast<double>(kMetalCenterHz),
        static_cast<double>(kMetalGainDb), static_cast<double>(kMetalQ),
        static_cast<double>(kMetalRingCenterHz),
        static_cast<double>(kMetalRingGainDb),
        static_cast<double>(kMetalRingQ),
        static_cast<double>(kToneOutputTrim),
        static_cast<double>(kToneLowPassCeiling));
    return true;
}

bool play(const char* assetId, const Spatialization& spatial) noexcept {
    if (!g_ready) return false;
    const Clip* clip = findClip(assetId);
    if (clip == nullptr) return false;
    VoiceSlot* slot = acquireVoice();
    if (slot == nullptr) return false;

    XAUDIO2_SEND_DESCRIPTOR sendDescriptors[2]{};
    XAUDIO2_VOICE_SENDS sendList{};
    const XAUDIO2_VOICE_SENDS* sends = nullptr;
    if (g_reverbReady) {
        sendDescriptors[0].pOutputVoice = g_masterVoice;
        sendDescriptors[1].pOutputVoice = g_reverbVoice;
        sendList.SendCount = 2;
        sendList.pSends = sendDescriptors;
        sends = &sendList;
    }
    HRESULT result = g_engine->CreateSourceVoice(
        &slot->voice, &clip->format, XAUDIO2_VOICE_USEFILTER,
        XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, sends, nullptr);
    if (FAILED(result) || slot->voice == nullptr) {
        *slot = {};
        return false;
    }

    const float gain = std::clamp(spatial.gain, 0.0f, 1.0f);
    const float pan = std::clamp(spatial.pan, -1.0f, 1.0f);
    const float lowPass = std::clamp(spatial.lowPass, 0.0f, 1.0f);
    result = slot->voice->SetVolume(kBaseGain * gain);
    if (SUCCEEDED(result)) {
        result = slot->voice->SetFrequencyRatio(kTonePitchRatio);
    }
    if (SUCCEEDED(result)) {
        std::vector<float> matrix(
            static_cast<std::size_t>(clip->format.nChannels) *
                g_outputChannels,
            0.0f);
        const float directPan = pan * kDirectPanScale;
        const float left = directPan <= 0.0f ? 1.0f : 1.0f - directPan;
        const float right = directPan >= 0.0f ? 1.0f : 1.0f + directPan;
        for (UINT32 source = 0; source < clip->format.nChannels; ++source) {
            const float sourceMix =
                1.0f / static_cast<float>(clip->format.nChannels);
            if (g_outputChannels == 1) {
                matrix[source] = sourceMix;
            } else {
                const std::size_t base =
                    static_cast<std::size_t>(source) * g_outputChannels;
                matrix[base] = left * sourceMix;
                matrix[base + 1u] = right * sourceMix;
            }
        }
        result = slot->voice->SetOutputMatrix(
            g_masterVoice, clip->format.nChannels, g_outputChannels,
            matrix.data());
    }
    if (SUCCEEDED(result) && g_reverbReady) {
        std::vector<float> wetMatrix(
            static_cast<std::size_t>(clip->format.nChannels) * 2u, 0.0f);
        const float wetPan = pan * kWetPanScale;
        const float wetLeft = wetPan <= 0.0f ? 1.0f : 1.0f - wetPan;
        const float wetRight = wetPan >= 0.0f ? 1.0f : 1.0f + wetPan;
        for (UINT32 source = 0; source < clip->format.nChannels; ++source) {
            const float sourceMix =
                1.0f / static_cast<float>(clip->format.nChannels);
            const std::size_t base = static_cast<std::size_t>(source) * 2u;
            wetMatrix[base] = wetLeft * sourceMix;
            wetMatrix[base + 1u] = wetRight * sourceMix;
        }
        result = slot->voice->SetOutputMatrix(
            g_reverbVoice, clip->format.nChannels, 2, wetMatrix.data());
    }
    if (SUCCEEDED(result)) {
        XAUDIO2_FILTER_PARAMETERS filter{};
        filter.Type = LowPassFilter;
        filter.Frequency = std::min(lowPass, kToneLowPassCeiling);
        filter.OneOverQ = 1.25f;
        result = slot->voice->SetFilterParameters(&filter);
    }
    if (FAILED(result)) {
        destroyVoice(slot);
        return false;
    }

    XAUDIO2_BUFFER buffer{};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = static_cast<UINT32>(clip->samples.size());
    buffer.pAudioData = clip->samples.data();
    result = slot->voice->SubmitSourceBuffer(&buffer);
    if (SUCCEEDED(result)) result = slot->voice->Start(0);
    if (FAILED(result)) {
        destroyVoice(slot);
        return false;
    }
    ++g_sequence;
    if (g_sequence == 0) ++g_sequence;
    slot->sequence = g_sequence;
    return true;
}

bool reverbReady() noexcept {
    return g_reverbReady;
}

}  // namespace nfsmw_exhaust::native_audio
