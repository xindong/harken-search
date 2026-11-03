#include "bootstrap/WindowsBootstrapFilesystem.h"

#include "bootstrap/HarkenSearchBootstrapAdmission.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <set>
#include <string_view>

namespace aris::bootstrap {
namespace {

constexpr std::array<std::uint8_t, 11> kMagic = {
    {'A', 'R', 'I', 'S', 'B', 'O', 'O', 'T', 'F', 'S', '2'}};
constexpr std::uint32_t kSchemaVersion = 10;
constexpr std::uint32_t kLegacySchemaVersion = 9;

bool supportedSchema(const std::uint32_t version) {
    return version == kLegacySchemaVersion || version == kSchemaVersion;
}
constexpr std::size_t kMaximumJournalBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumFieldBytes = 64U * 1024U;
constexpr std::size_t kMaximumPackagePlanBytes = 1024U * 1024U;
constexpr std::size_t kMaximumMetadataBytes = 256U * 1024U;
constexpr std::size_t kMaximumMetadataComponentBytes = 128U * 1024U;
constexpr std::size_t kMaximumItems = 4096U;
constexpr std::size_t kMaximumPathBytes = 4096U;
constexpr std::size_t kMaximumProtectedDataRoots = 64U;
constexpr std::size_t kJournalAuthenticationTagBytes = 32U;
constexpr std::string_view kInstalledInstallationId =
    "installed-a8b3c9d1-e4f5-4a6b-8c9d-0e1f2a3b4c5d";

bool decodeSha256Hex(const std::string& text,
                     std::array<std::uint8_t, 32>& output) {
    if (text.size() != output.size() * 2U) return false;
    const auto nibble = [](const char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = nibble(text[index * 2U]);
        const int low = nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool validJournalAuthenticationKey(const std::string_view key) {
    return key.size() == 64U &&
           std::all_of(key.begin(), key.end(), [](const char value) {
               return (value >= '0' && value <= '9') ||
                      (value >= 'a' && value <= 'f');
           });
}

std::array<std::uint8_t, kJournalAuthenticationTagBytes>
journalAuthenticationTag(const std::string_view key,
                         const std::uint8_t* bytes,
                         const std::size_t size) {
    constexpr std::string_view domain = "aris-bootstrap-journal-mac-v1";
    std::array<std::uint8_t, 64> innerPad{};
    std::array<std::uint8_t, 64> outerPad{};
    for (std::size_t index = 0; index < key.size(); ++index) {
        const std::uint8_t byte = static_cast<std::uint8_t>(key[index]);
        innerPad[index] = static_cast<std::uint8_t>(byte ^ 0x36U);
        outerPad[index] = static_cast<std::uint8_t>(byte ^ 0x5cU);
    }
    std::string inner;
    inner.reserve(innerPad.size() + domain.size() + 1U + size);
    inner.append(reinterpret_cast<const char*>(innerPad.data()),
                 innerPad.size());
    inner.append(domain.data(), domain.size());
    inner.push_back('\0');
    inner.append(reinterpret_cast<const char*>(bytes), size);
    std::array<std::uint8_t, 32> innerDigest{};
    if (!decodeSha256Hex(BootstrapSha256Hex(inner), innerDigest)) return {};

    std::string outer;
    outer.reserve(outerPad.size() + domain.size() + 1U + innerDigest.size());
    outer.append(reinterpret_cast<const char*>(outerPad.data()),
                 outerPad.size());
    outer.append(domain.data(), domain.size());
    outer.push_back('\0');
    outer.append(reinterpret_cast<const char*>(innerDigest.data()),
                 innerDigest.size());
    std::array<std::uint8_t, 32> result{};
    static_cast<void>(decodeSha256Hex(BootstrapSha256Hex(outer), result));
    return result;
}

bool constantTimeTagEquals(
    const std::array<std::uint8_t, kJournalAuthenticationTagBytes>& expected,
    const std::uint8_t* observed) {
    std::uint8_t difference = 0U;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        difference = static_cast<std::uint8_t>(
            difference | static_cast<std::uint8_t>(expected[index] ^
                                                   observed[index]));
    }
    return difference == 0U;
}

std::uint64_t fnv1a64(const std::uint8_t* bytes, const std::size_t size) noexcept {
    std::uint64_t digest = UINT64_C(14695981039346656037);
    for (std::size_t index = 0; index < size; ++index) {
        digest ^= static_cast<std::uint64_t>(bytes[index]);
        digest *= UINT64_C(1099511628211);
    }
    return digest;
}

void appendU32(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendU64(std::vector<std::uint8_t>& output, const std::uint64_t value) {
    for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendBytes(std::vector<std::uint8_t>& output,
                 const std::vector<std::uint8_t>& bytes) {
    appendU32(output, static_cast<std::uint32_t>(bytes.size()));
    output.insert(output.end(), bytes.begin(), bytes.end());
}

void appendString(std::vector<std::uint8_t>& output, const std::string& value) {
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void appendIdentity(std::vector<std::uint8_t>& output,
                    const WindowsFileIdentity& identity) {
    output.push_back(identity.valid ? 1U : 0U);
    if (!identity.valid) return;
    appendU64(output, identity.volumeSerial);
    output.insert(output.end(), identity.fileId.begin(), identity.fileId.end());
}

void appendProcessIdentity(std::vector<std::uint8_t>& output,
                           const BootstrapProcessIdentity& identity) {
    output.push_back(identity.valid ? 1U : 0U);
    if (!identity.valid) return;
    appendU64(output, identity.processId);
    appendU64(output, identity.creationTime);
    appendString(output, identity.imagePath);
    appendU64(output, identity.imageVolumeSerial);
    output.insert(output.end(), identity.imageFileId.begin(), identity.imageFileId.end());
}

void appendDeploymentFileIdentity(
    std::vector<std::uint8_t>& output,
    const aris::platform::DeploymentFileIdentity& identity) {
    output.push_back(identity.valid ? 1U : 0U);
    if (!identity.valid) return;
    appendString(output, identity.canonicalPath);
    appendU64(output, identity.volumeSerial);
    output.insert(output.end(), identity.fileId.begin(), identity.fileId.end());
}

void appendFingerprint(std::vector<std::uint8_t>& output,
                       const WindowsFileContentFingerprint& fingerprint) {
    output.push_back(fingerprint.valid ? 1U : 0U);
    if (!fingerprint.valid) return;
    appendU64(output, fingerprint.byteSize);
    output.insert(output.end(), fingerprint.sha256.begin(),
                  fingerprint.sha256.end());
}

void appendMetadata(std::vector<std::uint8_t>& output,
                    const WindowsFileMetadataSnapshot& metadata) {
    output.push_back(metadata.valid ? 1U : 0U);
    if (!metadata.valid) return;
    appendBytes(output, metadata.bytes);
}

void appendBool(std::vector<std::uint8_t>& output, const bool value) {
    output.push_back(value ? 1U : 0U);
}

void appendItem(std::vector<std::uint8_t>& output,
                const WindowsBootstrapJournalItemRecord& item) {
    appendString(output, item.relativePath);
    appendString(output, item.backupRelativePath);
    appendString(output, item.forwardTemporaryLeaf);
    appendString(output, item.restoreTemporaryLeaf);
    appendU32(output, static_cast<std::uint32_t>(item.classification));
    appendIdentity(output, item.stagingIdentity);
    appendIdentity(output, item.installParentIdentity);
    appendIdentity(output, item.originalIdentity);
    appendIdentity(output, item.backupIdentity);
    appendIdentity(output, item.installedIdentity);
    appendIdentity(output, item.restoredIdentity);
    appendIdentity(output, item.preparedIdentity);
    appendIdentity(output, item.restorePreparedIdentity);
    appendFingerprint(output, item.stagingContent);
    appendFingerprint(output, item.backupContent);
    appendFingerprint(output, item.preparedContent);
    appendFingerprint(output, item.restorePreparedContent);
    appendMetadata(output, item.rollbackMetadata);
    appendMetadata(output, item.installedMetadata);
    appendMetadata(output, item.restoredMetadata);
    appendMetadata(output, item.preparedMetadata);
    appendMetadata(output, item.restorePreparedMetadata);
    appendBool(output, item.removalAbsentVerified);
    appendBool(output, item.backupStarted);
    appendBool(output, item.backupComplete);
    appendBool(output, item.replacementStarted);
    appendBool(output, item.replacementPrepared);
    appendBool(output, item.replacementComplete);
    appendBool(output, item.restoreStarted);
    appendBool(output, item.restorePrepared);
    appendBool(output, item.restoreComplete);
    appendBool(output, item.removeNewStarted);
    appendBool(output, item.removeNewComplete);
    appendBool(output, item.removeBackupStarted);
    appendBool(output, item.removeBackupComplete);
}

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    bool takeU8(std::uint8_t& value) {
        if (offset_ >= bytes_.size()) return fail();
        value = bytes_[offset_++];
        return true;
    }

    bool takeU32(std::uint32_t& value) {
        if (remaining() < 4U) return fail();
        value = 0;
        for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
            value |= static_cast<std::uint32_t>(bytes_[offset_++]) << shift;
        }
        return true;
    }

    bool takeU64(std::uint64_t& value) {
        if (remaining() < 8U) return fail();
        value = 0;
        for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
            value |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
        }
        return true;
    }

    bool takeString(std::string& value, const std::size_t maximum,
                    bool allowEmpty = true) {
        std::uint32_t length = 0;
        if (!takeU32(length) || length > maximum || remaining() < length) {
            return fail();
        }
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_),
                     static_cast<std::size_t>(length));
        offset_ += length;
        if (!allowEmpty && value.empty()) return fail();
        return true;
    }

