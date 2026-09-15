#pragma once

#include "drift_assist_ini.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace nfsmw_drift_asi::vehicle_countersteer {

constexpr std::size_t kMaximumEntries = 512;
constexpr std::size_t kMaximumVehicleNameLength = 63;
constexpr float kMinimumMultiplier = 0.0f;
constexpr float kMaximumMultiplier = 2.0f;

struct Entry {
    std::uint32_t collectionKey = 0;
    float multiplier = 1.0f;
    std::array<char, kMaximumVehicleNameLength + 1> name{};
};

struct Registry {
    std::array<Entry, kMaximumEntries> entries{};
    std::size_t count = 0;

    const Entry* Find(std::uint32_t collectionKey) const {
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

    float MultiplierFor(std::uint32_t collectionKey) const {
        const Entry* const entry = Find(collectionKey);
        return entry != nullptr ? entry->multiplier : 1.0f;
    }
};

namespace detail {

constexpr std::uint32_t kGoldenRatio = 0x9E3779B9u;
constexpr std::uint32_t kSeed = 0xABCDEF00u;

inline void Mix(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c) {
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

inline std::uint32_t ReadLittleEndianU32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

inline bool ValidVehicleName(std::string_view name) {
    if (name.empty() || name.size() > kMaximumVehicleNameLength) {
        return false;
    }
    for (const char value : name) {
        const unsigned char byte = static_cast<unsigned char>(value);
        if (byte < 0x21u || byte > 0x7Eu || value == '=' || value == '[' ||
            value == ']') {
            return false;
        }
    }
    return true;
}

inline std::size_t FindWhitespace(std::string_view value) noexcept {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == ' ' || value[index] == '\t' ||
            value[index] == '\r' || value[index] == '\n') {
            return index;
        }
    }
    return std::string_view::npos;
}

}  // namespace detail

inline std::uint32_t BChunkHash(std::string_view name) {
    if (!detail::ValidVehicleName(name)) {
        return 0;
    }
    const auto* const bytes = reinterpret_cast<const unsigned char*>(
        name.data());

    std::uint32_t a = detail::kGoldenRatio;
    std::uint32_t b = detail::kGoldenRatio;
    std::uint32_t c = detail::kSeed;
    std::size_t position = 0;
    std::size_t remaining = name.size();
    while (remaining >= 12) {
        a += detail::ReadLittleEndianU32(bytes + position);
        b += detail::ReadLittleEndianU32(bytes + position + 4);
        c += detail::ReadLittleEndianU32(bytes + position + 8);
        detail::Mix(a, b, c);
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
    detail::Mix(a, b, c);
    return c;
}

// NFSMW's Attrib::StringToKey is Bob Jenkins' 1996 mix3 with the EA seed.
// Vehicle collection names are normalized to lowercase ASCII so INI lookup is
// case-insensitive while matching the lowercase pvehicle collection names.
inline std::uint32_t HashVehicleName(std::string_view name) {
    std::array<char, kMaximumVehicleNameLength> normalized{};
    if (!detail::ValidVehicleName(name)) {
        return 0;
    }
    for (std::size_t index = 0; index < name.size(); ++index) {
        const char value = name[index];
        normalized[index] =
            value >= 'A' && value <= 'Z'
                ? static_cast<char>(value - 'A' + 'a')
                : value;
    }
    return BChunkHash(std::string_view(normalized.data(), name.size()));
}

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
                    section, "SmartCountersteerVehicleMultipliers");
            } else if (inSection) {
                std::string_view name{};
                std::string_view value{};
                const std::size_t equals = line.find('=');
                if (equals != std::string_view::npos) {
                    name = nfsmw_drift::ini_detail::Trim(
                        line.substr(0, equals));
                    value = nfsmw_drift::ini_detail::Trim(
                        line.substr(equals + 1));
                } else {
                    const std::size_t separator =
                        detail::FindWhitespace(line);
                    if (separator == std::string_view::npos) {
                        return {false, lineNumber,
                                nfsmw_drift::IniParseErrorCode::MalformedLine};
                    }
                    name = line.substr(0, separator);
                    value = nfsmw_drift::ini_detail::Trim(
                        line.substr(separator));
                }
                float multiplier = 0.0f;
                const std::uint32_t key = HashVehicleName(name);
                if (key == 0 ||
                    !nfsmw_drift::ini_detail::ParseFloat(value, multiplier) ||
                    multiplier < kMinimumMultiplier ||
                    multiplier > kMaximumMultiplier ||
                    candidate.count >= candidate.entries.size()) {
                    return {false, lineNumber,
                            nfsmw_drift::IniParseErrorCode::InvalidValue};
                }
                if (candidate.Find(key) != nullptr) {
                    return {false, lineNumber,
                            nfsmw_drift::IniParseErrorCode::InvalidValue};
                }
                Entry& entry = candidate.entries[candidate.count++];
                entry.collectionKey = key;
                entry.multiplier = multiplier;
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

}  // namespace nfsmw_drift_asi::vehicle_countersteer
