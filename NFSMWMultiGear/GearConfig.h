#pragma once

// Parser for the optional per-transmission multi-gear configuration.
//
// A data line has either the legacy ratio-only form or the complete paired
// ratio/power-multiplier form:
//
//     997_top 10 0.639
//     997_top forward=10 0.639 0.542 0.461
//     name    N  gear8 gear9 gear10 gear11 gear12
//     name    N  gear8 power8 gear9 power9 ... gear12 power12
//
// A bare N is the number of forward gears, matching the user-facing format:
// `997_top 10 0.639` requests ten forward gears. `slots=N` (or `total=N`)
// explicitly uses MW's GEAR_RATIO array-slot count, including reverse and
// neutral; for example, `slots=10` means eight forward gears. `forward=N` is
// an explicit spelling of the bare form. N must resolve to 8..12 forward gears
// (10..14 total slots). The first ratio is always eighth gear. A caller can
// use ratioCount to decide whether it has enough values for the requested N.
// Partial legacy rows remain parseable so the log can identify them, but the
// runtime skips them rather than deriving missing ratios. Power multipliers
// default to 1.0 for every complete legacy row.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gearconfig {

constexpr std::size_t kMaxExtraRatios = 5; // gears 8 through 12
constexpr unsigned kMinForwardGears = 8;
constexpr unsigned kMaxForwardGears = 12;
constexpr unsigned kMinRatioSlots = kMinForwardGears + 2;
constexpr unsigned kMaxRatioSlots = kMaxForwardGears + 2;

enum class GearCountMode {
    // The declared count includes reverse and neutral, as GEAR_RATIO does.
    TotalRatioSlots,
    // The declared count is forward gears only.
    ForwardGears,
};

struct Entry {
    // Name as written in the file, retained for diagnostics/display.
    std::string name;

    // Raw second field from the file, before normalising count semantics.
    unsigned declaredCount = 0;
    GearCountMode countMode = GearCountMode::ForwardGears;

    // Number of GEAR_RATIO slots requested (reverse + neutral + forward).
    unsigned totalRatioSlots = 0;

    // Number of forward gears requested by this entry (8..12).
    unsigned maxForwardGears = 0;

    // ratios[0] is gear 8, ratios[1] is gear 9, etc.
    std::array<float, kMaxExtraRatios> ratios{};
    std::size_t ratioCount = 0;

    // powerMultipliers[0] is gear 8, etc. These scale drivetrain efficiency,
    // leaving the speed/RPM relationship defined by the corresponding ratio
    // unchanged. Legacy ratio-only rows retain the 1.0 defaults.
    std::array<float, kMaxExtraRatios> powerMultipliers{
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::size_t powerMultiplierCount = 0;

    // One-based source line, useful when reporting a malformed override.
    std::size_t line = 0;

    // Number of extra ratio values needed to cover every requested gear.
    std::size_t RequiredRatioCount() const noexcept {
        return maxForwardGears > 7 ? maxForwardGears - 7 : 0;
    }

    bool HasExplicitPowerMultipliers() const noexcept {
        return powerMultiplierCount != 0;
    }
};

struct Diagnostic {
    enum class Severity { Warning, Error };

    Severity severity = Severity::Warning;
    std::size_t line = 0;
    std::string message;
};

class Table {
public:
    // Loads a text file. A missing file is not an error: the table remains
    // empty, allowing the plugin to behave exactly as the unconfigured game.
    // Returns false only for a non-missing I/O or allocation failure.
    bool Load(const char* path);

    // Case-insensitive exact lookup. No wildcard, prefix, or fallback match is
    // performed; an absent key therefore cannot affect any vehicle.
    const Entry* Find(std::string_view transmissionName) const noexcept;

    // Optional bridge for the game's hashed collection keys. The callback is
    // supplied by the game layer (for MW05 this is bStringHash); no hash
    // algorithm is duplicated here. The lower-case form is tried as well so
    // an otherwise valid upper-case config name still resolves to MW's usual
    // lower-case identifier. A null callback never matches.
    using HashFunction = std::uint32_t (*)(const char*);
    const Entry* FindHash(std::uint32_t hash, HashFunction hashFunction) const noexcept;

    void Clear() noexcept;
    bool FileOpened() const noexcept { return fileOpened_; }
    std::size_t AcceptedCount() const noexcept { return entries_.size(); }

    const std::vector<Entry>& Entries() const noexcept { return entries_; }
    const std::vector<Diagnostic>& Diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    std::vector<Entry> entries_;
    std::vector<Diagnostic> diagnostics_;
    bool fileOpened_ = false;
};

} // namespace gearconfig
