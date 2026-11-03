#include "bootstrap/HarkenSearchBootstrapMain.h"
#include "bootstrap/HarkenSearchBootstrapAdmission.h"
#include "bootstrap/HarkenSearchBootstrapRuntime.h"
#include "bootstrap/WindowsBootstrapFilesystem.h"

#include "nlohmann_json.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace aris::bootstrap {
BootstrapExitCode mapResult(const BootstrapApplicationResult& result);
int FilesystemMutationStatusFromReason(const std::string& reason, int kind);
std::string FormatTerminalFilesystemAdmissionFailure(
    const WindowsBootstrapFilesystemRecoveryResult& recovery);
// The product links the persistence diagnostic writer. This focused entrypoint
// test checks exit mapping without writing into a real user's update directory.
void RecordBootstrapStageFailure(const char*, int, const char*) {}
}

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string fileIdHex(const aris::bootstrap::BootstrapProcessIdentity& identity) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(identity.imageFileId.size() * 2U);
    for (const std::uint8_t byte : identity.imageFileId) {
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

nlohmann::json processIdentityJson(
    const aris::bootstrap::BootstrapProcessIdentity& identity) {
    return {{"creation_time", identity.creationTime},
            {"file_id", fileIdHex(identity)},
            {"image_path", identity.imagePath},
            {"image_volume_serial", identity.imageVolumeSerial},
            {"process_id", identity.processId}};
}

nlohmann::json deploymentIdentityJson(
    const aris::platform::DeploymentFileIdentity& identity) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string fileId;
    for (const auto byte : identity.fileId) {
        fileId.push_back(digits[byte >> 4U]);
        fileId.push_back(digits[byte & 0x0fU]);
    }
    return {{"canonical_path", identity.canonicalPath},
            {"file_id", fileId},
            {"volume_serial", identity.volumeSerial}};
}

std::string encodeInvocation(
    const aris::bootstrap::InvocationEnvelope& invocation) {
    nlohmann::json existingInventory = nlohmann::json::array();
    for (const auto& item : invocation.existingInventory) {
        existingInventory.push_back({{"relative_path", item.relativePath}});
    }
    return nlohmann::json{
        {"deployment_mode", invocation.deploymentMode ==
            aris::platform::DeploymentMode::Installed ? "installed" : "portable"},
        {"entrypoint", invocation.entrypoint},
        {"existing_inventory", std::move(existingInventory)},
        {"health_nonce", invocation.healthNonce},
        {"installation_id", invocation.installationId},
        {"invocation_identity", invocation.invocationIdentity},
        {"old_process", processIdentityJson(invocation.oldProcess)},
        {"old_executable_identity", deploymentIdentityJson(
            invocation.oldExecutableIdentity)},
        {"old_process_id", invocation.oldProcessId},
        {"package_plan_digest", invocation.packagePlanDigest},
        {"parent_process", processIdentityJson(invocation.parentProcess)},
        {"previous_failure_count", invocation.previousFailureCount},
        {"protected_data_roots", invocation.protectedDataRoots},
        {"registered_app_id", invocation.registeredAppId},
        {"staging_root", invocation.stagingRoot},
        {"target_root", invocation.targetRoot},
        {"target_root_identity", deploymentIdentityJson(
            invocation.targetRootIdentity)},
        {"transaction_id", invocation.transactionId},
        {"transaction_root", invocation.transactionRoot},
        {"updater_owned_base", invocation.updaterOwnedBase},
    }.dump();
}

bool AcceptFixtureSignature(std::string_view message,
                            std::string_view signatureHex,
                            std::string_view keyId,
                            std::string* diagnostic) {
    const bool accepted = !message.empty() &&
                          signatureHex == std::string(128U, 'b') &&
                          keyId == "fixture-2026";
    if (!accepted && diagnostic != nullptr) *diagnostic = "fixture mismatch";
    return accepted;
}

std::string validPackage() {
    return R"({"asset_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","asset_size":3,"build_id":"build-1","channel":"testing","entrypoint":"Aris.exe","inventory":[{"byte_size":3,"relative_path":"Aris.exe","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}],"key_id":"fixture-2026","product":"aris","removals":[],"schema_version":1,"version":"1.2.3"})";
}

