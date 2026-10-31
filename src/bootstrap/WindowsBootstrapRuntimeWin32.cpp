#include "bootstrap/HarkenSearchBootstrapRuntime.h"
#include "bootstrap/HarkenSearchBootstrapMain.h"
#include "bootstrap/HarkenSearchBootstrapExitExplanation.h"
#include "bootstrap/WindowsBootstrapLaunchBroker.h"
#include "bootstrap/WindowsBootstrapProgress.h"
#include "platform/update_bootstrap_authority_port.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include "nlohmann_json.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aris::bootstrap {
namespace {

using json = nlohmann::json;

class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    [[nodiscard]] HANDLE release() noexcept {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

class ScopedProcThreadAttributeList final {
public:
    ScopedProcThreadAttributeList() = default;
    ~ScopedProcThreadAttributeList() {
        if (list_ != nullptr) DeleteProcThreadAttributeList(list_);
    }

    ScopedProcThreadAttributeList(const ScopedProcThreadAttributeList&) = delete;
    ScopedProcThreadAttributeList& operator=(
        const ScopedProcThreadAttributeList&) = delete;

    [[nodiscard]] bool initialize(void* storage, SIZE_T& bytes,
                                  const DWORD attributeCount) noexcept {
        auto* const candidate =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage);
        if (!InitializeProcThreadAttributeList(
                candidate, attributeCount, 0U, &bytes)) {
            return false;
        }
        list_ = candidate;
        return true;
    }

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept {
        return list_;
    }

private:
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), output.data(), required,
                            nullptr, nullptr) != required) {
        return {};
    }
    return output;
}

std::wstring finalPath(HANDLE handle);
std::wstring normalizeFinalPath(std::wstring value);

std::string lowerAscii(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

bool hashPinnedFileSha256(HANDLE file, const std::wstring& expectedPath,
                          std::uint64_t& byteSize, std::string& digest,
                          DWORD& nativeError) {
    nativeError = ERROR_SUCCESS;
    if (file == nullptr || file == INVALID_HANDLE_VALUE ||
        expectedPath.empty()) {
        nativeError = ERROR_INVALID_HANDLE;
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER size{};
    const bool observedAttributes = GetFileInformationByHandleEx(
        file, FileAttributeTagInfo, &attributes, sizeof(attributes)) != FALSE;
    const bool observedSize = GetFileSizeEx(file, &size) != FALSE;
    const std::wstring canonical = finalPath(file);
    if (!observedAttributes || !observedSize || size.QuadPart < 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        canonical.empty() ||
        normalizeFinalPath(expectedPath) != normalizeFinalPath(canonical)) {
        nativeError = observedAttributes && observedSize ? ERROR_INVALID_DATA
                                                         : GetLastError();
        return false;
    }
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        nativeError = GetLastError();
        return false;
    }

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                                   CRYPT_VERIFYCONTEXT) != FALSE;
    if (ok) ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while (ok) {
        DWORD read = 0U;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr)) {
            nativeError = GetLastError();
            ok = false;
            break;
        }
        if (read == 0U) break;
        if (!CryptHashData(hash, buffer.data(), read, 0U)) {
            nativeError = GetLastError();
            ok = false;
            break;
        }
    }
    std::array<std::uint8_t, 32U> bytes{};
    DWORD bytesLength = static_cast<DWORD>(bytes.size());
    if (ok && (!CryptGetHashParam(hash, HP_HASHVAL, bytes.data(), &bytesLength, 0U) ||
               bytesLength != static_cast<DWORD>(bytes.size()))) {
        nativeError = GetLastError();
        ok = false;
    }
    if (hash != 0) CryptDestroyHash(hash);
    if (provider != 0) CryptReleaseContext(provider, 0U);
    if (!ok) {
        if (nativeError == ERROR_SUCCESS) nativeError = ERROR_INVALID_DATA;
        return false;
    }
    static constexpr char digits[] = "0123456789abcdef";
    digest.clear();
    digest.reserve(bytes.size() * 2U);
    for (const std::uint8_t byte : bytes) {
        digest.push_back(digits[byte >> 4U]);
        digest.push_back(digits[byte & 0x0fU]);
    }
    byteSize = static_cast<std::uint64_t>(size.QuadPart);
    nativeError = ERROR_SUCCESS;
    return true;
}

std::wstring utf8ToWide(const std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (required <= 0) return {};
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), output.data(), required) !=
        required) {
        return {};
    }
    return output;
}

std::wstring finalPath(HANDLE handle) {
    std::vector<wchar_t> buffer(512U);
    for (;;) {
        const DWORD length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED);
        if (length == 0U) return {};
        if (length < buffer.size() - 1U) return {buffer.data(), length};
        if (buffer.size() > 32768U) return {};
        buffer.resize(buffer.size() * 2U);
    }
}

std::wstring normalizeFinalPath(std::wstring value) {
    if (value.rfind(L"\\\\?\\", 0U) == 0U) value.erase(0U, 4U);
    for (wchar_t& character : value) {
        if (character == L'\\') character = L'/';
        if (character >= L'A' && character <= L'Z') {
            character = static_cast<wchar_t>(character - L'A' + L'a');
        }
    }
    while (value.size() > 1U && value.back() == L'/') value.pop_back();
    return value;
}

std::wstring portableFinalPath(std::wstring value) {
    // The pure/journal boundary intentionally accepts drive-root paths only.
    // GetFinalPathNameByHandleW returns an extended-length prefix; retain the
    // exact opened object identity but store a portable drive-root spelling so
    // restart decoding does not reject the runtime's own process checkpoint.
    if (value.rfind(L"\\\\?\\UNC\\", 0U) == 0U) return {};
    if (value.rfind(L"\\\\?\\", 0U) == 0U) value.erase(0U, 4U);
    for (wchar_t& character : value) {
        if (character == L'\\') character = L'/';
    }
    return value;
}

bool captureFileIdentity(const std::wstring& path, std::uint64_t& volume,
                         std::array<std::uint8_t, 16>& fileId,
                         std::wstring& canonicalPath) {
    HANDLE file = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    const bool observedAttributes = GetFileInformationByHandleEx(
        file, FileAttributeTagInfo, &attributes, sizeof(attributes)) != FALSE;
    FILE_ID_INFO information{};
    const bool observedIdentity = GetFileInformationByHandleEx(
        file, FileIdInfo, &information, sizeof(information)) != FALSE;
    canonicalPath = finalPath(file);
    CloseHandle(file);
    if (!observedAttributes || !observedIdentity ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        canonicalPath.empty()) {
        return false;
    }
    volume = information.VolumeSerialNumber;
    std::copy(std::begin(information.FileId.Identifier),
              std::end(information.FileId.Identifier), fileId.begin());
    return true;
}

bool pinExactExecutableSnapshot(
    const BootstrapProcessIdentity& expected, HANDLE& imagePin,
    std::wstring& pinnedImagePath,
    BootstrapExecutableExpectation& executable, DWORD& nativeError) {
    imagePin = nullptr;
    pinnedImagePath.clear();
    executable = {};
    nativeError = ERROR_SUCCESS;
    if (!expected.valid || expected.imagePath.empty()) {
        nativeError = ERROR_INVALID_PARAMETER;
        return false;
    }
    const std::wstring requestedPath = utf8ToWide(expected.imagePath);
    if (requestedPath.empty()) {
        nativeError = ERROR_INVALID_NAME;
        return false;
    }
    ScopedHandle candidate(CreateFileW(
        requestedPath.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (candidate.get() == INVALID_HANDLE_VALUE) {
        nativeError = GetLastError();
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_ID_INFO identity{};
    const bool observed = GetFileInformationByHandleEx(
                              candidate.get(), FileAttributeTagInfo,
                              &attributes, sizeof(attributes)) != FALSE &&
        GetFileInformationByHandleEx(candidate.get(), FileIdInfo, &identity,
                                     sizeof(identity)) != FALSE;
    const DWORD observationError = observed ? ERROR_SUCCESS : GetLastError();
    const std::wstring canonicalPath = finalPath(candidate.get());
    std::array<std::uint8_t, 16U> fileId{};
    if (observed) {
        std::copy(std::begin(identity.FileId.Identifier),
                  std::end(identity.FileId.Identifier), fileId.begin());
    }
    if (!observed || canonicalPath.empty() ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        normalizeFinalPath(requestedPath) != normalizeFinalPath(canonicalPath) ||
        identity.VolumeSerialNumber != expected.imageVolumeSerial ||
        fileId != expected.imageFileId) {
        nativeError = observationError == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                                         : observationError;
        return false;
    }

    std::uint64_t byteSize = 0U;
    std::string digest;
    if (!hashPinnedFileSha256(candidate.get(), requestedPath, byteSize, digest,
                              nativeError)) {
        return false;
    }
    const std::wstring portablePath = portableFinalPath(canonicalPath);
    if (portablePath.empty()) {
        nativeError = ERROR_INVALID_NAME;
        return false;
    }
    executable.absolutePath = wideToUtf8(portablePath);
    executable.sha256 = std::move(digest);
    executable.byteSize = byteSize;
    executable.exactFileIdentity.valid = true;
    executable.exactFileIdentity.volumeSerial = identity.VolumeSerialNumber;
    executable.exactFileIdentity.fileId = fileId;
    if (!executable.valid()) {
        nativeError = ERROR_INVALID_DATA;
        executable = {};
        return false;
    }
    pinnedImagePath = canonicalPath;
    imagePin = candidate.release();
    return true;
}

bool captureIdentity(HANDLE process, BootstrapProcessIdentity& output) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
    ULARGE_INTEGER creation{};
    creation.LowPart = created.dwLowDateTime;
    creation.HighPart = created.dwHighDateTime;

    std::vector<wchar_t> buffer(512U);
    DWORD length = static_cast<DWORD>(buffer.size());
    while (!QueryFullProcessImageNameW(process, 0U, buffer.data(), &length)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() > 32768U) {
            return false;
        }
        buffer.resize(buffer.size() * 2U);
        length = static_cast<DWORD>(buffer.size());
    }
    const std::wstring requested(buffer.data(), length);
    std::uint64_t volume = 0U;
    std::array<std::uint8_t, 16> fileId{};
    std::wstring canonical;
    if (!captureFileIdentity(requested, volume, fileId, canonical) ||
        normalizeFinalPath(requested) != normalizeFinalPath(canonical)) {
        // Exact final-path equality rejects 8.3/reparse substitution. FileId
        // is still retained so a later rebind cannot rely on path text alone.
        return false;
    }
    output.processId = GetProcessId(process);
    output.creationTime = creation.QuadPart;
    const std::wstring portablePath = portableFinalPath(canonical);
    if (portablePath.empty()) return false;
    output.imagePath = wideToUtf8(portablePath);
    output.imageVolumeSerial = volume;
    output.imageFileId = fileId;
    output.valid = output.processId != 0U && !output.imagePath.empty();
    return output.valid;
}

bool sameIdentity(const BootstrapProcessIdentity& expected,
                  const BootstrapProcessIdentity& observed) {
    return expected.valid && observed.valid &&
           expected.processId == observed.processId &&
           expected.creationTime == observed.creationTime &&
           normalizeFinalPath(utf8ToWide(expected.imagePath)) ==
               normalizeFinalPath(utf8ToWide(observed.imagePath)) &&
           expected.imageVolumeSerial == observed.imageVolumeSerial &&
           expected.imageFileId == observed.imageFileId;
}

BootstrapProcessResult classifyOpenProcessFailure(const DWORD error) {
    return {error == ERROR_INVALID_PARAMETER
                ? BootstrapProcessStatus::NotFound
                : BootstrapProcessStatus::OpenFailed,
            error};
}

bool queryTokenUser(HANDLE process, std::vector<std::uint8_t>& storage) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    DWORD required = 0U;
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0U,
                                          &required));
    if (required == 0U || required > 64U * 1024U) {
        CloseHandle(token);
        return false;
    }
    storage.resize(required);
    const bool read = GetTokenInformation(token, TokenUser, storage.data(),
                                          required, &required) != FALSE;
    CloseHandle(token);
    return read;
}

bool isSameUserProcess(HANDLE process) {
    std::vector<std::uint8_t> current;
    std::vector<std::uint8_t> parent;
    if (!queryTokenUser(GetCurrentProcess(), current) ||
        !queryTokenUser(process, parent)) {
        return false;
    }
    const auto* currentUser =
        reinterpret_cast<const TOKEN_USER*>(current.data());
    const auto* parentUser = reinterpret_cast<const TOKEN_USER*>(parent.data());
    return IsValidSid(currentUser->User.Sid) != FALSE &&
           IsValidSid(parentUser->User.Sid) != FALSE &&
           EqualSid(currentUser->User.Sid, parentUser->User.Sid) != FALSE;
}

std::optional<std::array<std::uint8_t, 32U>> verifiedSignerCertificate(
    const std::wstring& path,
    const BootstrapProcessIdentity* expectedIdentity = nullptr) {
    if (path.empty()) return std::nullopt;
    HANDLE pinned = CreateFileW(
        path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (pinned == INVALID_HANDLE_VALUE) return std::nullopt;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    FILE_ID_INFO fileIdentity{};
    const bool observed =
        GetFileInformationByHandleEx(
            pinned, FileAttributeTagInfo, &attributes, sizeof(attributes)) != FALSE &&
        GetFileInformationByHandleEx(
            pinned, FileIdInfo, &fileIdentity, sizeof(fileIdentity)) != FALSE;
    const std::wstring openedPath = finalPath(pinned);
    bool identityMatches = observed && !openedPath.empty() &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U &&
        normalizeFinalPath(path) == normalizeFinalPath(openedPath);
    if (identityMatches && expectedIdentity != nullptr) {
        std::array<std::uint8_t, 16U> fileId{};
        std::copy(std::begin(fileIdentity.FileId.Identifier),
                  std::end(fileIdentity.FileId.Identifier), fileId.begin());
        identityMatches = expectedIdentity->valid &&
            expectedIdentity->imageVolumeSerial ==
                fileIdentity.VolumeSerialNumber &&
            expectedIdentity->imageFileId == fileId &&
            normalizeFinalPath(utf8ToWide(expectedIdentity->imagePath)) ==
                normalizeFinalPath(openedPath);
    }
    if (!identityMatches) {
        CloseHandle(pinned);
        return std::nullopt;
    }

    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG verified = WinVerifyTrust(nullptr, &action, &trust);

    std::optional<std::array<std::uint8_t, 32U>> result;
    if (verified == ERROR_SUCCESS && trust.hWVTStateData != nullptr) {
        const CRYPT_PROVIDER_DATA* provider =
            WTHelperProvDataFromStateData(trust.hWVTStateData);
        const CRYPT_PROVIDER_SGNR* signer = provider == nullptr
            ? nullptr
            : WTHelperGetProvSignerFromChain(
                  const_cast<CRYPT_PROVIDER_DATA*>(provider), 0U, FALSE, 0U);
        const PCCERT_CONTEXT certificate =
            signer != nullptr && signer->csCertChain != 0U
            ? signer->pasCertChain[0].pCert
            : nullptr;
        std::array<std::uint8_t, 32U> fingerprint{};
        DWORD fingerprintSize = static_cast<DWORD>(fingerprint.size());
        if (certificate != nullptr &&
            CertGetCertificateContextProperty(
                certificate, CERT_SHA256_HASH_PROP_ID, fingerprint.data(),
                &fingerprintSize) != FALSE &&
            fingerprintSize == fingerprint.size()) {
            result = fingerprint;
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    static_cast<void>(WinVerifyTrust(nullptr, &action, &trust));
    CloseHandle(pinned);
    return result;
}

bool isTrustedUpdaterImage(const BootstrapProcessIdentity& identity) {
#if defined(ARIS_BOOTSTRAP_TEST_TRUSTED_ROOTS)
    static_cast<void>(identity);
    return true;
#else
    std::vector<wchar_t> module(512U, L'\0');
    DWORD moduleLength = 0U;
    for (;;) {
        moduleLength = GetModuleFileNameW(
            nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (moduleLength == 0U) return false;
        if (moduleLength < module.size() - 1U) break;
        if (module.size() >= 32768U) return false;
        module.resize(module.size() * 2U);
    }
    const auto updaterSigner = verifiedSignerCertificate(
        utf8ToWide(identity.imagePath), &identity);
    const auto bootstrapSigner = verifiedSignerCertificate(
        std::wstring(module.data(), moduleLength));
    // Optional-additive Authenticode trust (open-source Path A). When either
    // image carries a valid Authenticode signer, require both present and an
    // identical signer so a signed release keeps its strong publisher binding
    // and a half-signed or foreign-signed pair is rejected. When neither image
    // is Authenticode-signed — unsigned open-source builds — the parent's
    // authority instead rests on the project-key trust floor already enforced
    // in this parent and package verification path: same-user parent, parent image under the
    // updater-owned base, the inherited handoff pipe's server identity, this
    // Bootstrap's compiled-in hash pin, and the Ed25519 package-plan verification
    // in RunHarkenSearchBootstrapEntrypoint. This is not a blanket skip: an update whose
    // Ed25519 package plan fails to verify is still rejected downstream.
    if (updaterSigner.has_value() || bootstrapSigner.has_value()) {
        return updaterSigner.has_value() && bootstrapSigner.has_value() &&
               *updaterSigner == *bootstrapSigner;
    }
    return true;
#endif
}

enum class BrokerFrameKind : std::uint32_t {
    LaunchRequest = 1U,
    LaunchResponse = 2U,
    LaunchAcknowledgement = 3U,
};

constexpr std::array<std::uint8_t, 8U> kBrokerFrameMagic{
    'A', 'R', 'I', 'S', 'B', 'R', 'K', '1'};
constexpr std::uint32_t kBrokerProtocolVersion = 1U;
constexpr std::size_t kBrokerHeaderBytes = 20U;
constexpr std::size_t kMaximumBrokerPayloadBytes = 512U * 1024U;
constexpr std::uint32_t kMaximumBrokerLaunches = 2U;

void appendBrokerU32(std::vector<std::uint8_t>& bytes,
                     const std::uint32_t value) {
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendBrokerU64(std::vector<std::uint8_t>& bytes,
                     const std::uint64_t value) {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

bool consumeBrokerU32(const std::vector<std::uint8_t>& bytes,
                      std::size_t& offset, std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return false;
    value = 0U;
    for (unsigned int index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) <<
            (index * 8U);
    }
    offset += 4U;
    return true;
}

bool consumeBrokerU64(const std::vector<std::uint8_t>& bytes,
                      std::size_t& offset, std::uint64_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 8U) return false;
    value = 0U;
    for (unsigned int index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) <<
            (index * 8U);
    }
    offset += 8U;
    return true;
}

std::vector<std::uint8_t> encodeBrokerFrame(
    const BrokerFrameKind kind, const std::vector<std::uint8_t>& payload) {
    if (payload.size() > kMaximumBrokerPayloadBytes) return {};
    std::vector<std::uint8_t> frame;
    frame.reserve(kBrokerHeaderBytes + payload.size());
    frame.insert(frame.end(), kBrokerFrameMagic.begin(),
                 kBrokerFrameMagic.end());
    appendBrokerU32(frame, kBrokerProtocolVersion);
    appendBrokerU32(frame, static_cast<std::uint32_t>(kind));
    appendBrokerU32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

bool decodeBrokerHeader(const std::array<std::uint8_t, kBrokerHeaderBytes>& header,
                        BrokerFrameKind& kind, std::uint32_t& payloadBytes) {
    if (!std::equal(kBrokerFrameMagic.begin(), kBrokerFrameMagic.end(),
                    header.begin())) {
        return false;
    }
    const std::vector<std::uint8_t> bytes(header.begin(), header.end());
    std::size_t offset = kBrokerFrameMagic.size();
    std::uint32_t version = 0U;
    std::uint32_t rawKind = 0U;
    if (!consumeBrokerU32(bytes, offset, version) ||
        !consumeBrokerU32(bytes, offset, rawKind) ||
        !consumeBrokerU32(bytes, offset, payloadBytes) ||
        offset != bytes.size() || version != kBrokerProtocolVersion ||
        rawKind < static_cast<std::uint32_t>(BrokerFrameKind::LaunchRequest) ||
        rawKind >
            static_cast<std::uint32_t>(BrokerFrameKind::LaunchAcknowledgement) ||
        payloadBytes > kMaximumBrokerPayloadBytes) {
        return false;
    }
    kind = static_cast<BrokerFrameKind>(rawKind);
    return true;
}

bool transferBrokerSync(const HANDLE pipe, void* const bytes,
                        const std::size_t size, const bool write,
                        DWORD& nativeError) {
    std::size_t offset = 0U;
    while (offset < size) {
        const std::size_t remaining = size - offset;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining, std::numeric_limits<DWORD>::max()));
        DWORD transferred = 0U;
        const BOOL ok = write
            ? WriteFile(pipe, static_cast<const std::uint8_t*>(bytes) + offset,
                        requested, &transferred, nullptr)
            : ReadFile(pipe, static_cast<std::uint8_t*>(bytes) + offset,
                       requested, &transferred, nullptr);
        if (ok == FALSE || transferred == 0U || transferred > requested) {
            nativeError = ok == FALSE ? GetLastError() : ERROR_BROKEN_PIPE;
            return false;
        }
        offset += transferred;
    }
    return true;
}

bool writeBrokerFrameSync(const HANDLE pipe, const BrokerFrameKind kind,
                          const std::vector<std::uint8_t>& payload,
                          DWORD& nativeError) {
    std::vector<std::uint8_t> frame = encodeBrokerFrame(kind, payload);
    return !frame.empty() &&
        transferBrokerSync(pipe, frame.data(), frame.size(), true, nativeError);
}

bool readBrokerFrameSync(const HANDLE pipe, BrokerFrameKind& kind,
                         std::vector<std::uint8_t>& payload,
                         DWORD& nativeError) {
    std::array<std::uint8_t, kBrokerHeaderBytes> header{};
    std::uint32_t payloadBytes = 0U;
    if (!transferBrokerSync(pipe, header.data(), header.size(), false,
                            nativeError) ||
        !decodeBrokerHeader(header, kind, payloadBytes)) {
        if (nativeError == ERROR_SUCCESS) nativeError = ERROR_INVALID_DATA;
        return false;
    }
    payload.assign(payloadBytes, 0U);
    return payload.empty() ||
        transferBrokerSync(pipe, payload.data(), payload.size(), false,
                           nativeError);
}

struct BrokerOverlappedOperation final {
    OVERLAPPED overlapped{};
    HANDLE event = nullptr;

    BrokerOverlappedOperation() {
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        overlapped.hEvent = event;
    }

    ~BrokerOverlappedOperation() {
        if (event != nullptr) CloseHandle(event);
    }
};

DWORD remainingBrokerTime(const ULONGLONG deadline) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) return 0U;
    const ULONGLONG remaining = deadline - now;
    return remaining > std::numeric_limits<DWORD>::max()
        ? std::numeric_limits<DWORD>::max()
        : static_cast<DWORD>(remaining);
}

