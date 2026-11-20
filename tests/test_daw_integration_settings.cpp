#include "SettingsDawTargets.h"
#include "ReaperSetupPolicy.h"
#include "platform/durable_file_ops.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using harken::settings::DawTargetDraft;
using namespace HarkenSearch::DawBridge;

static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static void DraftMergesOnlyEditedFields() {
    auto saved = std::vector<DawTarget>{harken::settings::kSettingsDawPresets[0].MakeTarget()};
    saved.front().exePath = "initial";
    DawTargetDraft editor;
    editor.Begin(saved);
    Check(editor.ChangedCount() == 0, "Opening the page must not add other DAWs");
    editor.Value()[0].enabled = false;
    editor.Value()[1].exePath = "ProTools.exe";
    editor.Value()[1].enabled = true;
    saved.front().exePath = "newer value from legacy UI";
    saved.front().lastDetectTime = 15.0;
    saved.front().isRunning = true;
    editor.MergeInto(saved);
    Check(saved.size() == 2 && saved[1].name == "Pro Tools", "Only edited missing DAW is added");
    Check(!saved[0].enabled && saved[1].enabled, "Send toggles were committed");
    Check(saved[0].exePath == "newer value from legacy UI", "Unedited live path must survive");
    Check(saved[0].lastDetectTime == 15.0 && saved[0].isRunning, "Runtime state was overwritten");
    editor.Begin(saved);
    editor.Value()[0].exePath = "discarded";
    editor.Begin(saved);
    Check(editor.ChangedCount() == 0 && editor.Value()[0].exePath == saved[0].exePath,
          "Revert must take the current live value");
}

static void DraftDoesNotAppendPastThePersistedLimit() {
    std::vector<DawTarget> saved(9);
    DawTargetDraft editor;
    editor.Begin(saved);
    editor.Value()[0].exePath = "reaper.exe";
    Check(!editor.MergeInto(saved) && saved.size() == 9,
          "A full config must not appear to save a path that is discarded on restart");
}

static void InstallAndNoticeUseDifferentGuards() {
    InstallStatus status;
    status.hostInstalled = true;
    status.targetResolved = true;
    status.bundledFilePresent = true;
    status.state = InstallState::NotInstalled;
    Check(CanInstallMissingReaperBridge(status), "Stopped, missing bridge is installable");
    Check(NeedsReaperSetupNotice(status), "Detected host without the bridge needs the notice");
    status.hostRunning = true;
    Check(!CanInstallMissingReaperBridge(status) && NeedsReaperSetupNotice(status),
          "Running must block installation but can still need a notice");
    status.filePresent = true;
    Check(!CanInstallMissingReaperBridge(status) && !NeedsReaperSetupNotice(status),
          "Existing files are not an invitation to overwrite");
    status.filePresent = false;
    status.hostRunning = false;
    status.alternateFilePresent = true;
    Check(!CanInstallMissingReaperBridge(status) && !NeedsReaperSetupNotice(status),
          "Bridge at another location is a diagnostic case, not a blank install");
    status.alternateFilePresent = false;
    status.legacyFilePresent = true;
    Check(!CanInstallMissingReaperBridge(status) && !NeedsReaperSetupNotice(status),
          "An existing legacy component must not prompt or get a second DLL");
    status.legacyFilePresent = false;
    status.hostInstalled = false;
    Check(!CanInstallMissingReaperBridge(status) && !NeedsReaperSetupNotice(status),
          "Missing REAPER must not install or show a notice");
}

static void PublishDoesNotReplaceAnExistingFile(const std::filesystem::path& root) {
    const auto staged = root / "staged.dll";
    const auto destination = root / "existing.dll";
    std::ofstream(staged) << "staged";
    std::ofstream(destination) << "unrelated";
    using namespace Platform::FileSystem;
    Check(ReplaceFileWriteThrough(staged.u8string(), destination.u8string(), false).status ==
          DurableFileOpStatus::TargetExists, "Exclusive commit must refuse an existing file");
    std::ifstream before(destination);
    std::string text;
    before >> text;
    Check(text == "unrelated" && std::filesystem::exists(staged), "Exclusive commit changed a file");
    const auto newTarget = root / "empty.dll";
    Check(ReplaceFileWriteThrough(staged.u8string(), newTarget.u8string(), false).status ==
          DurableFileOpStatus::Success, "Exclusive commit must publish to a free path");
    Check(std::filesystem::exists(newTarget) && !std::filesystem::exists(staged),
          "Exclusive commit must move, not copy");
}

int RunDawIntegrationSettingsChecks(const std::filesystem::path& root) {
    try {
        DraftMergesOnlyEditedFields();
        DraftDoesNotAppendPastThePersistedLimit();
        InstallAndNoticeUseDifferentGuards();
        PublishDoesNotReplaceAnExistingFile(root);
        std::cout << "PASS: DAW draft, REAPER guards, exclusive file publication\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
