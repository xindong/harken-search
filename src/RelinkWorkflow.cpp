#include "RelinkWorkflow.h"

#include "AppState.h"
#include "HarkenSearchWidgets.h"
#include "Localization.h"
#include "Theme.h"
#include "UiHostServices.h"
#include "Utils.h"
#include "imgui.h"
#include "platform/audio_file_dialog.h"
#include "platform/filesystem.h"
#include "platform/system.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

namespace aura::ui {
namespace {

static void JoinRelinkThreadWithTimeout(std::thread& worker) {
    if (!worker.joinable()) {
        return;
    }
#ifdef _WIN32
    void* handle = reinterpret_cast<void*>(worker.native_handle());
    if (Platform::WaitForNativeThreadHandle(
            handle, static_cast<uint32_t>(kRelinkShutdownJoinTimeoutMs))) {
        worker.join();
        return;
    }
    worker.detach();
#else
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kRelinkShutdownJoinTimeoutMs);
    while (worker.joinable() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (worker.joinable()) {
        worker.detach();
    }
#endif
}

static void DrainRetiredRelinkThreads(AppState& state) {
    for (auto& worker : state.retiredRelinkSearchThreads) {
        JoinRelinkThreadWithTimeout(worker);
    }
    state.retiredRelinkSearchThreads.clear();
}

static void RetireRelinkThread(AppState& state, std::thread& worker) {
    if (worker.joinable()) {
        state.retiredRelinkSearchThreads.emplace_back(std::move(worker));
    }
}

static void RequestRelinkSearchCancel(std::atomic<bool>& cancelFlag) {
    cancelFlag.store(true, std::memory_order_release);
}

static void ArmRelinkSearch(std::atomic<bool>& cancelFlag) {
    cancelFlag.store(false, std::memory_order_release);
}

static bool ApplyRelinkPathUpdate(
    AppState& state,
    const std::string& oldPath,
    const std::string& newPath) {
    const auto sourceStatus = Platform::FileSystem::ProbePath(oldPath);
    if (sourceStatus == Platform::FileSystem::PathStatus::Present) {
        state.relinkPathUpdateStatus = Database::RelinkPathUpdateStatus::SourceChanged;
    } else if (sourceStatus != Platform::FileSystem::PathStatus::Missing) {
        state.relinkPathUpdateStatus = Database::RelinkPathUpdateStatus::Failed;
    } else {
        const auto targetInfo = Platform::FileSystem::ProbeRegularFile(newPath);
        if (!targetInfo.isRegularFile || !targetInfo.sizeAvailable ||
            !Platform::FileSystem::CanOpenFileForRead(newPath)) {
            state.relinkPathUpdateStatus =
                Database::RelinkPathUpdateStatus::TargetUnavailable;
        } else {
            const std::string fileName = PathToUtf8(Utf8ToPath(newPath).filename());
            if (state.database.updateFilePath(
                    oldPath,
                    newPath,
                    fileName,
                    &state.relinkPathUpdateStatus)) {
                return true;
            }
        }
    }
    state.loadErrorToast = u8"重新关联失败：数据库更新未完成，请重试。";
    state.loadErrorToastUntil = ImGui::GetTime() + 6.0;
    return false;
}

static const char* RelinkMatchReasonText(Database::RelinkMatchReason reason) {
    switch (reason) {
        case Database::RelinkMatchReason::ExactFileNameAndSize:
            return L(Relink_MatchReasonExactNameSize);
    }
    return L(Relink_MatchReasonExactNameSize);
}

static const char* RelinkPathUpdateFailureText(
    Database::RelinkPathUpdateStatus status) {
    switch (status) {
        case Database::RelinkPathUpdateStatus::SourceChanged:
            return L(Relink_UpdateSourceChanged);
        case Database::RelinkPathUpdateStatus::TargetUnavailable:
            return L(Relink_UpdateTargetUnavailable);
        case Database::RelinkPathUpdateStatus::PathConflict:
            return L(Relink_UpdatePathConflict);
        case Database::RelinkPathUpdateStatus::Failed:
            return L(Relink_UpdateFailed);
        case Database::RelinkPathUpdateStatus::Updated:
            break;
    }
    return L(Relink_UpdateFailed);
}

}  // namespace

void EvaluateRelinkForRecord(AppState& state, const AudioRecord& record) {
    RequestRelinkSearchCancel(state.relinkBannerSearchCancel);
    const uint64_t generation =
        state.relinkBannerSearchGeneration.fetch_add(
            1, std::memory_order_acq_rel) + 1;
    if (state.relinkIgnoredThisSession.count(record.filePath) ||
        state.relinkDeferredThisSession) {
        return;
    }

    bool fileMissing = false;
    try {
        fileMissing = !Platform::FileSystem::Exists(record.filePath);
    } catch (...) {
        fileMissing = true;
    }

    if (!fileMissing) {
        if (state.relinkBanner.visible &&
            state.relinkBanner.missingPath == record.filePath) {
            state.relinkBanner.visible = false;
            state.relinkBanner.promptPending = false;
        }
        return;
    }

    const bool wasVisible = state.relinkBanner.visible;
    const bool pathChanged = state.relinkBanner.missingPath != record.filePath;
    state.relinkBanner.visible = true;
    state.relinkBanner.promptPending = !wasVisible || pathChanged;
    state.relinkBanner.missingPath = record.filePath;
    state.relinkBanner.searching = true;
    state.relinkBanner.candidates.clear();
    state.relinkBanner.searchReport = {};
    state.relinkBanner.searchReport.status = Database::RelinkSearchStatus::Searching;
    state.relinkBannerSearchScannedEntries.store(0, std::memory_order_release);
    state.relinkBanner.selectedCandidate = 0;
    state.relinkBannerSearchDone.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
        state.pendingRelinkCandidates.clear();
        state.pendingRelinkBannerSearchReport = {};
        state.pendingRelinkBannerGeneration = 0;
    }