std::string packageWithSignedRemoval() {
    std::string package = validPackage();
    const std::string emptyRemovals = R"("removals":[])";
    const std::size_t offset = package.find(emptyRemovals);
    if (offset != std::string::npos) {
        package.replace(offset, emptyRemovals.size(),
                        R"("removals":["obsolete.dll"])");
    }
    return package;
}

aris::bootstrap::BootstrapProcessIdentity processIdentity(
    const std::uint64_t processId,
    const std::uint64_t creationTime,
    const std::string& imagePath,
    const std::uint8_t fileIdByte) {
    aris::bootstrap::BootstrapProcessIdentity identity;
    identity.processId = processId;
    identity.creationTime = creationTime;
    identity.imagePath = imagePath;
    identity.imageVolumeSerial = 7U;
    identity.imageFileId.fill(fileIdByte);
    identity.valid = true;
    return identity;
}

aris::bootstrap::PackagePlanTrustConfig fixtureTrust() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex = std::string(64U, '1');
    trust.provisioned = true;
    return trust;
}

aris::bootstrap::InvocationEnvelope validInvocation(
    const std::string& packageDigest) {
    aris::bootstrap::InvocationEnvelope invocation;
    invocation.transactionId = "txn-recovery-1";
    invocation.deploymentMode = aris::platform::DeploymentMode::Portable;
    invocation.installationId = "portable-1";
    invocation.targetRoot = "C:/HarkenSearchTest/Aris";
    invocation.updaterOwnedBase = "C:/HarkenSearchTest/HarkenSearchUpdates";
    invocation.stagingRoot = "C:/HarkenSearchTest/HarkenSearchUpdates/staging";
    invocation.transactionRoot = "C:/HarkenSearchTest/HarkenSearchUpdates/transaction";
    invocation.invocationIdentity = "parent-capability-1";
    invocation.packagePlanDigest = packageDigest;
    invocation.healthNonce = std::string(64U, 'c');
    invocation.oldProcessId = 41U;
    invocation.entrypoint = "Aris.exe";
    invocation.parentProcess = processIdentity(
        7U, 1007U, "C:/HarkenSearchTest/HarkenSearchUpdates/HarkenSearchUpdater.exe", 0x17U);
    invocation.oldProcess = processIdentity(
        41U, 1041U, "C:/HarkenSearchTest/Aris/Aris.exe", 0x29U);
    invocation.targetRootIdentity = {"C:/HarkenSearchTest/Aris", 7U, {}, true};
    invocation.targetRootIdentity.fileId.fill(0x31U);
    invocation.oldExecutableIdentity = {
        invocation.oldProcess.imagePath,
        invocation.oldProcess.imageVolumeSerial,
        invocation.oldProcess.imageFileId,
        true};
    invocation.protectedDataRoots = {"C:/HarkenSearchTest/HarkenSearchData"};
    invocation.existingInventory = {{"Aris.exe"}};
    return invocation;
}

struct RecoveryFixture {
    aris::bootstrap::PackagePlanTrustConfig trust;
    aris::bootstrap::InvocationEnvelope invocation;
    aris::bootstrap::WindowsBootstrapJournalRecord record;
    bool valid = false;
};

