#include "asi_host/vehicle_countersteer.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

using nfsmw_drift_asi::vehicle_countersteer::HashVehicleName;
using nfsmw_drift_asi::vehicle_countersteer::BChunkHash;
using nfsmw_drift_asi::vehicle_countersteer::ParseIni;
using nfsmw_drift_asi::vehicle_countersteer::Registry;

int failures = 0;

void Require(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void TestKnownHashAndCaseNormalization() {
    Require(BChunkHash("BASE") == 0xA6B47FACu,
            "the portable vehicle hash must match NFSMW StringToKey");
    Require(HashVehicleName("default") == 0xEEC2271Au,
            "the portable vehicle hash must match MultiGear's known default collection key");
    Require(HashVehicleName("TT") == HashVehicleName("tt"),
            "vehicle names in the INI must be case-insensitive");
    Require(HashVehicleName("") == 0,
            "an empty vehicle collection name must be rejected");
}

void TestRequestedExample() {
    Registry registry{};
    const auto parsed = ParseIni(
        "[AssistConfig]\n"
        "smartCountersteerAngleRad=0.95993109\n"
        "[SmartCountersteerVehicleMultipliers]\n"
        "laferrari_top 0.75\n"
        "997s=0.855\n"
        "a3=0.22\n"
        "a4=0.15\n",
        registry);
    Require(parsed.ok && registry.count == 4,
            "all four requested per-car multipliers must parse");
    Require(std::fabs(registry.MultiplierFor(
                          HashVehicleName("laferrari_top")) -
                      0.75f) < 1.0e-6f,
            "the full add-on vehicle name must parse without an equals sign");
    Require(std::fabs(registry.MultiplierFor(HashVehicleName("997s")) -
                      0.855f) < 1.0e-6f,
            "997S must receive its configured multiplier");
    Require(std::fabs(registry.MultiplierFor(HashVehicleName("a3")) -
                      0.22f) < 1.0e-6f,
            "A3 must receive its configured multiplier");
    Require(std::fabs(registry.MultiplierFor(HashVehicleName("a4")) -
                      0.15f) < 1.0e-6f,
            "A4 must receive its configured multiplier");
    Require(std::fabs(registry.MultiplierFor(HashVehicleName("rx7")) -
                      1.0f) < 1.0e-6f,
            "an unlisted car must retain the base angle");
}

void TestInvalidInputDoesNotPartiallyCommit() {
    Registry registry{};
    Require(ParseIni(
                "[SmartCountersteerVehicleMultipliers]\ntt=0.75\n",
                registry).ok,
            "the baseline registry must parse");
    const auto invalid = ParseIni(
        "[SmartCountersteerVehicleMultipliers]\n"
        "a3=0.22\n"
        "a4=2.5\n",
        registry);
    Require(!invalid.ok && invalid.line == 3,
            "an out-of-range multiplier must report its source line");
    Require(registry.count == 1 &&
                std::fabs(registry.MultiplierFor(HashVehicleName("tt")) -
                          0.75f) < 1.0e-6f,
            "a rejected section must leave the previous registry intact");

    const auto duplicate = ParseIni(
        "[SmartCountersteerVehicleMultipliers]\n"
        "TT=0.75\n"
        "tt=0.80\n",
        registry);
    Require(!duplicate.ok && duplicate.line == 3,
            "case-insensitive duplicate vehicle entries must be rejected");
}

void TestMoreThan128VehicleEntries() {
    std::string ini = "[SmartCountersteerVehicleMultipliers]\n";
    for (int index = 0; index < 130; ++index) {
        ini += "vehicle_" + std::to_string(index) + " 0.75\n";
    }
    Registry registry{};
    const auto parsed = ParseIni(ini, registry);
    Require(parsed.ok && registry.count == 130,
            "a complete 130-car roster must fit in the registry");
}

}  // namespace

int main(int argc, char** argv) {
    TestKnownHashAndCaseNormalization();
    TestRequestedExample();
    TestInvalidInputDoesNotPartiallyCommit();
    TestMoreThan128VehicleEntries();
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        Registry registry{};
        const auto parsed = ParseIni(text, registry);
        Require(input.good() || input.eof(),
                "the requested INI must be readable");
        if (!parsed.ok) {
            std::cerr << "INI parse failed at line " << parsed.line << '\n';
            ++failures;
        } else {
            std::cout << "INI vehicle entries: " << registry.count << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " vehicle countersteer test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "vehicle countersteer tests passed\n";
    return EXIT_SUCCESS;
}
