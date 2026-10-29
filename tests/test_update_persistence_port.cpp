#include "platform/filesystem.h"
#include "platform/update_persistence_port.h"
#include "platform/update_persistence_port_test_hooks.h"
#include "update/UpdatePersistencePort.h"
#include "update/UpdateHttpTransport.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using aris::update::DownloadCleanupIntent;
using aris::update::DownloadCleanupIntentKind;
using aris::update::DownloadRequest;
using aris::update::RemoteValidators;
using aris::update::ResumeMetadata;
using aris::update::StagedIdentityStatus;
using aris::update::StoreIntent;
using aris::update::StoreIntentKind;
using aris::update::UpdatePersistenceError;
using aris::update::UpdatePersistencePort;

int failures = 0;

void check(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void dumpTree(const std::string& root) {
    Platform::FileSystem::DirectoryIterator iterator;
    if (!iterator.Open(root, true, true)) return;
    Platform::FileSystem::DirectoryEntry entry;
    while (iterator.Next(entry) == Platform::FileSystem::DirectoryReadStatus::Entry) {
        std::fprintf(stderr, "tree: %s\n", entry.pathUtf8.c_str());
    }
}

std::string join(const std::string& root, const std::string& relative) {
    return root + "/" + relative;
}

struct Fixture {
    Fixture() {
        root = Platform::FileSystem::TemporaryDirectory() +
               "/aris-update-persistence-contract-" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch().count());
        check(Platform::FileSystem::EnsureDirectoryExists(root),
              "isolated test root is created");
        auto created = Platform::UpdatePersistence::Testing::
            CreateForTrustedRoot(root);
        check(created.ok(), "test-only trusted-root factory succeeds");
        port = std::move(created.port);
    }

    ~Fixture() {
        port.reset();
        std::string ignored;
        Platform::FileSystem::RemoveAll(root, &ignored);
    }

    bool write(const std::string& relative, const std::string& bytes) {
        const auto slash = relative.find_last_of('/');
        if (slash != std::string::npos &&
            !Platform::FileSystem::EnsureDirectoryExists(
                join(root, relative.substr(0, slash)))) {
            return false;
        }
        return Platform::FileSystem::WriteFileText(join(root, relative), bytes);
    }

    std::string root;
    std::unique_ptr<UpdatePersistencePort> port;
};

StoreIntent storeIntent(StoreIntentKind kind,
                        std::string path,
                        std::string target = {},
                        std::string bytes = {}) {
    StoreIntent intent;
    intent.kind = kind;
    intent.path = std::move(path);
    intent.targetPath = std::move(target);
    intent.bytes = std::move(bytes);
    return intent;
}

DownloadCleanupIntent downloadIntent(DownloadCleanupIntentKind kind,
                                     std::string source,
                                     std::string target = {}) {
    DownloadCleanupIntent intent;
    intent.kind = kind;
    intent.sourcePath = std::move(source);
    intent.targetPath = std::move(target);
    return intent;
}

void test_atomic_temp_write_and_replace_is_ordered_and_durable() {
    Fixture fixture;
    const std::vector<StoreIntent> plan = {
        storeIntent(StoreIntentKind::WriteTempFile,
                    "patch-info.json.tmp", {}, "{\"schema_version\":1}"),
        storeIntent(StoreIntentKind::AtomicReplace,
                    "patch-info.json.tmp", "patch-info.json"),
        storeIntent(StoreIntentKind::WriteTempFile,
                    "1.2.0/Aris.zip.meta.json.tmp", {}, "{\"downloadedBytes\":3}"),
        storeIntent(StoreIntentKind::AtomicReplace,
                    "1.2.0/Aris.zip.meta.json.tmp",
                    "1.2.0/Aris.zip.meta.json"),
    };
    const auto result = fixture.port->ExecuteStoreIntents(plan);
    if (!result.ok()) {
        std::fprintf(stderr, "atomic plan error=%d system=%u completed=%zu\n",
                     static_cast<int>(result.error), result.systemError,
                     result.completedIntents);
    }
    check(result.ok() && result.completedIntents == 4,
          "patch-info and resume sidecar temp writes replace in caller order");

    std::string bytes;
    const bool statePublished = Platform::FileSystem::ReadFileText(
              join(fixture.root, "patch-info.json"), bytes) &&
              bytes == "{\"schema_version\":1}";
    if (!statePublished) dumpTree(fixture.root);
    check(statePublished,
          "atomic replacement publishes the exact caller-owned bytes");
    check(Platform::FileSystem::ProbePath(
              join(fixture.root, "patch-info.json.tmp")) ==
              Platform::FileSystem::PathStatus::Missing,
          "successful atomic replace consumes the temp sibling");
    bytes.clear();
    check(Platform::FileSystem::ReadFileText(
              join(fixture.root, "1.2.0/Aris.zip.meta.json"), bytes) &&
              bytes == "{\"downloadedBytes\":3}",
          "resume sidecar uses the same crash-safe temp sibling protocol");
}

