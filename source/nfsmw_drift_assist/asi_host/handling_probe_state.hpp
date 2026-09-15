#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nfsmw_drift_asi::handling_probe_state {

inline constexpr std::size_t kWheelCount = 4;
inline constexpr std::size_t kFrontWheelCount = 2;

enum class TextRangeFailure : std::uint8_t {
    None,
    ImageUnavailable,
    TextUnavailable,
    StartBeforeImage,
    StartOutsideImage,
    SpanOutsideImage,
};

constexpr TextRangeFailure ValidateTextRange(
    std::uintptr_t imageBase,
    std::size_t imageSize,
    std::uintptr_t textStart,
    std::size_t textSize) noexcept {
    if (imageBase == 0 || imageSize == 0) {
        return TextRangeFailure::ImageUnavailable;
    }
    if (textStart == 0 || textSize == 0) {
        return TextRangeFailure::TextUnavailable;
    }
    if (textStart < imageBase) {
        return TextRangeFailure::StartBeforeImage;
    }
    const std::uintptr_t offset = textStart - imageBase;
    if (offset >= imageSize) {
        return TextRangeFailure::StartOutsideImage;
    }
    if (textSize > imageSize - static_cast<std::size_t>(offset)) {
        return TextRangeFailure::SpanOutsideImage;
    }
    return TextRangeFailure::None;
}

constexpr bool TextRangeContains(std::uintptr_t textStart,
                                 std::size_t textSize,
                                 std::uintptr_t address,
                                 std::size_t size) noexcept {
    if (textStart == 0 || textSize == 0 || address < textStart ||
        size == 0) {
        return false;
    }
    const std::uintptr_t offset = address - textStart;
    return offset <= textSize &&
           size <= textSize - static_cast<std::size_t>(offset);
}

struct VehicleKey {
    std::uintptr_t pvehicle = 0;
    std::uintptr_t player = 0;
    std::uintptr_t rigidBody = 0;
    std::uintptr_t rigidBodyHolder = 0;
    std::uintptr_t rigidBodyInner = 0;
    std::uintptr_t suspension = 0;
    std::uintptr_t transmission = 0;
    std::array<std::uintptr_t, kWheelCount> wheels{};

    constexpr bool Complete() const noexcept {
        return pvehicle != 0 && player != 0 && rigidBody != 0 &&
               rigidBodyHolder != 0 && rigidBodyInner != 0;
    }
};

constexpr bool SameVehicle(const VehicleKey& left,
                           const VehicleKey& right) noexcept {
    if (left.pvehicle != right.pvehicle || left.player != right.player ||
        left.rigidBody != right.rigidBody ||
        left.rigidBodyHolder != right.rigidBodyHolder ||
        left.rigidBodyInner != right.rigidBodyInner ||
        left.suspension != right.suspension ||
        left.transmission != right.transmission) {
        return false;
    }
    for (std::size_t index = 0; index < kWheelCount; ++index) {
        if (left.wheels[index] != right.wheels[index]) {
            return false;
        }
    }
    return true;
}

constexpr int WheelIndex(const VehicleKey& key,
                         std::uintptr_t wheel) noexcept {
    if (wheel == 0) {
        return -1;
    }
    for (std::size_t index = 0; index < kWheelCount; ++index) {
        if (key.wheels[index] == wheel) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

constexpr bool HasDistinctCompleteWheelSet(
    const std::array<std::uintptr_t, kWheelCount>& wheels) noexcept {
    for (std::size_t index = 0; index < kWheelCount; ++index) {
        if (wheels[index] == 0) {
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (wheels[previous] == wheels[index]) {
                return false;
            }
        }
    }
    return true;
}

constexpr bool IsFrontWheel(std::size_t index) noexcept {
    return index < kFrontWheelCount;
}

enum class TorqueSign : std::uint8_t {
    Negative,
    Zero,
    Positive,
};

constexpr TorqueSign ClassifyTorque(float torque) noexcept {
    return torque > 0.0f ? TorqueSign::Positive
                         : (torque < 0.0f ? TorqueSign::Negative
                                          : TorqueSign::Zero);
}

class GenerationTracker {
public:
    std::uint32_t Observe(const VehicleKey& key) noexcept {
        if (!key.Complete()) {
            Invalidate();
            return 0;
        }
        if (!valid_ || !SameVehicle(key_, key)) {
            Advance();
            key_ = key;
            valid_ = true;
        }
        return generation_;
    }

    void Invalidate() noexcept {
        if (!valid_) {
            return;
        }
        Advance();
        key_ = VehicleKey{};
        valid_ = false;
    }

    bool valid() const noexcept { return valid_; }
    std::uint32_t generation() const noexcept { return generation_; }
    const VehicleKey& key() const noexcept { return key_; }

private:
    void Advance() noexcept {
        ++generation_;
        if (generation_ == 0) {
            ++generation_;
        }
    }

    VehicleKey key_{};
    std::uint32_t generation_ = 0;
    bool valid_ = false;
};

}  // namespace nfsmw_drift_asi::handling_probe_state
