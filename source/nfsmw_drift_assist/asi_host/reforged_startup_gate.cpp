#include "reforged_startup_gate.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <new>

#if defined(_MSC_VER)
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Crypt32.lib")
#endif

namespace nfsmw_drift::asi_host::reforged_startup_gate {
namespace {

constexpr std::size_t kChecksumOffset = 0x160u;
constexpr DWORD kRawSmbiosProvider = 0x52534D42u;  // 'RSMB'
constexpr unsigned kSystemUuidSource = 1u << 0;
constexpr unsigned kMachineGuidSource = 1u << 1;
constexpr unsigned kSystemVolumeSource = 1u << 2;
constexpr unsigned kRequiredSourceMask =
    kSystemUuidSource | kMachineGuidSource | kSystemVolumeSource;
constexpr wchar_t kDpapiDescription[] =
    L"Slippery Drifting FlashFish device binding";
constexpr char kDpapiEntropy[] =
    "NFSMW.Slippery_Drifting_FlashFish_by_AndyQinke.DeviceBinding.v1";

constexpr char kBindingJsonPrefix[] =
    "{\r\n"
    "  \"schema\": 1,\r\n"
    "  \"product\": \"Slippery_Drifting_FlashFish_by_AndyQinke\",\r\n"
    "  \"format\": \"DPAPI_LOCAL_MACHINE_HEX_V1\",\r\n"
    "  \"ciphertext_hex\": \"";
constexpr char kBindingJsonSuffix[] = "\"\r\n}\r\n";

void SetReason(char* reason, std::size_t reasonSize, const char* format, ...) {
    if (reason == nullptr || reasonSize == 0) return;
    va_list args;
    va_start(args, format);
    _vsnprintf_s(reason, reasonSize, _TRUNCATE, format, args);
    va_end(args);
}

void ClearReason(char* reason, std::size_t reasonSize) noexcept {
    if (reason != nullptr && reasonSize != 0) reason[0] = '\0';
}

template <typename T>
void WipeVectorStorage(std::vector<T>* value) noexcept {
    if (value == nullptr || value->data() == nullptr) return;
    const std::size_t capacity = value->capacity();
    const std::size_t maximum = std::numeric_limits<std::size_t>::max();
    if (capacity <= maximum / sizeof(T)) {
        SecureZeroMemory(value->data(), capacity * sizeof(T));
    } else if (!value->empty()) {
        SecureZeroMemory(value->data(), value->size() * sizeof(T));
    }
}

template <typename T>
class ScopedVectorWipe {
public:
    explicit ScopedVectorWipe(std::vector<T>* value) noexcept : value_(value) {}
    ScopedVectorWipe(const ScopedVectorWipe&) = delete;
    ScopedVectorWipe& operator=(const ScopedVectorWipe&) = delete;
    ~ScopedVectorWipe() {
        WipeVectorStorage(value_);
    }

private:
    std::vector<T>* value_;
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle() {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }
    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_;
};

template <typename T>
void WipeAndClearVector(std::vector<T>* value) noexcept {
    WipeVectorStorage(value);
    if (value != nullptr) value->clear();
}

std::uint32_t ReadLe32(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

void WriteLe32(std::uint32_t value, std::uint8_t output[4]) noexcept {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8u);
    output[2] = static_cast<std::uint8_t>(value >> 16u);
    output[3] = static_cast<std::uint8_t>(value >> 24u);
}

int UpperHexNibble(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool DecodeKnownHex(const char* text, std::uint8_t* output,
                    std::size_t outputSize) noexcept {
    if (text == nullptr || output == nullptr) return false;
    for (std::size_t index = 0; index < outputSize; ++index) {
        const int high = UpperHexNibble(text[index * 2u]);
        const int low = UpperHexNibble(text[index * 2u + 1u]);
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return text[outputSize * 2u] == '\0';
}

struct HashSession {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
};

bool BeginHash(ALG_ID algorithm, HashSession* session) noexcept {
    if (session == nullptr) return false;
    *session = {};
    if (!CryptAcquireContextW(&session->provider, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        return false;
    }
    if (!CryptCreateHash(session->provider, algorithm, 0, 0, &session->hash)) {
        CryptReleaseContext(session->provider, 0);
        *session = {};
        return false;
    }
    return true;
}

void EndHash(HashSession* session) noexcept {
    if (session == nullptr) return;
    if (session->hash != 0) CryptDestroyHash(session->hash);
    if (session->provider != 0) CryptReleaseContext(session->provider, 0);
    *session = {};
}

bool HashBytes(HCRYPTHASH hash, const void* data, std::size_t size) noexcept {
    if (size == 0) return true;
    if (hash == 0 || data == nullptr || size > MAXDWORD) return false;
    return CryptHashData(hash, static_cast<const BYTE*>(data),
                         static_cast<DWORD>(size), 0) != FALSE;
}

bool FinishHash(HCRYPTHASH hash, std::uint8_t* output,
                DWORD expectedSize) noexcept {
    if (hash == 0 || output == nullptr || expectedSize == 0) return false;
    DWORD actualSize = expectedSize;
    return CryptGetHashParam(hash, HP_HASHVAL, output, &actualSize, 0) != FALSE &&
           actualSize == expectedSize;
}

bool HashField(HCRYPTHASH hash, const char* name, const void* data,
               std::size_t size) noexcept {
    if (name == nullptr || (data == nullptr && size != 0) ||
        size > UINT32_MAX) {
        return false;
    }
    const std::size_t nameSize = std::strlen(name);
    if (nameSize > UINT32_MAX) return false;
    std::uint8_t nameLength[4] = {};
    std::uint8_t dataLength[4] = {};
    WriteLe32(static_cast<std::uint32_t>(nameSize), nameLength);
    WriteLe32(static_cast<std::uint32_t>(size), dataLength);
    return HashBytes(hash, nameLength, sizeof(nameLength)) &&
           HashBytes(hash, name, nameSize) &&
           HashBytes(hash, dataLength, sizeof(dataLength)) &&
           HashBytes(hash, data, size);
}

bool ReadFileAt(HANDLE file, std::uint64_t offset, void* destination,
                DWORD size) noexcept {
    if (file == INVALID_HANDLE_VALUE || destination == nullptr) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    DWORD total = 0;
    while (total < size) {
        DWORD current = 0;
        if (!ReadFile(file, static_cast<std::uint8_t*>(destination) + total,
                      size - total, &current, nullptr) ||
            current == 0) {
            return false;
        }
        total += current;
    }
    return true;
}

bool ComputeExecutableDigests(HANDLE file, std::uint8_t md5[16],
                              std::uint8_t fullSha256[32],
                              std::uint8_t canonicalSha256[32], char* reason,
                              std::size_t reasonSize) noexcept {
    if (file == INVALID_HANDLE_VALUE || md5 == nullptr ||
        fullSha256 == nullptr || canonicalSha256 == nullptr) {
        SetReason(reason, reasonSize, "invalid executable hash input");
        return false;
    }

    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        SetReason(reason, reasonSize, "could not rewind the Reforged executable");
        return false;
    }

    HashSession md5Session{};
    HashSession fullShaSession{};
    HashSession canonicalShaSession{};
    bool success = false;
    do {
        if (!BeginHash(CALG_MD5, &md5Session) ||
            !BeginHash(CALG_SHA_256, &fullShaSession) ||
            !BeginHash(CALG_SHA_256, &canonicalShaSession)) {
            SetReason(reason, reasonSize,
                      "could not initialize executable hash providers");
            break;
        }

        std::uint8_t buffer[16u * 1024u] = {};
        std::uint8_t canonicalBuffer[sizeof(buffer)] = {};
        std::uint64_t offset = 0;
        for (;;) {
            DWORD bytesRead = 0;
            if (!ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr)) {
                SetReason(reason, reasonSize,
                          "could not read the Reforged executable");
                break;
            }
            if (bytesRead == 0) {
                if (offset != kStampedExecutableSize) {
                    SetReason(reason, reasonSize,
                              "the Reforged executable size changed while reading");
                    break;
                }
                if (!FinishHash(md5Session.hash, md5, 16u) ||
                    !FinishHash(fullShaSession.hash, fullSha256, 32u) ||
                    !FinishHash(canonicalShaSession.hash, canonicalSha256,
                                32u)) {
                    SetReason(reason, reasonSize,
                              "could not finish executable hashes");
                    break;
                }
                success = true;
                break;
            }

            if (!HashBytes(md5Session.hash, buffer, bytesRead) ||
                !HashBytes(fullShaSession.hash, buffer, bytesRead)) {
                SetReason(reason, reasonSize,
                          "could not hash the Reforged executable");
                break;
            }

            if (offset < kOriginalExecutableSize) {
                const std::uint64_t remaining = kOriginalExecutableSize - offset;
                const DWORD canonicalBytes = static_cast<DWORD>(
                    remaining < bytesRead ? remaining : bytesRead);
                std::memcpy(canonicalBuffer, buffer, canonicalBytes);
                const std::uint64_t chunkEnd = offset + canonicalBytes;
                const std::uint64_t checksumEnd = kChecksumOffset + 4u;
                if (offset < checksumEnd && chunkEnd > kChecksumOffset) {
                    const std::size_t begin = static_cast<std::size_t>(
                        kChecksumOffset > offset ? kChecksumOffset - offset : 0u);
                    const std::size_t end = static_cast<std::size_t>(
                        checksumEnd < chunkEnd ? checksumEnd - offset
                                               : canonicalBytes);
                    std::memset(canonicalBuffer + begin, 0, end - begin);
                }
                if (!HashBytes(canonicalShaSession.hash, canonicalBuffer,
                               canonicalBytes)) {
                    SetReason(reason, reasonSize,
                              "could not hash the canonical game image");
                    break;
                }
            }
            offset += bytesRead;
        }
        SecureZeroMemory(buffer, sizeof(buffer));
        SecureZeroMemory(canonicalBuffer, sizeof(canonicalBuffer));
    } while (false);

    EndHash(&md5Session);
    EndHash(&fullShaSession);
    EndHash(&canonicalShaSession);
    return success;
}

bool IsUsableUuid(const std::uint8_t* uuid) noexcept {
    if (uuid == nullptr) return false;
    bool anyNonZero = false;
    bool anyNonFF = false;
    for (std::size_t index = 0; index < 16u; ++index) {
        anyNonZero = anyNonZero || uuid[index] != 0;
        anyNonFF = anyNonFF || uuid[index] != 0xFFu;
    }
    return anyNonZero && anyNonFF;
}

bool ReadSystemUuid(std::uint8_t output[16]) {
    if (output == nullptr) return false;
    const UINT size = GetSystemFirmwareTable(kRawSmbiosProvider, 0, nullptr, 0);
    if (size < 12u || size > 4u * 1024u * 1024u) return false;

    try {
        std::vector<std::uint8_t> data(size);
        ScopedVectorWipe<std::uint8_t> dataWipe(&data);
        if (GetSystemFirmwareTable(kRawSmbiosProvider, 0, data.data(), size) !=
            size) {
            return false;
        }
        const std::uint32_t tableLength = ReadLe32(data.data() + 4u);
        if (tableLength > size - 8u) return false;

        const std::size_t end = 8u + tableLength;
        std::size_t position = 8u;
        while (position + 4u <= end) {
            const std::uint8_t type = data[position];
            const std::uint8_t length = data[position + 1u];
            if (length < 4u || position + length > end) return false;
            if (type == 1u && length >= 0x19u) {
                const std::uint8_t* uuid = data.data() + position + 8u;
                if (IsUsableUuid(uuid)) {
                    std::memcpy(output, uuid, 16u);
                    return true;
                }
            }
            if (type == 127u) break;

            std::size_t next = position + length;
            while (next + 1u < end &&
                   !(data[next] == 0 && data[next + 1u] == 0)) {
                ++next;
            }
            if (next + 1u >= end) return false;
            position = next + 2u;
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    return false;
}

bool ReadMachineGuid(char output[128], char* reason,
                     std::size_t reasonSize) noexcept {
    if (output == nullptr) return false;
    output[0] = '\0';
    HKEY key = nullptr;
    LONG opened = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (opened != ERROR_SUCCESS) {
        opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                               L"SOFTWARE\\Microsoft\\Cryptography", 0,
                               KEY_QUERY_VALUE, &key);
    }
    if (opened != ERROR_SUCCESS) return false;

    char raw[128] = {};
    DWORD type = 0;
    DWORD size = sizeof(raw);
    const LONG queried = RegQueryValueExA(
        key, "MachineGuid", nullptr, &type, reinterpret_cast<BYTE*>(raw), &size);
    RegCloseKey(key);
    // Windows normally stores MachineGuid as REG_SZ.  Some deployment and
    // imaging tools preserve it as REG_EXPAND_SZ; the value is still read as
    // raw text and is normalized/validated below, so accepting that type does
    // not introduce expansion or environment-variable ambiguity.
    if (queried != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size < 2u ||
        size > sizeof(raw) || raw[size - 1u] != '\0') {
        SecureZeroMemory(raw, sizeof(raw));
        return false;
    }
    for (DWORD index = 0; index + 1u < size; ++index) {
        if (raw[index] == '\0') {
            SecureZeroMemory(raw, sizeof(raw));
            return false;
        }
    }
    raw[sizeof(raw) - 1u] = '\0';
    const bool normalized =
        NormalizeMachineGuidAscii(raw, output, reason, reasonSize);
    SecureZeroMemory(raw, sizeof(raw));
    return normalized;
}

bool ReadSystemVolumeSerial(std::uint32_t* serial) noexcept {
    if (serial == nullptr) return false;
    wchar_t windowsDirectory[32768] = {};
    const UINT length = GetWindowsDirectoryW(
        windowsDirectory, static_cast<UINT>(std::size(windowsDirectory)));
    if (length == 0 || length >= std::size(windowsDirectory)) return false;

    wchar_t volumeRoot[32768] = {};
    if (!GetVolumePathNameW(windowsDirectory, volumeRoot,
                            static_cast<DWORD>(std::size(volumeRoot)))) {
        return false;
    }
    DWORD value = 0;
    if (!GetVolumeInformationW(volumeRoot, nullptr, 0, &value, nullptr, nullptr,
                               nullptr, 0)) {
        return false;
    }
    *serial = static_cast<std::uint32_t>(value);
    return true;
}

bool GetCurrentExecutablePath(std::wstring* output, char* reason,
                              std::size_t reasonSize) {
    if (output == nullptr) {
        SetReason(reason, reasonSize, "executable path destination is null");
        return false;
    }
    output->clear();
    try {
        std::vector<wchar_t> buffer(512u);
        while (buffer.size() <= 32768u) {
            SetLastError(ERROR_SUCCESS);
            const DWORD length = GetModuleFileNameW(
                nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                SetReason(reason, reasonSize,
                          "could not resolve the main executable path (error %lu)",
                          static_cast<unsigned long>(GetLastError()));
                return false;
            }
            if (length < buffer.size()) {
                output->assign(buffer.data(), length);
                return true;
            }
            if (buffer.size() == 32768u) break;
            buffer.resize(std::min<std::size_t>(buffer.size() * 2u, 32768u));
        }
    } catch (const std::bad_alloc&) {
        SetReason(reason, reasonSize,
                  "could not allocate the main executable path buffer");
        return false;
    }
    SetReason(reason, reasonSize, "the main executable path is too long");
    return false;
}

bool DirectoryFromExecutable(std::wstring_view executablePath,
                             std::wstring* output, char* reason,
                             std::size_t reasonSize) {
    if (output == nullptr || executablePath.empty()) {
        SetReason(reason, reasonSize, "executable directory input is invalid");
        return false;
    }
    const std::size_t slash = executablePath.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos || slash == 0u) {
        SetReason(reason, reasonSize,
                  "could not resolve the executable directory");
        return false;
    }
    try {
        output->assign(executablePath.data(), slash);
    } catch (const std::bad_alloc&) {
        SetReason(reason, reasonSize,
                  "could not allocate the executable directory path");
        return false;
    }
    return true;
}

enum class DirectoryState {
    Missing,
    Directory,
    NotDirectory,
    ReparsePoint,
    Error,
};

DirectoryState InspectDirectory(const std::wstring& path, DWORD* errorOut) {
    if (errorOut != nullptr) *errorOut = ERROR_SUCCESS;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return DirectoryState::ReparsePoint;
        }
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                   ? DirectoryState::Directory
                   : DirectoryState::NotDirectory;
    }
    const DWORD error = GetLastError();
    if (errorOut != nullptr) *errorOut = error;
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return DirectoryState::Missing;
    }
    return DirectoryState::Error;
}

