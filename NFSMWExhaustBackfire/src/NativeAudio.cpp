#include "NativeAudio.hpp"

#include "NativeLog.hpp"
#include "nfsmw_exhaust/AudioBank.hpp"

#include <windows.h>
#include <mmreg.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nfsmw_exhaust::native_audio {
namespace {

constexpr std::size_t kClipCount = 16;
constexpr std::size_t kVoiceCount = 32;
constexpr std::size_t kPendingVoiceCount = 64;
constexpr std::size_t kMaxOutputChannels = 8;
constexpr std::uint64_t kActiveVoiceMaskBits = UINT32_MAX;
constexpr std::uint64_t kPendingPlayIncrement = 1ull << 32u;
constexpr std::uint64_t kMaxWaveBytes = 16u * 1024u * 1024u;
constexpr float kBaseGain = 0.710992f;
constexpr float kReverbGain = 1.152f;
constexpr float kDirectPanScale = 1.0f;
constexpr float kWetPanScale = 1.0f;
constexpr float kTonePitchRatio = 0.90f;
constexpr float kToneLowPassCeiling = 0.90f;
constexpr float kToneOutputTrim = 0.82f;
constexpr float kVehicleToneCutoffHz = 650.0f;
constexpr float kVehicleToneDarkStrength = 1.35f;
constexpr float kVehicleToneBrightHighStrength = 4.50f;
constexpr float kVehicleToneBrightLowCut = 0.65f;
constexpr float kVehicleToneDarkPitchRange = 0.108f;
constexpr float kVehicleToneBrightPitchRange = 0.30f;
constexpr float kVehicleToneDarkGainLinear = 0.75f;
constexpr float kVehicleToneDarkGainCurve = 0.25f;
constexpr float kVehicleToneDarkExtraGain = 0.10f;
constexpr float kVehicleToneDarkCompensationScale = 0.4165f;
constexpr float kVehicleToneDarkWetReduction = 0.20f;
constexpr float kBassCutoffHz = 380.0f;
constexpr float kBassBodyMix = 0.60f;
constexpr float kMetalCenterHz = 1450.0f;
constexpr float kMetalQ = 3.0f;
constexpr float kMetalGainDb = 4.5f;
constexpr float kMetalRingCenterHz = 2650.0f;
constexpr float kMetalRingQ = 4.0f;
constexpr float kMetalRingGainDb = 2.5f;
constexpr float kFadeInSeconds = 0.008f;
constexpr float kFadeOutSeconds = 0.180f;
constexpr float kMainFadeOutStartRatio = 0.50f;
constexpr char kBackgroundInAssetId[] = "audio/backfire/IN.wav";
constexpr char kBackgroundOutAssetId[] = "audio/backfire/OUT.wav";
constexpr float kBackgroundInMix = 0.30f;
constexpr float kBackgroundOutMix = 0.24f;

struct ReflectionTap {
    float delaySeconds;
    float gain;
    float spread;
};

// Sparse outdoor reflections preserve the previous open-air character while
// sharing the game's output device and playback clock.
constexpr std::array<ReflectionTap, 8> kReflectionTaps{{
    {0.110f, 0.180f, -0.18f}, {0.180f, 0.145f, 0.20f},
    {0.330f, 0.105f, -0.28f}, {0.520f, 0.078f, 0.30f},
    {0.800f, 0.056f, -0.34f}, {1.180f, 0.040f, 0.36f},
    {1.700f, 0.027f, -0.40f}, {2.300f, 0.017f, 0.42f},
}};

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
    const float omega = 2.0f * 3.14159265358979323846f * centerHz / sampleRate;
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
    std::vector<float> monoSamples;
};

struct VoiceSlot {
    const Clip* clip = nullptr;
    std::uint64_t sequence = 0;
    std::uint64_t outputFrame = 0;
    std::uint32_t outputSampleRate = 0;
    float gain = 0.0f;
    float pan = 0.0f;
    float lowPass = 1.0f;
    float lowPassState = 0.0f;
    float toneCharacter = 1.0f;
    float toneLowBandState = 0.0f;
    std::array<float, kReflectionTaps.size()> reflectedToneLowBandStates{};
    double sourceStep = 0.0;
    std::uint64_t directFrames = 0;
    std::uint64_t lastDelay = 0;
    std::array<std::uint64_t, kReflectionTaps.size()> tapDelays{};
    float lowPassAlpha = 1.0f;
    float toneLowPassAlpha = 1.0f;
    float toneGainCompensation = 1.0f;
    float wetTone = 1.0f;
};

struct PendingVoice {
    const Clip* clip = nullptr;
    Spatialization spatial{};
};

