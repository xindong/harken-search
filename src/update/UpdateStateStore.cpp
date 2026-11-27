#include "update/UpdateStateStore.h"
#include "HarkenSearchUpdateProductIdentity.h"
#include "update/UpdateReleaseConfig.h"
#include "nlohmann_json.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <utility>

namespace aris::update {
namespace {

using Json = nlohmann::ordered_json;

constexpr int kSchemaVersion = 1;
constexpr std::size_t kMaximumRecordBytes = 1024 * 1024;
constexpr std::size_t kMaximumRelativePathBytes = 4096;
constexpr std::string_view kReleaseAssetPrefix =
    ARIS_UPDATE_GITHUB_RELEASE_DOWNLOAD_PREFIX;
constexpr std::string_view kCompanyReleaseAssetPrefix =
    ARIS_UPDATE_COMPANY_RELEASE_DOWNLOAD_PREFIX;

std::string NormalizeSeparators(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool IsSha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isxdigit(ch) != 0;
           });
}

bool IsSafeSegment(const std::string& value) {
    if (value.empty() || value == "." || value == "..") return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
    });
}

bool IsSafeRelativePath(const std::string& input) {
    const std::string value = NormalizeSeparators(input);
    if (value.empty() || value.front() == '/' || value.find(':') != std::string::npos) {
        return false;
    }
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('/', begin);
        const std::string segment = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!IsSafeSegment(segment)) return false;
        if (end == std::string::npos) return true;
        begin = end + 1;
    }
    return false;
}

std::string BaseName(const std::string& relativePath) {
    const std::string normalized = NormalizeSeparators(relativePath);
    const std::size_t slash = normalized.find_last_of('/');
    return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

bool HasExactKeys(const Json& value,
                  std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) return false;
    for (const char* key : expected) {
        if (!value.contains(key)) return false;
    }
    return true;
}

const char* ChannelName(UpdateChannel channel) {
    return channel == UpdateChannel::Stable ? "stable" : "testing";
}

bool ParseChannel(const Json& value, UpdateChannel& channel) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    if (text == "stable") {
        channel = UpdateChannel::Stable;
        return true;
    }
    if (text == "testing") {
        channel = UpdateChannel::Testing;
        return true;
    }
    return false;
}

const char* PhaseName(UpdatePhase phase) {
    switch (phase) {
    case UpdatePhase::Idle: return "idle";
    case UpdatePhase::Checking: return "checking";
    case UpdatePhase::Available: return "available";
    case UpdatePhase::Downloading: return "downloading";
    case UpdatePhase::Ready: return "ready";
    case UpdatePhase::Superseding: return "superseding";
    case UpdatePhase::Error: return "error";
    }
    return "idle";
}

bool ParsePhase(const Json& value, UpdatePhase& phase) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    if (text == "idle") phase = UpdatePhase::Idle;
    else if (text == "checking") phase = UpdatePhase::Checking;
    else if (text == "available") phase = UpdatePhase::Available;
    else if (text == "downloading") phase = UpdatePhase::Downloading;
    else if (text == "ready") phase = UpdatePhase::Ready;
    else if (text == "superseding") phase = UpdatePhase::Superseding;
    else if (text == "error") phase = UpdatePhase::Error;
    else return false;
    return true;
}

bool IsKnownChannel(UpdateChannel channel) {
    return channel == UpdateChannel::Stable || channel == UpdateChannel::Testing;
}

bool IsKnownPhase(UpdatePhase phase) {
    switch (phase) {
    case UpdatePhase::Idle:
    case UpdatePhase::Checking:
    case UpdatePhase::Available:
    case UpdatePhase::Downloading:
    case UpdatePhase::Ready:
    case UpdatePhase::Superseding:
    case UpdatePhase::Error:
        return true;
    }
    return false;
}

