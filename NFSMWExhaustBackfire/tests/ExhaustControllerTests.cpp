#include "nfsmw_exhaust/Config.hpp"
#include "nfsmw_exhaust/ConfigFile.hpp"
#include "nfsmw_exhaust/ExhaustController.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace nfsmw_exhaust;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expectNear(float actual, float expected, float tolerance,
                const char* message) {
    if (std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

struct RecordingBridge final : IGameBridge {
    std::vector<VehicleSnapshot> vehicles;
    std::vector<FlameRequest> flames;
    std::vector<AudioRequest> audio;
    std::vector<std::pair<VehicleId, bool>> vanilla;
    bool acceptFlames = true;
    std::size_t flameAttempts = 0;

    std::size_t collectVehicles(VehicleSnapshot* output,
                                std::size_t capacity) override {
        const std::size_t count = std::min(capacity, vehicles.size());
        for (std::size_t i = 0; i < count; ++i) output[i] = vehicles[i];
        return count;
    }

    bool spawnExhaustFlame(const FlameRequest& request) override {
        ++flameAttempts;
        if (!acceptFlames) return false;
        flames.push_back(request);
        return true;
    }

    void playBackfireAudio(const AudioRequest& request) override {
        audio.push_back(request);
    }

    void setVanillaExhaustEnabled(VehicleId vehicleId, bool enabled) override {
        vanilla.emplace_back(vehicleId, enabled);
    }

    void log(const char*) override {}

    void clearEffects() {
        flames.clear();
        audio.clear();
    }
};

VehicleSnapshot baseSnapshot() {
    VehicleSnapshot snapshot{};
    snapshot.id = 7;
    snapshot.valid = true;
    snapshot.rpm = 9000.0f;
    snapshot.maxRpm = 9000.0f;
    snapshot.redlineRpm = 8800.0f;
    snapshot.atMaxRpm = true;
    snapshot.leftExhaust.present = true;
    snapshot.leftExhaust.worldPosition.x = -1.0f;
    snapshot.rightExhaust.present = true;
    snapshot.rightExhaust.worldPosition.x = 1.0f;
    snapshot.gear = 1;
    return snapshot;
}

ExhaustConfig deterministicConfig() {
    ExhaustConfig config{};
    config.randomSeed = 0x12345678u;
    config.lowProbability = 1.0f;
    config.highProbability = 1.0f;
    config.upshiftProbabilityScale = 1.0f;
    config.downshiftProbabilityScale = 1.0f;
    config.pairedShiftMode = false;
    config.sustainedProbability = 1.0f;
    config.flameAudioProbability = 1.0f;
    config.nitrousStartProbability = 1.0f;
    config.nitrousEndProbability = 1.0f;
    config.normalize();
    return config;
}

void expectAudioMatchesFlames(const RecordingBridge& bridge) {
    expect(bridge.audio.size() == bridge.flames.size(),
           "every accepted flame must have one audio cue");
    for (std::size_t i = 0; i < bridge.flames.size(); ++i) {
        expect(bridge.audio[i].vehicleId == bridge.flames[i].vehicleId,
               "audio and flame vehicle ids must match");
        expect(bridge.audio[i].scheduledAtMs == bridge.flames[i].scheduledAtMs,
               "audio and flame schedule times must match");
        expect(bridge.audio[i].emittedAtMs == bridge.flames[i].emittedAtMs,
               "audio and flame emission times must match");
        expect(bridge.audio[i].side == bridge.flames[i].side,
               "audio and flame exhaust sides must match");
        expect(bridge.audio[i].marker.present,
               "audio must carry the selected exhaust marker");
        expect(bridge.flames[i].marker.worldOrientation.w == 1.0f &&
                   bridge.audio[i].marker.worldOrientation.w == 1.0f,
               "audio and flame must carry the marker orientation");
        expect(bridge.audio[i].cue.assetId != nullptr,
               "audio cue must have an asset identifier");
        expect(bridge.audio[i].cue.clipIndex < AudioBank::kClipCount,
               "audio clip must be one of the twelve flat slots");
    }
}

void testProbabilityAnchors() {
    ExhaustConfig config{};
    expectNear(triggerProbabilityForMaxRpm(6000.0f, config), 0.40f, 1e-6f,
               "rpm below 6500 must use 40 percent probability");
    expectNear(triggerProbabilityForMaxRpm(6500.0f, config), 0.40f, 1e-6f,
               "6500 rpm must be the low probability anchor");
    expectNear(triggerProbabilityForMaxRpm(7750.0f, config), 0.65f, 1e-6f,
               "mid-range rpm probability must be linearly interpolated");
    expectNear(triggerProbabilityForMaxRpm(9000.0f, config), 0.90f, 1e-6f,
               "9000 rpm must be the high probability anchor");
    expectNear(triggerProbabilityForMaxRpm(10000.0f, config), 0.90f, 1e-6f,
               "rpm above 9000 must use 90 percent probability");
    expectNear(triggerProbabilityForMaxRpm(0.0f, config), 0.0f, 1e-6f,
               "invalid maximum rpm must not trigger");
    expectNear(shiftTriggerProbabilityForMaxRpm(
                   9500.0f, ShiftDirection::Up, config),
               0.45f, 1e-6f,
               "a high-rev upshift must use 50 percent of base probability");
    expectNear(shiftTriggerProbabilityForMaxRpm(
                   9500.0f, ShiftDirection::Down, config),
               0.90f, 1e-6f,
               "a high-rev downshift must use full base probability");
}

void testAllAudioSlotsAreReachable() {
    AudioBank audioBank;
    for (std::uint32_t value = 0; value < AudioBank::kClipCount; ++value) {
        const AudioCue cue = audioBank.at(value);
        expect(cue.clipIndex == value,
               "audio selection must reach each flat clip slot");
        expect(cue.assetId != nullptr && cue.assetId[0] != '\0',
               "every reserved audio slot must have an asset ID");
    }
    expect(audioBank.at(AudioBank::kClipCount).assetId == nullptr,
           "an out-of-range flat clip slot must be rejected");
}

void testShippedConfiguration(const char* sourceDirectory) {
    const std::string root = sourceDirectory == nullptr ? "." : sourceDirectory;
    ExhaustConfig config{};
    std::string error;
    expect(loadConfigFile((root + "/config/NFSMWExhaustBackfire.ini").c_str(),
                          &config, &error),
           "the shipped core configuration must load");
    expect(config.burstMinShots == 4 && config.burstMaxShots == 8,
           "the shipped burst configuration must preserve 4-8 shots");
    expect(config.downshiftBurstMinShots == 2 &&
               config.downshiftBurstMaxShots == 4,
           "the shipped downshift burst must contain 2-4 shots");
    expect(config.burstWindowMs == 500,
           "the shipped burst window must be 500 ms");
    expect(config.sustainedThresholdMs == 1000,
           "the shipped sustained threshold must be one second");
    expect(config.sustainedMinIntervalMs == 275 &&
               config.sustainedMaxIntervalMs == 750,
           "the shipped sustained density must be reduced by 80 percent");
    expectNear(config.sustainedProbability, 0.20f, 1e-6f,
                "the shipped sustained attempt probability must be 20 percent");
    expectNear(config.neutralSustainedProbability, 0.45f, 1e-6f,
               "neutral throttle must use a 45 percent sustained probability");
    expectNear(config.neutralMinGasInput, 0.25f, 1e-6f,
               "neutral boost must require 25 percent throttle");
    expectNear(config.flameAudioProbability, 0.30f, 1e-6f,
               "each accepted flame must use a 30 percent audio chance");
    expect(config.pairedShiftMode,
           "the shipped configuration must use paired shift mode");
    expectNear(config.shiftSimultaneousProbability, 0.225f, 1e-6f,
               "upshifts must use 22.5 percent simultaneous probability");
    expectNear(config.shiftSequentialProbability, 0.175f, 1e-6f,
               "upshifts must use 17.5 percent sequential probability");
    expectNear(config.pairedDownshiftProbability, 0.80f, 1e-6f,
               "paired downshifts must use a fixed 80 percent probability");
    expect(config.pairedSideDelayMs == 300,
           "paired sequential events must use a 300 ms delay");
    expectNear(config.nitrousStartProbability, 0.30f, 1e-6f,
               "NOS activation probability must be 30 percent");
    expectNear(config.nitrousEndProbability, 0.35f, 1e-6f,
               "NOS end probability must be 35 percent");
    expectNear(config.upshiftProbabilityScale, 0.50f, 1e-6f,
               "the shipped upshift probability scale must be 50 percent");
    expectNear(config.downshiftProbabilityScale, 1.0f, 1e-6f,
               "the shipped downshift probability scale must be 100 percent");
    expectNear(config.downshiftMinRpmRatio, 0.70f, 1e-6f,
               "the shipped downshift RPM floor must be 70 percent");
    expectNear(config.downshiftMinControlInput, 0.05f, 1e-6f,
               "the legacy downshift input setting must remain parseable");
    expect(config.requireBothMarkers,
           "the shipped configuration must require both markers");

    AudioBank audioBank;
    expect(audioBank.loadManifest((root + "/config/BackfireAudio.ini").c_str(),
                                  &error),
           "the shipped twelve-slot audio manifest must load");
    expect(std::string(audioBank.assetId(0)) ==
                "audio/backfire/g1_01.wav",
           "the first flat audio slot must map to the first WAV");
    expect(std::string(audioBank.assetId(11)) ==
                "audio/backfire/g4_03.wav",
           "the last flat audio slot must map to the twelfth WAV");

    const std::string firstAsset = audioBank.assetId(0);
    expect(!audioBank.loadManifest(
               (root + "/tests/data/IncompleteAudio.ini").c_str(), &error),
           "an incomplete audio manifest must fail");
    expect(!audioBank.loadManifest(
               (root + "/tests/data/DuplicateAudio.ini").c_str(), &error),
           "duplicate audio slots must fail");
    expect(std::string(audioBank.assetId(0)) == firstAsset,
           "a failed audio manifest must not partially modify the bank");

    const std::uint32_t originalBurstMinimum = config.burstMinShots;
    expect(!loadConfigFile((root + "/tests/data/InvalidConfig.ini").c_str(),
                           &config, &error),
           "negative unsigned configuration values must fail");
    expect(config.burstMinShots == originalBurstMinimum,
           "a failed configuration load must not partially apply values");
}

void testMarkerGate() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    snapshot.leftExhaust.present = false;
    snapshot.rightExhaust.present = false;
    snapshot.driverControlsValid = true;
    controller.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.nitrousActive = true;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 1);
    controller.tick(&snapshot, 1, 600);
    expect(bridge.flames.empty(),
           "a vehicle without both exhaust markers must not emit flames");
    expect(bridge.audio.empty(),
           "a vehicle without both exhaust markers must not emit audio");
}

