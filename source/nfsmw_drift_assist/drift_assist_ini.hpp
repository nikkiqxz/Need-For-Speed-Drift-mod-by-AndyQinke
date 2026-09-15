#pragma once

#include "drift_assist.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace nfsmw_drift {

enum class IniParseErrorCode : std::uint8_t {
    None,
    MalformedLine,
    InvalidValue,
};

struct IniParseResult {
    bool ok = true;
    std::size_t line = 0;
    IniParseErrorCode error = IniParseErrorCode::None;

    explicit operator bool() const { return ok; }
};

namespace ini_detail {

inline bool IsAsciiSpace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\f' || value == '\v';
}

inline std::string_view Trim(std::string_view value) {
    while (!value.empty() && IsAsciiSpace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && IsAsciiSpace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

inline char LowerAscii(char value) {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

inline bool EqualIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (LowerAscii(left[index]) != LowerAscii(right[index])) {
            return false;
        }
    }
    return true;
}

inline std::string_view RemoveComment(std::string_view value) {
    const std::size_t semicolon = value.find(';');
    const std::size_t hash = value.find('#');
    std::size_t comment = std::string_view::npos;
    if (semicolon != std::string_view::npos) {
        comment = semicolon;
    }
    if (hash != std::string_view::npos) {
        comment = comment == std::string_view::npos ? hash : std::min(comment, hash);
    }
    return comment == std::string_view::npos ? value : value.substr(0, comment);
}

inline bool HasUtf8Bom(std::string_view value) {
    return value.size() >= 3 &&
           static_cast<unsigned char>(value[0]) == 0xEFu &&
           static_cast<unsigned char>(value[1]) == 0xBBu &&
           static_cast<unsigned char>(value[2]) == 0xBFu;
}

inline bool ParseFloat(std::string_view value, float& result) {
    const std::string copy(value);
    if (copy.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE ||
        !std::isfinite(parsed)) {
        return false;
    }
    result = parsed;
    return true;
}

inline bool ParseUint8(std::string_view value, std::uint8_t& result) {
    float parsed = 0.0f;
    if (!ParseFloat(value, parsed) || parsed < 0.0f || parsed > 255.0f ||
        std::floor(parsed) != parsed) {
        return false;
    }
    result = static_cast<std::uint8_t>(parsed);
    return true;
}

inline bool ParseBool(std::string_view value, bool& result) {
    if (EqualIgnoreCase(value, "true") || value == "1" ||
        EqualIgnoreCase(value, "yes") || EqualIgnoreCase(value, "on")) {
        result = true;
        return true;
    }
    if (EqualIgnoreCase(value, "false") || value == "0" ||
        EqualIgnoreCase(value, "no") || EqualIgnoreCase(value, "off")) {
        result = false;
        return true;
    }
    return false;
}

inline bool ParseActivation(std::string_view value, ActivationMode& result) {
    if (EqualIgnoreCase(value, "HandbrakeHold")) {
        result = ActivationMode::HandbrakeHold;
        return true;
    }
    if (EqualIgnoreCase(value, "Manual")) {
        result = ActivationMode::Manual;
        return true;
    }
    if (EqualIgnoreCase(value, "Automatic")) {
        result = ActivationMode::Automatic;
        return true;
    }
    if (EqualIgnoreCase(value, "ManualOrAutomatic")) {
        result = ActivationMode::ManualOrAutomatic;
        return true;
    }
    return false;
}

inline bool ParseActuation(std::string_view value, ActuationMode& result) {
    if (EqualIgnoreCase(value, "SteeringOnly")) {
        result = ActuationMode::SteeringOnly;
        return true;
    }
    if (EqualIgnoreCase(value, "SteeringAndAttitude")) {
        result = ActuationMode::SteeringAndAttitude;
        return true;
    }
    if (EqualIgnoreCase(value, "AttitudeOnly")) {
        result = ActuationMode::AttitudeOnly;
        return true;
    }
    return false;
}

} // namespace ini_detail