    bool takeBytes(std::vector<std::uint8_t>& value, const std::size_t maximum,
                   bool allowEmpty = true) {
        std::uint32_t length = 0;
        if (!takeU32(length) || length > maximum || remaining() < length) {
            return fail();
        }
        value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                     bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
        offset_ += length;
        if (!allowEmpty && value.empty()) return fail();
        return true;
    }

    bool takeIdentity(WindowsFileIdentity& value) {
        std::uint8_t present = 0;
        if (!takeU8(present) || present > 1U) return fail();
        value = {};
        if (present == 0U) return true;
        if (!takeU64(value.volumeSerial) || remaining() < value.fileId.size()) {
            return fail();
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    value.fileId.size(), value.fileId.begin());
        offset_ += value.fileId.size();
        const bool nonZero = std::any_of(value.fileId.begin(), value.fileId.end(),
                                         [](const std::uint8_t byte) { return byte != 0U; });
        value.valid = nonZero;
        return value.valid || fail();
    }

    bool takeProcessIdentity(BootstrapProcessIdentity& value) {
        std::uint8_t present = 0;
        if (!takeU8(present) || present > 1U) return fail();
        value = {};
        if (present == 0U) return true;
        if (!takeU64(value.processId) || !takeU64(value.creationTime) ||
            !takeString(value.imagePath, kMaximumPathBytes, false) ||
            !takeU64(value.imageVolumeSerial) ||
            remaining() < value.imageFileId.size()) {
            return fail();
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    value.imageFileId.size(), value.imageFileId.begin());
        offset_ += value.imageFileId.size();
        value.valid = value.processId != 0U && value.creationTime != 0U &&
                      value.imageVolumeSerial != 0U && !value.imagePath.empty() &&
                      std::any_of(value.imageFileId.begin(), value.imageFileId.end(),
                                  [](const std::uint8_t byte) { return byte != 0U; });
        return value.valid || fail();
    }

    bool takeDeploymentFileIdentity(
        aris::platform::DeploymentFileIdentity& value) {
        std::uint8_t present = 0;
        if (!takeU8(present) || present > 1U) return fail();
        value = {};
        if (present == 0U) return true;
        if (!takeString(value.canonicalPath, kMaximumPathBytes, false) ||
            !takeU64(value.volumeSerial) || remaining() < value.fileId.size()) {
            return fail();
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    value.fileId.size(), value.fileId.begin());
        offset_ += value.fileId.size();
        value.valid = value.volumeSerial != 0U && !value.canonicalPath.empty();
        return value.valid || fail();
    }

    bool takeFingerprint(WindowsFileContentFingerprint& value) {
        std::uint8_t present = 0;
        if (!takeU8(present) || present > 1U) return fail();
        value = {};
        if (present == 0U) return true;
        if (!takeU64(value.byteSize) || remaining() < value.sha256.size()) return fail();
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    value.sha256.size(), value.sha256.begin());
        offset_ += value.sha256.size();
        value.valid = true;
        return true;
    }

    bool takeMetadata(WindowsFileMetadataSnapshot& value) {
        std::uint8_t present = 0;
        if (!takeU8(present) || present > 1U) return fail();
        value = {};
        if (present == 0U) return true;
        if (!takeBytes(value.bytes, kMaximumMetadataBytes, false)) return false;
        value.valid = true;
        return true;
    }

    bool takeBool(bool& value) {
        std::uint8_t encoded = 0;
        if (!takeU8(encoded) || encoded > 1U) return fail();
        value = encoded != 0U;
        return true;
    }

    std::size_t offset() const noexcept { return offset_; }
    std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    bool failed() const noexcept { return failed_; }

private:
    bool fail() noexcept {
        failed_ = true;
        return false;
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_ = 0;
    bool failed_ = false;
};

bool asciiAlnum(const unsigned char value) noexcept {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z');
}

bool reservedWindowsComponent(const std::string& component) {
    const std::size_t extension = component.find('.');
    std::string base = component.substr(0, extension);
    std::transform(base.begin(), base.end(), base.begin(),
                   [](const unsigned char value) {
                       return static_cast<char>(
                           value >= 'A' && value <= 'Z' ? value + ('a' - 'A')
                                                        : value);
                   });
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") {
        return true;
    }
    return base.size() == 4U &&
           (base.compare(0, 3, "com") == 0 ||
            base.compare(0, 3, "lpt") == 0) &&
           base[3] >= '1' && base[3] <= '9';
}

bool safePathComponentBase(const std::string& component,
                           const bool allowUnicode) {
    if (component.empty() || component.size() > 255U || component == "." ||
        component == ".." || component.back() == '.' ||
        component.back() == ' ' || reservedWindowsComponent(component)) {
        return false;
    }
    for (const unsigned char character : component) {
        if ((!allowUnicode && character >= 0x80U) || character < 0x20U ||
            character == '<' || character == '>' || character == ':' ||
            character == '"' || character == '|' || character == '?' ||
            character == '*') {
            return false;
        }
    }
    return true;
}

bool safeAbsoluteRootComponent(const std::string& component) {
    return safePathComponentBase(component, true);
}

bool safeRelativeComponent(const std::string& component) {
    return component.find('~') == std::string::npos &&
           safePathComponentBase(component, false);
}

bool safeIdentifier(const std::string& value, const std::size_t maximum) {
    if (value.empty() || value.size() > maximum ||
        !asciiAlnum(static_cast<unsigned char>(value.front())) ||
        !asciiAlnum(static_cast<unsigned char>(value.back()))) {
        return false;
    }
    for (const unsigned char character : value) {
        if ((!asciiAlnum(character) && character != '.' &&
             character != '-' && character != '_') || character == '~' ||
            character == '/' || character == '\\' || character < 0x20U) {
            return false;
        }
    }
    return true;
}

bool safeRelativePath(const std::string& value) {
    if (value.empty() || value.size() > kMaximumPathBytes ||
        value.front() == '/' || value.back() == '/' ||
        value.find('\\') != std::string::npos || value.find('~') != std::string::npos) {
        return false;
    }
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('/', begin);
        const std::string component = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (component.find(':') != std::string::npos ||
            !safeRelativeComponent(component)) return false;
        if (end == std::string::npos) break;
        begin = end + 1U;
    }
    return true;
}

bool safeAbsoluteRoot(const std::string& value) {
    if (value.empty() || value.size() > kMaximumPathBytes ||
        value.find('\0') != std::string::npos) {
        return false;
    }
    // The portable codec does not resolve paths. It rejects traversal and
    // invalid/empty components, while leaving literal '~' and Unicode root
    // components to the native exact-final-path/FileId authority check.
    std::string normalized = value;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.size() < 3U ||
        !((normalized[0] >= 'A' && normalized[0] <= 'Z') ||
          (normalized[0] >= 'a' && normalized[0] <= 'z')) ||
        normalized[1] != ':' || normalized[2] != '/' ||
        normalized.back() == '/' || normalized.find("//", 3U) !=
            std::string::npos) {
        return false;
    }
    std::size_t begin = 3U;
    while (begin < normalized.size()) {
        const std::size_t end = normalized.find('/', begin);
        const std::string component = normalized.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!safeAbsoluteRootComponent(component)) return false;
        if (end == std::string::npos) break;
        begin = end + 1U;
    }
    return true;
}

bool safeHealthToken(const std::string& value) {
    if (value.size() != 64U) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isdigit(character) != 0 ||
               (character >= 'a' && character <= 'f');
    });
}

bool safeDigestOrEmpty(const std::string& value) {
    if (value.empty()) return true;
    if (value.size() != 64U) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isdigit(character) != 0 ||
               (character >= 'a' && character <= 'f');
    });
}

bool safeSignedPackageReference(const WindowsBootstrapJournalRecord& record) {
    if (record.packagePlanBytes.empty() || record.packageSignatureHex.empty()) {
        return record.packagePlanBytes.empty() &&
               record.packageSignatureHex.empty();
    }
    return record.packagePlanBytes.size() <= kMaximumPackagePlanBytes &&
           record.packageSignatureHex.size() == 128U &&
           std::all_of(record.packageSignatureHex.begin(),
                       record.packageSignatureHex.end(),
                       [](const unsigned char character) {
                           return std::isdigit(character) != 0 ||
                                  (character >= 'a' && character <= 'f');
                       });
}

bool safeProcessIdentity(const BootstrapProcessIdentity& value) {
    if (!value.valid) return true;
    return value.processId != 0U && value.creationTime != 0U &&
           value.imageVolumeSerial != 0U && !value.imagePath.empty() &&
           safeAbsoluteRoot(value.imagePath) &&
           std::any_of(value.imageFileId.begin(), value.imageFileId.end(),
                       [](const std::uint8_t byte) { return byte != 0U; });
}

std::string normalizedProcessImage(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(
                           character >= 'A' && character <= 'Z'
                               ? character + ('a' - 'A')
                               : character);
                   });
    while (value.size() > 1U && value.back() == '/') value.pop_back();
    return value;
}

// Recovery may observe a new updater process after the original updater
// exited.  PID and creation time are therefore intentionally excluded only
// for this explicit rebind path; the canonical image spelling, volume and
// FileId remain exact authority and prevent a same-user image substitution.
bool sameProcessImageIdentity(const BootstrapProcessIdentity& left,
                              const BootstrapProcessIdentity& right) {
    return left.valid && right.valid &&
           normalizedProcessImage(left.imagePath) ==
               normalizedProcessImage(right.imagePath) &&
           left.imageVolumeSerial == right.imageVolumeSerial &&
           left.imageFileId == right.imageFileId;
}