bool transferBrokerOverlapped(const HANDLE pipe, void* const bytes,
                              const std::size_t size, const bool write,
                              const ULONGLONG deadline, DWORD& nativeError) {
    std::size_t offset = 0U;
    while (offset < size) {
        auto operation = std::make_unique<BrokerOverlappedOperation>();
        if (operation->event == nullptr) {
            nativeError = GetLastError();
            return false;
        }
        const DWORD timeout = remainingBrokerTime(deadline);
        if (timeout == 0U) {
            nativeError = ERROR_TIMEOUT;
            return false;
        }
        const std::size_t remaining = size - offset;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining, std::numeric_limits<DWORD>::max()));
        DWORD transferred = 0U;
        BOOL completed = write
            ? WriteFile(pipe, static_cast<const std::uint8_t*>(bytes) + offset,
                        requested, &transferred, &operation->overlapped)
            : ReadFile(pipe, static_cast<std::uint8_t*>(bytes) + offset,
                       requested, &transferred, &operation->overlapped);
        if (completed == FALSE) {
            const DWORD pendingError = GetLastError();
            if (pendingError != ERROR_IO_PENDING) {
                nativeError = pendingError;
                return false;
            }
            const DWORD wait = WaitForSingleObject(operation->event, timeout);
            if (wait != WAIT_OBJECT_0) {
                static_cast<void>(CancelIoEx(pipe, &operation->overlapped));
                const DWORD cancelled =
                    WaitForSingleObject(operation->event, 1000U);
                if (cancelled != WAIT_OBJECT_0) {
                    // Keep the OVERLAPPED storage alive if a broken driver
                    // fails to complete cancellation. The broker session is
                    // torn down by the caller and cannot issue another I/O.
                    static_cast<void>(operation.release());
                }
                nativeError = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT
                                                   : GetLastError();
                return false;
            }
            if (GetOverlappedResult(pipe, &operation->overlapped, &transferred,
                                    FALSE) == FALSE) {
                nativeError = GetLastError();
                return false;
            }
        }
        if (transferred == 0U || transferred > requested) {
            nativeError = ERROR_BROKEN_PIPE;
            return false;
        }
        offset += transferred;
    }
    return true;
}

bool writeBrokerFrameOverlapped(const HANDLE pipe, const BrokerFrameKind kind,
                                const std::vector<std::uint8_t>& payload,
                                const ULONGLONG deadline,
                                DWORD& nativeError) {
    std::vector<std::uint8_t> frame = encodeBrokerFrame(kind, payload);
    return !frame.empty() && transferBrokerOverlapped(
        pipe, frame.data(), frame.size(), true, deadline, nativeError);
}

bool readBrokerFrameOverlapped(const HANDLE pipe, BrokerFrameKind& kind,
                               std::vector<std::uint8_t>& payload,
                               const ULONGLONG deadline,
                               DWORD& nativeError) {
    std::array<std::uint8_t, kBrokerHeaderBytes> header{};
    std::uint32_t payloadBytes = 0U;
    if (!transferBrokerOverlapped(pipe, header.data(), header.size(), false,
                                  deadline, nativeError) ||
        !decodeBrokerHeader(header, kind, payloadBytes)) {
        if (nativeError == ERROR_SUCCESS) nativeError = ERROR_INVALID_DATA;
        return false;
    }
    payload.assign(payloadBytes, 0U);
    return payload.empty() || transferBrokerOverlapped(
        pipe, payload.data(), payload.size(), false, deadline, nativeError);
}

bool createBrokerNonce(std::array<std::uint8_t, 32U>& nonce,
                       DWORD& nativeError) {
    HCRYPTPROV provider = 0;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT) == FALSE) {
        nativeError = GetLastError();
        return false;
    }
    const BOOL generated = CryptGenRandom(
        provider, static_cast<DWORD>(nonce.size()), nonce.data());
    nativeError = generated != FALSE ? ERROR_SUCCESS : GetLastError();
    static_cast<void>(CryptReleaseContext(provider, 0U));
    return generated != FALSE;
}

struct BrokerLaunchRequest final {
    std::uint32_t sequence = 0U;
    std::array<std::uint8_t, 32U> nonce{};
    std::uintptr_t targetJob = 0U;
    std::uintptr_t inheritedStdin = 0U;
    std::uintptr_t inheritedStdout = 0U;
    std::string application;
    std::string directory;
};

std::vector<std::uint8_t> encodeBrokerLaunchRequest(
    const BrokerLaunchRequest& request) {
    if (request.sequence == 0U || request.application.empty() ||
        request.directory.empty() || request.targetJob == 0U ||
        request.application.size() > std::numeric_limits<std::uint32_t>::max() ||
        request.directory.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(64U + request.application.size() + request.directory.size());
    appendBrokerU32(payload, request.sequence);
    payload.insert(payload.end(), request.nonce.begin(), request.nonce.end());
    appendBrokerU64(payload, static_cast<std::uint64_t>(request.targetJob));
    appendBrokerU64(payload, static_cast<std::uint64_t>(request.inheritedStdin));
    appendBrokerU64(payload, static_cast<std::uint64_t>(request.inheritedStdout));
    appendBrokerU32(payload,
                    static_cast<std::uint32_t>(request.application.size()));
    appendBrokerU32(payload,
                    static_cast<std::uint32_t>(request.directory.size()));
    payload.insert(payload.end(), request.application.begin(),
                   request.application.end());
    payload.insert(payload.end(), request.directory.begin(),
                   request.directory.end());
    return payload;
}

bool decodeBrokerLaunchRequest(const std::vector<std::uint8_t>& payload,
                               BrokerLaunchRequest& request) {
    std::size_t offset = 0U;
    std::uint64_t targetJob = 0U;
    std::uint64_t inheritedStdin = 0U;
    std::uint64_t inheritedStdout = 0U;
    std::uint32_t applicationBytes = 0U;
    std::uint32_t directoryBytes = 0U;
    if (!consumeBrokerU32(payload, offset, request.sequence) ||
        offset > payload.size() || payload.size() - offset < request.nonce.size()) {
        return false;
    }
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                request.nonce.size(), request.nonce.begin());
    offset += request.nonce.size();
    if (!consumeBrokerU64(payload, offset, targetJob) ||
        !consumeBrokerU64(payload, offset, inheritedStdin) ||
        !consumeBrokerU64(payload, offset, inheritedStdout) ||
        !consumeBrokerU32(payload, offset, applicationBytes) ||
        !consumeBrokerU32(payload, offset, directoryBytes) ||
        targetJob == 0U || targetJob > std::numeric_limits<std::uintptr_t>::max() ||
        inheritedStdin > std::numeric_limits<std::uintptr_t>::max() ||
        inheritedStdout > std::numeric_limits<std::uintptr_t>::max() ||
        applicationBytes == 0U || directoryBytes == 0U ||
        applicationBytes > payload.size() - offset) {
        return false;
    }
    request.application.assign(
        reinterpret_cast<const char*>(payload.data() + offset),
        applicationBytes);
    offset += applicationBytes;
    if (directoryBytes != payload.size() - offset) return false;
    request.directory.assign(
        reinterpret_cast<const char*>(payload.data() + offset), directoryBytes);
    if (request.application.find('\0') != std::string::npos ||
        request.directory.find('\0') != std::string::npos) {
        return false;
    }
    request.targetJob = static_cast<std::uintptr_t>(targetJob);
    request.inheritedStdin = static_cast<std::uintptr_t>(inheritedStdin);
    request.inheritedStdout = static_cast<std::uintptr_t>(inheritedStdout);
    return request.sequence != 0U;
}

std::vector<std::uint8_t> encodeBrokerLaunchResponse(
    const BrokerLaunchRequest& request, const DWORD nativeStatus,
    const HANDLE process, const HANDLE thread) {
    std::vector<std::uint8_t> payload;
    payload.reserve(56U);
    appendBrokerU32(payload, request.sequence);
    payload.insert(payload.end(), request.nonce.begin(), request.nonce.end());
    appendBrokerU32(payload, nativeStatus);
    appendBrokerU64(
        payload, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(process)));
    appendBrokerU64(
        payload, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(thread)));
    return payload;
}

bool decodeBrokerLaunchResponse(
    const std::vector<std::uint8_t>& payload,
    const BrokerLaunchRequest& request, DWORD& nativeStatus,
    std::uintptr_t& process, std::uintptr_t& thread) {
    std::size_t offset = 0U;
    std::uint32_t sequence = 0U;
    std::uint32_t rawNativeStatus = 0U;
    std::uint64_t processValue = 0U;
    std::uint64_t threadValue = 0U;
    if (!consumeBrokerU32(payload, offset, sequence) ||
        offset > payload.size() || payload.size() - offset < request.nonce.size() ||
        sequence != request.sequence ||
        !std::equal(request.nonce.begin(), request.nonce.end(),
                    payload.begin() + static_cast<std::ptrdiff_t>(offset))) {
        return false;
    }
    offset += request.nonce.size();
    if (!consumeBrokerU32(payload, offset, rawNativeStatus) ||
        !consumeBrokerU64(payload, offset, processValue) ||
        !consumeBrokerU64(payload, offset, threadValue) ||
        offset != payload.size() ||
        processValue > std::numeric_limits<std::uintptr_t>::max() ||
        threadValue > std::numeric_limits<std::uintptr_t>::max()) {
        return false;
    }
    process = static_cast<std::uintptr_t>(processValue);
    thread = static_cast<std::uintptr_t>(threadValue);
    nativeStatus = static_cast<DWORD>(rawNativeStatus);
    return (nativeStatus == ERROR_SUCCESS) == (process != 0U && thread != 0U);
}

std::vector<std::uint8_t> encodeBrokerAcknowledgement(
    const BrokerLaunchRequest& request, const bool accepted) {
    std::vector<std::uint8_t> payload;
    payload.reserve(40U);
    appendBrokerU32(payload, request.sequence);
    payload.insert(payload.end(), request.nonce.begin(), request.nonce.end());
    appendBrokerU32(payload, accepted ? 1U : 0U);
    return payload;
}

bool decodeBrokerAcknowledgement(const std::vector<std::uint8_t>& payload,
                                 const BrokerLaunchRequest& request,
                                 bool& accepted) {
    std::size_t offset = 0U;
    std::uint32_t sequence = 0U;
    std::uint32_t rawAccepted = 0U;
    if (!consumeBrokerU32(payload, offset, sequence) ||
        offset > payload.size() || payload.size() - offset < request.nonce.size() ||
        sequence != request.sequence ||
        !std::equal(request.nonce.begin(), request.nonce.end(),
                    payload.begin() + static_cast<std::ptrdiff_t>(offset))) {
        return false;
    }
    offset += request.nonce.size();
    if (!consumeBrokerU32(payload, offset, rawAccepted) ||
        offset != payload.size() || rawAccepted > 1U) {
        return false;
    }
    accepted = rawAccepted != 0U;
    return true;
}

std::wstring recoveryJobName(const BootstrapProcessIdentity& identity) {
    if (!identity.valid || identity.processId == 0U ||
        identity.creationTime == 0U || identity.imageVolumeSerial == 0U) {
        return {};
    }
    constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring fileId;
    fileId.reserve(identity.imageFileId.size() * 2U);
    for (const std::uint8_t byte : identity.imageFileId) {
        fileId.push_back(kHex[byte >> 4U]);
        fileId.push_back(kHex[byte & 0x0fU]);
    }
    return L"Local\\HarkenSearchBootstrap-Tree-v1-" +
        std::to_wstring(identity.processId) + L"-" +
        std::to_wstring(identity.creationTime) + L"-" +
        std::to_wstring(identity.imageVolumeSerial) + L"-" + fileId;
}

HANDLE createRestrictedRecoveryJob(const std::wstring& name,
                                   DWORD& nativeError,
                                   bool& collision) {
    nativeError = ERROR_SUCCESS;
    collision = false;
    ScopedHandle token;
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        nativeError = GetLastError();
        return nullptr;
    }
    token.reset(rawToken);
    DWORD tokenBytes = 0U;
    static_cast<void>(GetTokenInformation(token.get(), TokenUser, nullptr, 0U,
                                          &tokenBytes));
    if (tokenBytes == 0U || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        nativeError = GetLastError();
        return nullptr;
    }
    std::vector<std::uint8_t> tokenStorage(tokenBytes);
    if (!GetTokenInformation(token.get(), TokenUser, tokenStorage.data(),
                             tokenBytes, &tokenBytes)) {
        nativeError = GetLastError();
        return nullptr;
    }
    const auto* const tokenUser =
        reinterpret_cast<const TOKEN_USER*>(tokenStorage.data());
    if (!IsValidSid(tokenUser->User.Sid)) {
        nativeError = ERROR_INVALID_SID;
        return nullptr;
    }
    const DWORD sidBytes = GetLengthSid(tokenUser->User.Sid);
    const DWORD aclBytes = static_cast<DWORD>(
        sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sidBytes);
    std::vector<std::uint8_t> aclStorage(aclBytes);
    auto* const acl = reinterpret_cast<ACL*>(aclStorage.data());
    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeAcl(acl, aclBytes, ACL_REVISION) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, JOB_OBJECT_ALL_ACCESS,
                             tokenUser->User.Sid) ||
        !InitializeSecurityDescriptor(&descriptor,
                                      SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)) {
        nativeError = GetLastError();
        return nullptr;
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = &descriptor;
    security.bInheritHandle = FALSE;
    SetLastError(ERROR_SUCCESS);
    HANDLE job = CreateJobObjectW(&security, name.c_str());
    nativeError = job == nullptr ? GetLastError() : ERROR_SUCCESS;
    collision = job != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
    return job;
}

enum class NativeProcessRole { Old, Child, Detached };

struct NativeProcessRecord {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE initialThread = nullptr;
    HANDLE imagePin = nullptr;
    std::wstring pinnedImagePath;
    NativeProcessRole role = NativeProcessRole::Detached;
    bool suspended = false;
    bool rebound = false;
};

class NativeProcessPort final : public BootstrapProcessPort {
public:
    NativeProcessPort() = default;

    NativeProcessPort(HANDLE brokerChannel, HANDLE brokerProcess,
                      HANDLE brokerJob,
                      BootstrapProcessIdentity brokerIdentity,
                      const std::uint32_t brokerTimeoutMs) noexcept
        : brokerChannel_(brokerChannel), brokerProcess_(brokerProcess),
          brokerJob_(brokerJob), brokerIdentity_(std::move(brokerIdentity)),
          brokerTimeoutMs_(brokerTimeoutMs) {}

    ~NativeProcessPort() override {
        for (auto& entry : handles_) closeRecord(entry.second, false);
        closeBroker(true);
    }

    BootstrapProcessResult OpenExpected(
        const BootstrapProcessIdentity& expected) override {
        if (!expected.valid || expected.processId == 0U) {
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
            static_cast<DWORD>(expected.processId));
        if (process == nullptr) {
            const BootstrapProcessResult classified =
                classifyOpenProcessFailure(GetLastError());
            if (classified.status != BootstrapProcessStatus::NotFound) {
                return classified;
            }
            HANDLE imagePin = nullptr;
            std::wstring pinnedImagePath;
            BootstrapExecutableExpectation executable;
            DWORD nativeError = ERROR_SUCCESS;
            if (!pinExactExecutableSnapshot(expected, imagePin,
                                            pinnedImagePath, executable,
                                            nativeError)) {
                return {BootstrapProcessStatus::IdentityMismatch, nativeError};
            }
            CloseHandle(imagePin);
            return {BootstrapProcessStatus::NotFound,
                    classified.nativeError, {}, std::move(executable)};
        }
        const DWORD initialWait = WaitForSingleObject(process, 0U);
        if (initialWait == WAIT_OBJECT_0) {
            HANDLE imagePin = nullptr;
            std::wstring pinnedImagePath;
            BootstrapExecutableExpectation executable;
            DWORD nativeError = ERROR_SUCCESS;
            const bool pinned = pinExactExecutableSnapshot(
                expected, imagePin, pinnedImagePath, executable, nativeError);
            if (imagePin != nullptr && imagePin != INVALID_HANDLE_VALUE) {
                CloseHandle(imagePin);
            }
            CloseHandle(process);
            if (!pinned) {
                return {BootstrapProcessStatus::IdentityMismatch, nativeError};
            }
            return {BootstrapProcessStatus::NotFound,
                    ERROR_PROCESS_ABORTED, {}, std::move(executable)};
        }
        if (initialWait == WAIT_FAILED) {
            const DWORD error = GetLastError();
            CloseHandle(process);
            return {BootstrapProcessStatus::WaitFailed, error};
        }
        BootstrapProcessIdentity observed;
        if (!captureIdentity(process, observed)) {
            const DWORD terminalWait = WaitForSingleObject(process, 0U);
            const DWORD error = GetLastError();
            CloseHandle(process);
            if (terminalWait == WAIT_OBJECT_0) {
                return {BootstrapProcessStatus::NotFound,
                        ERROR_PROCESS_ABORTED};
            }
            return {terminalWait == WAIT_FAILED
                        ? BootstrapProcessStatus::WaitFailed
                        : BootstrapProcessStatus::IdentityMismatch,
                    terminalWait == WAIT_FAILED ? error : ERROR_INVALID_DATA};
        }
        if (!sameIdentity(expected, observed)) {
            CloseHandle(process);
            return {BootstrapProcessStatus::IdentityMismatch};
        }
        HANDLE imagePin = nullptr;
        std::wstring pinnedImagePath;
        BootstrapExecutableExpectation executable;
        DWORD nativeError = ERROR_SUCCESS;
        if (!pinExactExecutableSnapshot(observed, imagePin, pinnedImagePath,
                                        executable, nativeError)) {
            CloseHandle(process);
            return {BootstrapProcessStatus::IdentityMismatch, nativeError};
        }
        return retain(process, nullptr, nullptr, NativeProcessRole::Old, false,
                      observed, imagePin, std::move(pinnedImagePath), false,
                      std::move(executable));
    }