RecoveryFixture recoveryFixture(const std::string& packageBytes = validPackage()) {
    RecoveryFixture fixture;
    fixture.trust = fixtureTrust();
    const std::string signature(128U, 'b');
    const auto package = aris::bootstrap::VerifyPackagePlan(
        packageBytes, signature, fixture.trust, AcceptFixtureSignature);
    if (!package.ok()) return fixture;
    fixture.invocation = validInvocation(package.packageDigest);
    const auto admitted = aris::bootstrap::AdmitBootstrapInvocationForTesting(
        package, fixture.invocation);
    if (!admitted.ok() || !admitted.plan.has_value() ||
        !admitted.journal.has_value()) {
        return fixture;
    }

    const auto& plan = *admitted.plan;
    auto& record = fixture.record;
    record.schemaVersion = 9U;
    record.generation = 1U;
    record.transactionId = plan.invocation.transactionId;
    record.deploymentMode = plan.identity.deploymentMode;
    record.installationId = plan.identity.installationId;
    record.registeredAppId = plan.identity.registeredAppId;
    record.targetRoot = plan.identity.targetRoot;
    record.targetRootIdentity = plan.identity.targetRootIdentity;
    record.oldExecutableIdentity = plan.identity.oldExecutableIdentity;
    record.updaterOwnedBase = plan.invocation.updaterOwnedBase;
    record.stagingRoot = plan.invocation.stagingRoot;
    record.transactionRoot = plan.invocation.transactionRoot;
    record.protectedDataRoots = plan.identity.protectedDataRoots;
    record.invocationIdentity = plan.invocation.invocationIdentity;
    record.version = plan.version;
    record.buildId = plan.buildId;
    record.packagePlanDigest = plan.packagePlanDigest;
    record.packageKeyId = plan.packageKeyId;
    record.packageChannel = plan.packageChannel;
    record.packagePlanBytes = packageBytes;
    record.packageSignatureHex = signature;
    record.healthNonceDigest = aris::bootstrap::BootstrapSha256Hex(
        fixture.invocation.healthNonce);
    record.oldProcessId = plan.oldProcessId;
    record.oldProcessIdentity = plan.invocation.oldProcess;
    record.parentProcessIdentity = plan.invocation.parentProcess;
    record.entrypoint = plan.entrypoint;
    record.phase = "prepared";
    for (const auto& item : admitted.journal->items) {
        aris::bootstrap::WindowsBootstrapJournalItemRecord durableItem;
        durableItem.relativePath = item.relativePath;
        durableItem.classification = item.classification;
        record.items.push_back(std::move(durableItem));
    }
    fixture.valid = true;
    return fixture;
}

aris::bootstrap::BootstrapChannelResult authenticatedChannel(
    const RecoveryFixture& fixture,
    const aris::bootstrap::InvocationEnvelope& invocation) {
    aris::bootstrap::BootstrapChannelResult channel;
    channel.status = aris::bootstrap::BootstrapChannelStatus::Accepted;
    channel.parentProcess = invocation.parentProcess;
    channel.handoff = aris::bootstrap::BootstrapHandoff{
        fixture.record.packagePlanBytes,
        fixture.record.packageSignatureHex,
        encodeInvocation(invocation)};
    return channel;
}

aris::bootstrap::DurableRecoveryAdmissionResult admitAuthenticatedRecovery(
    const RecoveryFixture& fixture,
    const aris::bootstrap::BootstrapChannelResult& channel) {
    aris::bootstrap::BootstrapHandoffSubmission submitted;
    if (channel.ok() && !channel.recoveryMarker && channel.handoff.has_value()) {
        const auto binding = aris::bootstrap::VerifyPackagePlanBinding(
            channel.handoff->packagePlanBytes,
            channel.handoff->packageSignatureHex, fixture.trust,
            AcceptFixtureSignature);
        const auto decoded = aris::bootstrap::DecodeInvocationEnvelope(
            channel.handoff->invocationBytes);
        if (binding.ok() && decoded.ok() && decoded.invocation.has_value()) {
            auto capability =
                aris::bootstrap::IssueInvocationCapabilityForTesting(
                    *binding.binding, *decoded.invocation,
                    channel.parentProcess);
            submitted = aris::bootstrap::SubmitVerifiedPackagePlan(
                *binding.binding,
                aris::bootstrap::AuthorizedBootstrapArchivePath(capability),
                std::move(capability));
        }
    }
    return aris::bootstrap::AdmitAuthenticatedBootstrapRecovery(
        fixture.record, fixture.trust, submitted,
        fixture.record.updaterOwnedBase, fixture.record.stagingRoot,
        fixture.record.transactionRoot, AcceptFixtureSignature);
}

void test_unprovisioned_target_fails_before_channel_use() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    const int code = aris::bootstrap::RunHarkenSearchBootstrapEntrypoint(
        0U, 0U, trust, {});
    check(code == static_cast<int>(aris::bootstrap::BootstrapExitCode::Unprovisioned),
          "unprovisioned composition refuses production handoff deterministically");
}

void test_provisioned_target_rejects_invalid_inherited_channel() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex =
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    trust.provisioned = true;
    const int code = aris::bootstrap::RunHarkenSearchBootstrapEntrypoint(
        0U, 0U, trust, {});
    check(code == static_cast<int>(aris::bootstrap::BootstrapExitCode::ChannelRejected),
          "provisioned composition still requires an OS-authenticated channel");
}

