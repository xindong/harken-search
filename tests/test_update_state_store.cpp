#include "update/UpdateStateStore.h"

#include <cstdio>
#include <string>

namespace {

using aris::update::PatchRecordDecisionKind;
using aris::update::StagedPatchIdentity;
using aris::update::StoreError;
using aris::update::StoreIntentKind;
using aris::update::UpdateCandidate;
using aris::update::UpdateChannel;
using aris::update::UpdatePhase;
using aris::update::UpdateStateStore;
using aris::update::UpdateStoreRecord;

int failures = 0;

void check(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

UpdateCandidate candidate(const char* version, const char* buildId) {
    UpdateCandidate value;
    value.version = version;
    value.buildId = buildId;
    value.assetUrl = std::string("https://github.com/xindong/harken-search/releases/download/v") +
                     version + "/Aris.zip";
    value.assetSize = 1000;
    value.sha256 = std::string(64, 'a');
    return value;
}

UpdateStoreRecord record() {
    UpdateStoreRecord value;
    value.channel = UpdateChannel::Testing;
    value.snapshot.phase = UpdatePhase::Ready;
    value.snapshot.readyPatch = candidate("1.2.0", "build-120");
    value.snapshot.downloadedBytes = 1000;
    value.snapshot.lastError = "newer check failed";
    value.failureCount = 1;
    value.applyAttempts = 0;
    value.rollback.pending = true;
    value.rollback.version = "1.1.0";
    value.rollback.reason = "health_timeout";
    value.stagedRelativePath = "1.2.0/Aris-1.2.0-win-x64-update.zip";
    value.diagnosticRelativePath = "logs/update-1.2.0.log";
    return value;
}

bool hasIntent(const aris::update::StoreWritePlan& plan, StoreIntentKind kind) {
    for (const auto& intent : plan.intents) {
        if (intent.kind == kind) return true;
    }
    return false;
}

bool hasIntent(const aris::update::PatchRecordDecision& decision,
               StoreIntentKind kind) {
    for (const auto& intent : decision.intents) {
        if (intent.kind == kind) return true;
    }
    return false;
}

StagedPatchIdentity identity(const UpdateStoreRecord& value) {
    StagedPatchIdentity result;
    result.exists = true;
    result.relativePath = value.stagedRelativePath;
    result.size = value.snapshot.readyPatch->assetSize;
    result.sha256 = value.snapshot.readyPatch->sha256;
    return result;
}

void test_round_trip_is_strict_deterministic_and_preserves_ready_patch() {
    const auto original = record();
    const auto first = UpdateStateStore::Serialize(original);
    const auto second = UpdateStateStore::Serialize(original);
    check(first.ok() && second.ok(), "valid store record serializes");
    check(first.bytes == second.bytes,
          "serialization is byte-for-byte deterministic");
    check(first.bytes.find("\n") == std::string::npos,
          "canonical serialization is compact");

    const auto decoded = UpdateStateStore::Deserialize(first.bytes);
    check(decoded.ok(), "canonical record decodes");
    check(decoded.record.channel == UpdateChannel::Testing,
          "channel round-trips");
    check(decoded.record.snapshot.phase == UpdatePhase::Ready,
          "snapshot phase round-trips");
    check(decoded.record.snapshot.readyPatch.has_value() &&
              decoded.record.snapshot.readyPatch->version == "1.2.0",
          "ready patch survives process restart");
    check(decoded.record.snapshot.lastError == "newer check failed",
          "failed newer check diagnostic does not discard ready patch");
    check(decoded.record.rollback.pending &&
              decoded.record.rollback.version == "1.1.0" &&
              decoded.record.rollback.reason == "health_timeout",
          "rollback marker round-trips");
}

void test_invalid_json_schema_product_and_channel_fail_closed() {
    const auto malformed = UpdateStateStore::Deserialize("{\"schema_version\":1");
    check(!malformed.ok() &&
              malformed.record.snapshot.phase == UpdatePhase::Idle &&
              !malformed.record.snapshot.readyPatch.has_value(),
          "truncated JSON returns recoverable empty state");

    const auto valid = UpdateStateStore::Serialize(record()).bytes;
    auto unsupported = valid;
    unsupported.replace(unsupported.find("\"schema_version\":1"), 18,
                        "\"schema_version\":2");
    check(!UpdateStateStore::Deserialize(unsupported).ok(),
          "unsupported schema is rejected");

    const std::string legacyProduct = "\"product\":\"aris\"";
    const auto productAt = valid.find(legacyProduct);
    check(productAt != std::string::npos,
          "state serialization still writes the legacy product identity");
    if (productAt == std::string::npos) return;
    auto v2State = valid;
    v2State.replace(productAt, legacyProduct.size(), "\"product\":\"harken-search\"");
    check(UpdateStateStore::Deserialize(v2State).ok(),
          "persisted state with the harken-search product is readable");

    auto foreign = valid;
    foreign.replace(productAt, legacyProduct.size(), "\"product\":\"other\"");
    check(!UpdateStateStore::Deserialize(foreign).ok(),
          "foreign product is rejected");

    auto badChannel = valid;
    badChannel.replace(badChannel.find("\"channel\":\"testing\""), 19,
                       "\"channel\":\"nightly\"");
    const auto rejectedChannel = UpdateStateStore::Deserialize(badChannel);
    check(!rejectedChannel.ok() &&
              !rejectedChannel.record.snapshot.readyPatch.has_value(),
          "unknown channel fails closed without partial state");
}

void test_atomic_write_plan_uses_fixed_allowlisted_sibling_paths() {
    const auto plan = UpdateStateStore::PlanAtomicWrite(record());
    check(plan.ok(), "valid record produces atomic write plan");
    check(plan.intents.size() == 2,
          "atomic write plan has exactly temp write and replace");
    if (plan.intents.size() == 2) {
        check(plan.intents[0].kind == StoreIntentKind::WriteTempFile &&
                  plan.intents[0].path == "patch-info.json.tmp" &&
                  !plan.intents[0].bytes.empty(),
              "canonical bytes are written to deterministic update-root-relative temp sibling");
        check(plan.intents[1].kind == StoreIntentKind::AtomicReplace &&
                  plan.intents[1].path == "patch-info.json.tmp" &&
                  plan.intents[1].targetPath == "patch-info.json",
              "temp sibling atomically replaces fixed patch-info path");
    }
    check(hasIntent(plan, StoreIntentKind::AtomicReplace),
          "replace intent is explicit rather than executed by codec");

    auto unsafeRecord = record();
    unsafeRecord.stagedRelativePath = "../../Aris.exe";
    check(!UpdateStateStore::PlanAtomicWrite(unsafeRecord).ok(),
          "unsafe staged path cannot produce file intents");
}

void test_staged_identity_current_version_and_attempt_cap_policy() {
    const auto value = record();
    const auto usable = UpdateStateStore::EvaluatePatch(
        value, "1.1.0", identity(value), 2);
    check(usable.kind == PatchRecordDecisionKind::Ready && usable.usable,
          "matching staged identity for newer patch is usable");

    auto mismatchIdentity = identity(value);
    mismatchIdentity.sha256 = std::string(64, 'b');
    const auto mismatch = UpdateStateStore::EvaluatePatch(
        value, "1.1.0", mismatchIdentity, 2);
    check(mismatch.kind == PatchRecordDecisionKind::InvalidStagedPatch &&
              !mismatch.usable &&
              hasIntent(mismatch, StoreIntentKind::DeleteStateFile),
          "staged identity mismatch fails closed and cleans record");

    const auto applied = UpdateStateStore::EvaluatePatch(
        value, "1.2.0", identity(value), 2);
    check(applied.kind == PatchRecordDecisionKind::AlreadyApplied &&
              hasIntent(applied, StoreIntentKind::DeleteStagedPatch) &&
              hasIntent(applied, StoreIntentKind::DeleteStateFile),
          "already-applied current-version record is ignored and cleaned");

    auto exhausted = value;
    exhausted.applyAttempts = 2;
    const auto quarantined = UpdateStateStore::EvaluatePatch(
        exhausted, "1.1.0", identity(exhausted), 2);
    check(quarantined.kind == PatchRecordDecisionKind::Quarantined &&
              hasIntent(quarantined, StoreIntentKind::QuarantineStagedPatch) &&
              hasIntent(quarantined, StoreIntentKind::DeleteStateFile),
          "bounded apply attempts quarantine patch and clear active record");

    StagedPatchIdentity missingIdentity;
    const auto missingAtCap = UpdateStateStore::EvaluatePatch(
        exhausted, "1.1.0", missingIdentity, 2);
    check(missingAtCap.kind == PatchRecordDecisionKind::InvalidStagedPatch &&
              !hasIntent(missingAtCap, StoreIntentKind::QuarantineStagedPatch) &&
              hasIntent(missingAtCap, StoreIntentKind::DeleteStateFile),
          "missing staged file is cleaned before apply-attempt quarantine policy");

    const auto firstAttempt = UpdateStateStore::BeginApply(value, 2);
    check(firstAttempt.allowed && firstAttempt.record.applyAttempts == 1,
          "apply attempt increments before external bootstrap work");
    auto atCap = value;
    atCap.applyAttempts = 2;
    check(!UpdateStateStore::BeginApply(atCap, 2).allowed,
          "apply cannot begin at configured cap");
}

void test_invalid_persisted_state_never_serializes_or_begins_apply() {
    auto invalidVersion = record();
    invalidVersion.snapshot.readyPatch->version = "release-latest";
    check(!UpdateStateStore::Serialize(invalidVersion).ok(),
          "non-semver candidate version is rejected");

    auto invalidUrl = record();
    invalidUrl.snapshot.readyPatch->assetUrl = "https://example.invalid/Aris.zip";
    check(!UpdateStateStore::Serialize(invalidUrl).ok(),
          "foreign staged asset URL is rejected");

    auto legacyBytes = UpdateStateStore::Serialize(record()).bytes;
    const auto canonicalRepository = legacyBytes.find("xindong/harken-search");
    legacyBytes.replace(canonicalRepository,
                        std::string("xindong/harken-search").size(),
                        "zhongziyu-cmd/aris-releases");
    const auto legacy = UpdateStateStore::Deserialize(legacyBytes);
    check(!legacy.ok() && legacy.record.snapshot.phase == UpdatePhase::Idle &&
              !legacy.record.snapshot.readyPatch.has_value(),
          "legacy persisted candidate fails closed to a fresh idle check");

    auto invalidReadyBytes = record();
    invalidReadyBytes.snapshot.downloadedBytes = 1001;
    check(!UpdateStateStore::Serialize(invalidReadyBytes).ok(),
          "ready offset beyond staged asset size is rejected");

    UpdateStoreRecord idle;
    check(!UpdateStateStore::BeginApply(idle, 2).allowed,
          "idle record cannot increment apply attempts");

    auto unsupportedSnapshot = record();
    unsupportedSnapshot.snapshot.schemaVersion = 999;
    check(!UpdateStateStore::Serialize(unsupportedSnapshot).ok(),
          "unsupported nested snapshot schema is rejected");
    auto unsupportedSnapshotBytes = UpdateStateStore::Serialize(record()).bytes;
    const auto nestedSchema = unsupportedSnapshotBytes.find(
        "\"snapshot\":{\"schema_version\":1");
    unsupportedSnapshotBytes.replace(nestedSchema, 30,
                                     "\"snapshot\":{\"schema_version\":2");
    check(UpdateStateStore::Deserialize(unsupportedSnapshotBytes).error ==
              StoreError::SchemaUnsupported,
          "unsupported nested snapshot schema has deterministic classification");

    auto invalidChannel = record();
    invalidChannel.channel = static_cast<UpdateChannel>(999);
    check(!UpdateStateStore::Serialize(invalidChannel).ok(),
          "unknown channel enum is rejected before serialization");

    auto invalidPhase = record();
    invalidPhase.snapshot.phase = static_cast<UpdatePhase>(999);
    check(!UpdateStateStore::Serialize(invalidPhase).ok(),
          "unknown phase enum is rejected before serialization");

    auto availableWithoutCandidate = record();
    availableWithoutCandidate.snapshot.phase = UpdatePhase::Available;
    availableWithoutCandidate.snapshot.readyPatch.reset();
    availableWithoutCandidate.snapshot.downloadedBytes = 0;
    availableWithoutCandidate.stagedRelativePath.clear();
    check(!UpdateStateStore::Serialize(availableWithoutCandidate).ok(),
          "available phase requires candidate metadata");

    auto excessivePath = record();
    excessivePath.diagnosticRelativePath = std::string(5000, 'x');
    check(!UpdateStateStore::Serialize(excessivePath).ok(),
          "persisted relative paths have a deterministic size bound");

    auto oversized = record();
    oversized.snapshot.lastError = std::string(1024 * 1024, 'x');
    check(!UpdateStateStore::Serialize(oversized).ok(),
          "serializer refuses records larger than decoder limit");

    auto invalidUtf8 = record();
    invalidUtf8.snapshot.lastError = std::string(1, static_cast<char>(0xff));
    bool returnedError = false;
    try {
        returnedError = !UpdateStateStore::Serialize(invalidUtf8).ok();
    } catch (...) {
        returnedError = false;
    }
    check(returnedError,
          "invalid UTF-8 fails through StoreError rather than throwing");
}

}  // namespace

int main() {
    test_round_trip_is_strict_deterministic_and_preserves_ready_patch();
    test_invalid_json_schema_product_and_channel_fail_closed();
    test_atomic_write_plan_uses_fixed_allowlisted_sibling_paths();
    test_staged_identity_current_version_and_attempt_cap_policy();
    test_invalid_persisted_state_never_serializes_or_begins_apply();
    return failures == 0 ? 0 : 1;
}