    BootstrapProcessResult RebindOwnedChild(
        const BootstrapProcessIdentity& expected) override {
        if (!expected.valid || expected.processId == 0U) {
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        const std::wstring jobName = recoveryJobName(expected);
        if (jobName.empty()) {
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        ScopedHandle recoveryJob(OpenJobObjectW(
            JOB_OBJECT_QUERY | JOB_OBJECT_TERMINATE |
                JOB_OBJECT_SET_ATTRIBUTES,
            FALSE, jobName.c_str()));
        const DWORD jobOpenError = recoveryJob.get() == nullptr
            ? GetLastError()
            : ERROR_SUCCESS;
        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE, static_cast<DWORD>(expected.processId));
        if (process == nullptr) {
            const DWORD processError = GetLastError();
            if (recoveryJob.get() != nullptr) {
                return {BootstrapProcessStatus::IdentityMismatch,
                        ERROR_INVALID_DATA};
            }
            return classifyOpenProcessFailure(processError);
        }
        const DWORD initialWait = WaitForSingleObject(process, 0U);
        if (initialWait == WAIT_OBJECT_0) {
            if (recoveryJob.get() != nullptr) {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                if (!QueryInformationJobObject(
                        recoveryJob.get(), JobObjectBasicAccountingInformation,
                        &accounting, sizeof(accounting), nullptr)) {
                    const DWORD error = GetLastError();
                    CloseHandle(process);
                    return {BootstrapProcessStatus::IdentityMismatch, error};
                }
                // A released child may signal just before its inherited
                // recovery-job handle closes.  An empty exact job proves
                // there is no live child left to substitute; keep the
                // fail-closed identity result for any active member.
                if (accounting.ActiveProcesses != 0U) {
                    CloseHandle(process);
                    return {BootstrapProcessStatus::IdentityMismatch,
                            ERROR_PROCESS_ABORTED};
                }
            }
            CloseHandle(process);
            return {BootstrapProcessStatus::NotFound, ERROR_PROCESS_ABORTED};
        }
        if (initialWait == WAIT_FAILED) {
            const DWORD error = GetLastError();
            CloseHandle(process);
            return {BootstrapProcessStatus::WaitFailed, error};
        }
        if (recoveryJob.get() == nullptr) {
            CloseHandle(process);
            return {BootstrapProcessStatus::IdentityMismatch,
                    jobOpenError == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                                  : jobOpenError};
        }
        BootstrapProcessIdentity observed;
        if (!captureIdentity(process, observed)) {
            const DWORD terminalWait = WaitForSingleObject(process, 0U);
            const DWORD error = GetLastError();
            CloseHandle(process);
            if (terminalWait == WAIT_OBJECT_0) {
                return {BootstrapProcessStatus::IdentityMismatch,
                        ERROR_PROCESS_ABORTED};
            }
            return {terminalWait == WAIT_FAILED
                        ? BootstrapProcessStatus::WaitFailed
                        : BootstrapProcessStatus::IdentityMismatch,
                    terminalWait == WAIT_FAILED ? error : ERROR_INVALID_DATA};
        }
        if (!sameIdentity(expected, observed)) {
            CloseHandle(process);
            return {BootstrapProcessStatus::IdentityMismatch};
        }
        BOOL inRecoveryJob = FALSE;
        if (!IsProcessInJob(process, recoveryJob.get(), &inRecoveryJob) ||
            inRecoveryJob == FALSE) {
            const DWORD error = GetLastError();
            CloseHandle(process);
            return {BootstrapProcessStatus::IdentityMismatch,
                    error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error};
        }
        const std::wstring observedPath = utf8ToWide(observed.imagePath);
        HANDLE imagePin = CreateFileW(
            observedPath.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (imagePin == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            CloseHandle(process);
            return {BootstrapProcessStatus::OpenFailed, error};
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        FILE_ID_INFO information{};
        const bool observedPin =
            GetFileInformationByHandleEx(
                imagePin, FileAttributeTagInfo, &attributes,
                sizeof(attributes)) != FALSE &&
            GetFileInformationByHandleEx(
                imagePin, FileIdInfo, &information, sizeof(information)) != FALSE;
        const DWORD observationError = observedPin ? ERROR_SUCCESS : GetLastError();
        const std::wstring pinnedPath = finalPath(imagePin);
        std::array<std::uint8_t, 16U> pinnedFileId{};
        if (observedPin) {
            std::copy(std::begin(information.FileId.Identifier),
                      std::end(information.FileId.Identifier),
                      pinnedFileId.begin());
        }
        if (!observedPin || pinnedPath.empty() ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
            normalizeFinalPath(observedPath) != normalizeFinalPath(pinnedPath) ||
            information.VolumeSerialNumber != observed.imageVolumeSerial ||
            pinnedFileId != observed.imageFileId) {
            CloseHandle(imagePin);
            CloseHandle(process);
            return {BootstrapProcessStatus::IdentityMismatch,
                    observationError == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                                      : observationError};
        }
        return retain(process, recoveryJob.release(), nullptr,
                      NativeProcessRole::Child, false, observed, imagePin,
                      pinnedPath, true);
    }

    BootstrapProcessResult WaitForExit(BootstrapOwnedProcess& process,
                                       const std::uint32_t timeoutMs) override {
        NativeProcessRecord* record = lookup(process);
        if (record == nullptr) return {BootstrapProcessStatus::InvalidIdentity};
        BootstrapProcessIdentity observed;
        if (!captureIdentity(record->process, observed) ||
            !sameIdentity(process.identity, observed)) {
            // A retained process HANDLE cannot be rebound to a different PID
            // after it exits.  QueryFullProcessImageNameW may legitimately
            // fail during the terminal window, so accept only an already
            // signalled retained handle; a live mismatch remains fatal.
            if (WaitForSingleObject(record->process, 0U) != WAIT_OBJECT_0) {
                return {BootstrapProcessStatus::IdentityMismatch};
            }
        }
        const DWORD wait = WaitForSingleObject(record->process, timeoutMs);
        if (wait == WAIT_TIMEOUT) return {BootstrapProcessStatus::TimedOut};
        if (wait != WAIT_OBJECT_0) return {BootstrapProcessStatus::WaitFailed, GetLastError()};
        return {BootstrapProcessStatus::Ok};
    }

    BootstrapProcessResult Launch(
        const std::string_view absoluteExecutable,
        const std::string_view workingDirectory,
        const BootstrapLaunchCapability& capability) override {
        return launch(absoluteExecutable, workingDirectory, capability, false,
                      NativeProcessRole::Detached);
    }

    BootstrapProcessResult LaunchSuspended(
        const std::string_view absoluteExecutable,
        const std::string_view workingDirectory,
        const BootstrapLaunchCapability& capability) override {
        return launch(absoluteExecutable, workingDirectory, capability, true,
                      NativeProcessRole::Child);
    }

    BootstrapProcessResult ValidateOwnedExecutable(
        const BootstrapOwnedProcess& process,
        const BootstrapExecutableExpectation& expected) override {
        NativeProcessRecord* record = lookup(process);
        if (record == nullptr || record->role != NativeProcessRole::Child ||
            (!record->suspended && !record->rebound) || !expected.valid()) {
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        BootstrapProcessIdentity observed;
        if (!captureIdentity(record->process, observed) ||
            !sameIdentity(process.identity, observed)) {
            return {BootstrapProcessStatus::IdentityMismatch};
        }
        const std::wstring expectedPath = utf8ToWide(expected.absolutePath);
        if (expectedPath.empty() ||
            normalizeFinalPath(expectedPath) !=
                normalizeFinalPath(utf8ToWide(observed.imagePath))) {
            return {BootstrapProcessStatus::IdentityMismatch};
        }
        if (expected.exactFileIdentity.valid &&
            (observed.imageVolumeSerial !=
                 expected.exactFileIdentity.volumeSerial ||
             observed.imageFileId != expected.exactFileIdentity.fileId)) {
            return {BootstrapProcessStatus::IdentityMismatch,
                    ERROR_INVALID_DATA};
        }
        std::uint64_t size = 0U;
        std::string digest;
        DWORD nativeError = ERROR_SUCCESS;
        if (record->imagePin == nullptr || record->imagePin == INVALID_HANDLE_VALUE ||
            normalizeFinalPath(record->pinnedImagePath) !=
                normalizeFinalPath(expectedPath) ||
            !hashPinnedFileSha256(record->imagePin, expectedPath, size, digest,
                                  nativeError)) {
            return {BootstrapProcessStatus::LaunchFailed, nativeError};
        }
        if (size != expected.byteSize ||
            lowerAscii(expected.sha256) != digest) {
            return {BootstrapProcessStatus::IdentityMismatch, ERROR_INVALID_DATA};
        }
        return {BootstrapProcessStatus::Ok};
    }

    BootstrapProcessResult ResumeOwned(BootstrapOwnedProcess& process) override {
        NativeProcessRecord* record = lookup(process);
        if (record == nullptr || record->role != NativeProcessRole::Child ||
            !record->suspended) {
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        if (record->initialThread == nullptr ||
            record->initialThread == INVALID_HANDLE_VALUE) {
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        if (ResumeThread(record->initialThread) == static_cast<DWORD>(-1)) {
            return {BootstrapProcessStatus::WaitFailed, GetLastError()};
        }
        CloseHandle(record->initialThread);
        record->initialThread = nullptr;
        record->suspended = false;
        return {BootstrapProcessStatus::Ok};
    }

    BootstrapProcessResult PrepareOwnedForCommit(
        BootstrapOwnedProcess& process) override {
        NativeProcessRecord* record = lookup(process);
        if (record == nullptr || record->role != NativeProcessRole::Child) {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
            std::fprintf(stderr, "runtime:prepare-ownership-invalid\n");
#endif
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        const DWORD wait = WaitForSingleObject(record->process, 0U);
        if (wait == WAIT_FAILED) {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
            std::fprintf(stderr, "runtime:prepare-ownership-wait-failed=%lu\n",
                         static_cast<unsigned long>(GetLastError()));
#endif
            return {BootstrapProcessStatus::WaitFailed, GetLastError()};
        }
        if (wait == WAIT_OBJECT_0) {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
            std::fprintf(stderr, "runtime:prepare-ownership-child-exited\n");
#endif
            return {BootstrapProcessStatus::IdentityMismatch,
                    ERROR_PROCESS_ABORTED};
        }
        BootstrapProcessIdentity observed;
        if (!captureIdentity(record->process, observed) ||
            !sameIdentity(process.identity, observed)) {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
            std::fprintf(stderr, "runtime:prepare-ownership-identity-mismatch\n");
#endif
            return {BootstrapProcessStatus::IdentityMismatch};
        }
        // Health acceptance is not commit. Keep kill-on-close armed through
        // durable CommitJournal publication; ReleaseOwned is the only normal
        // transition that disarms the job after commit succeeds.
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
        std::fprintf(stderr, "runtime:prepare-ownership-ok\n");
#endif
        return {BootstrapProcessStatus::Ok};
    }

    BootstrapProcessResult TerminateOwned(BootstrapOwnedProcess& process,
                                          const std::uint32_t exitCode) override {
        NativeProcessRecord* record = lookup(process);
        if (record == nullptr || record->role != NativeProcessRole::Child) {
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        const DWORD initialWait = WaitForSingleObject(record->process, 0U);
        if (initialWait == WAIT_FAILED) {
            return {BootstrapProcessStatus::WaitFailed, GetLastError()};
        }
        if (initialWait == WAIT_OBJECT_0) {
            const BootstrapProcessResult drained =
                terminateAndDrainJob(*record, exitCode);
            if (!drained.ok()) return drained;
            close(process);
            return {BootstrapProcessStatus::Ok};
        }
        BootstrapProcessIdentity observed;
        if (!captureIdentity(record->process, observed) ||
            !sameIdentity(process.identity, observed)) {
            // A process can stop exposing its image identity immediately
            // before its retained HANDLE becomes signalled.  This is not PID
            // substitution: the owned HANDLE cannot be rebound.  Accept only
            // a bounded transition to the exited state; a still-live mismatch
            // remains fail-closed.
            const DWORD terminalWait =
                WaitForSingleObject(record->process, 1000U);
            if (terminalWait == WAIT_OBJECT_0) {
                const BootstrapProcessResult drained =
                    terminateAndDrainJob(*record, exitCode);
                if (!drained.ok()) return drained;
                close(process);
                return {BootstrapProcessStatus::Ok};
            }
            if (terminalWait == WAIT_FAILED) {
                return {BootstrapProcessStatus::WaitFailed, GetLastError()};
            }
            return {BootstrapProcessStatus::IdentityMismatch};
        }
        if (record->job != nullptr) {
            const BootstrapProcessResult drained =
                terminateAndDrainJob(*record, exitCode);
            if (!drained.ok()) return drained;
            close(process);
            return {BootstrapProcessStatus::Ok};
        }
        if (!TerminateProcess(record->process, exitCode)) {
            const DWORD terminationError = GetLastError();
            const DWORD terminalWait =
                WaitForSingleObject(record->process, 1000U);
            if (terminalWait == WAIT_OBJECT_0) {
                close(process);
                return {BootstrapProcessStatus::Ok};
            }
            return {BootstrapProcessStatus::TerminationFailed,
                    terminalWait == WAIT_FAILED ? GetLastError()
                                                : terminationError};
        }
        if (WaitForSingleObject(record->process, 5000U) != WAIT_OBJECT_0) {
            return {BootstrapProcessStatus::TerminationFailed, GetLastError()};
        }
        close(process);
        return {BootstrapProcessStatus::Ok};
    }

    BootstrapProcessResult ReleaseOwned(BootstrapOwnedProcess& process) override {
        NativeProcessRecord* record = lookup(process);
        if (record == nullptr || record->role != NativeProcessRole::Child) {
            return {BootstrapProcessStatus::InvalidIdentity};
        }
        const DWORD wait = WaitForSingleObject(record->process, 0U);
        if (wait == WAIT_FAILED) {
            return {BootstrapProcessStatus::WaitFailed, GetLastError()};
        }
        if (wait == WAIT_OBJECT_0) {
            close(process);
            return {BootstrapProcessStatus::IdentityMismatch,
                    ERROR_PROCESS_ABORTED};
        }
        BootstrapProcessIdentity observed;
        if (!captureIdentity(record->process, observed) ||
            !sameIdentity(process.identity, observed)) {
            return {BootstrapProcessStatus::IdentityMismatch};
        }
        if (record->job != nullptr) {
            std::vector<RecoveryJobPin> pins;
            const BootstrapProcessResult pinned =
                pinRecoveryJobInMembers(*record, pins);
            if (!pinned.ok()) return pinned;
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            if (!SetInformationJobObject(record->job,
                                          JobObjectExtendedLimitInformation,
                                          &limits, sizeof(limits))) {
                const DWORD error = GetLastError();
                closeRecoveryJobPins(pins);
                return {BootstrapProcessStatus::TerminationFailed, error};
            }
        }
        close(process);
        return {BootstrapProcessStatus::Ok};
    }

    void Close(BootstrapOwnedProcess& process) noexcept override { close(process); }

private:
    struct RecoveryJobPin final {
        ScopedHandle process;
        HANDLE remoteHandle = nullptr;
    };

    static void closeRecoveryJobPins(
        std::vector<RecoveryJobPin>& pins) noexcept {
        for (RecoveryJobPin& pin : pins) {
            HANDLE localPin = nullptr;
            if (pin.process.get() != nullptr && pin.remoteHandle != nullptr &&
                DuplicateHandle(pin.process.get(), pin.remoteHandle,
                                GetCurrentProcess(), &localPin, 0U, FALSE,
                                DUPLICATE_SAME_ACCESS |
                                    DUPLICATE_CLOSE_SOURCE) &&
                localPin != nullptr) {
                CloseHandle(localPin);
            }
            pin.remoteHandle = nullptr;
        }
    }

    static BootstrapProcessResult pinRecoveryJobInMembers(
        NativeProcessRecord& record, std::vector<RecoveryJobPin>& pins) {
        constexpr std::size_t kMaximumOwnedProcesses = 256U;
        struct JobProcessIds final {
            DWORD assigned = 0U;
            DWORD listed = 0U;
            std::array<ULONG_PTR, kMaximumOwnedProcesses> processIds{};
        } processIds;
        if (!QueryInformationJobObject(
                record.job, JobObjectBasicProcessIdList, &processIds,
                sizeof(processIds), nullptr) ||
            processIds.assigned > processIds.listed ||
            processIds.listed > kMaximumOwnedProcesses) {
            return {BootstrapProcessStatus::TerminationFailed,
                    GetLastError() == ERROR_SUCCESS ? ERROR_INSUFFICIENT_BUFFER
                                                    : GetLastError()};
        }
        const DWORD rootProcessId = GetProcessId(record.process);
        bool pinnedRoot = false;
        pins.reserve(processIds.listed);
        for (DWORD index = 0U; index < processIds.listed; ++index) {
            const DWORD processId =
                static_cast<DWORD>(processIds.processIds[index]);
            ScopedHandle member(OpenProcess(
                PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION |
                    SYNCHRONIZE,
                FALSE, processId));
            if (member.get() == nullptr) {
                if (GetLastError() == ERROR_INVALID_PARAMETER) continue;
                closeRecoveryJobPins(pins);
                return {BootstrapProcessStatus::TerminationFailed,
                        GetLastError()};
            }
            if (WaitForSingleObject(member.get(), 0U) == WAIT_OBJECT_0) continue;
            BOOL inJob = FALSE;
            if (!IsProcessInJob(member.get(), record.job, &inJob) ||
                inJob == FALSE) {
                const DWORD error = GetLastError();
                closeRecoveryJobPins(pins);
                return {BootstrapProcessStatus::TerminationFailed,
                        error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error};
            }
            HANDLE remotePin = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), record.job, member.get(),
                                 &remotePin, JOB_OBJECT_QUERY, FALSE, 0U)) {
                const DWORD error = GetLastError();
                closeRecoveryJobPins(pins);
                return {BootstrapProcessStatus::TerminationFailed, error};
            }
            pinnedRoot = pinnedRoot || processId == rootProcessId;
            pins.push_back({std::move(member), remotePin});
        }
        if (!pinnedRoot) {
            closeRecoveryJobPins(pins);
            return {BootstrapProcessStatus::TerminationFailed,
                    ERROR_PROCESS_ABORTED};
        }
        return {BootstrapProcessStatus::Ok};
    }

    static BootstrapProcessResult bindRecoveryJob(
        const HANDLE process, ScopedHandle& launchCutJob,
        const BootstrapProcessIdentity& identity,
        ScopedHandle& recoveryJob) {
        const std::wstring name = recoveryJobName(identity);
        if (process == nullptr || process == INVALID_HANDLE_VALUE ||
            launchCutJob.get() == nullptr ||
            launchCutJob.get() == INVALID_HANDLE_VALUE || name.empty()) {
            return {BootstrapProcessStatus::LaunchFailed,
                    ERROR_INVALID_PARAMETER};
        }
        DWORD nativeError = ERROR_SUCCESS;
        bool collision = false;
        ScopedHandle candidate(
            createRestrictedRecoveryJob(name, nativeError, collision));
        if (candidate.get() == nullptr || collision) {
            return {BootstrapProcessStatus::LaunchFailed,
                    collision ? static_cast<std::uint32_t>(ERROR_ALREADY_EXISTS)
                              : nativeError};
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION recoveryLimits{};
        recoveryLimits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                candidate.get(), JobObjectExtendedLimitInformation,
                &recoveryLimits, sizeof(recoveryLimits)) ||
            !AssignProcessToJobObject(candidate.get(), process)) {
            return {BootstrapProcessStatus::LaunchFailed, GetLastError()};
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION disarmedLaunchCut{};
        if (!SetInformationJobObject(
                launchCutJob.get(), JobObjectExtendedLimitInformation,
                &disarmedLaunchCut, sizeof(disarmedLaunchCut))) {
            return {BootstrapProcessStatus::LaunchFailed, GetLastError()};
        }
        launchCutJob.reset();
        recoveryJob = std::move(candidate);
        return {BootstrapProcessStatus::Ok};
    }

    static BootstrapProcessResult terminateAndDrainJobHandles(
        const HANDLE rootProcess, const HANDLE job,
        const std::uint32_t exitCode) {
        if (job == nullptr || job == INVALID_HANDLE_VALUE) {
            return {BootstrapProcessStatus::Ok};
        }

        constexpr std::size_t kMaximumOwnedProcesses = 256U;
        struct JobProcessIds final {
            DWORD assigned = 0U;
            DWORD listed = 0U;
            std::array<ULONG_PTR, kMaximumOwnedProcesses> processIds{};
        } processIds;
        if (!QueryInformationJobObject(
                job, JobObjectBasicProcessIdList, &processIds,
                sizeof(processIds), nullptr)) {
            return {BootstrapProcessStatus::TerminationFailed, GetLastError()};
        }
        if (processIds.assigned > processIds.listed ||
            processIds.listed > kMaximumOwnedProcesses) {
            return {BootstrapProcessStatus::TerminationFailed,
                    ERROR_INSUFFICIENT_BUFFER};
        }
        std::vector<ScopedHandle> memberProcesses;
        memberProcesses.reserve(processIds.listed);
        for (DWORD index = 0U; index < processIds.listed; ++index) {
            const DWORD processId =
                static_cast<DWORD>(processIds.processIds[index]);
            ScopedHandle member(OpenProcess(
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                processId));
            if (member.get() == nullptr) {
                const DWORD error = GetLastError();
                if (error == ERROR_INVALID_PARAMETER) continue;
                return {BootstrapProcessStatus::TerminationFailed, error};
            }
            BOOL inJob = FALSE;
            if (!IsProcessInJob(member.get(), job, &inJob) ||
                inJob == FALSE) {
                const DWORD error = GetLastError();
                return {BootstrapProcessStatus::TerminationFailed,
                        error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error};
            }
            memberProcesses.emplace_back(std::move(member));
        }
        if (!TerminateJobObject(job, exitCode)) {
            return {BootstrapProcessStatus::TerminationFailed, GetLastError()};
        }
        const ULONGLONG deadline = GetTickCount64() + 5000U;
        for (const ScopedHandle& member : memberProcesses) {
            const ULONGLONG now = GetTickCount64();
            const DWORD remaining = now < deadline
                ? static_cast<DWORD>(deadline - now)
                : 0U;
            const DWORD memberWait =
                WaitForSingleObject(member.get(), remaining);
            if (memberWait == WAIT_FAILED) {
                return {BootstrapProcessStatus::WaitFailed, GetLastError()};
            }
            if (memberWait != WAIT_OBJECT_0) {
                return {BootstrapProcessStatus::TerminationFailed,
                        ERROR_TIMEOUT};
            }
        }
        for (;;) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            if (!QueryInformationJobObject(
                    job, JobObjectBasicAccountingInformation,
                    &accounting, sizeof(accounting), nullptr)) {
                return {BootstrapProcessStatus::TerminationFailed,
                        GetLastError()};
            }
            if (accounting.ActiveProcesses == 0U) break;
            if (GetTickCount64() >= deadline) {
                return {BootstrapProcessStatus::TerminationFailed,
                        ERROR_TIMEOUT};
            }
            Sleep(5U);
        }
        if (rootProcess == nullptr || rootProcess == INVALID_HANDLE_VALUE) {
            return {BootstrapProcessStatus::Ok};
        }
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now < deadline
            ? static_cast<DWORD>(deadline - now)
            : 0U;
        const DWORD processWait = WaitForSingleObject(rootProcess, remaining);
        if (processWait == WAIT_FAILED) {
            return {BootstrapProcessStatus::WaitFailed, GetLastError()};
        }
        if (processWait != WAIT_OBJECT_0) {
            return {BootstrapProcessStatus::TerminationFailed, ERROR_TIMEOUT};
        }
        return {BootstrapProcessStatus::Ok};
    }

    static BootstrapProcessResult terminateAndDrainJob(
        NativeProcessRecord& record, const std::uint32_t exitCode) {
        return terminateAndDrainJobHandles(record.process, record.job,
                                           exitCode);
    }

    void closeBroker(const bool terminate) noexcept {
        if (terminate && brokerJob_ != nullptr &&
            brokerJob_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(TerminateJobObject(brokerJob_, 0xE001U));
        }
        if (brokerChannel_ != nullptr &&
            brokerChannel_ != INVALID_HANDLE_VALUE) {
            CloseHandle(brokerChannel_);
        }
        if (brokerProcess_ != nullptr &&
            brokerProcess_ != INVALID_HANDLE_VALUE) {
            CloseHandle(brokerProcess_);
        }
        if (brokerJob_ != nullptr && brokerJob_ != INVALID_HANDLE_VALUE) {
            CloseHandle(brokerJob_);
        }
        brokerChannel_ = nullptr;
        brokerProcess_ = nullptr;
        brokerJob_ = nullptr;
        brokerIdentity_ = {};
        brokerTimeoutMs_ = 0U;
    }

    NativeProcessRecord* lookup(const BootstrapOwnedProcess& process) {
        const auto found = handles_.find(process.ownershipToken);
        return found == handles_.end() ? nullptr : &found->second;
    }

    static void closeRecord(NativeProcessRecord& record, const bool detachJob) noexcept {
        if (record.initialThread != nullptr &&
            record.initialThread != INVALID_HANDLE_VALUE) {
            CloseHandle(record.initialThread);
            record.initialThread = nullptr;
        }
        if (record.imagePin != nullptr && record.imagePin != INVALID_HANDLE_VALUE) {
            CloseHandle(record.imagePin);
            record.imagePin = nullptr;
        }
        if (record.job != nullptr) {
            if (detachJob) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                static_cast<void>(SetInformationJobObject(
                    record.job, JobObjectExtendedLimitInformation, &limits,
                    sizeof(limits)));
            }
            CloseHandle(record.job);
            record.job = nullptr;
        }
        if (record.process != nullptr) {
            CloseHandle(record.process);
            record.process = nullptr;
        }
    }

    void close(BootstrapOwnedProcess& process) noexcept {
        const auto found = handles_.find(process.ownershipToken);
        if (found != handles_.end()) {
            closeRecord(found->second, false);
            handles_.erase(found);
        }
        process = {};
    }

    BootstrapProcessResult retain(HANDLE process, HANDLE job,
                                  HANDLE initialThread,
                                  const NativeProcessRole role,
                                  const bool suspended,
                                  const BootstrapProcessIdentity& identity,
                                   HANDLE imagePin = nullptr,
                                   std::wstring pinnedImagePath = {},
                                   const bool rebound = false,
                                   BootstrapExecutableExpectation executable = {}) {
        std::uintptr_t token = nextToken_++;
        if (token == 0U) token = nextToken_++;
        handles_.emplace(
            token, NativeProcessRecord{process, job, initialThread, imagePin,
                                       std::move(pinnedImagePath), role,
                                       suspended, rebound});
        return {BootstrapProcessStatus::Ok, 0,
                {identity, token, reinterpret_cast<std::uintptr_t>(process)},
                std::move(executable)};
    }

    BootstrapProcessResult launchBrokered(
        const std::string_view absoluteExecutable,
        const std::string_view workingDirectory,
        const BootstrapLaunchCapability& capability,
        const NativeProcessRole role) {
        if (launchSequence_ >= kMaximumBrokerLaunches ||
            brokerChannel_ == nullptr ||
            brokerChannel_ == INVALID_HANDLE_VALUE ||
            brokerProcess_ == nullptr ||
            brokerProcess_ == INVALID_HANDLE_VALUE ||
            WaitForSingleObject(brokerProcess_, 0U) != WAIT_TIMEOUT) {
            return {BootstrapProcessStatus::LaunchFailed,
                    static_cast<std::uint32_t>(
                        launchSequence_ >= kMaximumBrokerLaunches
                            ? ERROR_TOO_MANY_CMDS
                            : ERROR_BROKEN_PIPE)};
        }
        ++launchSequence_;
        const std::wstring application = utf8ToWide(absoluteExecutable);
        const std::wstring directory = utf8ToWide(workingDirectory);
        if (application.empty() || directory.empty()) {
            return {BootstrapProcessStatus::LaunchFailed,
                    ERROR_INVALID_PARAMETER};
        }

        ScopedHandle imagePin(CreateFileW(
            application.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        const std::wstring pinnedImagePath = finalPath(imagePin.get());
        if (imagePin.get() == INVALID_HANDLE_VALUE ||
            GetFileInformationByHandleEx(
                imagePin.get(), FileAttributeTagInfo, &attributes,
                sizeof(attributes)) == FALSE ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
            pinnedImagePath.empty() ||
            normalizeFinalPath(pinnedImagePath) !=
                normalizeFinalPath(application)) {
            const DWORD error = GetLastError();
            return {BootstrapProcessStatus::LaunchFailed,
                    error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error};
        }

        ScopedHandle targetJob(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION targetLimits{};
        targetLimits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (targetJob.get() == nullptr ||
            SetInformationJobObject(
                targetJob.get(), JobObjectExtendedLimitInformation,
                &targetLimits, sizeof(targetLimits)) == FALSE) {
            return {BootstrapProcessStatus::LaunchFailed, GetLastError()};
        }

        HANDLE remoteJob = nullptr;
        HANDLE remoteStdin = nullptr;
        HANDLE remoteStdout = nullptr;
        const HANDLE current = GetCurrentProcess();
        if (!DuplicateHandle(current, targetJob.get(), brokerProcess_,
                             &remoteJob, 0U, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            return {BootstrapProcessStatus::LaunchFailed, GetLastError()};
        }
        if (capability.inheritedStdin != 0U &&
            !DuplicateHandle(
                current,
                reinterpret_cast<HANDLE>(capability.inheritedStdin),
                brokerProcess_, &remoteStdin, 0U, TRUE,
                DUPLICATE_SAME_ACCESS)) {
            const DWORD error = GetLastError();
            closeBroker(true);
            return {BootstrapProcessStatus::LaunchFailed, error};
        }
        if (capability.inheritedStdout != 0U) {
            if (capability.inheritedStdout == capability.inheritedStdin) {
                remoteStdout = remoteStdin;
            } else if (!DuplicateHandle(
                           current,
                           reinterpret_cast<HANDLE>(
                               capability.inheritedStdout),
                           brokerProcess_, &remoteStdout, 0U, TRUE,
                           DUPLICATE_SAME_ACCESS)) {
                const DWORD error = GetLastError();
                closeBroker(true);
                return {BootstrapProcessStatus::LaunchFailed, error};
            }
        }

        BrokerLaunchRequest request;
        request.sequence = launchSequence_;
        request.targetJob = reinterpret_cast<std::uintptr_t>(remoteJob);
        request.inheritedStdin =
            reinterpret_cast<std::uintptr_t>(remoteStdin);
        request.inheritedStdout =
            reinterpret_cast<std::uintptr_t>(remoteStdout);
        request.application = std::string(absoluteExecutable);
        request.directory = std::string(workingDirectory);
        DWORD nativeError = ERROR_SUCCESS;
        if (!createBrokerNonce(request.nonce, nativeError)) {
            closeBroker(true);
            return {BootstrapProcessStatus::LaunchFailed, nativeError};
        }
        const std::vector<std::uint8_t> requestPayload =
            encodeBrokerLaunchRequest(request);
        const ULONGLONG deadline = GetTickCount64() + brokerTimeoutMs_;
        BrokerFrameKind responseKind = BrokerFrameKind::LaunchResponse;
        std::vector<std::uint8_t> responsePayload;
        if (requestPayload.empty() ||
            !writeBrokerFrameOverlapped(
                brokerChannel_, BrokerFrameKind::LaunchRequest,
                requestPayload, deadline, nativeError) ||
            !readBrokerFrameOverlapped(
                brokerChannel_, responseKind, responsePayload, deadline,
                nativeError) ||
             responseKind != BrokerFrameKind::LaunchResponse) {
            const BootstrapProcessResult drained =
                terminateAndDrainJobHandles(nullptr, targetJob.get(), 0xE104U);
            closeBroker(true);
            return {BootstrapProcessStatus::LaunchFailed,
                    !drained.ok()
                        ? drained.nativeError
                        : nativeError == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                                       : nativeError};
        }

        DWORD launchStatus = ERROR_SUCCESS;
        std::uintptr_t remoteProcess = 0U;
        std::uintptr_t remoteThread = 0U;
        if (!decodeBrokerLaunchResponse(responsePayload, request, launchStatus,
                                        remoteProcess, remoteThread)) {
            const BootstrapProcessResult drained =
                terminateAndDrainJobHandles(nullptr, targetJob.get(), 0xE104U);
            closeBroker(true);
            return {BootstrapProcessStatus::LaunchFailed,
                    drained.ok() ? ERROR_INVALID_DATA : drained.nativeError};
        }

        ScopedHandle launchedProcess;
        ScopedHandle launchedThread;
        bool accepted = true;
        if (launchStatus == ERROR_SUCCESS) {
            HANDLE localProcess = nullptr;
            HANDLE localThread = nullptr;
            if (!DuplicateHandle(
                    brokerProcess_, reinterpret_cast<HANDLE>(remoteProcess),
                    current, &localProcess, 0U, FALSE,
                    DUPLICATE_SAME_ACCESS) ||
                !DuplicateHandle(
                    brokerProcess_, reinterpret_cast<HANDLE>(remoteThread),
                    current, &localThread, 0U, FALSE,
                    DUPLICATE_SAME_ACCESS)) {
                nativeError = GetLastError();
                if (localProcess != nullptr) CloseHandle(localProcess);
                if (localThread != nullptr) CloseHandle(localThread);
                accepted = false;
            } else {
                launchedProcess.reset(localProcess);
                launchedThread.reset(localThread);
            }
        }

        BootstrapProcessIdentity identity;
        if (accepted && launchStatus == ERROR_SUCCESS &&
            (WaitForSingleObject(launchedProcess.get(), 0U) != WAIT_TIMEOUT ||
             !captureIdentity(launchedProcess.get(), identity) ||
             normalizeFinalPath(utf8ToWide(identity.imagePath)) !=
                 normalizeFinalPath(application))) {
            nativeError = ERROR_INVALID_DATA;
            accepted = false;
        }
        if (accepted && launchStatus == ERROR_SUCCESS) {
            ScopedHandle recoveryJob;
            const BootstrapProcessResult bound = bindRecoveryJob(
                launchedProcess.get(), targetJob, identity, recoveryJob);
            if (!bound.ok()) {
                nativeError = bound.nativeError;
                accepted = false;
            } else {
                targetJob = std::move(recoveryJob);
            }
        }
        const std::vector<std::uint8_t> acknowledgement =
            encodeBrokerAcknowledgement(request, accepted);
        if (!writeBrokerFrameOverlapped(
                brokerChannel_, BrokerFrameKind::LaunchAcknowledgement,
                acknowledgement, deadline, nativeError)) {
            accepted = false;
            closeBroker(true);
        }
        if (!accepted) {
            const BootstrapProcessResult drained = terminateAndDrainJobHandles(
                launchedProcess.get(), targetJob.get(), 0xE104U);
            return {BootstrapProcessStatus::LaunchFailed,
                    !drained.ok()
                        ? drained.nativeError
                        : nativeError == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                                       : nativeError};
        }
        if (launchStatus != ERROR_SUCCESS) {
            return {BootstrapProcessStatus::LaunchFailed, launchStatus};
        }
        return retain(launchedProcess.release(), targetJob.release(),
                      launchedThread.release(), role, true, identity,
                      imagePin.release(), pinnedImagePath);
    }

    BootstrapProcessResult launch(
        const std::string_view absoluteExecutable,
        const std::string_view workingDirectory,
        const BootstrapLaunchCapability& capability,
        const bool suspended, const NativeProcessRole role) {
        if (suspended && brokerChannel_ != nullptr) {
            return launchBrokered(absoluteExecutable, workingDirectory,
                                  capability, role);
        }
        const std::wstring application = utf8ToWide(absoluteExecutable);
        const std::wstring directory = utf8ToWide(workingDirectory);
        if (application.empty() || directory.empty()) {
            return {BootstrapProcessStatus::LaunchFailed, ERROR_INVALID_PARAMETER};
        }
        ScopedHandle imagePin;
        std::wstring pinnedImagePath;
        if (suspended) {
            imagePin.reset(CreateFileW(
                application.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr));
            if (imagePin.get() == INVALID_HANDLE_VALUE) {
                return {BootstrapProcessStatus::LaunchFailed, GetLastError()};
            }
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            pinnedImagePath = finalPath(imagePin.get());
            if (GetFileInformationByHandleEx(
                    imagePin.get(), FileAttributeTagInfo, &attributes,
                    sizeof(attributes)) == FALSE ||
                (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
                pinnedImagePath.empty() ||
                normalizeFinalPath(pinnedImagePath) !=
                    normalizeFinalPath(application)) {
                const DWORD error = GetLastError() == ERROR_SUCCESS
                    ? ERROR_INVALID_DATA
                    : GetLastError();
                return {BootstrapProcessStatus::LaunchFailed, error};
            }
        }
        std::wstring command = L"\"" + application + L"\"";
        std::vector<wchar_t> commandLine(command.begin(), command.end());
        commandLine.push_back(L'\0');

        std::array<HANDLE, 2> inherited{};
        DWORD inheritedCount = 0U;
        if (capability.inheritedStdin != 0U) {
            inherited[inheritedCount++] =
                reinterpret_cast<HANDLE>(capability.inheritedStdin);
        }
        if (capability.inheritedStdout != 0U &&
            capability.inheritedStdout != capability.inheritedStdin) {
            inherited[inheritedCount++] =
                reinterpret_cast<HANDLE>(capability.inheritedStdout);
        }

        ScopedHandle job;
        if (suspended) {
            job.reset(CreateJobObjectW(nullptr, nullptr));
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (job.get() == nullptr ||
                !SetInformationJobObject(
                    job.get(), JobObjectExtendedLimitInformation, &limits,
                    sizeof(limits))) {
                return {BootstrapProcessStatus::LaunchFailed, GetLastError()};
            }
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        std::vector<std::uint8_t> attributeStorage;
        ScopedProcThreadAttributeList attributeList;
        const DWORD attributeCount =
            (inheritedCount != 0U ? 1U : 0U) + (suspended ? 1U : 0U);
        if (attributeCount != 0U) {
            SIZE_T bytes = 0U;
            static_cast<void>(InitializeProcThreadAttributeList(
                nullptr, attributeCount, 0U, &bytes));
            if (bytes == 0U) return {BootstrapProcessStatus::LaunchFailed, GetLastError()};
            attributeStorage.resize(bytes);
            if (!attributeList.initialize(attributeStorage.data(), bytes,
                                          attributeCount)) {
                const DWORD error = GetLastError();
                return {BootstrapProcessStatus::LaunchFailed, error};
            }
            startup.lpAttributeList = attributeList.get();
        }
        if (inheritedCount != 0U) {
            if (!UpdateProcThreadAttribute(
                    attributeList.get(), 0U,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                    sizeof(HANDLE) * inheritedCount, nullptr, nullptr)) {
                return {BootstrapProcessStatus::LaunchFailed, GetLastError()};
            }
            startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput =
                capability.inheritedStdin != 0U
                    ? reinterpret_cast<HANDLE>(capability.inheritedStdin)
                    : reinterpret_cast<HANDLE>(capability.inheritedStdout);
            startup.StartupInfo.hStdOutput =
                capability.inheritedStdout != 0U
                    ? reinterpret_cast<HANDLE>(capability.inheritedStdout)
                    : reinterpret_cast<HANDLE>(capability.inheritedStdin);
            startup.StartupInfo.hStdError = startup.StartupInfo.hStdOutput;
        }
        if (suspended) {
            HANDLE jobHandle = job.get();
            if (!UpdateProcThreadAttribute(
                    attributeList.get(), 0U, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                    &jobHandle, sizeof(jobHandle), nullptr, nullptr)) {
                return {BootstrapProcessStatus::LaunchFailed, GetLastError()};
            }
        }

#ifdef ARIS_BOOTSTRAP_TEST_FAIL_CHILD_LAUNCH
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        const char* const injectedLaunchFailure =
            std::getenv("ARIS_BOOTSTRAP_TEST_FAIL_CHILD_LAUNCH");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        if (suspended && capability.valid() && injectedLaunchFailure != nullptr &&
            std::string_view(injectedLaunchFailure) == "1") {
            return {BootstrapProcessStatus::LaunchFailed, ERROR_BAD_EXE_FORMAT};
        }
#endif

        PROCESS_INFORMATION processInfo{};
        // EXTENDED_STARTUPINFO_PRESENT is valid only when an attribute list
        // was initialized.  Passing it for ordinary launches makes
        // CreateProcessW fail with ERROR_INVALID_PARAMETER and would turn
        // rollback/relaunch into a false launch failure.
        DWORD flags = CREATE_UNICODE_ENVIRONMENT;
        if (startup.lpAttributeList != nullptr) {
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        }
        if (suspended) flags |= CREATE_SUSPENDED | CREATE_BREAKAWAY_FROM_JOB;
        const BOOL launched = CreateProcessW(
            application.c_str(), commandLine.data(), nullptr, nullptr,
            inheritedCount != 0U, flags, nullptr, directory.c_str(),
            &startup.StartupInfo, &processInfo);
        if (!launched) {
            const DWORD error = GetLastError();
            return {BootstrapProcessStatus::LaunchFailed, error};
        }
        ScopedHandle launchedProcess(processInfo.hProcess);
        ScopedHandle launchedThread(processInfo.hThread);

#ifdef ARIS_BOOTSTRAP_TEST_PROCESS_CUTS
        std::array<wchar_t, 2U> crashCut{};
        if (suspended &&
            GetEnvironmentVariableW(
                L"ARIS_BOOTSTRAP_TEST_FAIL_AFTER_CHILD_CREATE",
                crashCut.data(), static_cast<DWORD>(crashCut.size())) == 1U &&
            crashCut[0] == L'1') {
            job.reset();
            const DWORD terminated =
                WaitForSingleObject(launchedProcess.get(), 5000U);
            return {BootstrapProcessStatus::LaunchFailed,
                    static_cast<std::uint32_t>(
                        terminated == WAIT_OBJECT_0 ? ERROR_CANCELLED
                                                   : ERROR_TIMEOUT)};
        }
#endif

        BootstrapProcessIdentity identity;
        if (!captureIdentity(launchedProcess.get(), identity)) {
            job.reset();
            static_cast<void>(WaitForSingleObject(launchedProcess.get(), 5000U));
            return {BootstrapProcessStatus::LaunchFailed, ERROR_INVALID_DATA};
        }
        if (suspended) {
            ScopedHandle recoveryJob;
            const BootstrapProcessResult bound = bindRecoveryJob(
                launchedProcess.get(), job, identity, recoveryJob);
            if (!bound.ok()) {
                job.reset();
                static_cast<void>(
                    WaitForSingleObject(launchedProcess.get(), 5000U));
                return bound;
            }
            job = std::move(recoveryJob);
        }
        const BootstrapProcessResult result = retain(
            launchedProcess.release(), job.release(),
            suspended ? launchedThread.release() : nullptr, role, suspended,
            identity, imagePin.release(), std::move(pinnedImagePath));
        return result;
    }

    std::map<std::uintptr_t, NativeProcessRecord> handles_;
    std::uintptr_t nextToken_ = 1U;
    HANDLE brokerChannel_ = nullptr;
    HANDLE brokerProcess_ = nullptr;
    HANDLE brokerJob_ = nullptr;
    BootstrapProcessIdentity brokerIdentity_;
    std::uint32_t brokerTimeoutMs_ = 0U;
    std::uint32_t launchSequence_ = 0U;
};

class NativeHealthPort final : public BootstrapHealthPort {
public:
    ~NativeHealthPort() override { Close(); }

    BootstrapHealthResult Prepare(std::string_view nonce,
                                  std::string_view version,
                                  std::string_view buildId,
                                  BootstrapLaunchCapability& capability) override {
        if (prepared_ || nonce.size() != 64U || version.empty() || buildId.empty()) {
            return {BootstrapHealthStatus::ChannelRejected};
        }
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        HANDLE parentRead = INVALID_HANDLE_VALUE;
        HANDLE childWrite = INVALID_HANDLE_VALUE;
        HANDLE childRead = INVALID_HANDLE_VALUE;
        HANDLE parentWrite = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&parentRead, &childWrite, &attributes, 0U) ||
            !CreatePipe(&childRead, &parentWrite, &attributes, 0U) ||
            !SetHandleInformation(parentRead, HANDLE_FLAG_INHERIT, 0U) ||
            !SetHandleInformation(parentWrite, HANDLE_FLAG_INHERIT, 0U)) {
            if (parentRead != INVALID_HANDLE_VALUE) CloseHandle(parentRead);
            if (childWrite != INVALID_HANDLE_VALUE) CloseHandle(childWrite);
            if (childRead != INVALID_HANDLE_VALUE) CloseHandle(childRead);
            if (parentWrite != INVALID_HANDLE_VALUE) CloseHandle(parentWrite);
            return {BootstrapHealthStatus::ChannelRejected, GetLastError()};
        }
        parentRead_ = parentRead;
        parentWrite_ = parentWrite;
        childRead_ = childRead;
        childWrite_ = childWrite;
        nonce_ = std::string(nonce);
        version_ = std::string(version);
        buildId_ = std::string(buildId);
        prepared_ = true;
        consumed_ = false;
        capability.inheritedStdin = reinterpret_cast<std::uintptr_t>(childRead_);
        capability.inheritedStdout = reinterpret_cast<std::uintptr_t>(childWrite_);
        return {BootstrapHealthStatus::Accepted};
    }

    void ReleaseLaunchCapability() noexcept override {
        if (childRead_ != INVALID_HANDLE_VALUE) CloseHandle(childRead_);
        if (childWrite_ != INVALID_HANDLE_VALUE) CloseHandle(childWrite_);
        childRead_ = INVALID_HANDLE_VALUE;
        childWrite_ = INVALID_HANDLE_VALUE;
    }

    BootstrapHealthResult Await(const BootstrapOwnedProcess& child,
                                std::string_view nonce,
                                std::string_view version,
                                std::string_view buildId,
                                const std::uint32_t timeoutMs) override {
        if (consumed_) return {BootstrapHealthStatus::ReplayRejected};
        if (!prepared_ || parentRead_ == INVALID_HANDLE_VALUE ||
            parentWrite_ == INVALID_HANDLE_VALUE || nonce != nonce_ ||
            version != version_ || buildId != buildId_) {
            return {BootstrapHealthStatus::ChannelRejected};
        }
        if (!child.valid() || child.waitToken == 0U) {
            return {BootstrapHealthStatus::ChannelRejected, ERROR_INVALID_HANDLE};
        }
        const HANDLE childHandle = reinterpret_cast<HANDLE>(child.waitToken);
        if (childHandle == nullptr || childHandle == INVALID_HANDLE_VALUE) {
            return {BootstrapHealthStatus::ChannelRejected, ERROR_INVALID_HANDLE};
        }
        BootstrapProcessIdentity observedChild;
        if (!captureIdentity(childHandle, observedChild)) {
            const DWORD wait = WaitForSingleObject(childHandle, 0U);
            if (wait == WAIT_OBJECT_0) {
                return {BootstrapHealthStatus::ChildExited};
            }
            return {BootstrapHealthStatus::ChannelRejected, ERROR_INVALID_DATA};
        }
        if (!sameIdentity(child.identity, observedChild)) {
            return {BootstrapHealthStatus::ChannelRejected, ERROR_INVALID_DATA};
        }
        const json challenge = {{"build_id", std::string(buildId)},
                                {"token", std::string(nonce)},
                                {"version", std::string(version)}};
        const std::string challengeBytes = challenge.dump();
        DWORD written = 0U;
        if (!WriteFile(parentWrite_, challengeBytes.data(),
                       static_cast<DWORD>(challengeBytes.size()), &written, nullptr) ||
            written != challengeBytes.size()) {
            Close();
            return {BootstrapHealthStatus::ChannelRejected, GetLastError()};
        }
        CloseHandle(parentWrite_);
        parentWrite_ = INVALID_HANDLE_VALUE;

        const ULONGLONG start = GetTickCount64();
        std::string message;
        message.reserve(4096U);
        for (;;) {
            DWORD available = 0U;
            if (!PeekNamedPipe(parentRead_, nullptr, 0U, nullptr, &available, nullptr)) {
                const DWORD error = GetLastError();
                Close();
                return {error == ERROR_BROKEN_PIPE ? BootstrapHealthStatus::ChildExited
                                                   : BootstrapHealthStatus::ChannelRejected,
                        error};
            }
            if (available != 0U) {
                if (available > 4096U - message.size()) {
                    Close();
                    return {BootstrapHealthStatus::Malformed};
                }
                const std::size_t offset = message.size();
                message.resize(offset + available, '\0');
                DWORD read = 0U;
                if (!ReadFile(parentRead_, message.data() + offset, available,
                              &read, nullptr) ||
                    read != available) {
                    Close();
                    return {BootstrapHealthStatus::Malformed, GetLastError()};
                }
                const std::size_t delimiter = message.find('\n');
                if (delimiter == std::string::npos) continue;
                const bool trailingIsWhitespace = std::all_of(
                    message.begin() + static_cast<std::ptrdiff_t>(delimiter + 1U),
                    message.end(), [](const char character) {
                        return character == ' ' || character == '\t' ||
                               character == '\r' || character == '\n';
                    });
                if (delimiter == 0U || !trailingIsWhitespace) {
                    Close();
                    return {BootstrapHealthStatus::Malformed};
                }
                const BootstrapHealthResult parsed = parse(
                    message.substr(0U, delimiter), child.identity, nonce_,
                    version_, buildId_);
                if (parsed.ok()) {
                    // Keep the parent read end open until the composition
                    // publishes commit and calls Close(). A child-side
                    // heartbeat can therefore observe the exact ownership
                    // handoff; all failure paths still call Close() below.
                    consumed_ = true;
                    return parsed;
                }
                Close();
                return parsed;
            }
            if (child.waitToken != 0U &&
                WaitForSingleObject(reinterpret_cast<HANDLE>(child.waitToken), 0U) ==
                    WAIT_OBJECT_0) {
                Close();
                return {BootstrapHealthStatus::ChildExited};
            }
            if (GetTickCount64() - start >= timeoutMs) {
                Close();
                return {BootstrapHealthStatus::TimedOut};
            }
            Sleep(5U);
        }
    }

    void Close() noexcept override {
        if (parentRead_ != INVALID_HANDLE_VALUE) CloseHandle(parentRead_);
        if (parentWrite_ != INVALID_HANDLE_VALUE) CloseHandle(parentWrite_);
        if (childRead_ != INVALID_HANDLE_VALUE) CloseHandle(childRead_);
        if (childWrite_ != INVALID_HANDLE_VALUE) CloseHandle(childWrite_);
        parentRead_ = INVALID_HANDLE_VALUE;
        parentWrite_ = INVALID_HANDLE_VALUE;
        childRead_ = INVALID_HANDLE_VALUE;
        childWrite_ = INVALID_HANDLE_VALUE;
        prepared_ = false;
        nonce_.clear();
        version_.clear();
        buildId_.clear();
    }

private:
    static BootstrapHealthResult parse(
        const std::string& message, const BootstrapProcessIdentity& child,
        const std::string_view expectedNonce,
        const std::string_view expectedVersion,
        const std::string_view expectedBuildId) {
        const json value = json::parse(message, nullptr, false);
        if (value.is_discarded() || !value.is_object() || value.size() != 5U ||
            !value.contains("build_id") || !value.contains("process_id") ||
            !value.contains("token") || !value.contains("ui_ready") ||
            !value.contains("version") || !value["build_id"].is_string() ||
            !value["process_id"].is_number_unsigned() ||
            !value["token"].is_string() || !value["ui_ready"].is_boolean() ||
            !value["version"].is_string()) {
            return {BootstrapHealthStatus::Malformed};
        }
        HealthAcknowledgement acknowledgement;
        acknowledgement.buildId = value["build_id"].get<std::string>();
        acknowledgement.processId = value["process_id"].get<std::uint64_t>();
        acknowledgement.token = value["token"].get<std::string>();
        acknowledgement.uiReady = value["ui_ready"].get<bool>();
        acknowledgement.version = value["version"].get<std::string>();
        if (acknowledgement.processId != child.processId ||
            acknowledgement.token != expectedNonce ||
            acknowledgement.version != expectedVersion ||
            acknowledgement.buildId != expectedBuildId ||
            !acknowledgement.uiReady) {
            return {BootstrapHealthStatus::ChannelRejected};
        }
        return {BootstrapHealthStatus::Accepted, 0, acknowledgement};
    }

    HANDLE parentRead_ = INVALID_HANDLE_VALUE;
    HANDLE parentWrite_ = INVALID_HANDLE_VALUE;
    HANDLE childRead_ = INVALID_HANDLE_VALUE;
    HANDLE childWrite_ = INVALID_HANDLE_VALUE;
    std::string nonce_;
    std::string version_;
    std::string buildId_;
    bool prepared_ = false;
    bool consumed_ = false;
};

std::string fileIdHex(const std::array<std::uint8_t, 16U>& fileId) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(fileId.size() * 2U);
    for (const std::uint8_t byte : fileId) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

// A terminal cleanup successor must survive the bootstrap process that
// launched it.  CREATE_BREAKAWAY_FROM_JOB is valid only when the current job
// grants breakaway; in a non-kill-on-close containment job it is both
// unnecessary and rejected with ERROR_ACCESS_DENIED.  Keep the production
// breakaway path for ordinary launches, but adapt to an inherited harness/job
// boundary without weakening any filesystem or package authority checks.
DWORD TerminalCleanupCreateFlags() noexcept {
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
        EXTENDED_STARTUPINFO_PRESENT;
    BOOL inJob = FALSE;
    if (IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) == FALSE ||
        inJob == FALSE) {
        return flags | CREATE_BREAKAWAY_FROM_JOB;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (QueryInformationJobObject(
            nullptr, JobObjectExtendedLimitInformation, &limits,
            sizeof(limits), nullptr) == FALSE) {
        // Preserve the historical fail-closed behavior when the job cannot be
        // inspected; the caller will surface the native CreateProcess error.
        return flags | CREATE_BREAKAWAY_FROM_JOB;
    }

    const DWORD limitFlags = limits.BasicLimitInformation.LimitFlags;
    if ((limitFlags & JOB_OBJECT_LIMIT_BREAKAWAY_OK) != 0U ||
        (limitFlags & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK) != 0U) {
        return flags | CREATE_BREAKAWAY_FROM_JOB;
    }

    // A kill-on-close job without breakaway cannot safely host a terminal
    // successor: closing the bootstrap's job would kill the child before it
    // can consume the authenticated cleanup handoff.  Returning the ordinary
    // flags lets CreateProcess fail closed rather than pretending cleanup was
    // committed.
    if ((limitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0U) {
        return 0U;
    }
    return flags;
}

json terminalProcessIdentityJson(const BootstrapProcessIdentity& identity) {
    return {{"creation_time", identity.creationTime},
            {"file_id", fileIdHex(identity.imageFileId)},
            {"image_path", identity.imagePath},
            {"image_volume_serial", identity.imageVolumeSerial},
            {"process_id", identity.processId}};
}

json terminalDeploymentIdentityJson(
    const aris::platform::DeploymentFileIdentity& identity) {
    return {{"canonical_path", identity.canonicalPath},
            {"file_id", fileIdHex(identity.fileId)},
            {"volume_serial", identity.volumeSerial}};
}

std::string encodeTerminalCleanupHandoff(
    const ValidatedInstallPlan& plan,
    const ReplacementJournal& journal) {
    json existingInventory = json::array();
    for (const ReplacementJournalItem& item : journal.items) {
        if (item.classification != JournalItemClass::CreateNew) {
            existingInventory.push_back({{"relative_path", item.relativePath}});
        }
    }
    json protectedRoots = json::array();
    for (const std::string& root : plan.identity.protectedDataRoots) {
        protectedRoots.push_back(root);
    }
    const json invocation = {
        {"deployment_mode", plan.identity.deploymentMode ==
                aris::platform::DeploymentMode::Installed
            ? "installed"
            : "portable"},
        {"entrypoint", plan.entrypoint},
        {"existing_inventory", std::move(existingInventory)},
        {"health_nonce", plan.healthToken},
        {"installation_id", plan.identity.installationId},
        {"invocation_identity", plan.invocation.invocationIdentity},
        {"old_process", terminalProcessIdentityJson(
             plan.invocation.oldProcess)},
        {"old_executable_identity", terminalDeploymentIdentityJson(
             plan.identity.oldExecutableIdentity)},
        {"old_process_id", plan.oldProcessId},
        {"package_plan_digest", plan.packagePlanDigest},
        {"parent_process", terminalProcessIdentityJson(
             plan.invocation.parentProcess)},
        {"previous_failure_count", plan.previousFailureCount},
        {"protected_data_roots", std::move(protectedRoots)},
        {"registered_app_id", plan.identity.registeredAppId},
        {"staging_root", plan.invocation.stagingRoot},
        {"target_root", plan.identity.targetRoot},
        {"target_root_identity", terminalDeploymentIdentityJson(
             plan.identity.targetRootIdentity)},
        {"transaction_id", plan.invocation.transactionId},
        {"transaction_root", plan.invocation.transactionRoot},
        {"updater_owned_base", plan.invocation.updaterOwnedBase}};
    const json handoff = {
        {"invocation", invocation.dump()},
        {"package_plan", plan.packagePlanBytes},
        {"package_signature", plan.packageSignatureHex},
        {"transaction_id", plan.invocation.transactionId}};
    return handoff.dump();
}

constexpr std::array<std::uint8_t, 8U> kTerminalCleanupFrameMagic{
    'A', 'R', 'I', 'S', 'T', 'C', 'L', '1'};
constexpr std::uint32_t kTerminalCleanupProtocolVersion = 1U;
constexpr std::size_t kTerminalCleanupHeaderBytes = 16U;
constexpr std::size_t kMaximumTerminalCleanupPayloadBytes = 1100U * 1024U;

std::vector<std::uint8_t> encodeTerminalCleanupFrame(
    const std::string_view payload) {
    if (payload.empty() ||
        payload.size() > kMaximumTerminalCleanupPayloadBytes ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }
    std::vector<std::uint8_t> frame;
    frame.reserve(kTerminalCleanupHeaderBytes + payload.size());
    frame.insert(frame.end(), kTerminalCleanupFrameMagic.begin(),
                 kTerminalCleanupFrameMagic.end());
    appendBrokerU32(frame, kTerminalCleanupProtocolVersion);
    appendBrokerU32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

enum class TerminalCleanupHeaderStatus {
    Accepted,
    TooLarge,
    Malformed,
};

TerminalCleanupHeaderStatus decodeTerminalCleanupHeader(
    const std::array<std::uint8_t, kTerminalCleanupHeaderBytes>& header,
    std::uint32_t& payloadBytes) {
    if (!std::equal(kTerminalCleanupFrameMagic.begin(),
                    kTerminalCleanupFrameMagic.end(), header.begin())) {
        return TerminalCleanupHeaderStatus::Malformed;
    }
    const std::vector<std::uint8_t> bytes(header.begin(), header.end());
    std::size_t offset = kTerminalCleanupFrameMagic.size();
    std::uint32_t version = 0U;
    if (!consumeBrokerU32(bytes, offset, version) ||
        !consumeBrokerU32(bytes, offset, payloadBytes) ||
        offset != bytes.size() || version != kTerminalCleanupProtocolVersion ||
        payloadBytes == 0U) {
        return TerminalCleanupHeaderStatus::Malformed;
    }
    if (payloadBytes > kMaximumTerminalCleanupPayloadBytes) {
        return TerminalCleanupHeaderStatus::TooLarge;
    }
    return TerminalCleanupHeaderStatus::Accepted;
}

enum class TerminalCleanupFrameReadStatus {
    Accepted,
    TooLarge,
    Malformed,
    TimedOut,
    ReadFailed,
};

struct TerminalCleanupFrameReadResult {
    TerminalCleanupFrameReadStatus status =
        TerminalCleanupFrameReadStatus::ReadFailed;
    DWORD nativeError = ERROR_SUCCESS;
    std::string payload;
};

bool terminalPipeReachedEnd(const DWORD error) {
    return error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
        error == ERROR_PIPE_NOT_CONNECTED;
}

TerminalCleanupFrameReadResult readTerminalCleanupFrame(
    const HANDLE pipe, const std::uint32_t timeoutMs) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || timeoutMs == 0U) {
        return {TerminalCleanupFrameReadStatus::ReadFailed,
                ERROR_INVALID_HANDLE};
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    std::array<std::uint8_t, kTerminalCleanupHeaderBytes> header{};
    DWORD nativeError = ERROR_SUCCESS;
    if (!transferBrokerOverlapped(pipe, header.data(), header.size(), false,
                                  deadline, nativeError)) {
        return {nativeError == ERROR_TIMEOUT
                    ? TerminalCleanupFrameReadStatus::TimedOut
                    : (terminalPipeReachedEnd(nativeError)
                           ? TerminalCleanupFrameReadStatus::Malformed
                           : TerminalCleanupFrameReadStatus::ReadFailed),
                nativeError};
    }
    std::uint32_t payloadBytes = 0U;
    const TerminalCleanupHeaderStatus headerStatus =
        decodeTerminalCleanupHeader(header, payloadBytes);
    if (headerStatus == TerminalCleanupHeaderStatus::TooLarge) {
        return {TerminalCleanupFrameReadStatus::TooLarge,
                ERROR_BUFFER_OVERFLOW};
    }
    if (headerStatus != TerminalCleanupHeaderStatus::Accepted) {
        return {TerminalCleanupFrameReadStatus::Malformed,
                ERROR_INVALID_DATA};
    }
    std::string payload(payloadBytes, '\0');
    if (!transferBrokerOverlapped(pipe, payload.data(), payload.size(), false,
                                  deadline, nativeError)) {
        return {nativeError == ERROR_TIMEOUT
                    ? TerminalCleanupFrameReadStatus::TimedOut
                    : (terminalPipeReachedEnd(nativeError)
                           ? TerminalCleanupFrameReadStatus::Malformed
                           : TerminalCleanupFrameReadStatus::ReadFailed),
                nativeError};
    }
    for (;;) {
        DWORD available = 0U;
        if (!PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr)) {
            nativeError = GetLastError();
            if (terminalPipeReachedEnd(nativeError)) {
                return {TerminalCleanupFrameReadStatus::Accepted,
                        ERROR_SUCCESS, std::move(payload)};
            }
            return {TerminalCleanupFrameReadStatus::ReadFailed, nativeError};
        }
        if (available != 0U) {
            return {TerminalCleanupFrameReadStatus::Malformed,
                    ERROR_INVALID_DATA};
        }
        const DWORD remaining = remainingBrokerTime(deadline);
        if (remaining == 0U) {
            return {TerminalCleanupFrameReadStatus::TimedOut, ERROR_TIMEOUT};
        }
        Sleep((std::min)(remaining, static_cast<DWORD>(5U)));
    }
}

TerminalCleanupFrameReadResult readTerminalReadyFrame(
    const HANDLE pipe, const std::uint32_t timeoutMs) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || timeoutMs == 0U) {
        return {TerminalCleanupFrameReadStatus::ReadFailed,
                ERROR_INVALID_HANDLE};
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kTerminalCleanupHeaderBytes + 256U);
    std::size_t expectedBytes = 0U;
    for (;;) {
        DWORD available = 0U;
        if (!PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (terminalPipeReachedEnd(error) &&
                expectedBytes != 0U && bytes.size() == expectedBytes) {
                std::string payload(
                    reinterpret_cast<const char*>(
                        bytes.data() + kTerminalCleanupHeaderBytes),
                    expectedBytes - kTerminalCleanupHeaderBytes);
                return {TerminalCleanupFrameReadStatus::Accepted,
                        ERROR_SUCCESS, std::move(payload)};
            }
            return {terminalPipeReachedEnd(error)
                        ? TerminalCleanupFrameReadStatus::Malformed
                        : TerminalCleanupFrameReadStatus::ReadFailed,
                    error};
        }
        if (available != 0U) {
            if (bytes.size() + available >
                kMaximumTerminalCleanupPayloadBytes +
                    kTerminalCleanupHeaderBytes) {
                return {TerminalCleanupFrameReadStatus::TooLarge,
                        ERROR_BUFFER_OVERFLOW};
            }
            std::vector<std::uint8_t> chunk(available);
            DWORD read = 0U;
            if (!ReadFile(pipe, chunk.data(), available, &read, nullptr) ||
                read != available) {
                const DWORD error = GetLastError();
                return {terminalPipeReachedEnd(error)
                            ? TerminalCleanupFrameReadStatus::Malformed
                            : TerminalCleanupFrameReadStatus::ReadFailed,
                        error};
            }
            bytes.insert(bytes.end(), chunk.begin(), chunk.end());
            if (bytes.size() >= kTerminalCleanupHeaderBytes &&
                expectedBytes == 0U) {
                std::array<std::uint8_t, kTerminalCleanupHeaderBytes> header{};
                std::copy_n(bytes.begin(), kTerminalCleanupHeaderBytes,
                            header.begin());
                std::uint32_t payloadBytes = 0U;
                const TerminalCleanupHeaderStatus headerStatus =
                    decodeTerminalCleanupHeader(header, payloadBytes);
                if (headerStatus == TerminalCleanupHeaderStatus::TooLarge) {
                    return {TerminalCleanupFrameReadStatus::TooLarge,
                            ERROR_BUFFER_OVERFLOW};
                }
                if (headerStatus != TerminalCleanupHeaderStatus::Accepted) {
                    return {TerminalCleanupFrameReadStatus::Malformed,
                            ERROR_INVALID_DATA};
                }
                expectedBytes = kTerminalCleanupHeaderBytes + payloadBytes;
            }
            if (expectedBytes != 0U && bytes.size() > expectedBytes) {
                return {TerminalCleanupFrameReadStatus::Malformed,
                        ERROR_INVALID_DATA};
            }
        }
        const DWORD remaining = remainingBrokerTime(deadline);
        if (remaining == 0U) {
            return {TerminalCleanupFrameReadStatus::TimedOut, ERROR_TIMEOUT};
        }
        Sleep((std::min)(remaining, static_cast<DWORD>(5U)));
    }
}

bool connectTerminalCleanupPipe(const HANDLE pipe, const ULONGLONG deadline,
                                DWORD& nativeError) {
    auto operation = std::make_unique<BrokerOverlappedOperation>();
    if (operation->event == nullptr) {
        nativeError = GetLastError();
        return false;
    }
    if (ConnectNamedPipe(pipe, &operation->overlapped) != FALSE) return true;
    const DWORD pendingError = GetLastError();
    if (pendingError == ERROR_PIPE_CONNECTED) return true;
    if (pendingError != ERROR_IO_PENDING) {
        nativeError = pendingError;
        return false;
    }
    const DWORD timeout = remainingBrokerTime(deadline);
    if (timeout == 0U) {
        static_cast<void>(CancelIoEx(pipe, &operation->overlapped));
        nativeError = ERROR_TIMEOUT;
        return false;
    }
    const DWORD wait = WaitForSingleObject(operation->event, timeout);
    if (wait != WAIT_OBJECT_0) {
        static_cast<void>(CancelIoEx(pipe, &operation->overlapped));
        const DWORD cancelled = WaitForSingleObject(operation->event, 1000U);
        if (cancelled != WAIT_OBJECT_0) {
            static_cast<void>(operation.release());
        }
        nativeError = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        return false;
    }
    DWORD transferred = 0U;
    if (GetOverlappedResult(pipe, &operation->overlapped, &transferred,
                            FALSE) == FALSE) {
        nativeError = GetLastError();
        return nativeError == ERROR_PIPE_CONNECTED;
    }
    return true;
}

class CurrentUserPipeSecurity final {
public:
    CurrentUserPipeSecurity() {
        if (!queryTokenUser(GetCurrentProcess(), tokenUser_)) return;
        const auto* user =
            reinterpret_cast<const TOKEN_USER*>(tokenUser_.data());
        if (user == nullptr || IsValidSid(user->User.Sid) == FALSE) return;
        const DWORD sidBytes = GetLengthSid(user->User.Sid);
        const DWORD aclBytes = static_cast<DWORD>(
            sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) +
            sidBytes);
        acl_.assign(aclBytes, 0U);
        auto* const acl = reinterpret_cast<PACL>(acl_.data());
        if (InitializeAcl(acl, aclBytes, ACL_REVISION) == FALSE ||
            AddAccessAllowedAceEx(acl, ACL_REVISION, 0U, GENERIC_ALL,
                                  user->User.Sid) == FALSE ||
            InitializeSecurityDescriptor(
                &descriptor_, SECURITY_DESCRIPTOR_REVISION) == FALSE ||
            SetSecurityDescriptorDacl(&descriptor_, TRUE, acl, FALSE) ==
                FALSE) {
            return;
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = TRUE;
        valid_ = true;
    }

    bool valid() const noexcept { return valid_; }
    SECURITY_ATTRIBUTES* attributes() noexcept {
        return valid_ ? &attributes_ : nullptr;
    }

private:
    std::vector<std::uint8_t> tokenUser_;
    std::vector<std::uint8_t> acl_;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
    bool valid_ = false;
};

#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
void writeLocalDemoSuccessorLaunchDiagnostic(
    const char* stage,
    const DWORD nativeError,
    const DWORD processId = 0U) noexcept {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-successor-launch.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    std::fprintf(diagnostic, "stage=%s native=%lu pid=%lu\n", stage,
                 static_cast<unsigned long>(nativeError),
                 static_cast<unsigned long>(processId));
    std::fclose(diagnostic);
}

void writeLocalDemoSuccessorExitDiagnostic(
    const HANDLE successor, const char* context) noexcept {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-successor-exit.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    const DWORD waitResult =
        successor == nullptr || successor == INVALID_HANDLE_VALUE
            ? WAIT_FAILED
            : WaitForSingleObject(successor, 0U);
    DWORD exitCode = STILL_ACTIVE;
    DWORD exitError = ERROR_SUCCESS;
    if (successor == nullptr || successor == INVALID_HANDLE_VALUE ||
        !GetExitCodeProcess(successor, &exitCode)) {
        exitError = GetLastError();
    }
    std::fprintf(diagnostic,
                 "context=%s wait=%lu exit_code=%lu exit_error=%lu\n",
                 context == nullptr ? "unknown" : context,
                 static_cast<unsigned long>(waitResult),
                 static_cast<unsigned long>(exitCode),
                 static_cast<unsigned long>(exitError));
    std::fclose(diagnostic);
}

struct LocalDemoSuccessorHandleDiagnostic final {
    DWORD fileType = FILE_TYPE_UNKNOWN;
    DWORD fileTypeError = ERROR_SUCCESS;
    DWORD inheritFlags = 0U;
    DWORD inheritFlagsError = ERROR_SUCCESS;
    DWORD processId = 0U;
    DWORD processIdError = ERROR_SUCCESS;
};

LocalDemoSuccessorHandleDiagnostic inspectLocalDemoSuccessorHandle(
    const HANDLE handle) noexcept {
    LocalDemoSuccessorHandleDiagnostic result{};
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        result.fileTypeError = ERROR_INVALID_HANDLE;
        result.inheritFlagsError = ERROR_INVALID_HANDLE;
        result.processIdError = ERROR_INVALID_HANDLE;
        return result;
    }
    SetLastError(ERROR_SUCCESS);
    result.fileType = GetFileType(handle);
    result.fileTypeError = GetLastError();
    SetLastError(ERROR_SUCCESS);
    if (!GetHandleInformation(handle, &result.inheritFlags)) {
        result.inheritFlagsError = GetLastError();
    }
    SetLastError(ERROR_SUCCESS);
    result.processId = GetProcessId(handle);
    result.processIdError = GetLastError();
    return result;
}

void writeLocalDemoSuccessorCreateContextDiagnostic(
    const char* stage,
    const DWORD createFlags,
    const std::wstring& application,
    const std::wstring& directory,
    const HANDLE client,
    const HANDLE parent,
    const HANDLE ready,
    const SIZE_T attributeBytes,
    const bool attributeListInitialized,
    const bool handleListUpdated,
    const DWORD attributeProbeError,
    const DWORD attributeUpdateError,
    const DWORD nativeError,
    const DWORD firstCreateProcessError) noexcept {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-successor-create-context.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    BOOL inJob = FALSE;
    DWORD inJobError = ERROR_SUCCESS;
    SetLastError(ERROR_SUCCESS);
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob)) {
        inJobError = GetLastError();
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    DWORD jobQueryError = ERROR_SUCCESS;
    SetLastError(ERROR_SUCCESS);
    if (!QueryInformationJobObject(
            nullptr, JobObjectExtendedLimitInformation, &limits,
            sizeof(limits), nullptr)) {
        jobQueryError = GetLastError();
    }
    const auto clientInfo = inspectLocalDemoSuccessorHandle(client);
    const auto parentInfo = inspectLocalDemoSuccessorHandle(parent);
    const auto readyInfo = inspectLocalDemoSuccessorHandle(ready);
    const std::string applicationUtf8 = wideToUtf8(application);
    const std::string directoryUtf8 = wideToUtf8(directory);
    std::fprintf(
        diagnostic,
        "stage=%s native=%lu first_create_process_error=%lu\n"
        "create_flags=%lu application=%s directory=%s\n"
        "in_job=%lu in_job_error=%lu job_query_error=%lu job_limit_flags=%lu\n"
        "attribute_bytes=%llu attribute_list_initialized=%lu "
        "handle_list_updated=%lu attribute_probe_error=%lu "
        "attribute_update_error=%lu\n"
        "client_file_type=%lu client_file_type_error=%lu "
        "client_inherit_flags=%lu client_inherit_error=%lu "
        "client_process_id=%lu client_process_id_error=%lu\n"
        "parent_file_type=%lu parent_file_type_error=%lu "
        "parent_inherit_flags=%lu parent_inherit_error=%lu "
        "parent_process_id=%lu parent_process_id_error=%lu\n"
        "ready_file_type=%lu ready_file_type_error=%lu "
        "ready_inherit_flags=%lu ready_inherit_error=%lu "
        "ready_process_id=%lu ready_process_id_error=%lu\n",
        stage == nullptr ? "unknown" : stage,
        static_cast<unsigned long>(nativeError),
        static_cast<unsigned long>(firstCreateProcessError),
        static_cast<unsigned long>(createFlags), applicationUtf8.c_str(),
        directoryUtf8.c_str(), static_cast<unsigned long>(inJob),
        static_cast<unsigned long>(inJobError),
        static_cast<unsigned long>(jobQueryError),
        static_cast<unsigned long>(limits.BasicLimitInformation.LimitFlags),
        static_cast<unsigned long long>(attributeBytes),
        static_cast<unsigned long>(attributeListInitialized),
        static_cast<unsigned long>(handleListUpdated),
        static_cast<unsigned long>(attributeProbeError),
        static_cast<unsigned long>(attributeUpdateError),
        static_cast<unsigned long>(clientInfo.fileType),
        static_cast<unsigned long>(clientInfo.fileTypeError),
        static_cast<unsigned long>(clientInfo.inheritFlags),
        static_cast<unsigned long>(clientInfo.inheritFlagsError),
        static_cast<unsigned long>(clientInfo.processId),
        static_cast<unsigned long>(clientInfo.processIdError),
        static_cast<unsigned long>(parentInfo.fileType),
        static_cast<unsigned long>(parentInfo.fileTypeError),
        static_cast<unsigned long>(parentInfo.inheritFlags),
        static_cast<unsigned long>(parentInfo.inheritFlagsError),
        static_cast<unsigned long>(parentInfo.processId),
        static_cast<unsigned long>(parentInfo.processIdError),
        static_cast<unsigned long>(readyInfo.fileType),
        static_cast<unsigned long>(readyInfo.fileTypeError),
        static_cast<unsigned long>(readyInfo.inheritFlags),
        static_cast<unsigned long>(readyInfo.inheritFlagsError),
        static_cast<unsigned long>(readyInfo.processId),
        static_cast<unsigned long>(readyInfo.processIdError));
    std::fclose(diagnostic);
}
#endif

class NativeTerminalCleanupPort final : public BootstrapTerminalCleanupPort {
public:
    BootstrapTerminalCleanupResult LaunchSuccessor(
        const ValidatedInstallPlan& plan,
        const ReplacementJournal& journal,
        const BootstrapExecutableExpectation& expected) override {
        if (!plan.isValidated() || !expected.valid() ||
            expected.absolutePath.empty() || plan.packagePlanBytes.empty() ||
            plan.packageSignatureHex.size() != 128U) {
            return {BootstrapTerminalCleanupStatus::InvalidIdentity,
                    ERROR_INVALID_DATA};
        }
        const std::string expectedPath =
            plan.identity.targetRoot + "/HarkenSearchBootstrap.exe";
        if (normalizeFinalPath(utf8ToWide(expected.absolutePath)) !=
            normalizeFinalPath(utf8ToWide(expectedPath))) {
            return {BootstrapTerminalCleanupStatus::InvalidIdentity,
                    ERROR_INVALID_NAME};
        }
        const std::wstring application = utf8ToWide(expected.absolutePath);
        const std::wstring directory = utf8ToWide(plan.identity.targetRoot);
        if (application.empty() || directory.empty()) {
            return {BootstrapTerminalCleanupStatus::InvalidIdentity,
                    ERROR_INVALID_NAME};
        }

        ScopedHandle imagePin(CreateFileW(
            application.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        FILE_ID_INFO imageIdentity{};
        const std::wstring pinnedPath = finalPath(imagePin.get());
        if (imagePin.get() == INVALID_HANDLE_VALUE ||
            GetFileInformationByHandleEx(
                imagePin.get(), FileAttributeTagInfo, &attributes,
                sizeof(attributes)) == FALSE ||
            GetFileInformationByHandleEx(
                imagePin.get(), FileIdInfo, &imageIdentity,
                sizeof(imageIdentity)) == FALSE ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
            pinnedPath.empty() ||
            normalizeFinalPath(pinnedPath) != normalizeFinalPath(application)) {
            return {BootstrapTerminalCleanupStatus::InvalidIdentity,
                    GetLastError() == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                                     : GetLastError()};
        }
        std::uint64_t byteSize = 0U;
        std::string digest;
        DWORD hashError = ERROR_SUCCESS;
        if (!hashPinnedFileSha256(imagePin.get(), application, byteSize, digest,
                                  hashError) ||
            byteSize != expected.byteSize || lowerAscii(expected.sha256) != digest) {
            return {BootstrapTerminalCleanupStatus::InvalidIdentity,
                    hashError == ERROR_SUCCESS ? ERROR_INVALID_DATA : hashError};
        }

        CurrentUserPipeSecurity pipeSecurity;
        if (!pipeSecurity.valid()) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    ERROR_ACCESS_DENIED};
        }
        HANDLE readyReadRaw = nullptr;
        HANDLE readyWriteRaw = nullptr;
        if (!CreatePipe(&readyReadRaw, &readyWriteRaw,
                        pipeSecurity.attributes(), 64U * 1024U)) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    GetLastError()};
        }
        ScopedHandle readyServer(readyReadRaw);
        ScopedHandle readyClient(readyWriteRaw);
        if (!SetHandleInformation(readyServer.get(), HANDLE_FLAG_INHERIT, 0U)) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    GetLastError()};
        }
        std::array<std::uint8_t, 32U> pipeNonce{};
        DWORD pipeNonceError = ERROR_SUCCESS;
        if (!createBrokerNonce(pipeNonce, pipeNonceError)) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    pipeNonceError == ERROR_SUCCESS ? ERROR_CANNOT_MAKE
                                                    : pipeNonceError};
        }
        static constexpr char kPipeNonceHex[] = "0123456789abcdef";
        std::string pipeNonceHex;
        pipeNonceHex.reserve(pipeNonce.size() * 2U);
        for (const std::uint8_t byte : pipeNonce) {
            pipeNonceHex.push_back(kPipeNonceHex[byte >> 4U]);
            pipeNonceHex.push_back(kPipeNonceHex[byte & 0x0fU]);
        }
        const std::wstring pipeName =
            L"\\\\.\\pipe\\HarkenSearchBootstrapTerminal-" +
            utf8ToWide(pipeNonceHex);
        ScopedHandle server(CreateNamedPipeW(
            pipeName.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE |
                FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1U,
            2U * 1024U * 1024U, 2U * 1024U * 1024U, 5000U,
            pipeSecurity.attributes()));
        if (server.get() == INVALID_HANDLE_VALUE) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed, GetLastError()};
        }
        if (!SetHandleInformation(server.get(), HANDLE_FLAG_INHERIT, 0U)) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed, GetLastError()};
        }
        ScopedHandle client(CreateFileW(
            pipeName.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, pipeSecurity.attributes(),
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr));
        if (client.get() == INVALID_HANDLE_VALUE) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed, GetLastError()};
        }

        HANDLE duplicatedParent = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
                             GetCurrentProcess(), &duplicatedParent,
                             PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                             TRUE, 0U) ||
            duplicatedParent == nullptr || duplicatedParent == INVALID_HANDLE_VALUE) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed, GetLastError()};
        }
        ScopedHandle parentHandle(duplicatedParent);
        std::array<HANDLE, 3U> inherited{
            client.get(), parentHandle.get(), readyClient.get()};
        SIZE_T attributeBytes = 0U;
        SetLastError(ERROR_SUCCESS);
        static_cast<void>(InitializeProcThreadAttributeList(
            nullptr, 1U, 0U, &attributeBytes));
        const DWORD attributeProbeError = GetLastError();
        if (attributeBytes == 0U) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    attributeProbeError};
        }
        std::vector<std::uint8_t> attributeStorage(attributeBytes);
        ScopedProcThreadAttributeList attributeList;
        bool attributeListInitialized = false;
        bool handleListUpdated = false;
        DWORD attributeUpdateError = ERROR_SUCCESS;
        if (!attributeList.initialize(attributeStorage.data(), attributeBytes,
                                      1U)) {
            attributeUpdateError = GetLastError();
        } else {
            attributeListInitialized = true;
            if (!UpdateProcThreadAttribute(
                    attributeList.get(), 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inherited.data(), sizeof(HANDLE) * inherited.size(), nullptr,
                    nullptr)) {
                attributeUpdateError = GetLastError();
            } else {
                handleListUpdated = true;
            }
        }
        if (!attributeListInitialized || !handleListUpdated) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    attributeUpdateError};
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = client.get();
        startup.StartupInfo.hStdOutput = parentHandle.get();
        startup.StartupInfo.hStdError = readyClient.get();
        startup.lpAttributeList = attributeList.get();
        std::wstring command = L"\"" + application +
            L"\" --aris-bootstrap-terminal-cleanup";
        std::vector<wchar_t> commandLine(command.begin(), command.end());
        commandLine.push_back(L'\0');
        PROCESS_INFORMATION processInfo{};
        const DWORD successorCreateFlags = TerminalCleanupCreateFlags();
        if (successorCreateFlags == 0U) {
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    ERROR_ACCESS_DENIED};
        }
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoSuccessorCreateContextDiagnostic(
            "before-create", successorCreateFlags, application, directory,
            client.get(), parentHandle.get(), readyClient.get(), attributeBytes,
            attributeListInitialized, handleListUpdated, attributeProbeError,
            attributeUpdateError, ERROR_SUCCESS, ERROR_SUCCESS);