bool IsStrictNumericSemVersion(std::string_view value) {
    std::size_t begin = 0;
    for (int component = 0; component < 3; ++component) {
        const std::size_t end = component == 2 ? value.size()
                                                : value.find('.', begin);
        if (end == std::string_view::npos || end == begin) return false;
        const std::string_view token = value.substr(begin, end - begin);
        if (token.size() > 1 && token.front() == '0') return false;
        std::uint64_t number = 0;
        for (char digit : token) {
            if (digit < '0' || digit > '9') return false;
            number = number * 10 + static_cast<unsigned>(digit - '0');
            if (number > std::numeric_limits<std::uint32_t>::max()) return false;
        }
        begin = end + 1;
    }
    return begin == value.size() + 1;
}

bool IsSchemaVersion(const Json& value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() ==
               static_cast<std::uint64_t>(kSchemaVersion);
    }
    return value.is_number_integer() &&
           value.get<std::int64_t>() == kSchemaVersion;
}

bool IsValidCandidate(const UpdateCandidate& candidate) {
    return IsStrictNumericSemVersion(candidate.version) &&
           candidate.version.size() <= 64 && !candidate.buildId.empty() &&
           candidate.buildId.size() <= 256 && !candidate.assetUrl.empty() &&
           candidate.assetUrl.size() <= 4096 &&
           (candidate.assetUrl.rfind(kReleaseAssetPrefix, 0) == 0 ||
            candidate.assetUrl.rfind(kCompanyReleaseAssetPrefix, 0) == 0) &&
           candidate.assetSize > 0 &&
           IsSha256(candidate.sha256);
}

Json CandidateJson(const UpdateCandidate& candidate) {
    Json value = Json::object();
    value["version"] = candidate.version;
    value["build_id"] = candidate.buildId;
    value["asset_url"] = candidate.assetUrl;
    value["asset_size"] = candidate.assetSize;
    value["sha256"] = LowerAscii(candidate.sha256);
    return value;
}

bool ParseCandidate(const Json& value, UpdateCandidate& candidate) {
    if (!HasExactKeys(value, {"version", "build_id", "asset_url", "asset_size",
                              "sha256"}) ||
        !value["version"].is_string() || !value["build_id"].is_string() ||
        !value["asset_url"].is_string() || !value["asset_size"].is_number_unsigned() ||
        !value["sha256"].is_string()) {
        return false;
    }
    candidate.version = value["version"].get<std::string>();
    candidate.buildId = value["build_id"].get<std::string>();
    candidate.assetUrl = value["asset_url"].get<std::string>();
    candidate.assetSize = value["asset_size"].get<std::uint64_t>();
    candidate.sha256 = LowerAscii(value["sha256"].get<std::string>());
    return IsValidCandidate(candidate);
}

Json OptionalCandidateJson(const std::optional<UpdateCandidate>& candidate) {
    return candidate ? CandidateJson(*candidate) : Json(nullptr);
}

bool ParseOptionalCandidate(const Json& value,
                            std::optional<UpdateCandidate>& candidate) {
    if (value.is_null()) {
        candidate.reset();
        return true;
    }
    UpdateCandidate parsed;
    if (!ParseCandidate(value, parsed)) return false;
    candidate = std::move(parsed);
    return true;
}

