#include "nfsmw_exhaust/ConfigFile.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <utility>

namespace nfsmw_exhaust {
namespace {

std::string trim(std::string value) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    while (!value.empty() && !notSpace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && !notSpace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool parseUnsigned(const std::string& value, std::uint32_t* output) {
    if (output == nullptr || value.empty() || value.front() == '-') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
    if (errno != 0 || end == value.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    *output = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parseFloat(const std::string& value, float* output) {
    if (output == nullptr || value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value.c_str(), &end);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    *output = parsed;
    return true;
}

bool parseBool(const std::string& value, bool* output) {
    if (output == nullptr) return false;
    if (value == "1" || value == "true" || value == "TRUE" ||
        value == "yes" || value == "on") {
        *output = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "FALSE" ||
        value == "no" || value == "off") {
        *output = false;
        return true;
    }
    return false;
}

bool setValue(const std::string& key, const std::string& value,
              ExhaustConfig* config) {
    std::uint32_t integer = 0;
    float decimal = 0.0f;
    bool flag = false;

    if (key == "burst_min_shots" && parseUnsigned(value, &integer)) {
        config->burstMinShots = integer;
    } else if (key == "burst_max_shots" && parseUnsigned(value, &integer)) {
        config->burstMaxShots = integer;
    } else if (key == "downshift_burst_min_shots" &&
               parseUnsigned(value, &integer)) {
        config->downshiftBurstMinShots = integer;
    } else if (key == "downshift_burst_max_shots" &&
               parseUnsigned(value, &integer)) {
        config->downshiftBurstMaxShots = integer;
    } else if (key == "burst_window_ms" && parseUnsigned(value, &integer)) {
        config->burstWindowMs = integer;
    } else if (key == "sustained_threshold_ms" && parseUnsigned(value, &integer)) {
        config->sustainedThresholdMs = integer;
    } else if (key == "sustained_min_interval_ms" &&
               parseUnsigned(value, &integer)) {
        config->sustainedMinIntervalMs = integer;
    } else if (key == "sustained_max_interval_ms" &&
               parseUnsigned(value, &integer)) {
        config->sustainedMaxIntervalMs = integer;
    } else if (key == "low_max_rpm" && parseFloat(value, &decimal)) {
        config->lowMaxRpm = decimal;
    } else if (key == "high_max_rpm" && parseFloat(value, &decimal)) {
        config->highMaxRpm = decimal;
    } else if (key == "low_probability" && parseFloat(value, &decimal)) {
        config->lowProbability = decimal;
    } else if (key == "high_probability" && parseFloat(value, &decimal)) {
        config->highProbability = decimal;
    } else if (key == "upshift_probability_scale" &&
               parseFloat(value, &decimal)) {
        config->upshiftProbabilityScale = decimal;
    } else if (key == "downshift_probability_scale" &&
               parseFloat(value, &decimal)) {
        config->downshiftProbabilityScale = decimal;
    } else if (key == "near_limit_ratio" && parseFloat(value, &decimal)) {
        config->nearLimitRatio = decimal;
    } else if (key == "downshift_min_rpm_ratio" &&
               parseFloat(value, &decimal)) {
        config->downshiftMinRpmRatio = decimal;
    } else if (key == "downshift_min_control_input" &&
               parseFloat(value, &decimal)) {
        config->downshiftMinControlInput = decimal;
    } else if (key == "sustained_limit_ratio" &&
               parseFloat(value, &decimal)) {
        config->sustainedLimitRatio = decimal;
    } else if (key == "sustained_probability" &&
               parseFloat(value, &decimal)) {
        config->sustainedProbability = decimal;
    } else if (key == "neutral_sustained_probability" &&
               parseFloat(value, &decimal)) {
        config->neutralSustainedProbability = decimal;
    } else if (key == "neutral_min_gas_input" &&
               parseFloat(value, &decimal)) {
        config->neutralMinGasInput = decimal;
    } else if (key == "flame_audio_probability" &&
               parseFloat(value, &decimal)) {
        config->flameAudioProbability = decimal;
    } else if (key == "paired_shift_mode" && parseBool(value, &flag)) {
        config->pairedShiftMode = flag;
    } else if (key == "shift_simultaneous_probability" &&
               parseFloat(value, &decimal)) {
        config->shiftSimultaneousProbability = decimal;
    } else if (key == "shift_sequential_probability" &&
               parseFloat(value, &decimal)) {
        config->shiftSequentialProbability = decimal;
    } else if (key == "paired_downshift_probability" &&
               parseFloat(value, &decimal)) {
        config->pairedDownshiftProbability = decimal;
    } else if (key == "paired_side_delay_ms" &&
               parseUnsigned(value, &integer)) {
        config->pairedSideDelayMs = integer;
    } else if (key == "nitrous_start_probability" &&
               parseFloat(value, &decimal)) {
        config->nitrousStartProbability = decimal;
    } else if (key == "nitrous_end_probability" &&
               parseFloat(value, &decimal)) {
        config->nitrousEndProbability = decimal;
    } else if (key == "nitrous_sequential_probability" &&
               parseFloat(value, &decimal)) {
        config->nitrousSequentialProbability = decimal;
    } else if (key == "require_both_markers" && parseBool(value, &flag)) {
        config->requireBothMarkers = flag;
    } else if (key == "suppress_vanilla" && parseBool(value, &flag)) {
        config->suppressVanilla = flag;
    } else if (key == "max_tracked_vehicles" && parseUnsigned(value, &integer)) {
        config->maxTrackedVehicles = integer;
    } else if (key == "effect_variant" && parseUnsigned(value, &integer)) {
        config->effectVariant = integer;
    } else if (key == "random_seed" && parseUnsigned(value, &integer)) {
        config->randomSeed = integer;
    } else if (key == "effect_id") {
        config->effectId = value;
    } else {
        return false;
    }
    return true;
}

}  // namespace

bool loadConfigFile(const char* path, ExhaustConfig* config,
                   std::string* error) {
    if (error != nullptr) error->clear();
    if (path == nullptr || config == nullptr) {
        if (error != nullptr) *error = "null configuration argument";
        return false;
    }

    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) *error = "could not open configuration file";
        return false;
    }

    ExhaustConfig candidate = *config;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.erase(comment);
        line = trim(line);
        if (line.empty() || line.front() == '[') continue;

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            if (error != nullptr) {
                *error = "missing '=' on line " + std::to_string(lineNumber);
            }
            return false;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key.empty() || !setValue(key, value, &candidate)) {
            if (error != nullptr) {
                *error = "invalid value for '" + key + "' on line " +
                         std::to_string(lineNumber);
            }
            return false;
        }
    }

    candidate.normalize();
    *config = std::move(candidate);
    return true;
}

}  // namespace nfsmw_exhaust