bool ResolveOrCreateScriptsDirectory(std::wstring_view executablePath,
                                     std::wstring* output, char* reason,
                                     std::size_t reasonSize) {
    std::wstring parent;
    if (!DirectoryFromExecutable(executablePath, &parent, reason, reasonSize)) {
        return false;
    }

    try {
        const std::wstring upper = parent + L"\\SCRIPTS";
        const std::wstring lower = parent + L"\\scripts";
        DWORD error = ERROR_SUCCESS;
        DirectoryState state = InspectDirectory(upper, &error);
        if (state == DirectoryState::Directory) {
            *output = upper;
            return true;
        }
        if (state == DirectoryState::NotDirectory) {
            SetReason(reason, reasonSize,
                      "the SCRIPTS path exists but is not a directory");
            return false;
        }
        if (state == DirectoryState::ReparsePoint) {
            SetReason(reason, reasonSize,
                      "the SCRIPTS path is a reparse point and is rejected");
            return false;
        }
        if (state == DirectoryState::Error) {
            SetReason(reason, reasonSize,
                      "could not inspect the SCRIPTS directory (error %lu)",
                      static_cast<unsigned long>(error));
            return false;
        }

        state = InspectDirectory(lower, &error);
        if (state == DirectoryState::Directory) {
            *output = lower;
            return true;
        }
        if (state == DirectoryState::NotDirectory) {
            SetReason(reason, reasonSize,
                      "the scripts path exists but is not a directory");
            return false;
        }
        if (state == DirectoryState::ReparsePoint) {
            SetReason(reason, reasonSize,
                      "the scripts path is a reparse point and is rejected");
            return false;
        }
        if (state == DirectoryState::Error) {
            SetReason(reason, reasonSize,
                      "could not inspect the scripts directory (error %lu)",
                      static_cast<unsigned long>(error));
            return false;
        }

        if (!CreateDirectoryW(upper.c_str(), nullptr)) {
            const DWORD createError = GetLastError();
            if (createError != ERROR_ALREADY_EXISTS) {
                SetReason(reason, reasonSize,
                          "could not create the SCRIPTS directory (error %lu)",
                          static_cast<unsigned long>(createError));
                return false;
            }
            state = InspectDirectory(upper, &error);
            if (state != DirectoryState::Directory) {
                SetReason(reason, reasonSize,
                          "the concurrently created SCRIPTS path is invalid");
                return false;
            }
        }
        *output = upper;
        return true;
    } catch (const std::bad_alloc&) {
        SetReason(reason, reasonSize, "could not allocate the SCRIPTS path");
        return false;
    }
}

