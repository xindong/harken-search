#include "bootstrap/HarkenSearchBootstrapAdmission.h"

#include "bootstrap/HarkenSearchBootstrapRuntime.h"
#include "bootstrap/WindowsBootstrapFilesystem.h"

#include "ed25519/tweetnacl.h"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aris::bootstrap {
namespace {

BootstrapAdmission admitBootstrapInvocation(
    const PackagePlanAdmission& package,
    const InvocationEnvelope& invocation);

using json = nlohmann::json;
constexpr std::size_t kMaximumPackageBytes = 1024U * 1024U;
constexpr std::size_t kMaximumInvocationBytes = 64U * 1024U;
constexpr std::size_t kMaximumInventoryItems = 4096U;
constexpr std::size_t kMaximumProtectedDataRoots = 256U;
constexpr std::size_t kMaximumAbsolutePathBytes = 32768U;
constexpr std::uint32_t kMaximumFailureCount = 2U;

bool isLowerHex(std::string_view value, const std::size_t length) {
    if (value.size() != length) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

int hexNibble(const char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

bool decodeHex(std::string_view text, std::vector<unsigned char>& output) {
    if (text.size() % 2U != 0U) return false;
    output.resize(text.size() / 2U);
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = hexNibble(text[index * 2U]);
        const int low = hexNibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            output.clear();
            return false;
        }
        output[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

bool isStrictEd25519PublicKey(std::string_view text) {
    if (!isLowerHex(text, 64U)) return false;
    std::vector<unsigned char> key;
    if (!decodeHex(text, key) || key.size() != crypto_sign_PUBLICKEYBYTES) {
        return false;
    }

    // Compressed Edwards y is little-endian; bit 255 is the x sign. Reject
    // non-canonical y >= 2^255-19 before TweetNaCl's permissive decoder can
    // reduce it to another point.
    key.back() &= 0x7fU;
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> fieldPrime{};
    fieldPrime.fill(0xffU);
    fieldPrime.front() = 0xedU;
    fieldPrime.back() = 0x7fU;
    bool lessThanPrime = false;
    bool differsFromPrime = false;
    for (std::size_t index = key.size(); index-- > 0U;) {
        if (key[index] == fieldPrime[index]) continue;
        lessThanPrime = key[index] < fieldPrime[index];
        differsFromPrime = true;
        break;
    }
    if (!differsFromPrime || !lessThanPrime) return false;

    // Complete low-order blacklist used by strict Ed25519 implementations.
    // The input sign bit was cleared above, so alternate-sign encodings are
    // rejected by the same comparison (including x=0 with sign=1).
    constexpr std::array<std::string_view, 5U> smallOrder = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eaa78aad612",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    };
    for (const std::string_view encoded : smallOrder) {
        std::vector<unsigned char> lowOrder;
        if (!decodeHex(encoded, lowOrder)) return false;
        if (key == lowOrder) return false;
    }
    return true;
}

bool safeIdentifier(std::string_view value, const std::size_t minimum,
                    const std::size_t maximum) {
    if (value.size() < minimum || value.size() > maximum) return false;
    if (value.front() == '~' || value.back() == '~') return false;
    if (std::isalnum(static_cast<unsigned char>(value.front())) == 0 ||
        std::isalnum(static_cast<unsigned char>(value.back())) == 0) {
        return false;
    }
    for (const unsigned char character : value) {
        if (!(std::isalnum(character) || character == '.' || character == '_' ||
              character == '-')) {
            return false;
        }
    }
    return true;
}

bool safeSemver(std::string_view value) {
    if (value.empty() || value.size() > 64U) return false;
    std::size_t begin = 0;
    for (int component = 0; component < 3; ++component) {
        const std::size_t end = component == 2 ? value.size() : value.find('.', begin);
        if (end == std::string_view::npos || end == begin) return false;
        const std::string_view part = value.substr(begin, end - begin);
        if (part.size() > 1U && part.front() == '0') return false;
        for (const char digit : part) {
            if (digit < '0' || digit > '9') return false;
        }
        begin = end + 1U;
    }
    return begin == value.size() + 1U;
}

bool hasOnlyKeys(const json& value,
                 std::initializer_list<const char*> expectedKeys) {
    if (!value.is_object() || value.size() != expectedKeys.size()) return false;
    const std::unordered_set<std::string> expected(expectedKeys.begin(),
                                                   expectedKeys.end());
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        if (expected.find(iterator.key()) == expected.end()) return false;
    }
    return true;
}

bool isString(const json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string();
}

bool looksAbsoluteWindowsPath(std::string_view value) {
    if (value.empty() || value.size() > kMaximumAbsolutePathBytes) return false;
    const bool driveRoot = value.size() >= 3U &&
        ((value[0] >= 'A' && value[0] <= 'Z') ||
         (value[0] >= 'a' && value[0] <= 'z')) &&
        value[1] == ':' && (value[2] == '/' || value[2] == '\\');
    const bool uncRoot = value.size() >= 3U && value[0] == '\\' &&
        value[1] == '\\' && value[2] != '\\';
    if (!driveRoot && !uncRoot) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char c) {
        return c >= 0x20U && c != '"' && c != '<' && c != '>' && c != '|';
    });
}

bool safeRelativeLeaf(std::string_view value) {
    if (value.empty() || value.size() > 4096U || value.front() == '/' ||
        value.front() == '\\' || value.back() == '/' || value.back() == '\\' ||
        value.find(':') != std::string_view::npos ||
        value.find('~') != std::string_view::npos ||
        value.find("//") != std::string_view::npos ||
        value.find("\\\\") != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0U;
    while (begin < value.size()) {
        const std::size_t end = value.find_first_of("/\\", begin);
        const std::string_view component = value.substr(
            begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        if (component.empty() || component == "." || component == "..") return false;
        for (const unsigned char character : component) {
            if (character < 0x20U || character == '"' || character == '<' ||
                character == '>' || character == ':' || character == '|' ||
                character == '?' || character == '*') {
                return false;
            }
        }
        if (component.back() == '.' || component.back() == ' ') return false;
        begin = end == std::string_view::npos ? value.size() : end + 1U;
    }
    return true;
}

bool validProcessIdentity(const BootstrapProcessIdentity& value) {
    return value.valid && value.processId != 0U && value.creationTime != 0U &&
           value.processId <= std::numeric_limits<std::uint32_t>::max() &&
           value.imageVolumeSerial != 0U &&
           looksAbsoluteWindowsPath(value.imagePath) &&
           std::any_of(value.imageFileId.begin(), value.imageFileId.end(),
                       [](const std::uint8_t byte) { return byte != 0U; });
}

std::string normalizePathText(std::string_view value) {
    if (value.rfind("\\\\?\\", 0U) == 0U) value.remove_prefix(4U);
    std::string normalized;
    normalized.reserve(value.size());
    for (char character : value) {
        if (character == '\\') character = '/';
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        normalized.push_back(character);
    }
    while (normalized.size() > 3U && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

bool pathsOverlap(const std::string_view left, const std::string_view right) {
    const std::string normalizedLeft = normalizePathText(left);
    const std::string normalizedRight = normalizePathText(right);
    const auto containsRoot = [](const std::string& root,
                                 const std::string& candidate) {
        return candidate == root ||
            (candidate.size() > root.size() &&
             candidate.compare(0U, root.size(), root) == 0 &&
             candidate[root.size()] == '/');
    };
    return containsRoot(normalizedLeft, normalizedRight) ||
           containsRoot(normalizedRight, normalizedLeft);
}

bool trustedInvocationProcessPaths(const InvocationEnvelope& invocation) {
    // The parent image is dynamic authority supplied by the authenticated
    // one-shot channel.  Its exact PID/creation time/path/volume/FileId and
    // production signer are checked by the native channel; the pure value
    // boundary must not invent a product filename that rejects a legitimate
    // updater host (for example the installed Aris process itself).
    if (!validProcessIdentity(invocation.parentProcess) ||
        !validProcessIdentity(invocation.oldProcess)) {
        return false;
    }
    return !normalizePathText(invocation.parentProcess.imagePath).empty();
}

bool validInvocationEnvelope(const InvocationEnvelope& invocation) {
    const bool installed =
        invocation.deploymentMode == aris::platform::DeploymentMode::Installed;
    const bool portable =
        invocation.deploymentMode == aris::platform::DeploymentMode::Portable;
    if (!safeIdentifier(invocation.transactionId, 1U, 128U) ||
        !safeIdentifier(invocation.installationId, 1U, 128U) ||
        (!installed && !portable) ||
        (installed && invocation.installationId !=
            "installed-a8b3c9d1-e4f5-4a6b-8c9d-0e1f2a3b4c5d") ||
        (portable && invocation.installationId.rfind("portable-", 0U) != 0U) ||
        (installed && invocation.registeredAppId != aris::platform::kArisInstalledAppId) ||
        (portable && !invocation.registeredAppId.empty()) ||
        !safeIdentifier(invocation.invocationIdentity, 1U, 256U) ||
        !isLowerHex(invocation.packagePlanDigest, 64U) ||
        !isLowerHex(invocation.healthNonce, 64U) ||
        invocation.previousFailureCount > kMaximumFailureCount ||
        invocation.oldProcessId == 0U ||
        invocation.oldProcessId > std::numeric_limits<std::uint32_t>::max() ||
        invocation.entrypoint.empty() ||
        invocation.entrypoint.size() > kMaximumAbsolutePathBytes ||
        !looksAbsoluteWindowsPath(invocation.targetRoot) ||
        !looksAbsoluteWindowsPath(invocation.updaterOwnedBase) ||
        !looksAbsoluteWindowsPath(invocation.stagingRoot) ||
        !looksAbsoluteWindowsPath(invocation.transactionRoot) ||
        !validProcessIdentity(invocation.parentProcess) ||
        !validProcessIdentity(invocation.oldProcess) ||
        !invocation.targetRootIdentity.valid ||
        !invocation.oldExecutableIdentity.valid ||
        normalizePathText(invocation.targetRootIdentity.canonicalPath) !=
            normalizePathText(invocation.targetRoot) ||
        normalizePathText(invocation.oldExecutableIdentity.canonicalPath) !=
            normalizePathText(invocation.oldProcess.imagePath) ||
        invocation.oldExecutableIdentity.volumeSerial !=
            invocation.oldProcess.imageVolumeSerial ||
        invocation.oldExecutableIdentity.fileId != invocation.oldProcess.imageFileId ||
        invocation.oldProcess.processId != invocation.oldProcessId ||
        !trustedInvocationProcessPaths(invocation) ||
        !safeRelativeLeaf(invocation.entrypoint) ||
        invocation.protectedDataRoots.empty() ||
        invocation.protectedDataRoots.size() > kMaximumProtectedDataRoots ||
        invocation.existingInventory.size() > kMaximumInventoryItems) {
        return false;
    }
    for (const std::string& root : invocation.protectedDataRoots) {
        if (!looksAbsoluteWindowsPath(root) ||
            pathsOverlap(root, invocation.targetRoot) ||
            pathsOverlap(root, invocation.updaterOwnedBase) ||
            pathsOverlap(root, invocation.stagingRoot) ||
            pathsOverlap(root, invocation.transactionRoot)) {
            return false;
        }
    }
    for (std::size_t left = 0U; left < invocation.protectedDataRoots.size(); ++left) {
        for (std::size_t right = left + 1U;
             right < invocation.protectedDataRoots.size(); ++right) {
            if (pathsOverlap(invocation.protectedDataRoots[left],
                             invocation.protectedDataRoots[right])) {
                return false;
            }
        }
    }
    for (const ExistingInstallItem& item : invocation.existingInventory) {
        if (!safeRelativeLeaf(item.relativePath)) {
            return false;
        }
    }
    return true;
}

// Small self-contained SHA-256 implementation.  It is used only for binding
// exact package bytes; the FNV checksum in the filesystem journal remains
// corruption detection and is never authority.
class Sha256 {
public:
    Sha256() : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            block_[blockLength_++] = byte;
            if (blockLength_ == block_.size()) {
                transform();
                bitLength_ += 512U;
                blockLength_ = 0;
            }
        }
    }

    std::array<unsigned char, 32> finish() {
        const std::uint64_t originalBits = bitLength_ +
            static_cast<std::uint64_t>(blockLength_) * 8U;
        block_[blockLength_++] = 0x80U;
        if (blockLength_ > 56U) {
            while (blockLength_ < 64U) block_[blockLength_++] = 0;
            transform();
            blockLength_ = 0;
        }
        while (blockLength_ < 56U) block_[blockLength_++] = 0;
        for (int index = 7; index >= 0; --index) {
            block_[blockLength_++] = static_cast<unsigned char>(
                (originalBits >> (index * 8)) & 0xffU);
        }
        transform();

        std::array<unsigned char, 32> output{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            output[index * 4U] = static_cast<unsigned char>(state_[index] >> 24U);
            output[index * 4U + 1U] = static_cast<unsigned char>(state_[index] >> 16U);
            output[index * 4U + 2U] = static_cast<unsigned char>(state_[index] >> 8U);
            output[index * 4U + 3U] = static_cast<unsigned char>(state_[index]);
        }
        return output;
    }

private:
    static std::uint32_t rotateRight(const std::uint32_t value, const int count) {
        return (value >> count) | (value << (32 - count));
    }

    void transform() {
        static constexpr std::uint32_t constants[] = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
        std::uint32_t words[64]{};
        for (std::size_t index = 0; index < 16U; ++index) {
            words[index] = (static_cast<std::uint32_t>(block_[index * 4U]) << 24U) |
                (static_cast<std::uint32_t>(block_[index * 4U + 1U]) << 16U) |
                (static_cast<std::uint32_t>(block_[index * 4U + 2U]) << 8U) |
                static_cast<std::uint32_t>(block_[index * 4U + 3U]);
        }
        for (std::size_t index = 16U; index < 64U; ++index) {
            const std::uint32_t s0 = rotateRight(words[index - 15U], 7) ^
                rotateRight(words[index - 15U], 18) ^ (words[index - 15U] >> 3U);
            const std::uint32_t s1 = rotateRight(words[index - 2U], 17) ^
                rotateRight(words[index - 2U], 19) ^ (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (std::size_t index = 0; index < 64U; ++index) {
            const std::uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^
                rotateRight(e, 25);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + choose + constants[index] + words[index];
            const std::uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^
                rotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
        bitLength_ += 0; // document that the caller advances full-block length
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<unsigned char, 64> block_{};
    std::size_t blockLength_ = 0;
    std::uint64_t bitLength_ = 0;
};

std::string digestHex(std::string_view bytes) {
    Sha256 hash;
    hash.update(bytes);
    const auto digest = hash.finish();
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(64U);
    for (const unsigned char value : digest) {
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }
    return output;
}

bool verifyEd25519(std::string_view message, std::string_view signatureHex,
                   std::string_view publicKeyHex, std::string* diagnostic) {
    std::vector<unsigned char> signature;
    std::vector<unsigned char> publicKey;
    if (!decodeHex(signatureHex, signature) ||
        signature.size() != crypto_sign_BYTES ||
        !decodeHex(publicKeyHex, publicKey) ||
        publicKey.size() != crypto_sign_PUBLICKEYBYTES) {
        if (diagnostic != nullptr) *diagnostic = "signature/key encoding";
        return false;
    }
    std::vector<unsigned char> signedMessage;
    signedMessage.reserve(signature.size() + message.size());
    signedMessage.insert(signedMessage.end(), signature.begin(), signature.end());
    signedMessage.insert(signedMessage.end(), message.begin(), message.end());
    std::vector<unsigned char> opened(signedMessage.size());
    unsigned long long openedLength = 0;
    const int status = crypto_sign_open(
        opened.data(), &openedLength, signedMessage.data(),
        static_cast<unsigned long long>(signedMessage.size()), publicKey.data());
    if (status != 0 || openedLength != message.size() ||
        !std::equal(message.begin(), message.end(), opened.begin())) {
        if (diagnostic != nullptr) *diagnostic = "signature rejected";
        return false;
    }
    return true;
}

bool parseProcessIdentity(const json& value, BootstrapProcessIdentity& output) {
    if (!hasOnlyKeys(value, {"creation_time", "file_id", "image_path",
                             "image_volume_serial", "process_id"}) ||
        !value["creation_time"].is_number_unsigned() ||
        !value["image_volume_serial"].is_number_unsigned() ||
        !value["process_id"].is_number_unsigned() ||
        !isString(value, "file_id") || !isString(value, "image_path")) {
        return false;
    }
    const std::string fileId = value["file_id"].get<std::string>();
    const std::string path = value["image_path"].get<std::string>();
    if (!isLowerHex(fileId, 32U) || path.empty() || path.size() > 4096U ||
        value["process_id"].get<std::uint64_t>() == 0U ||
        value["creation_time"].get<std::uint64_t>() == 0U ||
        value["image_volume_serial"].get<std::uint64_t>() == 0U) {
        return false;
    }
    std::vector<unsigned char> decoded;
    if (!decodeHex(fileId, decoded) || decoded.size() != output.imageFileId.size()) {
        return false;
    }
    std::copy(decoded.begin(), decoded.end(), output.imageFileId.begin());
    output.processId = value["process_id"].get<std::uint64_t>();
    output.creationTime = value["creation_time"].get<std::uint64_t>();
    output.imageVolumeSerial = value["image_volume_serial"].get<std::uint64_t>();
    output.imagePath = path;
    output.valid = true;
    return true;
}

bool parseDeploymentFileIdentity(
    const json& value, aris::platform::DeploymentFileIdentity& output) {
    if (!hasOnlyKeys(value, {"canonical_path", "file_id", "volume_serial"}) ||
        !isString(value, "canonical_path") || !isString(value, "file_id") ||
        !value["volume_serial"].is_number_unsigned()) {
        return false;
    }
    const std::string fileId = value["file_id"].get<std::string>();
    const std::string path = value["canonical_path"].get<std::string>();
    if (!isLowerHex(fileId, 32U) || !looksAbsoluteWindowsPath(path) ||
        value["volume_serial"].get<std::uint64_t>() == 0U) {
        return false;
    }
    std::vector<unsigned char> decoded;
    if (!decodeHex(fileId, decoded) || decoded.size() != output.fileId.size() ||
        !std::any_of(decoded.begin(), decoded.end(),
                     [](const unsigned char byte) { return byte != 0U; })) {
        return false;
    }
    output.canonicalPath = path;
    output.volumeSerial = value["volume_serial"].get<std::uint64_t>();
    std::copy(decoded.begin(), decoded.end(), output.fileId.begin());
    output.valid = true;
    return true;
}

}  // namespace

PackagePlanAdmission VerifyPackagePlan(
    const std::string_view packageBytes, const std::string_view signatureHex,
    const PackagePlanTrustConfig& trust, const PackageSignatureVerifier testVerifier) {
    PackagePlanAdmission result;
    if (packageBytes.empty() || packageBytes.size() > kMaximumPackageBytes) {
        result.status = PackagePlanAdmissionStatus::TooLarge;
        return result;
    }
    if (!isLowerHex(signatureHex, 128U)) {
        result.status = PackagePlanAdmissionStatus::InvalidSignatureEncoding;
        return result;
    }
    if (!trust.provisioned || !safeIdentifier(trust.keyId, 1U, 64U) ||
        !isLowerHex(trust.publicKeyHex, 64U)) {
        result.status = PackagePlanAdmissionStatus::UnprovisionedKey;
        return result;
    }
    if (!isStrictEd25519PublicKey(trust.publicKeyHex)) {
        result.status = PackagePlanAdmissionStatus::InvalidKey;
        result.diagnostic = "strict Ed25519 public key";
        return result;
    }

    std::string diagnostic;
    const bool signatureOk = testVerifier != nullptr
        ? testVerifier(packageBytes, signatureHex, trust.keyId, &diagnostic)
        : verifyEd25519(packageBytes, signatureHex, trust.publicKeyHex, &diagnostic);
    if (!signatureOk) {
        result.status = PackagePlanAdmissionStatus::SignatureRejected;
        result.diagnostic = diagnostic;
        return result;
    }

    const json root = json::parse(packageBytes, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        result.status = PackagePlanAdmissionStatus::InvalidJson;
        return result;
    }
    if (!hasOnlyKeys(root, {"asset_sha256", "asset_size", "build_id", "channel",
                            "entrypoint", "inventory", "key_id", "product",
                            "removals", "schema_version", "version"})) {
        result.status = PackagePlanAdmissionStatus::InvalidSchema;
        return result;
    }
    if (!root["schema_version"].is_number_unsigned() ||
        root["schema_version"].get<std::uint32_t>() != 1U ||
        !isString(root, "asset_sha256") || !root["asset_size"].is_number_unsigned() ||
        !isString(root, "build_id") || !isString(root, "channel") ||
        !isString(root, "entrypoint") || !root["inventory"].is_array() ||
        !isString(root, "key_id") || !isString(root, "product") ||
        !root["removals"].is_array() || !isString(root, "version")) {
        result.status = PackagePlanAdmissionStatus::InvalidField;
        return result;
    }

    PackagePlan plan;
    plan.schemaVersion = 1;
    plan.product = root["product"].get<std::string>();
    plan.channel = root["channel"].get<std::string>();
    plan.version = root["version"].get<std::string>();
    plan.buildId = root["build_id"].get<std::string>();
    plan.keyId = root["key_id"].get<std::string>();
    plan.assetSha256 = root["asset_sha256"].get<std::string>();
    plan.assetSize = root["asset_size"].get<std::uint64_t>();
    plan.entrypoint = root["entrypoint"].get<std::string>();
    if (plan.product != "aris" ||
        (plan.channel != "stable" && plan.channel != "testing") ||
        !safeSemver(plan.version) || !safeIdentifier(plan.buildId, 1U, 128U) ||
        plan.keyId != trust.keyId || !isLowerHex(plan.assetSha256, 64U) ||
        plan.assetSize == 0U || !isLowerHex(digestHex(packageBytes), 64U) ||
        !safeIdentifier(plan.keyId, 1U, 64U)) {
        result.status = PackagePlanAdmissionStatus::InvalidField;
        return result;
    }
    if (plan.entrypoint.empty() || plan.entrypoint.size() > 4096U ||
        root["inventory"].size() == 0U ||
        root["inventory"].size() > kMaximumInventoryItems) {
        result.status = PackagePlanAdmissionStatus::InvalidInventory;
        return result;
    }
    std::set<std::string> paths;
    for (const auto& rawItem : root["inventory"]) {
        if (!hasOnlyKeys(rawItem, {"byte_size", "relative_path", "sha256"}) ||
            !rawItem["byte_size"].is_number_unsigned() ||
            !isString(rawItem, "relative_path") || !isString(rawItem, "sha256")) {
            result.status = PackagePlanAdmissionStatus::InvalidInventory;
            return result;
        }
        InstallInventoryItem item;
        item.relativePath = rawItem["relative_path"].get<std::string>();
        item.sha256 = rawItem["sha256"].get<std::string>();
        item.byteSize = rawItem["byte_size"].get<std::uint64_t>();
        if (!isLowerHex(item.sha256, 64U) || item.relativePath.empty() ||
            item.relativePath.size() > 4096U ||
            !paths.insert(item.relativePath).second) {
            result.status = PackagePlanAdmissionStatus::InvalidInventory;
            return result;
        }
        plan.inventory.push_back(std::move(item));
    }
    if (root["removals"].size() > kMaximumInventoryItems) {
        result.status = PackagePlanAdmissionStatus::InvalidInventory;
        return result;
    }
    std::set<std::string> foldedInventoryPaths;
    for (const InstallInventoryItem& item : plan.inventory) {
        std::string folded = item.relativePath;
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](const unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        foldedInventoryPaths.insert(std::move(folded));
    }
    std::set<std::string> foldedRemovalPaths;
    for (const auto& rawRemoval : root["removals"]) {
        if (!rawRemoval.is_string()) {
            result.status = PackagePlanAdmissionStatus::InvalidInventory;
            return result;
        }
        std::string path = rawRemoval.get<std::string>();
        if (!safeRelativeLeaf(path)) {
            result.status = PackagePlanAdmissionStatus::InvalidInventory;
            return result;
        }
        std::string folded = path;
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](const unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (!foldedRemovalPaths.insert(folded).second ||
            foldedInventoryPaths.find(folded) != foldedInventoryPaths.end()) {
            result.status = PackagePlanAdmissionStatus::InvalidInventory;
            return result;
        }
        plan.removals.push_back(std::move(path));
    }
    const auto entry = std::find_if(plan.inventory.begin(), plan.inventory.end(),
                                    [&plan](const InstallInventoryItem& item) {
                                        return item.relativePath == plan.entrypoint;
                                    });
    if (entry == plan.inventory.end()) {
        result.status = PackagePlanAdmissionStatus::InvalidInventory;
        return result;
    }
    result.status = PackagePlanAdmissionStatus::Accepted;
    result.plan = std::move(plan);
    result.packageDigest = digestHex(packageBytes);
    result.packageBytes.assign(packageBytes.data(), packageBytes.size());
    result.packageSignatureHex.assign(signatureHex.data(), signatureHex.size());
    return result;
}

struct VerifiedPackagePlanBinding::State {
    PackagePlanAdmission admission;
    std::string signatureDigest;
    std::vector<std::string> archiveEntries;
    std::vector<InstallInventoryItem> archiveItems;
};

namespace {

HarkenSearchPackagePolicyResult evaluateHarkenSearchPackagePolicy(
    const PackagePlanAdmission& admission) {
    HarkenSearchPackagePolicyResult result;
    if (!admission.ok() || !admission.plan.has_value() ||
        admission.packageBytes.empty() ||
        digestHex(admission.packageBytes) != admission.packageDigest) {
        result.status = HarkenSearchPackagePolicyStatus::InvalidBinding;
        result.diagnostic = "exact verified package bytes and digest";
        return result;
    }
    if (!admission.plan->removals.empty()) {
        result.status = HarkenSearchPackagePolicyStatus::RemovalsForbidden;
        result.diagnostic = "signed removals are not Harken Search user-data authority";
        return result;
    }

    static constexpr std::array<std::string_view, 10U> protectedExtensions{
        ".wav", ".mp3", ".flac", ".ogg", ".m4a", ".aac",
        ".wma", ".opus", ".aiff", ".aif"};
    for (const InstallInventoryItem& item : admission.plan->inventory) {
        const std::string path = normalizePathText(item.relativePath);
        const std::size_t slash = path.find_last_of('/');
        const std::string_view name(path.data() +
            (slash == std::string::npos ? 0U : slash + 1U),
            path.size() - (slash == std::string::npos ? 0U : slash + 1U));
        const auto endsWith = [name](const std::string_view suffix) {
            return name.size() >= suffix.size() &&
                   name.substr(name.size() - suffix.size()) == suffix;
        };
        const bool settingsIni = name.rfind("settings", 0U) == 0U && endsWith(".ini");
        const bool imguiIni = name.rfind("imgui", 0U) == 0U && endsWith(".ini");
        const bool wwiseTransfer = name == "wwise_transfer.json";
        bool databaseFile = false;
        for (std::size_t database = name.find(".db");
             database != std::string_view::npos;
             database = name.find(".db", database + 3U)) {
            const std::size_t suffix = database + 3U;
            if (suffix == name.size() || name[suffix] == '-' ||
                name[suffix] == '.') {
                databaseFile = true;
                break;
            }
        }
        const bool audioFile = std::any_of(
            protectedExtensions.begin(), protectedExtensions.end(), endsWith);
        if (settingsIni || imguiIni || wwiseTransfer || databaseFile || audioFile) {
            result.status = HarkenSearchPackagePolicyStatus::ProtectedInventory;
            result.diagnostic = "protected Harken Search user-data inventory path";
            return result;
        }
    }
    result.status = HarkenSearchPackagePolicyStatus::Accepted;
    return result;
}

}  // namespace

VerifiedPackagePlanBinding::VerifiedPackagePlanBinding(
    std::shared_ptr<const State> state) noexcept
    : state_(std::move(state)) {}

const std::string& VerifiedPackagePlanBinding::packagePlanDigest() const noexcept {
    static const std::string empty;
    return state_ != nullptr ? state_->admission.packageDigest : empty;
}

const std::string&
VerifiedPackagePlanBinding::packageSignatureDigest() const noexcept {
    static const std::string empty;
    return state_ != nullptr ? state_->signatureDigest : empty;
}

const std::string& VerifiedPackagePlanBinding::keyId() const noexcept {
    static const std::string empty;
    return state_ != nullptr && state_->admission.plan.has_value()
        ? state_->admission.plan->keyId
        : empty;
}

const std::string& VerifiedPackagePlanBinding::version() const noexcept {
    static const std::string empty;
    return state_ != nullptr && state_->admission.plan.has_value()
        ? state_->admission.plan->version
        : empty;
}

const std::string& VerifiedPackagePlanBinding::buildId() const noexcept {
    static const std::string empty;
    return state_ != nullptr && state_->admission.plan.has_value()
        ? state_->admission.plan->buildId
        : empty;
}

const std::string& VerifiedPackagePlanBinding::channel() const noexcept {
    static const std::string empty;
    return state_ != nullptr && state_->admission.plan.has_value()
        ? state_->admission.plan->channel
        : empty;
}

const std::string& VerifiedPackagePlanBinding::assetSha256() const noexcept {
    static const std::string empty;
    return state_ != nullptr && state_->admission.plan.has_value()
        ? state_->admission.plan->assetSha256
        : empty;
}

std::uint64_t VerifiedPackagePlanBinding::assetSize() const noexcept {
    return state_ != nullptr && state_->admission.plan.has_value()
        ? state_->admission.plan->assetSize
        : 0U;
}

const std::vector<std::string>&
VerifiedPackagePlanBinding::approvedArchiveEntries() const noexcept {
    static const std::vector<std::string> empty;
    return state_ != nullptr ? state_->archiveEntries : empty;
}

const std::vector<InstallInventoryItem>&
VerifiedPackagePlanBinding::approvedArchiveItems() const noexcept {
    static const std::vector<InstallInventoryItem> empty;
    return state_ != nullptr ? state_->archiveItems : empty;
}

VerifiedPackagePlanBindingResult VerifyPackagePlanBinding(
    const std::string_view packageBytes,
    const std::string_view signatureHex,
    const PackagePlanTrustConfig& trust,
    const PackageSignatureVerifier testVerifier) {
    VerifiedPackagePlanBindingResult result;
    PackagePlanAdmission admission = VerifyPackagePlan(
        packageBytes, signatureHex, trust, testVerifier);
    result.status = admission.status;
    result.diagnostic = admission.diagnostic;
    if (!admission.ok() || !admission.plan.has_value()) return result;

    auto state = std::make_shared<VerifiedPackagePlanBinding::State>();
    state->signatureDigest = BootstrapSha256Hex(admission.packageSignatureHex);
    state->archiveEntries.reserve(admission.plan->inventory.size());
    state->archiveItems = admission.plan->inventory;
    for (const InstallInventoryItem& item : admission.plan->inventory) {
        state->archiveEntries.push_back(item.relativePath);
    }
    state->admission = std::move(admission);
    result.binding.emplace(VerifiedPackagePlanBinding(std::move(state)));
    return result;
}

HarkenSearchPackagePolicyResult EvaluateHarkenSearchPackagePolicy(
    const VerifiedPackagePlanBinding& binding) {
    if (binding.state_ == nullptr) return {};
    return evaluateHarkenSearchPackagePolicy(binding.state_->admission);
}

struct InvocationCapability::State {
    InvocationEnvelope invocation;
    BootstrapProcessIdentity authenticatedParent;
    std::string archivePath;
    std::string packagePlanDigest;
    std::string packageSignatureDigest;
    std::string keyId;
    std::string assetSha256;
    std::uint64_t assetSize = 0U;
    std::vector<std::string> archiveEntries;
};

// The authenticating Win32 channel already matched the parent with sameIdentity,
// which compares the image path *normalized*. The same process is captured twice
// by different code paths (the updater's self-identity via one API, the
// Bootstrap's handle capture via another), so its image path can differ only in
// separator or case ('/' vs '\\'). Mirror sameIdentity here: compare exact
// process and file identity but normalize the image path, instead of a raw
// string operator== that rejects a genuine same-process match.
bool sameAuthenticatedParent(const BootstrapProcessIdentity& left,
                             const BootstrapProcessIdentity& right) {
    return left.valid && right.valid &&
           left.processId == right.processId &&
           left.creationTime == right.creationTime &&
           left.imageVolumeSerial == right.imageVolumeSerial &&
           left.imageFileId == right.imageFileId &&
           normalizePathText(left.imagePath) == normalizePathText(right.imagePath);
}

InvocationCapability::InvocationCapability(
    const VerifiedPackagePlanBinding& binding,
    const InvocationEnvelope& invocation,
    const BootstrapProcessIdentity& authenticatedParent) {
    if (binding.state_ == nullptr || !binding.state_->admission.ok() ||
        !validInvocationEnvelope(invocation) || !authenticatedParent.valid ||
        !sameAuthenticatedParent(authenticatedParent, invocation.parentProcess) ||
        invocation.packagePlanDigest != binding.packagePlanDigest()) {
        return;
    }
    std::string archivePath = invocation.stagingRoot;
    if (archivePath.empty()) return;
    if (archivePath.back() != '/' && archivePath.back() != '\\') {
        archivePath.push_back('/');
    }
    archivePath += "HarkenSearch-" + binding.version() + "-win-x64-update.zip";
    if (!looksAbsoluteWindowsPath(archivePath)) return;

    auto state = std::make_unique<State>();
    state->invocation = invocation;
    state->authenticatedParent = authenticatedParent;
    state->archivePath = std::move(archivePath);
    state->packagePlanDigest = binding.packagePlanDigest();
    state->packageSignatureDigest = binding.packageSignatureDigest();
    state->keyId = binding.keyId();
    state->assetSha256 = binding.assetSha256();
    state->assetSize = binding.assetSize();
    state->archiveEntries = binding.approvedArchiveEntries();
    state_ = std::move(state);
}

InvocationCapability::InvocationCapability(InvocationCapability&&) noexcept = default;
InvocationCapability& InvocationCapability::operator=(
    InvocationCapability&&) noexcept = default;
InvocationCapability::~InvocationCapability() = default;

std::string AuthorizedBootstrapArchivePath(
    const InvocationCapability& capability) {
    return capability.state_ != nullptr ? capability.state_->archivePath
                                        : std::string{};
}

BootstrapHandoffSubmission SubmitVerifiedPackagePlan(
    const VerifiedPackagePlanBinding& binding,
    const std::string_view archivePath,
    InvocationCapability&& capability) {
    BootstrapHandoffSubmission result;
    std::unique_ptr<InvocationCapability::State> authority =
        std::move(capability.state_);
    if (binding.state_ == nullptr || !binding.state_->admission.ok()) {
        result.status_ = BootstrapHandoffSubmissionStatus::InvalidBinding;
        result.diagnostic_ = "verified package binding";
        return result;
    }
    if (authority == nullptr) {
        result.status_ = BootstrapHandoffSubmissionStatus::InvalidCapability;
        result.diagnostic_ = "one-shot invocation capability";
        return result;
    }
    if (binding.keyId() != authority->keyId) {
        result.status_ = BootstrapHandoffSubmissionStatus::PackageKeyMismatch;
        result.diagnostic_ = "release key id";
        return result;
    }
    if (binding.approvedArchiveEntries() != authority->archiveEntries) {
        result.status_ = BootstrapHandoffSubmissionStatus::ArchiveEntriesMismatch;
        result.diagnostic_ = "signed archive entry names";
        return result;
    }
    if (binding.assetSha256() != authority->assetSha256 ||
        binding.assetSize() != authority->assetSize) {
        result.status_ = BootstrapHandoffSubmissionStatus::AssetBindingMismatch;
        result.diagnostic_ = "signed asset digest and size";
        return result;
    }
    if (binding.packagePlanDigest() != authority->packagePlanDigest) {
        result.status_ = BootstrapHandoffSubmissionStatus::PackageDigestMismatch;
        result.diagnostic_ = "package plan digest";
        return result;
    }
    if (binding.packageSignatureDigest() != authority->packageSignatureDigest) {
        result.status_ = BootstrapHandoffSubmissionStatus::PackageSignatureMismatch;
        result.diagnostic_ = "detached signature binding";
        return result;
    }
    if (normalizePathText(archivePath) !=
        normalizePathText(authority->archivePath)) {
        result.status_ = BootstrapHandoffSubmissionStatus::ArchivePathRejected;
        result.diagnostic_ = "Bootstrap-derived archive path";
        return result;
    }
#if !defined(ARIS_BOOTSTRAP_TEST_CAPABILITY_ISSUER)
    // The test-only issuer keeps lower-level signed-removal reducer and
    // durable-recovery diagnostics reachable. The separately built black-box
    // Bootstrap omits that seam and verifies this product gate end to end.
    const HarkenSearchPackagePolicyResult productPolicy =
        evaluateHarkenSearchPackagePolicy(binding.state_->admission);
    if (!productPolicy.ok()) {
        result.status_ = BootstrapHandoffSubmissionStatus::InvocationRejected;
        result.diagnostic_ = productPolicy.diagnostic;
        return result;
    }
#endif
    BootstrapAdmission admitted = admitBootstrapInvocation(
        binding.state_->admission, authority->invocation);
    if (!admitted.ok()) {
        result.status_ = BootstrapHandoffSubmissionStatus::InvocationRejected;
        result.diagnostic_ = admitted.diagnostic;
        return result;
    }
    result.status_ = BootstrapHandoffSubmissionStatus::Accepted;
    result.authenticatedParent_ = authority->authenticatedParent;
    result.admission_ = std::move(admitted);
    return result;
}

#if defined(ARIS_BOOTSTRAP_TEST_CAPABILITY_ISSUER)
InvocationCapability IssueInvocationCapabilityForTesting(
    const VerifiedPackagePlanBinding& binding,
    const InvocationEnvelope& invocation,
    const BootstrapProcessIdentity& authenticatedParent) {
    return InvocationCapability(binding, invocation, authenticatedParent);
}
#endif

std::string BootstrapSha256Hex(const std::string_view bytes) {
    return digestHex(bytes);
}

namespace {

bool packageMatchesValidatedPlan(const PackagePlan& package,
                                 const ValidatedInstallPlan& expected) {
    if (package.schemaVersion != 1U || package.product != "aris" ||
        package.channel != expected.packageChannel ||
        package.version != expected.version ||
        package.buildId != expected.buildId ||
        package.keyId != expected.packageKeyId ||
        package.entrypoint != expected.entrypoint ||
        package.inventory.size() != expected.inventory.size() ||
        package.removals != expected.removals) {
        return false;
    }
    return std::equal(
        package.inventory.begin(), package.inventory.end(),
        expected.inventory.begin(),
        [](const InstallInventoryItem& left,
           const InstallInventoryItem& right) {
            return left.relativePath == right.relativePath &&
                   left.sha256 == right.sha256 &&
                   left.byteSize == right.byteSize;
        });
}

}  // namespace

DurablePackageAuthorityResult ValidateDurablePackageAuthority(
    const ValidatedInstallPlan& expectedPlan,
    const std::string_view packageBytes,
    const std::string_view signatureHex,
    const PackagePlanTrustConfig& trust,
    const PackageSignatureVerifier testVerifier) {
    DurablePackageAuthorityResult result;
    if (!expectedPlan.isValidated()) {
        result.status = DurablePackageAuthorityStatus::InvalidPlan;
        result.diagnostic = "validated plan binding";
        return result;
    }
    // Schema 4 recovery is intentionally strict. A digest-only journal, or
    // a partially published reference, cannot reconstruct release authority.
    if (expectedPlan.packagePlanBytes.empty()) {
        result.status = DurablePackageAuthorityStatus::MissingPackageBytes;
        result.diagnostic = "plan package bytes";
        return result;
    }
    if (expectedPlan.packageSignatureHex.empty()) {
        result.status = DurablePackageAuthorityStatus::MissingPackageSignature;
        result.diagnostic = "plan package signature";
        return result;
    }
    if (packageBytes.empty()) {
        result.status = DurablePackageAuthorityStatus::MissingPackageBytes;
        result.diagnostic = "journal package bytes";
        return result;
    }
    if (signatureHex.empty()) {
        result.status = DurablePackageAuthorityStatus::MissingPackageSignature;
        result.diagnostic = "journal package signature";
        return result;
    }
    if (expectedPlan.packagePlanBytes.size() > kMaximumPackageBytes ||
        packageBytes.size() > kMaximumPackageBytes) {
        result.status = DurablePackageAuthorityStatus::PackageTooLarge;
        result.diagnostic = "bounded package bytes";
        return result;
    }
    if (packageBytes != expectedPlan.packagePlanBytes ||
        signatureHex != expectedPlan.packageSignatureHex) {
        result.status = DurablePackageAuthorityStatus::PackageReferenceMismatch;
        result.diagnostic = "journal package reference differs from plan";
        return result;
    }
    if (!isLowerHex(expectedPlan.packagePlanDigest, 64U) ||
        !isLowerHex(expectedPlan.invocation.packagePlanDigest, 64U)) {
        result.status = DurablePackageAuthorityStatus::InvalidPackageDigest;
        result.diagnostic = "plan package digest encoding";
        return result;
    }
    const std::string digest = digestHex(packageBytes);
    if (digest != expectedPlan.packagePlanDigest ||
        digest != expectedPlan.invocation.packagePlanDigest) {
        result.status = DurablePackageAuthorityStatus::PackageDigestMismatch;
        result.diagnostic = "package bytes digest";
        return result;
    }
    if (!trust.provisioned || trust.keyId.empty() ||
        trust.keyId != expectedPlan.packageKeyId) {
        result.status = DurablePackageAuthorityStatus::PackageKeyMismatch;
        result.diagnostic = "release key binding";
        return result;
    }

    const PackagePlanAdmission verified = VerifyPackagePlan(
        packageBytes, signatureHex, trust, testVerifier);
    if (!verified.ok()) {
        result.status = DurablePackageAuthorityStatus::SignatureRejected;
        result.diagnostic = verified.diagnostic;
        return result;
    }
    if (verified.packageDigest != digest || !verified.plan.has_value() ||
        !packageMatchesValidatedPlan(*verified.plan, expectedPlan)) {
        result.status = DurablePackageAuthorityStatus::PackagePlanMismatch;
        result.diagnostic = "signed package fields";
        return result;
    }
    result.status = DurablePackageAuthorityStatus::Accepted;
    return result;
}

namespace {

bool sameRecoveryImage(const BootstrapProcessIdentity& left,
                       const BootstrapProcessIdentity& right) {
    if (!left.valid || !right.valid ||
        left.imageVolumeSerial != right.imageVolumeSerial ||
        left.imageFileId != right.imageFileId) {
        return false;
    }
    auto normalize = [](std::string value) {
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
    };
    return normalize(left.imagePath) == normalize(right.imagePath);
}

bool sameRecoveryProcessLifetime(const BootstrapProcessIdentity& left,
                                 const BootstrapProcessIdentity& right) {
    return left.processId == right.processId &&
           left.creationTime == right.creationTime &&
           sameRecoveryImage(left, right);
}

bool safeRecoveryRootText(const std::string& value) {
    if (value.size() < 3U || value.size() > 4096U ||
        !std::isalpha(static_cast<unsigned char>(value[0])) ||
        value[1] != ':' || (value[2] != '/' && value[2] != '\\') ||
        value.back() == '/' || value.back() == '\\') {
        return false;
    }
    std::string normalized = value;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.find("//", 3U) != std::string::npos) return false;
    std::size_t begin = 3U;
    while (begin < normalized.size()) {
        const std::size_t end = normalized.find('/', begin);
        const std::string component = normalized.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (component.empty() || component == "." || component == ".." ||
            component.back() == '.' || component.back() == ' ') {
            return false;
        }
        for (const unsigned char character : component) {
            if (character < 0x20U || character == '<' || character == '>' ||
                character == ':' || character == '"' || character == '|' ||
                character == '?' || character == '*') {
                return false;
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1U;
    }
    return true;
}

std::string normalizeRecoveryRootText(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(
                           character >= 'A' && character <= 'Z'
                               ? character + ('a' - 'A')
                               : character);
                   });
    while (value.size() > 3U && value.back() == '/') value.pop_back();
    return value;
}

bool sameRecoveryRootText(const std::string& left,
                          const std::string& right) {
    return safeRecoveryRootText(left) && safeRecoveryRootText(right) &&
           normalizeRecoveryRootText(left) == normalizeRecoveryRootText(right);
}

}  // namespace

bool ValidateAnchoredWindowsBootstrapJournalRoots(
    const WindowsBootstrapJournalRecord& record,
    const std::string& expectedUpdaterOwnedBase,
    const std::string& expectedStagingRoot,
    const std::string& expectedTransactionRoot) {
    if (!safeRecoveryRootText(expectedUpdaterOwnedBase) ||
        !safeRecoveryRootText(expectedStagingRoot) ||
        !safeRecoveryRootText(expectedTransactionRoot) ||
        !safeRecoveryRootText(record.updaterOwnedBase) ||
        !safeRecoveryRootText(record.stagingRoot) ||
        !safeRecoveryRootText(record.transactionRoot)) {
        return false;
    }
    return normalizeRecoveryRootText(record.updaterOwnedBase) ==
               normalizeRecoveryRootText(expectedUpdaterOwnedBase) &&
           normalizeRecoveryRootText(record.stagingRoot) ==
               normalizeRecoveryRootText(expectedStagingRoot) &&
           normalizeRecoveryRootText(record.transactionRoot) ==
               normalizeRecoveryRootText(expectedTransactionRoot) &&
           normalizeRecoveryRootText(record.stagingRoot) !=
               normalizeRecoveryRootText(record.transactionRoot);
}

DurableRecoveryAdmissionResult AdmitDurableBootstrapRecovery(
    const WindowsBootstrapJournalRecord& record,
    const PackagePlanTrustConfig& trust,
    const BootstrapProcessIdentity& authenticatedParent,
    const std::string& expectedUpdaterOwnedBase,
    const std::string& expectedStagingRoot,
    const std::string& expectedTransactionRoot,
    const PackageSignatureVerifier testVerifier) {
    static_cast<void>(trust);
    static_cast<void>(authenticatedParent);
    static_cast<void>(expectedUpdaterOwnedBase);
    static_cast<void>(expectedStagingRoot);
    static_cast<void>(expectedTransactionRoot);
    static_cast<void>(testVerifier);
    DurableRecoveryAdmissionResult result;
    if ((record.schemaVersion != 9U && record.schemaVersion != 10U) || record.packagePlanBytes.empty() ||
        record.packageSignatureHex.empty()) {
        result.status = DurableRecoveryAdmissionStatus::MissingPackageAuthority;
        result.diagnostic = "schema-9/10 signed package reference";
        return result;
    }
    result.status = DurableRecoveryAdmissionStatus::FreshHandoffRequired;
    result.diagnostic = "durable record is evidence, not restart mutation authority";
    return result;
}

DurableRecoveryAdmissionResult AdmitAuthenticatedBootstrapRecovery(
    const WindowsBootstrapJournalRecord& record,
    const PackagePlanTrustConfig& trust,
    const BootstrapHandoffSubmission& submittedAuthority,
    const std::string& expectedUpdaterOwnedBase,
    const std::string& expectedStagingRoot,
    const std::string& expectedTransactionRoot,
    const PackageSignatureVerifier testVerifier) {
    DurableRecoveryAdmissionResult result;
    if ((record.schemaVersion != 9U && record.schemaVersion != 10U) || record.packagePlanBytes.empty() ||
        record.packageSignatureHex.empty()) {
        result.status = DurableRecoveryAdmissionStatus::MissingPackageAuthority;
        result.diagnostic = "schema-9/10 signed package reference";
        return result;
    }
    if (!submittedAuthority.ok() ||
        !submittedAuthority.admission_.has_value() ||
        !submittedAuthority.admission_->plan.has_value() ||
        !submittedAuthority.admission_->journal.has_value() ||
        !submittedAuthority.authenticatedParent_.valid) {
        result.status = DurableRecoveryAdmissionStatus::FreshHandoffRequired;
        result.diagnostic = "fresh opaque Bootstrap submission";
        return result;
    }
    const BootstrapAdmission& fresh = *submittedAuthority.admission_;
    const ValidatedInstallPlan& plan = *fresh.plan;
    if (!(submittedAuthority.authenticatedParent_ ==
          plan.invocation.parentProcess)) {
        result.status = DurableRecoveryAdmissionStatus::ChannelRejected;
        result.diagnostic = "opaque capability parent lifetime";
        return result;
    }
    if (!sameRecoveryImage(plan.invocation.parentProcess,
                           record.parentProcessIdentity)) {
        result.status = DurableRecoveryAdmissionStatus::ParentIdentityRejected;
        result.diagnostic = "current parent image differs from durable updater";
        return result;
    }
    if (!ValidateAnchoredWindowsBootstrapJournalRoots(
            record, expectedUpdaterOwnedBase, expectedStagingRoot,
            expectedTransactionRoot)) {
        result.status = DurableRecoveryAdmissionStatus::RootBindingRejected;
        result.diagnostic = "durable roots do not equal trusted anchors";
        return result;
    }
    WindowsBootstrapJournalRecord freshRootBinding = record;
    freshRootBinding.updaterOwnedBase = plan.invocation.updaterOwnedBase;
    freshRootBinding.stagingRoot = plan.invocation.stagingRoot;
    freshRootBinding.transactionRoot = plan.invocation.transactionRoot;
    if (!ValidateAnchoredWindowsBootstrapJournalRoots(
            freshRootBinding, expectedUpdaterOwnedBase, expectedStagingRoot,
            expectedTransactionRoot)) {
        result.status = DurableRecoveryAdmissionStatus::RootBindingRejected;
        result.diagnostic = "fresh roots do not equal trusted anchors";
        return result;
    }
    if (plan.packagePlanBytes != record.packagePlanBytes ||
        plan.packageSignatureHex != record.packageSignatureHex) {
        result.status = DurableRecoveryAdmissionStatus::PackageRejected;
        result.diagnostic = "fresh signed package reference differs from durable record";
        return result;
    }
    const DurablePackageAuthorityResult packageAuthority =
        ValidateDurablePackageAuthority(
            plan, record.packagePlanBytes, record.packageSignatureHex, trust,
            testVerifier);
    if (!packageAuthority.ok()) {
        result.status = DurableRecoveryAdmissionStatus::PackageRejected;
        result.diagnostic = packageAuthority.diagnostic;
        return result;
    }
    if (plan.packagePlanDigest != record.packagePlanDigest ||
        plan.invocation.packagePlanDigest != record.packagePlanDigest) {
        result.status = DurableRecoveryAdmissionStatus::PackageDigestMismatch;
        result.diagnostic = "fresh package digest differs from durable record";
        return result;
    }
    if (BootstrapSha256Hex(plan.healthToken) !=
        record.healthNonceDigest) {
        result.status = DurableRecoveryAdmissionStatus::HealthCapabilityRejected;
        result.diagnostic = "fresh health capability preimage mismatch";
        return result;
    }
    if (plan.invocation.transactionId != record.transactionId) {
        result.status = DurableRecoveryAdmissionStatus::InvocationBindingRejected;
        result.diagnostic = "fresh transaction id differs from durable binding";
        return result;
    }
    if (plan.identity.installationId != record.installationId) {
        result.status = DurableRecoveryAdmissionStatus::InvocationBindingRejected;
        result.diagnostic = "fresh installation id differs from durable binding";
        return result;
    }
    if (!sameRecoveryRootText(plan.identity.targetRoot, record.targetRoot)) {
        result.status = DurableRecoveryAdmissionStatus::InvocationBindingRejected;
        result.diagnostic = "fresh target root differs from durable binding";
        return result;
    }
    if (plan.invocation.invocationIdentity != record.invocationIdentity) {
        result.status = DurableRecoveryAdmissionStatus::InvocationBindingRejected;
        result.diagnostic = "fresh invocation id differs from durable binding";
        return result;
    }
    if (plan.identity.protectedDataRoots.size() !=
            record.protectedDataRoots.size() ||
        !std::equal(plan.identity.protectedDataRoots.begin(),
                    plan.identity.protectedDataRoots.end(),
                    record.protectedDataRoots.begin(),
                    [](const std::string& left, const std::string& right) {
                        return sameRecoveryRootText(left, right);
                    })) {
        result.status = DurableRecoveryAdmissionStatus::InvocationBindingRejected;
        result.diagnostic = "fresh protected roots differ from durable binding";
        return result;
    }
    if (plan.oldProcessId != record.oldProcessId ||
        !sameRecoveryProcessLifetime(plan.invocation.oldProcess,
                                     record.oldProcessIdentity)) {
        result.status = DurableRecoveryAdmissionStatus::InvocationBindingRejected;
        result.diagnostic = "fresh old-process identity differs from durable binding";
        return result;
    }
    if (plan.entrypoint != record.entrypoint) {
        result.status = DurableRecoveryAdmissionStatus::InvocationBindingRejected;
        result.diagnostic = "fresh entrypoint differs from durable binding";
        return result;
    }
    if (plan.version != record.version ||
        plan.buildId != record.buildId ||
        plan.packageKeyId != record.packageKeyId ||
        plan.packageChannel != record.packageChannel ||
        plan.entrypoint != record.entrypoint) {
        result.status = DurableRecoveryAdmissionStatus::PackageRejected;
        result.diagnostic = "durable package immutable fields";
        return result;
    }
    if (fresh.journal->items.size() != record.items.size()) {
        result.status = DurableRecoveryAdmissionStatus::InvalidJournal;
        result.diagnostic = "durable inventory item count";
        return result;
    }
    for (std::size_t index = 0; index < record.items.size(); ++index) {
        if (fresh.journal->items[index].relativePath !=
                record.items[index].relativePath ||
            fresh.journal->items[index].classification !=
                record.items[index].classification) {
            result.status = DurableRecoveryAdmissionStatus::InvalidJournal;
            result.diagnostic = "durable inventory classifications";
            return result;
        }
    }
    std::vector<ReplacementJournalItem> items;
    items.reserve(record.items.size());
    for (const WindowsBootstrapJournalItemRecord& item : record.items) {
        items.push_back({item.relativePath, item.classification,
                         item.classification == JournalItemClass::ReplaceExisting ||
                             item.classification == JournalItemClass::RemoveExisting,
                         item.backupComplete, item.replacementComplete});
    }
    const JournalBuildResult rehydrated = RehydrateReplacementJournal(
        plan, items, record.transactionId);
    if (!rehydrated.ok() || !rehydrated.journal.has_value()) {
        result.status = DurableRecoveryAdmissionStatus::InvalidJournal;
        result.diagnostic = "durable inventory classifications";
        return result;
    }
    result.status = DurableRecoveryAdmissionStatus::Accepted;
    result.plan = fresh.plan;
    result.journal = std::move(rehydrated.journal);
    return result;
}

InvocationDecodeResult DecodeInvocationEnvelope(const std::string_view bytes) {
    InvocationDecodeResult result;
    if (bytes.empty() || bytes.size() > kMaximumInvocationBytes) {
        result.status = InvocationDecodeStatus::TooLarge;
        return result;
    }
    const json root = json::parse(bytes, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        result.status = InvocationDecodeStatus::InvalidJson;
        return result;
    }
    if (!hasOnlyKeys(root, {"deployment_mode", "entrypoint", "existing_inventory", "health_nonce",
                            "installation_id", "invocation_identity", "old_process",
                             "old_executable_identity",
                             "old_process_id", "package_plan_digest", "parent_process",
                             "previous_failure_count",
                            "registered_app_id",
                            "staging_root", "target_root", "transaction_id",
                            "target_root_identity",
                            "transaction_root", "updater_owned_base",
                            "protected_data_roots"})) {
        result.status = InvocationDecodeStatus::InvalidSchema;
        return result;
    }
    const char* strings[] = {"deployment_mode", "entrypoint", "health_nonce", "installation_id",
                             "invocation_identity", "package_plan_digest",
                             "registered_app_id",
                             "staging_root", "target_root", "transaction_id",
                             "transaction_root", "updater_owned_base"};
    for (const char* key : strings) {
        if (!isString(root, key)) {
            result.status = InvocationDecodeStatus::InvalidField;
            return result;
        }
    }
    if (!root["old_process_id"].is_number_unsigned() ||
        !root["previous_failure_count"].is_number_unsigned() ||
        !root["existing_inventory"].is_array() ||
        !root["protected_data_roots"].is_array()) {
        result.status = InvocationDecodeStatus::InvalidField;
        return result;
    }
    InvocationEnvelope invocation;
    const std::string deploymentMode = root["deployment_mode"].get<std::string>();
    if (deploymentMode == "installed") {
        invocation.deploymentMode = aris::platform::DeploymentMode::Installed;
    } else if (deploymentMode == "portable") {
        invocation.deploymentMode = aris::platform::DeploymentMode::Portable;
    } else {
        result.status = InvocationDecodeStatus::InvalidField;
        return result;
    }
    invocation.entrypoint = root["entrypoint"].get<std::string>();
    invocation.healthNonce = root["health_nonce"].get<std::string>();
    invocation.installationId = root["installation_id"].get<std::string>();
    invocation.registeredAppId = root["registered_app_id"].get<std::string>();
    invocation.invocationIdentity = root["invocation_identity"].get<std::string>();
    invocation.packagePlanDigest = root["package_plan_digest"].get<std::string>();
    invocation.stagingRoot = root["staging_root"].get<std::string>();
    invocation.targetRoot = root["target_root"].get<std::string>();
    invocation.transactionId = root["transaction_id"].get<std::string>();
    invocation.transactionRoot = root["transaction_root"].get<std::string>();
    invocation.updaterOwnedBase = root["updater_owned_base"].get<std::string>();
    invocation.oldProcessId = root["old_process_id"].get<std::uint64_t>();
    const std::uint64_t previousFailureCount =
        root["previous_failure_count"].get<std::uint64_t>();
    if (previousFailureCount > kMaximumFailureCount) {
        result.status = InvocationDecodeStatus::InvalidField;
        return result;
    }
    invocation.previousFailureCount =
        static_cast<std::uint32_t>(previousFailureCount);
    if (!isLowerHex(invocation.healthNonce, 64U) ||
        !isLowerHex(invocation.packagePlanDigest, 64U) ||
        !safeIdentifier(invocation.transactionId, 1U, 128U) ||
        !safeIdentifier(invocation.installationId, 1U, 128U) ||
        !safeIdentifier(invocation.invocationIdentity, 1U, 256U) ||
        invocation.oldProcessId == 0U || invocation.entrypoint.empty()) {
        result.status = InvocationDecodeStatus::InvalidField;
        return result;
    }
    if (!root["parent_process"].is_object()) {
        result.status = InvocationDecodeStatus::InvalidIdentity;
        result.diagnostic = "parent-process-object";
        return result;
    }
    if (!root["old_process"].is_object()) {
        result.status = InvocationDecodeStatus::InvalidIdentity;
        result.diagnostic = "old-process-object";
        return result;
    }
    if (!root["target_root_identity"].is_object()) {
        result.status = InvocationDecodeStatus::InvalidIdentity;
        result.diagnostic = "target-root-identity-object";
        return result;
    }
    if (!root["old_executable_identity"].is_object()) {
        result.status = InvocationDecodeStatus::InvalidIdentity;
        result.diagnostic = "old-executable-identity-object";
        return result;
    }
    if (!parseProcessIdentity(root["parent_process"], invocation.parentProcess)) {
        result.status = InvocationDecodeStatus::InvalidIdentity;
        result.diagnostic = "parent-process-parse";
        return result;
    }
    if (!parseProcessIdentity(root["old_process"], invocation.oldProcess)) {
        result.status = InvocationDecodeStatus::InvalidIdentity;
        result.diagnostic = "old-process-parse";
        return result;
    }
    if (!parseDeploymentFileIdentity(root["target_root_identity"],
                                     invocation.targetRootIdentity)) {
        result.status = InvocationDecodeStatus::InvalidIdentity;
        result.diagnostic = "target-root-identity-parse";
        return result;
    }
    if (!parseDeploymentFileIdentity(root["old_executable_identity"],
                                     invocation.oldExecutableIdentity)) {
        result.status = InvocationDecodeStatus::InvalidIdentity;
        result.diagnostic = "old-executable-identity-parse";
        return result;
    }
    if (invocation.oldProcess.processId != invocation.oldProcessId) {
        result.status = InvocationDecodeStatus::InvalidIdentity;
        result.diagnostic = "old-process-id-mismatch";
        return result;
    }
    for (const auto& rawRoot : root["protected_data_roots"]) {
        if (!rawRoot.is_string()) {
            result.status = InvocationDecodeStatus::InvalidField;
            return result;
        }
        invocation.protectedDataRoots.push_back(rawRoot.get<std::string>());
    }
    if (invocation.protectedDataRoots.empty()) {
        result.status = InvocationDecodeStatus::InvalidField;
        return result;
    }
    for (const auto& rawItem : root["existing_inventory"]) {
        if (!hasOnlyKeys(rawItem, {"relative_path"}) ||
            !isString(rawItem, "relative_path")) {
            result.status = InvocationDecodeStatus::InvalidInventory;
            return result;
        }
        invocation.existingInventory.push_back(
            {rawItem["relative_path"].get<std::string>()});
    }
    if (invocation.existingInventory.size() > kMaximumInventoryItems) {
        result.status = InvocationDecodeStatus::InvalidInventory;
        return result;
    }
    // Keep the channel decoder's value boundary equivalent to direct
    // admission.  A caller must not be able to smuggle an oversized root,
    // malformed process tuple, or forged identifier through a parser that
    // happens to be used before AdmitBootstrapInvocation.
    if (!validInvocationEnvelope(invocation)) {
        result.status = InvocationDecodeStatus::InvalidField;
        return result;
    }
    result.status = InvocationDecodeStatus::Accepted;
    result.invocation = std::move(invocation);
    return result;
}

BootstrapHandoffDecodeResult DecodeBootstrapHandoff(const std::string_view bytes) {
    BootstrapHandoffDecodeResult result;
    if (bytes.empty() || bytes.size() > kMaximumInvocationBytes + kMaximumPackageBytes) {
        result.status = BootstrapHandoffDecodeStatus::TooLarge;
        return result;
    }
    const json root = json::parse(bytes, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        result.status = BootstrapHandoffDecodeStatus::InvalidJson;
        return result;
    }
    if (!hasOnlyKeys(root, {"invocation", "launch_broker", "package_plan",
                            "package_signature"})) {
        result.status = BootstrapHandoffDecodeStatus::InvalidSchema;
        return result;
    }
    if (!isString(root, "invocation") || !isString(root, "package_plan") ||
        !isString(root, "package_signature") ||
        !root["launch_broker"].is_object()) {
        result.status = BootstrapHandoffDecodeStatus::InvalidField;
        return result;
    }
    const json& broker = root["launch_broker"];
    if (!hasOnlyKeys(broker, {"channel_handle", "containment_job_handle",
                              "launch_timeout_ms", "process_handle",
                              "process_identity"})) {
        result.status = BootstrapHandoffDecodeStatus::InvalidSchema;
        return result;
    }
    if (!broker["channel_handle"].is_number_unsigned() ||
        !broker["containment_job_handle"].is_number_unsigned() ||
        !broker["launch_timeout_ms"].is_number_unsigned() ||
        !broker["process_handle"].is_number_unsigned() ||
        !broker["process_identity"].is_object()) {
        result.status = BootstrapHandoffDecodeStatus::InvalidField;
        return result;
    }
    BootstrapHandoff handoff;
    handoff.invocationBytes = root["invocation"].get<std::string>();
    handoff.packagePlanBytes = root["package_plan"].get<std::string>();
    handoff.packageSignatureHex = root["package_signature"].get<std::string>();
    handoff.launchBroker.channelHandle =
        broker["channel_handle"].get<std::uint64_t>();
    handoff.launchBroker.brokerProcessHandle =
        broker["process_handle"].get<std::uint64_t>();
    handoff.launchBroker.containmentJobHandle =
        broker["containment_job_handle"].get<std::uint64_t>();
    const std::uint64_t launchTimeoutMs =
        broker["launch_timeout_ms"].get<std::uint64_t>();
    if (launchTimeoutMs > std::numeric_limits<std::uint32_t>::max()) {
        result.status = BootstrapHandoffDecodeStatus::InvalidField;
        return result;
    }
    handoff.launchBroker.launchTimeoutMs =
        static_cast<std::uint32_t>(launchTimeoutMs);
    if (!parseProcessIdentity(broker["process_identity"],
                              handoff.launchBroker.brokerIdentity) ||
        !validProcessIdentity(handoff.launchBroker.brokerIdentity)) {
        result.status = BootstrapHandoffDecodeStatus::InvalidField;
        return result;
    }
    if (handoff.packagePlanBytes.empty() ||
        handoff.packagePlanBytes.size() > kMaximumPackageBytes ||
        handoff.invocationBytes.empty() ||
        handoff.invocationBytes.size() > kMaximumInvocationBytes ||
        !isLowerHex(handoff.packageSignatureHex, 128U) ||
        !handoff.launchBroker.valid()) {
        result.status = BootstrapHandoffDecodeStatus::InvalidField;
        return result;
    }
    result.status = BootstrapHandoffDecodeStatus::Accepted;
    result.handoff = std::move(handoff);
    return result;
}

BootstrapTerminalCleanupHandoffDecodeResult
DecodeBootstrapTerminalCleanupHandoff(const std::string_view bytes) {
    BootstrapTerminalCleanupHandoffDecodeResult result;
    if (bytes.empty() || bytes.size() > kMaximumInvocationBytes +
                                      kMaximumPackageBytes + 4096U) {
        result.status = BootstrapTerminalCleanupHandoffDecodeStatus::TooLarge;
        return result;
    }
    const json root = json::parse(bytes, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        result.status = BootstrapTerminalCleanupHandoffDecodeStatus::InvalidJson;
        return result;
    }
    if (!hasOnlyKeys(root, {"invocation", "package_plan", "package_signature",
                            "transaction_id"})) {
        result.status = BootstrapTerminalCleanupHandoffDecodeStatus::InvalidSchema;
        return result;
    }
    if (!isString(root, "invocation") || !isString(root, "package_plan") ||
        !isString(root, "package_signature") ||
        !isString(root, "transaction_id")) {
        result.status = BootstrapTerminalCleanupHandoffDecodeStatus::InvalidField;
        return result;
    }
    BootstrapTerminalCleanupHandoff handoff;
    handoff.invocationBytes = root["invocation"].get<std::string>();
    handoff.packagePlanBytes = root["package_plan"].get<std::string>();
    handoff.packageSignatureHex = root["package_signature"].get<std::string>();
    handoff.transactionId = root["transaction_id"].get<std::string>();
    if (handoff.packagePlanBytes.empty() ||
        handoff.packagePlanBytes.size() > kMaximumPackageBytes ||
        handoff.invocationBytes.empty() ||
        handoff.invocationBytes.size() > kMaximumInvocationBytes ||
        !isLowerHex(handoff.packageSignatureHex, 128U) ||
        !safeIdentifier(handoff.transactionId, 1U, 128U)) {
        result.status = BootstrapTerminalCleanupHandoffDecodeStatus::InvalidField;
        return result;
    }
    result.status = BootstrapTerminalCleanupHandoffDecodeStatus::Accepted;
    result.handoff = std::move(handoff);
    return result;
}

BootstrapRecoveryMarkerDecodeResult DecodeBootstrapRecoveryMarker(
    const std::string_view bytes) {
    BootstrapRecoveryMarkerDecodeResult result;
    if (bytes.empty() || bytes.size() > 128U) {
        result.diagnostic = "marker bounds";
        return result;
    }
    const json root = json::parse(bytes, nullptr, false);
    if (root.is_discarded() || !root.is_object() ||
        !hasOnlyKeys(root, {"recovery", "schema"}) ||
        !root.contains("recovery") || !root.contains("schema") ||
        !root["recovery"].is_boolean() || !root["schema"].is_number_unsigned() ||
        !root["recovery"].get<bool>() || root["schema"].get<unsigned>() != 1U) {
        result.diagnostic = "marker schema";
        return result;
    }
    result.accepted = true;
    return result;
}

namespace {

BootstrapAdmission admitBootstrapInvocation(
    const PackagePlanAdmission& package, const InvocationEnvelope& invocation) {
    BootstrapAdmission result;
    if (!package.ok()) {
        result.status = BootstrapAdmissionStatus::PackageRejected;
        return result;
    }
    if (package.packageDigest != invocation.packagePlanDigest) {
        result.status = BootstrapAdmissionStatus::PackageDigestMismatch;
        return result;
    }
    // Report a signed-plan/hand-off entrypoint mismatch before the broader
    // process-path envelope check so callers retain the established typed
    // InvalidPlan outcome for this contract violation.
    if (package.plan->entrypoint != invocation.entrypoint) {
        result.status = BootstrapAdmissionStatus::InvalidPlan;
        result.diagnostic = "signed entrypoint mismatch";
        return result;
    }
    if (!validInvocationEnvelope(invocation)) {
        result.status = BootstrapAdmissionStatus::InvalidInvocation;
        result.diagnostic = "invocation identity, roots, or bounds";
        return result;
    }
    SignedInstallPlan raw;
    raw.schemaVersion = 1;
    raw.product = package.plan->product;
    raw.channel = package.plan->channel;
    raw.version = package.plan->version;
    raw.buildId = package.plan->buildId;
    raw.keyId = package.plan->keyId;
    raw.detachedSignature = std::string(128U, '0');
    raw.payloadSha256 = package.plan->assetSha256;
    raw.packagePlanDigest = package.packageDigest;
    raw.oldProcessId = invocation.oldProcessId;
    raw.healthToken = invocation.healthNonce;
    raw.identity.targetRoot = invocation.targetRoot;
    raw.identity.deploymentMode = invocation.deploymentMode;
    raw.identity.installationId = invocation.installationId;
    raw.identity.registeredAppId = invocation.registeredAppId;
    raw.identity.targetRootIdentity = invocation.targetRootIdentity;
    raw.identity.oldExecutableIdentity = invocation.oldExecutableIdentity;
    raw.identity.protectedDataRoots = invocation.protectedDataRoots;
    raw.invocation.updaterOwnedBase = invocation.updaterOwnedBase;
    raw.invocation.deploymentMode = invocation.deploymentMode;
    raw.invocation.installationId = invocation.installationId;
    raw.invocation.targetRootIdentityBinding =
        invocation.targetRootIdentity.canonicalPath;
    raw.invocation.oldExecutableIdentityBinding =
        invocation.oldExecutableIdentity.canonicalPath;
    raw.invocation.stagingRoot = invocation.stagingRoot;
    raw.invocation.transactionRoot = invocation.transactionRoot;
    raw.invocation.invocationIdentity = invocation.invocationIdentity;
    raw.invocation.transactionId = invocation.transactionId;
    raw.invocation.packagePlanDigest = invocation.packagePlanDigest;
    raw.invocation.healthNonceDigest = BootstrapSha256Hex(invocation.healthNonce);
    raw.invocation.parentProcess = invocation.parentProcess;
    raw.invocation.oldProcess = invocation.oldProcess;
    raw.entrypoint = invocation.entrypoint;
    raw.inventory = package.plan->inventory;
    raw.removals = package.plan->removals;
    const InstallPlanValidation validated =
        ValidateSignedInstallPlan(raw, SignatureStatus::Verified);
    if (!validated.ok()) {
        result.status = BootstrapAdmissionStatus::InvalidPlan;
        result.diagnostic = std::to_string(static_cast<int>(validated.error));
        return result;
    }
    ValidatedInstallPlan admittedPlan = *validated.plan;
    admittedPlan.previousFailureCount = invocation.previousFailureCount;
    admittedPlan.packagePlanBytes = package.packageBytes;
    admittedPlan.packageSignatureHex = package.packageSignatureHex;
    const JournalBuildResult journal = BuildReplacementJournal(
        admittedPlan, invocation.existingInventory, invocation.transactionId);
    if (!journal.ok()) {
        result.status = BootstrapAdmissionStatus::InvalidJournalInputs;
        result.diagnostic = std::to_string(static_cast<int>(journal.error));
        return result;
    }
    result.status = BootstrapAdmissionStatus::Accepted;
    result.plan = std::move(admittedPlan);
    result.journal = journal.journal;
    return result;
}

}  // namespace

BootstrapAdmission AdmitBootstrapTerminalCleanup(
    const PackagePlanAdmission& package,
    const InvocationEnvelope& invocation) {
    // This deliberately reuses the value-level plan construction, but the
    // caller must use the terminal filesystem admission before mutation. The
    // channel parent is therefore not treated as the original updater parent
    // by this function.
    return admitBootstrapInvocation(package, invocation);
}

#if defined(ARIS_BOOTSTRAP_TEST_RAW_INVOCATION_ADMISSION)
BootstrapAdmission AdmitBootstrapInvocationForTesting(
    const PackagePlanAdmission& package,
    const InvocationEnvelope& invocation) {
    return admitBootstrapInvocation(package, invocation);
}
#endif

PackagePlanTrustConfig ProductionPackagePlanTrust() noexcept {
    PackagePlanTrustConfig result;
#if defined(ARIS_BOOTSTRAP_PACKAGE_KEY_ID) && \
    defined(ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
    result.keyId = ARIS_BOOTSTRAP_PACKAGE_KEY_ID;
    result.publicKeyHex = ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX;
    result.provisioned = safeIdentifier(result.keyId, 1U, 64U) &&
                         isStrictEd25519PublicKey(result.publicKeyHex);
#else
    result.provisioned = false;
#endif
    return result;
}

}  // namespace aris::bootstrap