std::array<Clip, kClipCount> g_clips{};
Clip g_backgroundIn{};
Clip g_backgroundOut{};
std::array<VoiceSlot, kVoiceCount> g_voices{};
std::array<PendingVoice, kPendingVoiceCount> g_pendingVoices{};
std::size_t g_pendingVoiceRead = 0;
std::size_t g_pendingVoiceCount = 0;
// Low 32 bits track active slots; high 32 bits count queued play requests.
// A single atomic snapshot makes the idle fast path linearizable.
std::atomic<std::uint64_t> g_voiceState{0};
std::atomic<bool> g_playbackAllowed{false};
std::uint64_t g_sequence = 0;
CRITICAL_SECTION g_voiceLock{};
CRITICAL_SECTION g_pendingVoiceLock{};
bool g_lockInitialized = false;
bool g_initialized = false;
bool g_ready = false;

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

bool shapePcm16Tone(Clip* clip, bool mainClip) noexcept {
    if (clip == nullptr || clip->format.wFormatTag != WAVE_FORMAT_PCM ||
        clip->format.wBitsPerSample != 16 || clip->format.nChannels == 0 ||
        clip->format.nBlockAlign != clip->format.nChannels * 2u ||
        clip->samples.size() % clip->format.nBlockAlign != 0) return false;

    const float sampleRate = static_cast<float>(clip->format.nSamplesPerSec);
    const float alpha = 1.0f - std::exp(
        -2.0f * 3.14159265358979323846f * kBassCutoffHz / sampleRate);
    const std::array<BiquadCoefficients, 2> filters{{
        makePeakingBiquad(sampleRate, kMetalCenterHz, kMetalQ, kMetalGainDb),
        makePeakingBiquad(sampleRate, kMetalRingCenterHz, kMetalRingQ,
                          kMetalRingGainDb)}};
    std::vector<float> lowBand(clip->format.nChannels, 0.0f);
    std::array<std::vector<float>, 2> input1, input2, output1, output2;
    for (std::size_t filter = 0; filter < filters.size(); ++filter) {
        input1[filter].resize(clip->format.nChannels, 0.0f);
        input2[filter].resize(clip->format.nChannels, 0.0f);
        output1[filter].resize(clip->format.nChannels, 0.0f);
        output2[filter].resize(clip->format.nChannels, 0.0f);
    }
    const std::size_t sampleCount = clip->samples.size() / sizeof(std::int16_t);
    const std::size_t frameCount = sampleCount / clip->format.nChannels;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        std::int16_t pcm = 0;
        std::memcpy(&pcm, clip->samples.data() + index * sizeof(pcm), sizeof(pcm));
        const std::size_t channel = index % clip->format.nChannels;
        const float dry = static_cast<float>(pcm) / 32768.0f;
        lowBand[channel] += alpha * (dry - lowBand[channel]);
        float metal = dry;
        for (std::size_t filter = 0; filter < filters.size(); ++filter) {
            const auto& c = filters[filter];
            const float filtered = c.b0 * metal + c.b1 * input1[filter][channel] +
                c.b2 * input2[filter][channel] - c.a1 * output1[filter][channel] -
                c.a2 * output2[filter][channel];
            input2[filter][channel] = input1[filter][channel];
            input1[filter][channel] = metal;
            output2[filter][channel] = output1[filter][channel];
            output1[filter][channel] = filtered;
            metal = filtered;
        }
        const std::size_t frame = index / clip->format.nChannels;
        const float envelope = testing::clipEnvelope(
            frame, frameCount, clip->format.nSamplesPerSec, mainClip);
        const float shaped = std::clamp(
            (metal + kBassBodyMix * lowBand[channel]) * kToneOutputTrim * envelope,
            -1.0f, 1.0f);
        const auto output = static_cast<std::int16_t>(std::lround(
            shaped * (shaped < 0.0f ? 32768.0f : 32767.0f)));
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
        static_cast<std::uint64_t>(size.QuadPart) <= kMaxWaveBytes;
    if (!validSize) {
        CloseHandle(file);
        return false;
    }
    output->resize(static_cast<std::size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    const bool read = ReadFile(file, output->data(),
        static_cast<DWORD>(output->size()), &bytesRead, nullptr) != FALSE &&
        bytesRead == output->size();
    CloseHandle(file);
    if (!read) output->clear();
    return read;
}

bool loadWave(const char* path, Clip* clip, bool mainClip) noexcept {
    if (clip == nullptr) return false;
    std::vector<std::uint8_t> file;
    if (!readFileBytes(path, &file) || !hasFourCc(file.data(), "RIFF") ||
        !hasFourCc(file.data() + 8, "WAVE")) return false;
    bool foundFormat = false;
    bool foundData = false;
    std::size_t offset = 12;
    while (offset + 8 <= file.size()) {
        const std::uint8_t* header = file.data() + offset;
        const std::uint32_t chunkSize = readU32(header + 4);
        const std::size_t payload = offset + 8;
        if (payload > file.size() || chunkSize > file.size() - payload) return false;
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
        clip->format.nChannels <= kMaxOutputChannels &&
        clip->format.nSamplesPerSec != 0 && clip->format.nBlockAlign != 0 &&
        clip->format.wBitsPerSample == 16 && clip->samples.size() <= UINT32_MAX;
    return valid && shapePcm16Tone(clip, mainClip);
}

bool resolveAssetPath(const char* modulePath, const char* assetId,
                      char output[MAX_PATH]) noexcept {
    if (modulePath == nullptr || assetId == nullptr || output == nullptr ||
        std::strstr(assetId, "..") != nullptr || std::strchr(assetId, ':') != nullptr ||
        *assetId == '\\' || *assetId == '/') return false;
    const char* slash = std::strrchr(modulePath, '\\');
    if (slash == nullptr) return false;
    const std::size_t directoryLength =
        static_cast<std::size_t>(slash - modulePath + 1);
    const std::size_t assetLength = std::strlen(assetId);
    if (directoryLength + assetLength >= MAX_PATH) return false;
    std::memcpy(output, modulePath, directoryLength);
    for (std::size_t index = 0; index < assetLength; ++index) {
        output[directoryLength + index] = assetId[index] == '/' ? '\\' : assetId[index];
    }
    output[directoryLength + assetLength] = '\0';
    return true;
}

bool sameFormat(const WAVEFORMATEX& left, const WAVEFORMATEX& right) noexcept {
    return left.wFormatTag == right.wFormatTag && left.nChannels == right.nChannels &&
        left.nSamplesPerSec == right.nSamplesPerSec &&
        left.nAvgBytesPerSec == right.nAvgBytesPerSec &&
        left.nBlockAlign == right.nBlockAlign &&
        left.wBitsPerSample == right.wBitsPerSample && left.cbSize == right.cbSize;
}

float pcm16At(const Clip& clip, std::size_t frame, std::size_t channel) noexcept {
    const std::size_t offset = frame * clip.format.nBlockAlign + channel * 2u;
    std::int16_t sample = 0;
    std::memcpy(&sample, clip.samples.data() + offset, sizeof(sample));
    return static_cast<float>(sample) / 32768.0f;
}

float monoPcm16At(const Clip& clip, std::size_t frame) noexcept {
    float sample = 0.0f;
    for (std::size_t channel = 0; channel < clip.format.nChannels; ++channel)
        sample += pcm16At(clip, frame, channel);
    return sample / static_cast<float>(clip.format.nChannels);
}

bool rebuildMonoSamples(Clip* clip) noexcept {
    if (clip == nullptr || clip->format.nBlockAlign == 0 ||
        clip->format.nChannels == 0 ||
        clip->samples.size() % clip->format.nBlockAlign != 0) {
        return false;
    }
    const std::size_t frameCount =
        clip->samples.size() / clip->format.nBlockAlign;
    try {
        clip->monoSamples.resize(frameCount);
    } catch (...) {
        clip->monoSamples.clear();
        return false;
    }
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        clip->monoSamples[frame] = monoPcm16At(*clip, frame);
    }
    return true;
}

float interpolatedMonoSample(const Clip& clip, double frame) noexcept {
    const std::size_t frameCount = clip.monoSamples.size();
    if (frame < 0.0 || frame >= static_cast<double>(frameCount)) return 0.0f;
    const auto first = static_cast<std::size_t>(frame);
    const std::size_t second = std::min(first + 1u, frameCount - 1u);
    const float fraction = static_cast<float>(frame - first);
    const float left = clip.monoSamples[first];
    return left + (clip.monoSamples[second] - left) * fraction;
}

bool mixBackgroundPair(Clip* mainClip, const Clip& backgroundIn,
                       const Clip& backgroundOut) noexcept {
    if (mainClip == nullptr || mainClip->format.wBitsPerSample != 16 ||
        !sameFormat(mainClip->format, backgroundIn.format) ||
        !sameFormat(mainClip->format, backgroundOut.format)) return false;
    const std::size_t blockAlign = mainClip->format.nBlockAlign;
    const std::size_t channels = mainClip->format.nChannels;
    const std::size_t mainFrames = mainClip->samples.size() / blockAlign;
    const std::size_t inFrames = backgroundIn.samples.size() / blockAlign;
    const std::size_t outFrames = backgroundOut.samples.size() / blockAlign;
    if (mainFrames == 0 || inFrames == 0 || outFrames == 0) return false;
    const std::size_t outStartFrame = mainFrames / 2u;
    const std::size_t totalFrames = std::max(
        std::max(mainFrames, inFrames), outStartFrame + outFrames);
    std::vector<std::uint8_t> mixed(totalFrames * blockAlign, 0u);
    for (std::size_t frame = 0; frame < totalFrames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            float sample = frame < mainFrames ? pcm16At(*mainClip, frame, channel) : 0.0f;
            if (frame < inFrames)
                sample += kBackgroundInMix * pcm16At(backgroundIn, frame, channel);
            if (frame >= outStartFrame && frame - outStartFrame < outFrames)
                sample += kBackgroundOutMix * pcm16At(
                    backgroundOut, frame - outStartFrame, channel);
            sample = std::clamp(sample, -1.0f, 1.0f);
            const auto output = static_cast<std::int16_t>(std::lround(
                sample * (sample < 0.0f ? 32768.0f : 32767.0f)));
            const std::size_t outputOffset = frame * blockAlign + channel * 2u;
            std::memcpy(mixed.data() + outputOffset, &output, sizeof(output));
        }
    }
    mainClip->samples = std::move(mixed);
    return true;
}

