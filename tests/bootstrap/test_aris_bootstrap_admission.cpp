#include "bootstrap/HarkenSearchBootstrapAdmission.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool AcceptFixtureSignature(std::string_view message,
                            std::string_view signatureHex,
                            std::string_view keyId,
                            std::string* diagnostic) {
    const bool accepted = !message.empty() &&
                          signatureHex == std::string(128, 'b') &&
                          keyId == "fixture-2026";
    if (!accepted && diagnostic != nullptr) *diagnostic = "fixture mismatch";
    return accepted;
}

bool AcceptFlexibleFixtureSignature(std::string_view message,
                                    std::string_view signatureHex,
                                    std::string_view keyId,
                                    std::string* diagnostic) {
    const bool accepted = !message.empty() &&
        (signatureHex == std::string(128, 'b') ||
         signatureHex == std::string(128, 'c')) &&
        (keyId == "fixture-2026" || keyId == "fixture-2027");
    if (!accepted && diagnostic != nullptr) *diagnostic = "fixture mismatch";
    return accepted;
}

std::string validPackage() {
    return R"({"asset_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","asset_size":3,"build_id":"build-1","channel":"testing","entrypoint":"HarkenSearch.exe","inventory":[{"byte_size":3,"relative_path":"HarkenSearch.exe","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}],"key_id":"fixture-2026","product":"aris","removals":[],"schema_version":1,"version":"1.2.3"})";
}

std::string packageWithRemoval() {
    std::string package = validPackage();
    const std::string from = R"("removals":[])";
    const std::string to = R"("removals":["obsolete.dll"])";
    const std::size_t offset = package.find(from);
    if (offset != std::string::npos) package.replace(offset, from.size(), to);
    return package;
}

std::string packageWithInventoryPath(const std::string& relativePath) {
    std::string package = validPackage();
    std::string encodedPath;
    encodedPath.reserve(relativePath.size());
    for (const char character : relativePath) {
        if (character == '\\') encodedPath.push_back('\\');
        encodedPath.push_back(character);
    }
    const std::string needle =
        R"("inventory":[{"byte_size":3,"relative_path":"HarkenSearch.exe","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])";
    const std::string replacement =
        R"("inventory":[{"byte_size":3,"relative_path":"HarkenSearch.exe","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},{"byte_size":1,"relative_path":")" +
        encodedPath +
        R"(","sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}])";
    const std::size_t offset = package.find(needle);
    if (offset != std::string::npos) package.replace(offset, needle.size(), replacement);
    return package;
}

std::string packageWithAlternateEntrypoint() {
    std::string package = validPackage();
    const std::string needle =
        R"("inventory":[{"byte_size":3,"relative_path":"HarkenSearch.exe","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])";
    const std::string replacement =
        R"("inventory":[{"byte_size":3,"relative_path":"HarkenSearch.exe","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},{"byte_size":4,"relative_path":"Other.exe","sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}])";
    const std::size_t offset = package.find(needle);
    if (offset != std::string::npos) package.replace(offset, needle.size(), replacement);
    return package;
}

std::string replacePackageField(std::string package,
                                const std::string& from,
                                const std::string& to) {
    const std::size_t offset = package.find(from);
    if (offset != std::string::npos) package.replace(offset, from.size(), to);
    return package;
}

aris::bootstrap::BootstrapProcessIdentity processIdentity(std::uint64_t pid) {
    aris::bootstrap::BootstrapProcessIdentity identity;
    identity.processId = pid;
    identity.creationTime = 1000U + pid;
    identity.imagePath = "C:/HarkenSearchTest/Aris/HarkenSearch.exe";
    identity.imageVolumeSerial = 7U;
    identity.imageFileId.fill(static_cast<std::uint8_t>(pid));
    identity.valid = true;
    return identity;
}

