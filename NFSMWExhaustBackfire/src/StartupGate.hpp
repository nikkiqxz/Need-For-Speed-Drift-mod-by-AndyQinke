#pragma once

#include <cstddef>
#include <cstdint>

namespace nfsmw_exhaust::startup_gate {

constexpr wchar_t kAuthorityModuleName[] = L"NFSMW.CGPhysicsFix.asi";
constexpr wchar_t kBindingFileName[] = L"NFSMW.CGPhysicsFix.device.json";
constexpr char kAuthorityExportName[] = "GetNFSMWValidationStatusV1";
constexpr char kExpectedExecutableSha256[] =
    "188FB0748DA83AE54126D1F674AB81D8B8B1CCF9D57311A46CC76D8BF71BCD8A";
constexpr char kCanonicalExecutableSha256[] =
    "941C755553B9D6B6C842AEBA8B7EB22600D828AF69F945B377F4146A833DC41D";
constexpr std::uint32_t kVerificationCode = 868086u;
constexpr std::uint32_t kRequiredSourceMask = 7u;
constexpr std::size_t kMaxPlaintextSize = 2048u;
constexpr std::size_t kMaxCiphertextSize = 4096u;
constexpr std::size_t kMaxBindingJsonSize = 16384u;

enum class AuthorityState : long {
    NotStarted = 0,
    Validating = 1,
    Verified = 2,
    Failed = 3,
};

struct ValidationStatusV1 {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    volatile long state;
    std::uint32_t bindingSchema;
    std::uint32_t deviceSourceMask;
    char authorityModuleSha256[65];
    char executableSha256[65];
    wchar_t bindingPath[260];
    char failureReason[256];
};

bool ValidateAuthorityStatus(const ValidationStatusV1* status, char* reason,
                             std::size_t reasonSize) noexcept;
bool ParseEncryptedBindingJson(const char* json, std::size_t jsonSize,
                               std::uint8_t* ciphertext,
                               std::size_t ciphertextCapacity,
                               std::size_t* ciphertextSize, char* reason,
                               std::size_t reasonSize) noexcept;
bool ValidateDecryptedPayload(const std::uint8_t* plaintext,
                              std::size_t plaintextSize,
                              const char* authorityModuleSha256, char* reason,
                              std::size_t reasonSize) noexcept;
bool ComputeFileSha256(const wchar_t* filePath, char output[65], char* reason,
                       std::size_t reasonSize) noexcept;
bool VerifyAuthoritySnapshot(const ValidationStatusV1* status,
                             const wchar_t* loadedAuthorityPath,
                             const wchar_t* gameExecutablePath, char* reason,
                             std::size_t reasonSize) noexcept;
bool VerifyLoadedAuthority(char* reason, std::size_t reasonSize) noexcept;

}  // namespace nfsmw_exhaust::startup_gate
