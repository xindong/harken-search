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

static void RequestRelinkSearchCancel(AppState& state) {
    state.relinkSearchCancel.store(true, std::memory_order_release);
}

static void ArmRelinkSearch(AppState& state) {
    state.relinkSearchCancel.store(false, std::memory_order_release);
}

static bool ApplyRelinkPathUpdate(
    AppState& state,
    const std::string& oldPath,
    const std::string& newPath) {
    if (state.database.updateFilePath(
            oldPath,
            newPath,
            PathToUtf8(Utf8ToPath(newPath).filename()))) {
        return true;
    }
    state.loadErrorToast = u8"重新关联失败：数据库更新未完成，请重试。";
    state.loadErrorToastUntil = ImGui::GetTime() + 6.0;
    return false;
}

}  // namespace

void EvaluateRelinkForRecord(AppState& state, const AudioRecord& record) {
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
    state.relinkBanner.selectedCandidate = 0;
    state.relinkBannerSearchDone.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
        state.pendingRelinkCandidates.clear();
        state.pendingRelinkBannerGeneration = 0;
    }

    const std::string mp = record.filePath;
    RequestRelinkSearchCancel(state);
    RetireRelinkThread(state, state.relinkBannerSearchThread);
    DrainRetiredRelinkThreads(state);
    ArmRelinkSearch(state);
    if (state.relinkBannerSearchFn) {
        state.relinkBannerSearchThread = std::thread(state.relinkBannerSearchFn, std::ref(state), mp, generation);
    } else {
        state.relinkBannerSearchThread = std::thread([&state, mp, generation]() {
            auto roots = state.database.getWatchedRoots();
            auto allResults = state.database.findRelinkCandidates(roots, &state.relinkSearchCancel);
            std::vector<std::string> cands;
            for (auto& [cand, paths] : allResults) {
                if (cand.dbPath == mp) {
                    cands = paths;
                    break;
                }
            }
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
            state.pendingRelinkBannerGeneration = generation;
            state.relinkBannerSearchDone.store(true, std::memory_order_release);
        });
    }
}

void PumpRelinkWorkflow(AppState& state) {
    if (state.relinkBannerSearchDone.exchange(false)) {
        std::vector<std::string> candidates;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
            generation = state.pendingRelinkBannerGeneration;
            candidates = std::move(state.pendingRelinkCandidates);
            state.pendingRelinkBannerGeneration = 0;
        }
        if (generation == state.relinkBannerSearchGeneration.load(
                std::memory_order_acquire)) {
            state.relinkBanner.searching = false;
            state.relinkBanner.candidates = std::move(candidates);
        }
    }
    if (state.relinkToolSearchDone.exchange(
            false, std::memory_order_acquire)) {
        decltype(state.relinkTool.results) results;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
            generation = state.pendingRelinkToolGeneration;
            results = std::move(state.pendingRelinkToolResults);
            state.pendingRelinkToolGeneration = 0;
        }
        if (generation == state.relinkToolSearchGeneration.load(
                std::memory_order_acquire)) {
            state.relinkTool.results = std::move(results);
            state.relinkTool.scanning = false;
        }
    }
}