const Clip* findClip(const char* assetId) noexcept {
    if (assetId == nullptr) return nullptr;
    for (const auto& clip : g_clips)
        if (_stricmp(clip.assetId.c_str(), assetId) == 0) return &clip;
    return nullptr;
}

VoiceSlot* acquireVoice() noexcept {
    for (auto& slot : g_voices) if (slot.clip == nullptr) return &slot;
    auto* oldest = &g_voices[0];
    for (auto& slot : g_voices)
        if (slot.sequence < oldest->sequence) oldest = &slot;
    return oldest;
}

void startVoice(const PendingVoice& pending,
                std::uint32_t* activeMask) noexcept {
    if (pending.clip == nullptr || activeMask == nullptr) return;
    VoiceSlot* slot = acquireVoice();
    ++g_sequence;
    if (g_sequence == 0) ++g_sequence;
    *slot = {};
    slot->clip = pending.clip;
    slot->sequence = g_sequence;
    slot->gain = kBaseGain *
                 std::clamp(pending.spatial.gain, 0.0f, 1.0f);
    slot->pan = std::clamp(pending.spatial.pan, -1.0f, 1.0f);
    slot->lowPass = std::clamp(pending.spatial.lowPass, 0.0f, 1.0f);
    slot->toneCharacter =
        std::clamp(pending.spatial.toneCharacter, 0.0f, 2.0f);
    const std::size_t slotIndex =
        static_cast<std::size_t>(slot - g_voices.data());
    *activeMask |= std::uint32_t{1} << slotIndex;
}