#endif
        if (!CreateProcessW(
                application.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                successorCreateFlags,
                nullptr, directory.c_str(), &startup.StartupInfo,
                &processInfo)) {
            const DWORD firstCreateProcessError = GetLastError();
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
            writeLocalDemoSuccessorLaunchDiagnostic(
                "create-process-failed", firstCreateProcessError);
            writeLocalDemoSuccessorCreateContextDiagnostic(
                "create-process-failed", successorCreateFlags, application,
                directory, client.get(), parentHandle.get(), readyClient.get(),
                attributeBytes, attributeListInitialized, handleListUpdated,
                attributeProbeError, attributeUpdateError,
                firstCreateProcessError, firstCreateProcessError);
#endif
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    firstCreateProcessError};
        }
        ScopedHandle successor(processInfo.hProcess);
        ScopedHandle successorThread(processInfo.hThread);
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoSuccessorLaunchDiagnostic(
            "created", ERROR_SUCCESS, processInfo.dwProcessId);
#endif

        BootstrapProcessIdentity successorIdentity;
        const bool successorMatchesPinnedImage =
            captureIdentity(successor.get(), successorIdentity) &&
            normalizeFinalPath(utf8ToWide(successorIdentity.imagePath)) ==
                normalizeFinalPath(application) &&
            successorIdentity.imageVolumeSerial ==
                imageIdentity.VolumeSerialNumber &&
            std::equal(
                successorIdentity.imageFileId.begin(),
                successorIdentity.imageFileId.end(),
                std::begin(imageIdentity.FileId.Identifier));
        if (!successorMatchesPinnedImage) {
            static_cast<void>(TerminateProcess(successor.get(), 0xE10AU));
            static_cast<void>(WaitForSingleObject(successor.get(), 5000U));
            return {BootstrapTerminalCleanupStatus::InvalidIdentity,
                    ERROR_INVALID_DATA};
        }
        if (!SetHandleInformation(client.get(), HANDLE_FLAG_INHERIT, 0U) ||
            !SetHandleInformation(parentHandle.get(), HANDLE_FLAG_INHERIT, 0U) ||
            !SetHandleInformation(readyClient.get(), HANDLE_FLAG_INHERIT, 0U)) {
            const DWORD error = GetLastError();
            static_cast<void>(TerminateProcess(successor.get(), 0xE10AU));
            static_cast<void>(WaitForSingleObject(successor.get(), 5000U));
            return {BootstrapTerminalCleanupStatus::LaunchFailed, error};
        }
        client.reset();
        parentHandle.reset();
        readyClient.reset();
        if (ResumeThread(successorThread.get()) ==
            (std::numeric_limits<DWORD>::max)()) {
            const DWORD error = GetLastError();
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
            writeLocalDemoSuccessorLaunchDiagnostic(
                "resume-failed", error, processInfo.dwProcessId);
#endif
            static_cast<void>(TerminateProcess(successor.get(), 0xE10AU));
            static_cast<void>(WaitForSingleObject(successor.get(), 5000U));
            return {BootstrapTerminalCleanupStatus::LaunchFailed, error};
        }

        const ULONGLONG deadline = GetTickCount64() + 5000U;
        DWORD transportError = ERROR_SUCCESS;
        if (!connectTerminalCleanupPipe(server.get(), deadline,
                                        transportError)) {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
            writeLocalDemoSuccessorLaunchDiagnostic(
                "connect-failed", transportError, processInfo.dwProcessId);
#endif
            static_cast<void>(TerminateProcess(successor.get(), 0xE10AU));
            static_cast<void>(WaitForSingleObject(successor.get(), 5000U));
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    transportError == ERROR_SUCCESS ? ERROR_PIPE_NOT_CONNECTED
                                                    : transportError};
        }
        const std::string payload = encodeTerminalCleanupHandoff(plan, journal);
        std::vector<std::uint8_t> bytes =
            encodeTerminalCleanupFrame(payload);
        if (bytes.empty() ||
            !transferBrokerOverlapped(server.get(), bytes.data(), bytes.size(),
                                      true, deadline, transportError)) {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
            writeLocalDemoSuccessorLaunchDiagnostic(
                "transfer-failed", transportError, processInfo.dwProcessId);
#endif
            static_cast<void>(TerminateProcess(successor.get(), 0xE10AU));
            static_cast<void>(WaitForSingleObject(successor.get(), 5000U));
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    transportError == ERROR_SUCCESS ? ERROR_INVALID_DATA
                                                    : transportError};
        }
        // Closing the only server handle is the authenticated end-of-frame.
        // The successor rejects an incomplete frame, trailing bytes, or a
        // server that stays open past the bounded channel deadline.
        server.reset();
        const TerminalCleanupFrameReadResult ready =
            readTerminalReadyFrame(readyServer.get(), 30'000U);
        const std::string expectedReady =
            std::to_string(processInfo.dwProcessId) + "|" +
            plan.invocation.transactionId;
        if (ready.status != TerminalCleanupFrameReadStatus::Accepted ||
            ready.payload != expectedReady) {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
            writeLocalDemoSuccessorLaunchDiagnostic(
                "ready-failed", ready.nativeError, processInfo.dwProcessId);
            writeLocalDemoSuccessorExitDiagnostic(successor.get(),
                                                  "ready-failed");
#endif
            static_cast<void>(TerminateProcess(successor.get(), 0xE10AU));
            static_cast<void>(WaitForSingleObject(successor.get(), 5000U));
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    ready.nativeError == ERROR_SUCCESS
                        ? ERROR_INVALID_DATA
                        : ready.nativeError};
        }
        readyServer.reset();
        const DWORD successorState =
            WaitForSingleObject(successor.get(), 0U);
        if (successorState == WAIT_OBJECT_0 || successorState == WAIT_FAILED) {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
            writeLocalDemoSuccessorLaunchDiagnostic(
                "successor-exited", successorState == WAIT_OBJECT_0
                                        ? ERROR_PROCESS_ABORTED
                                        : GetLastError(),
                processInfo.dwProcessId);
#endif
            return {BootstrapTerminalCleanupStatus::LaunchFailed,
                    successorState == WAIT_OBJECT_0
                        ? ERROR_PROCESS_ABORTED
                        : GetLastError()};
        }
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
        writeLocalDemoSuccessorLaunchDiagnostic(
            "accepted", ERROR_SUCCESS, processInfo.dwProcessId);