void testGlobalMinimumRpmGate() {
    ExhaustConfig config = deterministicConfig();
    config.pairedShiftMode = true;
    config.shiftSimultaneousProbability = 1.0f;
    config.shiftSequentialProbability = 0.0f;

    RecordingBridge floorBridge;
    ExhaustController floorController(floorBridge, config);
    VehicleSnapshot floor = baseSnapshot();
    floor.rpm = kMinimumBackfireRpm;
    floor.redlineRpm = kMinimumBackfireRpm;
    floor.maxRpm = kMinimumBackfireRpm;
    floor.atRedline = true;
    floor.atMaxRpm = true;
    floor.driverControlsValid = true;
    floorController.tick(&floor, 1, 0);
    floor.shiftEvent = true;
    floor.nitrousActive = true;
    floor.gear = 2;
    floorController.tick(&floor, 1, 10);
    floor.shiftEvent = false;
    floorController.tick(&floor, 1, 1200);
    expect(floorBridge.flames.empty(),
           "4000 RPM must block shift and sustained flames globally");
    expect(floorBridge.audio.empty(),
           "4000 RPM must block bound and NOS backfire audio globally");

    RecordingBridge aboveBridge;
    ExhaustController aboveController(aboveBridge, config);
    VehicleSnapshot above = baseSnapshot();
    above.rpm = kMinimumBackfireRpm + 1.0f;
    above.redlineRpm = above.rpm;
    above.maxRpm = above.rpm;
    above.atRedline = true;
    above.atMaxRpm = true;
    aboveController.tick(&above, 1, 0);
    above.shiftEvent = true;
    above.gear = 2;
    aboveController.tick(&above, 1, 10);
    expect(aboveBridge.flames.size() == 2,
           "RPM above 4000 must remain eligible for paired shift flames");
    expectAudioMatchesFlames(aboveBridge);

    RecordingBridge queuedBridge;
    ExhaustConfig queuedConfig = config;
    queuedConfig.shiftSimultaneousProbability = 0.0f;
    queuedConfig.shiftSequentialProbability = 1.0f;
    queuedConfig.pairedSideDelayMs = 100;
    ExhaustController queuedController(queuedBridge, queuedConfig);
    VehicleSnapshot queued = baseSnapshot();
    queuedController.tick(&queued, 1, 0);
    queued.shiftEvent = true;
    queued.gear = 2;
    queuedController.tick(&queued, 1, 10);
    expect(queuedBridge.flames.size() == 1,
           "sequential event must emit its first side above the RPM floor");
    queued.shiftEvent = false;
    queued.rpm = kMinimumBackfireRpm;
    queuedController.tick(&queued, 1, 50);
    queuedController.tick(&queued, 1, 110);
    expect(queuedBridge.flames.size() == 1,
           "dropping to 4000 RPM must cancel queued flame sides");
    expectAudioMatchesFlames(queuedBridge);
}

