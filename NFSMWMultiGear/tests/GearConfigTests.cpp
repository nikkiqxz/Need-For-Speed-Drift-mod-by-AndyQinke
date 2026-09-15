#include "../GearConfig.h"

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

bool HasDiagnostic(const gearconfig::Table& table,
                   gearconfig::Diagnostic::Severity severity,
                   std::size_t line) {
    for (const gearconfig::Diagnostic& diagnostic : table.Diagnostics()) {
        if (diagnostic.severity == severity && diagnostic.line == line) return true;
    }
    return false;
}

std::string JoinPath(const char* directory, const char* leaf) {
    std::string result = directory != nullptr ? directory : "";
    if (!result.empty() && result.back() != '\\' && result.back() != '/') {
        result.push_back('\\');
    }
    result += leaf;
    return result;
}

void TestMixedConfig(const char* dataDirectory) {
    gearconfig::Table table;
    const std::string path = JoinPath(dataDirectory, "GearConfigMixed.cfg");

    CHECK(table.Load(path.c_str()));
    CHECK(table.FileOpened());
    CHECK(table.AcceptedCount() == 3);

    // A bare count is always a forward-gear count. The user's three-column
    // example therefore requests ten forward gears, not ten array slots.
    const gearconfig::Entry* bare = table.Find("997_TOP");
    CHECK(bare != nullptr);
    if (bare != nullptr) {
        CHECK(bare->declaredCount == 10);
        CHECK(bare->countMode == gearconfig::GearCountMode::ForwardGears);
        CHECK(bare->maxForwardGears == 10);
        CHECK(bare->totalRatioSlots == 12);
        CHECK(bare->RequiredRatioCount() == 3);
        CHECK(bare->ratioCount == 1);
        CHECK(Near(bare->ratios[0], 0.639f));
        CHECK(bare->powerMultiplierCount == 0);
        CHECK(!bare->HasExplicitPowerMultipliers());
        CHECK(Near(bare->powerMultipliers[0], 1.0f));
    }

    // Raw GEAR_RATIO count includes reverse and neutral. slots=14 therefore
    // resolves to twelve forward gears and consumes ratios for gears 8..12.
    const gearconfig::Entry* slots = table.Find("viper_top");
    CHECK(slots != nullptr);
    if (slots != nullptr) {
        CHECK(slots->declaredCount == 14);
        CHECK(slots->countMode == gearconfig::GearCountMode::TotalRatioSlots);
        CHECK(slots->maxForwardGears == 12);
        CHECK(slots->totalRatioSlots == 14);
        CHECK(slots->RequiredRatioCount() == 5);
        CHECK(slots->ratioCount == 5);
        CHECK(Near(slots->ratios[0], 0.701f));
        CHECK(Near(slots->ratios[4], 0.431f));
        CHECK(slots->powerMultiplierCount == 0);
        for (float multiplier : slots->powerMultipliers) {
            CHECK(Near(multiplier, 1.0f));
        }
    }

    // Matching is case-insensitive but exact. The later duplicate replaces
    // the earlier row without adding a second entry.
    const gearconfig::Entry* duplicate = table.Find("duplicate_top");
    CHECK(duplicate != nullptr);
    if (duplicate != nullptr) {
        CHECK(duplicate->line == 5);
        CHECK(duplicate->ratioCount == 1);
        CHECK(Near(duplicate->ratios[0], 0.750f));
    }
    CHECK(table.Find("duplicate") == nullptr);
    CHECK(table.Find("unlisted_top") == nullptr);

    // Line 2 is valid but omits the gear 9 and 10 ratios. Line 5 is the
    // duplicate override. Lines 6..12 are deliberately malformed.
    CHECK(HasDiagnostic(table, gearconfig::Diagnostic::Severity::Warning, 2));
    CHECK(HasDiagnostic(table, gearconfig::Diagnostic::Severity::Warning, 5));
    for (std::size_t line = 6; line <= 12; ++line) {
        CHECK(HasDiagnostic(table, gearconfig::Diagnostic::Severity::Error, line));
    }
    CHECK(table.Diagnostics().size() == 9);
}

void TestMissingFile(const char* dataDirectory) {
    gearconfig::Table table;
    const std::string path = JoinPath(dataDirectory, "this-file-must-not-exist.cfg");

    CHECK(table.Load(path.c_str()));
    CHECK(!table.FileOpened());
    CHECK(table.AcceptedCount() == 0);
    CHECK(table.Diagnostics().empty());
    CHECK(table.Find("997_top") == nullptr);
}

} // namespace

int main(int argc, char** argv) {
    const char* dataDirectory = argc >= 2 ? argv[1] : "tests\\data";
    TestMixedConfig(dataDirectory);
    TestMissingFile(dataDirectory);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d GearConfig assertion(s) failed\n", g_failures);
        return 1;
    }

    std::puts("GearConfig tests passed");
    return 0;
}