void drainPendingVoices(std::uint32_t* activeMask) noexcept {
    if (activeMask == nullptr) return;
    EnterCriticalSection(&g_pendingVoiceLock);
    const std::size_t drained = g_pendingVoiceCount;
    for (std::size_t index = 0; index < drained; ++index) {
        startVoice(g_pendingVoices[g_pendingVoiceRead], activeMask);
        g_pendingVoices[g_pendingVoiceRead] = {};
        g_pendingVoiceRead = (g_pendingVoiceRead + 1u) % kPendingVoiceCount;
    }
    g_pendingVoiceCount = 0;
    LeaveCriticalSection(&g_pendingVoiceLock);
    if (drained != 0) {
        g_voiceState.fetch_sub(
            static_cast<std::uint64_t>(drained) * kPendingPlayIncrement,
            std::memory_order_release);
    }
}

void clearPendingVoices() noexcept {
    for (auto& pending : g_pendingVoices) pending = {};
    g_pendingVoiceRead = 0;
    g_pendingVoiceCount = 0;
}

void replaceActiveVoiceMask(std::uint32_t activeMask) noexcept {
    std::uint64_t state = g_voiceState.load(std::memory_order_relaxed);
    std::uint64_t replacement = 0;
    do {
        replacement = (state & ~kActiveVoiceMaskBits) | activeMask;
    } while (!g_voiceState.compare_exchange_weak(
        state, replacement, std::memory_order_release,
        std::memory_order_relaxed));
}

