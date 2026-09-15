#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nfsmw_drift::asi_host::reforged_startup_gate {

constexpr wchar_t kExecutableName[] = L"speed.exe";
constexpr wchar_t kBindingFileName[] =
    L"Slippery_Drifting_FlashFish_by_AndyQinke.device.json";
constexpr char kProductName[] =
    "Slippery_Drifting_FlashFish_by_AndyQinke";
// Keep the encrypted binding payload byte-compatible with 0.9.16. The
// runtime executable basename is validated independently via kExecutableName.
constexpr char kBindingExecutableIdentity[] =
    "Need For Speed MW-Reforged.exe";
constexpr char kBindingExecutableSha256[] =
    "188FB0748DA83AE54126D1F674AB81D8B8B1CCF9D57311A46CC76D8BF71BCD8A";
constexpr char kStampedExecutableMd5[] =
    "FCBBAC633822546B4B5D86DB5F2F03DE";
constexpr char kStampedExecutableSha256[] =
    "188FB0748DA83AE54126D1F674AB81D8B8B1CCF9D57311A46CC76D8BF71BCD8A";
constexpr char kFooterOriginalCanonicalSha256[] =
    "941C755553B9D6B6C842AEBA8B7EB22600D828AF69F945B377F4146A833DC41D";
constexpr char kExpandedCanonicalSha256[] =
    "941C755553B9D6B6C842AEBA8B7EB22600D828AF69F945B377F4146A833DC41D";
constexpr std::uint64_t kOriginalExecutableSize = 6135808u;
constexpr std::uint64_t kStampedExecutableSize =
    kOriginalExecutableSize + 64u;
constexpr std::uint32_t kOriginalPeChecksum = 0x005DC744u;
constexpr std::uint32_t kVerificationCode = 868086u;
constexpr std::size_t kFooterSize = 64u;
constexpr std::size_t kHardwareIdSize = 64u;
constexpr std::size_t kMaxPlaintextSize = 2048u;
constexpr std::size_t kMaxCiphertextSize = 4096u;
constexpr std::size_t kMaxBindingJsonSize = 16384u;

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
};

struct BindingResult {
    BindingStatus status = BindingStatus::Verified;
    std::wstring path;
    unsigned sourceMask = 0;
};

// Pure format/validation helpers exposed for focused unit tests.
bool ValidateFooterBytes(const std::uint8_t* footer, std::size_t size,
                         char* reason, std::size_t reasonSize) noexcept;

// Accepts non-empty printable ASCII, trims surrounding ASCII whitespace, and
// uppercases ASCII letters. It intentionally does not require GUID punctuation.
bool NormalizeMachineGuidAscii(const char* input, char output[128],
                               char* reason,
                               std::size_t reasonSize) noexcept;

bool ConstantTimeEqual(const std::uint8_t* first, std::size_t firstSize,
                       const std::uint8_t* second,
                       std::size_t secondSize) noexcept;

bool HexEncodeUpper(const std::uint8_t* input, std::size_t inputSize,
                    char* output, std::size_t outputSize,
                    std::size_t* written, char* reason,
                    std::size_t reasonSize) noexcept;

bool HexDecodeUpper(const char* input, std::size_t inputSize,
                    std::uint8_t* output, std::size_t outputSize,
                    std::size_t* written, char* reason,
                    std::size_t reasonSize) noexcept;

bool ComputeHardwareId(const DeviceComponents& components, char output[65],
                       unsigned* sourceMask, char* reason,
                       std::size_t reasonSize);

bool BuildPlaintextPayload(const char* hardwareId, char* output,
                           std::size_t outputSize, std::size_t* written,
                           char* reason, std::size_t reasonSize) noexcept;

bool BuildEncryptedBindingJson(const std::uint8_t* ciphertext,
                               std::size_t ciphertextSize, char* output,
                               std::size_t outputSize, std::size_t* written,
                               char* reason,
                               std::size_t reasonSize) noexcept;

bool ParseEncryptedBindingJson(const char* json, std::size_t jsonSize,
                               std::uint8_t* ciphertext,
                               std::size_t ciphertextCapacity,
                               std::size_t* ciphertextSize, char* reason,
                               std::size_t reasonSize) noexcept;

bool BuildBindingPath(std::wstring_view executablePath, std::wstring* output,
                      char* reason, std::size_t reasonSize);

// OS-facing helpers. They must run outside DllMain/loader lock.
bool ValidateExecutable(const wchar_t* executablePath, char* reason,
                        std::size_t reasonSize);

bool QueryDeviceComponents(DeviceComponents* components, char* reason,
                           std::size_t reasonSize);

bool ProtectPayloadForLocalMachine(const std::uint8_t* plaintext,
                                   std::size_t plaintextSize,
                                   std::vector<std::uint8_t>* ciphertext,
                                   char* reason, std::size_t reasonSize);

bool UnprotectPayloadForLocalMachine(const std::uint8_t* ciphertext,
                                     std::size_t ciphertextSize,
                                     std::vector<std::uint8_t>* plaintext,
                                     char* reason, std::size_t reasonSize);

// This lower-level function is useful for filesystem/race tests. The expected
// bytes are the deterministic plaintext, not a previously encrypted blob.
bool EnsureBindingFile(const wchar_t* path,
                       const std::uint8_t* expectedPlaintext,
                       std::size_t expectedPlaintextSize,
                       BindingStatus* status, char* reason,
                       std::size_t reasonSize);

// ValidateExecutable must have succeeded before calling this directly.
bool EnsureCurrentDeviceBinding(const wchar_t* executablePath,
                                BindingResult* result, char* reason,
                                std::size_t reasonSize);

// Resolve and validate the current process image without touching the
// filesystem outside the executable itself.  Callers that need to install
// hooks should run their in-memory profile checks after this function and
// create/verify the device binding only after those checks pass.
bool ValidateCurrentProcess(std::wstring* executablePath, char* reason,
                            std::size_t reasonSize);

// Single fail-closed entry point for the delayed ASI worker. It validates the
// exact main executable before creating or inspecting any binding file.
bool VerifyCurrentProcessAndEnsureBinding(BindingResult* result, char* reason,
                                          std::size_t reasonSize);

}  // namespace nfsmw_drift::asi_host::reforged_startup_gate