bool IsValidRecord(const UpdateStoreRecord& record) {
    if (!IsKnownChannel(record.channel) || !IsKnownPhase(record.snapshot.phase) ||
        record.snapshot.schemaVersion != kSchemaVersion) {
        return false;
    }
    if (record.snapshot.candidate && !IsValidCandidate(*record.snapshot.candidate)) {
        return false;
    }
    if (record.snapshot.readyPatch && !IsValidCandidate(*record.snapshot.readyPatch)) {
        return false;
    }
    switch (record.snapshot.phase) {
    case UpdatePhase::Idle:
    case UpdatePhase::Checking:
    case UpdatePhase::Error:
        if (record.snapshot.candidate || record.snapshot.readyPatch ||
            record.snapshot.downloadedBytes != 0) return false;
        break;
    case UpdatePhase::Available:
    case UpdatePhase::Downloading:
        if (!record.snapshot.candidate) return false;
        break;
    case UpdatePhase::Ready:
        if (record.snapshot.candidate || !record.snapshot.readyPatch) return false;
        break;
    case UpdatePhase::Superseding:
        if (record.snapshot.candidate || !record.snapshot.readyPatch) return false;
        break;
    }
    if (record.snapshot.restartConfirmed && record.snapshot.phase != UpdatePhase::Ready) {
        return false;
    }
    if (record.snapshot.candidate &&
        record.snapshot.downloadedBytes > record.snapshot.candidate->assetSize) {
        return false;
    }
    if (record.snapshot.phase == UpdatePhase::Ready &&
        record.snapshot.readyPatch &&
        record.snapshot.downloadedBytes > record.snapshot.readyPatch->assetSize) {
        return false;
    }
    if (record.snapshot.lastError.size() > 4096) return false;
    if (record.snapshot.readyPatch && !IsSafeRelativePath(record.stagedRelativePath)) {
        return false;
    }
    if (record.stagedRelativePath.size() > kMaximumRelativePathBytes ||
        (!record.stagedRelativePath.empty() &&
         !IsSafeRelativePath(record.stagedRelativePath))) {
        return false;
    }
    if (record.diagnosticRelativePath.size() > kMaximumRelativePathBytes ||
        (!record.diagnosticRelativePath.empty() &&
         !IsSafeRelativePath(record.diagnosticRelativePath))) {
        return false;
    }
    if (record.rollback.pending &&
        (!IsStrictNumericSemVersion(record.rollback.version) ||
         record.rollback.reason.empty())) {
        return false;
    }
    if (!record.rollback.pending &&
        (!record.rollback.version.empty() || !record.rollback.reason.empty())) {
        return false;
    }
    return record.rollback.version.size() <= 64 &&
           record.rollback.reason.size() <= 4096;
}

StoreIntent Intent(StoreIntentKind kind,
                   const std::string& path,
                   const std::string& target = {},
                   const std::string& bytes = {}) {
    StoreIntent intent;
    intent.kind = kind;
    intent.path = path;
    intent.targetPath = target;
    intent.bytes = bytes;
    return intent;
}

std::string StatePath() {
    return "patch-info.json";
}

}  // namespace

StoreSerializeResult UpdateStateStore::Serialize(const UpdateStoreRecord& record) {
    StoreSerializeResult result;
    if (!IsValidRecord(record)) {
        result.error = StoreError::FieldInvalid;
        return result;
    }

    try {
        Json snapshot = Json::object();
        snapshot["schema_version"] = record.snapshot.schemaVersion;
        snapshot["phase"] = PhaseName(record.snapshot.phase);
        snapshot["candidate"] = OptionalCandidateJson(record.snapshot.candidate);
        snapshot["ready_patch"] = OptionalCandidateJson(record.snapshot.readyPatch);
        snapshot["downloaded_bytes"] = record.snapshot.downloadedBytes;
        snapshot["last_error"] = record.snapshot.lastError;
        snapshot["restart_confirmed"] = record.snapshot.restartConfirmed;

        Json rollback = Json::object();
        rollback["pending"] = record.rollback.pending;
        rollback["version"] = record.rollback.version;
        rollback["reason"] = record.rollback.reason;

        Json root = Json::object();
        root["schema_version"] = kSchemaVersion;
        root["product"] = kLegacyUpdateProduct;
        root["channel"] = ChannelName(record.channel);
        root["snapshot"] = std::move(snapshot);
        root["failure_count"] = record.failureCount;
        root["apply_attempts"] = record.applyAttempts;
        root["rollback"] = std::move(rollback);
        root["staged_relative_path"] = record.stagedRelativePath;
        root["diagnostic_relative_path"] = record.diagnosticRelativePath;
        result.bytes = root.dump();
    } catch (const nlohmann::json::exception&) {
        result.bytes.clear();
        result.error = StoreError::FieldInvalid;
        return result;
    }
    if (result.bytes.size() > kMaximumRecordBytes) {
        result.bytes.clear();
        result.error = StoreError::FieldInvalid;
    }
    return result;
}

