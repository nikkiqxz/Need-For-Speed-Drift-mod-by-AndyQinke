#pragma once

#include <cstddef>
#include <cstdint>

namespace startup_gate {

constexpr char kExecutableName[] = "Need For Speed MW-Reforged.exe";
constexpr char kBindingFileName[] = "NFSMWMultiGear.device.json";
// The outer binding envelope is JSON schema 2. Its payload is protected with
// Windows DPAPI and then rendered as uppercase hexadecimal text.
constexpr std::uint32_t kBindingSchema = 2u;
constexpr std::size_t kBindingJsonCapacity = 12288u;
// Keep existing encrypted device bindings byte-compatible. Runtime image
// validation uses the expanded fingerprints below.
constexpr char kBindingExecutableSha256[] =
    "188FB0748DA83AE54126D1F674AB81D8B8B1CCF9D57311A46CC76D8BF71BCD8A";
constexpr char kStampedExecutableMd5[] =
    "97F5A7DFC1CE8F6CCDB6A812C583C3AA";
constexpr char kStampedExecutableSha256[] =
    "5A41CAFA931527F3E0BFEC7C45B3AA1D22428C46F3C185CA23431288F8FF53D1";
constexpr char kFooterOriginalCanonicalSha256[] =
    "941C755553B9D6B6C842AEBA8B7EB22600D828AF69F945B377F4146A833DC41D";
constexpr char kExpandedCanonicalSha256[] =
    "950C261747D06EBE21526CCDD948FC87D5585D9534FDD5D4777BACFA652040C4";
constexpr std::uint64_t kOriginalExecutableSize = 6135808u;
constexpr std::uint64_t kStampedExecutableSize =
    kOriginalExecutableSize + 64u;
constexpr std::uint32_t kOriginalPeChecksum = 0x005DC744u;
constexpr std::uint32_t kVerificationCode = 868086u;
constexpr std::size_t kFooterSize = 64u;

struct DeviceComponents {
    bool hasSystemUuid = false;
    std::uint8_t systemUuid[16] = {};
    char machineGuid[128] = {};
    bool hasSystemVolumeSerial = false;
    std::uint32_t systemVolumeSerial = 0;
};

enum class BindingStatus {
    Created,
    Verified,
    Migrated,
};

struct BindingResult {
    BindingStatus status = BindingStatus::Verified;
    char path[260] = {};
    unsigned sourceMask = 0;
};

bool ValidateFooterBytes(const std::uint8_t* footer, std::size_t size,
                         char* reason, std::size_t reasonSize) noexcept;

bool ValidateExecutable(const char* executablePath, std::uint64_t fileSize,
                        const char* md5, char* reason,
                        std::size_t reasonSize);

bool BuildBindingPath(const char* executablePath, char* output,
                      std::size_t outputSize, char* reason,
                      std::size_t reasonSize) noexcept;

bool ComputeDeviceId(const DeviceComponents& components, char output[65],
                     unsigned* sourceMask, char* reason,
                     std::size_t reasonSize);

bool BuildBindingPayload(const char* deviceId, std::uint32_t schema,
                         char* output, std::size_t outputSize,
                         std::size_t* written, char* reason,
                         std::size_t reasonSize) noexcept;

bool BuildBindingJson(const char* deviceId, char* output,
                      std::size_t outputSize, std::size_t* written,
                      char* reason, std::size_t reasonSize) noexcept;

// Kept solely for a one-time migration from the v0.8.0 plaintext binding.
bool BuildLegacyBindingJson(const char* deviceId, char* output,
                            std::size_t outputSize, std::size_t* written,
                            char* reason, std::size_t reasonSize) noexcept;

bool EnsureBindingFile(const char* path, const char* expected,
                       std::size_t expectedSize, BindingStatus* status,
                       char* reason, std::size_t reasonSize);

// Verifies an encrypted binding by decrypting its payload. If the exact
// legacy plaintext is present, it is atomically upgraded to the new envelope.
bool EnsureEncryptedBindingFile(const char* path, const char* expectedPayload,
                                std::size_t expectedPayloadSize,
                                const char* legacyExpected,
                                std::size_t legacyExpectedSize,
                                BindingStatus* status, char* reason,
                                std::size_t reasonSize);

bool EnsureCurrentDeviceBinding(const char* executablePath,
                                BindingResult* result, char* reason,
                                std::size_t reasonSize);

}  // namespace startup_gate