bool safeDeploymentFileIdentity(
    const aris::platform::DeploymentFileIdentity& value) {
    return value.valid && value.volumeSerial != 0U &&
           !value.canonicalPath.empty() && safeAbsoluteRoot(value.canonicalPath) &&
           std::any_of(value.fileId.begin(), value.fileId.end(),
                       [](const std::uint8_t byte) { return byte != 0U; });
}

bool sameDeploymentPath(const std::string& left, const std::string& right) {
    return normalizedProcessImage(left) == normalizedProcessImage(right);
}

bool strictAuthorityFields(const WindowsBootstrapJournalRecord& record) {
    const bool installed =
        record.deploymentMode == aris::platform::DeploymentMode::Installed;
    const bool portable =
        record.deploymentMode == aris::platform::DeploymentMode::Portable;
    const bool modeIdentityPair =
        (installed && record.installationId == kInstalledInstallationId &&
         record.registeredAppId == aris::platform::kArisInstalledAppId) ||
        (portable && record.installationId.rfind("portable-", 0U) == 0U &&
         record.registeredAppId.empty());
    return (installed || portable) && modeIdentityPair &&
           safeDeploymentFileIdentity(record.targetRootIdentity) &&
           safeDeploymentFileIdentity(record.oldExecutableIdentity) &&
           sameDeploymentPath(record.targetRootIdentity.canonicalPath,
                              record.targetRoot) &&
           safeProcessIdentity(record.oldProcessIdentity) &&
           sameDeploymentPath(record.oldExecutableIdentity.canonicalPath,
                              record.oldProcessIdentity.imagePath) &&
           record.oldExecutableIdentity.volumeSerial ==
               record.oldProcessIdentity.imageVolumeSerial &&
           record.oldExecutableIdentity.fileId ==
               record.oldProcessIdentity.imageFileId &&
           record.packagePlanDigest.size() == 64U &&
           safeDigestOrEmpty(record.packagePlanDigest) &&
           safeAbsoluteRoot(record.stagingRoot) &&
           safeAbsoluteRoot(record.transactionRoot) &&
           record.protectedDataRoots.size() <= kMaximumProtectedDataRoots &&
           std::all_of(record.protectedDataRoots.begin(),
                       record.protectedDataRoots.end(),
                       [](const std::string& root) {
                           return safeAbsoluteRoot(root);
                       }) &&
           !record.packageKeyId.empty() &&
           safeIdentifier(record.packageKeyId, 64U) &&
           (record.packageChannel == "stable" ||
            record.packageChannel == "testing") &&
           record.healthNonceDigest.size() == 64U &&
           safeDigestOrEmpty(record.healthNonceDigest) &&
           record.oldProcessIdentity.valid &&
           record.parentProcessIdentity.valid &&
           safeProcessIdentity(record.oldProcessIdentity) &&
           safeProcessIdentity(record.parentProcessIdentity) &&
           record.previousFailureCount <= 2U &&
           record.oldProcessId == record.oldProcessIdentity.processId;
}

bool safePhase(const std::string& value) {
    if (value.empty() || value.size() > 128U) return false;
    return std::all_of(value.begin(), value.end(),
                       [](const unsigned char character) {
                           return character >= 0x20U && character < 0x7FU &&
                                  character != '~';
                       });
}

std::uint16_t metadataU16(const std::vector<std::uint8_t>& bytes,
                          const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1U] << 8U);
}