void testShiftBurstAndAudioPairing() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.burstMinShots = 4;
    config.burstMaxShots = 8;
    config.burstWindowMs = 500;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    controller.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 10);
    snapshot.shiftEvent = false;
    for (std::uint64_t nowMs = 11; nowMs <= 510; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }

    expect(bridge.flames.size() >= 4 && bridge.flames.size() <= 8,
           "a shift burst must contain between four and eight shots");
    for (std::size_t i = 0; i < bridge.flames.size(); ++i) {
        const FlameRequest& flame = bridge.flames[i];
        expect(flame.scheduledAtMs >= 10 && flame.scheduledAtMs <= 510,
               "burst shots must stay inside the 500 ms window");
        expect(flame.emittedAtMs <= 510,
               "burst shots must actually emit inside the 500 ms window");
        if (i != 0) {
            expect(bridge.flames[i - 1].emittedAtMs < flame.emittedAtMs,
                   "a vehicle must never emit two shots in one frame");
        }
        if (i != 0) {
            expect(bridge.flames[i - 1].scheduledAtMs < flame.scheduledAtMs,
                   "random burst shots must not be simultaneous");
        }
        expect(flame.marker.present, "burst shot must select a present marker");
        expect(std::fabs(flame.marker.worldPosition.x) == 1.0f,
               "burst shot must use either the left or right marker");
    }
    expectAudioMatchesFlames(bridge);
}