aris::bootstrap::InvocationEnvelope validInvocation(
    const std::string& packageDigest) {
    aris::bootstrap::InvocationEnvelope invocation;
    invocation.transactionId = "txn-1";
    invocation.deploymentMode = aris::platform::DeploymentMode::Portable;
    invocation.installationId = "portable-1";
    invocation.targetRoot = "C:/HarkenSearchTest/Aris";
    invocation.updaterOwnedBase = "C:/HarkenSearchTest/HarkenSearchUpdates";
    invocation.stagingRoot = "C:/HarkenSearchTest/HarkenSearchUpdates/staging";
    invocation.transactionRoot = "C:/HarkenSearchTest/HarkenSearchUpdates/transaction";
    invocation.protectedDataRoots = {
        "C:/HarkenSearchTest/Roaming/HarkenSearchAudio",
        "C:/HarkenSearchTest/Local/Auro/Database"};
    invocation.invocationIdentity = "parent-capability-1";
    invocation.packagePlanDigest = packageDigest;
    invocation.healthNonce = std::string(64, 'c');
    invocation.oldProcessId = 41;
    invocation.entrypoint = "HarkenSearch.exe";
    invocation.parentProcess = processIdentity(41);
    invocation.parentProcess.imagePath =
        "C:/HarkenSearchTest/HarkenSearchUpdates/HarkenSearchUpdater.exe";
    invocation.oldProcess = processIdentity(41);
    invocation.targetRootIdentity = {"C:/HarkenSearchTest/Aris", 7U, {}, true};
    invocation.targetRootIdentity.fileId.fill(9U);
    invocation.oldExecutableIdentity = {
        invocation.oldProcess.imagePath,
        invocation.oldProcess.imageVolumeSerial,
        invocation.oldProcess.imageFileId,
        true};
    invocation.existingInventory = {{"HarkenSearch.exe"}, {"obsolete.dll"}};
    return invocation;
}

void test_package_signature_and_closed_schema() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64, '1');
    trust.provisioned = true;

    const auto accepted = aris::bootstrap::VerifyPackagePlan(
        validPackage(), std::string(128, 'b'), trust, AcceptFixtureSignature);
    check(accepted.ok(), "a verified package plan is admitted");
    check(accepted.plan.has_value() && accepted.plan->buildId == "build-1",
          "admission returns owned package values");
    check(accepted.plan.has_value() && accepted.plan->removals.empty(),
          "the Aris product fixture carries no release-signed removals");
    check(accepted.packageDigest.size() == 64,
          "admission computes a bounded SHA-256 package digest");
    check(accepted.packageBytes == validPackage() &&
              accepted.packageSignatureHex == std::string(128, 'b'),
          "accepted admission retains exact signed bytes for durable recovery");

    auto extra = validPackage();
    extra.insert(extra.size() - 1, ",\"unexpected\":true");
    const auto rejected = aris::bootstrap::VerifyPackagePlan(
        extra, std::string(128, 'b'), trust, AcceptFixtureSignature);
    check(rejected.status ==
              aris::bootstrap::PackagePlanAdmissionStatus::InvalidSchema,
          "extra package fields fail closed before semantic use");

    trust.provisioned = false;
    const auto unprovisioned = aris::bootstrap::VerifyPackagePlan(
        validPackage(), std::string(128, 'b'), trust, AcceptFixtureSignature);
    check(unprovisioned.status ==
              aris::bootstrap::PackagePlanAdmissionStatus::UnprovisionedKey,
          "an unprovisioned production key cannot admit a package");
}