// Hold the parent directory open without delete sharing while publishing a
// new binding.  Opening with OPEN_REPARSE_POINT lets us inspect the directory
// object itself instead of following a junction/symlink.  The handle also
// prevents a concurrent rename/reparse swap for the duration of the atomic
// publish operation.
bool OpenBindingDirectoryGuard(const wchar_t* bindingPath,
                               ScopedHandle* guard, char* reason,
                               std::size_t reasonSize) {
    if (bindingPath == nullptr || bindingPath[0] == L'\0' || guard == nullptr) {
        SetReason(reason, reasonSize,
                  "device-binding directory guard input is invalid");
        return false;
    }
    const std::wstring_view path(bindingPath);
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos || slash == 0u) {
        SetReason(reason, reasonSize,
                  "could not resolve the device-binding parent directory");
        return false;
    }
    std::wstring parent;
    try {
        // Preserve the trailing slash for a drive root (C:\\).
        const std::size_t parentSize =
            slash == 2u && path.size() >= 3u && path[1] == L':' ? 3u : slash;
        parent.assign(path.data(), parentSize);
    } catch (const std::bad_alloc&) {
        SetReason(reason, reasonSize,
                  "could not allocate the device-binding parent path");
        return false;
    }

    HANDLE directory = CreateFileW(
        parent.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        SetReason(reason, reasonSize,
                  "could not open the device-binding directory (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(directory, &information)) {
        const DWORD error = GetLastError();
        CloseHandle(directory);
        SetReason(reason, reasonSize,
                  "could not inspect the device-binding directory (error %lu)",
                  static_cast<unsigned long>(error));
        return false;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        CloseHandle(directory);
        SetReason(reason, reasonSize,
                  "device-binding parent is not a directory");
        return false;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(directory);
        SetReason(reason, reasonSize,
                  "device-binding parent is a reparse point and is rejected");
        return false;
    }
    guard->reset(directory);
    return true;
}

bool ReadComplete(HANDLE file, void* output, std::size_t size) noexcept {
    if (file == INVALID_HANDLE_VALUE || (output == nullptr && size != 0) ||
        size > MAXDWORD) {
        return false;
    }
    std::size_t total = 0;
    while (total < size) {
        DWORD current = 0;
        if (!ReadFile(file, static_cast<std::uint8_t*>(output) + total,
                      static_cast<DWORD>(size - total), &current, nullptr) ||
            current == 0) {
            return false;
        }
        total += current;
    }
    return true;
}