void testPairedShiftPatterns() {
    VehicleSnapshot snapshot = baseSnapshot();
    snapshot.driverControlsValid = true;
    snapshot.gasInput = 0.0f;

    RecordingBridge simultaneousBridge;
    ExhaustConfig simultaneousConfig = deterministicConfig();
    simultaneousConfig.pairedShiftMode = true;
    simultaneousConfig.shiftSimultaneousProbability = 1.0f;
    simultaneousConfig.shiftSequentialProbability = 0.0f;
    ExhaustController simultaneous(simultaneousBridge, simultaneousConfig);
    simultaneous.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    simultaneous.tick(&snapshot, 1, 10);
    expect(simultaneousBridge.flames.size() == 2,
           "an upshift must emit both sides even after the driver lifts");
    expect(simultaneousBridge.flames[0].emittedAtMs == 10 &&
               simultaneousBridge.flames[1].emittedAtMs == 10,
           "simultaneous paired sides must emit in the same frame");
    expect(simultaneousBridge.flames[0].side !=
               simultaneousBridge.flames[1].side,
           "a paired shift must include one left and one right side");
    expect(simultaneousBridge.flames[0].pattern ==
                   FlamePattern::Simultaneous &&
               simultaneousBridge.flames[1].pattern ==
                   FlamePattern::Simultaneous &&
               simultaneousBridge.flames[0].sequenceId != 0 &&
               simultaneousBridge.flames[0].sequenceId ==
                   simultaneousBridge.flames[1].sequenceId,
           "simultaneous sides must share pairing metadata");
    expectAudioMatchesFlames(simultaneousBridge);

    snapshot = baseSnapshot();
    snapshot.driverControlsValid = true;
    snapshot.gasInput = 1.0f;
    RecordingBridge sequentialBridge;
    ExhaustConfig sequentialConfig = deterministicConfig();
    sequentialConfig.pairedShiftMode = true;
    sequentialConfig.shiftSimultaneousProbability = 0.0f;
    sequentialConfig.shiftSequentialProbability = 1.0f;
    sequentialConfig.pairedSideDelayMs = 100;
    ExhaustController sequential(sequentialBridge, sequentialConfig);
    sequential.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    sequential.tick(&snapshot, 1, 10);
    expect(sequentialBridge.flames.size() == 1,
           "a sequential paired shift must emit its first side immediately");
    snapshot.shiftEvent = false;
    sequential.tick(&snapshot, 1, 109);
    expect(sequentialBridge.flames.size() == 1,
           "the second sequential side must wait 100 ms");
    sequential.tick(&snapshot, 1, 110);
    expect(sequentialBridge.flames.size() == 2,
           "the second sequential side must emit after 100 ms");
    expect(sequentialBridge.flames[0].side != sequentialBridge.flames[1].side,
           "sequential paired shifts must include both sides");
    expect(sequentialBridge.flames[0].pattern == FlamePattern::Sequential &&
               sequentialBridge.flames[1].pattern ==
                   FlamePattern::Sequential &&
               sequentialBridge.flames[0].sequenceId != 0 &&
               sequentialBridge.flames[0].sequenceId ==
                   sequentialBridge.flames[1].sequenceId,
           "sequential sides must share pairing metadata");
    expectAudioMatchesFlames(sequentialBridge);

    snapshot = baseSnapshot();
    snapshot.driverControlsValid = true;
    snapshot.gasInput = 1.0f;
    RecordingBridge noneBridge;
    ExhaustConfig noneConfig = deterministicConfig();
    noneConfig.pairedShiftMode = true;
    noneConfig.shiftSimultaneousProbability = 0.0f;
    noneConfig.shiftSequentialProbability = 0.0f;
    ExhaustController none(noneBridge, noneConfig);
    none.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    none.tick(&snapshot, 1, 10);
    expect(noneBridge.flames.empty() && noneBridge.audio.empty(),
           "the no-event paired outcome must stay silent");

    snapshot = baseSnapshot();
    snapshot.gear = 3;
    snapshot.driverControlsValid = true;
    RecordingBridge coastBridge;
    ExhaustConfig coastConfig = deterministicConfig();
    coastConfig.pairedShiftMode = true;
    coastConfig.shiftSimultaneousProbability = 1.0f;
    coastConfig.shiftSequentialProbability = 0.0f;
    ExhaustController coast(coastBridge, coastConfig);
    coast.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    coast.tick(&snapshot, 1, 10);
    expect(coastBridge.flames.size() == 2,
           "a zero-throttle downshift above 4000 RPM must remain eligible");
    expectAudioMatchesFlames(coastBridge);
}

void testFlameAudioProbabilityIsIndependent() {
    VehicleSnapshot snapshot = baseSnapshot();

    RecordingBridge silentBridge;
    ExhaustConfig silentConfig = deterministicConfig();
    silentConfig.pairedShiftMode = true;
    silentConfig.shiftSimultaneousProbability = 1.0f;
    silentConfig.shiftSequentialProbability = 0.0f;
    silentConfig.flameAudioProbability = 0.0f;
    ExhaustController silent(silentBridge, silentConfig);
    silent.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    silent.tick(&snapshot, 1, 10);
    expect(silentBridge.flames.size() == 2,
           "disabling flame audio must not suppress accepted flames");
    expect(silentBridge.audio.empty(),
           "zero flame-audio probability must keep accepted flames silent");

    snapshot = baseSnapshot();
    RecordingBridge audibleBridge;
    ExhaustConfig audibleConfig = silentConfig;
    audibleConfig.flameAudioProbability = 1.0f;
    ExhaustController audible(audibleBridge, audibleConfig);
    audible.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    audible.tick(&snapshot, 1, 10);
    expectAudioMatchesFlames(audibleBridge);
}