void test_invalid_plan_fails_before_any_mutation_and_rejects_root_escape() {
    Fixture fixture;
    const std::vector<StoreIntent> invalidPlan = {
        storeIntent(StoreIntentKind::WriteTempFile,
                    "patch-info.json.tmp", {}, "bytes"),
        storeIntent(StoreIntentKind::AtomicReplace,
                    "other.tmp", "patch-info.json"),
    };
    const auto rejected = fixture.port->ExecuteStoreIntents(invalidPlan);
    check(rejected.error == UpdatePersistenceError::InvalidIntent &&
              rejected.completedIntents == 0,
          "the full plan is validated before its first mutation");
    check(Platform::FileSystem::ProbePath(
              join(fixture.root, "patch-info.json.tmp")) ==
              Platform::FileSystem::PathStatus::Missing,
          "invalid later intent cannot leave an earlier temp file");

    check(fixture.write("patch-info.json.tmp", "stale"),
          "stale crash temp fixture is written");
    const auto reversePlan = fixture.port->ExecuteStoreIntents({
        storeIntent(StoreIntentKind::AtomicReplace,
                    "patch-info.json.tmp", "patch-info.json"),
        storeIntent(StoreIntentKind::WriteTempFile,
                    "patch-info.json.tmp", {}, "new"),
    });
    check(reversePlan.error == UpdatePersistenceError::InvalidIntent &&
              reversePlan.completedIntents == 0 &&
              Platform::FileSystem::ProbePath(
                  join(fixture.root, "patch-info.json")) ==
                  Platform::FileSystem::PathStatus::Missing,
          "replace-before-write cannot publish stale crash bytes");
    const auto replaceOnly = fixture.port->ExecuteStoreIntents({
        storeIntent(StoreIntentKind::AtomicReplace,
                    "patch-info.json.tmp", "patch-info.json"),
    });
    check(replaceOnly.error == UpdatePersistenceError::InvalidIntent,
          "atomic replace requires its immediately preceding temp write");

    for (const std::string path : {
             "../escape", "/absolute", "C:/drive", "safe/../../escape",
             "safe//file", ".", "safe/./file"}) {
        const auto result = fixture.port->ExecuteStoreIntents({
            storeIntent(StoreIntentKind::DeleteStagedPatch, path),
        });
        check(result.error == UpdatePersistenceError::InvalidIntent,
              "absolute, drive, empty-segment, and traversal paths are rejected");
        check(fixture.port->ObserveStagedPatch(path).status ==
                  StagedIdentityStatus::InvalidPath,
              "identity observation applies the same containment policy");
    }
    const std::string overlongComponent = std::string(256, 'a') + ".part";
    check(fixture.port->ExecuteDownloadIntents({
              downloadIntent(DownloadCleanupIntentKind::DeletePart,
                             overlongComponent),
          }).error == UpdatePersistenceError::InvalidIntent,
          "platform port enforces the same component bound as download policy");
}