bool WriteComplete(HANDLE file, const void* contents, std::size_t size) noexcept {
    if (file == INVALID_HANDLE_VALUE || (contents == nullptr && size != 0) ||
        size > MAXDWORD) {
        return false;
    }
    std::size_t total = 0;
    while (total < size) {
        DWORD current = 0;
        if (!WriteFile(file,
                       static_cast<const std::uint8_t*>(contents) + total,
                       static_cast<DWORD>(size - total), &current, nullptr) ||
            current == 0) {
            return false;
        }
        total += current;
    }
    return true;
}

enum class ExistingBinding { Missing, Matches, Mismatch, Error };

ExistingBinding CheckExistingBinding(const wchar_t* path,
                                     const std::uint8_t* expectedPlaintext,
                                     std::size_t expectedPlaintextSize,
                                     char* reason, std::size_t reasonSize) {
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return ExistingBinding::Missing;
        }
        SetReason(reason, reasonSize,
                  "could not inspect the device-binding file (error %lu)",
                  static_cast<unsigned long>(error));
        return ExistingBinding::Error;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        SetReason(reason, reasonSize,
                  "the device-binding path is a directory");
        return ExistingBinding::Error;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        SetReason(reason, reasonSize,
                  "the device-binding file is a reparse point and is rejected");
        return ExistingBinding::Error;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetReason(reason, reasonSize,
                  "could not open the existing device-binding file (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
        return ExistingBinding::Error;
    }

    LARGE_INTEGER fileSize{};
    const bool sizeOk = GetFileSizeEx(file, &fileSize) != FALSE &&
                        fileSize.QuadPart > 0 &&
                        fileSize.QuadPart <=
                            static_cast<LONGLONG>(kMaxBindingJsonSize);
    if (!sizeOk) {
        CloseHandle(file);
        SetReason(reason, reasonSize,
                  "the existing device-binding JSON size is invalid");
        return ExistingBinding::Mismatch;
    }

    try {
        std::vector<char> json(static_cast<std::size_t>(fileSize.QuadPart));
        ScopedVectorWipe<char> jsonWipe(&json);
        const bool read = ReadComplete(file, json.data(), json.size());
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (!read) {
            SetReason(reason, reasonSize,
                      "could not read the existing device-binding file");
            return ExistingBinding::Error;
        }

        std::vector<std::uint8_t> ciphertext(kMaxCiphertextSize);
        ScopedVectorWipe<std::uint8_t> ciphertextWipe(&ciphertext);
        std::size_t ciphertextSize = 0;
        if (!ParseEncryptedBindingJson(
                json.data(), json.size(), ciphertext.data(), ciphertext.size(),
                &ciphertextSize, reason, reasonSize)) {
            return ExistingBinding::Mismatch;
        }
        ciphertext.resize(ciphertextSize);

        std::vector<std::uint8_t> plaintext;
        ScopedVectorWipe<std::uint8_t> plaintextWipe(&plaintext);
        if (!UnprotectPayloadForLocalMachine(
                ciphertext.data(), ciphertext.size(), &plaintext, reason,
                reasonSize)) {
            return ExistingBinding::Mismatch;
        }
        const bool matches = ConstantTimeEqual(
            expectedPlaintext, expectedPlaintextSize, plaintext.data(),
            plaintext.size());
        if (!matches) {
            SetReason(reason, reasonSize,
                      "existing device binding does not match this computer");
            return ExistingBinding::Mismatch;
        }
        return ExistingBinding::Matches;
    } catch (const std::bad_alloc&) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        SetReason(reason, reasonSize,
                  "could not allocate the device-binding read buffer");
        return ExistingBinding::Error;
    }
}

}  // namespace

bool ValidateFooterBytes(const std::uint8_t* footer, std::size_t size,
                         char* reason,
                         std::size_t reasonSize) noexcept {
    ClearReason(reason, reasonSize);
    constexpr char magic[] = "NFSMWRF1";
    if (footer == nullptr || size != kFooterSize) {
        SetReason(reason, reasonSize, "Reforged footer is missing or truncated");
        return false;
    }
    if (std::memcmp(footer, magic, 8u) != 0 || ReadLe32(footer + 8u) != 1u ||
        ReadLe32(footer + 12u) != kFooterSize ||
        ReadLe32(footer + 16u) != kVerificationCode ||
        ReadLe32(footer + 20u) != kOriginalExecutableSize ||
        ReadLe32(footer + 24u) != kOriginalPeChecksum ||
        ReadLe32(footer + 28u) != 1u) {
        SetReason(reason, reasonSize,
                  "Reforged footer fields or verification code are invalid");
        return false;
    }
    std::uint8_t expectedDigest[32] = {};
    if (!DecodeKnownHex(kFooterOriginalCanonicalSha256, expectedDigest,
                        sizeof(expectedDigest)) ||
        !ConstantTimeEqual(footer + 32u, 32u, expectedDigest,
                           sizeof(expectedDigest))) {
        SecureZeroMemory(expectedDigest, sizeof(expectedDigest));
        SetReason(reason, reasonSize,
                  "Reforged footer original-image digest is invalid");
        return false;
    }
    SecureZeroMemory(expectedDigest, sizeof(expectedDigest));
    return true;
}

bool NormalizeMachineGuidAscii(const char* input, char output[128],
                               char* reason,
                               std::size_t reasonSize) noexcept {
    ClearReason(reason, reasonSize);
    if (output == nullptr) {
        SetReason(reason, reasonSize, "MachineGuid destination is null");
        return false;
    }
    output[0] = '\0';
    if (input == nullptr) {
        SetReason(reason, reasonSize, "MachineGuid is missing");
        return false;
    }
    const std::size_t length = strnlen_s(input, 128u);
    if (length == 0 || length == 128u) {
        SetReason(reason, reasonSize, "MachineGuid is malformed");
        return false;
    }

    std::size_t begin = 0;
    std::size_t end = length;
    while (begin < end &&
           (input[begin] == ' ' || input[begin] == '\t' ||
            input[begin] == '\r' || input[begin] == '\n')) {
        ++begin;
    }
    while (end > begin &&
           (input[end - 1u] == ' ' || input[end - 1u] == '\t' ||
            input[end - 1u] == '\r' || input[end - 1u] == '\n')) {
        --end;
    }
    if (begin == end || end - begin >= 128u) {
        SetReason(reason, reasonSize, "MachineGuid is empty or too long");
        return false;
    }
    for (std::size_t source = begin, destination = 0; source < end;
         ++source, ++destination) {
        unsigned char value = static_cast<unsigned char>(input[source]);
        if (value < 0x21u || value > 0x7Eu) {
            SecureZeroMemory(output, 128u);
            SetReason(reason, reasonSize,
                      "MachineGuid contains a non-ASCII character");
            return false;
        }
        if (value >= 'a' && value <= 'z') {
            value = static_cast<unsigned char>(value - 'a' + 'A');
        }
        output[destination] = static_cast<char>(value);
        output[destination + 1u] = '\0';
    }
    return true;
}

