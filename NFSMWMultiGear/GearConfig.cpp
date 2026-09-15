#include "GearConfig.h"

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace gearconfig {
namespace {

void AddDiagnostic(std::vector<Diagnostic>* diagnostics,
                   Diagnostic::Severity severity,
                   std::size_t line,
                   const char* message) {
    diagnostics->push_back(Diagnostic{severity, line, message});
}

// MW's VLT identifiers are ASCII hashes. Fold only ASCII here so matching is
// deterministic regardless of the process locale.
char FoldAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool EqualFolded(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (FoldAscii(lhs[i]) != FoldAscii(rhs[i])) return false;
    }
    return true;
}

bool IsIdentifier(std::string_view name) {
    if (name.empty() || name.size() > 63) return false;
    // Do not permit wildcard or path syntax. A row always names one exact
    // transmission collection; this is what keeps unlisted cars untouched.
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool ParseUnsigned(std::string_view text, unsigned* out) {
    if (text.empty()) return false;
    std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(copy.c_str(), &end, 10);
    if (errno == ERANGE || end == copy.c_str() || *end != '\0' ||
        value > std::numeric_limits<unsigned>::max()) {
        return false;
    }
    *out = static_cast<unsigned>(value);
    return true;
}

bool StartsWithFolded(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    return EqualFolded(text.substr(0, prefix.size()), prefix);
}

struct ParsedCount {
    GearCountMode mode = GearCountMode::ForwardGears;
    unsigned declared = 0;
    unsigned totalSlots = 0;
    unsigned forwardGears = 0;
};

bool ParseCount(std::string_view text, ParsedCount* out) {
    GearCountMode mode = GearCountMode::TotalRatioSlots;
    std::string_view number = text;

    if (StartsWithFolded(text, "forward=")) {
        mode = GearCountMode::ForwardGears;
        number = text.substr(8);
    } else if (StartsWithFolded(text, "slots=")) {
        mode = GearCountMode::TotalRatioSlots;
        number = text.substr(6);
    } else if (StartsWithFolded(text, "total=")) {
        mode = GearCountMode::TotalRatioSlots;
        number = text.substr(6);
    } else if (StartsWithFolded(text, "ratio_count=")) {
        mode = GearCountMode::TotalRatioSlots;
        number = text.substr(12);
    } else {
        // The public three-column format uses a forward-gear count. This is
        // intentionally not inferred from the numeric value: `10` means ten
        // forward gears, while users needing MW's raw slot count write
        // `slots=10`.
        mode = GearCountMode::ForwardGears;
    }

    unsigned declared = 0;
    if (!ParseUnsigned(number, &declared)) return false;

    unsigned forward = 0;
    unsigned total = 0;
    if (mode == GearCountMode::ForwardGears) {
        if (declared < kMinForwardGears || declared > kMaxForwardGears) return false;
        forward = declared;
        total = declared + 2;
    } else {
        if (declared < kMinRatioSlots || declared > kMaxRatioSlots) return false;
        total = declared;
        forward = declared - 2;
    }

    out->mode = mode;
    out->declared = declared;
    out->totalSlots = total;
    out->forwardGears = forward;
    return true;
}

bool ParsePositiveFloat(std::string_view text, float* out) {
    if (text.empty()) return false;
    float value = 0.0f;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(value) || value <= 0.0f) {
        return false;
    }
    *out = value;
    return true;
}

void StripComment(std::string* line) {
    // Config syntax intentionally has no quoting. The earliest marker wins.
    std::size_t cut = line->find_first_of("#;");
    const std::size_t slash = line->find("//");
    if (slash != std::string::npos && (cut == std::string::npos || slash < cut)) {
        cut = slash;
    }
    if (cut != std::string::npos) line->erase(cut);
}

void Trim(std::string* text) {
    const auto isSpace = [](unsigned char c) { return c <= 0x20; };
    std::size_t first = 0;
    while (first < text->size() && isSpace(static_cast<unsigned char>((*text)[first]))) {
        ++first;
    }
    std::size_t last = text->size();
    while (last > first && isSpace(static_cast<unsigned char>((*text)[last - 1]))) {
        --last;
    }
    if (first != 0 || last != text->size()) *text = text->substr(first, last - first);
}

std::vector<std::string_view> Tokens(std::string* line) {
    // Commas are accepted as a convenience, but no other punctuation is
    // silently treated as a separator. Views remain valid for this line.
    for (char& c : *line) {
        if (c == ',') c = ' ';
    }

    std::vector<std::string_view> result;
    std::size_t i = 0;
    while (i < line->size()) {
        while (i < line->size() && static_cast<unsigned char>((*line)[i]) <= 0x20) ++i;
        if (i == line->size()) break;
        const std::size_t begin = i;
        while (i < line->size() && static_cast<unsigned char>((*line)[i]) > 0x20) ++i;
        result.emplace_back(line->data() + begin, i - begin);
    }
    return result;
}

void ParseLine(std::string* line,
               std::size_t lineNumber,
               std::vector<Entry>* entries,
               std::vector<Diagnostic>* diagnostics) {
    StripComment(line);
    Trim(line);
    if (line->empty()) return;

    // A UTF-8 BOM is legal at the beginning of the first line.
    if (lineNumber == 1 && line->size() >= 3 &&
        static_cast<unsigned char>((*line)[0]) == 0xEF &&
        static_cast<unsigned char>((*line)[1]) == 0xBB &&
        static_cast<unsigned char>((*line)[2]) == 0xBF) {
        line->erase(0, 3);
        Trim(line);
    }

    const std::vector<std::string_view> fields = Tokens(line);
    if (fields.size() < 3) {
        AddDiagnostic(diagnostics, Diagnostic::Severity::Error, lineNumber,
                      "expected <name> <count> <gear8-ratio>");
        return;
    }
    if (!IsIdentifier(fields[0])) {
        AddDiagnostic(diagnostics, Diagnostic::Severity::Error, lineNumber,
                      "invalid transmission name (only ASCII letters, digits, and '_' are allowed)");
        return;
    }

    ParsedCount count;
    if (!ParseCount(fields[1], &count)) {
        AddDiagnostic(diagnostics, Diagnostic::Severity::Error, lineNumber,
                      "gear count must resolve to 8..12 forward gears (10..14 total slots); use forward=N or slots=N");
        return;
    }

    const std::size_t expectedRatios = count.forwardGears - kMinForwardGears + 1;
    const std::size_t valueCount = fields.size() - 2;
    const bool hasPowerMultipliers = valueCount == expectedRatios * 2;
    if (valueCount > expectedRatios && !hasPowerMultipliers) {
        AddDiagnostic(diagnostics, Diagnostic::Severity::Error, lineNumber,
                      "expected either one ratio per requested gear or complete <ratio> <power-multiplier> pairs");
        return;
    }

    Entry entry;
    entry.name.assign(fields[0].data(), fields[0].size());
    entry.declaredCount = count.declared;
    entry.countMode = count.mode;
    entry.totalRatioSlots = count.totalSlots;
    entry.maxForwardGears = count.forwardGears;
    entry.line = lineNumber;
    if (hasPowerMultipliers) {
        for (std::size_t i = 0; i < expectedRatios; ++i) {
            if (!ParsePositiveFloat(fields[2 + i * 2], &entry.ratios[i])) {
                AddDiagnostic(diagnostics, Diagnostic::Severity::Error,
                              lineNumber,
                              "gear ratios must be finite positive numbers");
                return;
            }
            if (!ParsePositiveFloat(fields[3 + i * 2],
                                    &entry.powerMultipliers[i])) {
                AddDiagnostic(
                    diagnostics, Diagnostic::Severity::Error, lineNumber,
                    "power multipliers must be finite positive numbers");
                return;
            }
            ++entry.ratioCount;
            ++entry.powerMultiplierCount;
        }
    } else {
        for (std::size_t i = 2; i < fields.size(); ++i) {
            if (!ParsePositiveFloat(fields[i], &entry.ratios[i - 2])) {
                AddDiagnostic(diagnostics, Diagnostic::Severity::Error,
                              lineNumber,
                              "gear ratios must be finite positive numbers");
                return;
            }
            ++entry.ratioCount;
        }
    }

    if (entry.ratioCount < expectedRatios) {
        AddDiagnostic(diagnostics, Diagnostic::Severity::Warning, lineNumber,
                      "fewer ratios than the requested gear count; the runtime will skip this entry");
    }

    // Last valid row wins. This permits a user-local override later in the
    // file while retaining a single unambiguous lookup result.
    for (Entry& old : *entries) {
        if (EqualFolded(old.name, entry.name)) {
            old = std::move(entry);
            AddDiagnostic(diagnostics, Diagnostic::Severity::Warning, lineNumber,
                          "duplicate transmission name; later row replaces the earlier one");
            return;
        }
    }
    entries->push_back(std::move(entry));
}

} // namespace

