#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nfsmw_exhaust::vehicle_tone {

constexpr std::size_t kMaximumEntries = 512;
constexpr std::size_t kMaximumVehicleNameLength = 63;
constexpr float kDefaultCharacter = 1.0f;
constexpr float kMinimumCharacter = 0.0f;
constexpr float kMaximumCharacter = 2.0f;
constexpr float kDefaultBatchTowardNeutralProbability = 0.30f;
constexpr float kDefaultBatchTowardNeutralAmount = 1.0f;
constexpr float kDefaultBatchFullNeutralDistance = 0.60f;
constexpr float kDefaultBatchMaxNeutralOvershoot = 0.20f;

struct Entry {
    std::uint32_t collectionKey = 0;
    float character = kDefaultCharacter;
    std::array<char, kMaximumVehicleNameLength + 1> name{};
};

struct Registry {
    std::array<Entry, kMaximumEntries> entries{};
    std::size_t count = 0;
    float batchTowardNeutralProbability =
        kDefaultBatchTowardNeutralProbability;
    float batchTowardNeutralAmount = kDefaultBatchTowardNeutralAmount;
    float batchFullNeutralDistance = kDefaultBatchFullNeutralDistance;
    float batchMaxNeutralOvershoot = kDefaultBatchMaxNeutralOvershoot;

    const Entry* find(std::uint32_t collectionKey) const noexcept;
    float characterFor(std::uint32_t collectionKey) const noexcept;
};

std::uint32_t bChunkHash(std::string_view name) noexcept;
std::uint32_t hashVehicleName(std::string_view name) noexcept;
float varyBatchCharacter(float character, float maximumPull,
                         float fullNeutralDistance, float maximumOvershoot,
                         float variationRoll) noexcept;

bool parseIni(std::string_view text, Registry* registry, std::string* error,
              std::size_t* errorLine = nullptr) noexcept;
bool loadFile(const char* path, Registry* registry,
              std::string* error) noexcept;

}  // namespace nfsmw_exhaust::vehicle_tone