#endif
        return {BootstrapTerminalCleanupStatus::Accepted};
    }
};

}  // namespace

#if defined(ARIS_BOOTSTRAP_TEST_PROCESS_CUTS)
bool CaptureBootstrapParentIdentityForTesting(
    std::uintptr_t processHandle, BootstrapProcessIdentity& identity) {
    return captureIdentity(reinterpret_cast<HANDLE>(processHandle), identity);
}
#endif

int RunWindowsBootstrapLaunchBroker(
    const std::uintptr_t inheritedChannelHandle) {
    const HANDLE channel = reinterpret_cast<HANDLE>(inheritedChannelHandle);
    if (channel == nullptr || channel == INVALID_HANDLE_VALUE ||
        GetFileType(channel) != FILE_TYPE_PIPE) {
        return static_cast<int>(ERROR_INVALID_HANDLE);
    }
    for (std::uint32_t expectedSequence = 1U;
         expectedSequence <= kMaximumBrokerLaunches; ++expectedSequence) {
        BrokerFrameKind kind = BrokerFrameKind::LaunchRequest;
        std::vector<std::uint8_t> payload;
        DWORD nativeError = ERROR_SUCCESS;
        if (!readBrokerFrameSync(channel, kind, payload, nativeError)) {
            return nativeError == ERROR_BROKEN_PIPE
                ? 0
                : static_cast<int>(nativeError);
        }
        BrokerLaunchRequest request;
        if (kind != BrokerFrameKind::LaunchRequest ||
            !decodeBrokerLaunchRequest(payload, request) ||
            request.sequence != expectedSequence) {
            return static_cast<int>(ERROR_INVALID_DATA);
        }

        ScopedHandle targetJob(
            reinterpret_cast<HANDLE>(request.targetJob));
        ScopedHandle inheritedStdin(
            reinterpret_cast<HANDLE>(request.inheritedStdin));
        ScopedHandle inheritedStdout(
            reinterpret_cast<HANDLE>(request.inheritedStdout));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        const bool validJob = targetJob.get() != nullptr &&
            targetJob.get() != INVALID_HANDLE_VALUE &&
            QueryInformationJobObject(
                targetJob.get(), JobObjectExtendedLimitInformation, &limits,
                sizeof(limits), nullptr) != FALSE &&
            (limits.BasicLimitInformation.LimitFlags &
             JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0U;
        const bool validStdin = inheritedStdin.get() == nullptr ||
            (inheritedStdin.get() != INVALID_HANDLE_VALUE &&
             GetFileType(inheritedStdin.get()) == FILE_TYPE_PIPE);
        const bool validStdout = inheritedStdout.get() == nullptr ||
            (inheritedStdout.get() != INVALID_HANDLE_VALUE &&
             GetFileType(inheritedStdout.get()) == FILE_TYPE_PIPE);
        const std::wstring application = utf8ToWide(request.application);
        const std::wstring directory = utf8ToWide(request.directory);

        ScopedHandle launchedProcess;
        ScopedHandle launchedThread;
        DWORD launchStatus = ERROR_SUCCESS;
        if (!validJob || !validStdin || !validStdout || application.empty() ||
            directory.empty()) {
            launchStatus = ERROR_INVALID_DATA;
        } else {
            std::array<HANDLE, 2U> inherited{};
            DWORD inheritedCount = 0U;
            if (inheritedStdin.get() != nullptr) {
                inherited[inheritedCount++] = inheritedStdin.get();
            }
            if (inheritedStdout.get() != nullptr &&
                inheritedStdout.get() != inheritedStdin.get()) {
                inherited[inheritedCount++] = inheritedStdout.get();
            }
            ScopedProcThreadAttributeList attributes;
            const DWORD attributeCount = 1U +
                (inheritedCount != 0U ? 1U : 0U);
            SIZE_T attributeBytes = 0U;
            static_cast<void>(InitializeProcThreadAttributeList(
                nullptr, attributeCount, 0U, &attributeBytes));
            std::vector<std::uint8_t> attributeStorage(attributeBytes);
            if (attributeBytes == 0U ||
                !attributes.initialize(attributeStorage.data(), attributeBytes,
                                       attributeCount)) {
                launchStatus = GetLastError();
            } else {
                HANDLE targetJobHandle = targetJob.get();
                if (!UpdateProcThreadAttribute(
                        attributes.get(), 0U, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                        &targetJobHandle, sizeof(targetJobHandle), nullptr,
                        nullptr)) {
                    launchStatus = GetLastError();
                } else if (inheritedCount != 0U &&
                           !UpdateProcThreadAttribute(
                               attributes.get(), 0U,
                               PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                               inherited.data(),
                               sizeof(HANDLE) * inheritedCount, nullptr,
                               nullptr)) {
                    launchStatus = GetLastError();
                } else {
                    std::wstring command = L"\"" + application + L"\"";
                    std::vector<wchar_t> commandLine(command.begin(),
                                                     command.end());
                    commandLine.push_back(L'\0');
                    STARTUPINFOEXW startup{};
                    startup.StartupInfo.cb = sizeof(startup);
                    startup.lpAttributeList = attributes.get();
                    if (inheritedCount != 0U) {
                        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                        startup.StartupInfo.hStdInput =
                            inheritedStdin.get() != nullptr
                                ? inheritedStdin.get()
                                : inheritedStdout.get();
                        startup.StartupInfo.hStdOutput =
                            inheritedStdout.get() != nullptr
                                ? inheritedStdout.get()
                                : inheritedStdin.get();
                        startup.StartupInfo.hStdError =
                            startup.StartupInfo.hStdOutput;
                    }
                    PROCESS_INFORMATION process{};
                    if (!CreateProcessW(
                            application.c_str(), commandLine.data(), nullptr,
                            nullptr, inheritedCount != 0U,
                            CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                                CREATE_BREAKAWAY_FROM_JOB |
                                EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, directory.c_str(), &startup.StartupInfo,
                            &process)) {
                        launchStatus = GetLastError();
                    } else {
                        launchedProcess.reset(process.hProcess);
                        launchedThread.reset(process.hThread);
                    }
                }
            }
        }

#ifdef ARIS_BOOTSTRAP_TEST_LAUNCH_BROKER
        if (launchedProcess.get() != nullptr) {
            std::vector<wchar_t> markerPath(32768U, L'\0');
            const DWORD markerLength = GetEnvironmentVariableW(
                L"ARIS_BOOTSTRAP_TEST_BROKER_CHILD_PID", markerPath.data(),
                static_cast<DWORD>(markerPath.size()));
            if (markerLength > 0U && markerLength < markerPath.size()) {
                ScopedHandle marker(CreateFileW(
                    markerPath.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
                const DWORD processId = GetProcessId(launchedProcess.get());
                DWORD written = 0U;
                if (marker.get() != INVALID_HANDLE_VALUE) {
                    static_cast<void>(WriteFile(
                        marker.get(), &processId, sizeof(processId), &written,
                        nullptr));
                    static_cast<void>(FlushFileBuffers(marker.get()));
                }
            }
            std::array<wchar_t, 64U> timeoutCut{};
            const DWORD timeoutCutLength = GetEnvironmentVariableW(
                L"ARIS_BOOTSTRAP_TEST_BROKER_CUT", timeoutCut.data(),
                static_cast<DWORD>(timeoutCut.size()));
            const std::wstring expectedTimeoutCut =
                L"stall-after-create-" +
                std::to_wstring(expectedSequence);
            if (timeoutCutLength == expectedTimeoutCut.size() &&
                std::wstring_view(timeoutCut.data(), timeoutCutLength) ==
                    expectedTimeoutCut) {
                Sleep(1000U);
            }
        }
#endif

        const std::vector<std::uint8_t> response = encodeBrokerLaunchResponse(
            request, launchStatus, launchedProcess.get(), launchedThread.get());
        if (!writeBrokerFrameSync(channel, BrokerFrameKind::LaunchResponse,
                                  response, nativeError)) {
            return static_cast<int>(nativeError);
        }
        BrokerFrameKind acknowledgementKind =
            BrokerFrameKind::LaunchAcknowledgement;
        std::vector<std::uint8_t> acknowledgementPayload;
        bool accepted = false;
        if (!readBrokerFrameSync(channel, acknowledgementKind,
                                 acknowledgementPayload, nativeError) ||
            acknowledgementKind !=
                BrokerFrameKind::LaunchAcknowledgement ||
            !decodeBrokerAcknowledgement(acknowledgementPayload, request,
                                         accepted)) {
            return static_cast<int>(nativeError == ERROR_SUCCESS
                                        ? ERROR_INVALID_DATA
                                        : nativeError);
        }
        if (!accepted && launchedProcess.get() != nullptr) {
            static_cast<void>(TerminateJobObject(targetJob.get(), 0xE103U));
        }
#ifdef ARIS_BOOTSTRAP_TEST_LAUNCH_BROKER
        std::array<wchar_t, 64U> crashCut{};
        const DWORD crashCutLength = GetEnvironmentVariableW(
            L"ARIS_BOOTSTRAP_TEST_BROKER_CUT", crashCut.data(),
            static_cast<DWORD>(crashCut.size()));
        if (expectedSequence == 1U &&
            crashCutLength == std::wstring_view(L"crash-after-first").size() &&
            std::wstring_view(crashCut.data(), crashCutLength) ==
                L"crash-after-first") {
            return static_cast<int>(ERROR_PROCESS_ABORTED);
        }
#endif
    }
    return 0;
}

std::unique_ptr<BootstrapProcessPort> CreateNativeBootstrapProcessPort() {
    return std::make_unique<NativeProcessPort>();
}

std::unique_ptr<BootstrapProcessPort>
CreateBrokeredNativeBootstrapProcessPort(
    WindowsBootstrapLaunchBrokerCapability& capability) {
    const HANDLE channel = reinterpret_cast<HANDLE>(capability.channelHandle);
    const HANDLE broker =
        reinterpret_cast<HANDLE>(capability.brokerProcessHandle);
    const HANDLE job =
        reinterpret_cast<HANDLE>(capability.containmentJobHandle);
    BootstrapProcessIdentity expectedBroker = capability.brokerIdentity;
    const std::uint32_t timeoutMs = capability.launchTimeoutMs;
    const bool supplied = capability.valid();
    capability = {};
    BootstrapProcessIdentity observedBroker;
    BootstrapProcessIdentity currentProcess;
    BOOL inJob = FALSE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    const bool accepted = supplied && timeoutMs <= 60'000U &&
        channel != nullptr && channel != INVALID_HANDLE_VALUE &&
        GetFileType(channel) == FILE_TYPE_PIPE &&
        broker != nullptr && broker != INVALID_HANDLE_VALUE &&
        WaitForSingleObject(broker, 0U) == WAIT_TIMEOUT &&
        isSameUserProcess(broker) && captureIdentity(broker, observedBroker) &&
        sameIdentity(expectedBroker, observedBroker) &&
        captureIdentity(GetCurrentProcess(), currentProcess) &&
        normalizeFinalPath(utf8ToWide(observedBroker.imagePath)) ==
            normalizeFinalPath(utf8ToWide(currentProcess.imagePath)) &&
        observedBroker.imageVolumeSerial == currentProcess.imageVolumeSerial &&
        observedBroker.imageFileId == currentProcess.imageFileId &&
        job != nullptr && job != INVALID_HANDLE_VALUE &&
        IsProcessInJob(broker, job, &inJob) != FALSE && inJob != FALSE &&
        QueryInformationJobObject(
            job, JobObjectExtendedLimitInformation, &limits, sizeof(limits),
            nullptr) != FALSE &&
        (limits.BasicLimitInformation.LimitFlags &
         (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
          JOB_OBJECT_LIMIT_BREAKAWAY_OK)) ==
            (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
             JOB_OBJECT_LIMIT_BREAKAWAY_OK);
    if (accepted) {
        HANDLE ownedChannel = nullptr;
        HANDLE ownedBroker = nullptr;
        HANDLE ownedJob = nullptr;
        const HANDLE current = GetCurrentProcess();
        const bool transferred = DuplicateHandle(
                                     current, channel, current, &ownedChannel,
                                     0U, FALSE, DUPLICATE_SAME_ACCESS) != FALSE &&
            DuplicateHandle(current, broker, current, &ownedBroker, 0U, FALSE,
                            DUPLICATE_SAME_ACCESS) != FALSE &&
            DuplicateHandle(current, job, current, &ownedJob, 0U, FALSE,
                            DUPLICATE_SAME_ACCESS) != FALSE;
        CloseHandle(channel);
        CloseHandle(broker);
        CloseHandle(job);
        if (transferred) {
            return std::make_unique<NativeProcessPort>(
                ownedChannel, ownedBroker, ownedJob,
                std::move(observedBroker), timeoutMs);
        }
        if (ownedChannel != nullptr) CloseHandle(ownedChannel);
        if (ownedBroker != nullptr) CloseHandle(ownedBroker);
        if (ownedJob != nullptr) CloseHandle(ownedJob);
        return nullptr;
    }
    if (channel != nullptr && channel != INVALID_HANDLE_VALUE) CloseHandle(channel);
    if (broker != nullptr && broker != INVALID_HANDLE_VALUE) CloseHandle(broker);
    if (job != nullptr && job != INVALID_HANDLE_VALUE) CloseHandle(job);
    return nullptr;
}

std::unique_ptr<BootstrapHealthPort> CreateNativeBootstrapHealthPort() {
    return std::make_unique<NativeHealthPort>();
}

std::unique_ptr<BootstrapTerminalCleanupPort>
CreateNativeBootstrapTerminalCleanupPort() {
    return std::make_unique<NativeTerminalCleanupPort>();
}

std::string NativeBootstrapUpdaterOwnedBase() {
#if defined(ARIS_BOOTSTRAP_TEST_TRUSTED_ROOTS)
    // The test composition supplies a private sandbox anchor explicitly.  It
    // is never compiled into the provisioned product target.
    std::vector<wchar_t> current(32768U, L'\0');
    const DWORD length = GetCurrentDirectoryW(
        static_cast<DWORD>(current.size()), current.data());
    if (length == 0U || length >= current.size()) return {};
    std::wstring directory(current.data(), length);
    while (directory.size() > 3U &&
           (directory.back() == L'\\' || directory.back() == L'/')) {
        directory.pop_back();
    }
    const std::size_t separator = directory.find_last_of(L"\\/");
    if (separator == std::wstring::npos || separator < 3U) return {};
    directory.resize(separator);
    directory.append(L"/updates");
    return wideToUtf8(directory);
#elif defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    return std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT);
#else
    PWSTR localAppData = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                             &localAppData) != S_OK || localAppData == nullptr) {
        return {};
    }
    std::string result = wideToUtf8(std::wstring(localAppData));
    CoTaskMemFree(localAppData);
    if (result.empty()) return {};
    if (result.back() != '/' && result.back() != '\\') result.push_back('/');
    result.append("HarkenSearch/updates");
    return result;
#endif
}

std::string NativeBootstrapUpdaterOwnedBase(const ValidatedInstallPlan& plan) {
#if defined(ARIS_BOOTSTRAP_TEST_TRUSTED_ROOTS)
    return plan.invocation.updaterOwnedBase;
#else
    // Recompute the base the same way the product side does, from the install
    // root in the signed plan.  This is an independent check -- it must not
    // trust plan.invocation.updaterOwnedBase -- but it has to apply the same
    // rule, which is that the base follows the install volume so
    // install/staging/transaction share one volume serial.
    //
    // Deriving it from %LOCALAPPDATA% instead meant an install off the system
    // drive computed C:\...\AppData\Local\HarkenSearch\updates here while the plan
    // carried E:\ProgramData\HarkenSearch\updates, so the roots check rejected a
    // perfectly valid invocation with AuthorityRejected (exit 13).
    PWSTR localAppData = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                             &localAppData) != S_OK || localAppData == nullptr) {
        return {};
    }
    const std::string localAppDataUtf8 = wideToUtf8(std::wstring(localAppData));
    CoTaskMemFree(localAppData);
    if (localAppDataUtf8.empty()) return {};
    return Platform::UpdateBootstrapAuthority::ResolveUpdaterOwnedBaseForInstall(
        plan.identity.targetRoot, localAppDataUtf8);