void test_sidecar_cleanup_part_promotion_and_missing_delete_are_explicit() {
    Fixture fixture;
    check(fixture.write("1.2.0/Aris.zip.part", "payload"),
          "part fixture is written");
    check(fixture.write("1.2.0/Aris.zip.meta.json", "metadata"),
          "sidecar fixture is written");

    const auto promoted = fixture.port->ExecuteDownloadIntents({
        downloadIntent(DownloadCleanupIntentKind::RenamePartToTarget,
                       "1.2.0/Aris.zip.part", "1.2.0/Aris.zip"),
        downloadIntent(DownloadCleanupIntentKind::DeleteMeta,
                       "1.2.0/Aris.zip.meta.json"),
    });
    if (!promoted.ok()) {
        std::fprintf(stderr, "promote error=%d system=%u completed=%zu\n",
                     static_cast<int>(promoted.error), promoted.systemError,
                     promoted.completedIntents);
    }
    check(promoted.ok() && promoted.completedIntents == 2,
          "part promotion precedes sidecar cleanup");
    check(Platform::FileSystem::ProbePath(join(fixture.root, "1.2.0/Aris.zip")) ==
              Platform::FileSystem::PathStatus::Present,
          "promoted target is present");
    check(Platform::FileSystem::ProbePath(
              join(fixture.root, "1.2.0/Aris.zip.meta.json")) ==
              Platform::FileSystem::PathStatus::Missing,
          "successful promotion cleans the resume sidecar");

    const auto missingDelete = fixture.port->ExecuteDownloadIntents({
        downloadIntent(DownloadCleanupIntentKind::DeletePart,
                       "1.2.0/missing.zip.part"),
    });
    check(missingDelete.ok() && missingDelete.completedIntents == 1,
          "delete-if-present is idempotent for a missing part");

    const auto missingParentDelete = fixture.port->ExecuteDownloadIntents({
        downloadIntent(DownloadCleanupIntentKind::DeletePart,
                       "missing-version/Aris.zip.part"),
        downloadIntent(DownloadCleanupIntentKind::DeleteMeta,
                       "other-missing-version/Aris.zip.meta.json"),
    });
    check(missingParentDelete.ok() &&
              missingParentDelete.completedIntents == 2,
          "delete-if-present remains idempotent when parent directories are missing");
}

void test_quarantine_and_staged_identity_observation_stay_inside_root() {
    Fixture fixture;
    check(fixture.write("1.2.0/Aris.zip", "abc"),
          "staged patch fixture is written");
    check(fixture.write("quarantine/1.2.0-Aris.zip", "stale"),
          "stale quarantine fixture is written");

    const auto observed = fixture.port->ObserveStagedPatch("1.2.0/Aris.zip");
    check(observed.status == StagedIdentityStatus::Present &&
              observed.identity.exists && observed.identity.size == 3 &&
              observed.identity.relativePath == "1.2.0/Aris.zip" &&
              observed.identity.sha256 ==
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "staged identity returns relative path, size, and SHA-256 only");

    const auto quarantined = fixture.port->ExecuteStoreIntents({
        storeIntent(StoreIntentKind::QuarantineStagedPatch,
                    "1.2.0/Aris.zip", "quarantine/1.2.0-Aris.zip"),
    });
    if (!quarantined.ok()) {
        std::fprintf(stderr, "quarantine error=%d system=%u completed=%zu\n",
                     static_cast<int>(quarantined.error), quarantined.systemError,
                     quarantined.completedIntents);
    }
    check(quarantined.ok() && quarantined.completedIntents == 1,
          "quarantine copy-and-delete completes as one audited intent");
    check(Platform::FileSystem::ProbePath(join(fixture.root, "1.2.0/Aris.zip")) ==
              Platform::FileSystem::PathStatus::Missing &&
              Platform::FileSystem::ProbePath(
                  join(fixture.root, "quarantine/1.2.0-Aris.zip")) ==
              Platform::FileSystem::PathStatus::Present,
          "quarantine atomically replaces a stale entry under the fixed root namespace");
    std::string quarantinedBytes;
    check(Platform::FileSystem::ReadFileText(
              join(fixture.root, "quarantine/1.2.0-Aris.zip"), quarantinedBytes) &&
              quarantinedBytes == "abc",
          "quarantine replacement preserves the current staged package bytes");

    const auto missing = fixture.port->ObserveStagedPatch("1.2.0/missing.zip");
    check(missing.status == StagedIdentityStatus::Missing &&
              !missing.identity.exists,
          "missing staged patch is distinct from a platform probe error");
}

void test_platform_failures_stop_the_plan_and_propagate_error() {
    Fixture fixture;
    check(Platform::FileSystem::EnsureDirectoryExists(
              join(fixture.root, "blocked")),
          "blocking target directory fixture is created");
    check(fixture.write("patch-info.json", "keep"),
          "later-delete sentinel is written");
    const auto failed = fixture.port->ExecuteStoreIntents({
        storeIntent(StoreIntentKind::WriteTempFile,
                    "blocked.tmp", {}, "replacement"),
        storeIntent(StoreIntentKind::AtomicReplace,
                    "blocked.tmp", "blocked"),
        storeIntent(StoreIntentKind::DeleteStateFile, "patch-info.json"),
    });
    check(failed.error == UpdatePersistenceError::ReplaceFailed &&
              failed.systemError != 0 && failed.completedIntents == 1 &&
              Platform::FileSystem::ProbePath(
                  join(fixture.root, "patch-info.json")) ==
                  Platform::FileSystem::PathStatus::Present,
          "replace failure is explicit and later intents are not executed");
}