std::uint32_t metadataU32(const std::vector<std::uint8_t>& bytes,
                          const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

bool validSid(const std::vector<std::uint8_t>& bytes,
              const std::size_t descriptorOffset,
              const std::size_t descriptorLength,
              const std::uint32_t relativeOffset) {
    if (relativeOffset == 0U) return true;
    if (relativeOffset < 20U || relativeOffset % 4U != 0U ||
        relativeOffset > descriptorLength ||
        descriptorLength - relativeOffset < 8U) {
        return false;
    }
    const std::size_t sidOffset =
        descriptorOffset + static_cast<std::size_t>(relativeOffset);
    const std::uint8_t subAuthorityCount = bytes[sidOffset + 1U];
    const std::size_t sidLength =
        8U + static_cast<std::size_t>(subAuthorityCount) * 4U;
    return bytes[sidOffset] == 1U && subAuthorityCount <= 15U &&
           sidLength <= descriptorLength - relativeOffset;
}

bool validAcl(const std::vector<std::uint8_t>& bytes,
              const std::size_t descriptorOffset,
              const std::size_t descriptorLength,
              const std::uint32_t relativeOffset) {
    if (relativeOffset == 0U) return true;
    if (relativeOffset < 20U || relativeOffset % 4U != 0U ||
        relativeOffset > descriptorLength ||
        descriptorLength - relativeOffset < 8U) {
        return false;
    }
    const std::size_t aclOffset =
        descriptorOffset + static_cast<std::size_t>(relativeOffset);
    const std::uint8_t revision = bytes[aclOffset];
    const std::uint16_t aclSize = metadataU16(bytes, aclOffset + 2U);
    const std::uint16_t aceCount = metadataU16(bytes, aclOffset + 4U);
    if ((revision != 2U && revision != 4U) || aclSize < 8U ||
        aclSize > descriptorLength - relativeOffset) {
        return false;
    }
    std::size_t aceOffset = aclOffset + 8U;
    const std::size_t aclEnd = aclOffset + aclSize;
    for (std::uint16_t index = 0; index < aceCount; ++index) {
        if (aceOffset > aclEnd || aclEnd - aceOffset < 4U) return false;
        const std::uint16_t aceSize = metadataU16(bytes, aceOffset + 2U);
        if (aceSize < 4U || aceSize % 4U != 0U ||
            aceSize > aclEnd - aceOffset) {
            return false;
        }
        aceOffset += aceSize;
    }
    return aceOffset <= aclEnd;
}

bool validSecurityDescriptor(const std::vector<std::uint8_t>& bytes,
                             const std::size_t offset,
                             const std::size_t length) {
    constexpr std::uint16_t kSelfRelative = 0x8000U;
    if (length < 20U || length > kMaximumMetadataComponentBytes ||
        offset > bytes.size() || length > bytes.size() - offset ||
        bytes[offset] != 1U ||
        (metadataU16(bytes, offset + 2U) & kSelfRelative) == 0U) {
        return false;
    }
    const std::uint32_t owner = metadataU32(bytes, offset + 4U);
    const std::uint32_t group = metadataU32(bytes, offset + 8U);
    const std::uint32_t sacl = metadataU32(bytes, offset + 12U);
    const std::uint32_t dacl = metadataU32(bytes, offset + 16U);
    return validSid(bytes, offset, length, owner) &&
           validSid(bytes, offset, length, group) &&
           validAcl(bytes, offset, length, sacl) &&
           validAcl(bytes, offset, length, dacl);
}

bool validMetadata(const WindowsFileMetadataSnapshot& value) {
    if (!value.valid) return true;
    if (value.bytes.size() < 52U ||
        value.bytes.size() > kMaximumMetadataBytes ||
        value.bytes[0] != 0x41U || value.bytes[1] != 0x42U ||
        value.bytes[2] != 0x4dU || value.bytes[3] != 0x32U) {
        return false;
    }
    const std::uint32_t descriptorLength = metadataU32(value.bytes, 40U);
    if (descriptorLength == 0U ||
        descriptorLength > kMaximumMetadataComponentBytes ||
        44U + static_cast<std::size_t>(descriptorLength) + 4U >
            value.bytes.size() ||
        !validSecurityDescriptor(value.bytes, 44U, descriptorLength)) {
        return false;
    }
    const std::size_t labelOffset =
        44U + static_cast<std::size_t>(descriptorLength);
    const std::uint32_t labelLength = metadataU32(value.bytes, labelOffset);
    return labelLength != 0U &&
           labelLength <= kMaximumMetadataComponentBytes &&
           labelOffset + 4U + static_cast<std::size_t>(labelLength) ==
               value.bytes.size() &&
           validSecurityDescriptor(value.bytes, labelOffset + 4U,
                                   labelLength);
}

bool validItem(const WindowsBootstrapJournalItemRecord& item,
               const std::uint32_t schemaVersion) {
    if (!safeRelativePath(item.relativePath)) return false;
    const auto identityRequired = [](const WindowsFileIdentity& value) {
        return value.valid;
    };
    const auto fingerprintRequired =
        [](const WindowsFileContentFingerprint& value) {
            return value.valid;
        };
    const bool replaceExisting =
        item.classification == JournalItemClass::ReplaceExisting;
    const bool createNew = item.classification == JournalItemClass::CreateNew;
    const bool removeExisting =
        item.classification == JournalItemClass::RemoveExisting;
    if (item.classification != JournalItemClass::PreserveExisting &&
        (!replaceExisting && !createNew && !removeExisting ||
         item.backupRelativePath.empty() ||
         item.forwardTemporaryLeaf.empty() || item.restoreTemporaryLeaf.empty())) {
        return false;
    }
    if (!item.backupRelativePath.empty() &&
        !safeRelativePath(item.backupRelativePath)) return false;
    if (!item.forwardTemporaryLeaf.empty() &&
        !safeRelativePath(item.forwardTemporaryLeaf)) return false;
    if (!item.restoreTemporaryLeaf.empty() &&
        !safeRelativePath(item.restoreTemporaryLeaf)) return false;
    if (!validMetadata(item.rollbackMetadata) ||
        !validMetadata(item.installedMetadata) ||
        !validMetadata(item.restoredMetadata) ||
        !validMetadata(item.preparedMetadata) ||
        !validMetadata(item.restorePreparedMetadata)) {
        return false;
    }
    if (item.classification == JournalItemClass::PreserveExisting) {
        return !item.removalAbsentVerified &&
               !item.backupStarted && !item.backupComplete &&
               !item.replacementStarted && !item.replacementPrepared &&
               !item.replacementComplete && !item.restoreStarted &&
               !item.restorePrepared && !item.restoreComplete &&
               !item.removeNewStarted &&
               !item.removeNewComplete && !item.removeBackupStarted &&
               !item.removeBackupComplete && identityRequired(item.originalIdentity);
    }
    if (item.removalAbsentVerified) {
        return removeExisting && identityRequired(item.installParentIdentity) &&
               !item.stagingIdentity.valid && !item.originalIdentity.valid &&
               !item.backupIdentity.valid && !item.installedIdentity.valid &&
               !item.restoredIdentity.valid && !item.preparedIdentity.valid &&
               !item.restorePreparedIdentity.valid &&
               !item.stagingContent.valid && !item.backupContent.valid &&
               !item.preparedContent.valid &&
               !item.restorePreparedContent.valid &&
               !item.rollbackMetadata.valid && !item.installedMetadata.valid &&
               !item.restoredMetadata.valid && !item.preparedMetadata.valid &&
               !item.restorePreparedMetadata.valid && !item.backupStarted &&
               !item.backupComplete && !item.replacementStarted &&
               !item.replacementPrepared && item.replacementComplete &&
               !item.restoreStarted && !item.restorePrepared &&
               !item.restoreComplete && !item.removeNewStarted &&
               !item.removeNewComplete && !item.removeBackupStarted &&
               !item.removeBackupComplete;
    }
    if ((replaceExisting || removeExisting) &&
        (!identityRequired(item.originalIdentity) ||
         !fingerprintRequired(item.backupContent) ||
         !item.rollbackMetadata.valid || !identityRequired(item.installParentIdentity))) {
        return false;
    }
    if (replaceExisting &&
        (!identityRequired(item.stagingIdentity) ||
         !fingerprintRequired(item.stagingContent))) {
        return false;
    }
    if (removeExisting &&
        (item.stagingIdentity.valid || item.stagingContent.valid ||
         item.installedIdentity.valid || item.installedMetadata.valid ||
         item.preparedIdentity.valid || item.preparedContent.valid ||
         item.preparedMetadata.valid || item.replacementPrepared)) {
        return false;
    }
    // Schema 10 may defer a new nested file's parent binding until directory
    // preparation. Never admit an unbound parent after file mutation starts.
    const bool pendingParent = schemaVersion >= 10U && createNew &&
        item.relativePath.find('/') != std::string::npos &&
        !item.replacementStarted && !item.replacementPrepared &&
        !item.replacementComplete && !item.removeNewStarted && !item.removeNewComplete;
    if (createNew &&
        (!identityRequired(item.stagingIdentity) ||
         (!identityRequired(item.installParentIdentity) && !pendingParent) ||
         item.originalIdentity.valid || item.backupIdentity.valid ||
         item.backupContent.valid || item.rollbackMetadata.valid ||
         item.backupStarted || item.backupComplete)) {
        return false;
    }
    if (item.backupComplete &&
        (!item.backupStarted || !identityRequired(item.backupIdentity) ||
         !fingerprintRequired(item.backupContent) ||
         !item.rollbackMetadata.valid)) {
        return false;
    }
    // Metadata snapshots are backend-owned opaque bytes. Windows may
    // canonicalize inherited/defaulted ACL provenance while preserving the
    // exact security contract, so the portable codec validates structure and
    // lifecycle presence only. Recovery performs the authoritative platform
    // equivalence checks before it adopts any prepared or published object.
    if (item.replacementPrepared &&
        (!item.replacementStarted || removeExisting ||
         !identityRequired(item.preparedIdentity) ||
         !fingerprintRequired(item.preparedContent) ||
         !item.preparedMetadata.valid ||
         item.preparedContent != item.stagingContent)) {
        return false;
    }
    if (item.replacementComplete && !removeExisting &&
        (!item.replacementStarted || !item.replacementPrepared ||
         !identityRequired(item.installedIdentity) ||
         !item.installedMetadata.valid ||
         item.installedIdentity != item.preparedIdentity)) {
        return false;
    }
    if (item.replacementComplete && removeExisting &&
        (!item.replacementStarted || item.replacementPrepared)) {
        return false;
    }
    if (item.restoreStarted &&
        ((!replaceExisting && !removeExisting) || !item.replacementComplete)) {
        return false;
    }
    if (item.restorePrepared &&
         (!item.restoreStarted || (!replaceExisting && !removeExisting) ||
         !identityRequired(item.restorePreparedIdentity) ||
         !fingerprintRequired(item.restorePreparedContent) ||
         !item.restorePreparedMetadata.valid ||
         item.restorePreparedContent != item.backupContent)) {
        return false;
    }
    if (item.restoreComplete &&
         (!item.restoreStarted || !item.restorePrepared ||
          (!replaceExisting && !removeExisting) ||
         !identityRequired(item.restoredIdentity) ||
         !item.restoredMetadata.valid ||
         item.restoredIdentity != item.restorePreparedIdentity)) {
        return false;
    }
    if (item.removeNewStarted && (!createNew || !item.replacementComplete)) {
        return false;
    }
    if (item.removeNewComplete && !item.removeNewStarted) return false;
    if (item.removeBackupStarted &&
        ((!replaceExisting && !removeExisting) || !item.backupComplete)) {
        return false;
    }
    if (item.removeBackupComplete && !item.removeBackupStarted) return false;
    return true;
}

bool validRuntimePairAuthority(const WindowsBootstrapJournalRecord& record) {
    const bool directoryBound = record.runtimePairDirectoryIdentity.valid;
    const bool bootstrapBound = record.runtimePairBootstrapIdentity.valid;
    const bool contentBound = record.runtimePairBootstrapContent.valid;
    if (!record.runtimePairExpected) {
        return !directoryBound && !bootstrapBound && !contentBound;
    }
    return directoryBound && bootstrapBound && contentBound;
}

bool validRecordState(const WindowsBootstrapJournalRecord& record,
                       const bool strictRecovery) {
    if (!validRuntimePairAuthority(record) ||
        (record.invocationCleanupComplete &&
         !record.invocationCleanupStarted)) {
        return false;
    }
    const bool committedCleanupTerminal =
        record.commitComplete && record.backupDirectoryRemoved &&
        !record.rollbackDurable && !record.rollbackComplete &&
        record.healthPhase == "accepted" && record.healthTokenConsumed;
    const bool rolledBackCleanupTerminal =
        !record.commitComplete && record.rollbackComplete &&
        record.backupDirectoryRemoved &&
        record.previousApplicationRelaunchComplete;
    if (record.invocationCleanupStarted &&
        !committedCleanupTerminal && !rolledBackCleanupTerminal) {
        return false;
    }
    const bool hasUpdatedChild = record.updatedChildIdentity.valid;
    if ((record.healthPhase.empty() &&
         (hasUpdatedChild || record.healthTokenConsumed)) ||
        (record.healthPhase == "awaiting-health" &&
         (!hasUpdatedChild || record.healthTokenConsumed)) ||
        (record.healthPhase == "accepted" &&
         (!hasUpdatedChild || !record.healthTokenConsumed)) ||
        (!record.healthPhase.empty() &&
         record.healthPhase != "awaiting-health" &&
         record.healthPhase != "accepted")) {
        return false;
    }
    if (record.committedApplicationRelaunchPending) {
        if (!record.commitComplete || record.rollbackDurable ||
            record.rollbackComplete) {
            return false;
        }
    } else if (record.commitComplete &&
               (record.healthPhase != "accepted" || !hasUpdatedChild ||
                !record.healthTokenConsumed)) {
        // The ordinary commit transition is reachable only after the original
        // exact child has acknowledged health. Empty/awaiting states after
        // commit require the explicit replayable relaunch marker above.
        return false;
    }
    // Generation one is the root of the two-slot chain and therefore has no
    // predecessor binding. Every later generation must carry both halves of
    // the predecessor observation (identity and content); accepting only one
    // half would let restart promote a slot whose previous durable winner was
    // substituted or truncated. Keep this on the untrusted recovery path so
    // tests can still construct malformed producer bytes for decoder tests.
    if (strictRecovery) {
        const bool predecessorIdentity = record.journalIdentity.valid;
        const bool predecessorContent = record.journalContent.valid;
        if (predecessorIdentity != predecessorContent ||
            (record.generation == 1U && predecessorIdentity) ||
            (record.generation > 1U && !predecessorIdentity)) {
            return false;
        }
    }
    // A started directory has no journal-bound identity until the directory
    // publication checkpoint completes. The state is a legitimate
    // write-ahead predecessor only after generation one is durable; if it is
    // the selected winner, BeginRecovery still quarantines it rather than
    // adopting an arbitrary same-name directory.
    if (strictRecovery && record.backupDirectoryStarted &&
        !record.backupDirectoryComplete && !record.initialJournalDurable) {
        return false;
    }
    if (record.backupDirectoryComplete &&
        !record.backupDirectoryRemoved &&
        !record.backupRootIdentity.valid) {
        return false;
    }
    if (record.backupDirectoryComplete && !record.backupDirectoryStarted) {
        return false;
    }
    if (record.backupDirectoryRemoveStarted &&
        (!record.backupDirectoryComplete ||
         (!record.commitComplete && !record.rollbackComplete))) {
        return false;
    }
    if (record.backupDirectoryRemoveStarted) {
        for (const auto& item : record.items) {
            if ((item.classification == JournalItemClass::ReplaceExisting ||
                 item.classification == JournalItemClass::RemoveExisting) &&
                !item.removalAbsentVerified &&
                (!item.backupComplete || !item.removeBackupComplete)) {
                return false;
            }
        }
    }
    if (record.commitComplete &&
        (record.rollbackDurable || record.rollbackComplete ||
         !record.backupDirectoryComplete)) {
        return false;
    }
    if (record.rollbackComplete && !record.rollbackDurable) return false;
    const bool hasPreviousApplication =
        record.previousApplicationIdentity.valid;
    if ((record.previousApplicationRelaunchStarted &&
         !record.rollbackComplete) ||
        (hasPreviousApplication &&
         !record.previousApplicationRelaunchStarted) ||
        (record.previousApplicationRelaunchComplete &&
         (!record.previousApplicationRelaunchStarted ||
          !hasPreviousApplication))) {
        return false;
    }

    // No forward replacement may begin until every ReplaceExisting source has
    // a journal-bound, durable backup.  Otherwise a crash can leave an early
    // target publication with no deterministic restore authority for a later
    // item.  This invariant is intentionally global rather than inferred from
    // the order of items in the vector.
    const bool anyReplacementStarted = std::any_of(
        record.items.begin(), record.items.end(),
        [](const WindowsBootstrapJournalItemRecord& item) {
            return !item.removalAbsentVerified &&
                   (item.replacementStarted || item.replacementPrepared ||
                    item.replacementComplete);
        });
    if (anyReplacementStarted) {
        for (const auto& candidate : record.items) {
            if ((candidate.classification == JournalItemClass::ReplaceExisting ||
                 candidate.classification == JournalItemClass::RemoveExisting) &&
                !candidate.removalAbsentVerified &&
                !candidate.backupComplete) {
                return false;
            }
        }
    }
    for (const auto& item : record.items) {
        if ((item.backupStarted || item.backupComplete ||
             item.replacementStarted || item.replacementPrepared ||
             item.replacementComplete) && !item.removalAbsentVerified &&
            !record.backupDirectoryComplete) {
            return false;
        }
        if ((item.restoreStarted || item.restorePrepared ||
             item.restoreComplete || item.removeNewStarted ||
             item.removeNewComplete) && !record.rollbackDurable) {
            return false;
        }
        if (strictRecovery && record.rollbackDurable && item.replacementStarted &&
            !item.replacementComplete) {
            return false;
        }
        if (strictRecovery && record.rollbackComplete &&
            item.replacementStarted && !item.replacementComplete) {
            return false;
        }
        if (record.commitComplete && item.classification !=
                                         JournalItemClass::PreserveExisting &&
            !item.replacementComplete) {
            return false;
        }
        if (record.commitComplete &&
            (item.restoreStarted || item.restorePrepared ||
             item.restoreComplete || item.removeNewStarted ||
             item.removeNewComplete)) {
            return false;
        }
        if (record.rollbackComplete &&
            (item.classification == JournalItemClass::ReplaceExisting ||
             item.classification == JournalItemClass::RemoveExisting) &&
            !item.removalAbsentVerified &&
            item.replacementComplete && !item.restoreComplete) {
            return false;
        }
        if (record.rollbackComplete && item.classification ==
                                           JournalItemClass::CreateNew &&
            item.replacementComplete && !item.removeNewComplete) {
            return false;
        }
        if (record.backupDirectoryRemoved &&
            (item.classification == JournalItemClass::ReplaceExisting ||
             item.classification == JournalItemClass::RemoveExisting) &&
            !item.removalAbsentVerified &&
            !item.removeBackupComplete) {
            return false;
        }
        // Removing a backup is terminal cleanup only.  A generation that
        // advertises this intent before commit/rollback is not replayable,
        // because restart could lose the sole rollback copy.
        if ((item.removeBackupStarted || item.removeBackupComplete) &&
            !record.commitComplete && !record.rollbackComplete) {
            return false;
        }
    }
    if (record.backupDirectoryRemoved &&
        (!record.backupDirectoryRemoveStarted ||
         (!record.commitComplete && !record.rollbackComplete))) {
        return false;
    }
    return true;
}

bool validAuxiliaryNames(const WindowsBootstrapJournalRecord& record) {
    std::set<std::string> reserved;
    const auto folded = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](const unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        return value;
    };
    for (const auto& item : record.items) {
        reserved.insert(folded(item.relativePath));
    }
    for (std::size_t index = 0; index < record.items.size(); ++index) {
        const auto& item = record.items[index];
        if (item.backupRelativePath !=
            "item-" + std::to_string(index) + ".bak") {
            return false;
        }
        if (item.classification == JournalItemClass::PreserveExisting) {
            if (!item.forwardTemporaryLeaf.empty() ||
                !item.restoreTemporaryLeaf.empty()) {
                return false;
            }
            continue;
        }
        const std::string prefix = ".aris-bootstrap-" + record.transactionId +
                                   "-" + std::to_string(index);
        const auto reserveLeaf = [&reserved, &item, &prefix, &folded](
                                     const char* operation) {
            for (std::size_t suffix = 0;; ++suffix) {
                const std::string leaf =
                    prefix + "-" + operation +
                    (suffix == 0U ? std::string{}
                                  : "-" + std::to_string(suffix)) + ".tmp";
                const std::size_t separator = item.relativePath.find_last_of('/');
                const std::string relative = separator == std::string::npos
                    ? leaf
                    : item.relativePath.substr(0, separator + 1U) + leaf;
                if (reserved.insert(folded(relative)).second) return leaf;
            }
        };
        if (item.forwardTemporaryLeaf != reserveLeaf("forward") ||
            item.restoreTemporaryLeaf != reserveLeaf("restore")) {
            return false;
        }
    }
    return true;
}

