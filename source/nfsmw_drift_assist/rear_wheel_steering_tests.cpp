#include "asi_host/rear_wheel_steering.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using nfsmw_drift_asi::rear_wheel_steering::ComputeOverrideAngleRad;
using nfsmw_drift_asi::rear_wheel_steering::kMaximumEntries;
using nfsmw_drift_asi::rear_wheel_steering::MaximumActuatorRateRadS;
using nfsmw_drift_asi::rear_wheel_steering::ParseIni;
using nfsmw_drift_asi::rear_wheel_steering::Registry;
using nfsmw_drift_asi::rear_wheel_steering::RotateDirectionAroundAxis;
using nfsmw_drift_asi::rear_wheel_steering::SlewAngleRad;
using nfsmw_drift_asi::vehicle_countersteer::HashVehicleName;

int failures = 0;

void Require(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool Near(float left, float right) {
    return std::fabs(left - right) < 1.0e-6f;
}

void TestEmptyAndRequestedForms() {
    Require(kMaximumEntries == 512,
            "rear-steering registry must support large vehicle lists");
    Registry registry{};
    Require(ParseIni("[AssistConfig]\nenabled=true\n", registry).ok &&
                registry.count == 0,
            "an absent rear-steering section must produce an empty registry");
    Require(ParseIni(
                "[RearWheelSteeringVehicles]\n"
                "laferrari_top -15 15\n"
                "A3=-3.5 2.25\n",
                registry).ok && registry.count == 2,
            "space and equals forms must both parse");
    const auto* const laferrari =
        registry.Find(HashVehicleName("LAFERRARI_TOP"));
    Require(laferrari != nullptr &&
                Near(laferrari->lowSpeedAngleDegrees, -15.0f) &&
                Near(laferrari->highSpeedAngleDegrees, 15.0f),
            "the requested full add-on vehicle name must be case-insensitive and exact");

    nfsmw_drift::AssistConfig assist{};
    const char* const combined =
        "[AssistConfig]\n"
        "enabled=true\n"
        "[SmartCountersteerVehicleMultipliers]\n"
        "tt=0.75\n"
        "[RearWheelSteeringVehicles]\n"
        "tt -5 5\n";
    nfsmw_drift_asi::vehicle_countersteer::Registry countersteer{};
    Require(nfsmw_drift::ParseAssistConfigIni(combined, assist).ok &&
                nfsmw_drift_asi::vehicle_countersteer::ParseIni(
                    combined, countersteer).ok &&
                ParseIni(combined, registry).ok,
            "all three INI parsers must accept the combined release file");
}

void TestVehicleNamesAreIndependentOfPerformanceNodes() {
    Registry registry{};
    Require(ParseIni(
                "[RearWheelSteeringVehicles]\n"
                "revuelto -6 2\n"
                "slr -5 3\n",
                registry).ok,
            "actual vehicle names must parse");
    Require(registry.Find(HashVehicleName("revuelto")) != nullptr &&
                registry.Find(HashVehicleName("slr")) != nullptr,
            "actual vehicle keys must select their own profiles");
    Require(registry.Find(HashVehicleName("revuelto_top")) == nullptr &&
                registry.Find(HashVehicleName("slr_top")) == nullptr &&
                registry.Find(HashVehicleName("aventadorsvj_top")) == nullptr,
            "performance-part node names must never select a vehicle profile");
}

void TestInvalidEntriesDoNotCommit() {
    Registry registry{};
    Require(ParseIni("[RearWheelSteeringVehicles]\ntt -5 5\n", registry).ok,
            "baseline rear-steering profile must parse");
    const char* invalidInputs[] = {
        "[RearWheelSteeringVehicles]\ntt -5\n",
        "[RearWheelSteeringVehicles]\ntt -5 5 6\n",
        "[RearWheelSteeringVehicles]\ntt nope 5\n",
        "[RearWheelSteeringVehicles]\ntt -16 5\n",
        "[RearWheelSteeringVehicles]\nTT -5 5\ntt -4 4\n",
    };
    for (const char* text : invalidInputs) {
        const auto parsed = ParseIni(text, registry);
        Require(!parsed.ok,
                "missing, extra, nonnumeric, out-of-range and duplicate values must fail");
        Require(registry.count == 1 &&
                    registry.Find(HashVehicleName("tt")) != nullptr,
                "a rejected section must not partially replace the registry");
    }
}

void TestRuntimeSelectionAndBoundaries() {
    Registry registry{};
    Require(ParseIni("[RearWheelSteeringVehicles]\ntt -5 5\n", registry).ok,
            "runtime profile must parse");
    float angle = 99.0f;
    const std::uint32_t tt = HashVehicleName("tt");
    Require(ComputeOverrideAngleRad(registry, tt, 0.5f, 69.99f / 3.6f,
                                    true, false, angle) &&
                Near(angle, -2.5f * nfsmw_drift::kPi / 180.0f),
            "below 70 km/h must use the low-speed angle and scale with input");
    Require(ComputeOverrideAngleRad(registry, tt, 1.0f, 70.0f / 3.6f,
                                    true, false, angle) &&
                Near(angle, 5.0f * nfsmw_drift::kPi / 180.0f),
            "70 km/h must use the high-speed angle");
    Require(ComputeOverrideAngleRad(registry, tt, 1.0f, 300.0f / 3.6f,
                                    true, false, angle) &&
                Near(angle, 5.0f * nfsmw_drift::kPi / 180.0f),
            "rear steering must remain active without a high-speed cutoff");
    Require(!ComputeOverrideAngleRad(registry, HashVehicleName("rx7"), 1.0f,
                                     20.0f, true, false, angle),
            "an unlisted vehicle must remain untouched");
    Require(!ComputeOverrideAngleRad(registry, tt, 1.0f, 20.0f,
                                     false, false, angle),
            "inactive driver assistance must release rear steering");
    Require(!ComputeOverrideAngleRad(registry, tt, 1.0f, 20.0f,
                                     true, true, angle),
            "active drift must release rear steering immediately");
}

void TestTireDirectionRotation() {
    std::array<float, 3> rotated{};
    const std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
    const std::array<float, 3> up{0.0f, 2.0f, 0.0f};
    Require(RotateDirectionAroundAxis(
                forward, up, 90.0f * nfsmw_drift::kPi / 180.0f, rotated) &&
                Near(rotated[0], 1.0f) && Near(rotated[1], 0.0f) &&
                Near(rotated[2], 0.0f),
            "rear tire direction must rotate around normalized contact normal");
    const float length = std::sqrt(rotated[0] * rotated[0] +
                                   rotated[1] * rotated[1] +
                                   rotated[2] * rotated[2]);
    Require(Near(length, 1.0f),
            "rear tire direction rotation must preserve vector length");
    Require(!RotateDirectionAroundAxis(
                forward, {0.0f, 0.0f, 0.0f}, 0.1f, rotated),
            "an invalid contact normal must fail closed");
}

void TestSteeringActuatorRate() {
    const float degreesToRad = nfsmw_drift::kPi / 180.0f;
    Require(Near(MaximumActuatorRateRadS(0.0f), 20.0f * degreesToRad) &&
                Near(MaximumActuatorRateRadS(30.0f / 3.6f),
                     20.0f * degreesToRad),
            "speeds at or below 30 km/h must use 20 degrees per second");
    Require(Near(MaximumActuatorRateRadS(75.0f / 3.6f),
                 15.0f * degreesToRad),
            "the actuator rate must interpolate linearly from 30 to 120 km/h");
    Require(Near(MaximumActuatorRateRadS(120.0f / 3.6f),
                 10.0f * degreesToRad) &&
                Near(MaximumActuatorRateRadS(300.0f / 3.6f),
                     10.0f * degreesToRad),
            "speeds at or above 120 km/h must use 10 degrees per second");
    const float rate = MaximumActuatorRateRadS(75.0f / 3.6f);
    constexpr float target = 5.0f * nfsmw_drift::kPi / 180.0f;
    const float first = SlewAngleRad(0.0f, target, 0.025f, rate);
    Require(Near(first, 0.375f * nfsmw_drift::kPi / 180.0f),
            "rear steering must use the speed-dependent actuator rate");
    const float phaseChange = SlewAngleRad(target, -target, 0.025f, rate);
    Require(phaseChange > 0.0f && phaseChange < target,
            "a 70 km/h phase change must slew through center instead of snapping");
    Require(Near(SlewAngleRad(target, target, 0.025f, rate), target),
            "an angle already at target must remain stable");
    Require(Near(SlewAngleRad(target, -target, 0.0f, rate), target),
            "multiple input polls in one millisecond must not reset the actuator");
}

}  // namespace

int main() {
    TestEmptyAndRequestedForms();
    TestVehicleNamesAreIndependentOfPerformanceNodes();
    TestInvalidEntriesDoNotCommit();
    TestRuntimeSelectionAndBoundaries();
    TestTireDirectionRotation();
    TestSteeringActuatorRate();
    if (failures != 0) {
        std::cerr << failures << " rear-wheel steering test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "rear-wheel steering tests passed\n";
    return EXIT_SUCCESS;
}