void testNitrousAudioEdgesOnly() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.nitrousStartProbability = 1.0f;
    config.nitrousEndProbability = 1.0f;
    config.nitrousSequentialProbability = 1.0f;
    config.pairedSideDelayMs = 100;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();
    snapshot.driverControlsValid = true;

    controller.tick(&snapshot, 1, 0);
    snapshot.nitrousActive = true;
    controller.tick(&snapshot, 1, 10);
    expect(bridge.audio.size() == 1,
           "NOS activation must emit the first audio side once");
    controller.tick(&snapshot, 1, 110);
    expect(bridge.audio.size() == 2,
           "NOS activation may emit the second side after 100 ms");
    controller.tick(&snapshot, 1, 500);
    expect(bridge.audio.size() == 2,
           "holding NOS must not repeat activation audio");
    expect(bridge.flames.empty(),
           "NOS edge audio must not create exhaust backfire flames");

    snapshot.nitrousActive = false;
    controller.tick(&snapshot, 1, 600);
    controller.tick(&snapshot, 1, 700);
    expect(bridge.audio.size() == 4,
           "NOS release must schedule one left and one right audio cue");
}

void testPairedDownshiftUsesGlobalRpmGateAndFixedProbability() {
    ExhaustConfig config = deterministicConfig();
    config.pairedShiftMode = true;
    config.shiftSimultaneousProbability = 1.0f;
    config.shiftSequentialProbability = 0.0f;
    config.pairedDownshiftProbability = 1.0f;

    RecordingBridge floorBridge;
    ExhaustController floorController(floorBridge, config);
    VehicleSnapshot floor = baseSnapshot();
    floor.rpm = kMinimumBackfireRpm;
    floor.atRedline = false;
    floor.atMaxRpm = false;
    floor.gear = 3;
    floor.driverControlsValid = true;
    floor.gasInput = 1.0f;
    floorController.tick(&floor, 1, 0);
    floor.shiftEvent = true;
    floor.shiftDirection = ShiftDirection::Down;
    floor.gear = 2;
    floorController.tick(&floor, 1, 10);
    expect(floorBridge.flames.empty() && floorBridge.audio.empty(),
           "a paired downshift at exactly 4000 RPM must remain blocked");

    RecordingBridge aboveBridge;
    ExhaustController aboveController(aboveBridge, config);
    VehicleSnapshot above = floor;
    above.rpm = kMinimumBackfireRpm + 0.1f;
    above.shiftEvent = false;
    above.shiftDirection = ShiftDirection::None;
    above.gear = 3;
    aboveController.tick(&above, 1, 0);
    above.shiftEvent = true;
    above.shiftDirection = ShiftDirection::Down;
    above.gear = 2;
    aboveController.tick(&above, 1, 10);
    expect(aboveBridge.flames.size() == 2 && aboveBridge.audio.size() == 2,
           "a deliberate paired downshift above 4000 RPM must use its fixed probability");

    config.pairedDownshiftProbability = 0.0f;
    RecordingBridge disabledBridge;
    ExhaustController disabledController(disabledBridge, config);
    above.shiftEvent = false;
    above.shiftDirection = ShiftDirection::None;
    above.gear = 3;
    disabledController.tick(&above, 1, 0);
    above.shiftEvent = true;
    above.shiftDirection = ShiftDirection::Down;
    above.gear = 2;
    disabledController.tick(&above, 1, 10);
    expect(disabledBridge.flames.empty() && disabledBridge.audio.empty(),
           "paired downshifts must honor their independent probability");
}

void testShiftUsesPreShiftRpm() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    controller.tick(&snapshot, 1, 0);
    snapshot.rpm = 5500.0f;
    snapshot.atMaxRpm = false;
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 10);
    snapshot.shiftEvent = false;
    for (std::uint64_t nowMs = 11; nowMs <= 510; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }

    expect(bridge.flames.size() >= 4 && bridge.flames.size() <= 8,
           "a shift must use the pre-shift redline state after RPM drops");
    expectAudioMatchesFlames(bridge);
}

void testDownshiftRpmThreshold() {
    ExhaustConfig config = deterministicConfig();
    config.downshiftBurstMinShots = 4;
    config.downshiftBurstMaxShots = 4;

    RecordingBridge belowBridge;
    ExhaustController belowController(belowBridge, config);
    VehicleSnapshot below = baseSnapshot();
    below.gear = 3;
    belowController.tick(&below, 1, 0);
    below.rpm = 6100.0f;
    below.atMaxRpm = false;
    below.shiftInProgress = true;
    below.shiftEvent = true;
    below.gearChanged = true;
    below.shiftDirection = ShiftDirection::Down;
    below.gear = 2;
    belowController.tick(&below, 1, 10);
    below.shiftEvent = false;
    below.gearChanged = false;
    below.shiftInProgress = false;
    for (std::uint64_t nowMs = 11; nowMs <= 510; ++nowMs) {
        belowController.tick(&below, 1, nowMs);
    }
    expect(belowBridge.flames.empty(),
           "a downshift below 70 percent of redline must not trigger");

    RecordingBridge aboveBridge;
    ExhaustController aboveController(aboveBridge, config);
    VehicleSnapshot above = baseSnapshot();
    above.gear = 3;
    aboveController.tick(&above, 1, 0);
    above.rpm = 6200.0f;
    above.atMaxRpm = false;
    above.shiftInProgress = true;
    above.shiftEvent = true;
    above.gearChanged = true;
    above.shiftDirection = ShiftDirection::Down;
    above.gear = 2;
    aboveController.tick(&above, 1, 10);
    above.shiftEvent = false;
    above.gearChanged = false;
    above.shiftInProgress = false;
    for (std::uint64_t nowMs = 11; nowMs <= 510; ++nowMs) {
        aboveController.tick(&above, 1, nowMs);
    }
    expect(aboveBridge.flames.size() == 4,
           "a downshift above 70 percent of redline must be eligible");
    expectAudioMatchesFlames(aboveBridge);
}