bool ConstantTimeEqual(const std::uint8_t* first, std::size_t firstSize,
                       const std::uint8_t* second,
                       std::size_t secondSize) noexcept {
    if ((first == nullptr && firstSize != 0) ||
        (second == nullptr && secondSize != 0)) {
        return false;
    }
    const std::size_t maximum = std::max(firstSize, secondSize);
    std::size_t difference = firstSize ^ secondSize;
    for (std::size_t index = 0; index < maximum; ++index) {
        const std::uint8_t firstByte = index < firstSize ? first[index] : 0u;
        const std::uint8_t secondByte = index < secondSize ? second[index] : 0u;
        difference |= static_cast<std::size_t>(firstByte ^ secondByte);
    }
    return difference == 0;
}

bool HexEncodeUpper(const std::uint8_t* input, std::size_t inputSize,
                    char* output, std::size_t outputSize,
                    std::size_t* written, char* reason,
                    std::size_t reasonSize) noexcept {
    ClearReason(reason, reasonSize);
    if (written != nullptr) *written = 0;
    if (output != nullptr && outputSize != 0) output[0] = '\0';
    if (input == nullptr || inputSize == 0 || output == nullptr ||
        inputSize > (std::numeric_limits<std::size_t>::max() - 1u) / 2u ||
        outputSize < inputSize * 2u + 1u) {
        SetReason(reason, reasonSize, "hex encoding input or buffer is invalid");
        return false;
    }
    constexpr char digits[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < inputSize; ++index) {
        output[index * 2u] = digits[input[index] >> 4u];
        output[index * 2u + 1u] = digits[input[index] & 0x0Fu];
    }
    output[inputSize * 2u] = '\0';
    if (written != nullptr) *written = inputSize * 2u;
    return true;
}