DownloadRequest managerRequest() {
    DownloadRequest request;
    request.version = "1.2.0";
    request.relativeTarget = "Aris.zip";
    request.url = "https://example.invalid/Aris.zip";
    request.expectedSize = 7;
    request.expectedSha256 = std::string(64, 'a');
    return request;
}

void test_manager_intents_compose_without_root_prefix_adaptation() {
    auto unsafe = managerRequest();
    unsafe.version = "../1.2.0";
    check(aris::update::UpdateDownloadManager::FinalizeChecksum(
              unsafe, std::string(64, 'a')).intents.empty(),
          "unsafe version cannot produce persistence intents");
    unsafe = managerRequest();
    unsafe.relativeTarget = "C:/escape.zip";
    check(aris::update::UpdateDownloadManager::FinalizeChecksum(
              unsafe, std::string(64, 'a')).intents.empty(),
          "absolute or drive target cannot produce persistence intents");
    unsafe = managerRequest();
    unsafe.relativeTarget = "Aris.zip.part";
    check(aris::update::UpdateDownloadManager::FinalizeChecksum(
              unsafe, std::string(64, 'a')).intents.empty(),
          "sidecar-colliding target cannot produce persistence intents");

    {
        Fixture fixture;
        check(fixture.write("1.2.0/Aris.zip.part", "payload"),
              "manager finalize part fixture is written");
        check(fixture.write("1.2.0/Aris.zip.meta.json", "metadata"),
              "manager finalize metadata fixture is written");
        const auto finalized = aris::update::UpdateDownloadManager::
            FinalizeChecksum(managerRequest(), std::string(64, 'a'));
        const auto executed = fixture.port->ExecuteDownloadIntents(
            finalized.intents);
        check(finalized.success && executed.ok() &&
                  Platform::FileSystem::ProbePath(
                      join(fixture.root, "1.2.0/Aris.zip")) ==
                      Platform::FileSystem::PathStatus::Present,
              "manager finalize intents execute directly below the trusted root");
    }

    {
        Fixture fixture;
        check(fixture.write("1.2.0/Aris.zip.part", "partial"),
              "manager reset part fixture is written");
        check(fixture.write("1.2.0/Aris.zip.meta.json", "stale"),
              "manager reset metadata fixture is written");
        const auto request = managerRequest();
        ResumeMetadata stale;
        stale.url = "https://example.invalid/old.zip";
        stale.expectedSize = request.expectedSize;
        stale.expectedSha256 = request.expectedSha256;
        stale.downloadedBytes = 7;
        const auto reset = aris::update::UpdateDownloadManager::DecideResume(
            request, stale, 7, RemoteValidators{});
        const auto executed = fixture.port->ExecuteDownloadIntents(reset.intents);
        check(executed.ok() &&
                  Platform::FileSystem::ProbePath(
                      join(fixture.root, "1.2.0/Aris.zip.part")) ==
                      Platform::FileSystem::PathStatus::Missing &&
                  Platform::FileSystem::ProbePath(
                      join(fixture.root, "1.2.0/Aris.zip.meta.json")) ==
                      Platform::FileSystem::PathStatus::Missing,
              "manager resume-reset intents execute directly below the trusted root");
    }
}

