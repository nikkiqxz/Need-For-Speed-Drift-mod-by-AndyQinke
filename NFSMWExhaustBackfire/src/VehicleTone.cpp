#include "VehicleTone.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace nfsmw_exhaust::vehicle_tone {
namespace {

constexpr std::uint32_t kGoldenRatio = 0x9E3779B9u;
constexpr std::uint32_t kSeed = 0xABCDEF00u;

void mix(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c) noexcept {
    a -= b; a -= c; a ^= c >> 13;
    b -= c; b -= a; b ^= a << 8;
    c -= a; c -= b; c ^= b >> 13;
    a -= b; a -= c; a ^= c >> 12;
    b -= c; b -= a; b ^= a << 16;
    c -= a; c -= b; c ^= b >> 5;
    a -= b; a -= c; a ^= c >> 3;
    b -= c; b -= a; b ^= a << 10;
    c -= a; c -= b; c ^= b >> 15;
}

std::uint32_t readU32(const unsigned char* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool validName(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaximumVehicleNameLength) return false;
    for (const char value : name) {
        const unsigned char byte = static_cast<unsigned char>(value);
        if (byte < 0x21u || byte > 0x7Eu || value == '=' || value == '[' ||
            value == ']') return false;
    }
    return true;
}

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
           value.front() == '\r' || value.front() == '\n')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
           value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
    return value;
}

bool equalIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const char a = left[index] >= 'A' && left[index] <= 'Z'
                           ? static_cast<char>(left[index] - 'A' + 'a')
                           : left[index];
        const char b = right[index] >= 'A' && right[index] <= 'Z'
                           ? static_cast<char>(right[index] - 'A' + 'a')
                           : right[index];
        if (a != b) return false;
    }
    return true;
}

bool parseFloat(std::string_view text, float* value) noexcept {
    if (value == nullptr || text.empty() || text.size() >= 64) return false;
    std::array<char, 64> buffer{};
    std::copy(text.begin(), text.end(), buffer.begin());
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(buffer.data(), &end);
    if (errno != 0 || end != buffer.data() + text.size() ||
        !std::isfinite(parsed)) return false;
    *value = parsed;
    return true;
}

bool fail(std::size_t line, const char* reason, std::string* error,
          std::size_t* errorLine) noexcept {
    if (errorLine != nullptr) *errorLine = line;
    if (error != nullptr) {
        try {
            *error = std::string(reason) + " on line " + std::to_string(line);
        } catch (...) {
            *error = reason;
        }
    }
    return false;
}

}  // namespace

const Entry* Registry::find(std::uint32_t collectionKey) const noexcept {
    if (collectionKey == 0) return nullptr;
    for (std::size_t index = 0; index < count; ++index)
        if (entries[index].collectionKey == collectionKey) return &entries[index];
    return nullptr;
}

float Registry::characterFor(std::uint32_t collectionKey) const noexcept {
    const Entry* entry = find(collectionKey);
    return entry != nullptr ? entry->character : kDefaultCharacter;
}

std::uint32_t bChunkHash(std::string_view name) noexcept {
    if (!validName(name)) return 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(name.data());
    std::uint32_t a = kGoldenRatio;
    std::uint32_t b = kGoldenRatio;
    std::uint32_t c = kSeed;
    std::size_t position = 0;
    std::size_t remaining = name.size();
    while (remaining >= 12) {
        a += readU32(bytes + position);
        b += readU32(bytes + position + 4);
        c += readU32(bytes + position + 8);
        mix(a, b, c);
        position += 12;
        remaining -= 12;
    }
    c += static_cast<std::uint32_t>(name.size());
    if (remaining >= 11) c += static_cast<std::uint32_t>(bytes[position + 10]) << 24;
    if (remaining >= 10) c += static_cast<std::uint32_t>(bytes[position + 9]) << 16;
    if (remaining >= 9) c += static_cast<std::uint32_t>(bytes[position + 8]) << 8;
    if (remaining >= 8) b += static_cast<std::uint32_t>(bytes[position + 7]) << 24;
    if (remaining >= 7) b += static_cast<std::uint32_t>(bytes[position + 6]) << 16;
    if (remaining >= 6) b += static_cast<std::uint32_t>(bytes[position + 5]) << 8;
    if (remaining >= 5) b += static_cast<std::uint32_t>(bytes[position + 4]);
    if (remaining >= 4) a += static_cast<std::uint32_t>(bytes[position + 3]) << 24;
    if (remaining >= 3) a += static_cast<std::uint32_t>(bytes[position + 2]) << 16;
    if (remaining >= 2) a += static_cast<std::uint32_t>(bytes[position + 1]) << 8;
    if (remaining >= 1) a += static_cast<std::uint32_t>(bytes[position]);
    mix(a, b, c);
    return c;
}

std::uint32_t hashVehicleName(std::string_view name) noexcept {
    if (!validName(name)) return 0;
    std::array<char, kMaximumVehicleNameLength> normalized{};
    for (std::size_t index = 0; index < name.size(); ++index) {
        const char value = name[index];
        normalized[index] = value >= 'A' && value <= 'Z'
                                ? static_cast<char>(value - 'A' + 'a')
                                : value;
    }
    return bChunkHash(std::string_view(normalized.data(), name.size()));
}