void ShutdownRelinkWorkflow(AppState& state) {
    state.relinkBannerSearchGeneration.fetch_add(1, std::memory_order_acq_rel);
    state.relinkToolSearchGeneration.fetch_add(1, std::memory_order_acq_rel);
    state.relinkBannerSearchDone.store(false, std::memory_order_release);
    state.relinkToolSearchDone.store(false, std::memory_order_release);
    state.relinkSearchCancel.store(true, std::memory_order_release);

    RetireRelinkThread(state, state.relinkBannerSearchThread);
    RetireRelinkThread(state, state.relinkToolSearchThread);
    DrainRetiredRelinkThreads(state);
    {
        std::lock_guard<std::mutex> lock(state.relinkWorkflowResultMutex);
        state.pendingRelinkCandidates.clear();
        state.pendingRelinkToolResults.clear();
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
            state.relinkBanner.visible = false;
            state.showSettingsDialog = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();

        ButtonOptions quiet;
        quiet.variant = ButtonVariant::Quiet;
        quiet.size = ButtonSize::Sm;
        if (Button(L(Relink_SkipThisBtn), quiet)) {
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
    ImGui::SetNextItemWidth(
        ImGui::GetContentRegionAvail().x -
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

        RequestRelinkSearchCancel(state);
        RetireRelinkThread(state, state.relinkToolSearchThread);
        DrainRetiredRelinkThreads(state);
        ArmRelinkSearch(state);
        if (state.relinkToolSearchFn) {
            state.relinkToolSearchThread = std::thread(state.relinkToolSearchFn, std::ref(state), roots, generation);
        } else {
            state.relinkToolSearchThread = std::thread(
                [&state, roots, generation]() mutable {
                auto results = state.database.findRelinkCandidates(roots, &state.relinkSearchCancel);
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
                state.pendingRelinkToolGeneration = generation;
                state.relinkToolSearchDone.store(true, std::memory_order_release);
            });
        }
    }
    if (wasScanning) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", L(Scan_Scanning));
    }

    ImGui::TextDisabled("%s", L(Relink_ScopeHint));

    if (!state.relinkTool.results.empty()) {
        int canLink = 0;
        int needChoice = 0;
        int selectedChoices = 0;
        int notFound = 0;
        for (auto& [candidate, paths] : state.relinkTool.results) {
            if (paths.size() == 1) ++canLink;
            else if (paths.size() > 1) {
                ++needChoice;
                const auto selected =
                    state.relinkTool.selectedByOrphanId.find(candidate.id);
                if (selected != state.relinkTool.selectedByOrphanId.end() &&
                    selected->second >= 0 &&
                    selected->second < static_cast<int>(paths.size())) {
                    ++selectedChoices;
                }
            }
            else ++notFound;
        }

        ImGui::Spacing();
        ImGui::TextColored(tcs.controlAccent, L(Relink_ReadyCount), canLink);
        ImGui::SameLine();
        ImGui::TextColored(tcs.meterWarning, L(Relink_NeedSelect), needChoice);
        ImGui::SameLine();
        ImGui::TextColored(tcs.resultsSecondaryText, L(Relink_NotFound), notFound);
        ImGui::Spacing();

        const float available = ImGui::GetContentRegionAvail().x;
        const float halfWidth =
            (available - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const float listHeight = 220.0f;

        ImGui::BeginChild("##rlkLeft", ImVec2(halfWidth, listHeight), true);
        ImGui::TextDisabled("%s", L(Relink_MissingFiles));
        ImGui::Separator();
        for (auto& [candidate, paths] : state.relinkTool.results) {
            const ImVec4 color = paths.size() == 1 ? tcs.controlAccent
                : paths.size() > 1 ? tcs.meterWarning
                                   : tcs.resultsSecondaryText;
            ImGui::TextColored(color, "%s", candidate.fileName.c_str());
            if (HoveredForHelp()) {
                HarkenSearch::SetTooltip("%s", candidate.dbPath.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##rlkRight", ImVec2(halfWidth, listHeight), true);
        ImGui::TextDisabled("%s", L(Relink_MatchResults));
        ImGui::Separator();
        for (auto& [candidate, paths] : state.relinkTool.results) {
            ImGui::PushID(candidate.id);
            if (paths.size() == 1) {
                const auto fileName = Utf8ToPath(paths[0]).filename();
                ImGui::TextColored(
                    ImVec4(0.4f, 0.85f, 0.4f, 1.0f),
                    L(Relink_ArrowFileFmt),
                    PathToUtf8(fileName).c_str());
                if (HoveredForHelp()) {
                    HarkenSearch::SetTooltip("%s", paths[0].c_str());
                }
            } else if (paths.size() > 1) {
                std::vector<const char*> items;
                items.reserve(paths.size());
                for (auto& path : paths) items.push_back(path.c_str());
                auto [selection, inserted] =
                    state.relinkTool.selectedByOrphanId.try_emplace(candidate.id, -1);
                (void)inserted;
                int& selected = selection->second;
                ImGui::SetNextItemWidth(halfWidth - 16.0f);
                ImGui::Combo(
                    "##rlkSel",
                    &selected,
                    items.data(),
                    static_cast<int>(items.size()));
            } else {
                ImGui::TextDisabled(u8"—");
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::TextDisabled("%s", L(Relink_PreserveNote));
        const int linkCount = canLink + selectedChoices;
        char confirmLabel[64];
        std::snprintf(confirmLabel, sizeof(confirmLabel), L(Relink_ConfirmBtn), linkCount);
        const float confirmButtonWidth = 130.0f;
        ImGui::SameLine(
            ImGui::GetContentRegionAvail().x -
            confirmButtonWidth + ImGui::GetCursorPosX());
        if (aura::ui::ActionButton(confirmLabel, ImVec2(confirmButtonWidth, 0), aura::ui::ButtonVariant::PrimarySearch)) {
            decltype(state.relinkTool.results) pendingResults;
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
                        paths[selected])) {
                    refreshSearch = true;
                } else {
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