void test_entrypoint_rejects_unbounded_wait_configuration_before_channel_use() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex =
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    trust.provisioned = true;
    const auto expectRejected = [&trust](
        const aris::bootstrap::BootstrapEntrypointOptions& options,
        const std::string_view diagnostic) {
        const int code = aris::bootstrap::RunHarkenSearchBootstrapEntrypoint(
            0U, 0U, trust, options);
        check(code == static_cast<int>(
                          aris::bootstrap::BootstrapExitCode::InvocationRejected),
              diagnostic);
    };

    aris::bootstrap::BootstrapEntrypointOptions options;
    options.channelTimeoutMs = 0U;
    expectRejected(options, "zero channel wait is rejected before native I/O");
    options = {};
    options.channelTimeoutMs = 60'001U;
    expectRejected(options, "unbounded channel wait is rejected before native I/O");
    options = {};
    options.oldProcessWaitMs = 300'001U;
    expectRejected(options, "unbounded old-process wait is rejected before native I/O");
    options = {};
    options.healthWaitMs = 120'001U;
    expectRejected(options, "unbounded health wait is rejected before native I/O");
}

void test_forward_only_product_recovery_requires_reinstall_before_channel_use() {
    aris::bootstrap::PackagePlanTrustConfig trust;
    trust.keyId = "fixture-2026";
    trust.publicKeyHex =
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    trust.provisioned = true;
    aris::bootstrap::BootstrapEntrypointOptions options;
    options.recovery = true;

    const int code = aris::bootstrap::RunHarkenSearchBootstrapEntrypoint(
        0U, 0U, trust, options);
    check(code == static_cast<int>(
                      aris::bootstrap::BootstrapExitCode::ReinstallRequired),
          "forward-only product recovery stops before channel I/O and requires reinstall");
}

void test_recovery_channel_marker_contains_no_dynamic_authority() {
    const auto decoded = aris::bootstrap::DecodeBootstrapRecoveryMarker(
        R"({"recovery":true,"schema":1})");
    check(decoded.ok(),
          "recovery channel accepts only the bounded one-shot mode marker");
    const auto forged = aris::bootstrap::DecodeBootstrapRecoveryMarker(
        R"({"recovery":true,"schema":1,"target_root":"D:/forged"})");
    check(!forged.ok(),
          "recovery channel rejects caller-supplied dynamic roots");
}

void test_marker_and_durable_record_are_not_sufficient_recovery_authority() {
    const RecoveryFixture fixture = recoveryFixture();
    check(fixture.valid, "marker-only recovery fixture is valid");
    if (!fixture.valid) return;

    const auto markerOnly = aris::bootstrap::AdmitDurableBootstrapRecovery(
        fixture.record, fixture.trust, fixture.record.parentProcessIdentity,
        fixture.record.updaterOwnedBase, fixture.record.stagingRoot,
        fixture.record.transactionRoot, AcceptFixtureSignature);
    check(!markerOnly.ok(),
          "a marker and durable journal alone never become restart mutation authority");
}

void test_recovery_requires_fresh_full_authenticated_handoff() {
    const RecoveryFixture fixture = recoveryFixture();
    check(fixture.valid, "full-handoff recovery fixture is valid");
    if (!fixture.valid) return;

    aris::bootstrap::BootstrapChannelResult markerOnly;
    markerOnly.status = aris::bootstrap::BootstrapChannelStatus::Accepted;
    markerOnly.parentProcess = fixture.invocation.parentProcess;
    markerOnly.recoveryMarker = true;
    const auto rejected = admitAuthenticatedRecovery(fixture, markerOnly);
    check(!rejected.ok(),
          "restart admission rejects a marker even when a durable journal exists");
}

void test_recovery_accepts_new_parent_lifetime_for_exact_same_image() {
    const RecoveryFixture fixture = recoveryFixture();
    check(fixture.valid, "parent-rebind recovery fixture is valid");
    if (!fixture.valid) return;

    auto invocation = fixture.invocation;
    invocation.parentProcess.processId = 19U;
    invocation.parentProcess.creationTime = 2019U;
    const auto admitted = admitAuthenticatedRecovery(
        fixture, authenticatedChannel(fixture, invocation));
    check(admitted.ok(),
          "restart admits a fresh parent lifetime only for the exact durable updater image");
    check(admitted.plan.has_value() &&
              admitted.plan->invocation.parentProcess == invocation.parentProcess,
          "restart binds the current authenticated parent PID and creation time");
    check(admitted.plan.has_value() &&
              admitted.plan->healthToken == invocation.healthNonce &&
              admitted.plan->invocation.healthNonceDigest ==
                  fixture.record.healthNonceDigest,
          "restart recovers the raw health capability only from its matching fresh preimage");
}