void addStereo(float value, float pan, std::uint32_t channels,
               float output[kMaxOutputChannels], bool wet) noexcept {
    if (channels == 1) {
        output[0] += value;
        return;
    }
    const float p = std::clamp(pan, -1.0f, 1.0f);
    const float left = p <= 0.0f ? 1.0f : 1.0f - p;
    const float right = p >= 0.0f ? 1.0f : 1.0f + p;
    output[0] += value * left;
    output[1] += value * right;
    if (!wet) return;
    if (channels == 4) {
        output[2] += value * left * 0.82f;
        output[3] += value * right * 0.82f;
    } else if (channels >= 6) {
        output[2] += value * (left + right) * 0.09f;
        output[4] += value * left * 0.82f;
        output[5] += value * right * 0.82f;
        if (channels >= 8) {
            output[6] += value * left * 0.62f;
            output[7] += value * right * 0.62f;
        }
    }
}

float shapeToneSampleCached(float source, float* lowBandState,
                            float toneCharacter, float alpha) noexcept {
    if (lowBandState == nullptr) return source;
    const float normalized = toneCharacter - 1.0f;
    if (normalized == 0.0f) return source;
    *lowBandState += alpha * (source - *lowBandState);
    const float low = *lowBandState;
    const float high = source - low;
    if (normalized < 0.0f) {
        const float amount = -normalized;
        return source + amount * kVehicleToneDarkStrength *
                            (0.55f * low - 0.70f * high);
    }
    return source + normalized *
                        (kVehicleToneBrightHighStrength * high -
                         kVehicleToneBrightLowCut * low);
}

void refreshVoiceRenderCache(VoiceSlot* voice,
                             std::uint32_t sampleRate) noexcept {
    if (voice == nullptr || voice->clip == nullptr || sampleRate == 0 ||
        (voice->outputSampleRate == sampleRate && voice->sourceStep > 0.0)) {
        return;
    }
    if (voice->outputSampleRate != 0 &&
        voice->outputSampleRate != sampleRate) {
        const double seconds = static_cast<double>(voice->outputFrame) /
                               voice->outputSampleRate;
        voice->outputFrame = static_cast<std::uint64_t>(seconds * sampleRate);
    }
    voice->outputSampleRate = sampleRate;
    const Clip& clip = *voice->clip;
    voice->sourceStep = static_cast<double>(clip.format.nSamplesPerSec) *
                        kTonePitchRatio *
                        testing::tonePitchMultiplier(voice->toneCharacter) /
                        sampleRate;
    const std::size_t sourceFrames =
        clip.samples.size() / clip.format.nBlockAlign;
    voice->directFrames = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(sourceFrames) / voice->sourceStep));
    voice->lastDelay = static_cast<std::uint64_t>(
        std::ceil(kReflectionTaps.back().delaySeconds * sampleRate));
    for (std::size_t index = 0; index < kReflectionTaps.size(); ++index) {
        voice->tapDelays[index] = static_cast<std::uint64_t>(std::lround(
            kReflectionTaps[index].delaySeconds * sampleRate));
    }
    voice->lowPassAlpha = std::clamp(
        std::min(voice->lowPass, kToneLowPassCeiling), 0.02f, 1.0f);
    voice->toneLowPassAlpha = 1.0f - std::exp(
        -2.0f * 3.14159265358979323846f * kVehicleToneCutoffHz /
        static_cast<float>(sampleRate));
    voice->toneGainCompensation =
        testing::toneGainCompensation(voice->toneCharacter);
    voice->wetTone =
        (0.55f + 0.45f * std::clamp(voice->lowPass, 0.0f, 1.0f)) *
        testing::toneWetMultiplier(voice->toneCharacter);
}

bool renderVoiceFrame(VoiceSlot* voice, std::uint32_t channels,
                      std::uint32_t sampleRate,
                      float output[kMaxOutputChannels]) noexcept {
    if (voice == nullptr || voice->clip == nullptr) return false;
    refreshVoiceRenderCache(voice, sampleRate);
    const Clip& clip = *voice->clip;
    if (voice->outputFrame >= voice->directFrames + voice->lastDelay) {
        *voice = {};
        return false;
    }
    if (voice->outputFrame < voice->directFrames) {
        const float rawSource = interpolatedMonoSample(
            clip, static_cast<double>(voice->outputFrame) * voice->sourceStep);
        const float source = shapeToneSampleCached(
            rawSource, &voice->toneLowBandState, voice->toneCharacter,
            voice->toneLowPassAlpha);
        voice->lowPassState += voice->lowPassAlpha *
                               (source - voice->lowPassState);
        addStereo(voice->lowPassState * voice->gain *
                      voice->toneGainCompensation,
                  voice->pan * kDirectPanScale, channels, output, false);
    }
    for (std::size_t index = 0; index < kReflectionTaps.size(); ++index) {
        const auto& tap = kReflectionTaps[index];
        const std::uint64_t delay = voice->tapDelays[index];
        if (voice->outputFrame < delay) continue;
        const std::uint64_t delayedFrame = voice->outputFrame - delay;
        if (delayedFrame >= voice->directFrames) continue;
        const float rawReflected = interpolatedMonoSample(
            clip, static_cast<double>(delayedFrame) * voice->sourceStep);
        const float reflected = shapeToneSampleCached(
            rawReflected, &voice->reflectedToneLowBandStates[index],
            voice->toneCharacter, voice->toneLowPassAlpha);
        const float wetValue = reflected * voice->gain *
                               voice->toneGainCompensation * kReverbGain *
                               tap.gain * voice->wetTone;
        const float wetPan = std::clamp(
            voice->pan * kWetPanScale + tap.spread, -1.0f, 1.0f);
        addStereo(wetValue, wetPan, channels, output, true);
    }
    ++voice->outputFrame;
    return true;
}

}  // namespace