#endif
}

void CloseBootstrapChannelHandle(const std::uintptr_t inheritedHandle) noexcept {
    const HANDLE pipe = reinterpret_cast<HANDLE>(inheritedHandle);
    if (pipe != nullptr && pipe != INVALID_HANDLE_VALUE &&
        GetFileType(pipe) == FILE_TYPE_PIPE) {
        CloseHandle(pipe);
    }
}

void CloseBootstrapParentProcessHandle(
    const std::uintptr_t inheritedHandle) noexcept {
    const HANDLE process = reinterpret_cast<HANDLE>(inheritedHandle);
    if (process != nullptr && process != INVALID_HANDLE_VALUE) {
        CloseHandle(process);
    }
}

BootstrapChannelResult ReadAuthenticatedBootstrapChannel(
    const std::uintptr_t inheritedHandle,
    const std::uintptr_t inheritedParentProcessHandle,
    const std::uint32_t timeoutMs) {
    const HANDLE parent =
        reinterpret_cast<HANDLE>(inheritedParentProcessHandle);
    if (parent == nullptr || parent == INVALID_HANDLE_VALUE) {
        return {BootstrapChannelStatus::ParentIdentityRejected};
    }
    const DWORD parentProcessId = GetProcessId(parent);
    if (parentProcessId == 0U ||
        WaitForSingleObject(parent, 0U) != WAIT_TIMEOUT) {
        return {BootstrapChannelStatus::ParentIdentityRejected,
                GetLastError()};
    }
    const HANDLE pipe = reinterpret_cast<HANDLE>(inheritedHandle);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return {BootstrapChannelStatus::InvalidHandle};
    }
    if (GetFileType(pipe) != FILE_TYPE_PIPE) {
        return {BootstrapChannelStatus::NotNamedPipe, GetLastError()};
    }
    ULONG serverProcessId = 0U;
    if (!GetNamedPipeServerProcessId(pipe, &serverProcessId) ||
        serverProcessId == 0U || serverProcessId != parentProcessId) {
        return {BootstrapChannelStatus::ParentIdentityRejected, GetLastError()};
    }
    BootstrapProcessIdentity parentIdentity;
    const bool captured = isSameUserProcess(parent) &&
                          captureIdentity(parent, parentIdentity) &&
                          BootstrapParentImageMatchesUpdaterBase(
                              parentIdentity,
                              NativeBootstrapUpdaterOwnedBase()) &&
                          isTrustedUpdaterImage(parentIdentity);
    if (!captured) return {BootstrapChannelStatus::ParentIdentityRejected, ERROR_INVALID_DATA};

    constexpr std::size_t kMaximumHandoffBytes = 1100U * 1024U;
    std::string bytes;
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        DWORD available = 0U;
        if (!PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            return {error == ERROR_BROKEN_PIPE ? BootstrapChannelStatus::Malformed
                                               : BootstrapChannelStatus::ReadFailed,
                    error};
        }
        if (available != 0U) {
            if (bytes.size() + available > kMaximumHandoffBytes) {
                return {BootstrapChannelStatus::TooLarge};
            }
            std::string chunk(available, '\0');
            DWORD read = 0U;
            if (!ReadFile(pipe, chunk.data(), available, &read, nullptr) ||
                read != available) {
                return {BootstrapChannelStatus::ReadFailed, GetLastError()};
            }
            bytes.append(chunk);
            const BootstrapHandoffDecodeResult decoded = DecodeBootstrapHandoff(bytes);
            if (decoded.ok()) {
                const InvocationDecodeResult invocation =
                    DecodeInvocationEnvelope(decoded.handoff->invocationBytes);
                if (!invocation.ok() ||
                    !sameIdentity(invocation.invocation->parentProcess, parentIdentity)) {
                    return {BootstrapChannelStatus::ParentIdentityRejected};
                }
                return {BootstrapChannelStatus::Accepted, 0, decoded.handoff,
                        parentIdentity};
            }
            const BootstrapRecoveryMarkerDecodeResult marker =
                DecodeBootstrapRecoveryMarker(bytes);
            if (marker.ok()) {
                return {BootstrapChannelStatus::Accepted, 0, std::nullopt,
                        parentIdentity, true};
            }
            if (decoded.status != BootstrapHandoffDecodeStatus::InvalidJson) {
                return {BootstrapChannelStatus::Malformed};
            }
        }
        if (GetTickCount64() - start >= timeoutMs) {
            return {BootstrapChannelStatus::TimedOut};
        }
        Sleep(5U);
    }
}