StoreDeserializeResult UpdateStateStore::Deserialize(std::string_view bytes) {
    StoreDeserializeResult result;
    if (bytes.empty() || bytes.size() > kMaximumRecordBytes) {
        result.error = StoreError::JsonInvalid;
        return result;
    }

    const Json root = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (root.is_discarded() ||
        !HasExactKeys(root, {"schema_version", "product", "channel", "snapshot",
                             "failure_count", "apply_attempts", "rollback",
                             "staged_relative_path", "diagnostic_relative_path"})) {
        result.error = StoreError::JsonInvalid;
        return result;
    }
    if (!IsSchemaVersion(root["schema_version"])) {
        result.error = StoreError::SchemaUnsupported;
        return result;
    }
    if (!root["product"].is_string() ||
        !IsSupportedUpdateProduct(root["product"].get<std::string>())) {
        result.error = StoreError::ProductMismatch;
        return result;
    }

    UpdateStoreRecord parsed;
    if (!ParseChannel(root["channel"], parsed.channel)) {
        result.error = StoreError::ChannelInvalid;
        return result;
    }
    const Json& snapshot = root["snapshot"];
    const Json& rollback = root["rollback"];
    if (!HasExactKeys(snapshot, {"schema_version", "phase", "candidate", "ready_patch",
                                 "downloaded_bytes", "last_error",
                                 "restart_confirmed"}) ||
        !HasExactKeys(rollback, {"pending", "version", "reason"})) {
        result.error = StoreError::FieldInvalid;
        return result;
    }
    if (!IsSchemaVersion(snapshot["schema_version"])) {
        result.error = StoreError::SchemaUnsupported;
        return result;
    }
    if (
        !ParsePhase(snapshot["phase"], parsed.snapshot.phase) ||
        !ParseOptionalCandidate(snapshot["candidate"], parsed.snapshot.candidate) ||
        !ParseOptionalCandidate(snapshot["ready_patch"], parsed.snapshot.readyPatch) ||
        !snapshot["downloaded_bytes"].is_number_unsigned() ||
        !snapshot["last_error"].is_string() ||
        !snapshot["restart_confirmed"].is_boolean() ||
        !root["failure_count"].is_number_unsigned() ||
        !root["apply_attempts"].is_number_unsigned() ||
        !rollback["pending"].is_boolean() || !rollback["version"].is_string() ||
        !rollback["reason"].is_string() ||
        !root["staged_relative_path"].is_string() ||
        !root["diagnostic_relative_path"].is_string()) {
        result.error = StoreError::FieldInvalid;
        return result;
    }

    const auto failureCount = root["failure_count"].get<std::uint64_t>();
    const auto applyAttempts = root["apply_attempts"].get<std::uint64_t>();
    if (failureCount > std::numeric_limits<std::uint32_t>::max() ||
        applyAttempts > std::numeric_limits<std::uint32_t>::max()) {
        result.error = StoreError::FieldInvalid;
        return result;
    }
    parsed.snapshot.downloadedBytes =
        snapshot["downloaded_bytes"].get<std::uint64_t>();
    parsed.snapshot.lastError = snapshot["last_error"].get<std::string>();
    parsed.snapshot.restartConfirmed = snapshot["restart_confirmed"].get<bool>();
    parsed.failureCount = static_cast<std::uint32_t>(failureCount);
    parsed.applyAttempts = static_cast<std::uint32_t>(applyAttempts);
    parsed.rollback.pending = rollback["pending"].get<bool>();
    parsed.rollback.version = rollback["version"].get<std::string>();
    parsed.rollback.reason = rollback["reason"].get<std::string>();
    parsed.stagedRelativePath = root["staged_relative_path"].get<std::string>();
    parsed.diagnosticRelativePath =
        root["diagnostic_relative_path"].get<std::string>();
    if (!IsValidRecord(parsed)) {
        result.error = StoreError::FieldInvalid;
        return result;
    }

    result.record = std::move(parsed);
    return result;
}