void test_aris_product_policy_denies_signed_removals_and_user_data_inventory() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64, '1');
    trust.provisioned = true;

    const auto safe = aris::bootstrap::VerifyPackagePlanBinding(
        validPackage(), std::string(128, 'b'), trust, AcceptFixtureSignature);
    check(safe.ok() && safe.binding.has_value(),
          "safe signed package produces an immutable binding");
    if (safe.ok() && safe.binding.has_value()) {
        const auto policy = aris::bootstrap::EvaluateHarkenSearchPackagePolicy(*safe.binding);
        check(policy.status == aris::bootstrap::HarkenSearchPackagePolicyStatus::Accepted,
              "empty removals and executable-only inventory pass Aris policy");
    }

    const auto removal = aris::bootstrap::VerifyPackagePlanBinding(
        packageWithRemoval(), std::string(128, 'b'), trust, AcceptFixtureSignature);
    check(removal.ok() && removal.binding.has_value(),
          "release signature verification remains distinct from product delete authority");
    if (removal.ok() && removal.binding.has_value()) {
        const auto policy = aris::bootstrap::EvaluateHarkenSearchPackagePolicy(*removal.binding);
        check(policy.status ==
                  aris::bootstrap::HarkenSearchPackagePolicyStatus::RemovalsForbidden,
              "a valid signature does not authorize any removal effect");
    }

    const std::vector<std::string> protectedPaths{
        "settings.ini", "Config\\Settings-window.INI", "imgui.ini",
        "Config/ImGui-layout.InI", "config/WWISE_TRANSFER.JSON",
        "data/Auro.db", "data/Auro.db-WAL", "data/Auro.db.shm",
        "Data\\Auro.dbx.db", "Data\\Auro.dbx.db-WAL",
        "audio/source.WAV", "audio/source.mp3", "audio/source.FLAC",
        "audio/source.ogg", "audio/source.M4A", "audio/source.aac",
        "audio/source.WMA", "audio/source.opus", "audio/source.AIFF",
        "audio/source.aif"};
    for (const std::string& protectedPath : protectedPaths) {
        const auto binding = aris::bootstrap::VerifyPackagePlanBinding(
            packageWithInventoryPath(protectedPath), std::string(128, 'b'),
            trust, AcceptFixtureSignature);
        check(binding.ok() && binding.binding.has_value(),
              "protected inventory fixture remains structurally signed");
        if (!binding.ok() || !binding.binding.has_value()) continue;
        const auto policy = aris::bootstrap::EvaluateHarkenSearchPackagePolicy(*binding.binding);
        check(policy.status ==
                  aris::bootstrap::HarkenSearchPackagePolicyStatus::ProtectedInventory,
              "case-insensitive slash-normalized user data inventory is denied");
    }
}

