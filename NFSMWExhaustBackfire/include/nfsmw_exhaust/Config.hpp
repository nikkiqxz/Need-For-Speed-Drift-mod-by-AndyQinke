#pragma once

#include <cstdint>
#include <string>

#include "Types.hpp"

namespace nfsmw_exhaust {

struct ExhaustConfig {
    std::uint32_t burstMinShots = 4;
    std::uint32_t burstMaxShots = 8;
    std::uint32_t downshiftBurstMinShots = 2;
    std::uint32_t downshiftBurstMaxShots = 4;
    std::uint32_t burstWindowMs = 500;

    std::uint32_t sustainedThresholdMs = 1000;
    std::uint32_t sustainedMinIntervalMs = 275;
    std::uint32_t sustainedMaxIntervalMs = 750;

    float lowMaxRpm = 6500.0f;
    float highMaxRpm = 9000.0f;
    float lowProbability = 0.40f;
    float highProbability = 0.90f;
    float upshiftProbabilityScale = 0.50f;
    float downshiftProbabilityScale = 1.0f;
    float nearLimitRatio = 0.985f;
    float downshiftMinRpmRatio = 0.70f;
    float downshiftMinControlInput = 0.05f;
    float sustainedLimitRatio = 0.995f;
    float sustainedProbability = 0.20f;
    float neutralSustainedProbability = 0.45f;
    float neutralMinGasInput = 0.25f;
    float flameAudioProbability = 0.30f;

    bool pairedShiftMode = true;
    float shiftSimultaneousProbability = 0.225f;
    float shiftSequentialProbability = 0.175f;
    float pairedDownshiftProbability = 0.80f;
    std::uint32_t pairedSideDelayMs = 300;

    float nitrousStartProbability = 0.30f;
    float nitrousEndProbability = 0.35f;
    float nitrousSequentialProbability = 0.50f;

    bool requireBothMarkers = true;
    bool suppressVanilla = true;
    std::uint32_t maxTrackedVehicles = 128;
    std::uint32_t effectVariant = 0;
    std::uint32_t randomSeed = 0;

    std::string effectId = "exhaust_backfire";

    void normalize() noexcept;
};

/* Linear interpolation between the two requested RPM probability anchors. */
float triggerProbabilityForMaxRpm(float maxRpm,
                                  const ExhaustConfig& config) noexcept;

float shiftTriggerProbabilityForMaxRpm(
    float maxRpm, ShiftDirection direction,
    const ExhaustConfig& config) noexcept;

}  // namespace nfsmw_exhaust
