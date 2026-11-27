#include "bootstrap/HarkenSearchBootstrapPlan.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using aris::bootstrap::AcknowledgeHealth;
using aris::bootstrap::BootstrapIntent;
using aris::bootstrap::BootstrapIntentKind;
using aris::bootstrap::BuildReplacementJournal;
using aris::bootstrap::CommitTransaction;
using aris::bootstrap::ExistingInstallItem;
using aris::bootstrap::FailureDisposition;
using aris::bootstrap::HealthAcknowledgement;
using aris::bootstrap::HealthAcknowledgementResult;
using aris::bootstrap::InstallInventoryItem;
using aris::bootstrap::JournalError;
using aris::bootstrap::JournalItemClass;
using aris::bootstrap::JournalRecordResult;
using aris::bootstrap::NextFailureDisposition;
using aris::bootstrap::OnOldProcessWait;
using aris::bootstrap::PlanError;
using aris::bootstrap::ProcessWaitOutcome;
using aris::bootstrap::RecordBackupSnapshot;
using aris::bootstrap::RecordReplacementWritten;
using aris::bootstrap::RecordUpdatedChildProcess;
using aris::bootstrap::ReplacementJournal;
using aris::bootstrap::RollbackAfterHealthTimeout;
using aris::bootstrap::RollbackForUpdatedChildCrash;
using aris::bootstrap::RollbackTransaction;
using aris::bootstrap::SignatureStatus;
using aris::bootstrap::SignedInstallPlan;
using aris::bootstrap::StartTransaction;
using aris::bootstrap::TransactionPhase;
using aris::bootstrap::UpdatedChildRecordResult;
using aris::bootstrap::ValidateSignedInstallPlan;

int failures = 0;