float varyBatchCharacter(float character, float maximumPull,
                         float fullNeutralDistance, float maximumOvershoot,
                         float variationRoll) noexcept {
    const float normalizedCharacter =
        std::clamp(character, kMinimumCharacter, kMaximumCharacter);
    const float distance = std::fabs(normalizedCharacter - kDefaultCharacter);
    const float threshold = std::clamp(fullNeutralDistance, 0.01f, 1.0f);
    const float ratio = std::clamp(distance / threshold, 0.0f, 1.0f);
    const float smoothRatio = ratio * ratio * (3.0f - 2.0f * ratio);
    const float pull = std::clamp(maximumPull, 0.0f, 1.0f) * smoothRatio;
    const float pulled = normalizedCharacter +
        (kDefaultCharacter - normalizedCharacter) * pull;
    const float roll = std::clamp(variationRoll, 0.0f, 1.0f);
    const float overshoot = std::clamp(maximumOvershoot, 0.0f, 1.0f) *
                            (1.0f - pull);
    if (distance < 0.000001f) {
        return std::clamp(kDefaultCharacter +
                              (roll * 2.0f - 1.0f) * overshoot,
                          kMinimumCharacter, kMaximumCharacter);
    }
    const float direction = normalizedCharacter < kDefaultCharacter
                                ? 1.0f
                                : -1.0f;
    return std::clamp(pulled + direction * roll * overshoot,
                      kMinimumCharacter, kMaximumCharacter);
}

bool parseIni(std::string_view text, Registry* registry, std::string* error,
              std::size_t* errorLine) noexcept {
    if (registry == nullptr) return fail(0, "null registry", error, errorLine);
    if (error != nullptr) error->clear();
    if (errorLine != nullptr) *errorLine = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEFu &&
        static_cast<unsigned char>(text[1]) == 0xBBu &&
        static_cast<unsigned char>(text[2]) == 0xBFu) text.remove_prefix(3);

    Registry candidate{};
    bool inToneSection = false;
    bool inVariationSection = false;
    std::size_t lineNumber = 1;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        const std::size_t comment = line.find_first_of("#;");
        if (comment != std::string_view::npos) line = line.substr(0, comment);
        line = trim(line);
        if (!line.empty()) {
            if (line.front() == '[') {
                const std::size_t close = line.find(']');
                if (close == std::string_view::npos ||
                    !trim(line.substr(close + 1)).empty()) {
                    return fail(lineNumber, "malformed section", error, errorLine);
                }
                const std::string_view section = trim(line.substr(1, close - 1));
                if (section.empty())
                    return fail(lineNumber, "empty section", error, errorLine);
                inToneSection =
                    equalIgnoreCase(section, "BackfireToneByVehicle");
                inVariationSection =
                    equalIgnoreCase(section, "BackfireToneVariation");
            } else if (inVariationSection) {
                const std::size_t equals = line.find('=');
                if (equals == std::string_view::npos)
                    return fail(lineNumber, "missing variation value", error,
                                errorLine);
                const std::string_view key = trim(line.substr(0, equals));
                const std::string_view value = trim(line.substr(equals + 1));
                float parsed = 0.0f;
                if (!parseFloat(value, &parsed) || parsed < 0.0f ||
                    parsed > 1.0f) {
                    return fail(lineNumber, "invalid tone variation value",
                                error, errorLine);
                }
                if (equalIgnoreCase(
                        key, "batch_toward_neutral_probability")) {
                    candidate.batchTowardNeutralProbability = parsed;
                } else if (equalIgnoreCase(
                               key, "batch_toward_neutral_amount")) {
                    candidate.batchTowardNeutralAmount = parsed;
                } else if (equalIgnoreCase(
                               key, "batch_full_neutral_distance")) {
                    candidate.batchFullNeutralDistance = parsed;
                } else if (equalIgnoreCase(
                               key, "batch_max_neutral_overshoot")) {
                    candidate.batchMaxNeutralOvershoot = parsed;
                } else {
                    return fail(lineNumber, "unknown tone variation setting",
                                error, errorLine);
                }
            } else if (inToneSection) {
                std::string_view name;
                std::string_view value;
                const std::size_t equals = line.find('=');
                if (equals != std::string_view::npos) {
                    name = trim(line.substr(0, equals));
                    value = trim(line.substr(equals + 1));
                } else {
                    const std::size_t separator = line.find_first_of(" \t\r\n");
                    if (separator == std::string_view::npos)
                        return fail(lineNumber, "missing tone value", error, errorLine);
                    name = line.substr(0, separator);
                    value = trim(line.substr(separator));
                }
                float character = 0.0f;
                const std::uint32_t key = hashVehicleName(name);
                if (key == 0 || !parseFloat(value, &character) ||
                    character < kMinimumCharacter || character > kMaximumCharacter ||
                    candidate.count >= candidate.entries.size() ||
                    candidate.find(key) != nullptr) {
                    return fail(lineNumber, "invalid or duplicate vehicle tone",
                                error, errorLine);
                }
                Entry& entry = candidate.entries[candidate.count++];
                entry.collectionKey = key;
                entry.character = character;
                for (std::size_t index = 0; index < name.size(); ++index) {
                    const char valueChar = name[index];
                    entry.name[index] = valueChar >= 'A' && valueChar <= 'Z'
                                            ? static_cast<char>(valueChar - 'A' + 'a')
                                            : valueChar;
                }
            }
        }
        if (end == text.size()) break;
        start = end + 1;
        ++lineNumber;
    }
    *registry = candidate;
    return true;
}

bool loadFile(const char* path, Registry* registry, std::string* error) noexcept {
    if (path == nullptr || registry == nullptr) {
        if (error != nullptr) *error = "null configuration argument";
        return false;
    }
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            if (error != nullptr) *error = "could not open configuration file";
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        return parseIni(text, registry, error);
    } catch (...) {
        if (error != nullptr) *error = "configuration read failed";
        return false;
    }
}

}  // namespace nfsmw_exhaust::vehicle_tone
