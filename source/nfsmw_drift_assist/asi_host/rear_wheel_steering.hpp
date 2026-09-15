#pragma once

#include "drift_assist_ini.hpp"
#include "vehicle_countersteer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nfsmw_drift_asi::rear_wheel_steering {

constexpr std::size_t kMaximumEntries = 512;
constexpr float kMaximumAbsoluteAngleDegrees = 15.0f;
constexpr float kPhaseBoundaryMps = 70.0f / 3.6f;
constexpr float kFullActuatorRateBelowMps = 30.0f / 3.6f;
constexpr float kMinimumActuatorRateAboveMps = 120.0f / 3.6f;
constexpr float kMaximumActuatorRateDegreesPerSecond = 20.0f;
constexpr float kMinimumActuatorRateDegreesPerSecond = 10.0f;

struct Entry {
    std::uint32_t collectionKey = 0;
    float lowSpeedAngleDegrees = 0.0f;
    float highSpeedAngleDegrees = 0.0f;
    std::array<char,
               vehicle_countersteer::kMaximumVehicleNameLength + 1> name{};
};

struct Registry {
    std::array<Entry, kMaximumEntries> entries{};
    std::size_t count = 0;

    const Entry* Find(std::uint32_t collectionKey) const noexcept {
        if (collectionKey == 0) {
            return nullptr;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (entries[index].collectionKey == collectionKey) {
                return &entries[index];
            }
        }
        return nullptr;
    }
};

namespace detail {

inline std::size_t FindWhitespace(std::string_view value) noexcept {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == ' ' || value[index] == '\t' ||
            value[index] == '\r' || value[index] == '\n') {
            return index;
        }
    }
    return std::string_view::npos;
}

inline bool ParseTwoAngles(std::string_view value,
                           float& lowAngleDegrees,
                           float& highAngleDegrees) noexcept {
    value = nfsmw_drift::ini_detail::Trim(value);
    const std::size_t separator = FindWhitespace(value);
    if (separator == std::string_view::npos) {
        return false;
    }
    const std::string_view low = value.substr(0, separator);
    value = nfsmw_drift::ini_detail::Trim(value.substr(separator));
    const std::size_t secondSeparator = FindWhitespace(value);
    const std::string_view high = secondSeparator == std::string_view::npos
                                      ? value
                                      : value.substr(0, secondSeparator);
    const std::string_view trailing =
        secondSeparator == std::string_view::npos
            ? std::string_view{}
            : nfsmw_drift::ini_detail::Trim(
                  value.substr(secondSeparator));
    return !low.empty() && !high.empty() && trailing.empty() &&
           nfsmw_drift::ini_detail::ParseFloat(low, lowAngleDegrees) &&
           nfsmw_drift::ini_detail::ParseFloat(high, highAngleDegrees) &&
           std::isfinite(lowAngleDegrees) &&
           std::isfinite(highAngleDegrees) &&
           std::fabs(lowAngleDegrees) <= kMaximumAbsoluteAngleDegrees &&
           std::fabs(highAngleDegrees) <= kMaximumAbsoluteAngleDegrees;
}

}  // namespace detail

inline nfsmw_drift::IniParseResult ParseIni(std::string_view text,
                                            Registry& out) {
    if (nfsmw_drift::ini_detail::HasUtf8Bom(text)) {
        text.remove_prefix(3);
    }

    Registry candidate{};
    bool inSection = false;
    std::size_t lineNumber = 1;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = nfsmw_drift::ini_detail::Trim(
            nfsmw_drift::ini_detail::RemoveComment(
                text.substr(start, end - start)));
        if (!line.empty()) {
            if (line.front() == '[') {
                const std::size_t close = line.find(']');
                if (close == std::string_view::npos ||
                    !nfsmw_drift::ini_detail::Trim(
                         line.substr(close + 1)).empty()) {
                    return {false, lineNumber,
                            nfsmw_drift::IniParseErrorCode::MalformedLine};
                }
                const std::string_view section =
                    nfsmw_drift::ini_detail::Trim(
                        line.substr(1, close - 1));
                if (section.empty()) {
                    return {false, lineNumber,
                            nfsmw_drift::IniParseErrorCode::MalformedLine};
                }
                inSection = nfsmw_drift::ini_detail::EqualIgnoreCase(
                    section, "RearWheelSteeringVehicles");
            } else if (inSection) {
                std::string_view name{};
                std::string_view values{};
                const std::size_t equals = line.find('=');
                if (equals != std::string_view::npos) {
                    name = nfsmw_drift::ini_detail::Trim(
                        line.substr(0, equals));
                    values = nfsmw_drift::ini_detail::Trim(
                        line.substr(equals + 1));
                } else {
                    const std::size_t separator =
                        detail::FindWhitespace(line);
                    if (separator == std::string_view::npos) {
                        return {false, lineNumber,
                                nfsmw_drift::IniParseErrorCode::MalformedLine};
                    }
                    name = line.substr(0, separator);
                    values = nfsmw_drift::ini_detail::Trim(
                        line.substr(separator));
                }

                float lowAngleDegrees = 0.0f;
                float highAngleDegrees = 0.0f;
                const std::uint32_t collectionKey =
                    vehicle_countersteer::HashVehicleName(name);
                if (collectionKey == 0 ||
                    !detail::ParseTwoAngles(
                        values, lowAngleDegrees, highAngleDegrees) ||
                    candidate.count >= candidate.entries.size() ||
                    candidate.Find(collectionKey) != nullptr) {
                    return {false, lineNumber,
                            nfsmw_drift::IniParseErrorCode::InvalidValue};
                }

                Entry& entry = candidate.entries[candidate.count++];
                entry.collectionKey = collectionKey;
                entry.lowSpeedAngleDegrees = lowAngleDegrees;
                entry.highSpeedAngleDegrees = highAngleDegrees;
                for (std::size_t index = 0; index < name.size(); ++index) {
                    const char character = name[index];
                    entry.name[index] =
                        character >= 'A' && character <= 'Z'
                            ? static_cast<char>(character - 'A' + 'a')
                            : character;
                }
                entry.name[name.size()] = '\0';
            }
        }
        if (end == text.size()) {
            break;
        }
        start = end + 1;
        ++lineNumber;
    }
    out = candidate;
    return {};
}