void test_updater_handoff_requires_bootstrap_issued_capability() {
    static_assert(!std::is_default_constructible_v<
                      aris::bootstrap::VerifiedPackagePlanBinding>,
                  "a caller must not default-construct signed authority");
    static_assert(!std::is_default_constructible_v<
                      aris::bootstrap::InvocationCapability>,
                  "a caller must not default-construct invocation authority");
    static_assert(!std::is_copy_constructible_v<
                      aris::bootstrap::InvocationCapability>,
                  "an invocation capability must be one-shot");
    static_assert(!std::is_copy_assignable_v<
                      aris::bootstrap::InvocationCapability>,
                  "an invocation capability must not be replayed by copy");

    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64, '1');
    trust.provisioned = true;
    const auto verified = aris::bootstrap::VerifyPackagePlanBinding(
        validPackage(), std::string(128, 'b'), trust,
        AcceptFlexibleFixtureSignature);
    check(verified.ok() && verified.binding.has_value(),
          "release verification returns an immutable signed-plan binding");
    if (!verified.ok() || !verified.binding.has_value()) return;
    const auto& binding = *verified.binding;
    check(binding.packagePlanDigest().size() == 64U &&
              binding.packageSignatureDigest().size() == 64U &&
              binding.keyId() == "fixture-2026" &&
              binding.version() == "1.2.3" &&
              binding.buildId() == "build-1" &&
              binding.channel() == "testing" &&
              binding.assetSha256() == std::string(64, 'a') &&
              binding.assetSize() == 3U &&
              binding.approvedArchiveEntries() ==
                  std::vector<std::string>{"HarkenSearch.exe"},
           "binding exposes only exact immutable package authority");
    check(binding.approvedArchiveItems().size() == 1U &&
              binding.approvedArchiveItems().front().relativePath == "HarkenSearch.exe" &&
              binding.approvedArchiveItems().front().byteSize == 3U &&
              binding.approvedArchiveItems().front().sha256 ==
                  std::string(64, 'a'),
          "binding exposes exact signed archive size and digest metadata");

    const auto invocation = validInvocation(binding.packagePlanDigest());
    auto authenticatedParent = invocation.parentProcess;
    for (char& character : authenticatedParent.imagePath) {
        if (character == '/') character = '\\';
    }
    auto capability = aris::bootstrap::IssueInvocationCapabilityForTesting(
        binding, invocation, authenticatedParent);
    check(!aris::bootstrap::AuthorizedBootstrapArchivePath(capability).empty(),
          "capability accepts equivalent slash spelling for the authenticated parent image");
    const std::string archive =
        aris::bootstrap::AuthorizedBootstrapArchivePath(capability);
    check(archive ==
              "C:/HarkenSearchTest/HarkenSearchUpdates/staging/HarkenSearch-1.2.3-win-x64-update.zip",
          "Bootstrap derives the only accepted archive path");
    const auto accepted = aris::bootstrap::SubmitVerifiedPackagePlan(
        binding, archive, std::move(capability));
    check(accepted.ok(),
          "verified package plus Bootstrap-issued capability is admitted");
    const auto replay = aris::bootstrap::SubmitVerifiedPackagePlan(
        binding, archive, std::move(capability));
    check(replay.status() ==
              aris::bootstrap::BootstrapHandoffSubmissionStatus::InvalidCapability,
          "a consumed invocation capability cannot be replayed");

    auto wrongArchiveCapability =
        aris::bootstrap::IssueInvocationCapabilityForTesting(
            binding, invocation, invocation.parentProcess);
    const auto wrongArchive = aris::bootstrap::SubmitVerifiedPackagePlan(
    binding, "C:/HarkenSearchTest/other/update.zip",
        std::move(wrongArchiveCapability));
    check(wrongArchive.status() ==
              aris::bootstrap::BootstrapHandoffSubmissionStatus::ArchivePathRejected,
          "caller-selected archive paths fail closed");

    aris::bootstrap::PackagePlanTrustConfig otherTrust = trust;
    otherTrust.keyId = "fixture-2027";
    const auto otherKey = aris::bootstrap::VerifyPackagePlanBinding(
        replacePackageField(validPackage(), "fixture-2026", "fixture-2027"),
        std::string(128, 'b'), otherTrust, AcceptFlexibleFixtureSignature);
    auto keyCapability = aris::bootstrap::IssueInvocationCapabilityForTesting(
        binding, invocation, invocation.parentProcess);
    const auto keyRejected = aris::bootstrap::SubmitVerifiedPackagePlan(
        *otherKey.binding, archive, std::move(keyCapability));
    check(keyRejected.status() ==
              aris::bootstrap::BootstrapHandoffSubmissionStatus::PackageKeyMismatch,
          "a different signed key id cannot replace capability authority");

    const auto otherEntries = aris::bootstrap::VerifyPackagePlanBinding(
        packageWithAlternateEntrypoint(), std::string(128, 'b'), trust,
        AcceptFlexibleFixtureSignature);
    auto entriesCapability =
        aris::bootstrap::IssueInvocationCapabilityForTesting(
            binding, invocation, invocation.parentProcess);
    const auto entriesRejected = aris::bootstrap::SubmitVerifiedPackagePlan(
        *otherEntries.binding, archive, std::move(entriesCapability));
    check(entriesRejected.status() ==
              aris::bootstrap::BootstrapHandoffSubmissionStatus::ArchiveEntriesMismatch,
          "different ZIP entry names cannot replace capability authority");

    const auto otherDigest = aris::bootstrap::VerifyPackagePlanBinding(
        replacePackageField(validPackage(), "1.2.3", "1.2.4"),
        std::string(128, 'b'), trust, AcceptFlexibleFixtureSignature);
    auto digestCapability =
        aris::bootstrap::IssueInvocationCapabilityForTesting(
            binding, invocation, invocation.parentProcess);
    const auto digestRejected = aris::bootstrap::SubmitVerifiedPackagePlan(
        *otherDigest.binding, archive, std::move(digestCapability));
    check(digestRejected.status() ==
              aris::bootstrap::BootstrapHandoffSubmissionStatus::PackageDigestMismatch,
          "a different signed package digest cannot replace capability authority");

    const auto otherSignature = aris::bootstrap::VerifyPackagePlanBinding(
        validPackage(), std::string(128, 'c'), trust,
        AcceptFlexibleFixtureSignature);
    auto signatureCapability =
        aris::bootstrap::IssueInvocationCapabilityForTesting(
            binding, invocation, invocation.parentProcess);
    const auto signatureRejected = aris::bootstrap::SubmitVerifiedPackagePlan(
        *otherSignature.binding, archive, std::move(signatureCapability));
    check(signatureRejected.status() ==
              aris::bootstrap::BootstrapHandoffSubmissionStatus::PackageSignatureMismatch,
          "a different detached signature cannot replace capability authority");
}