BootstrapTerminalChannelResult ReadAuthenticatedBootstrapTerminalChannel(
    const std::uintptr_t inheritedHandle,
    const std::uintptr_t inheritedParentProcessHandle,
    const std::uint32_t timeoutMs) {
    const HANDLE parent =
        reinterpret_cast<HANDLE>(inheritedParentProcessHandle);
    if (parent == nullptr || parent == INVALID_HANDLE_VALUE) {
        return {BootstrapTerminalChannelStatus::ParentIdentityRejected};
    }
    const DWORD parentProcessId = GetProcessId(parent);
    if (parentProcessId == 0U ||
        WaitForSingleObject(parent, 0U) != WAIT_TIMEOUT) {
        return {BootstrapTerminalChannelStatus::ParentIdentityRejected,
                GetLastError()};
    }
    const HANDLE pipe = reinterpret_cast<HANDLE>(inheritedHandle);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return {BootstrapTerminalChannelStatus::InvalidHandle};
    }
    if (GetFileType(pipe) != FILE_TYPE_PIPE) {
        return {BootstrapTerminalChannelStatus::NotNamedPipe, GetLastError()};
    }
    ULONG serverProcessId = 0U;
    if (!GetNamedPipeServerProcessId(pipe, &serverProcessId) ||
        serverProcessId == 0U || serverProcessId != parentProcessId) {
        return {BootstrapTerminalChannelStatus::ParentIdentityRejected,
                GetLastError()};
    }
    BootstrapProcessIdentity parentIdentity;
    if (!isSameUserProcess(parent) || !captureIdentity(parent, parentIdentity) ||
        !isTrustedUpdaterImage(parentIdentity)) {
        return {BootstrapTerminalChannelStatus::ParentIdentityRejected,
                ERROR_INVALID_DATA};
    }
    const std::string normalizedImage = lowerAscii(parentIdentity.imagePath);
    const std::size_t separator = normalizedImage.find_last_of("/\\");
    if (normalizedImage.substr(separator == std::string::npos
                                   ? 0U
                                   : separator + 1U) != "harkensearchbootstrap.exe") {
        return {BootstrapTerminalChannelStatus::ParentIdentityRejected,
                ERROR_INVALID_DATA};
    }

    const TerminalCleanupFrameReadResult frame =
        readTerminalCleanupFrame(pipe, timeoutMs);
    if (frame.status != TerminalCleanupFrameReadStatus::Accepted) {
        switch (frame.status) {
            case TerminalCleanupFrameReadStatus::TooLarge:
                return {BootstrapTerminalChannelStatus::TooLarge,
                        frame.nativeError};
            case TerminalCleanupFrameReadStatus::Malformed:
                return {BootstrapTerminalChannelStatus::Malformed,
                        frame.nativeError};
            case TerminalCleanupFrameReadStatus::TimedOut:
                return {BootstrapTerminalChannelStatus::TimedOut,
                        frame.nativeError};
            case TerminalCleanupFrameReadStatus::ReadFailed:
                return {BootstrapTerminalChannelStatus::ReadFailed,
                        frame.nativeError};
            case TerminalCleanupFrameReadStatus::Accepted:
                break;
        }
    }
    const BootstrapTerminalCleanupHandoffDecodeResult decoded =
        DecodeBootstrapTerminalCleanupHandoff(frame.payload);
    if (!decoded.ok()) {
        return {decoded.status ==
                        BootstrapTerminalCleanupHandoffDecodeStatus::TooLarge
                    ? BootstrapTerminalChannelStatus::TooLarge
                    : BootstrapTerminalChannelStatus::Malformed,
                ERROR_INVALID_DATA};
    }
    return {BootstrapTerminalChannelStatus::Accepted, 0, decoded.handoff,
            parentIdentity};
}

