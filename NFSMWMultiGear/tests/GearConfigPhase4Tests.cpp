#include "../GearConfig.h"
#include "../PowerMultiplier.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++g_failures;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

bool Near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.000001f;
}

std::string JoinPath(const char* directory, const char* leaf) {
    std::string result = directory != nullptr ? directory : "";
    if (!result.empty() && result.back() != '\\' && result.back() != '/') {
        result.push_back('\\');
    }
    result += leaf;
    return result;
}

bool HasDiagnostic(const gearconfig::Table& table,
                   gearconfig::Diagnostic::Severity severity,
                   std::size_t line,
                   const char* messagePart) {
    for (const gearconfig::Diagnostic& diagnostic : table.Diagnostics()) {
        if (diagnostic.severity != severity || diagnostic.line != line) {
            continue;
        }
        if (messagePart == nullptr ||
            diagnostic.message.find(messagePart) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void CheckCompleteTwelve(const gearconfig::Table& table,
                         const char* name,
                         gearconfig::GearCountMode expectedMode,
                         unsigned expectedDeclaredCount) {
    const gearconfig::Entry* entry = table.Find(name);
    CHECK(entry != nullptr);
    if (entry == nullptr) return;

    static constexpr float expectedRatios[] = {
        0.701f, 0.622f, 0.548f, 0.487f, 0.431f};
    CHECK(entry->declaredCount == expectedDeclaredCount);
    CHECK(entry->countMode == expectedMode);
    CHECK(entry->maxForwardGears == 12);
    CHECK(entry->totalRatioSlots == 14);
    CHECK(entry->RequiredRatioCount() == 5);
    CHECK(entry->ratioCount == 5);
    CHECK(entry->powerMultiplierCount == 0);
    CHECK(!entry->HasExplicitPowerMultipliers());
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(Near(entry->ratios[i], expectedRatios[i]));
        CHECK(Near(entry->powerMultipliers[i], 1.0f));
    }
}

void TestTwelveGearForms(const char* dataDirectory) {
    gearconfig::Table table;
    const std::string path = JoinPath(dataDirectory, "GearConfigPhase4.cfg");

    CHECK(table.Load(path.c_str()));
    CHECK(table.FileOpened());
    // Five complete legacy twelve-gear forms, two partial forms, one legacy
    // eight-gear form, and two complete paired forms are accepted.
    CHECK(table.AcceptedCount() == 10);

    CheckCompleteTwelve(table, "bare12",
                         gearconfig::GearCountMode::ForwardGears, 12);
    CheckCompleteTwelve(table, "forward12",
                         gearconfig::GearCountMode::ForwardGears, 12);
    CheckCompleteTwelve(table, "slots14",
                         gearconfig::GearCountMode::TotalRatioSlots, 14);
    CheckCompleteTwelve(table, "total14",
                         gearconfig::GearCountMode::TotalRatioSlots, 14);
    CheckCompleteTwelve(table, "ratio_count14",
                         gearconfig::GearCountMode::TotalRatioSlots, 14);

    const gearconfig::Entry* partial = table.Find("partial12_four");
    CHECK(partial != nullptr);
    if (partial != nullptr) {
        CHECK(partial->maxForwardGears == 12);
        CHECK(partial->totalRatioSlots == 14);
        CHECK(partial->RequiredRatioCount() == 5);
        CHECK(partial->ratioCount == 4);
        CHECK(Near(partial->ratios[0], 0.701f));
        CHECK(Near(partial->ratios[3], 0.487f));
        CHECK(Near(partial->ratios[4], 0.0f));
    }

    partial = table.Find("PARTIAL12_ONE");
    CHECK(partial != nullptr);
    if (partial != nullptr) {
        CHECK(partial->ratioCount == 1);
        CHECK(partial->RequiredRatioCount() == 5);
        CHECK(Near(partial->ratios[0], 0.701f));
        CHECK(Near(partial->ratios[1], 0.0f));
    }

    const gearconfig::Entry* eight = table.Find("complete8");
    CHECK(eight != nullptr);
    if (eight != nullptr) {
        CHECK(eight->maxForwardGears == 8);
        CHECK(eight->totalRatioSlots == 10);
        CHECK(eight->RequiredRatioCount() == 1);
        CHECK(eight->ratioCount == 1);
        CHECK(Near(eight->ratios[0], 0.639f));
        CHECK(Near(eight->powerMultipliers[0], 1.0f));
        CHECK(!eight->HasExplicitPowerMultipliers());
    }

    const gearconfig::Entry* paired = table.Find("PAIRED12");
    CHECK(paired != nullptr);
    if (paired != nullptr) {
        static constexpr float expectedRatios[] = {
            0.639f, 0.622f, 0.611f, 0.600f, 0.585f};
        static constexpr float expectedMultipliers[] = {
            1.25f, 1.10f, 0.95f, 1.50f, 2.00f};
        CHECK(paired->ratioCount == 5);
        CHECK(paired->powerMultiplierCount == 5);
        CHECK(paired->HasExplicitPowerMultipliers());
        for (std::size_t i = 0; i < 5; ++i) {
            CHECK(Near(paired->ratios[i], expectedRatios[i]));
            CHECK(Near(paired->powerMultipliers[i],
                       expectedMultipliers[i]));
        }
    }

    paired = table.Find("paired8");
    CHECK(paired != nullptr);
    if (paired != nullptr) {
        CHECK(paired->ratioCount == 1);
        CHECK(paired->powerMultiplierCount == 1);
        CHECK(Near(paired->ratios[0], 0.639f));
        CHECK(Near(paired->powerMultipliers[0], 1.75f));
    }

    // Lookup remains case-insensitive and exact, including for the new forms.
    CHECK(table.Find("BARE12") != nullptr);
    CHECK(table.Find("bare12_extra") == nullptr);
    CHECK(table.Find("extra12") == nullptr);
    CHECK(table.Find("missing12") == nullptr);
    CHECK(table.Find("incomplete_pairs") == nullptr);
    CHECK(table.Find("zero_multiplier") == nullptr);
    CHECK(table.Find("negative_multiplier") == nullptr);

    // Partial rows are syntactically valid and carry a warning for the
    // runtime policy; too many values and a missing gear-8 value are errors.
    CHECK(HasDiagnostic(table, gearconfig::Diagnostic::Severity::Warning, 7,
                        "fewer ratios"));
    CHECK(HasDiagnostic(table, gearconfig::Diagnostic::Severity::Warning, 8,
                        "fewer ratios"));
    CHECK(HasDiagnostic(table, gearconfig::Diagnostic::Severity::Error, 12,
                        "complete <ratio> <power-multiplier> pairs"));
    CHECK(HasDiagnostic(table, gearconfig::Diagnostic::Severity::Error, 13,
                        "expected <name> <count> <gear8-ratio>"));
    CHECK(HasDiagnostic(table, gearconfig::Diagnostic::Severity::Error, 14,
                        "complete <ratio> <power-multiplier> pairs"));
    CHECK(HasDiagnostic(table, gearconfig::Diagnostic::Severity::Error, 15,
                        "power multipliers"));
    CHECK(HasDiagnostic(table, gearconfig::Diagnostic::Severity::Error, 16,
                        "power multipliers"));
    CHECK(table.Diagnostics().size() == 7);
}

void TestPowerMultiplierMath() {
    float scaled = 0.0f;
    CHECK(gear_power::ScaleEfficiency(0.92f, 1.0f, &scaled));
    CHECK(Near(scaled, 0.92f));
    CHECK(gear_power::ScaleEfficiency(0.92f, 1.5f, &scaled));
    CHECK(Near(scaled, 1.38f));
    CHECK(!gear_power::ScaleEfficiency(0.92f, 0.0f, &scaled));
    CHECK(!gear_power::ScaleEfficiency(0.92f, -1.0f, &scaled));

    const float baseResult = 120.0f;
    const float baseRatio = 0.639f;
    const float requestedRatio = 0.600f;
    const float baseEfficiency = 0.92f;
    const float legacyExpected =
        baseResult * (baseRatio / requestedRatio);
    CHECK(gear_power::ScaleRatioEfficiencyResult(
        baseResult, baseRatio, baseEfficiency, requestedRatio,
        baseEfficiency, &scaled));
    CHECK(Near(scaled, legacyExpected));
    CHECK(scaled == legacyExpected);

    const float requestedEfficiency = baseEfficiency * 1.5f;
    CHECK(gear_power::ScaleRatioEfficiencyResult(
        baseResult, baseRatio, baseEfficiency, requestedRatio,
        requestedEfficiency, &scaled));
    CHECK(Near(scaled, legacyExpected / 1.5f));
    CHECK(!gear_power::ScaleRatioEfficiencyResult(
        baseResult, baseRatio, baseEfficiency, requestedRatio, 0.0f,
        &scaled));
}

void TestPublishedEfficiencyIsolation() {
    static constexpr float kBaseEfficiency = 0.65f;
    static constexpr float kMultipliers[] = {2.0f, 0.1f, 2.0f, 0.1f, 4.0f};
    float published[5] = {};
    float powered[5] = {};

    CHECK(gear_power::BuildEfficiencyViews(
        kBaseEfficiency, kMultipliers, 5, published, powered));
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(published[i] == kBaseEfficiency);
        CHECK(Near(powered[i], kBaseEfficiency * kMultipliers[i]));
    }

    static constexpr float kInvalidMultipliers[] = {1.0f, 0.0f};
    CHECK(!gear_power::BuildEfficiencyViews(
        kBaseEfficiency, kInvalidMultipliers, 2, published, powered));
}

void TestPowerMultiplierIsolation() {
    static constexpr float kBaseEfficiency = 0.65f;
    static constexpr float kMultipliers[] = {2.0f, 0.1f, 2.0f, 0.1f, 4.0f};
    float value = 0.0f;

    // Native reverse/neutral/gears 1-7 remain bit-for-bit neutral even when a
    // caller accidentally supplies an extended-gear multiplier.
    for (std::uint32_t raw = 0; raw <= 8; ++raw) {
        for (float multiplier : kMultipliers) {
            CHECK(gear_power::SelectRuntimeEfficiency(
                kBaseEfficiency, multiplier, raw,
                static_cast<std::int32_t>(raw), 9, 13, true, &value));
            CHECK(value == kBaseEfficiency);
        }
        // A static low-gear query must remain neutral even while the vehicle
        // is currently in any configured high gear.
        for (std::int32_t current = 9; current <= 13; ++current) {
            CHECK(gear_power::SelectRuntimeEfficiency(
                kBaseEfficiency, kMultipliers[current - 9], raw, current, 9,
                13, true, &value));
            CHECK(value == kBaseEfficiency);
        }
    }

    // Each configured multiplier applies only to the matching live gear.
    for (std::uint32_t raw = 9; raw <= 13; ++raw) {
        const float multiplier = kMultipliers[raw - 9];
        CHECK(gear_power::SelectRuntimeEfficiency(
            kBaseEfficiency, multiplier, raw,
            static_cast<std::int32_t>(raw), 9, 13, true, &value));
        CHECK(Near(value, kBaseEfficiency * multiplier));

        // Matching current/query values are insufficient on their own. A
        // non-power caller must still receive the neutral static value.
        CHECK(gear_power::SelectRuntimeEfficiency(
            kBaseEfficiency, multiplier, raw,
            static_cast<std::int32_t>(raw), 9, 13, false, &value));
        CHECK(value == kBaseEfficiency);

        CHECK(gear_power::SelectRuntimeEfficiency(
            kBaseEfficiency, multiplier, raw, 2, 9, 13, true, &value));
        CHECK(value == kBaseEfficiency);
        CHECK(gear_power::SelectRuntimeEfficiency(
            kBaseEfficiency, multiplier, raw,
            static_cast<std::int32_t>(raw == 13 ? 12 : raw + 1), 9, 13, true,
            &value));
        CHECK(value == kBaseEfficiency);
        CHECK(gear_power::SelectRuntimeEfficiency(
            kBaseEfficiency, multiplier, raw, -1, 9, 13, true, &value));
        CHECK(value == kBaseEfficiency);
        CHECK(gear_power::SelectRuntimeEfficiency(
            kBaseEfficiency, multiplier, raw, 14, 9, 13, true, &value));
        CHECK(value == kBaseEfficiency);
    }

    // A high-low-high-low sequence must not leave a multiplied value behind.
    static constexpr std::int32_t kSequence[] = {
        9, 2, 10, 3, 11, 4, 12, 5, 13, 8};
    for (std::int32_t current : kSequence) {
        const std::uint32_t query = static_cast<std::uint32_t>(current);
        const float multiplier =
            current >= 9 ? kMultipliers[current - 9] : 4.0f;
        CHECK(gear_power::SelectRuntimeEfficiency(
            kBaseEfficiency, multiplier, query, current, 9, 13, true,
            &value));
        if (current >= 9) {
            CHECK(Near(value, kBaseEfficiency * multiplier));
        } else {
            CHECK(value == kBaseEfficiency);
        }
    }

    CHECK(gear_power::SelectRuntimeEfficiency(
        kBaseEfficiency, kMultipliers[2], 11, 11, 9, 13, true, &value));
    CHECK(Near(value, kBaseEfficiency * kMultipliers[2]));
    CHECK(gear_power::SelectRuntimeEfficiency(
        kBaseEfficiency, kMultipliers[2], 11, 11, 9, 13, false, &value));
    CHECK(value == kBaseEfficiency);
    CHECK(gear_power::SelectRuntimeEfficiency(
        kBaseEfficiency, kMultipliers[2], 11, -1, 9, 13, true, &value));
    CHECK(value == kBaseEfficiency);
}

}  // namespace

int main(int argc, char** argv) {
    const char* dataDirectory = argc >= 2 ? argv[1] : "tests\\data";
    TestTwelveGearForms(dataDirectory);
    TestPowerMultiplierMath();
    TestPublishedEfficiencyIsolation();
    TestPowerMultiplierIsolation();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d GearConfig Phase 4 assertion(s) failed\n",
                     g_failures);
        return 1;
    }

    std::puts("GearConfig Phase 4 tests passed");
    return 0;
}