bool samePlan(const WindowsBootstrapJournalRecord& record,
              const ValidatedInstallPlan& plan,
              const bool allowParentProcessRebind) {
    if (record.deploymentMode != plan.identity.deploymentMode ||
        record.installationId != plan.identity.installationId ||
        record.registeredAppId != plan.identity.registeredAppId ||
        !(record.targetRootIdentity == plan.identity.targetRootIdentity) ||
        !(record.oldExecutableIdentity == plan.identity.oldExecutableIdentity) ||
        record.targetRootIdentity.canonicalPath !=
            plan.invocation.targetRootIdentityBinding ||
        record.oldExecutableIdentity.canonicalPath !=
            plan.invocation.oldExecutableIdentityBinding ||
        !(record.oldProcessIdentity == plan.invocation.oldProcess) ||
        record.targetRoot != plan.identity.targetRoot ||
        record.updaterOwnedBase != plan.invocation.updaterOwnedBase ||
        record.stagingRoot != plan.invocation.stagingRoot ||
        record.transactionRoot != plan.invocation.transactionRoot ||
        record.protectedDataRoots != plan.identity.protectedDataRoots ||
        record.invocationIdentity != plan.invocation.invocationIdentity ||
        record.version != plan.version || record.buildId != plan.buildId ||
        record.transactionId != plan.invocation.transactionId ||
        record.oldProcessId != plan.oldProcessId ||
        record.entrypoint != plan.entrypoint) {
        return false;
    }
    if (record.packagePlanDigest != plan.packagePlanDigest ||
        record.packageKeyId != plan.packageKeyId ||
        record.packageChannel != plan.packageChannel) {
        return false;
    }
    if ((!record.packagePlanBytes.empty() || !plan.packagePlanBytes.empty()) &&
        (record.packagePlanBytes != plan.packagePlanBytes ||
         record.packageSignatureHex != plan.packageSignatureHex)) {
        return false;
    }
    const std::string expectedHealthDigest =
        plan.invocation.healthNonceDigest.empty()
            ? BootstrapSha256Hex(plan.healthToken)
            : plan.invocation.healthNonceDigest;
    if (record.healthNonceDigest != expectedHealthDigest) {
        return false;
    }
    if (!(record.oldProcessIdentity == plan.invocation.oldProcess)) {
        return false;
    }
    const bool parentMatches = allowParentProcessRebind
        ? sameProcessImageIdentity(record.parentProcessIdentity,
                                   plan.invocation.parentProcess)
        : record.parentProcessIdentity == plan.invocation.parentProcess;
    if (!parentMatches) {
        return false;
    }
    std::size_t incomingItems = 0U;
    for (const WindowsBootstrapJournalItemRecord& candidate : record.items) {
        if (candidate.classification == JournalItemClass::ReplaceExisting ||
            candidate.classification == JournalItemClass::CreateNew) {
            ++incomingItems;
            const auto incoming = std::find_if(
                plan.inventory.begin(), plan.inventory.end(),
                [&candidate](const InstallInventoryItem& item) {
                    return item.relativePath == candidate.relativePath;
                });
            if (incoming == plan.inventory.end()) return false;
        } else if (candidate.classification ==
                   JournalItemClass::RemoveExisting) {
            if (std::find(plan.removals.begin(), plan.removals.end(),
                          candidate.relativePath) == plan.removals.end()) {
                return false;
            }
        } else if (std::find(plan.removals.begin(), plan.removals.end(),
                             candidate.relativePath) != plan.removals.end()) {
            return false;
        }
    }
    if (incomingItems != plan.inventory.size()) return false;
    for (const InstallInventoryItem& item : plan.inventory) {
        const auto found = std::find_if(
            record.items.begin(), record.items.end(),
            [&item](const WindowsBootstrapJournalItemRecord& candidate) {
                return candidate.relativePath == item.relativePath &&
                       (candidate.classification ==
                            JournalItemClass::ReplaceExisting ||
                        candidate.classification ==
                            JournalItemClass::CreateNew);
            });
        if (found == record.items.end()) return false;
        if (!found->stagingContent.valid || item.sha256.size() != 64U ||
            found->stagingContent.byteSize != item.byteSize) {
            return false;
        }
        for (std::size_t index = 0; index < found->stagingContent.sha256.size(); ++index) {
            const auto nibble = [](const char value) -> std::uint8_t {
                if (value >= '0' && value <= '9') {
                    return static_cast<std::uint8_t>(value - '0');
                }
                return static_cast<std::uint8_t>(10 + value - 'a');
            };
            const std::uint8_t expected = static_cast<std::uint8_t>(
                (nibble(item.sha256[index * 2U]) << 4U) |
                nibble(item.sha256[index * 2U + 1U]));
            if (found->stagingContent.sha256[index] != expected) return false;
        }
    }
    return true;
}