float testing::tonePitchMultiplier(float toneCharacter) noexcept {
    const float normalized = std::clamp(toneCharacter, 0.0f, 2.0f) - 1.0f;
    if (normalized < 0.0f)
        return 1.0f + normalized * kVehicleToneDarkPitchRange;
    return 1.0f + normalized * kVehicleToneBrightPitchRange;
}

float testing::toneGainCompensation(float toneCharacter) noexcept {
    const float darkAmount =
        std::max(1.0f - std::clamp(toneCharacter, 0.0f, 2.0f), 0.0f);
    const float existing = 1.0f + kVehicleToneDarkGainLinear * darkAmount +
                           kVehicleToneDarkGainCurve * darkAmount * darkAmount;
    const float current = existing *
        (1.0f + kVehicleToneDarkExtraGain * darkAmount * darkAmount);
    return 1.0f +
           kVehicleToneDarkCompensationScale * (current - 1.0f);
}

float testing::toneWetMultiplier(float toneCharacter) noexcept {
    const float normalized =
        std::clamp(toneCharacter, 0.0f, 2.0f) - 1.0f;
    if (normalized < 0.0f)
        return 1.0f + normalized * kVehicleToneDarkWetReduction;
    return 1.0f + normalized * 0.30f;
}

float testing::shapeToneSample(float source, float* lowBandState,
                               float toneCharacter,
                               std::uint32_t sampleRate) noexcept {
    if (lowBandState == nullptr || sampleRate == 0) return source;
    const float alpha = 1.0f - std::exp(
        -2.0f * 3.14159265358979323846f * kVehicleToneCutoffHz /
        static_cast<float>(sampleRate));
    return shapeToneSampleCached(source, lowBandState,
        std::clamp(toneCharacter, 0.0f, 2.0f), alpha);
}

float testing::clipEnvelope(std::size_t frame, std::size_t frameCount,
                            std::uint32_t sampleRate,
                            bool mainClip) noexcept {
    if (frameCount == 0 || frame >= frameCount || sampleRate == 0) return 0.0f;
    const std::size_t fadeInFrames = std::min(
        frameCount, static_cast<std::size_t>(sampleRate * kFadeInSeconds));
    const std::size_t fadeOutFrames = mainClip
        ? frameCount - static_cast<std::size_t>(
              frameCount * kMainFadeOutStartRatio)
        : std::min(frameCount, static_cast<std::size_t>(
              sampleRate * kFadeOutSeconds));
    float envelope = 1.0f;
    if (fadeInFrames > 1 && frame < fadeInFrames) {
        const float phase = static_cast<float>(frame) /
                            static_cast<float>(fadeInFrames - 1u);
        envelope *= std::sin(phase * 1.57079632679489661923f);
    }
    if (fadeOutFrames > 1 && frame >= frameCount - fadeOutFrames) {
        const float phase = static_cast<float>(frameCount - 1u - frame) /
                            static_cast<float>(fadeOutFrames - 1u);
        envelope *= std::sin(phase * 1.57079632679489661923f);
    }
    return envelope;
}