    const std::string mp = record.filePath;
    RetireRelinkThread(state, state.relinkBannerSearchThread);
    DrainRetiredRelinkThreads(state);
    ArmRelinkSearch(state.relinkBannerSearchCancel);
    if (state.relinkBannerSearchFn) {
        state.relinkBannerSearchThread = std::thread(state.relinkBannerSearchFn, std::ref(state), mp, generation);
    } else {
        state.relinkBannerSearchThread = std::thread([&state, mp, generation]() {
            auto roots = state.database.getWatchedRoots();
            auto report = state.database.findRelinkCandidatesDetailed(
                roots,
                &state.relinkBannerSearchCancel,
                &state.relinkBannerSearchScannedEntries);
            std::vector<std::string> cands;
            Database::RelinkCandidateMatches targetMatches;
            for (auto& [cand, paths] : report.candidates) {
                if (cand.dbPath == mp) {
                    cands.reserve(paths.size());
                    for (const auto& path : paths) cands.push_back(path.path);
                    targetMatches.emplace_back(std::move(cand), std::move(paths));
                    break;
                }
            }
            report.candidates = std::move(targetMatches);
            if (state.relinkBannerSearchGeneration.load(std::memory_order_acquire) !=
                generation) {
                return;
            }
            std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
            if (state.relinkBannerSearchGeneration.load(std::memory_order_acquire) !=
                generation) {
                return;
            }
            state.pendingRelinkCandidates = std::move(cands);
            state.pendingRelinkBannerSearchReport = std::move(report);
            state.pendingRelinkBannerGeneration = generation;
            state.relinkBannerSearchDone.store(true, std::memory_order_release);
        });
    }
}

void PumpRelinkWorkflow(AppState& state) {
    if (state.relinkBannerSearchDone.exchange(false)) {
        std::vector<std::string> candidates;
        Database::RelinkSearchResult searchReport;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
            generation = state.pendingRelinkBannerGeneration;
            candidates = std::move(state.pendingRelinkCandidates);
            searchReport = std::move(state.pendingRelinkBannerSearchReport);
            state.pendingRelinkBannerGeneration = 0;
        }
        if (generation == state.relinkBannerSearchGeneration.load(
                std::memory_order_acquire)) {
            state.relinkBanner.searching = false;
            state.relinkBanner.candidates = std::move(candidates);
            state.relinkBanner.searchReport = std::move(searchReport);
        }
    }
    if (state.relinkToolSearchDone.exchange(
            false, std::memory_order_acquire)) {
        decltype(state.relinkTool.results) results;
        Database::RelinkSearchResult searchReport;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
            generation = state.pendingRelinkToolGeneration;
            results = std::move(state.pendingRelinkToolResults);
            searchReport = std::move(state.pendingRelinkToolSearchReport);
            state.pendingRelinkToolGeneration = 0;
        }
        if (generation == state.relinkToolSearchGeneration.load(
                std::memory_order_acquire)) {
            state.relinkTool.results = std::move(results);
            state.relinkTool.searchReport = std::move(searchReport);
            state.relinkTool.scanning = false;
        }
    }
}