bool sameJournal(const WindowsBootstrapJournalRecord& record,
                 const ReplacementJournal& journal) {
    if (record.transactionId != journal.transactionId ||
        record.items.size() != journal.items.size()) return false;
    for (std::size_t index = 0; index < journal.items.size(); ++index) {
        if (record.items[index].relativePath != journal.items[index].relativePath ||
            record.items[index].classification != journal.items[index].classification) {
            return false;
        }
    }
    return true;
}

bool sameIdentityIfBound(const WindowsFileIdentity& actual,
                         const WindowsFileIdentity& expected) {
    return !expected.valid || actual == expected;
}

WindowsBootstrapJournalDecodeResult failure(
    const WindowsBootstrapJournalDecodeStatus status,
    const std::size_t fieldIndex) {
    WindowsBootstrapJournalDecodeResult result;
    result.status = status;
    result.fieldIndex = fieldIndex;
    return result;
}

int journalSlotIndex(const WindowsBootstrapJournalCandidate& candidate) {
    if (candidate.name == "aris-bootstrap.journal.a" &&
        !candidate.temporary) {
        return 0;
    }
    if (candidate.name == "aris-bootstrap.journal.b" &&
        !candidate.temporary) {
        return 1;
    }
    if (candidate.name == "aris-bootstrap.journal.a.tmp" &&
        candidate.temporary) {
        return 0;
    }
    if (candidate.name == "aris-bootstrap.journal.b.tmp" &&
        candidate.temporary) {
        return 1;
    }
    return -1;
}

bool readItem(Reader& reader, WindowsBootstrapJournalItemRecord& item,
              const std::uint32_t schemaVersion) {
    std::uint32_t classification = 0;
    if (!reader.takeString(item.relativePath, kMaximumPathBytes, false) ||
        !reader.takeString(item.backupRelativePath, kMaximumPathBytes) ||
        !reader.takeString(item.forwardTemporaryLeaf, kMaximumPathBytes) ||
        !reader.takeString(item.restoreTemporaryLeaf, kMaximumPathBytes) ||
        !reader.takeU32(classification) || classification > 3U ||
        !reader.takeIdentity(item.stagingIdentity) ||
        !reader.takeIdentity(item.installParentIdentity) ||
        !reader.takeIdentity(item.originalIdentity) ||
        !reader.takeIdentity(item.backupIdentity) ||
        !reader.takeIdentity(item.installedIdentity) ||
        !reader.takeIdentity(item.restoredIdentity) ||
        !reader.takeIdentity(item.preparedIdentity) ||
        !reader.takeIdentity(item.restorePreparedIdentity) ||
        !reader.takeFingerprint(item.stagingContent) ||
        !reader.takeFingerprint(item.backupContent) ||
        !reader.takeFingerprint(item.preparedContent) ||
        !reader.takeFingerprint(item.restorePreparedContent) ||
        !reader.takeMetadata(item.rollbackMetadata) ||
        !reader.takeMetadata(item.installedMetadata) ||
        !reader.takeMetadata(item.restoredMetadata) ||
        !reader.takeMetadata(item.preparedMetadata) ||
        !reader.takeMetadata(item.restorePreparedMetadata) ||
        !reader.takeBool(item.removalAbsentVerified) ||
        !reader.takeBool(item.backupStarted) ||
        !reader.takeBool(item.backupComplete) ||
        !reader.takeBool(item.replacementStarted) ||
        !reader.takeBool(item.replacementPrepared) ||
        !reader.takeBool(item.replacementComplete) ||
        !reader.takeBool(item.restoreStarted) ||
        !reader.takeBool(item.restorePrepared) ||
        !reader.takeBool(item.restoreComplete) ||
        !reader.takeBool(item.removeNewStarted) ||
        !reader.takeBool(item.removeNewComplete) ||
        !reader.takeBool(item.removeBackupStarted) ||
        !reader.takeBool(item.removeBackupComplete)) {
        return false;
    }
    item.classification = static_cast<JournalItemClass>(classification);
    return validItem(item, schemaVersion);
}

void appendRecordBody(std::vector<std::uint8_t>& body,
                      const WindowsBootstrapJournalRecord& record) {
    appendString(body, record.transactionId);
    appendU32(body, static_cast<std::uint32_t>(record.deploymentMode));
    appendString(body, record.installationId);
    appendString(body, record.registeredAppId);
    appendString(body, record.targetRoot);
    appendDeploymentFileIdentity(body, record.targetRootIdentity);
    appendDeploymentFileIdentity(body, record.oldExecutableIdentity);
    appendString(body, record.updaterOwnedBase);
    appendString(body, record.stagingRoot);
    appendString(body, record.transactionRoot);
    appendU32(body, static_cast<std::uint32_t>(record.protectedDataRoots.size()));
    for (const std::string& root : record.protectedDataRoots) {
        appendString(body, root);
    }
    appendU32(body, record.previousFailureCount);
    appendBool(body, record.failureRecordedForTransaction);
    appendString(body, record.invocationIdentity);
    appendString(body, record.version);
    appendString(body, record.buildId);
    appendString(body, record.packagePlanDigest);
    appendString(body, record.packageKeyId);
    appendString(body, record.packageChannel);
    appendString(body, record.packagePlanBytes);
    appendString(body, record.packageSignatureHex);
    appendString(body, record.healthNonceDigest);
    appendU64(body, record.oldProcessId);
    appendProcessIdentity(body, record.oldProcessIdentity);
    appendProcessIdentity(body, record.parentProcessIdentity);
    appendProcessIdentity(body, record.updatedChildIdentity);
    appendProcessIdentity(body, record.previousApplicationIdentity);
    appendString(body, record.healthPhase);
    appendString(body, record.entrypoint);
    appendString(body, record.phase);
    appendIdentity(body, record.installRootIdentity);
    appendIdentity(body, record.stagingRootIdentity);
    appendIdentity(body, record.transactionRootIdentity);
    appendIdentity(body, record.backupRootIdentity);
    appendIdentity(body, record.journalIdentity);
    appendFingerprint(body, record.journalContent);
    appendBool(body, record.initialJournalDurable);
    appendBool(body, record.backupDirectoryStarted);
    appendBool(body, record.backupDirectoryComplete);
    appendBool(body, record.commitComplete);
    appendBool(body, record.rollbackDurable);
    appendBool(body, record.rollbackComplete);
    appendBool(body, record.backupDirectoryRemoveStarted);
    appendBool(body, record.backupDirectoryRemoved);
    appendBool(body, record.healthTokenConsumed);
    appendBool(body, record.committedApplicationRelaunchPending);
    appendBool(body, record.previousApplicationRelaunchStarted);
    appendBool(body, record.previousApplicationRelaunchComplete);
    appendBool(body, record.runtimePairExpected);
    appendIdentity(body, record.runtimePairDirectoryIdentity);
    appendIdentity(body, record.runtimePairBootstrapIdentity);
    appendFingerprint(body, record.runtimePairBootstrapContent);
    appendBool(body, record.invocationCleanupStarted);
    appendBool(body, record.invocationCleanupComplete);
    appendU32(body, static_cast<std::uint32_t>(record.items.size()));
    for (const auto& item : record.items) appendItem(body, item);
}

}  // namespace

std::vector<std::uint8_t> EncodeWindowsBootstrapJournal(
    const WindowsBootstrapJournalRecord& record) {
    if (!supportedSchema(record.schemaVersion) || record.generation == 0U ||
        record.items.size() > kMaximumItems || !safeIdentifier(record.transactionId, 128U) ||
        !safeIdentifier(record.installationId, 128U) ||
        !safeAbsoluteRoot(record.targetRoot) ||
        !safeAbsoluteRoot(record.updaterOwnedBase) ||
        !safeAbsoluteRoot(record.stagingRoot) ||
        !safeAbsoluteRoot(record.transactionRoot) ||
        record.protectedDataRoots.size() > kMaximumProtectedDataRoots ||
        !std::all_of(record.protectedDataRoots.begin(),
                     record.protectedDataRoots.end(),
                     [](const std::string& root) { return safeAbsoluteRoot(root); }) ||
        !safeIdentifier(record.invocationIdentity, 256U) ||
        !safeIdentifier(record.version, 128U) ||
        !safeIdentifier(record.buildId, 128U) ||
        !safeDigestOrEmpty(record.packagePlanDigest) ||
        (!record.packageKeyId.empty() && !safeIdentifier(record.packageKeyId, 64U)) ||
        (!record.packageChannel.empty() &&
         (record.packageChannel != "stable" && record.packageChannel != "testing")) ||
        !safeSignedPackageReference(record) ||
        !safeDigestOrEmpty(record.healthNonceDigest) ||
        !validJournalAuthenticationKey(record.healthToken) ||
        !safeProcessIdentity(record.oldProcessIdentity) ||
        !safeProcessIdentity(record.parentProcessIdentity) ||
        !safeProcessIdentity(record.updatedChildIdentity) ||
        !safeRelativePath(record.entrypoint) || !safePhase(record.phase) ||
        (!record.healthPhase.empty() && !safePhase(record.healthPhase))) {
        return {};
    }
    WindowsBootstrapJournalRecord durable = record;
    durable.healthNonceDigest = durable.healthNonceDigest.empty()
        ? BootstrapSha256Hex(durable.healthToken)
        : durable.healthNonceDigest;
    durable.healthToken.clear();
    if (!strictAuthorityFields(durable)) return {};
    for (const auto& item : record.items) {
        if (!validItem(item, record.schemaVersion)) return {};
    }
    if (!validRecordState(record, false) || !validAuxiliaryNames(record)) return {};
    std::vector<std::uint8_t> output;
    output.reserve(1024U);
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    appendU32(output, durable.schemaVersion);
    appendU64(output, durable.generation);
    const std::size_t bodyLengthOffset = output.size();
    appendU32(output, 0U);
    std::vector<std::uint8_t> body;
    appendRecordBody(body, durable);
    if (body.size() > kMaximumJournalBytes || body.size() > UINT32_MAX) return {};
    const std::uint32_t bodyLength = static_cast<std::uint32_t>(body.size());
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        output[bodyLengthOffset + shift / 8U] =
            static_cast<std::uint8_t>(bodyLength >> shift);
    }
    output.insert(output.end(), body.begin(), body.end());
    appendU64(output, fnv1a64(output.data(), output.size()));
    const auto authentication = journalAuthenticationTag(
        record.healthToken, output.data(), output.size());
    output.insert(output.end(), authentication.begin(), authentication.end());
    return output.size() <= kMaximumJournalBytes ? output : std::vector<std::uint8_t>{};
}