void test_transport_append_intent_uses_trusted_relative_sink() {
    Fixture fixture;
    auto request = managerRequest();
    request.expectedSize = 4;
    request.expectedSha256 = std::string(64, 'a');
    request.url =
        "https://github.com/xindong/harken-search/releases/download/v1.2.0/Aris.zip";
    const auto admitted = aris::update::UpdateDownloadManager::Admit(
        aris::update::DownloadFlightState{}, request);
    const auto plan = aris::update::UpdateHttpTransport::BuildRequest(
        request, aris::update::ResumeDecision{}, admitted.state,
        *admitted.token).plan;
    aris::update::HttpResponseHead head;
    head.statusCode = 200;
    head.contentLength = 4;
    head.etag = "etag-120";
    const auto response = aris::update::UpdateHttpTransport::EvaluateResponse(
        plan, head);
    const auto session = aris::update::UpdateHttpTransport::BeginSession(
        plan, response);
    const auto chunk = aris::update::UpdateHttpTransport::AcceptChunk(
        session, admitted.state, plan.identity,
        std::vector<std::uint8_t>{'a', 'b', 'c'});
    const auto activated = fixture.port->ActivateTransfer(
        admitted.state, *admitted.token, plan.identity);
    auto mismatchedMeta = chunk.resumeMetadata;
    mismatchedMeta.metadata.downloadedBytes = 2;
    check(fixture.port->ExecuteTransferChunk(
              chunk.intent, mismatchedMeta).error ==
              UpdatePersistenceError::InvalidIntent &&
              Platform::FileSystem::ProbePath(
                  join(fixture.root, plan.identity.partPath)) ==
                  Platform::FileSystem::PathStatus::Missing,
          "metadata offset mismatch fails before partial-file mutation");
    mismatchedMeta.metadata.downloadedBytes = 4;
    check(fixture.port->ExecuteTransferChunk(
              chunk.intent, mismatchedMeta).error ==
              UpdatePersistenceError::InvalidIntent &&
              Platform::FileSystem::ProbePath(
                  join(fixture.root, plan.identity.partPath)) ==
                  Platform::FileSystem::PathStatus::Missing,
          "ahead metadata offset also fails before partial-file mutation");
    const auto persisted = fixture.port->ExecuteTransferChunk(
        chunk.intent, chunk.resumeMetadata);
    const auto observed = fixture.port->ObserveStagedPatch(plan.identity.partPath);
    check(activated.ok() && persisted.ok() &&
              observed.status == StagedIdentityStatus::Present &&
              observed.identity.size == 3 &&
              observed.identity.sha256 ==
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "transport append intent is persisted and hashed through the trusted root port");
    std::string metaBytes;
    check(Platform::FileSystem::ReadFileText(
              join(fixture.root, "1.2.0/Aris.zip.meta.json"), metaBytes) &&
              metaBytes.find("\"downloadedBytes\":3") != std::string::npos,
          "resume sidecar atomically follows the persisted part offset");
    const auto released = aris::update::UpdateDownloadManager::Release(
        admitted.state, *admitted.token);
    check(released.activeKey.empty() && fixture.port->DeactivateTransfer(
              *admitted.token, plan.identity).ok(),
          "released flight revokes the persistence lease explicitly");
    auto revokedAppend = chunk.intent;
    revokedAppend.offset = 3;
    revokedAppend.bytes = {'d'};
    auto revokedMeta = chunk.resumeMetadata;
    revokedMeta.metadata.downloadedBytes = 4;
    check(fixture.port->ExecuteTransferChunk(
              revokedAppend, revokedMeta).error ==
              UpdatePersistenceError::StaleGeneration,
          "revoked generation cannot land queued effects before re-admission");

    fixture.port.reset();
    auto recreated = Platform::UpdatePersistence::Testing::CreateForTrustedRoot(
        fixture.root);
    check(recreated.ok(), "trusted persistence port recreates after restart");
    fixture.port = std::move(recreated.port);
    const auto recovered = fixture.port->ObserveResumeMetadata(
        "1.2.0/Aris.zip.meta.json");
    aris::update::RemoteValidators validators;
    validators.etag = "etag-120";
    check(recovered.status == aris::update::ResumeMetadataStatus::Present &&
              aris::update::UpdateDownloadManager::DecideResume(
                  request, recovered.metadata, 3, validators).mode ==
                  aris::update::ResumeMode::Resume,
          "strict sidecar round-trip restores resumable state after port recreation");

    std::string legacyMeta;
    check(Platform::FileSystem::ReadFileText(
              join(fixture.root, "1.2.0/Aris.zip.meta.json"), legacyMeta),
          "canonical resume metadata fixture is readable");
    const auto canonicalRepository = legacyMeta.find("xindong/harken-search");
    legacyMeta.replace(canonicalRepository,
                       std::string("xindong/harken-search").size(),
                       "zhongziyu-cmd/aris-releases");
    check(fixture.write("1.2.0/Aris.zip.meta.json", legacyMeta) &&
              fixture.port->ObserveResumeMetadata(
                  "1.2.0/Aris.zip.meta.json").status ==
                  aris::update::ResumeMetadataStatus::Malformed,
          "legacy resume metadata fails closed before resume");

    check(fixture.write("1.2.0/Aris.zip.meta.json", "{truncated"),
          "malformed sidecar fixture is written");
    check(fixture.port->ObserveResumeMetadata(
              "1.2.0/Aris.zip.meta.json").status ==
              aris::update::ResumeMetadataStatus::Malformed,
          "malformed resume sidecar fails closed");

    const auto newer = aris::update::UpdateDownloadManager::Admit(released, request);
    const auto newerPlan = aris::update::UpdateHttpTransport::BuildRequest(
        request, aris::update::ResumeDecision{}, newer.state,
        *newer.token).plan;
    check(fixture.port->ActivateTransfer(
              newer.state, *newer.token, newerPlan.identity).ok(),
          "new generation becomes the persistence lease owner");
    check(fixture.port->DeactivateTransfer(
              *admitted.token, plan.identity).error ==
              UpdatePersistenceError::StaleGeneration,
          "stale token cannot revoke the new persistence lease owner");
    auto delayed = chunk.intent;
    delayed.offset = 3;
    delayed.bytes = {'d'};
    delayed.identity = plan.identity;
    auto delayedMeta = chunk.resumeMetadata;
    delayedMeta.identity = plan.identity;
    delayedMeta.metadata.downloadedBytes = 4;
    check(fixture.port->ExecuteTransferChunk(delayed, delayedMeta).error ==
              UpdatePersistenceError::StaleGeneration,
          "queued old-generation effects cannot land after re-admission");
}