bool initialize(const char* modulePath, const char* audioManifestPath) noexcept {
    if (g_initialized) return g_ready;
    g_initialized = true;
    if (!g_lockInitialized) {
        InitializeCriticalSection(&g_voiceLock);
        InitializeCriticalSection(&g_pendingVoiceLock);
        g_lockInitialized = true;
    }
    nfsmw_exhaust::AudioBank audioBank;
    std::string manifestError;
    if (!audioBank.loadManifest(audioManifestPath, &manifestError)) {
        native_log::write("AUDIO_INIT_FAIL manifest='%s' reason='%s'",
            audioManifestPath == nullptr ? "<null>" : audioManifestPath,
            manifestError.c_str());
        shutdown();
        return false;
    }
    for (std::size_t clipIndex = 0; clipIndex < kClipCount; ++clipIndex) {
        const char* assetId = audioBank.assetId(clipIndex);
        Clip& clip = g_clips[clipIndex];
        clip.assetId = assetId;
        char path[MAX_PATH] = {};
        if (!resolveAssetPath(modulePath, assetId, path) ||
            !loadWave(path, &clip, true)) {
            native_log::write("AUDIO_INIT_FAIL invalid WAV asset='%s'", assetId);
            shutdown();
            return false;
        }
    }
    g_backgroundIn.assetId = kBackgroundInAssetId;
    g_backgroundOut.assetId = kBackgroundOutAssetId;
    char backgroundInPath[MAX_PATH] = {};
    char backgroundOutPath[MAX_PATH] = {};
    if (!resolveAssetPath(modulePath, kBackgroundInAssetId, backgroundInPath) ||
        !resolveAssetPath(modulePath, kBackgroundOutAssetId, backgroundOutPath) ||
        !loadWave(backgroundInPath, &g_backgroundIn, false) ||
        !loadWave(backgroundOutPath, &g_backgroundOut, false)) {
        native_log::write("AUDIO_INIT_FAIL invalid IN/OUT background assets");
        shutdown();
        return false;
    }
    for (auto& clip : g_clips) {
        if (!mixBackgroundPair(&clip, g_backgroundIn, g_backgroundOut) ||
            !rebuildMonoSamples(&clip)) {
            native_log::write("AUDIO_INIT_FAIL background format mismatch asset='%s'",
                              clip.assetId.c_str());
            shutdown();
            return false;
        }
    }
    g_ready = true;
    g_playbackAllowed.store(true, std::memory_order_release);
    const auto& format = g_clips[0].format;
    native_log::write(
        "AUDIO_INIT_OK backend=GamePCM clips=%u backgrounds=2 voices=%u queue=%u "
        "format=%uHz/%ubit/%uch spatial=1 volume=%.6f directPanScale=%.2f "
        "reverb=1 reverbGain=%.3f preset=OPEN_AIR "
        "tone=pitch%.2f/bass%.2f@%.0fHz/metal%.0fHz+%.1fdB/Q%.1f/"
        "ring%.0fHz+%.1fdB/Q%.1f/trim%.2f/lowpass%.2f/fade%.0fms+%.0fms "
        "mainFadeOutStart=%.0fpct background=in%.2f/out%.2f@50pct "
        "vehicleTone=darkx%.2f/pitch-%.1fpct/gain%.2f "
        "brightHighx%.2f/lowCut%.2f/pitch+%.1fpct",
        static_cast<unsigned>(g_clips.size()),
        static_cast<unsigned>(g_voices.size()),
        static_cast<unsigned>(g_pendingVoices.size()), format.nSamplesPerSec,
        format.wBitsPerSample, format.nChannels,
        static_cast<double>(kBaseGain), static_cast<double>(kDirectPanScale),
        static_cast<double>(kReverbGain), static_cast<double>(kTonePitchRatio),
        static_cast<double>(kBassBodyMix), static_cast<double>(kBassCutoffHz),
        static_cast<double>(kMetalCenterHz), static_cast<double>(kMetalGainDb),
        static_cast<double>(kMetalQ), static_cast<double>(kMetalRingCenterHz),
        static_cast<double>(kMetalRingGainDb), static_cast<double>(kMetalRingQ),
        static_cast<double>(kToneOutputTrim),
        static_cast<double>(kToneLowPassCeiling),
        static_cast<double>(kFadeInSeconds * 1000.0f),
        static_cast<double>(kFadeOutSeconds * 1000.0f),
        static_cast<double>(kMainFadeOutStartRatio * 100.0f),
        static_cast<double>(kBackgroundInMix),
        static_cast<double>(kBackgroundOutMix),
        static_cast<double>(kVehicleToneDarkStrength),
        static_cast<double>(kVehicleToneDarkPitchRange * 100.0f),
        static_cast<double>((1.0f + kVehicleToneDarkGainLinear +
                             kVehicleToneDarkGainCurve) *
                            (1.0f + kVehicleToneDarkExtraGain)),
        static_cast<double>(kVehicleToneBrightHighStrength),
        static_cast<double>(kVehicleToneBrightLowCut),
        static_cast<double>(kVehicleToneBrightPitchRange * 100.0f));
    return true;
}