void testDownshiftUsesReducedBurstDensity() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.downshiftBurstMinShots = 2;
    config.downshiftBurstMaxShots = 4;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();
    snapshot.gear = 3;

    controller.tick(&snapshot, 1, 0);
    snapshot.rpm = 7000.0f;
    snapshot.atMaxRpm = false;
    snapshot.gearChanged = true;
    snapshot.shiftDirection = ShiftDirection::Down;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 10);
    snapshot.gearChanged = false;
    snapshot.shiftDirection = ShiftDirection::None;
    for (std::uint64_t nowMs = 11; nowMs <= 510; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }

    expect(bridge.flames.size() >= 2 && bridge.flames.size() <= 4,
           "a downshift burst must contain between two and four shots");
    expectAudioMatchesFlames(bridge);
}

void testZeroThrottleDownshiftRemainsEligible() {
    ExhaustConfig config = deterministicConfig();
    config.downshiftBurstMinShots = 2;
    config.downshiftBurstMaxShots = 2;

    RecordingBridge coastBridge;
    ExhaustController coastController(coastBridge, config);
    VehicleSnapshot coast = baseSnapshot();
    coast.gear = 3;
    coast.driverControlsValid = true;
    coastController.tick(&coast, 1, 0);
    coast.rpm = 7000.0f;
    coast.atMaxRpm = false;
    coast.gearChanged = true;
    coast.shiftDirection = ShiftDirection::Down;
    coast.gear = 2;
    coastController.tick(&coast, 1, 10);
    coast.gearChanged = false;
    coast.shiftDirection = ShiftDirection::None;
    for (std::uint64_t nowMs = 11; nowMs <= 510; ++nowMs) {
        coastController.tick(&coast, 1, nowMs);
    }
    expect(coastBridge.flames.size() == 2,
           "a zero-input downshift above the RPM threshold must trigger");
    expectAudioMatchesFlames(coastBridge);

    RecordingBridge intentBridge;
    ExhaustController intentController(intentBridge, config);
    VehicleSnapshot intent = baseSnapshot();
    intent.gear = 3;
    intent.driverControlsValid = true;
    intent.brakeInput = 0.25f;
    intentController.tick(&intent, 1, 0);
    intent.rpm = 7000.0f;
    intent.atMaxRpm = false;
    intent.gearChanged = true;
    intent.shiftDirection = ShiftDirection::Down;
    intent.gear = 2;
    intentController.tick(&intent, 1, 10);
    intent.gearChanged = false;
    intent.shiftDirection = ShiftDirection::None;
    for (std::uint64_t nowMs = 11; nowMs <= 510; ++nowMs) {
        intentController.tick(&intent, 1, nowMs);
    }
    expect(intentBridge.flames.size() == 2,
           "a deliberate brake downshift must remain eligible");
    expectAudioMatchesFlames(intentBridge);
}

void testDelayedDirectionIsLatchedOnce() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.downshiftBurstMinShots = 4;
    config.downshiftBurstMaxShots = 4;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();
    snapshot.gear = 3;

    controller.tick(&snapshot, 1, 0);
    snapshot.rpm = 6200.0f;
    snapshot.atMaxRpm = false;
    snapshot.shiftInProgress = true;
    snapshot.shiftEvent = true;
    controller.tick(&snapshot, 1, 10);

    snapshot.shiftEvent = false;
    snapshot.gearChanged = true;
    snapshot.shiftDirection = ShiftDirection::Down;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 11);
    snapshot.gearChanged = false;
    snapshot.shiftDirection = ShiftDirection::None;
    snapshot.shiftInProgress = false;
    for (std::uint64_t nowMs = 12; nowMs <= 511; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }

    expect(bridge.flames.size() == 4,
           "a delayed downshift direction must produce exactly one burst");
    expectAudioMatchesFlames(bridge);
}

void testMultiFrameShiftLatchesPreShiftRpm() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.burstMinShots = 4;
    config.burstMaxShots = 4;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    controller.tick(&snapshot, 1, 0);
    snapshot.rpm = 5500.0f;
    snapshot.atMaxRpm = false;
    snapshot.shiftInProgress = true;
    controller.tick(&snapshot, 1, 10);
    snapshot.gearChanged = true;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 100);
    snapshot.gearChanged = false;
    snapshot.shiftInProgress = false;
    for (std::uint64_t nowMs = 101; nowMs <= 600; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }

    expect(bridge.flames.size() == 4,
           "a multi-frame shift must retain the pre-shift limiter state");
    expectAudioMatchesFlames(bridge);
}

void testShiftCycleSignalsDoNotDoubleTrigger() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.burstMinShots = 4;
    config.burstMaxShots = 4;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    controller.tick(&snapshot, 1, 0);
    snapshot.shiftInProgress = true;
    snapshot.shiftEvent = true;
    controller.tick(&snapshot, 1, 10);
    snapshot.shiftEvent = false;
    for (std::uint64_t nowMs = 11; nowMs < 100; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }
    snapshot.gearChanged = true;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 100);
    snapshot.gearChanged = false;
    for (std::uint64_t nowMs = 101; nowMs < 200; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }
    snapshot.shiftInProgress = false;
    for (std::uint64_t nowMs = 200; nowMs <= 510; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }

    expect(bridge.flames.size() == 4,
           "multiple signals in one shift cycle must trigger only one burst");
}

