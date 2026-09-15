#include "asi_host/driver_assist.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using nfsmw_drift_asi::driver_assist::Controller;
using nfsmw_drift_asi::driver_assist::Input;

void Require(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

Input BaseInput() {
    Input input{};
    input.dt = 0.05f;
    input.speedMps = 30.0f;
    return input;
}

void TestDriftPriorityAndReactivationDelay() {
    Controller controller;
    Input input = BaseInput();
    Require(controller.Update(input).modeActive,
            "driver mode should be active before drifting");
    input.driftActive = true;
    Require(!controller.Update(input).modeActive,
            "drift must immediately suspend driver mode");
    input.driftActive = false;
    for (int i = 0; i < 3; ++i) {
        Require(!controller.Update(input).modeActive,
                "driver mode must wait 0.2 seconds after drift");
    }
    Require(controller.Update(input).modeActive,
            "driver mode must reactivate at 0.2 seconds");
}

void TestAbsDelayFadeAndInterrupt() {
    Controller controller;
    Input input = BaseInput();
    input.brake = 1.0f;
    for (int i = 0; i < 9; ++i) {
        Require(!controller.Update(input).absWorking,
                "ABS must not work before half a second");
    }
    auto output = controller.Update(input);
    Require(output.absWorking, "ABS should start at half a second");
    Require(std::fabs(output.absDecelerationScale - 0.5f) < 0.001f,
            "ABS should start at half drift deceleration");
    for (int i = 0; i < 60; ++i) output = controller.Update(input);
    Require(std::fabs(output.absDecelerationScale - 0.1f) < 0.002f,
            "ABS should fade to 20 percent of its starting force");
    input.brake = 0.0f;
    Require(!controller.Update(input).absWorking,
            "releasing brake must interrupt ABS immediately");
    input.brake = 1.0f;
    Require(!controller.Update(input).absWorking,
            "a new brake hold must restart the half-second delay");
}

void TestEscSpeedGateAndGentleStraightening() {
    Controller controller;
    Input input = BaseInput();
    input.speedMps = 20.0f;
    input.bodyOffsetRad = 0.1f;
    input.yawRateRadS = 0.2f;
    for (int i = 0; i < 60; ++i) {
        Require(!controller.Update(input).escWorking,
                "ESC must stay off below 80 km/h");
    }
    input.speedMps = 30.0f;
    for (int i = 0; i < 7; ++i) controller.Update(input);
    const auto output = controller.Update(input);
    Require(output.escStraightWorking,
            "straight-line ESC should fade in after neutral steering");
    Require(output.yawVelocityDeltaRadS < 0.0f &&
                std::fabs(output.yawVelocityDeltaRadS) < 0.01f,
            "straight-line ESC correction should be small and restorative");
}

void TestRapidDirectionRecoveryAndTcsOverride() {
    Controller controller;
    Input input = BaseInput();
    input.bodyOffsetRad = 9.0f * 3.14159265358979323846f / 180.0f;
    const float directions[] = {1.0f, -1.0f, 1.0f, -1.0f};
    for (float direction : directions) {
        input.steering = direction;
        controller.Update(input);
    }
    const auto output = controller.Update(input);
    Require(output.escRecoveryWorking,
            "three rapid direction changes with slip should trigger recovery");
    Require(output.tcsWorking,
            "ESC recovery must force the TCS indicator");
    Require(output.yawVelocityDeltaRadS < 0.0f,
            "recovery correction should oppose positive body offset");
}

void TestDirectEscRecoveryTriggers() {
    Controller controller;
    Input input = BaseInput();
    input.bodyOffsetRad = 10.1f * 3.14159265358979323846f / 180.0f;
    auto output = controller.Update(input);
    Require(output.escRecoveryWorking,
            "a body offset above ten degrees must directly trigger recovery");
    Require(output.yawVelocityDeltaRadS < 0.0f,
            "direct offset recovery must oppose the measured offset");

    controller.Reset();
    input.bodyOffsetRad = 0.05f;
    input.fourWheelSlip = true;
    output = controller.Update(input);
    Require(output.escRecoveryWorking,
            "validated four-wheel slip must directly trigger recovery");
}

void TestLaunchControlLatchAndRelease() {
    Controller controller;
    Input input = BaseInput();
    input.speedMps = 1.0f;
    input.longitudinalSpeedMps = 1.0f;
    input.throttle = 1.0f;
    input.handbrake = 1.0f;
    auto output = controller.Update(input);
    Require(output.lcWorking && !output.absWorking && !output.tcsWorking,
            "throttle plus handbrake below ten km/h must enter LC with priority");
    input.handbrake = 0.0f;
    output = controller.Update(input);
    Require(!output.lcWorking,
            "releasing the initiating handbrake must end LC immediately");

    input.throttle = 0.0f;
    controller.Update(input);
    input.throttle = 1.0f;
    input.brake = 1.0f;
    output = controller.Update(input);
    Require(output.lcWorking,
            "throttle plus foot brake below ten km/h must enter LC");
    input.throttle = 0.0f;
    input.brake = 0.0f;
    output = controller.Update(input);
    Require(!output.lcWorking,
            "releasing all controls must end LC immediately");
}

void TestLaunchControlEntryGates() {
    Controller controller;
    Input input = BaseInput();
    input.throttle = 1.0f;
    input.brake = 1.0f;
    input.speedMps = 10.0f / 3.6f;
    input.longitudinalSpeedMps = input.speedMps;
    Require(!controller.Update(input).lcWorking,
            "LC must require speed strictly below ten km/h");
    input.speedMps = 1.0f;
    input.longitudinalSpeedMps = -1.0f;
    Require(!controller.Update(input).lcWorking,
            "LC must not arm while reversing");
}

void TestTcsGearProbabilities() {
    using nfsmw_drift_asi::driver_assist::TcsProbabilityForGear;
    const float expected[] = {0.90f, 0.80f, 0.70f, 0.60f,
                              0.50f, 0.40f, 0.30f};
    for (int gear = 1; gear <= 7; ++gear) {
        Require(std::fabs(TcsProbabilityForGear(gear) -
                          expected[gear - 1]) < 1.0e-6f,
                "TCS probability table must match the seven requested gears");
    }
    Require(TcsProbabilityForGear(0) == 0.0f &&
                TcsProbabilityForGear(8) == 0.0f,
            "non-forward gears must have no random TCS episode");
}

void TestEscRecoveryStrengthAndSmoothAngleGain() {
    constexpr float kPi = 3.14159265358979323846f;
    Controller lowAngleController;
    Input low = BaseInput();
    low.bodyOffsetRad = 10.0f * kPi / 180.0f;
    const auto lowOutput = lowAngleController.Update(low);
    Require(lowOutput.escRecoveryWorking &&
                lowOutput.yawVelocityDeltaRadS < 0.0f,
            "ten-degree offset should begin restorative ESC recovery");

    Controller middleAngleController;
    Input middle = BaseInput();
    middle.bodyOffsetRad = 22.5f * kPi / 180.0f;
    const auto middleOutput = middleAngleController.Update(middle);

    Controller highAngleController;
    Input high = BaseInput();
    high.bodyOffsetRad = 35.0f * kPi / 180.0f;
    const auto highOutput = highAngleController.Update(high);

    const float lowStrength = std::fabs(lowOutput.yawVelocityDeltaRadS);
    const float middleStrength =
        std::fabs(middleOutput.yawVelocityDeltaRadS);
    const float highStrength = std::fabs(highOutput.yawVelocityDeltaRadS);
    const float expectedLowStrength =
        (0.48f * 1.15f) * (low.dt / 0.9f) * low.dt;
    Require(std::fabs(lowStrength - expectedLowStrength) < 1.0e-6f,
            "ESC recovery baseline must be fifteen percent stronger");
    Require(middleStrength > lowStrength &&
                middleStrength < highStrength,
            "ESC recovery authority must rise smoothly with body offset");
    Require(std::fabs(highStrength - 2.0f * lowStrength) < 1.0e-6f,
            "ESC recovery angle gain should reach two times at 35 degrees");
    Require(highStrength < 0.004f,
            "the first ESC recovery frame must remain gradual");
}

void TestSlowDirectionChangesDoNotTriggerRecovery() {
    Controller controller;
    Input input = BaseInput();
    input.bodyOffsetRad = 9.0f * 3.14159265358979323846f / 180.0f;
    input.dt = 0.1f;
    const float directions[] = {1.0f, -1.0f, 1.0f, -1.0f};
    for (float direction : directions) {
        input.steering = direction;
        controller.Update(input);
        input.steering = 0.0f;
        for (int i = 0; i < 11; ++i) controller.Update(input);
    }
    Require(!controller.Update(input).escRecoveryWorking,
            "direction changes spread beyond two seconds are not rapid");
}

void TestManualSteeringCancelsStraightEsc() {
    Controller controller;
    Input input = BaseInput();
    input.bodyOffsetRad = 0.1f;
    input.yawRateRadS = 0.1f;
    for (int i = 0; i < 40; ++i) controller.Update(input);
    input.steering = 1.0f;
    for (int i = 0; i < 6; ++i) controller.Update(input);
    Require(!controller.Update(input).escStraightWorking,
            "manual steering must quickly release straight-line ESC");
}

}  // namespace

int main() {
    TestDriftPriorityAndReactivationDelay();
    TestAbsDelayFadeAndInterrupt();
    TestEscSpeedGateAndGentleStraightening();
    TestRapidDirectionRecoveryAndTcsOverride();
    TestDirectEscRecoveryTriggers();
    TestSlowDirectionChangesDoNotTriggerRecovery();
    TestManualSteeringCancelsStraightEsc();
    TestLaunchControlLatchAndRelease();
    TestLaunchControlEntryGates();
    TestTcsGearProbabilities();
    TestEscRecoveryStrengthAndSmoothAngleGain();
    std::cout << "driver assist tests passed\n";
    return 0;
}