bool Table::Load(const char* path) {
    Clear();
    if (path == nullptr || *path == '\0') return true;

    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        // A missing optional file is intentionally equivalent to an empty
        // table. Other open failures (for example, a directory or an access
        // failure) are reported to the caller.
        struct _stat info {};
        errno = 0;
        if (_stat(path, &info) != 0 && errno == ENOENT) return true;
        AddDiagnostic(&diagnostics_, Diagnostic::Severity::Error, 0,
                      "could not open gear configuration");
        return false;
    }
    fileOpened_ = true;

    try {
        std::string line;
        std::size_t lineNumber = 0;
        while (std::getline(input, line)) {
            ++lineNumber;
            ParseLine(&line, lineNumber, &entries_, &diagnostics_);
        }
        if (input.bad()) {
            AddDiagnostic(&diagnostics_, Diagnostic::Severity::Error, lineNumber,
                          "I/O error while reading gear configuration");
            return false;
        }
    } catch (...) {
        AddDiagnostic(&diagnostics_, Diagnostic::Severity::Error, 0,
                      "out of memory while parsing gear configuration");
        return false;
    }
    return true;
}

const Entry* Table::Find(std::string_view transmissionName) const noexcept {
    for (const Entry& entry : entries_) {
        if (EqualFolded(entry.name, transmissionName)) return &entry;
    }
    return nullptr;
}

const Entry* Table::FindHash(std::uint32_t hash, HashFunction hashFunction) const noexcept {
    if (hashFunction == nullptr) return nullptr;
    for (const Entry& entry : entries_) {
        if (hashFunction(entry.name.c_str()) == hash) return &entry;

        // MW identifiers are conventionally lower-case. Fold into a fixed
        // buffer so this bridge stays allocation-free and noexcept.
        char folded[64] = {};
        if (entry.name.size() >= sizeof(folded)) continue;
        for (std::size_t i = 0; i < entry.name.size(); ++i) {
            folded[i] = FoldAscii(entry.name[i]);
        }
        if (hashFunction(folded) == hash) return &entry;
    }
    return nullptr;
}

void Table::Clear() noexcept {
    entries_.clear();
    diagnostics_.clear();
    fileOpened_ = false;
}

} // namespace gearconfig