void testSkewedShiftSignalsBelongToOneCycle() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.burstMinShots = 4;
    config.burstMaxShots = 4;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    controller.tick(&snapshot, 1, 0);

    /* The event leads the multi-frame shift by one frame. */
    snapshot.shiftEvent = true;
    controller.tick(&snapshot, 1, 10);
    snapshot.shiftEvent = false;
    snapshot.shiftInProgress = true;
    controller.tick(&snapshot, 1, 11);
    controller.tick(&snapshot, 1, 20);

    /* The gear confirmation trails the end of the shift by one frame. */
    snapshot.shiftInProgress = false;
    controller.tick(&snapshot, 1, 21);
    snapshot.gearChanged = true;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 22);
    snapshot.gearChanged = false;
    controller.tick(&snapshot, 1, 23);

    for (std::uint64_t nowMs = 24; nowMs <= 510; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }

    expect(bridge.flames.size() == 4,
           "skewed signals from one shift must produce only one burst");
    expectAudioMatchesFlames(bridge);
}

void testOverlappingShiftBurstsAreQueued() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.burstMinShots = 4;
    config.burstMaxShots = 4;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    controller.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 10);
    snapshot.shiftEvent = false;
    for (std::uint64_t nowMs = 11; nowMs < 100; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }
    snapshot.shiftEvent = true;
    snapshot.gear = 3;
    controller.tick(&snapshot, 1, 100);
    snapshot.shiftEvent = false;
    for (std::uint64_t nowMs = 101; nowMs <= 600; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }

    expect(bridge.flames.size() == 8,
           "a second qualifying shift must append its own four-shot burst");
    expectAudioMatchesFlames(bridge);
}

void testExpiredBurstDoesNotCreateLateTail() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    controller.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 10);
    snapshot.shiftEvent = false;
    const std::size_t emittedBeforeStall = bridge.flames.size();
    controller.tick(&snapshot, 1, 600);
    expect(bridge.flames.size() == emittedBeforeStall,
           "expired burst shots must not play as a late tail after a stall");
}

void testRejectedBurstShotRetriesUntilAccepted() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.burstMinShots = 4;
    config.burstMaxShots = 4;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    controller.tick(&snapshot, 1, 0);
    bridge.acceptFlames = false;
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 10);
    snapshot.shiftEvent = false;
    for (std::uint64_t nowMs = 11; nowMs <= 200; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }

    expect(bridge.flameAttempts > 0,
           "a due burst shot must be submitted while the bridge rejects it");
    expect(bridge.flames.empty(),
           "rejected flame attempts must not count as successful shots");
    expect(bridge.audio.empty(),
           "rejected flame attempts must not play audio");

    bridge.acceptFlames = true;
    for (std::uint64_t nowMs = 201; nowMs <= 510; ++nowMs) {
        controller.tick(&snapshot, 1, nowMs);
    }

    expect(bridge.flames.size() == 4,
           "all four queued shots must complete after the bridge recovers");
    expectAudioMatchesFlames(bridge);
}

void testSustainedHighRpm() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.sustainedThresholdMs = 1000;
    config.sustainedMinIntervalMs = 100;
    config.sustainedMaxIntervalMs = 100;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    controller.tick(&snapshot, 1, 0);
    controller.tick(&snapshot, 1, 999);
    expect(bridge.flames.empty(),
           "sustained mode must wait a full second before emitting");
    controller.tick(&snapshot, 1, 1000);
    expect(bridge.flames.size() == 1,
           "sustained mode must emit when the one-second threshold is reached");
    controller.tick(&snapshot, 1, 1099);
    expect(bridge.flames.size() == 1,
           "sustained interval must be respected");
    controller.tick(&snapshot, 1, 1100);
    expect(bridge.flames.size() == 2,
           "sustained mode must continue random emissions");
    expectAudioMatchesFlames(bridge);
}

void testSustainedAtRedlineBelowMaximum() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.sustainedThresholdMs = 1000;
    config.sustainedMinIntervalMs = 100;
    config.sustainedMaxIntervalMs = 100;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();
    snapshot.rpm = 8000.0f;
    snapshot.redlineRpm = 8000.0f;
    snapshot.maxRpm = 9000.0f;
    snapshot.atRedline = true;
    snapshot.atMaxRpm = false;

    controller.tick(&snapshot, 1, 0);
    controller.tick(&snapshot, 1, 1000);
    controller.tick(&snapshot, 1, 1100);
    expect(bridge.flames.size() == 2,
           "redline below max RPM must still sustain emissions");
    expectAudioMatchesFlames(bridge);
}

void testSustainedUsesTighterLimitThreshold() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();
    snapshot.atMaxRpm = false;
    snapshot.atRedline = false;
    snapshot.redlineRpm = 0.0f;
    snapshot.rpm = 8900.0f;

    controller.tick(&snapshot, 1, 0);
    controller.tick(&snapshot, 1, 1100);
    expect(bridge.flames.empty(),
           "98.9 percent RPM must not count as continuously at the limiter");

    snapshot.rpm = 8955.0f;
    controller.tick(&snapshot, 1, 2000);
    controller.tick(&snapshot, 1, 3000);
    expect(bridge.flames.size() == 1,
           "99.5 percent RPM must arm sustained mode after one second");
}