bool HexDecodeUpper(const char* input, std::size_t inputSize,
                    std::uint8_t* output, std::size_t outputSize,
                    std::size_t* written, char* reason,
                    std::size_t reasonSize) noexcept {
    ClearReason(reason, reasonSize);
    if (written != nullptr) *written = 0;
    if (output != nullptr && outputSize != 0) {
        SecureZeroMemory(output, outputSize);
    }
    if (input == nullptr || inputSize == 0 || (inputSize & 1u) != 0 ||
        output == nullptr || outputSize < inputSize / 2u) {
        SetReason(reason, reasonSize, "hex decoding input or buffer is invalid");
        return false;
    }
    for (std::size_t index = 0; index < inputSize / 2u; ++index) {
        const int high = UpperHexNibble(input[index * 2u]);
        const int low = UpperHexNibble(input[index * 2u + 1u]);
        if (high < 0 || low < 0) {
            SetReason(reason, reasonSize,
                      "ciphertext is not uppercase hexadecimal");
            return false;
        }
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    if (written != nullptr) *written = inputSize / 2u;
    return true;
}

bool ComputeHardwareId(const DeviceComponents& components, char output[65],
                       unsigned* sourceMask, char* reason,
                       std::size_t reasonSize) {
    ClearReason(reason, reasonSize);
    if (output == nullptr || !components.hasSystemUuid ||
        !IsUsableUuid(components.systemUuid) ||
        !components.hasSystemVolumeSerial) {
        SetReason(reason, reasonSize,
                  "required device identity components are missing");
        return false;
    }
    output[0] = '\0';
    char canonicalMachineGuid[128] = {};
    if (!NormalizeMachineGuidAscii(components.machineGuid,
                                   canonicalMachineGuid, reason, reasonSize)) {
        return false;
    }
    const std::size_t machineGuidSize = std::strlen(canonicalMachineGuid);

    HashSession session{};
    if (!BeginHash(CALG_SHA_256, &session)) {
        SecureZeroMemory(canonicalMachineGuid, sizeof(canonicalMachineGuid));
        SetReason(reason, reasonSize,
                  "could not initialize the hardware SHA-256");
        return false;
    }

    constexpr char domain[] =
        "Slippery_Drifting_FlashFish_by_AndyQinke.DeviceBinding.v1";
    std::uint8_t verificationCode[4] = {};
    WriteLe32(kVerificationCode, verificationCode);
    std::uint8_t digest[32] = {};
    const bool success =
        HashField(session.hash, "domain", domain, sizeof(domain) - 1u) &&
        HashField(session.hash, "verification-code", verificationCode,
                  sizeof(verificationCode)) &&
        HashField(session.hash, "smbios-system-uuid", components.systemUuid,
                  sizeof(components.systemUuid)) &&
        HashField(session.hash, "windows-machine-guid", canonicalMachineGuid,
                  machineGuidSize) &&
        [&]() {
            std::uint8_t volumeSerial[4] = {};
            WriteLe32(components.systemVolumeSerial, volumeSerial);
            const bool hashed = HashField(session.hash, "system-volume-serial",
                                          volumeSerial,
                                          sizeof(volumeSerial));
            SecureZeroMemory(volumeSerial, sizeof(volumeSerial));
            return hashed;
        }() &&
        FinishHash(session.hash, digest, sizeof(digest));
    EndHash(&session);
    SecureZeroMemory(canonicalMachineGuid, sizeof(canonicalMachineGuid));
    SecureZeroMemory(verificationCode, sizeof(verificationCode));
    if (!success) {
        SecureZeroMemory(digest, sizeof(digest));
        SetReason(reason, reasonSize,
                  "could not calculate the hardware SHA-256");
        return false;
    }
    std::size_t encoded = 0;
    const bool encodedOk = HexEncodeUpper(digest, sizeof(digest), output, 65u,
                                          &encoded, reason, reasonSize);
    SecureZeroMemory(digest, sizeof(digest));
    if (!encodedOk || encoded != kHardwareIdSize) {
        output[0] = '\0';
        return false;
    }
    if (sourceMask != nullptr) *sourceMask = kRequiredSourceMask;
    return true;
}

bool BuildPlaintextPayload(const char* hardwareId, char* output,
                           std::size_t outputSize, std::size_t* written,
                           char* reason,
                           std::size_t reasonSize) noexcept {
    ClearReason(reason, reasonSize);
    if (written != nullptr) *written = 0;
    if (output != nullptr && outputSize != 0) output[0] = '\0';
    if (hardwareId == nullptr || output == nullptr || outputSize == 0 ||
        strnlen_s(hardwareId, 65u) != kHardwareIdSize) {
        SetReason(reason, reasonSize,
                  "hardware ID or plaintext destination is invalid");
        return false;
    }
    for (std::size_t index = 0; index < kHardwareIdSize; ++index) {
        if (UpperHexNibble(hardwareId[index]) < 0) {
            SetReason(reason, reasonSize,
                      "hardware ID is not uppercase SHA-256");
            return false;
        }
    }

    const int length = _snprintf_s(
        output, outputSize, _TRUNCATE,
        "{\r\n"
        "  \"schema\": 1,\r\n"
        "  \"product\": \"%s\",\r\n"
        "  \"executable\": \"%s\",\r\n"
        "  \"verification_code\": %u,\r\n"
        "  \"executable_sha256\": \"%s\",\r\n"
        "  \"hardware_id_algorithm\": \"SHA-256\",\r\n"
        "  \"identity_source_mask\": %u,\r\n"
        "  \"hardware_id\": \"%s\"\r\n"
        "}\r\n",
        kProductName, kBindingExecutableIdentity,
        static_cast<unsigned>(kVerificationCode),
        kBindingExecutableSha256, kRequiredSourceMask, hardwareId);
    if (length < 0 || static_cast<std::size_t>(length) >= kMaxPlaintextSize) {
        SecureZeroMemory(output, outputSize);
        SetReason(reason, reasonSize,
                  "device-binding plaintext buffer is too small");
        return false;
    }
    if (written != nullptr) *written = static_cast<std::size_t>(length);
    return true;
}

bool BuildEncryptedBindingJson(const std::uint8_t* ciphertext,
                               std::size_t ciphertextSize, char* output,
                               std::size_t outputSize, std::size_t* written,
                               char* reason,
                               std::size_t reasonSize) noexcept {
    ClearReason(reason, reasonSize);
    if (written != nullptr) *written = 0;
    if (output != nullptr && outputSize != 0) output[0] = '\0';
    if (ciphertext == nullptr || ciphertextSize == 0 ||
        ciphertextSize > kMaxCiphertextSize || output == nullptr) {
        SetReason(reason, reasonSize,
                  "ciphertext or binding JSON destination is invalid");
        return false;
    }
    constexpr std::size_t prefixSize = sizeof(kBindingJsonPrefix) - 1u;
    constexpr std::size_t suffixSize = sizeof(kBindingJsonSuffix) - 1u;
    const std::size_t hexSize = ciphertextSize * 2u;
    const std::size_t required = prefixSize + hexSize + suffixSize;
    if (outputSize <= required || required > kMaxBindingJsonSize) {
        if (output != nullptr && outputSize != 0) {
            SecureZeroMemory(output, outputSize);
        }
        SetReason(reason, reasonSize,
                  "device-binding JSON buffer is too small");
        return false;
    }

    std::memcpy(output, kBindingJsonPrefix, prefixSize);
    constexpr char digits[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < ciphertextSize; ++index) {
        output[prefixSize + index * 2u] = digits[ciphertext[index] >> 4u];
        output[prefixSize + index * 2u + 1u] =
            digits[ciphertext[index] & 0x0Fu];
    }
    std::memcpy(output + prefixSize + hexSize, kBindingJsonSuffix, suffixSize);
    output[required] = '\0';
    if (written != nullptr) *written = required;
    return true;
}

bool ParseEncryptedBindingJson(const char* json, std::size_t jsonSize,
                               std::uint8_t* ciphertext,
                               std::size_t ciphertextCapacity,
                               std::size_t* ciphertextSize, char* reason,
                               std::size_t reasonSize) noexcept {
    ClearReason(reason, reasonSize);
    if (ciphertextSize != nullptr) *ciphertextSize = 0;
    constexpr std::size_t prefixSize = sizeof(kBindingJsonPrefix) - 1u;
    constexpr std::size_t suffixSize = sizeof(kBindingJsonSuffix) - 1u;
    if (json == nullptr || ciphertext == nullptr ||
        jsonSize <= prefixSize + suffixSize || jsonSize > kMaxBindingJsonSize ||
        std::memcmp(json, kBindingJsonPrefix, prefixSize) != 0 ||
        std::memcmp(json + jsonSize - suffixSize, kBindingJsonSuffix,
                    suffixSize) != 0) {
        SetReason(reason, reasonSize,
                  "device-binding JSON is not in canonical form");
        return false;
    }
    const std::size_t hexSize = jsonSize - prefixSize - suffixSize;
    if (hexSize == 0 || (hexSize & 1u) != 0 ||
        hexSize / 2u > kMaxCiphertextSize ||
        ciphertextCapacity < hexSize / 2u) {
        SetReason(reason, reasonSize,
                  "device-binding ciphertext size is invalid");
        return false;
    }
    return HexDecodeUpper(json + prefixSize, hexSize, ciphertext,
                          ciphertextCapacity, ciphertextSize, reason,
                          reasonSize);
}

bool BuildBindingPath(std::wstring_view executablePath, std::wstring* output,
                      char* reason, std::size_t reasonSize) {
    ClearReason(reason, reasonSize);
    if (output == nullptr) {
        SetReason(reason, reasonSize,
                  "device-binding path destination is null");
        return false;
    }
    std::wstring directory;
    if (!DirectoryFromExecutable(executablePath, &directory, reason,
                                 reasonSize)) {
        return false;
    }
    try {
        *output = directory + L"\\SCRIPTS\\" + kBindingFileName;
    } catch (const std::bad_alloc&) {
        SetReason(reason, reasonSize,
                  "could not allocate the device-binding path");
        return false;
    }
    return true;
}

bool ValidateExecutable(const wchar_t* executablePath, char* reason,
                        std::size_t reasonSize) {
    ClearReason(reason, reasonSize);
    if (executablePath == nullptr || executablePath[0] == L'\0') {
        SetReason(reason, reasonSize, "executable path is null or empty");
        return false;
    }
    const wchar_t* slash = std::wcsrchr(executablePath, L'\\');
    const wchar_t* forwardSlash = std::wcsrchr(executablePath, L'/');
    if (forwardSlash != nullptr && (slash == nullptr || forwardSlash > slash)) {
        slash = forwardSlash;
    }
    const wchar_t* baseName = slash == nullptr ? executablePath : slash + 1;
    if (CompareStringOrdinal(baseName, -1, kExecutableName, -1, TRUE) !=
        CSTR_EQUAL) {
        SetReason(reason, reasonSize,
                  "main executable name is not speed.exe");
        return false;
    }

    HANDLE file = CreateFileW(executablePath, GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetReason(reason, reasonSize,
                  "could not open the Reforged executable (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
        return false;
    }

    bool valid = false;
    LARGE_INTEGER fileSize{};
    std::uint8_t footer[kFooterSize] = {};
    std::uint8_t md5[16] = {};
    std::uint8_t fullSha256[32] = {};
    std::uint8_t canonicalSha256[32] = {};
    std::uint8_t expectedMd5[16] = {};
    std::uint8_t expectedFullSha256[32] = {};
    std::uint8_t expectedCanonicalSha256[32] = {};
    do {
        if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0 ||
            static_cast<std::uint64_t>(fileSize.QuadPart) !=
                kStampedExecutableSize) {
            SetReason(reason, reasonSize,
                      "Reforged executable size is not %llu bytes",
                      static_cast<unsigned long long>(kStampedExecutableSize));
            break;
        }
        if (!ReadFileAt(file, kOriginalExecutableSize, footer,
                        static_cast<DWORD>(sizeof(footer)))) {
            SetReason(reason, reasonSize, "could not read the Reforged footer");
            break;
        }
        if (!ValidateFooterBytes(footer, sizeof(footer), reason, reasonSize)) {
            break;
        }
        if (!ComputeExecutableDigests(file, md5, fullSha256, canonicalSha256,
                                      reason, reasonSize)) {
            break;
        }
        LARGE_INTEGER finalSize{};
        if (!GetFileSizeEx(file, &finalSize) ||
            finalSize.QuadPart != fileSize.QuadPart) {
            SetReason(reason, reasonSize,
                      "the Reforged executable changed during validation");
            break;
        }
        if (!DecodeKnownHex(kStampedExecutableMd5, expectedMd5,
                            sizeof(expectedMd5)) ||
            !DecodeKnownHex(kStampedExecutableSha256, expectedFullSha256,
                            sizeof(expectedFullSha256)) ||
            !DecodeKnownHex(kExpandedCanonicalSha256,
                            expectedCanonicalSha256,
                            sizeof(expectedCanonicalSha256))) {
            SetReason(reason, reasonSize,
                      "compiled executable fingerprints are malformed");
            break;
        }
        if (!ConstantTimeEqual(md5, sizeof(md5), expectedMd5,
                               sizeof(expectedMd5))) {
            SetReason(reason, reasonSize,
                      "stamped executable MD5 does not match");
            break;
        }
        if (!ConstantTimeEqual(fullSha256, sizeof(fullSha256),
                               expectedFullSha256,
                               sizeof(expectedFullSha256))) {
            SetReason(reason, reasonSize,
                      "stamped executable SHA-256 does not match");
            break;
        }
        if (!ConstantTimeEqual(canonicalSha256, sizeof(canonicalSha256),
                               expectedCanonicalSha256,
                               sizeof(expectedCanonicalSha256))) {
            SetReason(reason, reasonSize,
                      "original game-image SHA-256 does not match");
            break;
        }
        valid = true;
    } while (false);
    CloseHandle(file);
    SecureZeroMemory(footer, sizeof(footer));
    SecureZeroMemory(md5, sizeof(md5));
    SecureZeroMemory(fullSha256, sizeof(fullSha256));
    SecureZeroMemory(canonicalSha256, sizeof(canonicalSha256));
    SecureZeroMemory(expectedMd5, sizeof(expectedMd5));
    SecureZeroMemory(expectedFullSha256, sizeof(expectedFullSha256));
    SecureZeroMemory(expectedCanonicalSha256,
                     sizeof(expectedCanonicalSha256));
    return valid;
}

bool QueryDeviceComponents(DeviceComponents* components, char* reason,
                           std::size_t reasonSize) {
    ClearReason(reason, reasonSize);
    if (components == nullptr) {
        SetReason(reason, reasonSize, "device component destination is null");
        return false;
    }
    *components = {};
    components->hasSystemUuid = ReadSystemUuid(components->systemUuid);
    if (!components->hasSystemUuid) {
        SecureZeroMemory(components, sizeof(*components));
        SetReason(reason, reasonSize,
                  "could not read a usable SMBIOS system UUID");
        return false;
    }
    if (!ReadMachineGuid(components->machineGuid, reason, reasonSize)) {
        if (reason != nullptr && reasonSize != 0 && reason[0] == '\0') {
            SetReason(reason, reasonSize,
                      "could not read Windows MachineGuid");
        }
        SecureZeroMemory(components, sizeof(*components));
        return false;
    }
    components->hasSystemVolumeSerial =
        ReadSystemVolumeSerial(&components->systemVolumeSerial);
    if (!components->hasSystemVolumeSerial) {
        SecureZeroMemory(components, sizeof(*components));
        SetReason(reason, reasonSize,
                  "could not read the Windows system-volume serial");
        return false;
    }
    return true;
}

bool ProtectPayloadForLocalMachine(const std::uint8_t* plaintext,
                                   std::size_t plaintextSize,
                                   std::vector<std::uint8_t>* ciphertext,
                                   char* reason, std::size_t reasonSize) {
    ClearReason(reason, reasonSize);
    if (plaintext == nullptr || plaintextSize == 0 ||
        plaintextSize > kMaxPlaintextSize || plaintextSize > MAXDWORD ||
        ciphertext == nullptr) {
        SetReason(reason, reasonSize, "DPAPI plaintext input is invalid");
        return false;
    }
    WipeAndClearVector(ciphertext);
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plaintextSize);
    input.pbData = const_cast<BYTE*>(plaintext);
    DATA_BLOB entropy{};
    entropy.cbData = static_cast<DWORD>(sizeof(kDpapiEntropy) - 1u);
    entropy.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(kDpapiEntropy));
    DATA_BLOB output{};
    if (!CryptProtectData(&input, kDpapiDescription, &entropy, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE |
                              CRYPTPROTECT_UI_FORBIDDEN,
                          &output)) {
        SetReason(reason, reasonSize,
                  "DPAPI could not protect the binding payload (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
        return false;
    }
    if (output.pbData == nullptr || output.cbData == 0 ||
        output.cbData > kMaxCiphertextSize) {
        if (output.pbData != nullptr) {
            SecureZeroMemory(output.pbData, output.cbData);
            LocalFree(output.pbData);
        }
        SetReason(reason, reasonSize,
                  "DPAPI returned an invalid ciphertext size");
        return false;
    }
    try {
        ciphertext->assign(output.pbData, output.pbData + output.cbData);
    } catch (const std::bad_alloc&) {
        if (!ciphertext->empty()) {
            WipeAndClearVector(ciphertext);
        }
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        SetReason(reason, reasonSize,
                  "could not allocate the DPAPI ciphertext buffer");
        return false;
    }
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool UnprotectPayloadForLocalMachine(const std::uint8_t* ciphertext,
                                     std::size_t ciphertextSize,
                                     std::vector<std::uint8_t>* plaintext,
                                     char* reason, std::size_t reasonSize) {
    ClearReason(reason, reasonSize);
    if (ciphertext == nullptr || ciphertextSize == 0 ||
        ciphertextSize > kMaxCiphertextSize || ciphertextSize > MAXDWORD ||
        plaintext == nullptr) {
        SetReason(reason, reasonSize, "DPAPI ciphertext input is invalid");
        return false;
    }
    WipeAndClearVector(plaintext);
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(ciphertextSize);
    input.pbData = const_cast<BYTE*>(ciphertext);
    DATA_BLOB entropy{};
    entropy.cbData = static_cast<DWORD>(sizeof(kDpapiEntropy) - 1u);
    entropy.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(kDpapiEntropy));
    DATA_BLOB output{};
    LPWSTR description = nullptr;
    if (!CryptUnprotectData(&input, &description, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        SetReason(reason, reasonSize,
                  "DPAPI could not decrypt the existing binding (error %lu)",
                  static_cast<unsigned long>(GetLastError()));
        return false;
    }
    if (description != nullptr) LocalFree(description);
    if (output.pbData == nullptr || output.cbData == 0 ||
        output.cbData > kMaxPlaintextSize) {
        if (output.pbData != nullptr) {
            SecureZeroMemory(output.pbData, output.cbData);
            LocalFree(output.pbData);
        }
        SetReason(reason, reasonSize,
                  "DPAPI returned an invalid plaintext size");
        return false;
    }
    try {
        plaintext->assign(output.pbData, output.pbData + output.cbData);
    } catch (const std::bad_alloc&) {
        if (!plaintext->empty()) {
            WipeAndClearVector(plaintext);
        }
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        SetReason(reason, reasonSize,
                  "could not allocate the DPAPI plaintext buffer");
        return false;
    }
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool EnsureBindingFile(const wchar_t* path,
                       const std::uint8_t* expectedPlaintext,
                       std::size_t expectedPlaintextSize,
                       BindingStatus* status, char* reason,
                       std::size_t reasonSize) {
    ClearReason(reason, reasonSize);
    if (path == nullptr || path[0] == L'\0' || expectedPlaintext == nullptr ||
        expectedPlaintextSize == 0 ||
        expectedPlaintextSize > kMaxPlaintextSize || status == nullptr) {
        SetReason(reason, reasonSize, "device-binding input is invalid");
        return false;
    }

    ExistingBinding existing = CheckExistingBinding(
        path, expectedPlaintext, expectedPlaintextSize, reason, reasonSize);
    if (existing == ExistingBinding::Matches) {
        *status = BindingStatus::Verified;
        return true;
    }
    if (existing != ExistingBinding::Missing) return false;

    // Keep the validated parent directory open without delete sharing while
    // the new file is created and atomically published.  This closes the
    // check-then-use window in which a junction could otherwise be swapped in.
    ScopedHandle directoryGuard;
    if (!OpenBindingDirectoryGuard(path, &directoryGuard, reason, reasonSize)) {
        return false;
    }

    try {
        std::vector<std::uint8_t> ciphertext;
        ScopedVectorWipe<std::uint8_t> ciphertextWipe(&ciphertext);
        if (!ProtectPayloadForLocalMachine(
                expectedPlaintext, expectedPlaintextSize, &ciphertext, reason,
                reasonSize)) {
            return false;
        }
        std::vector<char> json(kMaxBindingJsonSize + 1u);
        ScopedVectorWipe<char> jsonWipe(&json);
        std::size_t jsonSize = 0;
        if (!BuildEncryptedBindingJson(
                ciphertext.data(), ciphertext.size(), json.data(), json.size(),
                &jsonSize, reason, reasonSize)) {
            return false;
        }

        std::wstring temporaryPath;
        HANDLE file = INVALID_HANDLE_VALUE;
        DWORD createError = ERROR_FILE_EXISTS;
        for (unsigned attempt = 0; attempt < 16u; ++attempt) {
            wchar_t suffix[96] = {};
            if (_snwprintf_s(
                    suffix, std::size(suffix), _TRUNCATE,
                    L".tmp.%08lX.%08lX.%08lX.%u",
                    static_cast<unsigned long>(GetCurrentProcessId()),
                    static_cast<unsigned long>(GetCurrentThreadId()),
                    static_cast<unsigned long>(GetTickCount()), attempt) < 0) {
                SetReason(reason, reasonSize,
                          "temporary binding suffix is too long");
                return false;
            }
            temporaryPath.assign(path);
            temporaryPath.append(suffix);
            file = CreateFileW(
                temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            createError = GetLastError();
            if (createError != ERROR_FILE_EXISTS &&
                createError != ERROR_ALREADY_EXISTS) {
                break;
            }
        }
        if (file == INVALID_HANDLE_VALUE) {
            SetReason(reason, reasonSize,
                      "could not create the temporary device binding (error %lu)",
                      static_cast<unsigned long>(createError));
            return false;
        }

        const bool wrote = WriteComplete(file, json.data(), jsonSize) &&
                           FlushFileBuffers(file) != FALSE;
        const bool closed = CloseHandle(file) != FALSE;
        if (!wrote || !closed) {
            DeleteFileW(temporaryPath.c_str());
            SetReason(reason, reasonSize,
                      "could not persist the temporary device binding");
            return false;
        }

        if (MoveFileExW(temporaryPath.c_str(), path, MOVEFILE_WRITE_THROUGH)) {
            *status = BindingStatus::Created;
            return true;
        }
        const DWORD moveError = GetLastError();
        DeleteFileW(temporaryPath.c_str());
        if (moveError == ERROR_ALREADY_EXISTS || moveError == ERROR_FILE_EXISTS) {
            existing = CheckExistingBinding(path, expectedPlaintext,
                                            expectedPlaintextSize, reason,
                                            reasonSize);
            if (existing == ExistingBinding::Matches) {
                *status = BindingStatus::Verified;
                return true;
            }
            return false;
        }
        SetReason(reason, reasonSize,
                  "could not publish the device binding (error %lu)",
                  static_cast<unsigned long>(moveError));
        return false;
    } catch (const std::bad_alloc&) {
        SetReason(reason, reasonSize,
                  "could not allocate the device-binding write buffers");
        return false;
    }
}

bool EnsureCurrentDeviceBinding(const wchar_t* executablePath,
                                BindingResult* result, char* reason,
                                std::size_t reasonSize) {
    ClearReason(reason, reasonSize);
    if (executablePath == nullptr || result == nullptr) {
        SetReason(reason, reasonSize,
                  "device-binding executable path or result is null");
        return false;
    }
    *result = {};

    DeviceComponents components{};
    char hardwareId[65] = {};
    char plaintext[kMaxPlaintextSize] = {};
    std::size_t plaintextSize = 0;
    bool success = false;
    do {
        if (!QueryDeviceComponents(&components, reason, reasonSize)) break;
        if (!ComputeHardwareId(components, hardwareId, &result->sourceMask,
                               reason, reasonSize)) {
            break;
        }
        if (!BuildPlaintextPayload(hardwareId, plaintext, sizeof(plaintext),
                                   &plaintextSize, reason, reasonSize)) {
            break;
        }

        std::wstring scriptsDirectory;
        if (!ResolveOrCreateScriptsDirectory(executablePath, &scriptsDirectory,
                                             reason, reasonSize)) {
            break;
        }
        try {
            result->path = scriptsDirectory + L"\\" + kBindingFileName;
        } catch (const std::bad_alloc&) {
            SetReason(reason, reasonSize,
                      "could not allocate the device-binding file path");
            break;
        }
        if (!EnsureBindingFile(
                result->path.c_str(),
                reinterpret_cast<const std::uint8_t*>(plaintext), plaintextSize,
                &result->status, reason, reasonSize)) {
            break;
        }
        success = true;
    } while (false);
    SecureZeroMemory(&components, sizeof(components));
    SecureZeroMemory(hardwareId, sizeof(hardwareId));
    SecureZeroMemory(plaintext, sizeof(plaintext));
    if (!success) {
        result->sourceMask = 0;
        result->path.clear();
    }
    return success;
}

bool ValidateCurrentProcess(std::wstring* executablePath, char* reason,
                            std::size_t reasonSize) {
    ClearReason(reason, reasonSize);
    if (executablePath == nullptr) {
        SetReason(reason, reasonSize,
                  "current executable path destination is null");
        return false;
    }
    executablePath->clear();
    try {
        if (!GetCurrentExecutablePath(executablePath, reason, reasonSize)) {
            return false;
        }
        return ValidateExecutable(executablePath->c_str(), reason, reasonSize);
    } catch (const std::bad_alloc&) {
        SetReason(reason, reasonSize,
                  "could not allocate the current executable path");
        executablePath->clear();
        return false;
    } catch (...) {
        SetReason(reason, reasonSize,
                  "current executable validation failed unexpectedly");
        executablePath->clear();
        return false;
    }
}

bool VerifyCurrentProcessAndEnsureBinding(BindingResult* result, char* reason,
                                          std::size_t reasonSize) {
    ClearReason(reason, reasonSize);
    if (result == nullptr) {
        SetReason(reason, reasonSize, "startup-gate result is null");
        return false;
    }
    *result = {};
    try {
        std::wstring executablePath;
        if (!ValidateCurrentProcess(&executablePath, reason, reasonSize)) {
            return false;
        }
        return EnsureCurrentDeviceBinding(executablePath.c_str(), result, reason,
                                          reasonSize);
    } catch (const std::bad_alloc&) {
        SetReason(reason, reasonSize,
                  "startup gate could not allocate a required buffer");
        return false;
    } catch (...) {
        SetReason(reason, reasonSize,
                  "startup gate failed with an unexpected exception");
        return false;
    }
}

}  // namespace nfsmw_drift::asi_host::reforged_startup_gate