void test_production_verifier_rejects_small_order_public_keys() {
    const std::string forgedIdentitySignature =
        "01" + std::string(63U * 2U, '0');
    constexpr std::array<std::string_view, 14U> publicKeys = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0000000000000000000000000000000000000000000000000000000000000080",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000080",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eaa78aad612",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eaa78aad692",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    };
    std::size_t caseIndex = 0U;
    for (const std::string_view publicKey : publicKeys) {
        aris::bootstrap::PackagePlanTrustConfig trust;
        trust.keyId = "fixture-2026";
        trust.publicKeyHex = std::string(publicKey);
        trust.provisioned = true;
        const auto rejected = aris::bootstrap::VerifyPackagePlan(
            validPackage(), forgedIdentitySignature, trust);
        if (rejected.status !=
            aris::bootstrap::PackagePlanAdmissionStatus::InvalidKey) {
            ++failures;
            std::cerr << "FAIL: strict Ed25519 public-key case " << caseIndex
                      << " was not rejected before signature verification\n";
        }
        ++caseIndex;
    }
}

std::string handoffWithBroker(const std::string& brokerJson) {
    return std::string(R"({"invocation":"{}","launch_broker":)") + brokerJson +
        R"(,"package_plan":"{}","package_signature":")" +
        std::string(128U, 'b') + R"("})";
}

void test_handoff_requires_exact_inherited_launch_broker_capability() {
    const std::string identity =
        R"({"creation_time":2001,"file_id":"11111111111111111111111111111111","image_path":"C:/Program Files/Aris/HarkenSearchBootstrap.exe","image_volume_serial":7,"process_id":91})";
    const std::string broker =
        std::string(R"({"channel_handle":11,"containment_job_handle":13,"launch_timeout_ms":5000,"process_handle":12,"process_identity":)") +
        identity + "}";
    const auto accepted = aris::bootstrap::DecodeBootstrapHandoff(
        handoffWithBroker(broker));
    check(accepted.ok() && accepted.handoff.has_value() &&
              accepted.handoff->launchBroker.channelHandle == 11U &&
              accepted.handoff->launchBroker.brokerProcessHandle == 12U &&
              accepted.handoff->launchBroker.containmentJobHandle == 13U &&
              accepted.handoff->launchBroker.launchTimeoutMs == 5000U &&
              accepted.handoff->launchBroker.brokerIdentity.processId == 91U,
          "handoff admits one exact inherited broker capability tuple");

    const std::string missingBroker =
        std::string(R"({"invocation":"{}","package_plan":"{}","package_signature":")") +
        std::string(128U, 'b') + R"("})";
    check(aris::bootstrap::DecodeBootstrapHandoff(missingBroker).status ==
              aris::bootstrap::BootstrapHandoffDecodeStatus::InvalidSchema,
          "handoff without a launch broker fails closed");

    const std::string zeroHandleBroker =
        std::string(R"({"channel_handle":0,"containment_job_handle":13,"launch_timeout_ms":5000,"process_handle":12,"process_identity":)") +
        identity + "}";
    check(aris::bootstrap::DecodeBootstrapHandoff(
              handoffWithBroker(zeroHandleBroker)).status ==
              aris::bootstrap::BootstrapHandoffDecodeStatus::InvalidField,
          "handoff rejects a missing inherited broker handle");

    const std::string unboundedBroker =
        std::string(R"({"channel_handle":11,"containment_job_handle":13,"launch_timeout_ms":60001,"process_handle":12,"process_identity":)") +
        identity + "}";
    check(aris::bootstrap::DecodeBootstrapHandoff(
              handoffWithBroker(unboundedBroker)).status ==
              aris::bootstrap::BootstrapHandoffDecodeStatus::InvalidField,
          "handoff rejects an unbounded broker launch deadline");

    std::string substitutedBroker = broker;
    substitutedBroker.insert(substitutedBroker.size() - 1U,
                             R"(,"unreviewed_handle":14)");
    check(aris::bootstrap::DecodeBootstrapHandoff(
              handoffWithBroker(substitutedBroker)).status ==
              aris::bootstrap::BootstrapHandoffDecodeStatus::InvalidSchema,
          "handoff rejects unreviewed broker capability fields");
}