void test_retention_listing_and_removal_stay_scoped_to_version_trees() {
    Fixture fixture;
    check(fixture.write("1.0.21/staging/pkg.zip", "old"), "old package staged");
    check(fixture.write("1.0.22/staging/pkg.zip", "prev"), "previous package staged");
    check(fixture.write("1.0.23/staging/pkg.zip", "current"), "current package staged");
    check(fixture.write("staging/shared.bin", "keep"), "shared staging written");
    check(fixture.write("quarantine/x.zip", "keep"), "quarantine written");

    const auto listing = fixture.port->ObserveVersionDirectories();
    check(listing.ok, "retention listing reads the update root");
    check(listing.directoryNames == std::vector<std::string>(
              {"1.0.21", "1.0.22", "1.0.23", "quarantine", "staging"}),
          "listing is sorted and exposes names, never absolute roots");

    const auto plan = aris::update::UpdateDownloadManager::PlanVersionRetention(
        listing.directoryNames, "1.0.23");
    check(plan.ok && plan.pruneVersions == std::vector<std::string>({"1.0.21"}),
          "real listing feeds the same keep-current-and-previous decision");

    for (const auto& version : plan.pruneVersions) {
        const auto removed = fixture.port->DeleteVersionDirectory(version);
        check(removed.ok() && removed.completedIntents == 1,
              "one listed version tree is removed through the port");
    }
    check(!Platform::FileSystem::Exists(join(fixture.root, "1.0.21")),
          "pruned tree including nested staging is gone from disk");
    check(Platform::FileSystem::Exists(join(fixture.root, "1.0.22/staging/pkg.zip")) &&
              Platform::FileSystem::Exists(join(fixture.root, "1.0.23/staging/pkg.zip")),
          "previous and running packages survive the prune");
    check(Platform::FileSystem::Exists(join(fixture.root, "staging/shared.bin")) &&
              Platform::FileSystem::Exists(join(fixture.root, "quarantine/x.zip")),
          "non-version directories are never pruned");

    const auto rejected = fixture.port->DeleteVersionDirectory("staging");
    check(!rejected.ok(), "non-semver names are rejected before any effect");
    const auto forged = fixture.port->DeleteVersionDirectory("../1.0.22");
    check(!forged.ok(), "traversal names are rejected before any effect");

    const auto requeued = fixture.port->DeleteVersionDirectory("1.0.21");
    check(requeued.ok(), "a second prune of the same version stays idempotent");
}

}  // namespace

int main() {
    test_atomic_temp_write_and_replace_is_ordered_and_durable();
    test_invalid_plan_fails_before_any_mutation_and_rejects_root_escape();
    test_sidecar_cleanup_part_promotion_and_missing_delete_are_explicit();
    test_quarantine_and_staged_identity_observation_stay_inside_root();
    test_platform_failures_stop_the_plan_and_propagate_error();
    test_manager_intents_compose_without_root_prefix_adaptation();
    test_transport_append_intent_uses_trusted_relative_sink();
    test_retention_listing_and_removal_stay_scoped_to_version_trees();
    return failures == 0 ? 0 : 1;
}
