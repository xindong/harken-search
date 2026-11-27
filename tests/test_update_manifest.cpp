#include "update/UpdateManifest.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

// Deterministic test-only Ed25519 key material. This is not a release key.
const std::string kManifest =
    R"({"schema_version":1,"product":"aris","channel":"testing","version":"1.2.0","platform":"windows","architecture":"x64","minimum_bootstrap_version":"1.0.0","minimum_direct_version":"1.0.0","data_compatibility":"backward_compatible","asset":{"url":"https://github.com/xindong/aris/releases/download/v1.2.0/Aris-1.2.0-win-x64-update.zip","size":42,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"published_at":"2026-07-28T00:00:00Z","build_id":"abc123"})";
const std::string kLegacyManifest =
    R"({"schema_version":1,"product":"aris","channel":"testing","version":"1.2.0","platform":"windows","architecture":"x64","minimum_bootstrap_version":"1.0.0","minimum_direct_version":"1.0.0","data_compatibility":"backward_compatible","asset":{"url":"https://github.com/zhongziyu-cmd/aris-releases/releases/download/v1.2.0/Aris-1.2.0-win-x64-update.zip","size":42,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"published_at":"2026-07-28T00:00:00Z","build_id":"abc123"})";
const std::string kPublicKey =
    "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8";
const std::string kSignature =
    "df358ea2f5dc1c212ee302fd9242466b419d0cb8a8bcd5e9c7fa16edd68ccbc6"
    "f2bf563e107ad05792ffa58bc6e9eb6160b42044c982dc83b9b26fbf44d5f40a";
const std::string kLegacySignature =
    "9f1ab98c5ac351f6c599dd6f2cf45878b10edf215dd3adcaab301d95ea31f8ad"
    "bb619ac47de838392cc0b03978537cb87d266632336a195fc83a57e0f6b9f30a";
const std::string kV2Signature =
    "081ee0d467d88df3461bb7ad7fdd7f040c63f0db7386ab359b15ef6532b236e9"
    "86dc79dfa1287d7ce615a4b77ee8100d045e285f14b638df7e6cacde6f50fd01";
const std::string kForeignSignature =
    "14521d1a2649abcf9eb5a73e7eced54b4cbb1e960bebc5ea93c17b3b62d9236a"
    "ff01888a940f03ae77cdf4f717030552540cc73cc5c173109b0aa07e09320706";

std::string ManifestWithProduct(const std::string& product) {
    std::string manifest = kManifest;
    const std::string legacyProduct = "\"product\":\"aris\"";
    manifest.replace(manifest.find(legacyProduct), legacyProduct.size(),
                     "\"product\":\"" + product + "\"");
    return manifest;
}
const std::string kCompanyCloudManifest =
    R"({"architecture":"x64","asset":{"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size":42,"url":"https://harkensearch.xd.com/aris/releases/download/v1.2.0/Aris-1.2.0-win-x64-update.zip"},"build_id":"abc123","channel":"testing","data_compatibility":"backward_compatible","minimum_bootstrap_version":"1.0.0","minimum_direct_version":"1.0.0","platform":"windows","product":"aris","published_at":"2026-07-28T00:00:00Z","schema_version":1,"version":"1.2.0"})";
const std::string kCompanyCloudPublicKey =
    "be995aa21a029720aeda921d77d32c3999de15308ebf88e245fa880c49adb4dd";
const std::string kCompanyCloudSignature =
    "e7ba1552e9a4f49d321cb90557469cdc0a49f57b5c0bd1349966d9d2c5f18264"
    "d1b624ceb5d8d8cc30c2249e35027106e3c5aa52e92e3655f6203efa4de30f08";

}  // namespace

int main() {
    aris::update::ManifestPolicy policy;
    policy.currentVersion = "1.1.0";
    policy.selectedChannel = aris::update::UpdateChannel::Testing;
    policy.platform = "windows";
    policy.architecture = "x64";
    policy.allowedRepository = "xindong/aris";

    const auto valid = aris::update::VerifyAndParseManifest(
        kManifest, kSignature, kPublicKey, policy);
    check(valid.ok(), "valid signed testing manifest is accepted");
    check(valid.manifest.version == "1.2.0", "version is owned by parsed result");
    check(valid.manifest.asset.size == 42, "asset size is preserved");
    const auto v2Manifest = ManifestWithProduct("harken-search");
    check(aris::update::VerifyAndParseManifest(
              v2Manifest, kV2Signature, kPublicKey, policy).ok(),
          "signed harken-search manifest is accepted alongside legacy identity");
    check(aris::update::VerifyAndParseManifest(
              v2Manifest, kSignature, kPublicKey, policy).error ==
              aris::update::ManifestError::SignatureInvalid,
          "changing the product invalidates the existing signature");
    check(aris::update::VerifyAndParseManifest(
              ManifestWithProduct("other-product"), kForeignSignature, kPublicKey,
              policy).error == aris::update::ManifestError::ProductMismatch,
          "valid signature does not admit an unrelated product");
    check(aris::update::VerifyAndParseManifest(
              kLegacyManifest, kLegacySignature, kPublicKey, policy).error ==
              aris::update::ManifestError::RepositoryNotAllowed,
          "validly signed legacy repository asset is rejected by canonical admission");
    check(aris::update::VerifyAndParseManifest(
              kCompanyCloudManifest, kCompanyCloudSignature, kCompanyCloudPublicKey,
              policy).ok(),
          "validly signed company cloud asset is accepted by canonical admission");

    std::string tampered = kManifest;
    tampered.replace(tampered.find("1.2.0"), 5, "9.9.9");
    check(aris::update::VerifyAndParseManifest(
              tampered, kSignature, kPublicKey, policy).error ==
              aris::update::ManifestError::SignatureInvalid,
          "tampered bytes fail before semantic parsing");

    auto stablePolicy = policy;
    stablePolicy.selectedChannel = aris::update::UpdateChannel::Stable;
    check(aris::update::VerifyAndParseManifest(
              kManifest, kSignature, kPublicKey, stablePolicy).error ==
              aris::update::ManifestError::ChannelMismatch,
          "testing manifest cannot enter stable channel");

    auto downgradePolicy = policy;
    downgradePolicy.currentVersion = "1.3.0";
    check(aris::update::VerifyAndParseManifest(
              kManifest, kSignature, kPublicKey, downgradePolicy).error ==
              aris::update::ManifestError::NotNewer,
          "downgrade and same-version updates are refused");

    return failures == 0 ? 0 : 1;
}
