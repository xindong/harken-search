#include "update/UpdateDownloadManager.h"

#include <cstdio>
#include <optional>
#include <string>

namespace {

using aris::update::DownloadAdmission;
using aris::update::DownloadAdmissionKind;
using aris::update::DownloadCleanupIntentKind;
using aris::update::DownloadErrorKind;
using aris::update::DownloadFlightState;
using aris::update::DownloadRequest;
using aris::update::RemoteValidators;
using aris::update::ResumeMetadata;
using aris::update::ResumeMode;
using aris::update::RetryPolicy;
using aris::update::UpdateDownloadManager;

int failures = 0;

void check(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool hasIntent(const aris::update::DownloadCleanupDecision& decision,
               DownloadCleanupIntentKind kind) {
    for (const auto& intent : decision.intents) {
        if (intent.kind == kind) return true;
    }
    return false;
}

bool hasIntent(const aris::update::ResumeDecision& decision,
               DownloadCleanupIntentKind kind) {
    for (const auto& intent : decision.intents) {
        if (intent.kind == kind) return true;
    }
    return false;
}

DownloadRequest request() {
    DownloadRequest value;
    value.version = "1.2.0";
    value.relativeTarget = "Aris-1.2.0-win-x64-update.zip";
    value.url = "https://github.com/xindong/aris/releases/download/v1.2.0/Aris.zip";
    value.expectedSize = 1000;
    value.expectedSha256 = std::string(64, 'a');
    return value;
}

ResumeMetadata metadata(const DownloadRequest& value) {
    ResumeMetadata meta;
    meta.url = value.url;
    meta.expectedSize = value.expectedSize;
    meta.expectedSha256 = value.expectedSha256;
    meta.downloadedBytes = 400;
    meta.etag = "etag-120";
    meta.lastModified = "Tue, 28 Jul 2026 00:00:00 GMT";
    return meta;
}

void test_staging_paths_are_scoped_and_sidecars_are_fixed() {
    const auto paths = UpdateDownloadManager::BuildStagingPaths(request());
    check(paths.ok(), "safe relative update target is accepted");
    check(paths.paths.targetPath ==
              "1.2.0/Aris-1.2.0-win-x64-update.zip",
          "target path is update-root-relative and version scoped");
    check(paths.paths.partPath == paths.paths.targetPath + ".part",
          "partial transfer path uses fixed .part sidecar");
    check(paths.paths.metaPath == paths.paths.targetPath + ".meta.json",
          "resume metadata path uses fixed .meta.json sidecar");

    auto unsafe = request();
    unsafe.relativeTarget = "../Aris.zip";
    check(!UpdateDownloadManager::BuildStagingPaths(unsafe).ok(),
          "parent traversal target is rejected");
    unsafe.relativeTarget = "C:/Windows/Aris.zip";
    check(!UpdateDownloadManager::BuildStagingPaths(unsafe).ok(),
          "absolute drive target is rejected");
    unsafe = request();
    unsafe.version = "../1.2.0";
    check(!UpdateDownloadManager::BuildStagingPaths(unsafe).ok(),
          "unsafe version segment is rejected");
    for (const char* reservedVersion : {"updates", "quarantine", "1.2"}) {
        unsafe = request();
        unsafe.version = reservedVersion;
        check(!UpdateDownloadManager::BuildStagingPaths(unsafe).ok(),
              "only strict numeric semver may own a top-level staging namespace");
    }
    for (const char* reservedTarget : {"Aris.zip.part", "Aris.zip.meta.json",
                                       "Aris.zip.tmp"}) {
        unsafe = request();
        unsafe.relativeTarget = reservedTarget;
        check(!UpdateDownloadManager::BuildStagingPaths(unsafe).ok(),
              "caller target cannot collide with updater-owned sidecars");
    }
    unsafe = request();
    unsafe.relativeTarget = std::string(256, 'a');
    check(!UpdateDownloadManager::BuildStagingPaths(unsafe).ok(),
          "overlong path components are rejected before persistence admission");
    unsafe.relativeTarget = std::string(4090, 'a');
    check(!UpdateDownloadManager::BuildStagingPaths(unsafe).ok(),
          "overlong relative staging paths are rejected before intent emission");
}

void test_fresh_resume_and_stale_metadata_decisions() {
    const auto value = request();
    const auto fresh = UpdateDownloadManager::DecideResume(
        value, std::nullopt, 0, RemoteValidators{});
    check(fresh.mode == ResumeMode::Fresh && fresh.offset == 0,
          "missing sidecar and part starts fresh");

    RemoteValidators validators;
    validators.etag = "etag-120";
    validators.lastModified = "Tue, 28 Jul 2026 00:00:00 GMT";
    const auto resume = UpdateDownloadManager::DecideResume(
        value, metadata(value), 400, validators);
    check(resume.mode == ResumeMode::Resume && resume.offset == 400,
          "matching url size hash part and validator resumes");
    check(resume.ifRange == "etag-120",
          "resume prefers ETag for If-Range");

    auto lastModifiedMeta = metadata(value);
    lastModifiedMeta.etag.clear();
    validators.etag.clear();
    const auto lastModifiedResume = UpdateDownloadManager::DecideResume(
        value, lastModifiedMeta, 400, validators);
    check(lastModifiedResume.mode == ResumeMode::Resume &&
              lastModifiedResume.ifRange == validators.lastModified,
          "resume falls back to Last-Modified for If-Range");

    auto validatorSchemeChanged = metadata(value);
    validators.etag.clear();
    validators.lastModified = validatorSchemeChanged.lastModified;
    const auto changedSchemeResume = UpdateDownloadManager::DecideResume(
        value, validatorSchemeChanged, 400, validators);
    check(changedSchemeResume.mode == ResumeMode::Resume &&
              changedSchemeResume.ifRange == validators.lastModified,
          "current Last-Modified validator is used when remote ETag is absent");

    auto stale = metadata(value);
    stale.url = "https://example.invalid/stale.zip";
    const auto reset = UpdateDownloadManager::DecideResume(
        value, stale, 400, validators);
    check(reset.mode == ResumeMode::Fresh,
          "stale metadata resets to a fresh download");
    check(hasIntent(reset, DownloadCleanupIntentKind::DeletePart) &&
              hasIntent(reset, DownloadCleanupIntentKind::DeleteMeta),
          "stale metadata requests part and sidecar cleanup");

    validators.etag = "new-etag";
    const auto validatorReset = UpdateDownloadManager::DecideResume(
        value, metadata(value), 400, validators);
    check(validatorReset.mode == ResumeMode::Fresh,
          "changed remote ETag invalidates resume");
}

void test_disk_preflight_and_progress_clamping() {
    const auto value = request();
    RemoteValidators validators;
    validators.etag = "etag-120";
    const auto enough = UpdateDownloadManager::CheckDiskSpace(
        value, metadata(value), 400, validators, 650, 50);
    check(enough.allowed && enough.requiredBytes == 650,
          "disk preflight credits a validated resumable part");

    auto stale = metadata(value);
    stale.expectedSha256 = std::string(64, 'b');
    const auto insufficient = UpdateDownloadManager::CheckDiskSpace(
        value, stale, 400, validators, 649, 50);
    check(!insufficient.allowed && insufficient.requiredBytes == 1050 &&
              insufficient.shortfallBytes == 401,
          "non-resumable part receives no disk-space credit");

    const auto progress = UpdateDownloadManager::ClampProgress(1250, 1000);
    check(progress.loadedBytes == 1000 && progress.percent == 100.0,
          "progress clamps loaded bytes and percent to total");
}

void test_retry_is_deterministic_capped_and_classified() {
    RetryPolicy policy;
    policy.maxAttempts = 6;
    policy.baseDelayMs = 1000;
    policy.maxDelayMs = 30000;

    const auto first = UpdateDownloadManager::DecideRetry(
        DownloadErrorKind::Network, 1, policy);
    check(first.retry && first.nextAttempt == 2 && first.delayMs == 1000,
          "first transient failure retries after deterministic base delay");
    const auto fifth = UpdateDownloadManager::DecideRetry(
        DownloadErrorKind::Http5xx, 5, policy);
    check(fifth.retry && fifth.nextAttempt == 6 && fifth.delayMs == 16000,
          "retry backoff remains exponential before cap");
    check(!UpdateDownloadManager::DecideRetry(
               DownloadErrorKind::Network, 6, policy).retry,
          "retry stops at max attempts");
    check(!UpdateDownloadManager::DecideRetry(
               DownloadErrorKind::Checksum, 1, policy).retry,
          "checksum mismatch is never retried");
}

void test_single_flight_rejects_duplicate_and_releases_owner() {
    DownloadFlightState idle;
    const auto value = request();
    const auto admitted = UpdateDownloadManager::Admit(idle, value);
    check(admitted.kind == DownloadAdmissionKind::Accepted &&
              !admitted.state.activeKey.empty() && admitted.token.has_value(),
          "idle scheduler admits one flight");
    const auto duplicate =
        UpdateDownloadManager::Admit(admitted.state, value);
    check(duplicate.kind == DownloadAdmissionKind::Duplicate,
          "duplicate canonical tuple is rejected");
    auto other = value;
    other.url += "?mirror=other";
    const auto busy = UpdateDownloadManager::Admit(admitted.state, other);
    check(busy.kind == DownloadAdmissionKind::Busy,
          "different flight is rejected while capacity one is busy");
    const auto released = UpdateDownloadManager::Release(
        admitted.state, *admitted.token);
    check(released.activeKey.empty(), "owner release returns scheduler to idle");

    const auto next = UpdateDownloadManager::Admit(released, value);
    check(next.kind == DownloadAdmissionKind::Accepted && next.token.has_value(),
          "released scheduler admits a new generation");
    const auto staleRelease = UpdateDownloadManager::Release(
        next.state, *admitted.token);
    check(!staleRelease.activeKey.empty(),
          "stale generation token cannot release a newer flight");

    auto nextVersion = value;
    nextVersion.version = "1.3.0";
    const auto nextVersionAdmission = UpdateDownloadManager::Admit(idle, nextVersion);
    check(nextVersionAdmission.kind == DownloadAdmissionKind::Accepted &&
              nextVersionAdmission.state.activeKey != admitted.state.activeKey,
          "single-flight identity includes the relative version namespace");
}

void test_checksum_cleanup_and_successful_rename_intents() {
    const auto value = request();
    const auto mismatch = UpdateDownloadManager::FinalizeChecksum(
        value, std::string(64, 'b'));
    check(!mismatch.success,
          "checksum mismatch rejects completion");
    check(hasIntent(mismatch, DownloadCleanupIntentKind::DeletePart) &&
              hasIntent(mismatch, DownloadCleanupIntentKind::DeleteMeta),
          "checksum mismatch wipes corrupt part and metadata");
    check(!hasIntent(mismatch, DownloadCleanupIntentKind::RenamePartToTarget),
          "checksum mismatch never renames into staged target");

    const auto success = UpdateDownloadManager::FinalizeChecksum(
        value, std::string(64, 'a'));
    check(success.success, "matching checksum accepts completion");
    check(hasIntent(success, DownloadCleanupIntentKind::RenamePartToTarget) &&
              hasIntent(success, DownloadCleanupIntentKind::DeleteMeta),
          "successful completion renames part then removes metadata");
    check(!hasIntent(success, DownloadCleanupIntentKind::DeletePart),
          "successful completion does not delete verified part before rename");

    const auto invalid = UpdateDownloadManager::FinalizeChecksum(
        value, "not-a-sha256");
    check(!invalid.success,
          "matching malformed digests never authorize completion");

    auto unsafe = value;
    unsafe.relativeTarget = "../../install/Aris.exe";
    const auto unsafeFinalize = UpdateDownloadManager::FinalizeChecksum(
        unsafe, std::string(64, 'a'));
    check(!unsafeFinalize.success && unsafeFinalize.intents.empty(),
          "invalid staging request cannot emit forged cleanup or rename intents");
}

void test_version_retention_keeps_current_previous_and_pending() {
    using aris::update::UpdateDownloadManager;

    const std::vector<std::string> directories{
        "1.0.21", "1.0.22", "1.0.23", "staging", "transaction-7",
        "quarantine", "logs"};
    const auto plan = UpdateDownloadManager::PlanVersionRetention(
        directories, "1.0.23");
    check(plan.ok, "numeric semver current version authorizes a plan");
    check(plan.pruneVersions == std::vector<std::string>({"1.0.21"}),
          "only versions older than the newest previous package are pruned");

    const auto pending = UpdateDownloadManager::PlanVersionRetention(
        {"1.0.23", "1.0.24"}, "1.0.23");
    check(pending.ok && pending.pruneVersions.empty(),
          "a downloaded newer candidate is never pruned");

    const auto onlyCurrent = UpdateDownloadManager::PlanVersionRetention(
        {"1.0.23"}, "1.0.23");
    check(onlyCurrent.ok && onlyCurrent.pruneVersions.empty(),
          "the running version alone keeps its own package");

    const auto numericOrder = UpdateDownloadManager::PlanVersionRetention(
        {"1.0.9", "1.0.10", "1.0.2"}, "1.0.10");
    check(numericOrder.pruneVersions == std::vector<std::string>({"1.0.2"}),
          "retention compares numeric components, not text order");

    const auto unparsable = UpdateDownloadManager::PlanVersionRetention(
        directories, "not-a-version");
    check(!unparsable.ok && unparsable.pruneVersions.empty(),
          "an invalid running version forbids any pruning");

    check(UpdateDownloadManager::IsVersionDirectoryName("1.0.23") &&
              !UpdateDownloadManager::IsVersionDirectoryName("../1.0") &&
              !UpdateDownloadManager::IsVersionDirectoryName("01.0.23") &&
              !UpdateDownloadManager::IsVersionDirectoryName("staging"),
          "directory admission accepts only strict numeric semver names");
}

}  // namespace

int main() {
    test_staging_paths_are_scoped_and_sidecars_are_fixed();
    test_fresh_resume_and_stale_metadata_decisions();
    test_disk_preflight_and_progress_clamping();
    test_retry_is_deterministic_capped_and_classified();
    test_single_flight_rejects_duplicate_and_releases_owner();
    test_checksum_cleanup_and_successful_rename_intents();
    test_version_retention_keeps_current_previous_and_pending();
    return failures == 0 ? 0 : 1;
}