void testSustainedStopsWhenLeavingLimit() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    config.sustainedThresholdMs = 1000;
    config.sustainedMinIntervalMs = 100;
    config.sustainedMaxIntervalMs = 100;
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    controller.tick(&snapshot, 1, 0);
    controller.tick(&snapshot, 1, 1000);
    expect(bridge.flames.size() == 1,
           "sustained mode must be active at the limiter");

    snapshot.atMaxRpm = false;
    snapshot.atRedline = false;
    snapshot.rpm = 8700.0f;
    controller.tick(&snapshot, 1, 1100);
    expect(bridge.flames.size() == 1,
           "sustained emissions must stop immediately below the limit");

    snapshot.atMaxRpm = true;
    snapshot.rpm = 9000.0f;
    controller.tick(&snapshot, 1, 1200);
    controller.tick(&snapshot, 1, 2199);
    expect(bridge.flames.size() == 1,
           "returning to the limiter must restart the one-second timer");
    controller.tick(&snapshot, 1, 2200);
    expect(bridge.flames.size() == 2,
           "sustained mode may resume after a new continuous second");
}

void testNeutralThrottleRaisesSustainedProbability() {
    ExhaustConfig config = deterministicConfig();
    config.sustainedThresholdMs = 1000;
    config.sustainedMinIntervalMs = 100;
    config.sustainedMaxIntervalMs = 100;
    config.sustainedProbability = 0.0f;
    config.neutralSustainedProbability = 1.0f;
    config.neutralMinGasInput = 0.25f;

    RecordingBridge neutralBridge;
    ExhaustController neutralController(neutralBridge, config);
    VehicleSnapshot neutral = baseSnapshot();
    neutral.gear = 1;
    neutral.driverControlsValid = true;
    neutral.gasInput = 1.0f;
    neutralController.tick(&neutral, 1, 0);
    neutralController.tick(&neutral, 1, 1000);
    expect(neutralBridge.flames.size() == 1,
           "neutral throttle must use the elevated sustained probability");

    RecordingBridge idleBridge;
    ExhaustController idleController(idleBridge, config);
    VehicleSnapshot idle = neutral;
    idle.gasInput = 0.0f;
    idleController.tick(&idle, 1, 0);
    idleController.tick(&idle, 1, 1000);
    expect(idleBridge.flames.empty(),
           "neutral without deliberate throttle must use normal probability");

    RecordingBridge drivingBridge;
    ExhaustController drivingController(drivingBridge, config);
    VehicleSnapshot driving = neutral;
    driving.gear = 2;
    drivingController.tick(&driving, 1, 0);
    drivingController.tick(&driving, 1, 1000);
    expect(drivingBridge.flames.empty(),
           "a driving gear must not receive the neutral probability boost");
}

void testVanillaSuppressionAndFailedSpawn() {
    RecordingBridge bridge;
    ExhaustConfig config = deterministicConfig();
    ExhaustController controller(bridge, config);
    VehicleSnapshot snapshot = baseSnapshot();

    bridge.acceptFlames = false;
    controller.tick(&snapshot, 0, 0);
    controller.tick(&snapshot, 1, 0);
    snapshot.shiftEvent = true;
    snapshot.gear = 2;
    controller.tick(&snapshot, 1, 1);
    controller.tick(&snapshot, 1, 600);
    expect(bridge.audio.empty(),
           "rejected flame requests must not play a corresponding sound");
    expect(!bridge.vanilla.empty() && !bridge.vanilla.back().second,
           "vanilla exhaust must be disabled while the replacement is active");
    controller.reset();
    expect(bridge.vanilla.back().second,
           "reset must restore the vanilla exhaust path");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        testProbabilityAnchors();
        testAllAudioSlotsAreReachable();
        testShippedConfiguration(argc > 1 ? argv[1] : ".");
        testMarkerGate();
        testGlobalMinimumRpmGate();
        testShiftBurstAndAudioPairing();
        testPairedShiftPatterns();
        testPairedDownshiftUsesGlobalRpmGateAndFixedProbability();
        testFlameAudioProbabilityIsIndependent();
        testNitrousAudioEdgesOnly();
        testShiftUsesPreShiftRpm();
        testDownshiftRpmThreshold();
        testDownshiftUsesReducedBurstDensity();
        testZeroThrottleDownshiftRemainsEligible();
        testDelayedDirectionIsLatchedOnce();
        testMultiFrameShiftLatchesPreShiftRpm();
        testShiftCycleSignalsDoNotDoubleTrigger();
        testSkewedShiftSignalsBelongToOneCycle();
        testOverlappingShiftBurstsAreQueued();
        testExpiredBurstDoesNotCreateLateTail();
        testRejectedBurstShotRetriesUntilAccepted();
        testSustainedHighRpm();
        testSustainedAtRedlineBelowMaximum();
        testSustainedUsesTighterLimitThreshold();
        testSustainedStopsWhenLeavingLimit();
        testNeutralThrottleRaisesSustainedProbability();
        testVanillaSuppressionAndFailedSpawn();
    } catch (const std::exception& error) {
        std::cerr << "NFSMWExhaustBackfire tests failed: " << error.what()
                  << '\n';
        return 1;
    }
    std::cout << "NFSMWExhaustBackfire tests passed\n";
    return 0;
}