void test_schema_seven_recovery_requires_reinstall_without_migration() {
    auto fixture = recoveryFixture();
    check(fixture.valid, "schema-seven rejection fixture is valid");
    if (!fixture.valid) return;
    fixture.record.schemaVersion = 7U;
    const auto rejected = admitAuthenticatedRecovery(
        fixture, authenticatedChannel(fixture, fixture.invocation));
    check(!rejected.ok(),
          "schema seven is not migrated or guessed into deployment identity authority");
}

void test_schema_ten_recovery_keeps_authenticated_handoff_required() {
    auto fixture = recoveryFixture();
    check(fixture.valid, "schema-ten recovery fixture is valid");
    if (!fixture.valid) return;
    fixture.record.schemaVersion = 10U;
    check(admitAuthenticatedRecovery(
              fixture, authenticatedChannel(fixture, fixture.invocation)).ok(),
          "schema-ten directory transactions accept the exact fresh authenticated handoff");
    const auto marker = aris::bootstrap::AdmitDurableBootstrapRecovery(
        fixture.record, fixture.trust, fixture.record.parentProcessIdentity,
        fixture.record.updaterOwnedBase, fixture.record.stagingRoot,
        fixture.record.transactionRoot, AcceptFixtureSignature);
    check(marker.status == aris::bootstrap::DurableRecoveryAdmissionStatus::FreshHandoffRequired,
          "schema-ten journals alone never authorize recovery mutations");
}

void test_recovery_rehydrates_signed_removal_with_backup_authority() {
    const RecoveryFixture fixture = recoveryFixture(packageWithSignedRemoval());
    check(fixture.valid, "signed-removal recovery fixture is valid");
    if (!fixture.valid) return;

    const auto admitted = admitAuthenticatedRecovery(
        fixture, authenticatedChannel(fixture, fixture.invocation));
    check(admitted.ok(),
          "authenticated restart rehydrates a release-signed removal");
    if (!admitted.journal.has_value()) return;

    const auto removal = std::find_if(
        admitted.journal->items.begin(), admitted.journal->items.end(),
        [](const aris::bootstrap::ReplacementJournalItem& item) {
            return item.classification ==
                   aris::bootstrap::JournalItemClass::RemoveExisting;
        });
    check(removal != admitted.journal->items.end() && removal->requiresBackup,
          "signed removal retains durable backup authority after restart");
}

void test_recovery_rejects_fresh_package_or_digest_substitution() {
    const RecoveryFixture fixture = recoveryFixture();
    check(fixture.valid, "package-rebind recovery fixture is valid");
    if (!fixture.valid) return;

    auto digestSubstitution = fixture.invocation;
    digestSubstitution.packagePlanDigest.assign(64U, 'd');
    check(!admitAuthenticatedRecovery(
               fixture, authenticatedChannel(fixture, digestSubstitution)).ok(),
          "restart rejects a fresh invocation whose package digest differs from the journal");

    auto packageSubstitution = authenticatedChannel(fixture, fixture.invocation);
    const std::string original = "build-1";
    const std::size_t offset =
        packageSubstitution.handoff->packagePlanBytes.find(original);
    check(offset != std::string::npos,
          "package substitution fixture locates the signed immutable field");
    if (offset != std::string::npos) {
        packageSubstitution.handoff->packagePlanBytes.replace(
            offset, original.size(), "build-2");
        check(!admitAuthenticatedRecovery(fixture, packageSubstitution).ok(),
              "restart rejects different signed package bytes even when the fixture verifier accepts them");
    }

    auto signatureSubstitution = authenticatedChannel(fixture, fixture.invocation);
    signatureSubstitution.handoff->packageSignatureHex.assign(128U, 'c');
    check(!admitAuthenticatedRecovery(fixture, signatureSubstitution).ok(),
          "restart rejects a substituted detached package signature");
}

