#include "bootstrap/HarkenSearchBootstrapPlan.h"
#include "HarkenSearchUpdateProductIdentity.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>

namespace aris::bootstrap {
namespace {

constexpr std::size_t kMaximumPortablePathBytes = 4096U;
constexpr std::size_t kMaximumWindowsComponentBytes = 255U;
constexpr std::size_t kMaximumInventoryItems = 4096U;

struct WindowsPath {
    char drive = '\0';
    std::vector<std::string> components;
    std::string normalized;
};

std::string toLowerAscii(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const unsigned char character : value) {
        lowered.push_back(static_cast<char>(std::tolower(character)));
    }
    return lowered;
}

bool hasControlOrNul(const std::string& value) {
    for (const unsigned char character : value) {
        if (character == 0 || character < 0x20) return true;
    }
    return false;
}

// Relative inventory names are compared by both the portable policy and the
// native adapter. Keep their shared value domain ASCII-only so the portable
// ASCII fold cannot disagree with NTFS ordinal case-insensitive comparison
// for a UTF-8 spelling. Absolute roots remain Unicode-capable on Windows.
bool hasNonAscii(const std::string& value) {
    for (const unsigned char character : value) {
        if (character >= 0x80U) return true;
    }
    return false;
}

bool isLowerHex(const std::string& value, const std::size_t requiredLength) {
    if (value.size() != requiredLength) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool isSafeIdentifier(const std::string& value,
                      const std::size_t minimumLength,
                      const std::size_t maximumLength) {
    if (value.size() < minimumLength || value.size() > maximumLength) return false;
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

bool isValidSemverCoreNumber(const std::string& value) {
    if (value.empty()) return false;
    if (value.size() > 1 && value.front() == '0') return false;
    std::uint64_t number = 0;
    for (const unsigned char character : value) {
        if (std::isdigit(character) == 0) return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (number > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
            return false;
        }
        number = number * 10 + digit;
    }
    return true;
}

bool isValidSemver(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    const std::size_t prerelease = value.find('-');
    const std::string core = value.substr(0, prerelease);
    std::size_t begin = 0;
    for (std::size_t component = 0; component < 3; ++component) {
        const std::size_t end = component == 2 ? core.size() : core.find('.', begin);
        if (end == std::string::npos || end == begin) return false;
        const std::string piece = core.substr(begin, end - begin);
        if (!isValidSemverCoreNumber(piece)) return false;
        begin = end + 1;
    }
    if (begin != core.size() + 1) return false;

    if (prerelease == std::string::npos) return true;
    const std::string suffix = value.substr(prerelease + 1);
    if (suffix.empty()) return false;
    begin = 0;
    while (begin < suffix.size()) {
        const std::size_t end = suffix.find('.', begin);
        const std::string identifier = suffix.substr(begin, end - begin);
        if (identifier.empty()) return false;
        bool numericOnly = true;
        for (const unsigned char character : identifier) {
            if (!(std::isalnum(character) || character == '-')) return false;
            numericOnly = numericOnly && std::isdigit(character) != 0;
        }
        if (numericOnly && identifier.size() > 1 && identifier.front() == '0') return false;
        if (end == std::string::npos) return true;
        begin = end + 1;
    }
    return false;
}

bool isReservedWindowsName(const std::string& segment) {
    const std::size_t extension = segment.find('.');
    const std::string base = toLowerAscii(segment.substr(0, extension));
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
    if (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0)) {
        return base[3] >= '1' && base[3] <= '9';
    }
    return false;
}

bool isSafeWindowsPathComponentBase(const std::string& component) {
    if (component.empty() || component == "." || component == ".." ||
        hasControlOrNul(component)) {
        return false;
    }
    if (component.back() == '.' || component.back() == ' ') return false;
    for (const char character : component) {
        switch (character) {
            case '<':
            case '>':
            case ':':
            case '"':
            case '|':
            case '?':
            case '*':
                return false;
            default:
                break;
        }
    }
    return !isReservedWindowsName(component);
}

bool isSafeAbsoluteRootComponent(const std::string& component) {
    return isSafeWindowsPathComponentBase(component);
}

bool isSafeRelativeComponent(const std::string& component) {
    return component.find('~') == std::string::npos &&
           isSafeWindowsPathComponentBase(component);
}

bool parseAbsoluteWindowsPath(const std::string& raw, WindowsPath* result) {
    if (raw.size() < 3 || raw.size() > kMaximumPortablePathBytes ||
        hasControlOrNul(raw)) return false;
    std::string value = raw;
    std::replace(value.begin(), value.end(), '\\', '/');
    if (!std::isalpha(static_cast<unsigned char>(value[0])) || value[1] != ':' ||
        value[2] != '/') {
        return false;
    }
    if (value.find("//", 3) != std::string::npos) return false;

    WindowsPath parsed;
    parsed.drive = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
    std::size_t start = 3;
    while (start < value.size()) {
        const std::size_t end = value.find('/', start);
        const std::string component = value.substr(start, end - start);
        if (component.size() > kMaximumWindowsComponentBytes) return false;
        if (!isSafeAbsoluteRootComponent(component)) return false;
        parsed.components.push_back(component);
        if (end == std::string::npos) break;
        start = end + 1;
        if (start == value.size()) return false;
    }

    parsed.normalized.assign(1, parsed.drive);
    parsed.normalized += ":/";
    for (std::size_t index = 0; index < parsed.components.size(); ++index) {
        if (index != 0) parsed.normalized += '/';
        parsed.normalized += parsed.components[index];
    }
    if (parsed.normalized.size() > kMaximumPortablePathBytes) return false;
    *result = std::move(parsed);
    return true;
}

bool isSystemDirectory(const WindowsPath& path) {
    if (path.components.empty()) return false;
    const std::string first = toLowerAscii(path.components.front());
    if (first == "windows" || first == "recovery" ||
        first == "system volume information" || first == "$recycle.bin") {
        return true;
    }
    return path.components.size() == 1 &&
           (first == "program files" || first == "program files (x86)" ||
            first == "users");
}

bool isKnownDataDirectory(const WindowsPath& path) {
    if (path.components.empty()) return false;
    for (const auto& component : path.components) {
        const std::string lowered = toLowerAscii(component);
        if (lowered == "appdata" || lowered == "localappdata" ||
            lowered == "programdata") {
            return true;
        }
    }
    return false;
}

bool isSameOrDescendant(const WindowsPath& candidate, const WindowsPath& ancestor) {
    if (candidate.drive != ancestor.drive ||
        candidate.components.size() < ancestor.components.size()) {
        return false;
    }
    for (std::size_t index = 0; index < ancestor.components.size(); ++index) {
        if (toLowerAscii(candidate.components[index]) !=
            toLowerAscii(ancestor.components[index])) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> normalizeRelativeInstallPath(const std::string& raw) {
    if (raw.empty() || raw.size() > kMaximumPortablePathBytes ||
        hasControlOrNul(raw) || hasNonAscii(raw) ||
        raw.front() == '/' || raw.front() == '\\' ||
        raw.back() == '/' || raw.back() == '\\' ||
        raw.find(':') != std::string::npos) {
        return std::nullopt;
    }
    std::string value = raw;
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.find("//") != std::string::npos) return std::nullopt;

    std::stringstream stream(value);
    std::string component;
    std::string normalized;
    while (std::getline(stream, component, '/')) {
        if (component.size() > kMaximumWindowsComponentBytes) {
            return std::nullopt;
        }
        if (!isSafeRelativeComponent(component)) return std::nullopt;
        if (!normalized.empty()) normalized += '/';
        normalized += component;
    }
    if (normalized.size() > kMaximumPortablePathBytes) return std::nullopt;
    return normalized.empty() ? std::nullopt : std::optional<std::string>(normalized);
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
    std::size_t difference = left.size() ^ right.size();
    const std::size_t longest = std::max(left.size(), right.size());
    for (std::size_t index = 0; index < longest; ++index) {
        const unsigned char leftCharacter =
            index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
        const unsigned char rightCharacter =
            index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
        difference |= static_cast<std::size_t>(leftCharacter ^ rightCharacter);
    }
    return difference == 0;
}

void appendBindingField(std::string& binding, const std::string& value) {
    binding += std::to_string(value.size());
    binding += ':';
    binding += value;
}

void appendBindingNumber(std::string& binding, const std::uint64_t value) {
    appendBindingField(binding, std::to_string(value));
}

void appendProcessIdentityBinding(std::string& binding,
                                  const BootstrapProcessIdentity& identity) {
    appendBindingNumber(binding, identity.valid ? 1U : 0U);
    appendBindingNumber(binding, identity.processId);
    appendBindingNumber(binding, identity.creationTime);
    appendBindingField(binding, identity.imagePath);
    appendBindingNumber(binding, identity.imageVolumeSerial);
    appendBindingField(binding, std::string(
                                     reinterpret_cast<const char*>(
                                         identity.imageFileId.data()),
                                     identity.imageFileId.size()));
}

void appendDeploymentFileIdentityBinding(
    std::string& binding,
    const aris::platform::DeploymentFileIdentity& identity) {
    appendBindingNumber(binding, identity.valid ? 1U : 0U);
    appendBindingField(binding, identity.canonicalPath);
    appendBindingNumber(binding, identity.volumeSerial);
    appendBindingField(binding, std::string(
                                     reinterpret_cast<const char*>(
                                         identity.fileId.data()),
                                     identity.fileId.size()));
}

bool hasNonZeroFileId(const std::array<std::uint8_t, 16>& fileId) {
    return std::any_of(fileId.begin(), fileId.end(),
                       [](const std::uint8_t value) { return value != 0U; });
}

std::string validatedPlanBinding(const ValidatedInstallPlan& plan) {
    std::string binding;
    appendBindingNumber(binding, static_cast<std::uint32_t>(plan.identity.deploymentMode));
    appendBindingField(binding, plan.identity.targetRoot);
    appendBindingField(binding, plan.identity.installationId);
    appendBindingField(binding, plan.identity.registeredAppId);
    appendDeploymentFileIdentityBinding(
        binding, plan.identity.targetRootIdentity);
    appendDeploymentFileIdentityBinding(
        binding, plan.identity.oldExecutableIdentity);
    appendBindingNumber(binding, plan.identity.protectedDataRoots.size());
    for (const auto& root : plan.identity.protectedDataRoots) {
        appendBindingField(binding, root);
    }
    appendBindingField(binding, plan.invocation.updaterOwnedBase);
    appendBindingNumber(binding, static_cast<std::uint32_t>(plan.invocation.deploymentMode));
    appendBindingField(binding, plan.invocation.installationId);
    appendBindingField(binding, plan.invocation.targetRootIdentityBinding);
    appendBindingField(binding, plan.invocation.oldExecutableIdentityBinding);
    appendBindingField(binding, plan.invocation.stagingRoot);
    appendBindingField(binding, plan.invocation.transactionRoot);
    appendBindingField(binding, plan.invocation.invocationIdentity);
    appendBindingField(binding, plan.invocation.transactionId);
    appendBindingField(binding, plan.invocation.packagePlanDigest);
    appendBindingField(binding, plan.invocation.healthNonceDigest);
    appendProcessIdentityBinding(binding, plan.invocation.parentProcess);
    appendProcessIdentityBinding(binding, plan.invocation.oldProcess);
    appendBindingField(binding, plan.version);
    appendBindingField(binding, plan.buildId);
    appendBindingField(binding, plan.packageKeyId);
    appendBindingField(binding, plan.packageChannel);
    appendBindingField(binding, plan.packagePlanDigest);
    appendBindingNumber(binding, plan.oldProcessId);
    appendBindingField(binding, plan.healthToken);
    appendBindingField(binding, plan.entrypoint);
    appendBindingNumber(binding, plan.inventory.size());
    for (const auto& item : plan.inventory) {
        appendBindingField(binding, item.relativePath);
        appendBindingField(binding, item.sha256);
        appendBindingNumber(binding, item.byteSize);
    }
    appendBindingNumber(binding, plan.removals.size());
    for (const auto& path : plan.removals) {
        appendBindingField(binding, path);
    }
    return binding;
}

std::string journalStructureBinding(const ReplacementJournal& journal) {
    std::string binding;
    appendBindingField(binding, journal.transactionId);
    appendBindingNumber(binding, journal.items.size());
    for (const auto& item : journal.items) {
        appendBindingField(binding, item.relativePath);
        appendBindingNumber(binding, static_cast<std::uint64_t>(item.classification));
        appendBindingNumber(binding, item.requiresBackup ? 1 : 0);
    }
    return binding;
}

BootstrapIntent intent(BootstrapIntentKind kind,
                       const std::string& relativePath = {},
                       const std::uint64_t processId = 0,
                       const std::string& reason = {}) {
    BootstrapIntent value;
    value.kind = kind;
    value.relativePath = relativePath;
    value.processId = processId;
    value.reason = reason;
    return value;
}

TransactionDecision failBeforeMutation(const TransactionState& previous,
                                       const std::string& reason) {
    TransactionDecision result;
    result.state = previous;
    result.state.phase = TransactionPhase::FailedBeforeMutation;
    const FailureDisposition failure = NextFailureDisposition(previous.previousFailureCount);
    result.state.recordedFailureCount = failure.recordedFailureCount;
    result.intents.push_back(intent(BootstrapIntentKind::RecordFailure, {}, 0, reason));
    if (failure.quarantine) {
        result.intents.push_back(intent(BootstrapIntentKind::QuarantinePackage, {}, 0, reason));
    }
    return result;
}

TransactionDecision blockRecovery(const TransactionState& previous,
                                  const std::string& reason) {
    TransactionDecision result;
    result.state = previous;
    result.state.phase = TransactionPhase::RecoveryBlocked;
    result.state.healthTokenConsumed = true;
    const FailureDisposition failure = NextFailureDisposition(previous.previousFailureCount);
    result.state.recordedFailureCount = failure.recordedFailureCount;
    result.intents.push_back(intent(BootstrapIntentKind::PersistJournal,
                                    {},
                                    0,
                                    "recovery-blocked"));
    if (previous.updatedChildProcessId != 0) {
        result.intents.push_back(intent(BootstrapIntentKind::TerminateUpdatedChild,
                                        {},
                                        previous.updatedChildProcessId,
                                        reason));
    }
    result.intents.push_back(intent(BootstrapIntentKind::RecordFailure, {}, 0, reason));
    result.intents.push_back(intent(BootstrapIntentKind::QuarantinePackage, {}, 0, reason));
    return result;
}

bool hasUnrecoverableJournalGap(const ReplacementJournal& journal) {
    for (const auto& item : journal.items) {
        if ((item.classification == JournalItemClass::ReplaceExisting ||
             item.classification == JournalItemClass::RemoveExisting) &&
            item.replacementRecorded && !item.backupRecorded) {
            return true;
        }
    }
    return false;
}

TransactionDecision rollback(const TransactionState& previous, const std::string& reason) {
    TransactionDecision result;
    result.state = previous;
    if (previous.phase != TransactionPhase::AwaitingUpdatedChildProcess &&
        previous.phase != TransactionPhase::AwaitingHealth) {
        return result;
    }
    if (!previous.plan.isValidated() ||
        !previous.journal.isValidatedFor(previous.plan)) {
        return blockRecovery(previous, "invalid-transaction-state");
    }

    result.state.phase = TransactionPhase::RolledBack;
    result.state.healthTokenConsumed = true;
    const FailureDisposition failure = NextFailureDisposition(previous.previousFailureCount);
    result.state.recordedFailureCount = failure.recordedFailureCount;

    if (hasUnrecoverableJournalGap(previous.journal)) {
        return blockRecovery(previous, "unrecoverable-journal-gap");
    }

    result.intents.push_back(intent(BootstrapIntentKind::PersistJournal, {}, 0, "rolling-back"));
    if (previous.updatedChildProcessId != 0) {
        result.intents.push_back(intent(BootstrapIntentKind::TerminateUpdatedChild,
                                        {},
                                        previous.updatedChildProcessId,
                                        reason));
    }
    for (const auto& item : previous.journal.items) {
        if ((item.classification == JournalItemClass::ReplaceExisting ||
             item.classification == JournalItemClass::RemoveExisting) &&
            item.backupRecorded && item.replacementRecorded) {
            result.intents.push_back(intent(BootstrapIntentKind::RestoreBackup,
                                            item.relativePath,
                                            0,
                                            reason));
        }
    }
    for (const auto& item : previous.journal.items) {
        if (item.classification == JournalItemClass::CreateNew &&
            item.replacementRecorded) {
            result.intents.push_back(intent(BootstrapIntentKind::RemoveNewItem,
                                            item.relativePath,
                                            0,
                                            reason));
        }
    }
    result.intents.push_back(intent(BootstrapIntentKind::RelaunchPreviousApplication,
                                    previous.plan.entrypoint,
                                    0,
                                    reason));
    result.intents.push_back(intent(BootstrapIntentKind::RecordFailure, {}, 0, reason));
    if (failure.quarantine) {
        result.intents.push_back(intent(BootstrapIntentKind::QuarantinePackage, {}, 0, reason));
    }
    return result;
}

bool allIncomingReplacementsRecorded(const ReplacementJournal& journal) {
    for (const auto& item : journal.items) {
        if (item.classification == JournalItemClass::ReplaceExisting &&
            (!item.backupRecorded || !item.replacementRecorded)) {
            return false;
        }
        if (item.classification == JournalItemClass::CreateNew &&
            !item.replacementRecorded) {
            return false;
        }
        if (item.classification == JournalItemClass::RemoveExisting &&
            (!item.backupRecorded || !item.replacementRecorded)) {
            return false;
        }
    }
    return true;
}

std::optional<std::size_t> journalItemIndex(const ReplacementJournal& journal,
                                            const std::string& relativePath) {
    const auto normalized = normalizeRelativeInstallPath(relativePath);
    if (!normalized) return std::nullopt;
    const std::string insensitivePath = toLowerAscii(*normalized);
    for (std::size_t index = 0; index < journal.items.size(); ++index) {
        if (toLowerAscii(journal.items[index].relativePath) == insensitivePath) {
            return index;
        }
    }
    return std::nullopt;
}

JournalRecordDecision journalRecordFailure(const TransactionState& state,
                                           const JournalRecordResult status) {
    JournalRecordDecision result;
    result.result = status;
    result.transaction.state = state;
    return result;
}

bool isReplacementProgressPhase(const TransactionPhase phase) {
    return phase == TransactionPhase::AwaitingUpdatedChildProcess ||
           phase == TransactionPhase::AwaitingHealth;
}

}  // namespace

bool ValidatedInstallPlan::isValidated() const {
    return validated_ && validationBinding_ == validatedPlanBinding(*this);
}

bool ReplacementJournal::isValidated() const {
    return validated_ && structureBinding_ == journalStructureBinding(*this);
}

bool ReplacementJournal::isValidatedFor(const ValidatedInstallPlan& plan) const {
    return isValidated() && plan.isValidated() &&
           planBinding_ == validatedPlanBinding(plan);
}

InstallPlanValidation ValidateSignedInstallPlan(
    const SignedInstallPlan& raw,
    const SignatureStatus signatureStatus) {
    InstallPlanValidation result;
    if (signatureStatus != SignatureStatus::Verified) {
        result.error = PlanError::SignatureRejected;
        return result;
    }
    if (raw.schemaVersion != 1) {
        result.error = PlanError::InvalidSchema;
        return result;
    }
    if (!update::IsSupportedUpdateProduct(raw.product)) {
        result.error = PlanError::InvalidProduct;
        return result;
    }
    if (!isValidSemver(raw.version)) {
        result.error = PlanError::InvalidVersion;
        return result;
    }
    if (!isSafeIdentifier(raw.buildId, 1, 128)) {
        result.error = PlanError::InvalidBuildId;
        return result;
    }
    if (!raw.channel.empty() && raw.channel != "stable" && raw.channel != "testing") {
        result.error = PlanError::InvalidChannel;
        return result;
    }
    if (!raw.packagePlanDigest.empty() &&
        !isLowerHex(raw.packagePlanDigest, 64)) {
        result.error = PlanError::InvalidPackagePlanDigest;
        return result;
    }
    if (!isSafeIdentifier(raw.keyId, 1, 64)) {
        result.error = PlanError::InvalidKeyId;
        return result;
    }
    if (!isLowerHex(raw.detachedSignature, 128)) {
        result.error = PlanError::InvalidSignatureEnvelope;
        return result;
    }
    if (!isLowerHex(raw.payloadSha256, 64)) {
        result.error = PlanError::InvalidPayloadDigest;
        return result;
    }
    if (raw.oldProcessId == 0) {
        result.error = PlanError::InvalidProcessId;
        return result;
    }
    if (!isLowerHex(raw.healthToken, 64)) {
        result.error = PlanError::InvalidHealthToken;
        return result;
    }
    if (!isSafeIdentifier(raw.identity.installationId, 1, 128)) {
        result.error = PlanError::InvalidInstallationId;
        return result;
    }
    if (raw.identity.deploymentMode != aris::platform::DeploymentMode::Installed &&
        raw.identity.deploymentMode != aris::platform::DeploymentMode::Portable) {
        result.error = PlanError::InvalidInstallationId;
        return result;
    }
    const bool installed = raw.identity.deploymentMode ==
        aris::platform::DeploymentMode::Installed;
    const bool portable = raw.identity.deploymentMode ==
        aris::platform::DeploymentMode::Portable;
    if (!installed && !portable) {
        result.error = PlanError::InvalidInstallationId;
        return result;
    }
    if (installed &&
        (raw.identity.installationId !=
             "installed-a8b3c9d1-e4f5-4a6b-8c9d-0e1f2a3b4c5d" ||
         raw.identity.registeredAppId != aris::platform::kArisInstalledAppId)) {
        result.error = PlanError::InvalidInstallationId;
        return result;
    }
    if (portable && (raw.identity.installationId.rfind("portable-", 0U) != 0U ||
                     !raw.identity.registeredAppId.empty())) {
        result.error = PlanError::InvalidInstallationId;
        return result;
    }
    if (raw.invocation.deploymentMode != raw.identity.deploymentMode ||
        raw.invocation.installationId != raw.identity.installationId ||
        raw.invocation.targetRootIdentityBinding !=
            raw.identity.targetRootIdentity.canonicalPath ||
        raw.invocation.oldExecutableIdentityBinding !=
            raw.identity.oldExecutableIdentity.canonicalPath) {
        result.error = PlanError::InvalidInstallationId;
        return result;
    }

    WindowsPath target;
    if (!parseAbsoluteWindowsPath(raw.identity.targetRoot, &target)) {
        result.error = PlanError::InvalidTargetRoot;
        return result;
    }
    if (target.components.empty()) {
        result.error = PlanError::DiskRootRejected;
        return result;
    }
    if (isSystemDirectory(target)) {
        result.error = PlanError::SystemDirectoryRejected;
        return result;
    }
    if (isKnownDataDirectory(target)) {
        result.error = PlanError::DataDirectoryRejected;
        return result;
    }
    WindowsPath targetIdentityPath;
    if (!raw.identity.targetRootIdentity.valid ||
        raw.identity.targetRootIdentity.volumeSerial == 0U ||
        !hasNonZeroFileId(raw.identity.targetRootIdentity.fileId) ||
        !parseAbsoluteWindowsPath(
            raw.identity.targetRootIdentity.canonicalPath,
            &targetIdentityPath) ||
        targetIdentityPath.normalized != target.normalized) {
        result.error = PlanError::InvalidTargetRoot;
        return result;
    }
    WindowsPath oldExecutableIdentityPath;
    WindowsPath oldProcessImagePath;
    if (!raw.identity.oldExecutableIdentity.valid ||
        raw.identity.oldExecutableIdentity.volumeSerial == 0U ||
        !hasNonZeroFileId(raw.identity.oldExecutableIdentity.fileId) ||
        !raw.invocation.oldProcess.valid ||
        raw.invocation.oldProcess.processId != raw.oldProcessId ||
        raw.invocation.oldProcess.imageVolumeSerial == 0U ||
        !hasNonZeroFileId(raw.invocation.oldProcess.imageFileId) ||
        !parseAbsoluteWindowsPath(
            raw.identity.oldExecutableIdentity.canonicalPath,
            &oldExecutableIdentityPath) ||
        !parseAbsoluteWindowsPath(raw.invocation.oldProcess.imagePath,
                                  &oldProcessImagePath) ||
        oldExecutableIdentityPath.normalized != oldProcessImagePath.normalized ||
        raw.identity.oldExecutableIdentity.volumeSerial !=
            raw.invocation.oldProcess.imageVolumeSerial ||
        raw.identity.oldExecutableIdentity.fileId !=
            raw.invocation.oldProcess.imageFileId) {
        result.error = PlanError::InvalidInvocationIdentity;
        return result;
    }
    if (raw.identity.protectedDataRoots.empty()) {
        result.error = PlanError::MissingProtectedDataRoots;
        return result;
    }

    std::vector<std::string> normalizedDataRoots;
    normalizedDataRoots.reserve(raw.identity.protectedDataRoots.size());
    for (const auto& rawDataRoot : raw.identity.protectedDataRoots) {
        WindowsPath dataRoot;
        if (!parseAbsoluteWindowsPath(rawDataRoot, &dataRoot) || dataRoot.components.empty()) {
            result.error = PlanError::InvalidProtectedDataRoot;
            return result;
        }
        if (isSameOrDescendant(target, dataRoot) || isSameOrDescendant(dataRoot, target)) {
            result.error = PlanError::DataDirectoryRejected;
            return result;
        }
        normalizedDataRoots.push_back(dataRoot.normalized);
    }

    if (!isSafeIdentifier(raw.invocation.invocationIdentity, 1, 256)) {
        result.error = PlanError::InvalidInvocationIdentity;
        return result;
    }
    WindowsPath updaterBase;
    if (!parseAbsoluteWindowsPath(raw.invocation.updaterOwnedBase,
                                  &updaterBase) ||
        updaterBase.components.empty() || isSystemDirectory(updaterBase)) {
        result.error = PlanError::InvalidUpdaterOwnedBase;
        return result;
    }
    WindowsPath stagingRoot;
    if (!parseAbsoluteWindowsPath(raw.invocation.stagingRoot, &stagingRoot) ||
        stagingRoot.components.empty()) {
        result.error = PlanError::InvalidStagingRoot;
        return result;
    }
    WindowsPath transactionRoot;
    if (!parseAbsoluteWindowsPath(raw.invocation.transactionRoot,
                                  &transactionRoot) ||
        transactionRoot.components.empty()) {
        result.error = PlanError::InvalidTransactionRoot;
        return result;
    }
    if (!isSameOrDescendant(stagingRoot, updaterBase) ||
        stagingRoot.components.size() == updaterBase.components.size() ||
        !isSameOrDescendant(transactionRoot, updaterBase) ||
        transactionRoot.components.size() == updaterBase.components.size() ||
        isSameOrDescendant(stagingRoot, transactionRoot) ||
        isSameOrDescendant(transactionRoot, stagingRoot) ||
        isSameOrDescendant(target, updaterBase) ||
        isSameOrDescendant(updaterBase, target)) {
        result.error = PlanError::InvocationRootOverlap;
        return result;
    }

    if (raw.inventory.empty()) {
        result.error = PlanError::EmptyInventory;
        return result;
    }
    if (raw.inventory.size() > kMaximumInventoryItems) {
        result.error = PlanError::InvalidRelativePath;
        return result;
    }
    std::set<std::string> inventoryPaths;
    std::vector<InstallInventoryItem> normalizedInventory;
    normalizedInventory.reserve(raw.inventory.size());
    for (const auto& rawItem : raw.inventory) {
        const auto normalizedPath = normalizeRelativeInstallPath(rawItem.relativePath);
        if (!normalizedPath) {
            result.error = PlanError::InvalidRelativePath;
            return result;
        }
        const std::string insensitivePath = toLowerAscii(*normalizedPath);
        if (!inventoryPaths.insert(insensitivePath).second) {
            result.error = PlanError::DuplicateInventoryPath;
            return result;
        }
        if (!isLowerHex(rawItem.sha256, 64)) {
            result.error = PlanError::InvalidInventoryDigest;
            return result;
        }
        normalizedInventory.push_back({*normalizedPath, rawItem.sha256, rawItem.byteSize});
    }

    if (raw.removals.size() > kMaximumInventoryItems) {
        result.error = PlanError::InvalidRemovalPath;
        return result;
    }
    std::set<std::string> removalPaths;
    std::vector<std::string> normalizedRemovals;
    normalizedRemovals.reserve(raw.removals.size());
    for (const std::string& rawPath : raw.removals) {
        const auto normalizedPath = normalizeRelativeInstallPath(rawPath);
        if (!normalizedPath) {
            result.error = PlanError::InvalidRemovalPath;
            return result;
        }
        const std::string insensitivePath = toLowerAscii(*normalizedPath);
        if (!removalPaths.insert(insensitivePath).second) {
            result.error = PlanError::DuplicateRemovalPath;
            return result;
        }
        if (inventoryPaths.find(insensitivePath) != inventoryPaths.end()) {
            result.error = PlanError::InventoryRemovalOverlap;
            return result;
        }
        normalizedRemovals.push_back(*normalizedPath);
    }

    const auto normalizedEntrypoint = normalizeRelativeInstallPath(raw.entrypoint);
    if (!normalizedEntrypoint) {
        result.error = PlanError::InvalidEntrypoint;
        return result;
    }
    if (inventoryPaths.find(toLowerAscii(*normalizedEntrypoint)) == inventoryPaths.end()) {
        result.error = PlanError::EntrypointNotInInventory;
        return result;
    }

    ValidatedInstallPlan plan;
    plan.identity.deploymentMode = raw.identity.deploymentMode;
    plan.identity.targetRoot = target.normalized;
    plan.identity.installationId = raw.identity.installationId;
    plan.identity.registeredAppId = raw.identity.registeredAppId;
    plan.identity.targetRootIdentity = raw.identity.targetRootIdentity;
    plan.identity.oldExecutableIdentity = raw.identity.oldExecutableIdentity;
    plan.identity.protectedDataRoots = std::move(normalizedDataRoots);
    plan.invocation.updaterOwnedBase = updaterBase.normalized;
    plan.invocation.deploymentMode = raw.invocation.deploymentMode;
    plan.invocation.installationId = raw.invocation.installationId.empty()
        ? raw.identity.installationId : raw.invocation.installationId;
    plan.invocation.targetRootIdentityBinding = raw.invocation.targetRootIdentityBinding;
    plan.invocation.oldExecutableIdentityBinding = raw.invocation.oldExecutableIdentityBinding;
    plan.invocation.stagingRoot = stagingRoot.normalized;
    plan.invocation.transactionRoot = transactionRoot.normalized;
    plan.invocation.invocationIdentity =
        raw.invocation.invocationIdentity;
    plan.invocation.transactionId = raw.invocation.transactionId;
    plan.invocation.packagePlanDigest = raw.invocation.packagePlanDigest;
    plan.invocation.healthNonceDigest = raw.invocation.healthNonceDigest;
    plan.invocation.parentProcess = raw.invocation.parentProcess;
    plan.invocation.oldProcess = raw.invocation.oldProcess;
    plan.version = raw.version;
    plan.buildId = raw.buildId;
    plan.packageKeyId = raw.keyId;
    plan.packageChannel = raw.channel;
    plan.packagePlanDigest = raw.packagePlanDigest;
    plan.oldProcessId = raw.oldProcessId;
    plan.healthToken = raw.healthToken;
    plan.entrypoint = *normalizedEntrypoint;
    plan.inventory = std::move(normalizedInventory);
    plan.removals = std::move(normalizedRemovals);
    plan.validationBinding_ = validatedPlanBinding(plan);
    plan.validated_ = true;
    result.plan = std::move(plan);
    return result;
}

JournalBuildResult BuildReplacementJournal(
    const ValidatedInstallPlan& plan,
    const std::vector<ExistingInstallItem>& existingInventory,
    const std::string& transactionId) {
    JournalBuildResult result;
    if (!plan.isValidated()) {
        result.error = JournalError::InvalidPlan;
        return result;
    }
    if (!isSafeIdentifier(transactionId, 1, 128)) {
        result.error = JournalError::InvalidTransactionId;
        return result;
    }

    std::vector<std::string> normalizedExisting;
    normalizedExisting.reserve(existingInventory.size());
    std::set<std::string> existingPaths;
    for (const auto& existing : existingInventory) {
        const auto normalizedPath = normalizeRelativeInstallPath(existing.relativePath);
        if (!normalizedPath) {
            result.error = JournalError::InvalidExistingPath;
            return result;
        }
        const std::string insensitivePath = toLowerAscii(*normalizedPath);
        if (!existingPaths.insert(insensitivePath).second) {
            result.error = JournalError::DuplicateExistingPath;
            return result;
        }
        normalizedExisting.push_back(*normalizedPath);
    }

    ReplacementJournal journal;
    journal.transactionId = transactionId;
    for (const auto& incoming : plan.inventory) {
        const bool exists = existingPaths.find(toLowerAscii(incoming.relativePath)) !=
                            existingPaths.end();
        journal.items.push_back({incoming.relativePath,
                                 exists ? JournalItemClass::ReplaceExisting
                                        : JournalItemClass::CreateNew,
                                 exists});
    }
    for (const std::string& removal : plan.removals) {
        journal.items.push_back(
            {removal, JournalItemClass::RemoveExisting, true});
    }
    std::set<std::string> incomingPaths;
    for (const auto& incoming : plan.inventory) {
        incomingPaths.insert(toLowerAscii(incoming.relativePath));
    }
    std::set<std::string> removalPaths;
    for (const std::string& removal : plan.removals) {
        removalPaths.insert(toLowerAscii(removal));
    }
    for (const auto& existing : normalizedExisting) {
        const std::string folded = toLowerAscii(existing);
        if (incomingPaths.find(folded) == incomingPaths.end() &&
            removalPaths.find(folded) == removalPaths.end()) {
            journal.items.push_back(
                {existing, JournalItemClass::PreserveExisting, false});
        }
    }
    journal.planBinding_ = validatedPlanBinding(plan);
    journal.structureBinding_ = journalStructureBinding(journal);
    journal.validated_ = true;
    result.journal = std::move(journal);
    return result;
}

JournalBuildResult RehydrateReplacementJournal(
    const ValidatedInstallPlan& plan,
    const std::vector<ReplacementJournalItem>& durableItems,
    const std::string& transactionId) {
    JournalBuildResult result;
    if (!plan.isValidated()) {
        result.error = JournalError::InvalidPlan;
        return result;
    }
    if (!isSafeIdentifier(transactionId, 1, 128) ||
        (!plan.invocation.transactionId.empty() &&
         transactionId != plan.invocation.transactionId)) {
        result.error = JournalError::InvalidTransactionId;
        return result;
    }

    std::set<std::string> incoming;
    for (const InstallInventoryItem& item : plan.inventory) {
        incoming.insert(toLowerAscii(item.relativePath));
    }
    std::set<std::string> removals;
    for (const std::string& path : plan.removals) {
        removals.insert(toLowerAscii(path));
    }
    std::set<std::string> seen;
    std::set<std::string> actionable;
    std::set<std::string> actionableRemovals;
    ReplacementJournal journal;
    journal.transactionId = transactionId;
    journal.items.reserve(durableItems.size());
    for (ReplacementJournalItem item : durableItems) {
        const auto normalized = normalizeRelativeInstallPath(item.relativePath);
        if (!normalized) {
            result.error = JournalError::InvalidDurableJournal;
            return result;
        }
        item.relativePath = *normalized;
        const std::string folded = toLowerAscii(item.relativePath);
        if (!seen.insert(folded).second) {
            result.error = JournalError::InvalidDurableJournal;
            return result;
        }
        const bool isIncoming = incoming.find(folded) != incoming.end();
        if (isIncoming) {
            if (item.classification == JournalItemClass::PreserveExisting ||
                item.classification == JournalItemClass::RemoveExisting ||
                item.requiresBackup !=
                    (item.classification == JournalItemClass::ReplaceExisting) ||
                (item.backupRecorded && !item.requiresBackup)) {
                result.error = JournalError::InvalidDurableJournal;
                return result;
            }
            actionable.insert(folded);
        } else if (removals.find(folded) != removals.end()) {
            if (item.classification != JournalItemClass::RemoveExisting ||
                !item.requiresBackup ||
                (item.replacementRecorded && !item.backupRecorded)) {
                result.error = JournalError::InvalidDurableJournal;
                return result;
            }
            actionableRemovals.insert(folded);
        } else {
            if (item.classification != JournalItemClass::PreserveExisting ||
                item.requiresBackup || item.backupRecorded ||
                item.replacementRecorded) {
                result.error = JournalError::InvalidDurableJournal;
                return result;
            }
        }
        journal.items.push_back(std::move(item));
    }
    if (actionable != incoming || actionableRemovals != removals) {
        result.error = JournalError::InvalidDurableJournal;
        return result;
    }

    journal.planBinding_ = validatedPlanBinding(plan);
    journal.structureBinding_ = journalStructureBinding(journal);
    journal.validated_ = true;
    result.journal = std::move(journal);
    return result;
}

FailureDisposition NextFailureDisposition(const std::uint32_t previousFailureCount) {
    FailureDisposition result;
    if (previousFailureCount == std::numeric_limits<std::uint32_t>::max()) {
        result.recordedFailureCount = previousFailureCount;
    } else {
        result.recordedFailureCount = previousFailureCount + 1;
    }
    result.quarantine = result.recordedFailureCount >= 2;
    return result;
}

TransactionDecision StartTransaction(
    const ValidatedInstallPlan& plan,
    const ReplacementJournal& journal,
    const std::uint32_t previousFailureCount) {
    TransactionDecision result;
    result.state.plan = plan;
    result.state.journal = journal;
    result.state.previousFailureCount = previousFailureCount;
    if (!plan.isValidated() || !journal.isValidatedFor(plan)) {
        return failBeforeMutation(result.state, "invalid-transaction-input");
    }
    result.state.phase = TransactionPhase::WaitingForOldProcess;
    result.intents.push_back(intent(BootstrapIntentKind::WaitForOldProcessExit,
                                    {},
                                    plan.oldProcessId));
    return result;
}

TransactionDecision OnOldProcessWait(
    const TransactionState& previous,
    const ProcessWaitOutcome outcome) {
    TransactionDecision result;
    result.state = previous;
    if (previous.phase != TransactionPhase::WaitingForOldProcess) return result;
    if (!previous.plan.isValidated() ||
        !previous.journal.isValidatedFor(previous.plan)) {
        return blockRecovery(previous, "invalid-transaction-state");
    }
    if (outcome != ProcessWaitOutcome::Exited) {
        return failBeforeMutation(previous, "old-process-timeout");
    }

    result.state.phase = TransactionPhase::AwaitingUpdatedChildProcess;
    result.intents.push_back(intent(BootstrapIntentKind::PersistJournal, {}, 0, "prepared"));
    for (const auto& item : previous.journal.items) {
        if (item.classification == JournalItemClass::ReplaceExisting ||
            item.classification == JournalItemClass::RemoveExisting) {
            result.intents.push_back(intent(BootstrapIntentKind::SnapshotBackup,
                                            item.relativePath));
        }
    }
    for (const auto& item : previous.journal.items) {
        if (item.classification == JournalItemClass::ReplaceExisting ||
            item.classification == JournalItemClass::CreateNew) {
            result.intents.push_back(intent(BootstrapIntentKind::ReplaceInventoryItem,
                                            item.relativePath));
        } else if (item.classification == JournalItemClass::RemoveExisting) {
            result.intents.push_back(intent(BootstrapIntentKind::RemoveInstalledItem,
                                            item.relativePath));
        }
    }
    result.intents.push_back(intent(BootstrapIntentKind::LaunchUpdatedChild,
                                    previous.plan.entrypoint));
    return result;
}

UpdatedChildRecordDecision RecordUpdatedChildProcess(
    const TransactionState& previous,
    const std::uint64_t processId) {
    UpdatedChildRecordDecision result;
    result.transaction.state = previous;
    if (!previous.plan.isValidated() ||
        !previous.journal.isValidatedFor(previous.plan)) {
        result.result = UpdatedChildRecordResult::InvalidState;
        result.transaction = blockRecovery(previous, "invalid-transaction-state");
        return result;
    }
    if (previous.phase == TransactionPhase::AwaitingHealth &&
        previous.updatedChildProcessId == processId && processId != 0) {
        result.result = UpdatedChildRecordResult::Idempotent;
        return result;
    }
    if (previous.phase != TransactionPhase::AwaitingUpdatedChildProcess) {
        result.result = UpdatedChildRecordResult::InvalidState;
        return result;
    }
    if (processId == 0) {
        result.result = UpdatedChildRecordResult::InvalidProcessId;
        return result;
    }

    result.result = UpdatedChildRecordResult::Recorded;
    result.transaction.state.phase = TransactionPhase::AwaitingHealth;
    result.transaction.state.updatedChildProcessId = processId;
    result.transaction.intents.push_back(intent(BootstrapIntentKind::PersistJournal,
                                                {},
                                                processId,
                                                "updated-child-recorded"));
    result.transaction.intents.push_back(intent(BootstrapIntentKind::AwaitHealthAcknowledgement,
                                                {},
                                                processId));
    return result;
}

JournalRecordDecision RecordBackupSnapshot(
    const TransactionState& previous,
    const std::string& relativePath) {
    if (!isReplacementProgressPhase(previous.phase) ||
        !previous.plan.isValidated() ||
        !previous.journal.isValidatedFor(previous.plan)) {
        return journalRecordFailure(previous, JournalRecordResult::InvalidState);
    }
    if (!normalizeRelativeInstallPath(relativePath)) {
        return journalRecordFailure(previous, JournalRecordResult::InvalidPath);
    }
    const auto itemIndex = journalItemIndex(previous.journal, relativePath);
    if (!itemIndex) {
        return journalRecordFailure(previous, JournalRecordResult::InvalidItem);
    }
    const auto& original = previous.journal.items[*itemIndex];
    if (original.classification != JournalItemClass::ReplaceExisting &&
        original.classification != JournalItemClass::RemoveExisting) {
        return journalRecordFailure(previous, JournalRecordResult::InvalidItem);
    }
    if (original.backupRecorded) {
        return journalRecordFailure(previous, JournalRecordResult::Idempotent);
    }

    JournalRecordDecision result;
    result.result = JournalRecordResult::Recorded;
    result.transaction.state = previous;
    auto& item = result.transaction.state.journal.items[*itemIndex];
    item.backupRecorded = true;
    result.transaction.intents.push_back(intent(BootstrapIntentKind::PersistJournal,
                                                item.relativePath,
                                                0,
                                                "backup-recorded"));
    return result;
}

JournalRecordDecision RecordReplacementWritten(
    const TransactionState& previous,
    const std::string& relativePath) {
    if (!isReplacementProgressPhase(previous.phase) ||
        !previous.plan.isValidated() ||
        !previous.journal.isValidatedFor(previous.plan)) {
        return journalRecordFailure(previous, JournalRecordResult::InvalidState);
    }
    if (!normalizeRelativeInstallPath(relativePath)) {
        return journalRecordFailure(previous, JournalRecordResult::InvalidPath);
    }
    const auto itemIndex = journalItemIndex(previous.journal, relativePath);
    if (!itemIndex) {
        return journalRecordFailure(previous, JournalRecordResult::InvalidItem);
    }
    const auto& original = previous.journal.items[*itemIndex];
    if (original.classification == JournalItemClass::PreserveExisting) {
        return journalRecordFailure(previous, JournalRecordResult::InvalidItem);
    }
    if (original.replacementRecorded) {
        return journalRecordFailure(previous, JournalRecordResult::Idempotent);
    }
    if ((original.classification == JournalItemClass::ReplaceExisting ||
         original.classification == JournalItemClass::RemoveExisting) &&
        !original.backupRecorded) {
        return journalRecordFailure(previous, JournalRecordResult::BackupRequired);
    }

    JournalRecordDecision result;
    result.result = JournalRecordResult::Recorded;
    result.transaction.state = previous;
    auto& item = result.transaction.state.journal.items[*itemIndex];
    item.replacementRecorded = true;
    result.transaction.intents.push_back(intent(BootstrapIntentKind::PersistJournal,
                                                item.relativePath,
                                                0,
                                                "replacement-recorded"));
    return result;
}

HealthDecision AcknowledgeHealth(
    const TransactionState& previous,
    const HealthAcknowledgement& acknowledgement) {
    HealthDecision result;
    result.transaction.state = previous;
    if (previous.phase == TransactionPhase::Committed || previous.healthTokenConsumed) {
        result.health = HealthAcknowledgementResult::ReplayRejected;
        return result;
    }
    if (previous.phase != TransactionPhase::AwaitingHealth) {
        result.health = HealthAcknowledgementResult::NotAwaitingHealth;
        return result;
    }
    if (!previous.plan.isValidated() ||
        !previous.journal.isValidatedFor(previous.plan)) {
        result.health = HealthAcknowledgementResult::IncompleteReplacement;
        result.transaction = blockRecovery(previous, "invalid-transaction-state");
        return result;
    }
    if (previous.updatedChildProcessId == 0) {
        result.health = HealthAcknowledgementResult::NotAwaitingHealth;
        return result;
    }
    if (acknowledgement.processId != 0U &&
        acknowledgement.processId != previous.updatedChildProcessId) {
        result.health = HealthAcknowledgementResult::ProcessMismatch;
        result.transaction = rollback(previous, "health-process-mismatch");
        return result;
    }
    if (!allIncomingReplacementsRecorded(previous.journal)) {
        result.health = HealthAcknowledgementResult::IncompleteReplacement;
        result.transaction = rollback(previous, "incomplete-replacement-record");
        return result;
    }
    if (!constantTimeEquals(previous.plan.healthToken, acknowledgement.token)) {
        result.health = HealthAcknowledgementResult::TokenMismatch;
        result.transaction = rollback(previous, "health-token-mismatch");
        return result;
    }
    if (!acknowledgement.uiReady) {
        result.health = HealthAcknowledgementResult::UiNotReady;
        result.transaction = rollback(previous, "health-ui-not-ready");
        return result;
    }
    if (previous.plan.version != acknowledgement.version) {
        result.health = HealthAcknowledgementResult::VersionMismatch;
        result.transaction = rollback(previous, "health-version-mismatch");
        return result;
    }
    if (previous.plan.buildId != acknowledgement.buildId) {
        result.health = HealthAcknowledgementResult::BuildIdMismatch;
        result.transaction = rollback(previous, "health-build-id-mismatch");
        return result;
    }

    result.health = HealthAcknowledgementResult::Accepted;
    result.transaction.state.phase = TransactionPhase::Committed;
    result.transaction.state.healthTokenConsumed = true;
    result.transaction.intents.push_back(intent(BootstrapIntentKind::PersistJournal, {}, 0,
                                                "committing"));
    result.transaction.intents.push_back(intent(BootstrapIntentKind::CommitJournal));
    result.transaction.intents.push_back(intent(BootstrapIntentKind::RemoveBackup));
    return result;
}

TransactionDecision CommitTransaction(const TransactionState& state) {
    TransactionDecision result;
    result.state = state;
    return result;
}

TransactionDecision RollbackTransaction(const TransactionState& state) {
    return rollback(state, "explicit-rollback");
}

TransactionDecision RollbackAfterHealthTimeout(const TransactionState& state) {
    return rollback(state, "health-timeout");
}

TransactionDecision RollbackForUpdatedChildCrash(const TransactionState& state) {
    return rollback(state, "updated-child-crash");
}

}  // namespace aris::bootstrap
