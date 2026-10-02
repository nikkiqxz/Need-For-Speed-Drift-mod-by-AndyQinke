#include "VehicleTone.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

bool near(float left, float right) {
    return std::fabs(left - right) < 0.000001f;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace nfsmw_exhaust::vehicle_tone;
    expect(bChunkHash("BASE") == 0xA6B47FACu,
           "portable hash must match NFSMW StringToKey");
    expect(hashVehicleName("default") == 0xEEC2271Au,
           "default collection hash must match the known game key");
    expect(hashVehicleName("RX7") == hashVehicleName("rx7"),
           "vehicle collection names must be case-insensitive");

    Registry registry{};
    std::string error;
    std::size_t line = 0;
    expect(parseIni(
               "[burst]\nburst_min_shots=4\n"
               "[BackfireToneVariation]\n"
               "batch_toward_neutral_probability=0.30\n"
               "batch_toward_neutral_amount=1.00\n"
               "batch_full_neutral_distance=0.60\n"
               "batch_max_neutral_overshoot=0.20\n"
               "[BackfireToneByVehicle]\nM3GTRE46 0.35\nrx7=1.70\n"
               "darkest=0\nbrightest 2\n",
               &registry, &error, &line),
           "valid per-vehicle tone section must parse");
    expect(registry.count == 4, "all tone entries must be retained");
    expect(near(registry.characterFor(hashVehicleName("m3gtre46")), 0.35f),
           "whitespace form must parse case-insensitively");
    expect(near(registry.characterFor(hashVehicleName("RX7")), 1.70f),
           "equals form must parse case-insensitively");
    expect(near(registry.characterFor(hashVehicleName("darkest")), 0.0f) &&
               near(registry.characterFor(hashVehicleName("brightest")), 2.0f),
           "both inclusive range endpoints must parse");
    expect(near(registry.characterFor(hashVehicleName("unlisted")), 1.0f),
           "unlisted vehicles must use the neutral default");
    expect(near(registry.batchTowardNeutralProbability, 0.30f) &&
               near(registry.batchTowardNeutralAmount, 1.0f) &&
               near(registry.batchFullNeutralDistance, 0.60f) &&
               near(registry.batchMaxNeutralOvershoot, 0.20f),
           "batch tone variation settings must parse");
    expect(near(varyBatchCharacter(0.39f, 1.0f, 0.60f, 0.20f, 1.0f),
                1.0f) &&
               near(varyBatchCharacter(1.70f, 1.0f, 0.60f, 0.20f, 1.0f),
                    1.0f),
           "far tone values must be able to return fully to neutral");
    expect(varyBatchCharacter(0.90f, 1.0f, 0.60f, 0.20f, 1.0f) > 1.0f &&
               varyBatchCharacter(1.10f, 1.0f, 0.60f, 0.20f, 1.0f) < 1.0f &&
               near(varyBatchCharacter(1.0f, 1.0f, 0.60f, 0.20f, 0.0f),
                    0.80f) &&
               near(varyBatchCharacter(1.0f, 1.0f, 0.60f, 0.20f, 1.0f),
                    1.20f),
           "near-neutral batches must support up to 0.20 cross-neutral variation");

    const Registry previous = registry;
    expect(!parseIni("[BackfireToneByVehicle]\ncar=0.5\nCAR=1.5\n",
                     &registry, &error, &line) && line == 3,
           "case-insensitive duplicate names must be rejected");
    expect(registry.count == previous.count &&
               near(registry.characterFor(hashVehicleName("rx7")), 1.70f),
           "a rejected section must not partially replace the registry");
    expect(!parseIni("[BackfireToneByVehicle]\ncar=2.01\n",
                     &registry, &error, &line) && line == 2,
           "values above two must be rejected");
    expect(!parseIni("[BackfireToneByVehicle]\ncar=-0.01\n",
                     &registry, &error, &line) && line == 2,
           "values below zero must be rejected");
    expect(!parseIni(
               "[BackfireToneVariation]\n"
               "batch_toward_neutral_probability=1.01\n",
               &registry, &error, &line) && line == 2,
           "batch variation probability above one must be rejected");

    std::string large = "[BackfireToneByVehicle]\n";
    for (int index = 0; index < 130; ++index)
        large += "vehicle_" + std::to_string(index) + " 1.0\n";
    expect(parseIni(large, &registry, &error, &line) && registry.count == 130,
           "a roster larger than 128 vehicles must fit");

    if (argc > 1) {
        const std::filesystem::path configPath =
            std::filesystem::path(argv[1]) / "config" /
            "NFSMWExhaustBackfire.ini";
        expect(loadFile(configPath.string().c_str(), &registry, &error),
               "the shipped per-vehicle tone configuration must parse");
        expect(registry.count == 131,
               "the shipped configuration must contain all 131 racer Collections");
        expect(near(registry.characterFor(hashVehicleName("bmwm3gtr")), 0.79f),
               "the shipped configuration must preserve its dark vehicle values");
        expect(near(registry.characterFor(hashVehicleName("zc33s")), 1.40f),
               "the shipped configuration must preserve its bright JDM values");
        expect(near(registry.characterFor(hashVehicleName("a4")), 1.0f),
               "the shipped configuration must preserve explicit neutral values");
        expect(near(registry.characterFor(hashVehicleName("lvn")), 0.51f),
               "the latest supplied vehicle configuration must be installed");
        expect(near(registry.batchTowardNeutralProbability, 0.30f) &&
                   near(registry.batchTowardNeutralAmount, 1.0f) &&
                   near(registry.batchFullNeutralDistance, 0.60f) &&
                   near(registry.batchMaxNeutralOvershoot, 0.20f),
               "the shipped adaptive batch variation settings must match");
    }

    if (failures != 0) return 1;
    std::puts("Vehicle Collection hashing and per-car tone configuration verified");
    return 0;
}