void test_recovery_rejects_dynamic_invocation_binding_substitution() {
    const RecoveryFixture fixture = recoveryFixture();
    check(fixture.valid, "dynamic-binding recovery fixture is valid");
    if (!fixture.valid) return;

    const auto expectRejected = [&fixture](
        const aris::bootstrap::InvocationEnvelope& invocation,
        const std::string_view message) {
        check(!admitAuthenticatedRecovery(
                   fixture, authenticatedChannel(fixture, invocation)).ok(),
              message);
    };

    auto target = fixture.invocation;
    target.targetRoot = "C:/HarkenSearchTest/OtherAris";
    target.oldProcess.imagePath = "C:/HarkenSearchTest/OtherAris/Aris.exe";
    expectRejected(target, "restart rejects a substituted target root");

    auto staging = fixture.invocation;
    staging.stagingRoot = "C:/HarkenSearchTest/HarkenSearchUpdates/other-staging";
    expectRejected(staging, "restart rejects a substituted staging root");

    auto transactionRoot = fixture.invocation;
    transactionRoot.transactionRoot =
        "C:/HarkenSearchTest/HarkenSearchUpdates/other-transaction";
    expectRejected(transactionRoot,
                   "restart rejects a substituted transaction root");

    auto protectedData = fixture.invocation;
    protectedData.protectedDataRoots = {"C:/HarkenSearchTest/OtherHarkenSearchData"};
    expectRejected(protectedData,
                   "restart rejects substituted protected-data roots");

    auto transaction = fixture.invocation;
    transaction.transactionId = "txn-recovery-2";
    expectRejected(transaction, "restart rejects a substituted transaction id");

    auto invocationIdentity = fixture.invocation;
    invocationIdentity.invocationIdentity = "parent-capability-2";
    expectRejected(invocationIdentity,
                   "restart rejects a substituted invocation identity");

    auto installation = fixture.invocation;
    installation.installationId = "portable-2";
    expectRejected(installation,
                   "restart rejects a substituted installation identity");
}

void test_recovery_rejects_health_capability_preimage_substitution() {
    const RecoveryFixture fixture = recoveryFixture();
    check(fixture.valid, "health-preimage recovery fixture is valid");
    if (!fixture.valid) return;

    auto invocation = fixture.invocation;
    invocation.healthNonce.assign(64U, 'd');
    const auto rejected = admitAuthenticatedRecovery(
        fixture, authenticatedChannel(fixture, invocation));
    check(!rejected.ok(),
          "restart rejects a raw health capability whose digest differs from the journal");
}

void test_recovery_rejects_parent_image_or_channel_substitution() {
    const RecoveryFixture fixture = recoveryFixture();
    check(fixture.valid, "parent-substitution recovery fixture is valid");
    if (!fixture.valid) return;

    auto substitutedImage = fixture.invocation;
    substitutedImage.parentProcess.processId = 19U;
    substitutedImage.parentProcess.creationTime = 2019U;
    substitutedImage.parentProcess.imageFileId[0] ^= 1U;
    check(!admitAuthenticatedRecovery(
               fixture, authenticatedChannel(fixture, substitutedImage)).ok(),
          "restart rejects a fresh parent whose updater image FileId differs from the journal");

    auto currentInvocation = fixture.invocation;
    currentInvocation.parentProcess.processId = 19U;
    currentInvocation.parentProcess.creationTime = 2019U;
    auto channel = authenticatedChannel(fixture, currentInvocation);
    ++channel.parentProcess.creationTime;
    check(!admitAuthenticatedRecovery(fixture, channel).ok(),
          "restart rejects an invocation parent tuple that differs from the authenticated channel peer");
}

}  // namespace