bool play(const char* assetId, const Spatialization& spatial) noexcept {
    if (!g_lockInitialized ||
        !g_playbackAllowed.load(std::memory_order_acquire)) return false;
    EnterCriticalSection(&g_pendingVoiceLock);
    if (!g_ready ||
        !g_playbackAllowed.load(std::memory_order_acquire)) {
        LeaveCriticalSection(&g_pendingVoiceLock);
        return false;
    }
    const Clip* clip = findClip(assetId);
    if (clip == nullptr || g_pendingVoiceCount >= kPendingVoiceCount) {
        LeaveCriticalSection(&g_pendingVoiceLock);
        return false;
    }
    const std::size_t write =
        (g_pendingVoiceRead + g_pendingVoiceCount) % kPendingVoiceCount;
    g_pendingVoices[write] = PendingVoice{clip, spatial};
    ++g_pendingVoiceCount;
    g_voiceState.fetch_add(kPendingPlayIncrement, std::memory_order_release);
    LeaveCriticalSection(&g_pendingVoiceLock);
    return true;
}

void mixIntoGameBuffer(std::int16_t* samples, std::size_t frameCount,
                       std::uint32_t channels,
                       std::uint32_t sampleRate) noexcept {
    if (!g_lockInitialized ||
        !g_playbackAllowed.load(std::memory_order_acquire) ||
        samples == nullptr || frameCount == 0 ||
        channels == 0 || channels > kMaxOutputChannels ||
        sampleRate < 8000 || sampleRate > 192000) return;
    if (g_voiceState.load(std::memory_order_acquire) == 0) return;
    EnterCriticalSection(&g_voiceLock);
    if (!g_ready ||
        !g_playbackAllowed.load(std::memory_order_acquire)) {
        LeaveCriticalSection(&g_voiceLock);
        return;
    }
    const std::uint64_t voiceState =
        g_voiceState.load(std::memory_order_relaxed);
    std::uint32_t activeMask =
        static_cast<std::uint32_t>(voiceState & kActiveVoiceMaskBits);
    drainPendingVoices(&activeMask);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        if (activeMask == 0) break;
        float plugin[kMaxOutputChannels]{};
        bool active = false;
        std::uint32_t remainingVoices = activeMask;
        while (remainingVoices != 0) {
            unsigned long index = 0;
            _BitScanForward(&index, remainingVoices);
            const std::uint32_t bit = std::uint32_t{1} << index;
            remainingVoices &= ~bit;
            if (renderVoiceFrame(&g_voices[index], channels, sampleRate,
                                 plugin)) {
                active = true;
            } else {
                activeMask &= ~bit;
            }
        }
        if (!active) break;
        const std::size_t base = frame * channels;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            if (std::fabs(plugin[channel]) < 0.0000001f) continue;
            const float game = static_cast<float>(samples[base + channel]) / 32768.0f;
            const float mixed = std::clamp(game + plugin[channel], -1.0f, 1.0f);
            samples[base + channel] = static_cast<std::int16_t>(std::lround(
                mixed * (mixed < 0.0f ? 32768.0f : 32767.0f)));
        }
    }
    replaceActiveVoiceMask(activeMask);
    LeaveCriticalSection(&g_voiceLock);
}

void setPlaybackAllowed(bool allowed) noexcept {
    if (!g_lockInitialized) {
        g_playbackAllowed.store(false, std::memory_order_release);
        return;
    }

    // Close the gate before waiting for the mixer lock so a play request
    // cannot cross a pause or frontend transition boundary.
    if (!allowed) {
        g_playbackAllowed.store(false, std::memory_order_release);
    }
    EnterCriticalSection(&g_voiceLock);
    EnterCriticalSection(&g_pendingVoiceLock);
    if (allowed && g_ready) {
        g_playbackAllowed.store(true, std::memory_order_release);
    } else {
        g_playbackAllowed.store(false, std::memory_order_release);
        for (auto& voice : g_voices) voice = {};
        clearPendingVoices();
        g_voiceState.store(0, std::memory_order_release);
    }
    LeaveCriticalSection(&g_pendingVoiceLock);
    LeaveCriticalSection(&g_voiceLock);
}

void shutdown() noexcept {
    g_playbackAllowed.store(false, std::memory_order_release);
    if (g_lockInitialized) EnterCriticalSection(&g_voiceLock);
    if (g_lockInitialized) EnterCriticalSection(&g_pendingVoiceLock);
    g_ready = false;
    for (auto& voice : g_voices) voice = {};
    clearPendingVoices();
    g_voiceState.store(0, std::memory_order_release);
    for (auto& clip : g_clips) clip = {};
    g_backgroundIn = {};
    g_backgroundOut = {};
    g_sequence = 0;
    if (g_lockInitialized) {
        LeaveCriticalSection(&g_pendingVoiceLock);
        LeaveCriticalSection(&g_voiceLock);
    }
    g_initialized = false;
}

bool reverbReady() noexcept {
    return g_ready;
}

}  // namespace nfsmw_exhaust::native_audio
