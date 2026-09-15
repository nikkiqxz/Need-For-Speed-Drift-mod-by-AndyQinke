#pragma once

#include <cstdint>

namespace nfsmw_drift_asi::driver_assist {

constexpr float kActivationDelayAfterDriftSeconds = 0.2f;
constexpr float kMinimumEscSpeedMps = 80.0f / 3.6f;
constexpr float kAbsBrakeThreshold = 0.5f;
constexpr float kAbsHoldSeconds = 0.5f;
constexpr float kAbsFadeSeconds = 3.0f;
constexpr float kAbsMinimumEnvelope = 0.2f;
constexpr float kAbsDriftDecelerationFraction = 0.5f;
constexpr float kLaunchControlMaximumSpeedMps = 10.0f / 3.6f;
constexpr float kLaunchControlThrottleThreshold = 0.5f;
constexpr float kLaunchControlBrakeThreshold = 0.5f;

constexpr float TcsProbabilityForGear(int gear) noexcept {
    switch (gear) {
        case 1: return 0.90f;
        case 2: return 0.80f;
        case 3: return 0.70f;
        case 4: return 0.60f;
        case 5: return 0.50f;
        case 6: return 0.40f;
        case 7: return 0.30f;
        default: return 0.0f;
    }
}

struct Input {
    float dt = 0.0f;
    bool driftActive = false;
    float speedMps = 0.0f;
    float steering = 0.0f;
    float brake = 0.0f;
    float throttle = 0.0f;
    float handbrake = 0.0f;
    float longitudinalSpeedMps = 0.0f;
    float bodyOffsetRad = 0.0f;
    float yawRateRadS = 0.0f;
    bool fourWheelSlip = false;
    int gear = 0;
};

struct Output {
    bool modeActive = false;
    bool absWorking = false;
    bool escStraightWorking = false;
    bool escRecoveryWorking = false;
    bool escWorking = false;
    bool tcsWorking = false;
    bool lcWorking = false;
    float absDecelerationScale = 0.0f;
    float yawVelocityDeltaRadS = 0.0f;
};

class Controller {
public:
    explicit Controller(std::uint32_t randomSeed = 0x53444641u) noexcept;

    Output Update(const Input& input) noexcept;
    void Reset(bool initiallyActive = true) noexcept;

private:
    void ResetOperationalState() noexcept;
    float NextRandomUnit() noexcept;

    std::uint32_t randomState_ = 0x53444641u;
    bool modeActive_ = true;
    bool waitingAfterDrift_ = false;
    float reactivationSeconds_ = 0.0f;

    float brakeHeldSeconds_ = 0.0f;
    float neutralSteeringSeconds_ = 0.0f;
    float straightRamp_ = 0.0f;

    int lastSteeringDirection_ = 0;
    int directionChanges_ = 0;
    float directionChangeWindowSeconds_ = 0.0f;
    bool recoveryActive_ = false;
    float recoverySeconds_ = 0.0f;
    float recoveryCenteredSeconds_ = 0.0f;
    float recoveryRamp_ = 0.0f;

    bool throttleEpisodeArmed_ = true;
    float tcsRemainingSeconds_ = 0.0f;
    bool lcActive_ = false;
    bool lcStartedWithBrake_ = false;
    bool lcStartedWithHandbrake_ = false;
};

}  // namespace nfsmw_drift_asi::driver_assist