void test_filesystem_mutation_reason_parser_and_mapping() {
    using namespace aris::bootstrap;
    const std::string oldReason = "filesystem-mutation-failed-1-19";
    const std::string newReason =
        "filesystem-mutation-failed-1-19|backend=24|native=5";

    check(FilesystemMutationStatusFromReason(oldReason, 1) == 19,
          "old reason parses status 19");
    check(FilesystemMutationStatusFromReason(newReason, 1) == 19,
          "new reason with backend/native still parses status 19");
    check(100 + FilesystemMutationStatusFromReason(oldReason, 1) == 119,
          "old reason status 19 maps to exit 119");
    check(100 + FilesystemMutationStatusFromReason(newReason, 1) == 119,
          "new reason status 19 maps to exit 119");

    BootstrapApplicationResult kind2{};
    kind2.status = BootstrapApplicationStatus::Quarantined;
    kind2.reason = "filesystem-mutation-failed-2-19|backend=24|native=5";
    check(mapResult(kind2) == BootstrapExitCode::QuarantinedSnapshotFailed,
          "kind 2 attribution stays QuarantinedSnapshotFailed");

    BootstrapApplicationResult kind3{};
    kind3.status = BootstrapApplicationStatus::Quarantined;
    kind3.reason = "filesystem-mutation-failed-3-19|backend=24|native=5";
    check(mapResult(kind3) == BootstrapExitCode::QuarantinedReplacementFailed,
          "kind 3 attribution stays QuarantinedReplacementFailed");

    BootstrapApplicationResult kind1{};
    kind1.status = BootstrapApplicationStatus::Quarantined;
    kind1.reason = "filesystem-mutation-failed-1-19|backend=24|native=5";
    check(mapResult(kind1) == BootstrapExitCode::QuarantinedMutationFailed,
          "kind 1 without test diagnostics stays QuarantinedMutationFailed");

    BootstrapApplicationResult unconfirmedHealth{};
    unconfirmedHealth.status = BootstrapApplicationStatus::InstalledHealthUnconfirmed;
    unconfirmedHealth.reason = "installed-health-unconfirmed:updated-child-crashed";
    unconfirmedHealth.mutated = true;
    check(mapResult(unconfirmedHealth) == BootstrapExitCode::HealthRejected,
          "replaced files do not turn a crashed restarted child into success");
}

void test_terminal_filesystem_admission_failure_preserves_typed_details() {
    using namespace aris::bootstrap;
    WindowsBootstrapFilesystemRecoveryResult first;
    first.filesystem.status = WindowsBootstrapFilesystemStatus::PartialDurableState;
    first.filesystem.backendStatus = WindowsFilesystemBackendStatus::UnsupportedMetadata;
    first.filesystem.nativeError = 282U;
    const std::string firstExtra = FormatTerminalFilesystemAdmissionFailure(first);
    check(firstExtra ==
              "filesystem.status=" + std::to_string(static_cast<int>(first.filesystem.status)) +
                  "|backendStatus=" +
                  std::to_string(static_cast<int>(first.filesystem.backendStatus)) +
                  "|nativeError=282",
          "terminal admission includes filesystem, backend and native failure codes");

    WindowsBootstrapFilesystemRecoveryResult second;
    second.filesystem.status = WindowsBootstrapFilesystemStatus::IdentityMismatch;
    second.filesystem.backendStatus = WindowsFilesystemBackendStatus::AccessDenied;
    second.filesystem.nativeError = 5U;
    const std::string secondExtra = FormatTerminalFilesystemAdmissionFailure(second);
    check(secondExtra != firstExtra && secondExtra.find("nativeError=5") != std::string::npos,
          "different terminal admission failures remain distinguishable");
}

int main() {
    test_unprovisioned_target_fails_before_channel_use();
    test_filesystem_mutation_reason_parser_and_mapping();
    test_terminal_filesystem_admission_failure_preserves_typed_details();
    test_provisioned_target_rejects_invalid_inherited_channel();
    test_entrypoint_rejects_unbounded_wait_configuration_before_channel_use();
    test_forward_only_product_recovery_requires_reinstall_before_channel_use();
    test_recovery_channel_marker_contains_no_dynamic_authority();
    test_marker_and_durable_record_are_not_sufficient_recovery_authority();
    test_recovery_requires_fresh_full_authenticated_handoff();
    test_recovery_accepts_new_parent_lifetime_for_exact_same_image();
    test_schema_seven_recovery_requires_reinstall_without_migration();
    test_schema_ten_recovery_keeps_authenticated_handoff_required();
    test_recovery_rehydrates_signed_removal_with_backup_authority();
    test_recovery_rejects_fresh_package_or_digest_substitution();
    test_recovery_rejects_dynamic_invocation_binding_substitution();
    test_recovery_rejects_health_capability_preimage_substitution();
    test_recovery_rejects_parent_image_or_channel_substitution();
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "HarkenSearchBootstrap entrypoint tests passed\n";
    return 0;
}