// Parse the [AssistConfig] section from an INI text buffer. Unknown keys and
// other sections are ignored for forward compatibility. The destination is
// changed only when every recognized value parses successfully; range and
// finite-value sanitization is still performed by DriftAssistController's
// constructor/setConfig().
inline IniParseResult ParseAssistConfigIni(std::string_view text, AssistConfig& out) {
    // Most Windows editors emit a UTF-8 BOM. It is metadata, not part of the
    // first section name, so strip it before trimming/parsing the first line.
    if (ini_detail::HasUtf8Bom(text)) {
        text.remove_prefix(3);
    }

    AssistConfig candidate = out;
    bool inAssistConfigSection = true;
    std::size_t lineNumber = 1;
    std::size_t start = 0;

    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }

        std::string_view line = ini_detail::Trim(
            ini_detail::RemoveComment(text.substr(start, end - start)));
        if (!line.empty()) {
            if (line.front() == '[') {
                const std::size_t close = line.find(']');
                if (close == std::string_view::npos ||
                    !ini_detail::Trim(line.substr(close + 1)).empty()) {
                    return {false, lineNumber, IniParseErrorCode::MalformedLine};
                }
                const std::string_view section =
                    ini_detail::Trim(line.substr(1, close - 1));
                if (section.empty()) {
                    return {false, lineNumber, IniParseErrorCode::MalformedLine};
                }
                inAssistConfigSection =
                    ini_detail::EqualIgnoreCase(section, "AssistConfig");
            } else if (inAssistConfigSection) {
                const std::size_t separator = line.find('=');
                if (separator == std::string_view::npos) {
                    return {false, lineNumber, IniParseErrorCode::MalformedLine};
                }

                const std::string_view key = ini_detail::Trim(line.substr(0, separator));
                const std::string_view value = ini_detail::Trim(line.substr(separator + 1));
                if (key.empty()) {
                    return {false, lineNumber, IniParseErrorCode::MalformedLine};
                }

                bool recognized = false;
                bool valid = true;

#define NFSMW_INI_BOOL_FIELD(field)                                              \
    if (ini_detail::EqualIgnoreCase(key, #field)) {                              \
        recognized = true;                                                        \
        valid = ini_detail::ParseBool(value, candidate.field);                    \
    }
#define NFSMW_INI_FLOAT_FIELD(field)                                             \
    if (ini_detail::EqualIgnoreCase(key, #field)) {                              \
        recognized = true;                                                        \
        valid = ini_detail::ParseFloat(value, candidate.field);                   \
    }

                NFSMW_INI_BOOL_FIELD(enabled)
                NFSMW_INI_BOOL_FIELD(requirePlayer)
                NFSMW_INI_BOOL_FIELD(allowReverse)
                NFSMW_INI_BOOL_FIELD(enableYawVelocityAssist)
                NFSMW_INI_BOOL_FIELD(enableThrottleProtection)
                NFSMW_INI_BOOL_FIELD(useVelocityRelativeDriftAngle)
                NFSMW_INI_FLOAT_FIELD(minSpeedMps)
                NFSMW_INI_FLOAT_FIELD(minLongitudinalSpeedMps)
                if (ini_detail::EqualIgnoreCase(key, "minGroundedWheels")) {
                    recognized = true;
                    valid = ini_detail::ParseUint8(value, candidate.minGroundedWheels);
                }
                NFSMW_INI_FLOAT_FIELD(groundContactLossGraceSeconds)
                NFSMW_INI_FLOAT_FIELD(enterSideslipRad)
                NFSMW_INI_FLOAT_FIELD(exitSideslipRad)
                NFSMW_INI_FLOAT_FIELD(enterRearSlip)
                NFSMW_INI_FLOAT_FIELD(exitRearSlip)
                NFSMW_INI_FLOAT_FIELD(enterHandbrake)
                NFSMW_INI_FLOAT_FIELD(handbrakeActivationHoldSeconds)
                NFSMW_INI_FLOAT_FIELD(handbrakeActivationThreshold)
                NFSMW_INI_FLOAT_FIELD(directionDeadzone)
                NFSMW_INI_BOOL_FIELD(enableSmartCountersteer)
                NFSMW_INI_FLOAT_FIELD(countersteerActivationBodyOffsetRad)
                NFSMW_INI_FLOAT_FIELD(smartCountersteerAngleRad)
                NFSMW_INI_FLOAT_FIELD(smartCountersteerHandoffDelaySeconds)
                NFSMW_INI_FLOAT_FIELD(smartCountersteerReengageDelaySeconds)
                NFSMW_INI_FLOAT_FIELD(manualSteeringTransitionSeconds)
                NFSMW_INI_BOOL_FIELD(enableManualYawAssist)
                NFSMW_INI_FLOAT_FIELD(manualSameDirectionTargetYawRateRadS)
                NFSMW_INI_FLOAT_FIELD(manualSameDirectionYawAccelerationRadS2)
                NFSMW_INI_FLOAT_FIELD(manualSameDirectionMaximumYawAngularDelta)
                NFSMW_INI_FLOAT_FIELD(manualSameDirectionRampSeconds)
                NFSMW_INI_FLOAT_FIELD(manualSameDirectionInitialStrength)
                NFSMW_INI_FLOAT_FIELD(manualOppositeMaximumRecoveryYawRateRadS)
                NFSMW_INI_FLOAT_FIELD(manualOppositeYawDampingAccelerationRadS2)
                NFSMW_INI_FLOAT_FIELD(manualOppositeMaximumYawAngularDelta)
                NFSMW_INI_FLOAT_FIELD(manualYawHardBodyOffsetRad)
                NFSMW_INI_FLOAT_FIELD(pendulumTransitionZeroBandRad)
                NFSMW_INI_FLOAT_FIELD(pendulumTransitionConfirmBodyOffsetRad)
                NFSMW_INI_FLOAT_FIELD(pendulumTransitionWindowSeconds)
                NFSMW_INI_FLOAT_FIELD(sameDirectionFullSteerSeconds)
                NFSMW_INI_FLOAT_FIELD(countersteerTransitionSeconds)
                NFSMW_INI_FLOAT_FIELD(minimumCountersteerAngleRad)
                NFSMW_INI_FLOAT_FIELD(maximumCountersteerAngleRad)
                NFSMW_INI_FLOAT_FIELD(exitBodyOffsetRad)
                NFSMW_INI_FLOAT_FIELD(noDirectionTimeoutSeconds)
                NFSMW_INI_FLOAT_FIELD(minimumDriftBodyOffsetRad)
                NFSMW_INI_FLOAT_FIELD(bodyOffsetHoldSpeedRadS)
                NFSMW_INI_FLOAT_FIELD(bodyOffsetHoldYawAccelerationRadS2)
                NFSMW_INI_FLOAT_FIELD(bodyOffsetHoldMaximumYawAngularDelta)
                NFSMW_INI_FLOAT_FIELD(bodyOffsetHoldSlowdownOffsetRad)
                NFSMW_INI_FLOAT_FIELD(driftAngleEntrySeconds)
                NFSMW_INI_FLOAT_FIELD(driftAngleEntrySpeedRadS)
                NFSMW_INI_FLOAT_FIELD(driftAngleEntryYawAccelerationRadS2)
                NFSMW_INI_FLOAT_FIELD(driftAngleEntryOffsetCutoffRad)
                NFSMW_INI_FLOAT_FIELD(driftAngleEntryMaximumYawAngularDelta)
                NFSMW_INI_FLOAT_FIELD(bodyRotationSpeedRadS)
                NFSMW_INI_FLOAT_FIELD(bodyRotationSlowdownOffsetRad)
                NFSMW_INI_FLOAT_FIELD(entryYawBoostSeconds)
                NFSMW_INI_FLOAT_FIELD(entryYawBoostRateRadS)
                NFSMW_INI_FLOAT_FIELD(entryYawBoostAccelerationRadS2)
                NFSMW_INI_FLOAT_FIELD(entryYawBoostOffsetCutoffRad)
                NFSMW_INI_FLOAT_FIELD(entryYawBoostMaximumYawAngularDelta)
                NFSMW_INI_FLOAT_FIELD(bodyYawAccelerationRadS2)
                NFSMW_INI_FLOAT_FIELD(directionChangeSpeedRadS)
                NFSMW_INI_FLOAT_FIELD(directionChangeYawAccelerationRadS2)
                NFSMW_INI_FLOAT_FIELD(directionChangeMaximumYawAngularDelta)
                NFSMW_INI_FLOAT_FIELD(directionChangeSlowdownOffsetRad)
                NFSMW_INI_FLOAT_FIELD(autoCenterSpeedRadS)
                NFSMW_INI_FLOAT_FIELD(autoCenterYawAccelerationRadS2)
                NFSMW_INI_FLOAT_FIELD(autoCenterMaximumYawAngularDelta)
                NFSMW_INI_FLOAT_FIELD(autoCenterSlowdownOffsetRad)
                NFSMW_INI_FLOAT_FIELD(exitDriftAngleRateRadS)
                NFSMW_INI_FLOAT_FIELD(exitSettleSeconds)
                NFSMW_INI_FLOAT_FIELD(driftAngleRateFilterSeconds)
                NFSMW_INI_FLOAT_FIELD(maximumBodyOffsetRad)
                NFSMW_INI_FLOAT_FIELD(frontGripMultiplierDuringDrift)
                NFSMW_INI_FLOAT_FIELD(rearDriveMultiplierDuringDrift)
                NFSMW_INI_FLOAT_FIELD(targetSideslipRad)
                NFSMW_INI_FLOAT_FIELD(maximumSideslipRad)
                NFSMW_INI_FLOAT_FIELD(wheelbaseM)
                NFSMW_INI_FLOAT_FIELD(maximumSteerAngleRad)
                NFSMW_INI_FLOAT_FIELD(yawReferenceScale)
                NFSMW_INI_FLOAT_FIELD(maximumYawRateRadS)
                NFSMW_INI_FLOAT_FIELD(sideslipKp)
                NFSMW_INI_FLOAT_FIELD(sideslipKd)
                NFSMW_INI_FLOAT_FIELD(yawRateKp)
                NFSMW_INI_FLOAT_FIELD(yawRateDamping)
                NFSMW_INI_FLOAT_FIELD(maximumSteeringCorrection)
                NFSMW_INI_FLOAT_FIELD(maximumSteeringCorrectionRate)
                NFSMW_INI_FLOAT_FIELD(steeringAuthority)
                NFSMW_INI_FLOAT_FIELD(blendInSeconds)
                NFSMW_INI_FLOAT_FIELD(blendOutSeconds)
                NFSMW_INI_FLOAT_FIELD(collisionAuthority)
                NFSMW_INI_FLOAT_FIELD(rollKp)
                NFSMW_INI_FLOAT_FIELD(rollKd)
                NFSMW_INI_FLOAT_FIELD(pitchKp)
                NFSMW_INI_FLOAT_FIELD(pitchKd)
                NFSMW_INI_FLOAT_FIELD(yawVelocityKp)
                NFSMW_INI_FLOAT_FIELD(attitudeAuthority)
                NFSMW_INI_FLOAT_FIELD(airborneAuthority)
                NFSMW_INI_FLOAT_FIELD(maximumRollAngularDelta)
                NFSMW_INI_FLOAT_FIELD(maximumPitchAngularDelta)
                NFSMW_INI_FLOAT_FIELD(maximumYawAngularDelta)
                NFSMW_INI_FLOAT_FIELD(throttleCutAtMaximumSlip)
                NFSMW_INI_FLOAT_FIELD(recoveryBrakeAtMaximumSlip)
                NFSMW_INI_FLOAT_FIELD(recoverySideslipRad)
                if (ini_detail::EqualIgnoreCase(key, "activation")) {
                    recognized = true;
                    valid = ini_detail::ParseActivation(value, candidate.activation);
                }
                if (ini_detail::EqualIgnoreCase(key, "actuation")) {
                    recognized = true;
                    valid = ini_detail::ParseActuation(value, candidate.actuation);
                }

#undef NFSMW_INI_BOOL_FIELD
#undef NFSMW_INI_FLOAT_FIELD

                // Unknown keys are intentionally ignored, including an empty
                // value. Recognized keys retain a useful distinction between a
                // missing value and a syntactically present but invalid value.
                if (recognized && value.empty()) {
                    return {false, lineNumber, IniParseErrorCode::MalformedLine};
                }
                if (recognized && !valid) {
                    return {false, lineNumber, IniParseErrorCode::InvalidValue};
                }
            }
        }

        if (end == text.size()) {
            break;
        }
        start = end + 1;
        ++lineNumber;
    }

    out = candidate;
    return {true, 0, IniParseErrorCode::None};
}

} // namespace nfsmw_drift