void test_invocation_digest_binding() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64, '1');
    trust.provisioned = true;
    const auto package = aris::bootstrap::VerifyPackagePlan(
        validPackage(), std::string(128, 'b'), trust, AcceptFixtureSignature);
    check(package.ok(), "fixture package remains valid for invocation binding");

    auto invocation = validInvocation(package.packageDigest);
    const auto admitted = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, invocation);
    check(admitted.ok(), "OS-authenticated invocation binds to package digest");
    check(admitted.plan.has_value() &&
              admitted.plan->invocation.transactionId == "txn-1",
          "combined admission preserves transaction capability values");
    check(admitted.plan.has_value() &&
              admitted.plan->packagePlanBytes == validPackage() &&
              admitted.plan->packageSignatureHex == std::string(128, 'b'),
          "combined plan carries the immutable signed package reference");
    check(admitted.plan.has_value() && admitted.plan->removals.empty() &&
              admitted.journal.has_value() && admitted.journal->items.size() == 2U &&
              admitted.journal->items[0].classification ==
                  aris::bootstrap::JournalItemClass::ReplaceExisting &&
              admitted.journal->items[1].classification ==
                  aris::bootstrap::JournalItemClass::PreserveExisting,
          "zero-removal policy preserves target extras instead of deleting them");

    invocation.packagePlanDigest.assign(64, 'd');
    const auto mismatch = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, invocation);
    check(mismatch.status ==
              aris::bootstrap::BootstrapAdmissionStatus::PackageDigestMismatch,
          "a forged invocation digest cannot redirect the signed package");
}

void test_signed_entrypoint_cannot_be_replaced_by_invocation() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64, '1');
    trust.provisioned = true;
    const auto package = aris::bootstrap::VerifyPackagePlan(
        packageWithAlternateEntrypoint(), std::string(128, 'b'), trust,
        AcceptFixtureSignature);
    check(package.ok(), "package with two inventory entries is valid");

    auto invocation = validInvocation(package.packageDigest);
    invocation.entrypoint = "Other.exe";
    const auto rejected = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, invocation);
    check(rejected.status ==
              aris::bootstrap::BootstrapAdmissionStatus::InvalidPlan,
          "invocation cannot replace the signed package entrypoint path");
}

void test_parent_and_old_process_require_exact_identity_tuple() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64, '1');
    trust.provisioned = true;
    const auto package = aris::bootstrap::VerifyPackagePlan(
        validPackage(), std::string(128, 'b'), trust, AcceptFixtureSignature);
    auto invocation = validInvocation(package.packageDigest);
    invocation.oldProcess.processId++;
    const auto rejected = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, invocation);
    check(rejected.status ==
              aris::bootstrap::BootstrapAdmissionStatus::InvalidInvocation,
          "old process PID must bind to its supplied identity tuple");
}

