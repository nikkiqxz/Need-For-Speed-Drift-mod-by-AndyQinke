#include "nfsmw_exhaust/Config.hpp"

#include <algorithm>
#include <cmath>

namespace nfsmw_exhaust {

void ExhaustConfig::normalize() noexcept {
    burstMinShots = std::clamp(burstMinShots, 1u, 64u);
    burstMaxShots = std::clamp(burstMaxShots, burstMinShots, 64u);
    downshiftBurstMinShots =
        std::clamp(downshiftBurstMinShots, 1u, 64u);
    downshiftBurstMaxShots = std::clamp(
        downshiftBurstMaxShots, downshiftBurstMinShots, 64u);
    burstWindowMs = std::min(burstWindowMs, 60000u);
    if (burstWindowMs < burstMaxShots - 1u) {
        burstWindowMs = burstMaxShots - 1u;
    }

    sustainedThresholdMs =
        std::clamp(sustainedThresholdMs, 1u, 600000u);
    sustainedMinIntervalMs =
        std::clamp(sustainedMinIntervalMs, 1u, 60000u);
    sustainedMaxIntervalMs =
        std::clamp(sustainedMaxIntervalMs, 1u, 60000u);
    if (sustainedMaxIntervalMs < sustainedMinIntervalMs) {
        sustainedMaxIntervalMs = sustainedMinIntervalMs;
    }
    maxTrackedVehicles = std::clamp(maxTrackedVehicles, 1u, 4096u);

    if (!std::isfinite(lowMaxRpm) || lowMaxRpm < 0.0f) lowMaxRpm = 6500.0f;
    if (!std::isfinite(highMaxRpm) || highMaxRpm <= lowMaxRpm) {
        highMaxRpm = std::max(lowMaxRpm + 1.0f, 9000.0f);
    }

    if (!std::isfinite(lowProbability)) lowProbability = 0.40f;
    if (!std::isfinite(highProbability)) highProbability = 0.90f;
    lowProbability = std::clamp(lowProbability, 0.0f, 1.0f);
    highProbability = std::clamp(highProbability, 0.0f, 1.0f);
    if (highProbability < lowProbability) highProbability = lowProbability;

    if (!std::isfinite(upshiftProbabilityScale)) {
        upshiftProbabilityScale = 0.50f;
    }
    if (!std::isfinite(downshiftProbabilityScale)) {
        downshiftProbabilityScale = 1.0f;
    }
    upshiftProbabilityScale =
        std::clamp(upshiftProbabilityScale, 0.0f, 1.0f);
    downshiftProbabilityScale =
        std::clamp(downshiftProbabilityScale, 0.0f, 1.0f);

    if (!std::isfinite(nearLimitRatio)) nearLimitRatio = 0.985f;
    if (!std::isfinite(downshiftMinRpmRatio)) {
        downshiftMinRpmRatio = 0.70f;
    }
    if (!std::isfinite(sustainedLimitRatio)) sustainedLimitRatio = 0.995f;
    nearLimitRatio = std::clamp(nearLimitRatio, 0.50f, 1.0f);
    downshiftMinRpmRatio =
        std::clamp(downshiftMinRpmRatio, 0.10f, 1.0f);
    if (!std::isfinite(downshiftMinControlInput)) {
        downshiftMinControlInput = 0.05f;
    }
    downshiftMinControlInput =
        std::clamp(downshiftMinControlInput, 0.0f, 1.0f);
    sustainedLimitRatio =
        std::clamp(sustainedLimitRatio, nearLimitRatio, 1.0f);

    if (!std::isfinite(sustainedProbability)) sustainedProbability = 0.20f;
    if (!std::isfinite(neutralSustainedProbability)) {
        neutralSustainedProbability = 0.45f;
    }
    if (!std::isfinite(neutralMinGasInput)) neutralMinGasInput = 0.25f;
    if (!std::isfinite(flameAudioProbability)) {
        flameAudioProbability = 0.30f;
    }
    if (!std::isfinite(shiftSimultaneousProbability)) {
        shiftSimultaneousProbability = 0.225f;
    }
    if (!std::isfinite(shiftSequentialProbability)) {
        shiftSequentialProbability = 0.175f;
    }
    if (!std::isfinite(pairedDownshiftProbability)) {
        pairedDownshiftProbability = 0.80f;
    }
    if (!std::isfinite(nitrousStartProbability)) {
        nitrousStartProbability = 0.30f;
    }
    if (!std::isfinite(nitrousEndProbability)) {
        nitrousEndProbability = 0.35f;
    }
    if (!std::isfinite(nitrousSequentialProbability)) {
        nitrousSequentialProbability = 0.50f;
    }
    sustainedProbability = std::clamp(sustainedProbability, 0.0f, 1.0f);
    neutralSustainedProbability =
        std::clamp(neutralSustainedProbability, 0.0f, 1.0f);
    neutralMinGasInput = std::clamp(neutralMinGasInput, 0.0f, 1.0f);
    flameAudioProbability =
        std::clamp(flameAudioProbability, 0.0f, 1.0f);
    shiftSimultaneousProbability =
        std::clamp(shiftSimultaneousProbability, 0.0f, 1.0f);
    shiftSequentialProbability = std::clamp(
        shiftSequentialProbability, 0.0f, 1.0f - shiftSimultaneousProbability);
    pairedDownshiftProbability =
        std::clamp(pairedDownshiftProbability, 0.0f, 1.0f);
    pairedSideDelayMs = std::clamp(pairedSideDelayMs, 1u, 5000u);
    nitrousStartProbability =
        std::clamp(nitrousStartProbability, 0.0f, 1.0f);
    nitrousEndProbability =
        std::clamp(nitrousEndProbability, 0.0f, 1.0f);
    nitrousSequentialProbability =
        std::clamp(nitrousSequentialProbability, 0.0f, 1.0f);

    if (effectId.empty()) effectId = "exhaust_backfire";
    if (effectId.size() > 127u) effectId.resize(127u);
}

float triggerProbabilityForMaxRpm(float maxRpm,
                                  const ExhaustConfig& config) noexcept {
    if (!std::isfinite(maxRpm) || maxRpm <= 0.0f) return 0.0f;
    if (maxRpm <= config.lowMaxRpm) return config.lowProbability;
    if (maxRpm >= config.highMaxRpm) return config.highProbability;

    const float span = config.highMaxRpm - config.lowMaxRpm;
    if (span <= 0.0f) return config.highProbability;
    const float t = (maxRpm - config.lowMaxRpm) / span;
    return config.lowProbability +
           (config.highProbability - config.lowProbability) * t;
}

float shiftTriggerProbabilityForMaxRpm(
    float maxRpm, ShiftDirection direction,
    const ExhaustConfig& config) noexcept {
    const float base = triggerProbabilityForMaxRpm(maxRpm, config);
    const float scale = direction == ShiftDirection::Down
                            ? config.downshiftProbabilityScale
                            : config.upshiftProbabilityScale;
    return std::clamp(base * scale, 0.0f, 1.0f);
}

}  // namespace nfsmw_exhaust
