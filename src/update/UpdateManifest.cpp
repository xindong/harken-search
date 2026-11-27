#include "UpdateManifest.h"
#include "HarkenSearchUpdateProductIdentity.h"
#include "UpdateReleaseConfig.h"

#include "ed25519/tweetnacl.h"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace aris::update {
namespace {

using json = nlohmann::json;

ManifestResult Failure(ManifestError error, const char* detail) {
    ManifestResult result;
    result.error = error;
    result.detail = detail;
    return result;
}

int HexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool DecodeHex(std::string_view text, std::size_t expectedBytes,
               std::vector<unsigned char>& output) {
    if (text.size() != expectedBytes * 2) return false;
    output.resize(expectedBytes);
    for (std::size_t index = 0; index < expectedBytes; ++index) {
        const int high = HexDigit(text[index * 2]);
        const int low = HexDigit(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            output.clear();
            return false;
        }
        output[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

bool VerifySignature(std::string_view message,
                     const std::vector<unsigned char>& signature,
                     const std::vector<unsigned char>& publicKey) {
    std::vector<unsigned char> signedMessage;
    signedMessage.reserve(signature.size() + message.size());
    signedMessage.insert(signedMessage.end(), signature.begin(), signature.end());
    signedMessage.insert(signedMessage.end(), message.begin(), message.end());

    std::vector<unsigned char> opened(signedMessage.size());
    unsigned long long openedLength = 0;
    const int status = crypto_sign_open(
        opened.data(), &openedLength, signedMessage.data(),
        static_cast<unsigned long long>(signedMessage.size()), publicKey.data());
    return status == 0 && openedLength == message.size() &&
           std::equal(message.begin(), message.end(), opened.begin());
}

bool HasOnlyKeys(const json& object,
                 std::initializer_list<const char*> expectedKeys) {
    if (!object.is_object() || object.size() != expectedKeys.size()) return false;
    const std::unordered_set<std::string> expected(expectedKeys.begin(), expectedKeys.end());
    for (auto item = object.begin(); item != object.end(); ++item) {
        if (expected.find(item.key()) == expected.end()) return false;
    }
    return true;
}

bool HasString(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string();
}

struct SemVersion {
    std::array<std::uint32_t, 3> components{};
};

bool ParseSemVersion(std::string_view text, SemVersion& output) {
    std::size_t begin = 0;
    for (std::size_t component = 0; component < output.components.size(); ++component) {
        const std::size_t end = component + 1 == output.components.size()
            ? text.size()
            : text.find('.', begin);
        if (end == std::string_view::npos || end == begin) return false;
        const std::string_view token = text.substr(begin, end - begin);
        if (token.size() > 1 && token.front() == '0') return false;
        std::uint64_t value = 0;
        for (char digit : token) {
            if (digit < '0' || digit > '9') return false;
            value = value * 10 + static_cast<unsigned>(digit - '0');
            if (value > std::numeric_limits<std::uint32_t>::max()) return false;
        }
        output.components[component] = static_cast<std::uint32_t>(value);
        begin = end + 1;
    }
    return begin == text.size() + 1;
}

bool IsNewer(const SemVersion& candidate, const SemVersion& current) {
    return candidate.components > current.components;
}

bool IsHexDigest(std::string_view digest) {
    return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char value) {
        return HexDigit(value) >= 0;
    });
}

std::string ChannelName(UpdateChannel channel) {
    return channel == UpdateChannel::Stable ? "stable" : "testing";
}

}  // namespace

ManifestResult VerifyAndParseManifest(std::string_view manifestBytes,
                                      std::string_view signatureHex,
                                      std::string_view publicKeyHex,
                                      const ManifestPolicy& policy) {
    std::vector<unsigned char> signature;
    if (!DecodeHex(signatureHex, crypto_sign_BYTES, signature)) {
        return Failure(ManifestError::SignatureEncodingInvalid,
                       "signature must be 64-byte hexadecimal Ed25519 data");
    }

    std::vector<unsigned char> publicKey;
    if (!DecodeHex(publicKeyHex, crypto_sign_PUBLICKEYBYTES, publicKey)) {
        return Failure(ManifestError::PublicKeyEncodingInvalid,
                       "public key must be 32-byte hexadecimal Ed25519 data");
    }

    if (!VerifySignature(manifestBytes, signature, publicKey)) {
        return Failure(ManifestError::SignatureInvalid,
                       "manifest signature verification failed");
    }

    const json root = json::parse(manifestBytes, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return Failure(ManifestError::JsonInvalid, "manifest is not valid JSON");
    }

    if (!HasOnlyKeys(root, {"schema_version", "product", "channel", "version",
                            "platform", "architecture", "minimum_bootstrap_version",
                            "minimum_direct_version", "data_compatibility", "asset",
                            "published_at", "build_id"})) {
        return Failure(ManifestError::MissingField,
                       "manifest fields do not match schema version 1");
    }
    if (!root["schema_version"].is_number_integer() ||
        root["schema_version"].get<int>() != 1) {
        return Failure(ManifestError::SchemaUnsupported,
                       "manifest schema version is unsupported");
    }
    for (const char* key : {"product", "channel", "version", "platform",
                            "architecture", "minimum_bootstrap_version",
                            "minimum_direct_version", "data_compatibility",
                            "published_at", "build_id"}) {
        if (!HasString(root, key)) {
            return Failure(ManifestError::MissingField,
                           "manifest contains a missing or mistyped string field");
        }
    }
    if (!IsSupportedUpdateProduct(root["product"].get<std::string>())) {
        return Failure(ManifestError::ProductMismatch, "manifest product is not supported");
    }
    if (root["channel"].get<std::string>() != ChannelName(policy.selectedChannel)) {
        return Failure(ManifestError::ChannelMismatch,
                       "manifest channel does not match selected channel");
    }
    if (root["platform"].get<std::string>() != policy.platform) {
        return Failure(ManifestError::PlatformMismatch,
                       "manifest platform does not match this application");
    }
    if (root["architecture"].get<std::string>() != policy.architecture) {
        return Failure(ManifestError::ArchitectureMismatch,
                       "manifest architecture does not match this application");
    }

    const std::string version = root["version"].get<std::string>();
    const std::string minimumBootstrap = root["minimum_bootstrap_version"].get<std::string>();
    const std::string minimumDirect = root["minimum_direct_version"].get<std::string>();
    SemVersion targetVersion;
    SemVersion currentVersion;
    SemVersion minimumBootstrapVersion;
    SemVersion minimumDirectVersion;
    if (!ParseSemVersion(version, targetVersion) ||
        !ParseSemVersion(policy.currentVersion, currentVersion) ||
        !ParseSemVersion(minimumBootstrap, minimumBootstrapVersion) ||
        !ParseSemVersion(minimumDirect, minimumDirectVersion)) {
        return Failure(ManifestError::VersionInvalid,
                       "manifest and policy versions must be strict numeric semver");
    }
    if (!IsNewer(targetVersion, currentVersion)) {
        return Failure(ManifestError::NotNewer,
                       "manifest version is not newer than the current version");
    }

    if (root["data_compatibility"].get<std::string>() != "backward_compatible") {
        return Failure(ManifestError::DataCompatibilityUnsupported,
                       "automatic update requires backward-compatible data");
    }

    const auto& asset = root["asset"];
    if (!HasOnlyKeys(asset, {"url", "size", "sha256"}) ||
        !HasString(asset, "url") || !HasString(asset, "sha256") ||
        !asset["size"].is_number_unsigned()) {
        return Failure(ManifestError::AssetInvalid,
                       "manifest asset fields are missing or mistyped");
    }
    const std::string assetUrl = asset["url"].get<std::string>();
    const std::string githubPrefix = "https://github.com/" + policy.allowedRepository +
        "/releases/download/";
    const bool githubApproved = !policy.allowedRepository.empty() &&
        assetUrl.rfind(githubPrefix, 0) == 0;
    const bool companyApproved =
        assetUrl.rfind(ARIS_UPDATE_COMPANY_RELEASE_DOWNLOAD_PREFIX, 0) == 0;
    if (!githubApproved && !companyApproved) {
        return Failure(ManifestError::RepositoryNotAllowed,
                       "manifest asset is outside the approved repository");
    }
    const std::uint64_t assetSize = asset["size"].get<std::uint64_t>();
    const std::string assetDigest = asset["sha256"].get<std::string>();
    if (assetSize == 0 || !IsHexDigest(assetDigest)) {
        return Failure(ManifestError::AssetInvalid,
                       "manifest asset size or SHA-256 is invalid");
    }

    ManifestResult result;
    result.manifest.schemaVersion = 1;
    result.manifest.channel = policy.selectedChannel;
    result.manifest.version = version;
    result.manifest.minimumBootstrapVersion = minimumBootstrap;
    result.manifest.minimumDirectVersion = minimumDirect;
    result.manifest.dataCompatibility = DataCompatibility::BackwardCompatible;
    result.manifest.asset.url = assetUrl;
    result.manifest.asset.size = assetSize;
    result.manifest.asset.sha256 = assetDigest;
    result.manifest.publishedAt = root["published_at"].get<std::string>();
    result.manifest.buildId = root["build_id"].get<std::string>();
    return result;
}

}  // namespace aris::update