void test_distinct_updater_parent_is_bound_to_its_fixed_host_identity() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64, '1');
    trust.provisioned = true;
    const auto package = aris::bootstrap::VerifyPackagePlan(
        validPackage(), std::string(128, 'b'), trust, AcceptFixtureSignature);
    auto invocation = validInvocation(package.packageDigest);
    invocation.parentProcess.processId = 7U;
    invocation.parentProcess.creationTime = 1007U;
    invocation.parentProcess.imagePath =
        "C:/HarkenSearchTest/HarkenSearchUpdates/HarkenSearchUpdater.exe";
    const auto admitted = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, invocation);
    check(admitted.ok(),
          "a distinct updater parent identity is admitted by the pure boundary");

    invocation.parentProcess.imageFileId[0] ^= 1U;
    const auto rejected = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, invocation);
    check(rejected.ok(),
          "pure admission leaves observed parent FileId binding to the channel boundary");

    // The authenticated parent is not required to use a fictional fixed
    // filename. Native channel admission binds its exact lifetime/image
    // identity and production signer; the pure value boundary accepts a
    // legitimate updater host path outside this fixture directory.
    invocation = validInvocation(package.packageDigest);
    invocation.parentProcess.imagePath = "C:/Program Files/Aris/HarkenSearch.exe";
    const auto alternateHost = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, invocation);
    check(alternateHost.ok(),
          "pure admission accepts an authenticated updater host path without a hard-coded filename");
}

void test_direct_invocation_is_bounded_and_validated() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64, '1');
    trust.provisioned = true;
    const auto package = aris::bootstrap::VerifyPackagePlan(
        validPackage(), std::string(128, 'b'), trust, AcceptFixtureSignature);

    auto malformedIdentity = validInvocation(package.packageDigest);
    malformedIdentity.invocationIdentity = "forged~identity";
    const auto identityRejected = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, malformedIdentity);
    check(identityRejected.status ==
              aris::bootstrap::BootstrapAdmissionStatus::InvalidInvocation,
          "direct admission rejects an invalid invocation identity");

    auto oversizedRoots = validInvocation(package.packageDigest);
    oversizedRoots.protectedDataRoots.assign(257U, "C:/HarkenSearchTest/HarkenSearchData");
    const auto rootsRejected = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, oversizedRoots);
    check(rootsRejected.status ==
              aris::bootstrap::BootstrapAdmissionStatus::InvalidInvocation,
          "direct admission bounds protected data roots before plan construction");

    for (auto invalidRoots : std::vector<std::vector<std::string>>{
             {}, {""}, {"relative/data"},
             {"C:/HarkenSearchTest/Data", "c:\\harkensearchtest\\data"},
             {"C:/HarkenSearchTest/Aris/Database", "C:/HarkenSearchTest/Roaming"},
             {"C:/HarkenSearchTest/HarkenSearchUpdates", "C:/HarkenSearchTest/Roaming"},
             {"C:/HarkenSearchTest/HarkenSearchUpdates/staging/cache", "C:/HarkenSearchTest/Roaming"},
             {"C:/HarkenSearchTest/HarkenSearchUpdates/transaction", "C:/HarkenSearchTest/Roaming"}}) {
        auto invalid = validInvocation(package.packageDigest);
        invalid.protectedDataRoots = std::move(invalidRoots);
        const auto rejected = aris::bootstrap::AdmitBootstrapInvocationForTesting(
            package, invalid);
        check(rejected.status ==
                  aris::bootstrap::BootstrapAdmissionStatus::InvalidInvocation,
              "protected roots must be two distinct absolute non-overlapping roots");
    }

    auto distinctRoots = validInvocation(package.packageDigest);
    distinctRoots.protectedDataRoots = {
        "C:/HarkenSearchTest/Data", "c:\\aristest\\data"};
    const auto distinctRootsAccepted =
        aris::bootstrap::AdmitBootstrapInvocationForTesting(package, distinctRoots);
    check(distinctRootsAccepted.status ==
              aris::bootstrap::BootstrapAdmissionStatus::Accepted,
          "different absolute data roots remain valid across legacy product names");

    auto excessiveFailures = validInvocation(package.packageDigest);
    excessiveFailures.previousFailureCount = 3U;
    const auto failureCountRejected = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, excessiveFailures);
    check(failureCountRejected.status ==
              aris::bootstrap::BootstrapAdmissionStatus::InvalidInvocation,
          "failure count is bounded before it can bypass the two-attempt quarantine policy");
}