StoreWritePlan UpdateStateStore::PlanAtomicWrite(
    const UpdateStoreRecord& record) {
    StoreWritePlan plan;
    const auto serialized = Serialize(record);
    if (!serialized.ok()) {
        plan.error = serialized.error;
        return plan;
    }
    const std::string statePath = StatePath();
    const std::string tempPath = statePath + ".tmp";
    plan.intents.push_back(
        Intent(StoreIntentKind::WriteTempFile, tempPath, {}, serialized.bytes));
    plan.intents.push_back(
        Intent(StoreIntentKind::AtomicReplace, tempPath, statePath));
    return plan;
}

PatchRecordDecision UpdateStateStore::EvaluatePatch(
    const UpdateStoreRecord& record,
    const std::string& currentVersion,
    const StagedPatchIdentity& stagedIdentity,
    std::uint32_t maxApplyAttempts) {
    PatchRecordDecision decision;
    if (!IsValidRecord(record) || !IsStrictNumericSemVersion(currentVersion)) {
        return decision;
    }
    if (!record.snapshot.readyPatch) {
        decision.kind = PatchRecordDecisionKind::NoReadyPatch;
        return decision;
    }

    const auto& ready = *record.snapshot.readyPatch;
    const std::string stagedPath = NormalizeSeparators(record.stagedRelativePath);
    const std::string statePath = StatePath();
    if (ready.version == currentVersion) {
        decision.kind = PatchRecordDecisionKind::AlreadyApplied;
        decision.intents.push_back(
            Intent(StoreIntentKind::DeleteStagedPatch, stagedPath));
        decision.intents.push_back(
            Intent(StoreIntentKind::DeleteStateFile, statePath));
        return decision;
    }
    const bool identityMatches =
        stagedIdentity.exists &&
        stagedIdentity.relativePath == record.stagedRelativePath &&
        stagedIdentity.size == ready.assetSize && IsSha256(stagedIdentity.sha256) &&
        LowerAscii(stagedIdentity.sha256) == LowerAscii(ready.sha256);
    if (!identityMatches) {
        decision.kind = PatchRecordDecisionKind::InvalidStagedPatch;
        if (stagedIdentity.exists) {
            decision.intents.push_back(
                Intent(StoreIntentKind::DeleteStagedPatch, stagedPath));
        }
        decision.intents.push_back(
            Intent(StoreIntentKind::DeleteStateFile, statePath));
        return decision;
    }

    if (maxApplyAttempts == 0 || record.applyAttempts >= maxApplyAttempts) {
        decision.kind = PatchRecordDecisionKind::Quarantined;
        const std::string quarantinePath = "quarantine/" + ready.version + "-" +
                                           BaseName(record.stagedRelativePath);
        decision.intents.push_back(Intent(StoreIntentKind::QuarantineStagedPatch,
                                          stagedPath,
                                          quarantinePath));
        decision.intents.push_back(
            Intent(StoreIntentKind::DeleteStateFile, statePath));
        return decision;
    }

    decision.kind = PatchRecordDecisionKind::Ready;
    decision.usable = true;
    return decision;
}

ApplyAttemptDecision UpdateStateStore::BeginApply(
    const UpdateStoreRecord& record,
    std::uint32_t maxApplyAttempts) {
    ApplyAttemptDecision decision;
    decision.record = record;
    if (!IsValidRecord(record) || record.snapshot.phase != UpdatePhase::Ready ||
        !record.snapshot.readyPatch || record.stagedRelativePath.empty() ||
        maxApplyAttempts == 0 ||
        record.applyAttempts >= maxApplyAttempts) {
        return decision;
    }
    decision.allowed = true;
    ++decision.record.applyAttempts;
    return decision;
}

}  // namespace aris::update
