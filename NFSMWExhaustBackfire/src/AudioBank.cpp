#include "nfsmw_exhaust/AudioBank.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

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

bool parseKey(const std::string& key, std::size_t* clipIndex) {
    if (clipIndex == nullptr || key.size() != 6 ||
        key.compare(0, 4, "clip") != 0 || !std::isdigit(key[4]) ||
        !std::isdigit(key[5])) {
        return false;
    }
    const std::size_t number =
        static_cast<std::size_t>(key[4] - '0') * 10u +
        static_cast<std::size_t>(key[5] - '0');
    if (number == 0 || number > AudioBank::kClipCount) return false;
    *clipIndex = number - 1u;
    return true;
}

}  // namespace

AudioBank::AudioBank()
    : assetIds_{
          "backfire.clip01", "backfire.clip02", "backfire.clip03",
          "backfire.clip04", "backfire.clip05", "backfire.clip06",
          "backfire.clip07", "backfire.clip08", "backfire.clip09",
          "backfire.clip10", "backfire.clip11", "backfire.clip12"} {}

const char* AudioBank::assetId(std::size_t clipIndex) const noexcept {
    if (clipIndex >= kClipCount) return nullptr;
    return assetIds_[clipIndex].c_str();
}

AudioCue AudioBank::at(std::size_t clipIndex) const noexcept {
    if (clipIndex >= kClipCount) return {};
    return AudioCue{static_cast<std::uint8_t>(clipIndex),
                    assetIds_[clipIndex].c_str()};
}

bool AudioBank::loadManifest(const char* path, std::string* error) {
    if (error != nullptr) error->clear();
    if (path == nullptr) {
        if (error != nullptr) *error = "null audio manifest path";
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) *error = "could not open audio manifest";
        return false;
    }

    auto candidate = assetIds_;
    std::array<bool, kClipCount> seen{};
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
                *error = "missing '=' on audio manifest line " +
                         std::to_string(lineNumber);
            }
            return false;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        std::size_t index = 0;
        if (!parseKey(key, &index) || value.empty()) {
            if (error != nullptr) {
                *error = "invalid audio manifest entry '" + key + "'";
            }
            return false;
        }
        if (seen[index]) {
            if (error != nullptr) {
                *error = "duplicate audio manifest entry '" + key + "'";
            }
            return false;
        }
        seen[index] = true;
        candidate[index] = value;
    }

    if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
        if (error != nullptr) {
            *error = "audio manifest must define all 12 flat clip slots";
        }
        return false;
    }

    assetIds_ = candidate;
    return true;
}

}  // namespace nfsmw_exhaust