WindowsBootstrapJournalDecodeResult DecodeWindowsBootstrapJournal(
    const std::vector<std::uint8_t>& bytes,
    const ValidatedInstallPlan* expectedPlan,
    const ReplacementJournal* expectedJournal,
    const WindowsBootstrapJournalRootIdentities* expectedRoots,
    const bool allowParentProcessRebind,
    const WindowsBootstrapJournalPackageAuthority* packageAuthority) {
    if (bytes.empty()) return failure(WindowsBootstrapJournalDecodeStatus::Empty, 0U);
    if (bytes.size() > kMaximumJournalBytes) {
        return failure(WindowsBootstrapJournalDecodeStatus::TooLarge, 0U);
    }
    constexpr std::size_t kHeaderBytes = kMagic.size() + 4U + 8U + 4U;
    constexpr std::size_t kTrailerBytes =
        sizeof(std::uint64_t) + kJournalAuthenticationTagBytes;
    if (bytes.size() < kHeaderBytes + kTrailerBytes) {
        return failure(WindowsBootstrapJournalDecodeStatus::Truncated, 0U);
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return failure(WindowsBootstrapJournalDecodeStatus::InvalidMagic, 0U);
    }
    Reader header(bytes);
    std::uint8_t ignored = 0;
    for (std::size_t index = 0; index < kMagic.size(); ++index) {
        if (!header.takeU8(ignored)) return failure(WindowsBootstrapJournalDecodeStatus::Truncated, index);
    }
    std::uint32_t version = 0;
    std::uint64_t generation = 0;
    std::uint32_t bodyLength = 0;
    if (!header.takeU32(version) || !header.takeU64(generation) ||
        !header.takeU32(bodyLength) || !supportedSchema(version) || generation == 0U) {
        return failure(supportedSchema(version)
                           ? WindowsBootstrapJournalDecodeStatus::InvalidField
                           : WindowsBootstrapJournalDecodeStatus::UnsupportedVersion,
                       1U);
    }
    if (bodyLength > kMaximumJournalBytes ||
        kHeaderBytes + static_cast<std::size_t>(bodyLength) + kTrailerBytes !=
            bytes.size()) {
        return failure(WindowsBootstrapJournalDecodeStatus::Truncated, 2U);
    }
    std::uint64_t encodedChecksum = 0;
    const std::size_t authenticationOffset =
        bytes.size() - kJournalAuthenticationTagBytes;
    const std::size_t checksumOffset =
        authenticationOffset - sizeof(std::uint64_t);
    for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
        encodedChecksum |= static_cast<std::uint64_t>(bytes[checksumOffset + shift / 8U]) << shift;
    }
    if (encodedChecksum != fnv1a64(bytes.data(), checksumOffset)) {
        return failure(WindowsBootstrapJournalDecodeStatus::InvalidChecksum, 3U);
    }
    if (expectedPlan != nullptr) {
        if (!validJournalAuthenticationKey(expectedPlan->healthToken)) {
            return failure(
                WindowsBootstrapJournalDecodeStatus::InvalidAuthentication,
                3U);
        }
        const auto expectedAuthentication = journalAuthenticationTag(
            expectedPlan->healthToken, bytes.data(), authenticationOffset);
        if (!constantTimeTagEquals(expectedAuthentication,
                                   bytes.data() + authenticationOffset)) {
            return failure(
                WindowsBootstrapJournalDecodeStatus::InvalidAuthentication,
                3U);
        }
    }

    std::vector<std::uint8_t> body(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(checksumOffset));
    Reader reader(body);
    WindowsBootstrapJournalRecord record;
    record.schemaVersion = version;
    record.generation = generation;
    std::uint32_t deploymentMode = 0U;
    if (!reader.takeString(record.transactionId, 128U, false) ||
        !reader.takeU32(deploymentMode) || deploymentMode > 1U ||
        !reader.takeString(record.installationId, 128U, false) ||
        !reader.takeString(record.registeredAppId, 128U) ||
        !reader.takeString(record.targetRoot, kMaximumPathBytes, false) ||
        !reader.takeDeploymentFileIdentity(record.targetRootIdentity) ||
        !reader.takeDeploymentFileIdentity(record.oldExecutableIdentity) ||
        !reader.takeString(record.updaterOwnedBase, kMaximumPathBytes, false) ||
        !reader.takeString(record.stagingRoot, kMaximumPathBytes, false) ||
        !reader.takeString(record.transactionRoot, kMaximumPathBytes, false)) {
        return failure(WindowsBootstrapJournalDecodeStatus::InvalidField, 4U);
    }
    record.deploymentMode = static_cast<aris::platform::DeploymentMode>(deploymentMode);
    std::uint32_t protectedRootCount = 0U;
    if (!reader.takeU32(protectedRootCount) ||
        protectedRootCount > kMaximumProtectedDataRoots) {
        return failure(WindowsBootstrapJournalDecodeStatus::InvalidField, 4U);
    }
    record.protectedDataRoots.reserve(protectedRootCount);
    for (std::uint32_t index = 0U; index < protectedRootCount; ++index) {
        std::string root;
        if (!reader.takeString(root, kMaximumPathBytes, false)) {
            return failure(WindowsBootstrapJournalDecodeStatus::InvalidField, 4U);
        }
        record.protectedDataRoots.push_back(std::move(root));
    }
    if (!reader.takeU32(record.previousFailureCount) ||
        !reader.takeBool(record.failureRecordedForTransaction) ||
        !reader.takeString(record.invocationIdentity, 256U, false) ||
        !reader.takeString(record.version, 128U, false) ||
        !reader.takeString(record.buildId, 128U, false) ||
        !reader.takeString(record.packagePlanDigest, 64U) ||
        !reader.takeString(record.packageKeyId, 64U) ||
        !reader.takeString(record.packageChannel, 16U) ||
        !reader.takeString(record.packagePlanBytes, kMaximumPackagePlanBytes) ||
        !reader.takeString(record.packageSignatureHex, 128U) ||
        !reader.takeString(record.healthNonceDigest, 64U) ||
        !reader.takeU64(record.oldProcessId) ||
        !reader.takeProcessIdentity(record.oldProcessIdentity) ||
        !reader.takeProcessIdentity(record.parentProcessIdentity) ||
        !reader.takeProcessIdentity(record.updatedChildIdentity) ||
        !reader.takeProcessIdentity(record.previousApplicationIdentity) ||
        !reader.takeString(record.healthPhase, 128U) ||
        !reader.takeString(record.entrypoint, kMaximumPathBytes, false) ||
        !reader.takeString(record.phase, 128U, false) ||
        !safeIdentifier(record.transactionId, 128U) ||
        !safeIdentifier(record.installationId, 128U) ||
        !safeAbsoluteRoot(record.targetRoot) ||
        !safeAbsoluteRoot(record.updaterOwnedBase) ||
        !safeAbsoluteRoot(record.stagingRoot) ||
        !safeAbsoluteRoot(record.transactionRoot) ||
        record.protectedDataRoots.size() > kMaximumProtectedDataRoots ||
        !std::all_of(record.protectedDataRoots.begin(),
                     record.protectedDataRoots.end(),
                     [](const std::string& root) { return safeAbsoluteRoot(root); }) ||
        !safeIdentifier(record.invocationIdentity, 256U) ||
        !safeIdentifier(record.version, 128U) ||
        !safeIdentifier(record.buildId, 128U) ||
        !safeDigestOrEmpty(record.packagePlanDigest) ||
        (!record.packageKeyId.empty() && !safeIdentifier(record.packageKeyId, 64U)) ||
        (!record.packageChannel.empty() && record.packageChannel != "stable" &&
         record.packageChannel != "testing") ||
        !safeSignedPackageReference(record) ||
        !safeDigestOrEmpty(record.healthNonceDigest) ||
        !safeProcessIdentity(record.oldProcessIdentity) ||
        !safeProcessIdentity(record.parentProcessIdentity) ||
        !safeProcessIdentity(record.updatedChildIdentity) ||
        !safeProcessIdentity(record.previousApplicationIdentity) ||
        !safeRelativePath(record.entrypoint) || !safePhase(record.phase) ||
        (!record.healthPhase.empty() && !safePhase(record.healthPhase)) ||
        !strictAuthorityFields(record)) {
        return failure(WindowsBootstrapJournalDecodeStatus::InvalidField, 4U);
    }
    if (!reader.takeIdentity(record.installRootIdentity) ||
        !reader.takeIdentity(record.stagingRootIdentity) ||
        !reader.takeIdentity(record.transactionRootIdentity) ||
        !reader.takeIdentity(record.backupRootIdentity) ||
        !reader.takeIdentity(record.journalIdentity) ||
        !reader.takeFingerprint(record.journalContent) ||
        !reader.takeBool(record.initialJournalDurable) ||
        !reader.takeBool(record.backupDirectoryStarted) ||
        !reader.takeBool(record.backupDirectoryComplete) ||
        !reader.takeBool(record.commitComplete) ||
        !reader.takeBool(record.rollbackDurable) ||
        !reader.takeBool(record.rollbackComplete) ||
        !reader.takeBool(record.backupDirectoryRemoveStarted) ||
        !reader.takeBool(record.backupDirectoryRemoved) ||
        !reader.takeBool(record.healthTokenConsumed) ||
        !reader.takeBool(record.committedApplicationRelaunchPending) ||
        !reader.takeBool(record.previousApplicationRelaunchStarted) ||
        !reader.takeBool(record.previousApplicationRelaunchComplete) ||
        !reader.takeBool(record.runtimePairExpected) ||
        !reader.takeIdentity(record.runtimePairDirectoryIdentity) ||
        !reader.takeIdentity(record.runtimePairBootstrapIdentity) ||
        !reader.takeFingerprint(record.runtimePairBootstrapContent) ||
        !reader.takeBool(record.invocationCleanupStarted) ||
        !reader.takeBool(record.invocationCleanupComplete)) {
        return failure(WindowsBootstrapJournalDecodeStatus::InvalidField, 5U);
    }
    std::uint32_t itemCount = 0;
    if (!reader.takeU32(itemCount) || itemCount > kMaximumItems) {
        return failure(WindowsBootstrapJournalDecodeStatus::InvalidItem, 6U);
    }
    record.items.reserve(itemCount);
    std::set<std::string> paths;
    for (std::uint32_t index = 0; index < itemCount; ++index) {
        WindowsBootstrapJournalItemRecord item;
        if (!readItem(reader, item, version)) {
            return failure(WindowsBootstrapJournalDecodeStatus::InvalidItem, index);
        }
        std::string folded = item.relativePath;
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](const unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        if (!paths.insert(folded).second) {
            return failure(WindowsBootstrapJournalDecodeStatus::DuplicateItem, index);
        }
        record.items.push_back(std::move(item));
    }
    if (reader.remaining() != 0U) {
        return failure(WindowsBootstrapJournalDecodeStatus::InvalidField,
                       reader.offset());
    }
    if (!validRecordState(record, true)) {
        return failure(WindowsBootstrapJournalDecodeStatus::InvalidField, 10U);
    }
    if (!validAuxiliaryNames(record)) {
        return failure(WindowsBootstrapJournalDecodeStatus::InvalidItem, 10U);
    }
    if (expectedPlan != nullptr &&
        !samePlan(record, *expectedPlan, allowParentProcessRebind)) {
        return failure(WindowsBootstrapJournalDecodeStatus::PlanMismatch, 7U);
    }
    if (packageAuthority != nullptr) {
        if (packageAuthority->trust == nullptr || expectedPlan == nullptr) {
            return failure(WindowsBootstrapJournalDecodeStatus::InvalidPackageAuthority,
                           11U);
        }
        const DurablePackageAuthorityResult authority =
            ValidateDurablePackageAuthority(
                *expectedPlan, record.packagePlanBytes,
                record.packageSignatureHex, *packageAuthority->trust,
                packageAuthority->verifier);
        if (!authority.ok()) {
            return failure(WindowsBootstrapJournalDecodeStatus::InvalidPackageAuthority,
                           11U);
        }
    }
    if (expectedJournal != nullptr && !sameJournal(record, *expectedJournal)) {
        return failure(WindowsBootstrapJournalDecodeStatus::JournalMismatch, 8U);
    }
    if (expectedRoots != nullptr &&
        (!sameIdentityIfBound(record.installRootIdentity, expectedRoots->install) ||
         !sameIdentityIfBound(record.stagingRootIdentity, expectedRoots->staging) ||
         !sameIdentityIfBound(record.transactionRootIdentity, expectedRoots->transaction) ||
         !sameIdentityIfBound(record.backupRootIdentity, expectedRoots->backup))) {
        return failure(WindowsBootstrapJournalDecodeStatus::RootMismatch, 9U);
    }
    WindowsBootstrapJournalDecodeResult result;
    result.status = WindowsBootstrapJournalDecodeStatus::Ok;
    result.record = std::move(record);
    return result;
}