void test_process_identity_pid_fits_native_width() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64, '1');
    trust.provisioned = true;
    const auto package = aris::bootstrap::VerifyPackagePlan(
        validPackage(), std::string(128, 'b'), trust, AcceptFixtureSignature);
    auto invocation = validInvocation(package.packageDigest);
    invocation.oldProcessId = 0x1'0000'0000ULL;
    invocation.oldProcess.processId = invocation.oldProcessId;
    invocation.parentProcess.processId = invocation.oldProcessId;
    const auto rejected = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, invocation);
    check(rejected.status ==
              aris::bootstrap::BootstrapAdmissionStatus::InvalidInvocation,
          "process IDs wider than the native DWORD boundary are rejected");
}

void test_durable_package_authority_rejects_missing_or_forged_reference() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64, '1');
    trust.provisioned = true;
    const auto package = aris::bootstrap::VerifyPackagePlan(
        validPackage(), std::string(128, 'b'), trust, AcceptFixtureSignature);
    check(package.ok(), "durable-authority fixture package is admitted");
    if (!package.ok()) return;

    const auto invocation = validInvocation(package.packageDigest);
    const auto admitted = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, invocation);
    check(admitted.ok(), "durable-authority fixture invocation is admitted");
    if (!admitted.ok()) return;

    const auto accepted = aris::bootstrap::ValidateDurablePackageAuthority(
        *admitted.plan, package.packageBytes, package.packageSignatureHex,
        trust, AcceptFixtureSignature);
    check(accepted.ok(),
          "durable recovery accepts the exact previously verified package reference");

    const auto missingBytes = aris::bootstrap::ValidateDurablePackageAuthority(
        *admitted.plan, {}, package.packageSignatureHex, trust,
        AcceptFixtureSignature);
    check(!missingBytes.ok(),
          "durable recovery rejects a journal with missing package bytes");

    const auto missingSignature = aris::bootstrap::ValidateDurablePackageAuthority(
        *admitted.plan, package.packageBytes, {}, trust, AcceptFixtureSignature);
    check(!missingSignature.ok(),
          "durable recovery rejects a journal with missing package signature");

    std::string substitutedBytes = package.packageBytes;
    substitutedBytes[substitutedBytes.size() / 2U] =
        substitutedBytes[substitutedBytes.size() / 2U] == 'a' ? 'b' : 'a';
    const auto substituted = aris::bootstrap::ValidateDurablePackageAuthority(
        *admitted.plan, substitutedBytes, package.packageSignatureHex, trust,
        AcceptFixtureSignature);
    check(!substituted.ok(),
          "durable recovery rejects package-byte substitution even with a valid envelope checksum");

    const auto forgedSignature = aris::bootstrap::ValidateDurablePackageAuthority(
        *admitted.plan, package.packageBytes, std::string(128, 'c'), trust,
        AcceptFixtureSignature);
    check(!forgedSignature.ok(),
          "durable recovery rejects detached-signature substitution");
}

}  // namespace

int main() {
    test_package_signature_and_closed_schema();
    test_aris_product_policy_denies_signed_removals_and_user_data_inventory();
    test_updater_handoff_requires_bootstrap_issued_capability();
    test_production_verifier_rejects_small_order_public_keys();
    test_handoff_requires_exact_inherited_launch_broker_capability();
    test_invocation_digest_binding();
    test_signed_entrypoint_cannot_be_replaced_by_invocation();
    test_parent_and_old_process_require_exact_identity_tuple();
    test_distinct_updater_parent_is_bound_to_its_fixed_host_identity();
    test_direct_invocation_is_bounded_and_validated();
    test_process_identity_pid_fits_native_width();
    test_durable_package_authority_rejects_missing_or_forged_reference();
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "HarkenSearchBootstrap admission tests passed\n";
    return 0;
}