BootstrapTerminalReadyResult SignalBootstrapTerminalReady(
    const std::uintptr_t inheritedReadyHandle,
    const std::string_view transactionId,
    const std::uint32_t timeoutMs) {
    const HANDLE ready = reinterpret_cast<HANDLE>(inheritedReadyHandle);
    if (ready == nullptr || ready == INVALID_HANDLE_VALUE ||
        GetFileType(ready) != FILE_TYPE_PIPE || transactionId.empty() ||
        timeoutMs == 0U) {
        return {BootstrapTerminalReadyStatus::InvalidHandle,
                ERROR_INVALID_HANDLE};
    }
    const std::string payload = std::to_string(GetCurrentProcessId()) +
        "|" + std::string(transactionId);
    const std::vector<std::uint8_t> frame = encodeTerminalCleanupFrame(payload);
    if (frame.empty()) {
        return {BootstrapTerminalReadyStatus::WriteFailed,
                ERROR_INVALID_DATA};
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    DWORD nativeError = ERROR_SUCCESS;
    std::size_t offset = 0U;
    while (offset < frame.size()) {
        const DWORD remaining = remainingBrokerTime(deadline);
        if (remaining == 0U) {
            return {BootstrapTerminalReadyStatus::TimedOut, ERROR_TIMEOUT};
        }
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            frame.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0U;
        if (!WriteFile(ready, frame.data() + offset, requested, &written,
                       nullptr) ||
            written == 0U || written > requested) {
            nativeError = GetLastError();
            return {nativeError == ERROR_TIMEOUT
                        ? BootstrapTerminalReadyStatus::TimedOut
                        : BootstrapTerminalReadyStatus::WriteFailed,
                    nativeError};
        }
        offset += written;
    }
    return {BootstrapTerminalReadyStatus::Sent, ERROR_SUCCESS};
}

void CloseBootstrapTerminalReadyHandle(
    const std::uintptr_t inheritedHandle) noexcept {
    const HANDLE ready = reinterpret_cast<HANDLE>(inheritedHandle);
    if (ready != nullptr && ready != INVALID_HANDLE_VALUE) {
        CloseHandle(ready);
    }
}

BootstrapTerminalParentWaitResult WaitForBootstrapTerminalParentExit(
    const std::uintptr_t inheritedParentProcessHandle,
    const BootstrapProcessIdentity& expectedParent,
    const std::uint32_t timeoutMs) {
    const HANDLE parent =
        reinterpret_cast<HANDLE>(inheritedParentProcessHandle);
    if (parent == nullptr || parent == INVALID_HANDLE_VALUE ||
        !expectedParent.valid) {
        return {BootstrapTerminalParentWaitStatus::InvalidIdentity};
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    ULARGE_INTEGER creation{};
    if (GetProcessId(parent) != expectedParent.processId ||
        !GetProcessTimes(parent, &created, &exited, &kernel, &user)) {
        return {BootstrapTerminalParentWaitStatus::InvalidIdentity,
                ERROR_INVALID_DATA};
    }
    creation.LowPart = created.dwLowDateTime;
    creation.HighPart = created.dwHighDateTime;
    if (creation.QuadPart != expectedParent.creationTime) {
        return {BootstrapTerminalParentWaitStatus::InvalidIdentity,
                ERROR_INVALID_DATA};
    }
    const DWORD wait = WaitForSingleObject(parent, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        return {BootstrapTerminalParentWaitStatus::TimedOut, ERROR_TIMEOUT};
    }
    if (wait != WAIT_OBJECT_0) {
        return {BootstrapTerminalParentWaitStatus::WaitFailed, GetLastError()};
    }
    return {BootstrapTerminalParentWaitStatus::Exited};
}

}  // namespace aris::bootstrap

namespace {

enum class BootstrapNativeMode {
    Normal,
    Recovery,
    LaunchBroker,
    TerminalCleanup,
    Invalid,
};

BootstrapNativeMode parseBootstrapNativeMode() {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) return BootstrapNativeMode::Invalid;
    BootstrapNativeMode mode = BootstrapNativeMode::Invalid;
    if (argumentCount == 1) {
        mode = BootstrapNativeMode::Normal;
    } else if (argumentCount == 2 &&
               std::wstring_view(arguments[1]) == L"--recovery") {
        mode = BootstrapNativeMode::Recovery;
    } else if (argumentCount == 2 &&
               std::wstring_view(arguments[1]) ==
                   L"--aris-bootstrap-launch-broker") {
        mode = BootstrapNativeMode::LaunchBroker;
    } else if (argumentCount == 2 &&
               std::wstring_view(arguments[1]) ==
                   L"--aris-bootstrap-terminal-cleanup") {
        mode = BootstrapNativeMode::TerminalCleanup;
    }
    LocalFree(arguments);
    return mode;
}

bool clearTerminalInheritedHandleFlags(
    const HANDLE handoff,
    const HANDLE ready,
    const HANDLE parentProcess,
    DWORD* const failureError = nullptr) noexcept {
    if (failureError != nullptr) *failureError = ERROR_SUCCESS;
    if (handoff == nullptr || handoff == INVALID_HANDLE_VALUE ||
        ready == nullptr || ready == INVALID_HANDLE_VALUE ||
        parentProcess == nullptr || parentProcess == INVALID_HANDLE_VALUE) {
        if (failureError != nullptr) *failureError = ERROR_INVALID_HANDLE;
        return false;
    }
    const BOOL handoffCleared =
        SetHandleInformation(handoff, HANDLE_FLAG_INHERIT, 0U);
    if (handoffCleared == FALSE && failureError != nullptr) {
        *failureError = GetLastError();
    }
    const BOOL readyCleared =
        SetHandleInformation(ready, HANDLE_FLAG_INHERIT, 0U);
    if (readyCleared == FALSE && failureError != nullptr &&
        *failureError == ERROR_SUCCESS) {
        *failureError = GetLastError();
    }
    const BOOL parentCleared =
        SetHandleInformation(parentProcess, HANDLE_FLAG_INHERIT, 0U);
    if (parentCleared == FALSE && failureError != nullptr &&
        *failureError == ERROR_SUCCESS) {
        *failureError = GetLastError();
    }
    return handoffCleared != FALSE && readyCleared != FALSE &&
           parentCleared != FALSE;
}

#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
void writeLocalDemoTerminalEntryDiagnostic(
    const HANDLE handoff,
    const HANDLE ready,
    const HANDLE parentProcess,
    const bool cleared,
    const DWORD failureError) noexcept {
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-terminal-entry.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") != 0 ||
        diagnostic == nullptr) {
        return;
    }
    const DWORD handoffType =
        handoff == nullptr || handoff == INVALID_HANDLE_VALUE
            ? 0U
            : GetFileType(handoff);
    const DWORD readyType =
        ready == nullptr || ready == INVALID_HANDLE_VALUE
            ? 0U
            : GetFileType(ready);
    const DWORD parentType =
        parentProcess == nullptr || parentProcess == INVALID_HANDLE_VALUE
            ? 0U
            : GetFileType(parentProcess);
    std::fprintf(diagnostic,
                 "handoff=%p handoffType=%lu ready=%p readyType=%lu "
                 "parent=%p parentType=%lu cleared=%d error=%lu\n",
                 handoff, static_cast<unsigned long>(handoffType), ready,
                 static_cast<unsigned long>(readyType), parentProcess,
                 static_cast<unsigned long>(parentType), cleared ? 1 : 0,
                 static_cast<unsigned long>(failureError));
    std::fclose(diagnostic);
}
#endif

void writeTerminalEntryDiagnosticIfEnabled(
    const HANDLE handoff,
    const HANDLE ready,
    const HANDLE parentProcess,
    const bool cleared,
    const DWORD failureError) noexcept {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    writeLocalDemoTerminalEntryDiagnostic(
        handoff, ready, parentProcess, cleared, failureError);
#else
    static_cast<void>(handoff);
    static_cast<void>(ready);
    static_cast<void>(parentProcess);
    static_cast<void>(cleared);
    static_cast<void>(failureError);
#endif
}

}  // namespace

#ifndef ARIS_BOOTSTRAP_NO_MAIN
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const HANDLE handoff = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE ready = GetStdHandle(STD_ERROR_HANDLE);
    const HANDLE parentProcess = GetStdHandle(STD_OUTPUT_HANDLE);
    const BootstrapNativeMode mode = parseBootstrapNativeMode();
    if (mode == BootstrapNativeMode::LaunchBroker) {
        return aris::bootstrap::RunWindowsBootstrapLaunchBroker(
            reinterpret_cast<std::uintptr_t>(handoff));
    }
    if (mode == BootstrapNativeMode::TerminalCleanup) {
        DWORD terminalHandleError = ERROR_SUCCESS;
        const bool terminalHandlesCleared =
            clearTerminalInheritedHandleFlags(
                handoff, ready, parentProcess, &terminalHandleError);
        writeTerminalEntryDiagnosticIfEnabled(
            handoff, ready, parentProcess, terminalHandlesCleared,
            terminalHandleError);
        if (!terminalHandlesCleared) {
            return static_cast<int>(
                aris::bootstrap::BootstrapExitCode::InvocationRejected);
        }
        const aris::bootstrap::PackagePlanTrustConfig trust =
            aris::bootstrap::ProductionPackagePlanTrust();
        return aris::bootstrap::RunHarkenSearchBootstrapTerminalCleanupEntrypoint(
            reinterpret_cast<std::uintptr_t>(handoff),
            reinterpret_cast<std::uintptr_t>(ready),
            reinterpret_cast<std::uintptr_t>(parentProcess), trust);
    }
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
    std::fprintf(stderr, "runtime:argv-recovery=%d command=%ls\n",
                 mode == BootstrapNativeMode::Recovery
                     ? 1
                     : (mode == BootstrapNativeMode::Normal ? 0 : -1),
                 GetCommandLineW());
    std::fflush(stderr);
#endif
    if (mode != BootstrapNativeMode::Normal &&
        mode != BootstrapNativeMode::Recovery) {
        return static_cast<int>(aris::bootstrap::BootstrapExitCode::InvocationRejected);
    }
    aris::bootstrap::BootstrapEntrypointOptions options;
    options.recovery = mode == BootstrapNativeMode::Recovery;
#ifdef ARIS_BOOTSTRAP_TEST_TIMEOUTS
    options.oldProcessWaitMs = 1000U;
    options.healthWaitMs = 5000U;
#endif
#ifdef HARKEN_BOOTSTRAP_WEBVIEW_TESTING
    options.oldProcessWaitMs = 6000U;
#endif
    const aris::bootstrap::PackagePlanTrustConfig trust =
        aris::bootstrap::ProductionPackagePlanTrust();
    const bool launchedFromHandoff =
        handoff != nullptr && handoff != INVALID_HANDLE_VALUE &&
        GetFileType(handoff) == FILE_TYPE_PIPE;
#if defined(ARIS_BOOTSTRAP_TEST_DIAGNOSTICS) && !defined(HARKEN_BOOTSTRAP_WEBVIEW_TESTING)
    static_cast<void>(launchedFromHandoff);
#else
    // Only the interactive normal updater owns a window. Brokers and terminal
    // cleanup never create another UI, and observation grants no authority.
    // UI browser subprocesses must not inherit the one-shot channel or pinned
    // parent. Broker handles are made non-inheritable before SetTargetVersion.
    const bool webViewAllowed = mode == BootstrapNativeMode::Normal && launchedFromHandoff &&
        clearTerminalInheritedHandleFlags(handoff, ready, parentProcess);
    auto progress = mode == BootstrapNativeMode::Normal && launchedFromHandoff
        ? aris::bootstrap::CreateBootstrapProgressWindow(false, webViewAllowed) : nullptr;
    options.progress = progress.get();
#endif
    const int result = aris::bootstrap::RunHarkenSearchBootstrapEntrypoint(
        reinterpret_cast<std::uintptr_t>(handoff),
        reinterpret_cast<std::uintptr_t>(parentProcess), trust, options);
#if !defined(ARIS_BOOTSTRAP_TEST_DIAGNOSTICS) || defined(HARKEN_BOOTSTRAP_WEBVIEW_TESTING)
    const bool progressPresented = progress && progress->Finish(result);
    if (mode == BootstrapNativeMode::Normal && result != 0 &&
        launchedFromHandoff && !progressPresented) {
        // Bootstrap distinguishes 30+ exit codes; this dialog used to collapse
        // all of them into "uninstall and reinstall".  That is wrong for most:
        // a rollback leaves the previous version working, a stray process just
        // needs a retry, and a rejected release is not fixed by reinstalling.
        // Reporting the real outcome is what stops "I reinstalled and it still
        // fails" from being the only thing a user can say.
        const auto explanation = aris::bootstrap::ExplainBootstrapExit(result);
        if (explanation.showDialog) {
            std::wstring text;
            if (!explanation.message.empty()) {
                const int wide = MultiByteToWideChar(
                    CP_UTF8, 0, explanation.message.c_str(),
                    static_cast<int>(explanation.message.size()), nullptr, 0);
                if (wide > 0) {
                    text.resize(static_cast<std::size_t>(wide));
                    MultiByteToWideChar(
                        CP_UTF8, 0, explanation.message.c_str(),
                        static_cast<int>(explanation.message.size()),
                        text.data(), wide);
                }
            }
            const wchar_t* caption =
                explanation.requiresReinstall ? L"Harken Search 需要重新安装" : L"Harken Search 更新未完成";
            MessageBoxW(nullptr, text.c_str(), caption,
                        MB_OK |
                            (explanation.requiresReinstall ? MB_ICONERROR
                                                           : MB_ICONWARNING) |
                            MB_SETFOREGROUND);
        }
    }
#endif
    return result;
}
#endif

#else

namespace aris::bootstrap {
std::unique_ptr<BootstrapProcessPort> CreateNativeBootstrapProcessPort() {
    return nullptr;
}
std::unique_ptr<BootstrapProcessPort>
CreateBrokeredNativeBootstrapProcessPort(
    WindowsBootstrapLaunchBrokerCapability& capability) {
    capability = {};
    return nullptr;
}
std::unique_ptr<BootstrapHealthPort> CreateNativeBootstrapHealthPort() {
    return nullptr;
}
std::unique_ptr<BootstrapTerminalCleanupPort>
CreateNativeBootstrapTerminalCleanupPort() {
    return nullptr;
}
BootstrapChannelResult ReadAuthenticatedBootstrapChannel(
    std::uintptr_t, std::uintptr_t, std::uint32_t) {
    return {BootstrapChannelStatus::Unsupported};
}
BootstrapTerminalChannelResult ReadAuthenticatedBootstrapTerminalChannel(
    std::uintptr_t, std::uintptr_t, std::uint32_t) {
    return {BootstrapTerminalChannelStatus::Unsupported};
}
BootstrapTerminalReadyResult SignalBootstrapTerminalReady(
    std::uintptr_t, std::string_view, std::uint32_t) {
    return {BootstrapTerminalReadyStatus::Unsupported};
}
void CloseBootstrapTerminalReadyHandle(std::uintptr_t) noexcept {}
BootstrapTerminalParentWaitResult WaitForBootstrapTerminalParentExit(
    std::uintptr_t, const BootstrapProcessIdentity&, std::uint32_t) {
    return {BootstrapTerminalParentWaitStatus::Unsupported};
}
std::string NativeBootstrapUpdaterOwnedBase(const ValidatedInstallPlan&) {
    return {};
}
std::string NativeBootstrapUpdaterOwnedBase() {
    return {};
}
void CloseBootstrapChannelHandle(std::uintptr_t) noexcept {}
void CloseBootstrapParentProcessHandle(std::uintptr_t) noexcept {}
}  // namespace aris::bootstrap

#endif
