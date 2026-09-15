#include "asi_host/handling_probe_state.hpp"

#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void Require(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

nfsmw_drift_asi::handling_probe_state::VehicleKey MakeKey(
    std::uintptr_t base) {
    nfsmw_drift_asi::handling_probe_state::VehicleKey key{};
    key.pvehicle = base + 1;
    key.player = base + 2;
    key.rigidBody = base + 3;
    key.rigidBodyHolder = base + 4;
    key.rigidBodyInner = base + 5;
    key.suspension = base + 6;
    key.transmission = base + 7;
    key.wheels = {base + 10, base + 11, base + 12, base + 13};
    return key;
}

void TestGenerationIsStableForOneVehicle() {
    using nfsmw_drift_asi::handling_probe_state::GenerationTracker;
    GenerationTracker tracker;
    const auto key = MakeKey(100);
    const std::uint32_t first = tracker.Observe(key);
    const std::uint32_t second = tracker.Observe(key);
    Require(first != 0 && second == first && tracker.valid(),
            "a stable player vehicle must retain one non-zero generation");
}

void TestEveryOwnedObjectChangeAdvancesGeneration() {
    using nfsmw_drift_asi::handling_probe_state::GenerationTracker;
    GenerationTracker tracker;
    auto key = MakeKey(200);
    std::uint32_t generation = tracker.Observe(key);

    key.wheels[3] += 100;
    const std::uint32_t wheelGeneration = tracker.Observe(key);
    Require(wheelGeneration != generation,
            "wheel reconstruction must advance the vehicle generation");
    generation = wheelGeneration;

    key.transmission += 100;
    const std::uint32_t transmissionGeneration = tracker.Observe(key);
    Require(transmissionGeneration != generation,
            "transmission replacement must advance the vehicle generation");
    generation = transmissionGeneration;

    key.player += 100;
    Require(tracker.Observe(key) != generation,
            "player identity replacement must advance the generation");
}

void TestInvalidationRevokesOldVehicle() {
    using nfsmw_drift_asi::handling_probe_state::GenerationTracker;
    GenerationTracker tracker;
    const auto key = MakeKey(300);
    const std::uint32_t first = tracker.Observe(key);
    tracker.Invalidate();
    Require(!tracker.valid() && tracker.generation() != first,
            "an invalid ownership frame must revoke the old generation");
    const std::uint32_t recovered = tracker.Observe(key);
    Require(recovered != 0 && recovered != first && tracker.valid(),
            "recovery must publish a fresh generation even at reused addresses");
}

void TestWheelOwnershipAndAxleClassification() {
    using namespace nfsmw_drift_asi::handling_probe_state;
    const auto key = MakeKey(400);
    Require(HasDistinctCompleteWheelSet(key.wheels),
            "four non-null distinct wheel pointers must form a complete set");
    for (std::size_t index = 0; index < kWheelCount; ++index) {
        Require(WheelIndex(key, key.wheels[index]) ==
                    static_cast<int>(index),
                "an exact owned wheel pointer must retain its table index");
        Require(IsFrontWheel(index) == (index < 2),
                "only wheel indices zero and one may be classified as front");
    }
    Require(WheelIndex(key, 0) == -1 && WheelIndex(key, 9999) == -1,
            "null or foreign wheel pointers must never be sampled");

    auto partial = key.wheels;
    partial[2] = 0;
    Require(!HasDistinctCompleteWheelSet(partial),
            "a partial wheel table must disable the wheel channel");
    auto duplicate = key.wheels;
    duplicate[3] = duplicate[2];
    Require(!HasDistinctCompleteWheelSet(duplicate),
            "duplicate wheel pointers must disable the wheel channel");
}

void TestTorqueSignPreservesDirection() {
    using nfsmw_drift_asi::handling_probe_state::ClassifyTorque;
    using nfsmw_drift_asi::handling_probe_state::TorqueSign;
    Require(ClassifyTorque(12.0f) == TorqueSign::Positive,
            "positive drive torque must be identified separately");
    Require(ClassifyTorque(0.0f) == TorqueSign::Zero &&
                ClassifyTorque(-0.0f) == TorqueSign::Zero,
            "zero torque must remain a pass-through category");
    Require(ClassifyTorque(-12.0f) == TorqueSign::Negative,
            "negative engine-braking or reverse torque must remain separate");
}

void TestValidatedTextRangeContract() {
    using nfsmw_drift_asi::handling_probe_state::TextRangeContains;
    using nfsmw_drift_asi::handling_probe_state::TextRangeFailure;
    using nfsmw_drift_asi::handling_probe_state::ValidateTextRange;

    constexpr std::uintptr_t imageBase = 0x00400000u;
    constexpr std::size_t imageSize = 0x00693000u;
    constexpr std::uintptr_t textStart = 0x00401000u;
    constexpr std::size_t textSize = 0x00200000u;

    Require(ValidateTextRange(imageBase, imageSize, textStart, textSize) ==
                TextRangeFailure::None,
            "the main gate's in-image text range must be accepted");
    Require(ValidateTextRange(0, imageSize, textStart, textSize) ==
                TextRangeFailure::ImageUnavailable,
            "a missing image base must reject the text contract");
    Require(ValidateTextRange(imageBase, imageSize, 0, textSize) ==
                TextRangeFailure::TextUnavailable,
            "a missing text start must reject the text contract");
    Require(ValidateTextRange(imageBase, imageSize, imageBase - 1,
                              textSize) ==
                TextRangeFailure::StartBeforeImage,
            "a text start before the image must be distinguished");
    Require(ValidateTextRange(imageBase, imageSize,
                              imageBase + imageSize, 1) ==
                TextRangeFailure::StartOutsideImage,
            "a text start at the image end must be rejected");
    Require(ValidateTextRange(imageBase, imageSize,
                              imageBase + imageSize - 1, 2) ==
                TextRangeFailure::SpanOutsideImage,
            "a text span crossing the image end must be rejected");

    Require(TextRangeContains(textStart, textSize, textStart, 1),
            "the first byte must be inside the trusted text range");
    Require(TextRangeContains(textStart, textSize,
                              textStart + textSize - 1, 1),
            "the final byte must be inside the trusted text range");
    Require(!TextRangeContains(textStart, textSize,
                               textStart + textSize, 1),
            "the first byte after text must be rejected");
    Require(!TextRangeContains(textStart, textSize,
                               textStart + textSize - 1, 2),
            "a target crossing the text end must be rejected");
}

}  // namespace

int main() {
    TestGenerationIsStableForOneVehicle();
    TestEveryOwnedObjectChangeAdvancesGeneration();
    TestInvalidationRevokesOldVehicle();
    TestWheelOwnershipAndAxleClassification();
    TestTorqueSignPreservesDirection();
    TestValidatedTextRangeContract();
    if (failures != 0) {
        std::cerr << failures << " handling probe assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All handling probe state tests passed\n";
    return EXIT_SUCCESS;
}