inline bool ComputeOverrideAngleRad(const Registry& registry,
                                    std::uint32_t collectionKey,
                                    float rawSteering,
                                    float speedMps,
                                    bool driverAssistActive,
                                    bool driftActive,
                                    float& angleRad) noexcept {
    const Entry* const entry = registry.Find(collectionKey);
    if (entry == nullptr || !driverAssistActive || driftActive ||
        !std::isfinite(rawSteering) || !std::isfinite(speedMps) ||
        speedMps < 0.0f) {
        return false;
    }
    const float maximumAngleDegrees =
        speedMps < kPhaseBoundaryMps
            ? entry->lowSpeedAngleDegrees
            : entry->highSpeedAngleDegrees;
    const float normalizedSteering =
        std::clamp(rawSteering, -1.0f, 1.0f);
    angleRad = normalizedSteering * maximumAngleDegrees *
               (nfsmw_drift::kPi / 180.0f);
    return std::isfinite(angleRad);
}

inline float MaximumActuatorRateRadS(float speedMps) noexcept {
    float rateDegreesPerSecond = kMaximumActuatorRateDegreesPerSecond;
    if (std::isfinite(speedMps) &&
        speedMps > kFullActuatorRateBelowMps) {
        const float blend = std::clamp(
            (speedMps - kFullActuatorRateBelowMps) /
                (kMinimumActuatorRateAboveMps -
                 kFullActuatorRateBelowMps),
            0.0f, 1.0f);
        rateDegreesPerSecond =
            kMaximumActuatorRateDegreesPerSecond +
            (kMinimumActuatorRateDegreesPerSecond -
             kMaximumActuatorRateDegreesPerSecond) * blend;
    }
    return rateDegreesPerSecond * (nfsmw_drift::kPi / 180.0f);
}

inline float SlewAngleRad(float current,
                          float target,
                          float dt,
                          float maximumRateRadS) noexcept {
    if (!std::isfinite(current) || !std::isfinite(target) ||
        !std::isfinite(dt) || !std::isfinite(maximumRateRadS) ||
        maximumRateRadS <= 0.0f) {
        return 0.0f;
    }
    if (dt <= 0.0f) return current;
    const float maximumStep = maximumRateRadS * dt;
    return current + std::clamp(target - current,
                                -maximumStep, maximumStep);
}

inline bool RotateDirectionAroundAxis(
    const std::array<float, 3>& direction,
    const std::array<float, 3>& axisInput,
    float angleRad,
    std::array<float, 3>& rotated) noexcept {
    const float axisLengthSquared =
        axisInput[0] * axisInput[0] + axisInput[1] * axisInput[1] +
        axisInput[2] * axisInput[2];
    if (!std::isfinite(angleRad) || !std::isfinite(axisLengthSquared) ||
        axisLengthSquared < 0.25f || axisLengthSquared > 4.0f) {
        return false;
    }
    const float inverseLength = 1.0f / std::sqrt(axisLengthSquared);
    const std::array<float, 3> axis = {
        axisInput[0] * inverseLength, axisInput[1] * inverseLength,
        axisInput[2] * inverseLength};
    const float cosine = std::cos(angleRad);
    const float sine = std::sin(angleRad);
    const float dot = axis[0] * direction[0] + axis[1] * direction[1] +
                      axis[2] * direction[2];
    const std::array<float, 3> cross = {
        axis[1] * direction[2] - axis[2] * direction[1],
        axis[2] * direction[0] - axis[0] * direction[2],
        axis[0] * direction[1] - axis[1] * direction[0]};
    for (std::size_t index = 0; index < rotated.size(); ++index) {
        if (!std::isfinite(direction[index])) return false;
        rotated[index] = direction[index] * cosine + cross[index] * sine +
                         axis[index] * dot * (1.0f - cosine);
        if (!std::isfinite(rotated[index])) return false;
    }
    return true;
}

}  // namespace nfsmw_drift_asi::rear_wheel_steering