void check(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

SignedInstallPlan validPlan() {
    SignedInstallPlan plan;
    plan.schemaVersion = 1;
    plan.product = "aris";
    plan.version = "1.2.3";
    plan.buildId = "build-abc123";
    plan.keyId = "testing-2026";
    plan.detachedSignature = std::string(128, 'a');
    plan.payloadSha256 = std::string(64, 'b');
    plan.oldProcessId = 41;
    plan.healthToken = std::string(64, 'c');
    plan.identity.targetRoot = "D:\\Tools\\Aris";
    plan.identity.installationId = "portable-aris-01";
    plan.identity.targetRootIdentity = {"D:/Tools/Aris", 7U, {}, true};
    plan.identity.targetRootIdentity.fileId.fill(0x22U);
    plan.identity.protectedDataRoots = {
        "C:\\Users\\test-user\\AppData\\Local\\Aris",
        "C:\\Users\\test-user\\Documents\\Aris Data",
    };
    plan.invocation.updaterOwnedBase = "D:\\Trusted\\HarkenSearchUpdater";
    plan.invocation.stagingRoot = "D:\\Trusted\\HarkenSearchUpdater\\staging";
    plan.invocation.transactionRoot =
        "D:\\Trusted\\HarkenSearchUpdater\\transaction";
    plan.invocation.invocationIdentity = "signed-invocation-01";
    plan.invocation.deploymentMode = plan.identity.deploymentMode;
    plan.invocation.installationId = plan.identity.installationId;
    plan.invocation.targetRootIdentityBinding =
        plan.identity.targetRootIdentity.canonicalPath;
    plan.invocation.oldExecutableIdentityBinding = "D:/Tools/Aris/Aris.exe";
    plan.identity.oldExecutableIdentity = {
        "D:/Tools/Aris/Aris.exe", 7U, {}, true};
    plan.identity.oldExecutableIdentity.fileId.fill(0x33U);
    plan.invocation.parentProcess.valid = true;
    plan.invocation.parentProcess.processId = plan.oldProcessId;
    plan.invocation.parentProcess.creationTime = 10'041U;
    plan.invocation.parentProcess.imagePath =
        plan.identity.oldExecutableIdentity.canonicalPath;
    plan.invocation.parentProcess.imageVolumeSerial =
        plan.identity.oldExecutableIdentity.volumeSerial;
    plan.invocation.parentProcess.imageFileId =
        plan.identity.oldExecutableIdentity.fileId;
    plan.invocation.oldProcess = plan.invocation.parentProcess;
    plan.entrypoint = "Aris.exe";
    plan.inventory = {
        {"Aris.exe", std::string(64, 'd'), 1024},
        {"resources\\core.pak", std::string(64, 'e'), 2048},
    };
    plan.removals = {"obsolete.dll"};
    return plan;
}

const aris::bootstrap::ReplacementJournalItem* findJournalItem(
    const ReplacementJournal& journal,
    const std::string& path) {
    for (const auto& item : journal.items) {
        if (item.relativePath == path) return &item;
    }
    return nullptr;
}

bool hasIntent(const std::vector<BootstrapIntent>& intents,
               BootstrapIntentKind kind,
               const std::string& path = "") {
    for (const auto& intent : intents) {
        if (intent.kind == kind && (path.empty() || intent.relativePath == path)) {
            return true;
        }
    }
    return false;
}

bool hasNoReplacementIntent(const std::vector<BootstrapIntent>& intents) {
    return !hasIntent(intents, BootstrapIntentKind::SnapshotBackup) &&
           !hasIntent(intents, BootstrapIntentKind::ReplaceInventoryItem) &&
           !hasIntent(intents, BootstrapIntentKind::RestoreBackup) &&
           !hasIntent(intents, BootstrapIntentKind::RemoveNewItem);
}

bool isStrictRelative(const std::string& path) {
    return path.empty() ||
           (path.find("..") == std::string::npos &&
            path.find(':') == std::string::npos &&
            path.front() != '/' && path.front() != '\\');
}

void test_strict_signed_plan_and_install_identity_validation() {
    const auto valid = ValidateSignedInstallPlan(validPlan(), SignatureStatus::Verified);
    check(valid.ok(), "valid authenticated plan is accepted");
    auto v2Plan = validPlan();
    v2Plan.product = "harken-search";
    check(ValidateSignedInstallPlan(v2Plan, SignatureStatus::Verified).ok(),
          "authenticated harken-search install plan is accepted");
    v2Plan.product = "other-product";
    check(ValidateSignedInstallPlan(v2Plan, SignatureStatus::Verified).error ==
              PlanError::InvalidProduct,
          "unrelated product is rejected by install planning");
    check(valid.plan.has_value(), "validated plan is an owned value");
    if (valid.plan) {
        check(valid.plan->identity.targetRoot == "D:/Tools/Aris",
              "validated target root is normalized without filesystem access");
        check(valid.plan->inventory.at(1).relativePath == "resources/core.pak",
              "validated inventory uses install-root-relative normalized paths");
        check(valid.plan->invocation.stagingRoot ==
                  "D:/Trusted/HarkenSearchUpdater/staging" &&
                  valid.plan->invocation.transactionRoot ==
                      "D:/Trusted/HarkenSearchUpdater/transaction",
              "validated signed invocation owns the exact writable roots");
    }

    auto invalidTargetIdentity = validPlan();
    invalidTargetIdentity.identity.targetRootIdentity.valid = false;
    check(!ValidateSignedInstallPlan(invalidTargetIdentity,
                                     SignatureStatus::Verified).ok(),
          "invalid target-root identity is rejected before plan validation");

    auto zeroTargetFileId = validPlan();
    zeroTargetFileId.identity.targetRootIdentity.fileId.fill(0U);
    check(!ValidateSignedInstallPlan(zeroTargetFileId,
                                     SignatureStatus::Verified).ok(),
          "zero target-root file id is rejected before plan validation");

    auto mismatchedTargetIdentityPath = validPlan();
    mismatchedTargetIdentityPath.identity.targetRootIdentity.canonicalPath =
        "D:/Tools/Other";
    mismatchedTargetIdentityPath.invocation.targetRootIdentityBinding =
        mismatchedTargetIdentityPath.identity.targetRootIdentity.canonicalPath;
    check(!ValidateSignedInstallPlan(mismatchedTargetIdentityPath,
                                     SignatureStatus::Verified).ok(),
          "target-root identity must name the normalized target root");

    auto invalidOldExecutableIdentity = validPlan();
    invalidOldExecutableIdentity.identity.oldExecutableIdentity.valid = false;
    check(!ValidateSignedInstallPlan(invalidOldExecutableIdentity,
                                     SignatureStatus::Verified).ok(),
          "invalid old-executable identity is rejected before plan validation");

    auto mismatchedOldExecutableIdentity = validPlan();
    mismatchedOldExecutableIdentity.identity.oldExecutableIdentity.fileId.back() ^= 1U;
    check(!ValidateSignedInstallPlan(mismatchedOldExecutableIdentity,
                                     SignatureStatus::Verified).ok(),
          "old-executable identity must match the authenticated old process");

    check(ValidateSignedInstallPlan(validPlan(), SignatureStatus::Rejected).error ==
              PlanError::SignatureRejected,
          "unverified signed plan is rejected before install planning");

    auto trailingVersionSegment = validPlan();
    trailingVersionSegment.version = "1.2.3.";
    check(ValidateSignedInstallPlan(trailingVersionSegment, SignatureStatus::Verified).error ==
              PlanError::InvalidVersion,
          "version with a trailing empty semver segment is rejected");

    auto emptyPrereleaseSegment = validPlan();
    emptyPrereleaseSegment.version = "1.2.3-rc..1";
    check(ValidateSignedInstallPlan(emptyPrereleaseSegment, SignatureStatus::Verified).error ==
              PlanError::InvalidVersion,
          "version with an empty prerelease segment is rejected");

    auto overflowingVersionComponent = validPlan();
    overflowingVersionComponent.version = "4294967296.0.0";
    check(ValidateSignedInstallPlan(overflowingVersionComponent, SignatureStatus::Verified).error ==
              PlanError::InvalidVersion,
          "version with a component beyond the update contract range is rejected");

    auto diskRoot = validPlan();
    diskRoot.identity.targetRoot = "D:\\";
    check(ValidateSignedInstallPlan(diskRoot, SignatureStatus::Verified).error ==
              PlanError::DiskRootRejected,
          "disk root cannot be an installation target");

    auto systemDirectory = validPlan();
    systemDirectory.identity.targetRoot = "C:\\Windows\\System32\\Aris";
    check(ValidateSignedInstallPlan(systemDirectory, SignatureStatus::Verified).error ==
              PlanError::SystemDirectoryRejected,
          "system directory cannot be an installation target");

    auto dataDirectory = validPlan();
    dataDirectory.identity.targetRoot =
        "C:\\Users\\test-user\\AppData\\Local\\Aris";
    check(ValidateSignedInstallPlan(dataDirectory, SignatureStatus::Verified).error ==
              PlanError::DataDirectoryRejected,
          "declared protected data directory cannot be an installation target");

    auto literalTildeTargetRoot = validPlan();
    literalTildeTargetRoot.identity.targetRoot =
        "C:\\Tools\\R&D~Audio\\Aris";
    literalTildeTargetRoot.identity.targetRootIdentity.canonicalPath =
        "C:/Tools/R&D~Audio/Aris";
    literalTildeTargetRoot.invocation.targetRootIdentityBinding =
        literalTildeTargetRoot.identity.targetRootIdentity.canonicalPath;
    check(ValidateSignedInstallPlan(literalTildeTargetRoot,
                                    SignatureStatus::Verified).ok(),
          "absolute target roots retain literal tilde components after normalization");

    auto literalTildeProtectedRoot = validPlan();
    literalTildeProtectedRoot.identity.protectedDataRoots.front() =
        "C:\\Users\\test-user\\R&D~Audio\\Aris Data";
    check(ValidateSignedInstallPlan(literalTildeProtectedRoot,
                                    SignatureStatus::Verified).ok(),
          "protected absolute roots retain literal tilde components for native identity binding");

    auto escapedInventory = validPlan();
    escapedInventory.inventory.front().relativePath = "..\\settings.db";
    check(ValidateSignedInstallPlan(escapedInventory, SignatureStatus::Verified).error ==
              PlanError::InvalidRelativePath,
          "inventory cannot escape the validated install root");

    auto trailingSeparator = validPlan();
    trailingSeparator.inventory.front().relativePath = "bin\\";
    check(ValidateSignedInstallPlan(trailingSeparator, SignatureStatus::Verified).error ==
              PlanError::InvalidRelativePath,
          "inventory path cannot canonicalize away a trailing separator");

    auto nonAsciiInventory = validPlan();
    nonAsciiInventory.inventory.front().relativePath =
        std::string("r") + "\xC3\x84" + ".exe";
    check(ValidateSignedInstallPlan(nonAsciiInventory, SignatureStatus::Verified).error ==
              PlanError::InvalidRelativePath,
          "non-ASCII inventory names are rejected where portable and NTFS case folds could diverge");

    auto upperShortNameAliasInventory = validPlan();
    upperShortNameAliasInventory.inventory.front().relativePath =
        "bin\\ARIS~1.EXE";
    check(ValidateSignedInstallPlan(upperShortNameAliasInventory,
                                    SignatureStatus::Verified).error ==
              PlanError::InvalidRelativePath,
          "uppercase backslash inventory aliases are rejected before handle-relative mutation");

    auto lowerShortNameAliasInventory = validPlan();
    lowerShortNameAliasInventory.inventory.front().relativePath =
        "bin/aris~1.exe";
    check(ValidateSignedInstallPlan(lowerShortNameAliasInventory,
                                    SignatureStatus::Verified).error ==
              PlanError::InvalidRelativePath,
          "lowercase slash inventory aliases are rejected before path normalization");

    auto unicodeTargetRoot = validPlan();
    unicodeTargetRoot.identity.targetRoot =
        std::string("D:\\Tools\\") + "\xC3\x84" + "ris";
    unicodeTargetRoot.identity.targetRootIdentity.canonicalPath =
        std::string("D:/Tools/") + "\xC3\x84" + "ris";
    unicodeTargetRoot.invocation.targetRootIdentityBinding =
        unicodeTargetRoot.identity.targetRootIdentity.canonicalPath;
    check(ValidateSignedInstallPlan(unicodeTargetRoot,
                                    SignatureStatus::Verified).ok(),
          "absolute Windows roots remain Unicode-capable for real user paths");

    auto invalidAbsoluteCharacter = validPlan();
    invalidAbsoluteCharacter.identity.targetRoot =
        "C:\\Tools\\Bad<Name\\Aris";
    check(ValidateSignedInstallPlan(invalidAbsoluteCharacter,
                                    SignatureStatus::Verified).error ==
              PlanError::InvalidTargetRoot,
          "absolute trusted roots still reject Windows-invalid path characters");

    auto missingInvocation = validPlan();
    missingInvocation.invocation.transactionRoot.clear();
    check(ValidateSignedInstallPlan(missingInvocation,
                                    SignatureStatus::Verified).error ==
              PlanError::InvalidTransactionRoot,
          "a signed plan without a transaction write root is rejected before adapter authority");

    auto overlappingInvocation = validPlan();
    overlappingInvocation.invocation.stagingRoot =
        overlappingInvocation.identity.targetRoot + "\\staging";
    check(ValidateSignedInstallPlan(overlappingInvocation,
                                    SignatureStatus::Verified).error ==
              PlanError::InvocationRootOverlap,
          "signed staging roots cannot overlap the install target");

    auto dataRootInvocation = validPlan();
    dataRootInvocation.invocation.updaterOwnedBase =
        "C:\\Users\\test-user\\AppData\\Local\\Aris";
    dataRootInvocation.invocation.stagingRoot =
        "C:\\Users\\test-user\\AppData\\Local\\Aris\\staging";
    dataRootInvocation.invocation.transactionRoot =
        "C:\\Users\\test-user\\AppData\\Local\\Aris\\transaction";
    check(ValidateSignedInstallPlan(dataRootInvocation,
                                    SignatureStatus::Verified).ok(),
          "signed updater roots may live under an updater-owned data base while remaining disjoint from install");

    auto oversizedInventoryPath = validPlan();
    oversizedInventoryPath.inventory.front().relativePath =
        std::string(4097U, 'a');
    check(ValidateSignedInstallPlan(oversizedInventoryPath,
                                    SignatureStatus::Verified).error ==
              PlanError::InvalidRelativePath,
          "inventory paths are bounded before the filesystem adapter sees them");

    auto oversizedAbsoluteComponent = validPlan();
    oversizedAbsoluteComponent.identity.targetRoot =
        "D:\\Tools\\" + std::string(256U, 'a') + "\\Aris";
    check(ValidateSignedInstallPlan(oversizedAbsoluteComponent,
                                    SignatureStatus::Verified).error ==
              PlanError::InvalidTargetRoot,
          "absolute root components are bounded to Windows directory limits");

    auto traversalLikeInstallationId = validPlan();
    traversalLikeInstallationId.identity.installationId = "..";
    check(ValidateSignedInstallPlan(traversalLikeInstallationId, SignatureStatus::Verified).error ==
              PlanError::InvalidInstallationId,
          "installation identity cannot use a traversal-like identifier");

    auto shortNameAliasInstallationId = validPlan();
    shortNameAliasInstallationId.identity.installationId = "portable~ARIS";
    check(ValidateSignedInstallPlan(shortNameAliasInstallationId,
                                    SignatureStatus::Verified).error ==
              PlanError::InvalidInstallationId,
          "installation identity rejects lexical 8.3 alias markers");

    auto tildeBuildId = validPlan();
    tildeBuildId.buildId = "build~200";
    check(ValidateSignedInstallPlan(tildeBuildId,
                                    SignatureStatus::Verified).error ==
              PlanError::InvalidBuildId,
          "build identity rejects lexical tilde markers");

    auto tildeKeyId = validPlan();
    tildeKeyId.keyId = "testing~2026";
    check(ValidateSignedInstallPlan(tildeKeyId,
                                    SignatureStatus::Verified).error ==
              PlanError::InvalidKeyId,
          "signature key identity rejects lexical tilde markers");
}

void test_deployment_mode_cannot_be_guessed_from_installation_id() {
    auto guessed = validPlan();
    guessed.identity.installationId =
        "installed-a8b3c9d1-e4f5-4a6b-8c9d-0e1f2a3b4c5d";
    guessed.invocation.installationId = guessed.identity.installationId;
    check(!ValidateSignedInstallPlan(guessed, SignatureStatus::Verified).ok(),
          "portable mode cannot be guessed as installed from installation id");

    auto mismatched = validPlan();
    mismatched.invocation.deploymentMode =
        aris::platform::DeploymentMode::Installed;
    check(!ValidateSignedInstallPlan(mismatched, SignatureStatus::Verified).ok(),
          "invocation mode mismatch fails closed");
}

void test_inventory_replacement_journal_classifies_old_new_and_backup_items() {
    const auto validated = ValidateSignedInstallPlan(validPlan(), SignatureStatus::Verified);
    check(validated.ok(), "journal fixture plan validates");
    if (!validated.plan) return;

    const std::vector<ExistingInstallItem> oldInventory = {
        {"Aris.exe"},
        {"obsolete.dll"},
        {"user-notes.txt"},
    };
    const auto journal = BuildReplacementJournal(
        *validated.plan, oldInventory, "attempt-42");
    check(journal.ok(), "validated old inventory produces a replacement journal");
    if (!journal.journal) return;

    const auto* replaced = findJournalItem(*journal.journal, "Aris.exe");
    const auto* added = findJournalItem(*journal.journal, "resources/core.pak");
    const auto* removed = findJournalItem(*journal.journal, "obsolete.dll");
    const auto* preserved = findJournalItem(*journal.journal, "user-notes.txt");
    check(replaced && replaced->classification == JournalItemClass::ReplaceExisting &&
              replaced->requiresBackup,
          "overwritten old item is classified for backup then replacement");
    check(added && added->classification == JournalItemClass::CreateNew &&
              !added->requiresBackup,
          "new item is classified for removal on rollback rather than backup");
    check(removed && removed->classification == JournalItemClass::RemoveExisting &&
              removed->requiresBackup,
          "only an exact signed removal is classified for backup then deletion");
    check(preserved && preserved->classification == JournalItemClass::PreserveExisting &&
              !preserved->requiresBackup,
          "existing item outside signed inventory and removals is preserved");

    const std::vector<ExistingInstallItem> escaped = {{"..\\user-data.db"}};
    check(BuildReplacementJournal(*validated.plan, escaped, "attempt-42").error ==
              JournalError::InvalidExistingPath,
          "journal refuses unvalidated pre-install inventory paths");
    check(BuildReplacementJournal(*validated.plan, oldInventory, "..").error ==
              JournalError::InvalidTransactionId,
          "journal transaction identity cannot be traversal-like");
    check(BuildReplacementJournal(*validated.plan, oldInventory,
                                  "Attempt~42").error ==
              JournalError::InvalidTransactionId,
          "journal transaction identity rejects lexical 8.3 alias markers");
}

void test_process_wait_and_replacement_emit_only_pure_intents() {
    const auto validated = ValidateSignedInstallPlan(validPlan(), SignatureStatus::Verified);
    if (!validated.plan) {
        check(false, "transaction fixture plan validates");
        return;
    }
    const auto journal = BuildReplacementJournal(
        *validated.plan, {{"Aris.exe"}, {"obsolete.dll"}}, "attempt-43");
    if (!journal.journal) {
        check(false, "transaction fixture journal builds");
        return;
    }

    const auto started = StartTransaction(*validated.plan, *journal.journal, 0);
    check(started.state.phase == TransactionPhase::WaitingForOldProcess,
          "transaction starts by waiting for the owned old process");
    check(hasIntent(started.intents, BootstrapIntentKind::WaitForOldProcessExit),
          "transaction emits a process wait intent without touching processes");

    const auto replacing = OnOldProcessWait(started.state, ProcessWaitOutcome::Exited);
    check(replacing.state.phase == TransactionPhase::AwaitingUpdatedChildProcess,
          "exited old process advances to an owned-child launch record");
    check(hasIntent(replacing.intents, BootstrapIntentKind::PersistJournal),
          "replacement persists the inventory journal before mutation");
    check(hasIntent(replacing.intents, BootstrapIntentKind::SnapshotBackup, "Aris.exe"),
          "replacement snapshots each overwritten item");
    check(hasIntent(replacing.intents, BootstrapIntentKind::SnapshotBackup,
                    "obsolete.dll") &&
              hasIntent(replacing.intents,
                        BootstrapIntentKind::RemoveInstalledItem,
                        "obsolete.dll"),
          "signed removals are backed up before a typed delete intent is emitted");
    check(hasIntent(replacing.intents, BootstrapIntentKind::ReplaceInventoryItem, "Aris.exe") &&
              hasIntent(replacing.intents, BootstrapIntentKind::ReplaceInventoryItem,
                        "resources/core.pak"),
          "replacement emits item-level signed-inventory intents");
    check(hasIntent(replacing.intents, BootstrapIntentKind::LaunchUpdatedChild, "Aris.exe"),
          "replacement launches only the signed entrypoint");
    for (const auto& intent : replacing.intents) {
        check(isStrictRelative(intent.relativePath),
              "all replacement intents keep paths install-root-relative");
    }
    const auto childStarted = RecordUpdatedChildProcess(replacing.state, 79);
    check(childStarted.result == UpdatedChildRecordResult::Recorded &&
              childStarted.transaction.state.phase == TransactionPhase::AwaitingHealth &&
              childStarted.transaction.state.updatedChildProcessId == 79 &&
              hasIntent(childStarted.transaction.intents,
                        BootstrapIntentKind::AwaitHealthAcknowledgement),
          "only a recorded update child PID opens the health acknowledgement window");

    const auto timeoutStarted = StartTransaction(*validated.plan, *journal.journal, 1);
    const auto timedOut = OnOldProcessWait(
        timeoutStarted.state, ProcessWaitOutcome::TimedOut);
    check(timedOut.state.phase == TransactionPhase::FailedBeforeMutation,
          "old process timeout fails before any replacement action");
    check(hasNoReplacementIntent(timedOut.intents),
          "old process timeout has no filesystem mutation or rollback intent");
    check(hasIntent(timedOut.intents, BootstrapIntentKind::QuarantinePackage),
          "second failed attempt quarantines instead of retrying indefinitely");
}

void test_health_acknowledgement_and_rollback_policy() {
    const auto validated = ValidateSignedInstallPlan(validPlan(), SignatureStatus::Verified);
    if (!validated.plan) {
        check(false, "health fixture plan validates");
        return;
    }
    const auto journal = BuildReplacementJournal(
        *validated.plan, {{"Aris.exe"}, {"obsolete.dll"}}, "attempt-44");
    if (!journal.journal) {
        check(false, "health fixture journal builds");
        return;
    }
    const auto awaiting = OnOldProcessWait(
        StartTransaction(*validated.plan, *journal.journal, 0).state,
        ProcessWaitOutcome::Exited);

    HealthAcknowledgement healthy;
    healthy.token = validated.plan->healthToken;
    healthy.version = validated.plan->version;
    healthy.buildId = validated.plan->buildId;
    healthy.uiReady = true;

    const auto noChild = RecordUpdatedChildProcess(awaiting.state, 0);
    check(noChild.result == UpdatedChildRecordResult::InvalidProcessId &&
              noChild.transaction.intents.empty(),
          "an unknown update child PID cannot enter the health transaction");

    const auto childBeforeReplacement = RecordUpdatedChildProcess(awaiting.state, 81);
    const auto incomplete = AcknowledgeHealth(childBeforeReplacement.transaction.state, healthy);
    check(incomplete.health == HealthAcknowledgementResult::IncompleteReplacement &&
              incomplete.transaction.state.phase == TransactionPhase::RolledBack,
          "health cannot commit an inventory whose durable replacement records are incomplete");

    const auto backupRecorded = RecordBackupSnapshot(awaiting.state, "Aris.exe");
    check(backupRecorded.result == JournalRecordResult::Recorded &&
              backupRecorded.transaction.state.journal.items.front().backupRecorded,
          "backup progress is recorded durably before an old item is replaced");
    const auto removalBackupRecorded = RecordBackupSnapshot(
        backupRecorded.transaction.state, "obsolete.dll");
    check(removalBackupRecorded.result == JournalRecordResult::Recorded,
          "signed removal backup progress is journaled before deletion");
    const auto existingReplaced = RecordReplacementWritten(
        removalBackupRecorded.transaction.state, "Aris.exe");
    check(existingReplaced.result == JournalRecordResult::Recorded &&
              existingReplaced.transaction.state.journal.items.front().replacementRecorded,
          "overwritten item replacement is journaled after its backup exists");
    const auto newItemReplaced = RecordReplacementWritten(
        existingReplaced.transaction.state, "resources/core.pak");
    check(newItemReplaced.result == JournalRecordResult::Recorded,
          "new item replacement is journaled before health confirmation");
    const auto allReplaced = RecordReplacementWritten(
        newItemReplaced.transaction.state, "obsolete.dll");
    check(allReplaced.result == JournalRecordResult::Recorded,
          "signed removal completion is journaled before health confirmation");
    check(RecordReplacementWritten(allReplaced.transaction.state, "resources/core.pak").result ==
              JournalRecordResult::Idempotent &&
              RecordReplacementWritten(allReplaced.transaction.state, "resources/core.pak")
                  .transaction.intents.empty(),
          "replaying an already journaled replacement is idempotent");
    check(RecordReplacementWritten(allReplaced.transaction.state, "..\\outside.exe").result ==
              JournalRecordResult::InvalidPath,
          "journal progress refuses any path outside the validated inventory");

    auto missingBackupRecord = allReplaced.transaction.state;
    missingBackupRecord.journal.items.front().backupRecorded = false;
    const auto tamperedChild = RecordUpdatedChildProcess(missingBackupRecord, 82);
    const auto corruptJournal = AcknowledgeHealth(tamperedChild.transaction.state, healthy);
    check(corruptJournal.health == HealthAcknowledgementResult::IncompleteReplacement &&
              corruptJournal.transaction.state.phase == TransactionPhase::RecoveryBlocked &&
              !hasIntent(corruptJournal.transaction.intents,
                         BootstrapIntentKind::RelaunchPreviousApplication) &&
              hasIntent(corruptJournal.transaction.intents,
                        BootstrapIntentKind::QuarantinePackage),
          "unrecoverable journal corruption quarantines instead of launching an unproven old app");

    const auto childAfterReplacement = RecordUpdatedChildProcess(allReplaced.transaction.state, 83);
    check(childAfterReplacement.result == UpdatedChildRecordResult::Recorded,
          "successful launch records its owned child PID before health verification");

    const auto accepted = AcknowledgeHealth(childAfterReplacement.transaction.state, healthy);
    check(accepted.health == HealthAcknowledgementResult::Accepted &&
              accepted.transaction.state.phase == TransactionPhase::Committed,
          "matching one-time health token version and build commits the transaction");
    check(hasIntent(accepted.transaction.intents, BootstrapIntentKind::CommitJournal) &&
              hasIntent(accepted.transaction.intents, BootstrapIntentKind::RemoveBackup),
          "health commit emits journal finalization intents");

    const auto replay = AcknowledgeHealth(accepted.transaction.state, healthy);
    check(replay.health == HealthAcknowledgementResult::ReplayRejected &&
              replay.transaction.intents.empty(),
          "health acknowledgement token replay is refused without new action");
    check(CommitTransaction(accepted.transaction.state).intents.empty(),
          "repeating a committed transaction is idempotent");

    auto badToken = healthy;
    badToken.token = std::string(64, 'f');
    const auto tokenMismatch = AcknowledgeHealth(childAfterReplacement.transaction.state, badToken);
    check(tokenMismatch.health == HealthAcknowledgementResult::TokenMismatch &&
              tokenMismatch.transaction.state.phase == TransactionPhase::RolledBack,
          "health token mismatch creates a rollback decision");
    check(hasIntent(tokenMismatch.transaction.intents, BootstrapIntentKind::TerminateUpdatedChild) &&
              hasIntent(tokenMismatch.transaction.intents, BootstrapIntentKind::RestoreBackup,
                        "Aris.exe") &&
              hasIntent(tokenMismatch.transaction.intents,
                        BootstrapIntentKind::RestoreBackup,
                        "obsolete.dll") &&
              hasIntent(tokenMismatch.transaction.intents, BootstrapIntentKind::RemoveNewItem,
                        "resources/core.pak") &&
              hasIntent(tokenMismatch.transaction.intents,
                        BootstrapIntentKind::RelaunchPreviousApplication),
          "rollback terminates only update child, restores backups, cleans new files, and relaunches old app");
    check(tokenMismatch.transaction.intents.at(1).kind == BootstrapIntentKind::TerminateUpdatedChild &&
              tokenMismatch.transaction.intents.at(1).processId == 83,
          "rollback termination is scoped to the recorded update child PID");
    check(RollbackTransaction(tokenMismatch.transaction.state).intents.empty(),
          "repeating a completed rollback is idempotent");

    auto badVersion = healthy;
    badVersion.version = "9.9.9";
    check(AcknowledgeHealth(childAfterReplacement.transaction.state, badVersion).health ==
              HealthAcknowledgementResult::VersionMismatch,
          "health version mismatch is not accepted");

    auto badBuild = healthy;
    badBuild.buildId = "other-build";
    check(AcknowledgeHealth(childAfterReplacement.transaction.state, badBuild).health ==
              HealthAcknowledgementResult::BuildIdMismatch,
          "health build-id mismatch is not accepted");

    check(RollbackAfterHealthTimeout(childAfterReplacement.transaction.state).state.phase ==
              TransactionPhase::RolledBack,
          "health timeout has a rollback plan");
    check(RollbackForUpdatedChildCrash(childAfterReplacement.transaction.state).state.phase ==
              TransactionPhase::RolledBack,
          "updated child crash has a rollback plan");
}

void test_failure_quarantine_threshold() {
    const FailureDisposition first = NextFailureDisposition(0);
    const FailureDisposition second = NextFailureDisposition(1);
    const FailureDisposition later = NextFailureDisposition(2);
    check(first.recordedFailureCount == 1 && !first.quarantine,
          "first failed attempt remains recoverable");
    check(second.recordedFailureCount == 2 && second.quarantine,
          "second failed attempt is quarantined");
    check(later.recordedFailureCount == 3 && later.quarantine,
          "later failures remain quarantined rather than re-enabling retries");
}

void test_validated_values_cannot_be_rebound_or_structurally_tampered() {
    const auto validated = ValidateSignedInstallPlan(validPlan(), SignatureStatus::Verified);
    if (!validated.plan) {
        check(false, "tamper fixture plan validates");
        return;
    }
    const auto journal = BuildReplacementJournal(
        *validated.plan, {{"Aris.exe"}}, "attempt-tamper");
    if (!journal.journal) {
        check(false, "tamper fixture journal builds");
        return;
    }

    auto reboundPlan = *validated.plan;
    reboundPlan.identity.targetRoot = "C:/Windows/System32/Aris";
    const auto reboundPlanStart = StartTransaction(reboundPlan, *journal.journal, 0);
    check(reboundPlanStart.state.phase == TransactionPhase::FailedBeforeMutation &&
              hasNoReplacementIntent(reboundPlanStart.intents),
          "validated plan cannot be rebound to a different install root");

    auto targetFileIdTamper = *validated.plan;
    targetFileIdTamper.identity.targetRootIdentity.fileId.back() ^= 1U;
    check(!targetFileIdTamper.isValidated(),
          "validated plan binding covers the complete target-root file id");

    auto targetValidTamper = *validated.plan;
    targetValidTamper.identity.targetRootIdentity.valid = false;
    check(!targetValidTamper.isValidated(),
          "validated plan binding covers target-root identity validity");

    auto oldFileIdTamper = *validated.plan;
    oldFileIdTamper.identity.oldExecutableIdentity.fileId.back() ^= 1U;
    check(!oldFileIdTamper.isValidated(),
          "validated plan binding covers the complete old-executable file id");

    auto oldValidTamper = *validated.plan;
    oldValidTamper.identity.oldExecutableIdentity.valid = false;
    check(!oldValidTamper.isValidated(),
          "validated plan binding covers old-executable identity validity");

    auto otherRawPlan = validPlan();
    otherRawPlan.identity.targetRoot = "E:\\Portable\\Aris";
    otherRawPlan.identity.targetRootIdentity.canonicalPath =
        "E:/Portable/Aris";
    otherRawPlan.invocation.targetRootIdentityBinding =
        otherRawPlan.identity.targetRootIdentity.canonicalPath;
    const auto otherPlan = ValidateSignedInstallPlan(otherRawPlan, SignatureStatus::Verified);
    const auto otherJournal = otherPlan.plan
                                  ? BuildReplacementJournal(
                                        *otherPlan.plan, {{"Aris.exe"}}, "attempt-other")
                                  : aris::bootstrap::JournalBuildResult{};
    if (!otherPlan.plan || !otherJournal.journal) {
        check(false, "alternate binding fixtures validate");
        return;
    }
    const auto mixedStart = StartTransaction(*validated.plan, *otherJournal.journal, 0);
    check(mixedStart.state.phase == TransactionPhase::FailedBeforeMutation &&
              hasNoReplacementIntent(mixedStart.intents),
          "replacement journal cannot be rebound to another validated install plan");

    auto tamperedJournal = *journal.journal;
    tamperedJournal.items.front().relativePath = "..\\settings.db";
    const auto tamperedStart = StartTransaction(*validated.plan, tamperedJournal, 0);
    check(tamperedStart.state.phase == TransactionPhase::FailedBeforeMutation &&
              hasNoReplacementIntent(tamperedStart.intents),
          "structurally tampered journal is rejected before mutation");

    auto waiting = StartTransaction(*validated.plan, *journal.journal, 0).state;
    waiting.journal.items.front().relativePath = "..\\settings.db";
    const auto tamperedWait = OnOldProcessWait(waiting, ProcessWaitOutcome::Exited);
    check(tamperedWait.state.phase == TransactionPhase::RecoveryBlocked &&
              hasNoReplacementIntent(tamperedWait.intents) &&
              !hasIntent(tamperedWait.intents,
                         BootstrapIntentKind::RelaunchPreviousApplication) &&
              hasIntent(tamperedWait.intents, BootstrapIntentKind::QuarantinePackage),
          "post-start journal tampering blocks recovery without unsafe path intents");
}

}  // namespace

int main() {
    test_strict_signed_plan_and_install_identity_validation();
    test_deployment_mode_cannot_be_guessed_from_installation_id();
    test_inventory_replacement_journal_classifies_old_new_and_backup_items();
    test_process_wait_and_replacement_emit_only_pure_intents();
    test_health_acknowledgement_and_rollback_policy();
    test_failure_quarantine_threshold();
    test_validated_values_cannot_be_rebound_or_structurally_tampered();
    return failures == 0 ? 0 : 1;
}