WindowsBootstrapJournalSelection SelectDurableWindowsBootstrapJournal(
    const std::vector<WindowsBootstrapJournalCandidate>& candidates,
    const ValidatedInstallPlan* expectedPlan,
    const ReplacementJournal* expectedJournal,
    const WindowsBootstrapJournalRootIdentities* expectedRoots,
    const bool allowParentProcessRebind,
    const WindowsBootstrapJournalPackageAuthority* packageAuthority) {
    WindowsBootstrapJournalSelection result;
    if (candidates.empty()) return result;
    std::uint64_t selectedGeneration = 0;
    struct DecodedCandidate {
        const WindowsBootstrapJournalCandidate* candidate = nullptr;
        WindowsBootstrapJournalRecord record;
    };
    std::vector<DecodedCandidate> decodedCandidates;
    std::vector<std::size_t> winners;
    for (const auto& candidate : candidates) {
        if (!candidate.durable) continue;
        const auto decoded = DecodeWindowsBootstrapJournal(
            candidate.bytes, expectedPlan, expectedJournal, expectedRoots,
            allowParentProcessRebind, packageAuthority);
        if (!decoded.ok()) {
            result.status = WindowsBootstrapJournalSelectionStatus::InvalidCandidate;
            return result;
        }
        // Candidate observations are still required to carry an identity and
        // content binding.  Slot parity is checked for the selected winner
        // below; retaining lower-generation observations with a mismatched
        // slot lets the selector report the precise ambiguity/invalid-winner
        // result instead of silently discarding evidence.
        if (!candidate.identity.valid || !candidate.content.valid) {
            result.status = WindowsBootstrapJournalSelectionStatus::InvalidCandidate;
            return result;
        }
        decodedCandidates.push_back(
            DecodedCandidate{&candidate, *decoded.record});
        if (selectedGeneration == 0U ||
            decoded.record->generation > selectedGeneration) {
            selectedGeneration = decoded.record->generation;
            winners.clear();
        }
        if (decoded.record->generation == selectedGeneration) {
            winners.push_back(decodedCandidates.size() - 1U);
        }
    }
    if (winners.empty()) return result;
    if (winners.size() != 1U) {
        result.status = WindowsBootstrapJournalSelectionStatus::Ambiguous;
        return result;
    }
    const DecodedCandidate& winner = decodedCandidates[winners.front()];
    const int winnerSlot = journalSlotIndex(*winner.candidate);
    const int expectedWinnerSlot =
        winner.record.generation % 2U == 0U ? 1 : 0;
    if (winnerSlot != expectedWinnerSlot) {
        result.status = WindowsBootstrapJournalSelectionStatus::InvalidCandidate;
        return result;
    }
    // A higher generation is recoverable only when its exact prior durable
    // slot is still present and its observed identity/content match the
    // predecessor binding embedded in the record.  The two-slot protocol
    // intentionally retains only the immediate predecessor; older links are
    // no longer present after a normal sequence of publications.
    if (winner.record.generation > 1U) {
        std::vector<const DecodedCandidate*> predecessors;
        const std::uint64_t predecessorGeneration =
            winner.record.generation - 1U;
        for (const auto& candidate : decodedCandidates) {
            if (candidate.record.generation == predecessorGeneration) {
                predecessors.push_back(&candidate);
            }
        }
        // Terminal retirement removes the inactive predecessor before the
        // winner.  A crash in that narrow interval may therefore leave one
        // durable terminal slot.  It is safe to resume only as a no-mutation
        // terminal record after the caller has authenticated the exact plan
        // capability and rebound the exact roots; never grant this exception
        // to an unbound corruption-only decode or while any other candidate
        // remains ambiguous.
        const bool authenticatedTerminalRetirementCut =
            decodedCandidates.size() == 1U && predecessors.empty() &&
            expectedPlan != nullptr && expectedRoots != nullptr &&
            !winner.candidate->temporary &&
            winner.record.backupDirectoryRemoved &&
            (winner.record.commitComplete ||
             (winner.record.rollbackComplete &&
              winner.record.previousApplicationRelaunchComplete));
        if (!authenticatedTerminalRetirementCut &&
            (predecessors.size() != 1U ||
            predecessors.front()->candidate == nullptr ||
            predecessors.front()->candidate->temporary ||
            journalSlotIndex(*predecessors.front()->candidate) == winnerSlot ||
            !predecessors.front()->candidate->identity.valid ||
            !predecessors.front()->candidate->content.valid ||
            winner.record.journalIdentity !=
                predecessors.front()->candidate->identity ||
            winner.record.journalContent !=
                predecessors.front()->candidate->content)) {
            result.status = WindowsBootstrapJournalSelectionStatus::InvalidCandidate;
            return result;
        }
    }
    result.status = WindowsBootstrapJournalSelectionStatus::Selected;
    result.selectedName =
        decodedCandidates[winners.front()].candidate->name;
    result.record = std::move(decodedCandidates[winners.front()].record);
    return result;
}

}  // namespace aris::bootstrap