void ShutdownRelinkWorkflow(AppState& state) {
    state.relinkBannerSearchGeneration.fetch_add(1, std::memory_order_acq_rel);
    state.relinkToolSearchGeneration.fetch_add(1, std::memory_order_acq_rel);
    state.relinkBannerSearchDone.store(false, std::memory_order_release);
    state.relinkToolSearchDone.store(false, std::memory_order_release);
    RequestRelinkSearchCancel(state.relinkBannerSearchCancel);
    RequestRelinkSearchCancel(state.relinkToolSearchCancel);

    RetireRelinkThread(state, state.relinkBannerSearchThread);
    RetireRelinkThread(state, state.relinkToolSearchThread);
    DrainRetiredRelinkThreads(state);
    {
        std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
        state.pendingRelinkCandidates.clear();
        state.pendingRelinkToolResults.clear();
        state.pendingRelinkBannerSearchReport = {};
        state.pendingRelinkToolSearchReport = {};
        state.pendingRelinkBannerGeneration = 0;
        state.pendingRelinkToolGeneration = 0;
    }
    state.relinkBanner.searching = false;
    state.relinkTool.scanning = false;
}

bool RenderRelinkBanner(AppState& state) {
    if (!state.relinkBanner.visible) return false;

    static constexpr const char* kRelinkPromptId = u8"音效关联文件丢失##RelinkPrompt";
    if (state.relinkBanner.promptPending) {
        ImGui::OpenPopup(kRelinkPromptId);
        state.relinkBanner.promptPending = false;
    }

    bool refreshSearch = false;
    if (BeginDialog(kRelinkPromptId, nullptr, 560.0f, 0.0f)) {
        ImGui::TextWrapped(L(Relink_MissingFileHeader));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", state.relinkBanner.missingPath.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(L(Relink_ChooseActionHint));
        ImGui::Spacing();

        ButtonOptions secondary;
        secondary.variant = ButtonVariant::Secondary;
        secondary.size = ButtonSize::Sm;
        if (Button(L(Relink_RemindLaterBtn), secondary)) {
            RequestRelinkSearchCancel(state.relinkBannerSearchCancel);
            state.relinkBannerSearchGeneration.fetch_add(1, std::memory_order_acq_rel);
            state.relinkDeferredThisSession = true;
            state.relinkBanner.visible = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();

        ButtonOptions primary;
        primary.variant = ButtonVariant::PrimarySearch;
        primary.size = ButtonSize::Sm;
        if (Button(L(Relink_OpenSettingsBtn), primary)) {
            RequestRelinkSearchCancel(state.relinkBannerSearchCancel);
            state.relinkBanner.visible = false;
            state.showSettingsDialog = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();

        ButtonOptions quiet;
        quiet.variant = ButtonVariant::Quiet;
        quiet.size = ButtonSize::Sm;
        if (Button(L(Relink_SkipThisBtn), quiet)) {
            RequestRelinkSearchCancel(state.relinkBannerSearchCancel);
            state.relinkBannerSearchGeneration.fetch_add(1, std::memory_order_acq_rel);
            state.relinkIgnoredThisSession.insert(state.relinkBanner.missingPath);
            state.relinkBanner.visible = false;
            ImGui::CloseCurrentPopup();
        }
        EndDialog();
    }
    return refreshSearch;
}

bool RenderRelinkTool(AppState& state) {
    bool refreshSearch = false;
    const auto& tcs = ThemeColors::current();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(tcs.metadataPanelLabel, "%s", L(Relink_Title));
    ImGui::Separator();
    ImGui::Spacing();

    static char rlkRootBuf[1024] = {};
    if (!state.relinkTool.searchRoot.empty() &&
        state.relinkTool.searchRoot.size() < sizeof(rlkRootBuf) &&
        rlkRootBuf[0] == '\0') {
        std::memcpy(
            rlkRootBuf,
            state.relinkTool.searchRoot.c_str(),
            state.relinkTool.searchRoot.size() + 1);
    }

    const float buttonWidth = 70.0f;
    const float scanButtonWidth = 90.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float contentRight = (std::min)(
        ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
        ImGui::GetWindowDrawList()->GetClipRectMax().x);
    const float contentWidth = contentRight - ImGui::GetCursorScreenPos().x;
    ImGui::SetNextItemWidth(
        contentWidth -
        buttonWidth - scanButtonWidth - spacing * 2.0f);
    if (ImGui::InputText(u8"##rlkRoot", rlkRootBuf, sizeof(rlkRootBuf))) {
        state.relinkTool.searchRoot = rlkRootBuf;
    }
    ImGui::SameLine();
    if (aura::ui::ActionButton(L(DAW_Browse), ImVec2(buttonWidth, 0))) {
        const std::string folder = Platform::BrowseFolderDialogModern(
            HarkenSearch::UiHost::MainWindowOwner(), u8"选择音频文件夹");
        if (!folder.empty()) {
#ifdef _WIN32
            strncpy_s(rlkRootBuf, sizeof(rlkRootBuf), folder.c_str(), _TRUNCATE);
#else
            std::snprintf(rlkRootBuf, sizeof(rlkRootBuf), "%s", folder.c_str());
#endif
            state.relinkTool.searchRoot = folder;
        }
    }
    ImGui::SameLine();

    const bool wasScanning = state.relinkTool.scanning;
    if (wasScanning) ImGui::BeginDisabled();
    if (aura::ui::ActionButton(L(Relink_AutoMatch), ImVec2(scanButtonWidth, 0), aura::ui::ButtonVariant::PrimarySearch)) {
        state.relinkTool.scanning = true;
        state.relinkTool.results.clear();
        state.relinkTool.confirmed = false;
        state.relinkTool.selectedByOrphanId.clear();
        state.relinkTool.updateFailures.clear();
        state.relinkTool.failedRootsExpanded = false;
        state.relinkTool.searchReport = {};
        state.relinkTool.searchReport.status = Database::RelinkSearchStatus::Searching;
        state.relinkToolSearchScannedEntries.store(0, std::memory_order_release);
        state.relinkToolSearchDone.store(false, std::memory_order_relaxed);
        const uint64_t generation =
            state.relinkToolSearchGeneration.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        {
            std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
            state.pendingRelinkToolResults.clear();
            state.pendingRelinkToolGeneration = 0;
        }

        std::vector<std::string> roots = state.database.getWatchedRoots();
        if (!state.relinkTool.searchRoot.empty()) {
            roots.push_back(state.relinkTool.searchRoot);
        }

        RequestRelinkSearchCancel(state.relinkToolSearchCancel);
        RetireRelinkThread(state, state.relinkToolSearchThread);
        DrainRetiredRelinkThreads(state);
        ArmRelinkSearch(state.relinkToolSearchCancel);
        if (state.relinkToolSearchFn) {
            state.relinkToolSearchThread = std::thread(state.relinkToolSearchFn, std::ref(state), roots, generation);
        } else {
            state.relinkToolSearchThread = std::thread(
                [&state, roots, generation]() mutable {
                auto report = state.database.findRelinkCandidatesDetailed(
                    roots,
                    &state.relinkToolSearchCancel,
                    &state.relinkToolSearchScannedEntries);
                auto results = report.candidates;
                if (state.relinkToolSearchGeneration.load(std::memory_order_acquire) !=
                    generation) {
                    return;
                }
                std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
                if (state.relinkToolSearchGeneration.load(std::memory_order_acquire) !=
                    generation) {
                    return;
                }
                state.pendingRelinkToolResults = std::move(results);
                state.pendingRelinkToolSearchReport = std::move(report);
                state.pendingRelinkToolGeneration = generation;
                state.relinkToolSearchDone.store(true, std::memory_order_release);
            });
        }
    }
    if (wasScanning) {
        ImGui::EndDisabled();
    }

    ImGui::TextDisabled("%s", L(Relink_ScopeHint));

    int canLink = 0;
    int needChoice = 0;
    int selectedChoices = 0;
    int notFound = 0;
    for (auto& [candidate, paths] : state.relinkTool.results) {
        if (paths.size() == 1) {
            ++canLink;
        } else if (paths.size() > 1) {
            ++needChoice;
            const auto selected =
                state.relinkTool.selectedByOrphanId.find(candidate.id);
            if (selected != state.relinkTool.selectedByOrphanId.end() &&
                selected->second >= 0 &&
                selected->second < static_cast<int>(paths.size())) {
                ++selectedChoices;
            }
        } else {
            ++notFound;
        }
    }

    const auto& searchReport = state.relinkTool.searchReport;
    if (searchReport.status == Database::RelinkSearchStatus::Searching) {
        const uint64_t scanned = state.relinkToolSearchScannedEntries.load(
            std::memory_order_acquire);
        if (state.relinkToolSearchCancel.load(std::memory_order_acquire)) {
            ImGui::TextDisabled("%s", L(Relink_SearchStopping));
        } else {
            ImGui::TextDisabled(L(Relink_SearchingFmt),
                static_cast<unsigned long long>(scanned));
            ImGui::SameLine();
            if (ImGui::SmallButton(L(Relink_CancelSearch))) {
                RequestRelinkSearchCancel(state.relinkToolSearchCancel);
            }
        }
    } else if (searchReport.status == Database::RelinkSearchStatus::Completed) {
        if (state.relinkTool.results.empty()) {
            ImGui::TextDisabled(L(Relink_NoMissingRecordsFmt),
                static_cast<unsigned long long>(searchReport.scannedEntries));
        } else if (notFound == static_cast<int>(state.relinkTool.results.size())) {
            ImGui::TextDisabled(L(Relink_NoCandidatesFmt),
                static_cast<unsigned long long>(searchReport.scannedEntries));
        } else {
            ImGui::TextDisabled(L(Relink_SearchCompletedFmt),
                static_cast<unsigned long long>(searchReport.scannedEntries));
        }
    } else if (searchReport.status == Database::RelinkSearchStatus::Cancelled) {
        ImGui::TextDisabled(L(Relink_SearchCancelledFmt),
            static_cast<unsigned long long>(searchReport.scannedEntries));
    } else if (searchReport.status == Database::RelinkSearchStatus::PartialFailure) {
        ImGui::TextColored(tcs.meterWarning, L(Relink_SearchPartialFailureFmt),
            static_cast<unsigned long long>(searchReport.scannedEntries),
            searchReport.failedRoots.size());
        ImGui::SameLine();
        if (ImGui::SmallButton(state.relinkTool.failedRootsExpanded
                ? L(Relink_HideFailedRoots) : L(Relink_ShowFailedRoots))) {
            state.relinkTool.failedRootsExpanded =
                !state.relinkTool.failedRootsExpanded;
        }
        if (state.relinkTool.failedRootsExpanded) {
            for (const auto& root : searchReport.failedRoots) {
                ImGui::TextWrapped("%s", root.c_str());
            }
        }
    } else if (searchReport.status == Database::RelinkSearchStatus::Failed) {
        ImGui::TextColored(tcs.meterWarning, L(Relink_SearchFailedFmt),
            searchReport.errorMessage.empty()
                ? L(Relink_SearchFailedFallback)
                : searchReport.errorMessage.c_str());
    }

    if (!state.relinkTool.results.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(tcs.controlAccent, L(Relink_ReadyCount), canLink);
        ImGui::SameLine();
        ImGui::TextColored(tcs.meterWarning, L(Relink_NeedSelect), needChoice);
        ImGui::SameLine();
        ImGui::TextColored(tcs.resultsSecondaryText, L(Relink_NotFound), notFound);
        ImGui::Spacing();

        if (ImGui::BeginTable(
                "##RelinkResults",
                2,
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp,
                ImVec2(contentWidth, 220.0f))) {
            ImGui::TableSetupColumn(L(Relink_MissingFiles), ImGuiTableColumnFlags_WidthStretch, 0.85f);
            ImGui::TableSetupColumn(L(Relink_MatchResults), ImGuiTableColumnFlags_WidthStretch, 1.15f);
            ImGui::TableHeadersRow();
            for (auto& [candidate, paths] : state.relinkTool.results) {
                ImGui::PushID(candidate.id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const ImVec4 color = paths.size() == 1 ? tcs.controlAccent
                    : paths.size() > 1 ? tcs.meterWarning
                                       : tcs.resultsSecondaryText;
                ImGui::TextColored(color, "%s", candidate.fileName.c_str());
                if (HoveredForHelp()) {
                    HarkenSearch::SetTooltip("%s", candidate.dbPath.c_str());
                }
                ImGui::TextDisabled("%s", candidate.dbPath.c_str());

                ImGui::TableSetColumnIndex(1);
                if (paths.size() == 1) {
                    ImGui::TextWrapped("%s", paths[0].path.c_str());
                    ImGui::TextDisabled("%s", RelinkMatchReasonText(paths[0].reason));
                } else if (paths.size() > 1) {
                    std::vector<const char*> items;
                    items.reserve(paths.size());
                    for (const auto& path : paths) items.push_back(path.path.c_str());
                    auto [selection, inserted] =
                        state.relinkTool.selectedByOrphanId.try_emplace(candidate.id, -1);
                    (void)inserted;
                    int& selected = selection->second;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    ImGui::Combo(
                        "##rlkSel",
                        &selected,
                        items.data(),
                        static_cast<int>(items.size()));
                    const auto reasonIndex = selected >= 0 && selected < static_cast<int>(paths.size())
                        ? selected : 0;
                    ImGui::TextDisabled("%s",
                        RelinkMatchReasonText(paths[reasonIndex].reason));
                } else {
                    ImGui::TextDisabled("%s", L(Relink_NoMatchForFile));
                }
                const auto failure = state.relinkTool.updateFailures.find(candidate.id);
                if (failure != state.relinkTool.updateFailures.end()) {
                    ImGui::TextColored(tcs.meterWarning, "%s",
                        RelinkPathUpdateFailureText(failure->second));
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        const float confirmButtonWidth = 130.0f;
        const float confirmButtonX = contentRight - confirmButtonWidth;
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", L(Relink_PreserveNote));
        ImGui::PopTextWrapPos();
        const int linkCount = canLink + selectedChoices;
        char confirmLabel[64];
        std::snprintf(confirmLabel, sizeof(confirmLabel), L(Relink_ConfirmBtn), linkCount);
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x <= confirmButtonX) {
            ImGui::SameLine();
        }
        // Screen coordinates avoid counting the settings group's offset twice.
        ImGui::SetCursorScreenPos(ImVec2(confirmButtonX, ImGui::GetCursorScreenPos().y));
        ImGui::BeginDisabled(linkCount == 0);
        const bool confirm = aura::ui::ActionButton(
            confirmLabel, ImVec2(confirmButtonWidth, 0),
            aura::ui::ButtonVariant::PrimarySearch);
        ImGui::EndDisabled();
        if (confirm) {
            decltype(state.relinkTool.results) pendingResults;
            state.relinkTool.updateFailures.clear();
            for (auto& [candidate, paths] : state.relinkTool.results) {
                if (paths.empty()) continue;
                int selected = 0;
                if (paths.size() > 1) {
                    const auto it =
                        state.relinkTool.selectedByOrphanId.find(candidate.id);
                    if (it != state.relinkTool.selectedByOrphanId.end()) {
                        selected = it->second;
                    } else {
                        selected = -1;
                    }
                }
                if (selected < 0 || selected >= static_cast<int>(paths.size())) {
                    pendingResults.push_back({candidate, paths});
                    continue;
                }
                if (ApplyRelinkPathUpdate(
                        state,
                        candidate.dbPath,
                        paths[selected].path)) {
                    refreshSearch = true;
                } else {
                    state.relinkTool.updateFailures[candidate.id] =
                        state.relinkPathUpdateStatus;
                    pendingResults.push_back({candidate, paths});
                }
            }
            state.relinkTool.results = std::move(pendingResults);
            state.relinkTool.confirmed = state.relinkTool.results.empty();
        }
    }

    if (state.relinkTool.confirmed && state.relinkTool.results.empty()) {
        ImGui::TextColored(tcs.controlAccent, "%s", L(Relink_Complete));
    }

    return refreshSearch;
}

} // namespace aura::ui
