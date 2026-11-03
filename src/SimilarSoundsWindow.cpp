#include "SimilarSoundsWindow.h"

#include "AppState.h"
#include "HarkenSearchWidgets.h"
#include "HarkenSearchMetrics.h"
#include "DockNativeCaptionBridge.h"
#include "imgui_internal.h"
#include "Localization.h"
#include "SimilarityQueryV2.h"
#include "Utils.h"
#include "Theme.h"
#include "ThemeInspector.h"
#include "UIRenderer.h"
#include "ui/TransportContrast.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace aura::ui {

namespace {
constexpr float kContentPadding = 12.0f;
constexpr float kItemGap = 8.0f;

ImVec4 SimilarSecondaryText(const AppState& state) {
    const auto& theme = ThemeColors::current();
    if (state.settings.theme == AppTheme::Custom ||
        state.settings.useCustomColors || ThemeColors::customized()) {
        return theme.resultsSecondaryText;
    }
    return TransportContrast::ForSurface(
        theme.resultsSecondaryText, theme.metadataPanelBG, 6.0);
}
}

static std::optional<HarkenSearch::SimilarityReference> ResolveCurrentSimilarityReference(AppState& state) {
    auto fromRecord = [](const AudioRecord& record)
        -> std::optional<HarkenSearch::SimilarityReference> {
        if (record.id <= 0 || record.filePath.empty()) return std::nullopt;
        HarkenSearch::SimilarityReference reference;
        reference.fileId = record.id;
        reference.filePath = record.filePath;
        reference.displayName = !record.displayName.empty()
            ? record.displayName
            : (!record.fileName.empty() ? record.fileName : record.filePath);
        return reference;
    };

    if (state.selectedResultIndex >= 0 &&
        state.selectedResultIndex < static_cast<int>(state.searchResults.size())) {
        if (auto reference = fromRecord(state.searchResults[state.selectedResultIndex])) {
            return reference;
        }
    }
    if (state.currentFileId <= 0) return std::nullopt;
    for (const AudioRecord& record : state.searchResults) {
        if (record.id == state.currentFileId) return fromRecord(record);
    }
    return fromRecord(state.database.getFileById(state.currentFileId));
}

static bool DrawSimilarSoundsActionRow(
    AppState& state,
    HarkenSearch::SimilarityWorkflow& workflow,
    const HarkenSearch::SimilarityWorkflowSnapshot& snapshot) {
    const bool searching = snapshot.status == HarkenSearch::SimilarityWorkflowStatus::Searching;
    const bool indexing = state.batchFeatureExtracting.load();
    const bool hasReference = snapshot.status != HarkenSearch::SimilarityWorkflowStatus::NoSelection;
    const char* actionLabel = indexing ? L(Similar_Indexing) : (searching ? L(Similar_Searching) :
        (snapshot.batchNumber > 0 ? L(Similar_NextBatch) : L(Similar_Find)));
    const float height = std::max(ImGui::GetFrameHeight(), aura::ui::metrics::kHeightMd);
    const float available = ImGui::GetContentRegionAvail().x;
    constexpr float comboWidth = 132.0f;
    const float deepWidth = ImGui::CalcTextSize(L(Similar_DeepThink)).x + 52.0f;
    const float actionWidth = ImGui::CalcTextSize(actionLabel).x + 28.0f;
    const bool stacked = available < comboWidth + deepWidth + actionWidth + 2*kItemGap;
    const float widths[] = {deepWidth, comboWidth, actionWidth};
    ImVec2 positions[3];
    float nextX = 0; int optionRow = 0;
    for (int i = 0; i < 3; ++i) {
        if (nextX > 0 && ((stacked && i == 2) || nextX + widths[i] > available)) {
            ++optionRow; nextX = 0;
        }
        positions[i] = ImVec2(nextX, (optionRow+1)*(height+kItemGap));
        nextX += widths[i] + kItemGap;
    }
    const float actionHeight = (optionRow+2)*height + (optionRow+1)*kItemGap;
    bool changed = false, searchRequested = false;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::current().metadataPanelBG);
    if (ImGui::BeginChild("##SimilarActionRow", ImVec2(0,actionHeight), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const float rowX = ImGui::GetCursorPosX(), rowY = ImGui::GetCursorPosY();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(L(Similar_CurrentAudio)); ImGui::SameLine();
        const float fileWidth = std::max(20.0f,ImGui::GetContentRegionAvail().x);
        const ImVec2 textMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##SimilarCurrentFile", ImVec2(fileWidth,height));
        const ImVec2 textMax(textMin.x+fileWidth,textMin.y+height);
        const char* currentName = L(Similar_NoSelection);
        if (!snapshot.reference.displayName.empty()) currentName = snapshot.reference.displayName.c_str();
        ImVec2 currentNameSize = ImGui::CalcTextSize(currentName);
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(textMin.x,textMin.y+ImGui::GetStyle().FramePadding.y),
            textMax,textMax.x,currentName,nullptr,&currentNameSize);
        ImGui::SetCursorPos(ImVec2(rowX+positions[0].x,rowY+positions[0].y+(height-18)*0.5f));
        const ImVec2 deepMin = ImGui::GetCursorScreenPos();
        if (aura::ui::Toggle(L(Similar_DeepThink), &state.similarSoundsDeep)) changed = true;
        ThemeInspector::RegisterZone(deepMin, ImVec2(deepMin.x + 36, deepMin.y + 18), "similar.options.deep");
        if (ImGui::IsItemHovered()) aura::ui::Tooltip(L(Similar_DeepHint));
        ImGui::SetCursorPos(ImVec2(rowX+positions[1].x,rowY+positions[1].y));
        const char* presets[] = {L(Similar_PresetBalanced),L(Similar_PresetTimbre),L(Similar_PresetRhythm),L(Similar_PresetPitch),L(Similar_PresetSpectral)};
        int preset = static_cast<int>(state.similarSoundsPreset);
        ImGui::SetNextItemWidth(comboWidth);
        if (aura::ui::Combo("##SimilarPreset", &preset, presets, IM_ARRAYSIZE(presets), aura::ui::ComboVariant::Flat)) {
            state.similarSoundsPreset = static_cast<SimilarityPreset>(preset); changed = true;
        }
        ThemeInspector::RegisterLastItem("similar.options.preset");
        if (changed) {
            workflow.Invalidate(); state.similarSoundsSelectedRow = -1;
            state.settingsDirty = true; state.settingsDirtyAt = ImGui::GetTime();
        }
        ImGui::SetCursorPos(ImVec2(rowX+positions[2].x,rowY+positions[2].y));
        aura::ui::ButtonOptions options;
        options.variant = aura::ui::ButtonVariant::PrimarySearch;
        options.disabled = indexing || !hasReference;
        if (searching && !changed) {
            if (aura::ui::ActionButton(L(Cancel), ImVec2(0,height), aura::ui::ButtonVariant::Quiet)) {
                workflow.CancelSearch(); state.similarSoundsSelectedRow = -1; changed = true;
            }
        } else if (aura::ui::Button(actionLabel, options)) {
            state.similarSoundsSelectedRow = -1;
            HarkenSearch::SimilarityOptions searchOptions;
            searchOptions.mode = HarkenSearch::SimilarityMode::Substitute; searchOptions.deep = state.similarSoundsDeep; searchOptions.preset = state.similarSoundsPreset;
            searchRequested = workflow.RequestSearch(searchOptions, 50);
        }
    }
    ImGui::EndChild(); ImGui::PopStyleColor();
    return changed || searchRequested;
}

static const char* SimilarSoundsBodyMessage(
    const HarkenSearch::SimilarityWorkflowSnapshot& snapshot,
    bool indexing) {
    if (indexing) return L(Similar_Indexing);
    if (snapshot.status == HarkenSearch::SimilarityWorkflowStatus::NoSelection) {
        return L(Similar_NoSelection);
    }
    if (snapshot.status == HarkenSearch::SimilarityWorkflowStatus::Error) {
        if (snapshot.error == HarkenSearch::SimilarityWorkflowError::DatabaseUnavailable) {
            return L(Similar_DatabaseError);
        }
        if (snapshot.error == HarkenSearch::SimilarityWorkflowError::ReferenceUnreadable) {
            return L(Similar_ReadError);
        }
        if (snapshot.error == HarkenSearch::SimilarityWorkflowError::ModelUnavailable) return L(Similar_ModelUnavailable);
        if (snapshot.error == HarkenSearch::SimilarityWorkflowError::IndexRequired) return L(Similar_IndexRequired);
        if (snapshot.error == HarkenSearch::SimilarityWorkflowError::SearchFailed) return L(Similar_SearchFailed);
        if (snapshot.error == HarkenSearch::SimilarityWorkflowError::Cancelled) return L(Similar_Cancelled);
        if (snapshot.error == HarkenSearch::SimilarityWorkflowError::NoMoreCandidates) return L(Similar_NoMoreCandidates);
        return L(Similar_NoResults);
    }
    if (snapshot.status != HarkenSearch::SimilarityWorkflowStatus::Searching &&
        snapshot.rows.empty()) {
        return L(Similar_NoResults);
    }
    return nullptr;
}

static void ToggleSimilarSoundsAudition(
    AppState& state,
    const HarkenSearch::SimilarityDisplayRow& row) {
    const bool loadedThis = state.audioEngine.isLoaded() &&
        state.audioEngine.getLoadedFilePath() == row.filePath;
    if (!loadedThis) {
        state.audioEngine.loadFileAsync(row.filePath, true);
        ScheduleDeferredWaveformDetail(state, row.filePath, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
    } else if (state.audioEngine.isPlaying()) {
        state.audioEngine.pause();
    } else {
        if (state.audioEngine.getPlaybackPosition() >= 0.99f) {
            state.audioEngine.seekToPosition(0.0f);
        }
        state.audioEngine.play();
    }
    MarkFilePlayed(state, row.filePath, row.fileId, true);
}

static void DrawSimilarSoundsResultsTable(
    AppState& state,
    const HarkenSearch::SimilarityWorkflowSnapshot& snapshot,
    bool windowFocused) {
    const ThemeColorScheme& theme = ThemeColors::current();
    const bool sharedPanelStyle = state.settings.theme != AppTheme::Custom &&
        !state.settings.useCustomColors && !ThemeColors::customized();
    const ImVec4 infoSurface = theme.metadataPanelBG;
    const ImVec4 tableBody = sharedPanelStyle ? infoSurface : theme.resultsBG;
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,
        sharedPanelStyle ? ImLerp(infoSurface, theme.toolbarBG, 0.14f) : theme.resultsHeaderBG);
    ImGui::PushStyleColor(ImGuiCol_TableRowBg, tableBody);
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt,
        ImLerp(tableBody, sharedPanelStyle ? theme.resultsBG : theme.resultsHeaderBG, 0.32f));
    if (sharedPanelStyle) {
        ImVec4 edge = ThemeColors::infoPanelDockEdge(theme);
        edge.w = 0.45f;
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, edge);
        edge.w = 0.18f;
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, edge);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, tableBody);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(kItemGap, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    }
    const ImGuiTableFlags flags = (sharedPanelStyle ? ImGuiTableFlags_None : ImGuiTableFlags_BordersOuter) |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const float similarRowHeight = std::max(
        24.0f, static_cast<float>(state.settings.rowHeight));
    if (ImGui::BeginTable("##SimilarResultsTable", 5, flags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn(L(Similar_ColumnFileName),
            ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableSetupColumn(L(Similar_ColumnWaveform),
            ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn(L(Similar_ColumnDuration),
            ImGuiTableColumnFlags_WidthFixed,
            sharedPanelStyle ? ImGui::CalcTextSize("00:00:00.00").x : 84.0f);
        ImGui::TableSetupColumn(L(Similar_ColumnFormat),
            ImGuiTableColumnFlags_WidthFixed, sharedPanelStyle ? std::max(
                ImGui::CalcTextSize(L(Similar_ColumnFormat)).x, ImGui::CalcTextSize("FLAC").x) : 76.0f);
        ImGui::TableSetupColumn(L(Similar_ColumnScore),
            ImGuiTableColumnFlags_WidthFixed, sharedPanelStyle ? std::max(
                ImGui::CalcTextSize(L(Similar_ColumnScore)).x, ImGui::CalcTextSize("100.0").x) : 88.0f);
        const LStr headers[] = {
            LStr::Similar_ColumnFileName, LStr::Similar_ColumnWaveform,
            LStr::Similar_ColumnDuration, LStr::Similar_ColumnFormat,
            LStr::Similar_ColumnScore
        };
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int column = 0; column < IM_ARRAYSIZE(headers); ++column) {
            ImGui::TableSetColumnIndex(column);
            ImGui::TableHeader(GetLocalizedString(headers[column]));
            if (column == 4 && ImGui::IsItemHovered()) {
                aura::ui::Tooltip(L(Similar_ScoreTooltip));
            }
        }

        if (const char* message = SimilarSoundsBodyMessage(
                snapshot, state.batchFeatureExtracting.load())) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, SimilarSecondaryText(state));
            ImGui::TextUnformatted(message);
            ImGui::PopStyleColor();
        }
        for (int index = 0; index < static_cast<int>(snapshot.rows.size()); ++index) {
            const HarkenSearch::SimilarityDisplayRow& row = snapshot.rows[index];
            ImGui::PushID(row.filePath.c_str());
            ImGui::TableNextRow(ImGuiTableRowFlags_None, similarRowHeight);
            ImGui::TableSetColumnIndex(0);
            const int previousSelectedRow = state.similarSoundsSelectedRow;
            if (ImGui::Selectable(row.fileName.c_str(),
                    state.similarSoundsSelectedRow == index,
                    ImGuiSelectableFlags_SpanAllColumns |
                        ImGuiSelectableFlags_AllowDoubleClick |
                        ImGuiSelectableFlags_AllowOverlap,
                    ImVec2(0.0f, similarRowHeight))) {
                const bool waveColumnHovered =
                    (ImGui::TableGetColumnFlags(1) & ImGuiTableColumnFlags_IsHovered) != 0;
                const bool wasPlaying = state.audioEngine.isPlaying();
                const bool selectionChanged = previousSelectedRow != index;
                state.similarSoundsSelectedRow = index;
                if (!waveColumnHovered) {
                    state.uiState.pendingMiniWaveSeekPos = -1.0f;
                    const bool loadedThis = state.audioEngine.isLoaded() &&
                        state.audioEngine.getLoadedFilePath() == row.filePath;
                    const PlaybackAction action = GetPlaybackAction(state.settings,
                        wasPlaying, selectionChanged,
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                    if (action == PlaybackAction::Pause) {
                        state.audioEngine.pause();
                    } else if (action == PlaybackAction::Play) {
                        if (!loadedThis || state.audioEngine.isLoadFilePending()) {
                            state.audioEngine.loadFileAsync(row.filePath, true);
                            ScheduleDeferredWaveformDetail(
                                state, row.filePath, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
                        } else {
                            if (state.audioEngine.getPlaybackPosition() >= 0.99f) {
                                state.audioEngine.seekToPosition(0.0f);
                            }
                            state.audioEngine.play();
                        }
                        MarkFilePlayed(state, row.filePath, row.fileId, true);
                    }
                }
            }
            ImGui::TableSetColumnIndex(1);
            const bool loadedThis = state.audioEngine.isLoaded() &&
                state.audioEngine.getLoadedFilePath() == row.filePath;
            const float playPos = loadedThis
                ? GetSmoothedPlaybackPosition(state) : -1.0f;
            const float waveWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            const float waveHeight = similarRowHeight;
            const ImVec2 waveCellMin = ImGui::GetCursorScreenPos();
            const auto cacheIt = state.waveformCache.find(row.filePath);
            if (cacheIt == state.waveformCache.end()) {
                state.visibleFilePaths.insert(row.filePath);
                state.addPendingWaveform(row.filePath, true);
                ImGui::Dummy(ImVec2(waveWidth, waveHeight));
            } else {
                const WaveformData& cacheEntry = cacheIt->second;
                DrawMiniWaveform(cacheEntry, playPos, waveWidth, waveHeight);
            }
            ImGui::SetCursorScreenPos(waveCellMin);
            ImGui::InvisibleButton("##SimilarWaveHit", ImVec2(waveWidth, waveHeight));
            if (ThemeInspector::IsCollecting())
                ThemeInspector::RegisterLastItem(("similar.result.wave." + std::to_string(index)).c_str());
            const bool waveClicked = ImGui::IsItemClicked();
            if (waveClicked) state.similarSoundsSelectedRow = index;
            if (ImGui::IsItemActive() || ImGui::IsItemClicked()) {
                const float seekPos = std::clamp(
                    (ImGui::GetMousePos().x - waveCellMin.x) / waveWidth, 0.0f, 1.0f);
                if (!loadedThis) {
                    if (ImGui::IsItemClicked()) {
                        state.uiState.pendingMiniWaveSeekPos = -1.0f;
                        state.uiState.miniWaveSeekApplied = true;
                        state.uiState.miniWaveSeekTargetPath = row.filePath;
                        state.uiState.lastMiniWaveSeekPos = seekPos;
                        state.audioEngine.loadFileAsync(row.filePath, true);
                        ScheduleDeferredWaveformDetail(
                            state, row.filePath, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
                        MarkFilePlayed(state, row.filePath, row.fileId, true);
                        state.audioEngine.seekToPosition(seekPos);
                    }
                } else if (ImGui::IsItemClicked()) {
                    state.uiState.pendingMiniWaveSeekPos = -1.0f;
                    state.uiState.miniWaveSeekApplied = true;
                    state.uiState.miniWaveSeekTargetPath.clear();
                    state.uiState.lastMiniWaveSeekPos = seekPos;
                    state.audioEngine.seekToPosition(seekPos);
                    if (!state.audioEngine.isPlaying()) state.audioEngine.play();
                } else if (std::abs(seekPos - state.uiState.lastMiniWaveSeekPos) > 0.002f) {
                    state.uiState.lastMiniWaveSeekPos = seekPos;
                    state.audioEngine.seekToPosition(seekPos);
                }
            }
            const auto alignRowText = [&]() {
                if (sharedPanelStyle) ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                    std::max(0.0f, (similarRowHeight - ImGui::GetTextLineHeight()) * 0.5f));
            };
            ImGui::TableSetColumnIndex(2);
            alignRowText();
            ImGui::TextUnformatted(FormatTime(row.durationSeconds).c_str());
            ImGui::TableSetColumnIndex(3);
            alignRowText();
            ImGui::TextUnformatted(row.format.empty() ? "-" : row.format.c_str());
            ImGui::TableSetColumnIndex(4);
            alignRowText();
            const int score = static_cast<int>(
                std::clamp(row.similarity, 0.0f, 1.0f) * 100.0f + 0.5f);
            ImGui::Text("%d", score);
            if (ImGui::IsItemHovered() && row.breakdown.valid) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(L(Similar_ScoreTooltip));
                const char* dimensions[] = {L(Similar_DimCategory),L(Similar_DimTimbre),L(Similar_DimEnvelope),L(Similar_DimRhythm),L(Similar_DimPitch),L(Similar_DimDuration)};
                for (size_t dimension = 0; dimension < 6; ++dimension) {
                    if (row.breakdown.weights[dimension] > 0) {
                        ImGui::Text("%s  %.0f / 100  (%.0f%%)",dimensions[dimension],100*row.breakdown.parts[dimension],100*row.breakdown.weights[dimension]);
                    }
                }
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (windowFocused && !ImGui::GetIO().WantTextInput &&
        state.similarSoundsSelectedRow >= 0 &&
        state.similarSoundsSelectedRow < static_cast<int>(snapshot.rows.size()) &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        const HarkenSearch::SimilarityDisplayRow& row =
            snapshot.rows[state.similarSoundsSelectedRow];
        // ProcessKeybindings runs before this standalone window. Let its
        // global transport toggle an already-loaded row only once.
        const bool loadedThis = state.audioEngine.isLoaded() &&
            state.audioEngine.getLoadedFilePath() == row.filePath;
        if (!loadedThis) {
            ToggleSimilarSoundsAudition(state, row);
        }
    }
    if (sharedPanelStyle) {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }
    ImGui::PopStyleColor(3);
}

void OpenSimilarSoundsWindow(AppState& state) {
    if (!state.panelSimilarSounds.visible) {
        state.batchFeatureCompletionNotice.store(false);
    }
    if (!state.similarSoundsWorkflow) {
        state.similarSoundsWorkflow =
            std::make_unique<HarkenSearch::SimilarityWorkflow>(
                HarkenSearch::MakeSimilarityQueryV2Handler(GetExeDirUtf8() + "/models/clap"));
    }
    state.panelSimilarSounds.visible = true;
    state.panelSimilarSounds.floating = true;
    state.panelSimilarSounds.wasDragging = false;
    if (state.panelSimilarSounds.floatW < 220.0f) state.panelSimilarSounds.floatW = 640.0f;
    if (state.panelSimilarSounds.floatH < 180.0f) state.panelSimilarSounds.floatH = 560.0f;
    if (state.panelSimilarSounds.floatX < 80.0f) state.panelSimilarSounds.floatX = 420.0f;
    if (state.panelSimilarSounds.floatY < 40.0f) state.panelSimilarSounds.floatY = 120.0f;
    state.similarSoundsWindow.restorePos = ImVec2(state.panelSimilarSounds.floatX, state.panelSimilarSounds.floatY);
    state.similarSoundsWindow.restoreSize = ImVec2(state.panelSimilarSounds.floatW, state.panelSimilarSounds.floatH);
    aura::ui::OpenStandaloneWindow(&state.similarSoundsWindow);
    state.similarSoundsWindow.requestMove = true;
}

void InvalidateSimilarSoundsWindow(AppState& state) {
    if (!state.similarSoundsWorkflow) return;
    state.similarSoundsWorkflow->Invalidate();
    state.similarSoundsSelectedRow = -1;
    state.similarSoundsLocalReference.reset();
}

static void DrawSimilarAnalysisReport(AppState& state) {
    if (state.showSimilarAnalysisReport) {
        ImGui::OpenPopup(L(Similar_AnalysisReport));
        state.showSimilarAnalysisReport = false;
    }
    ImGui::SetNextWindowSize(ImVec2(660, 380), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::BeginPopupModal(L(Similar_AnalysisReport), &open)) {
        std::lock_guard<std::mutex> lock(state.batchFeatureResultMutex);
        const auto& report = state.batchFeatureResult;
        ImGui::Text(L(Similar_AnalysisSummaryFmt), report.progress.saved, report.progress.skipped, report.progress.failed);
        if (report.cancelled) ImGui::TextWrapped("%s", L(Similar_AnalysisCancelled));
        if (!report.modelError.empty()) ImGui::TextWrapped("%s", L(Similar_AnalysisModelMissing));
        if (!report.error.empty()) ImGui::TextWrapped("%s", L(Similar_AnalysisFailed));
        if (ImGui::BeginChild("##SimilarAnalysisFailures", ImVec2(0, -42), false)) {
            const char* reasons[] = {L(Similar_FailureUnreadable), L(Similar_FailureSilent), L(Similar_FailureChanged), L(Similar_FailureModel), L(Similar_FailureSave)};
            ImGuiListClipper clipper; clipper.Begin(static_cast<int>(report.issues.size()), ImGui::GetTextLineHeightWithSpacing() * 2);
            while (clipper.Step()) for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const auto& issue = report.issues[index];
                ImGui::Text("%s: %s", reasons[static_cast<int>(issue.reason)], issue.file.name.c_str());
                ImGui::TextUnformatted(issue.file.path.c_str());
            }
        }
        ImGui::EndChild();
        if (aura::ui::ActionButton(L(Close), ImVec2(100, 28), aura::ui::ButtonVariant::Quiet)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void DrawSimilarSoundsBody(AppState& state, bool windowFocused) {
    HarkenSearch::SimilarityWorkflow& workflow = *state.similarSoundsWorkflow;
    HarkenSearch::SimilarityWorkflowSnapshot snapshot = workflow.Snapshot();
    const bool sharedPanelStyle = state.settings.theme != AppTheme::Custom &&
        !state.settings.useCustomColors && !ThemeColors::customized();
    if (sharedPanelStyle) {
        // The host owns the single perimeter. Inset the content inside it,
        // so controls and the scrollable table never sit on that line.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(kContentPadding, kContentPadding));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kItemGap, kItemGap));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ThemeColors::current().metadataPanelBG);
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColors::current().metadataPanelText);
        if (!ImGui::BeginChild("##SimilarContent", ImVec2(0.0f, 0.0f),
                ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
            return;
        }
    }
    if (DrawSimilarSoundsActionRow(state, workflow, snapshot)) {
        snapshot = workflow.Snapshot();
    }
    if (sharedPanelStyle) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
            ThemeColors::current().metadataPanelBG);
        if (ImGui::BeginChild("##SimilarIndexRegion", ImVec2(0.0f, 0.0f),
                ImGuiChildFlags_AutoResizeY,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            DrawSimilarityIndexPanel(state);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        DrawSimilarityIndexPanel(state);
    }
    // Starting analysis invalidates rows, even if the task finishes this frame.
    snapshot = workflow.Snapshot();
    bool headingVisible = true;
    if (sharedPanelStyle) {
        ImVec4 divider = ThemeColors::infoPanelDockEdge(ThemeColors::current());
        divider.w = 0.32f;
        ImGui::PushStyleColor(ImGuiCol_Separator, divider);
        ImGui::Separator();
        ImGui::PopStyleColor();
    } else {
        headingVisible = ImGui::BeginChild("##SimilarResultsHeading", ImVec2(0.0f, 36.0f), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    }
    if (headingVisible) {
        if (!sharedPanelStyle) {
            const float rowY = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(rowY + (36.0f - ImGui::GetTextLineHeight()) * 0.5f);
        }
        ImGui::TextUnformatted(L(Similar_Results));
        ImGui::SameLine();
        ImGui::PushStyleColor(
            ImGuiCol_Text, SimilarSecondaryText(state));
        ImGui::Text("%d", static_cast<int>(snapshot.rows.size()));
        if (snapshot.totalFiles > 0 && snapshot.indexed < snapshot.totalFiles) {
            ImGui::SameLine(); ImGui::Text(L(Similar_CoverageFmt),static_cast<int>(snapshot.indexed),static_cast<int>(snapshot.totalFiles));
        }
        ImGui::PopStyleColor();
    }
    if (!sharedPanelStyle) ImGui::EndChild();
    DrawSimilarSoundsResultsTable(state, snapshot, windowFocused);
    DrawSimilarAnalysisReport(state);
    if (sharedPanelStyle) {
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }
}

static void PollSimilarSoundsWorkflow(AppState& state) {
    auto mainReference = ResolveCurrentSimilarityReference(state);
    const int64_t referenceId = mainReference ? mainReference->fileId : -1;
    if (referenceId != state.similarSoundsObservedSelectionId) {
        state.similarSoundsObservedSelectionId = referenceId;
        state.similarSoundsSelectedRow = -1;
        state.similarSoundsLocalReference.reset();
    }
    auto reference = state.similarSoundsLocalReference ?
        state.similarSoundsLocalReference : std::move(mainReference);
    HarkenSearch::SimilarityWorkflow& workflow = *state.similarSoundsWorkflow;
    workflow.SetDatabasePath(state.database.getMainDbPath());
    workflow.SetReference(std::move(reference));
    const bool batchExtracting = state.batchFeatureExtracting.load();
    if (batchExtracting && !state.similarSoundsObservedBatchExtracting) {
        workflow.Invalidate();
        state.similarSoundsSelectedRow = -1;
    }
    state.similarSoundsObservedBatchExtracting = batchExtracting;
    workflow.Poll();
}

void RenderSimilarSoundsContent(AppState& state) {
    if (!state.similarSoundsWorkflow) {
        state.similarSoundsWorkflow =
            std::make_unique<HarkenSearch::SimilarityWorkflow>(
                HarkenSearch::MakeSimilarityQueryV2Handler(GetExeDirUtf8() + "/models/clap"));
    }
    PollSimilarSoundsWorkflow(state);
    DrawSimilarSoundsBody(state, ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
}

void RenderSimilarSoundsWindow(AppState& state) {
    if (!state.similarSoundsWorkflow) return;
    if (!state.panelSimilarSounds.floating) return;
    PollSimilarSoundsWorkflow(state);
    HarkenSearch::SimilarityWorkflow& workflow = *state.similarSoundsWorkflow;

    const bool windowWasOpen = state.similarSoundsWindow.open;
    if (!aura::ui::BeginStandaloneWindow(
            L(Similar_Title), "##SimilarSoundsWindow", &state.similarSoundsWindow)) {
        if (windowWasOpen && !state.similarSoundsWindow.open) {
            workflow.Invalidate();
            state.similarSoundsSelectedRow = -1;
            state.similarSoundsLocalReference.reset();
            state.panelSimilarSounds.visible = false;
        }
        return;
    }
    const bool windowFocused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    // Host RenderUI calls driveFloatingDockDrag("similar-sounds") for native caption
    // and inner-strip docking; this window only binds the extra viewport HWND.
    if (ImGuiViewport* vp = ImGui::GetWindowViewport()) {
        aura::ui::dock::BindNativeCaptionModule(vp->PlatformHandle, "similar-sounds");
        const bool nativeHeld = aura::ui::dock::NativeCaptionDragging() &&
            aura::ui::dock::NativeCaptionModuleId() == "similar-sounds";
        if (aura::ui::dock::ShouldRequestStandaloneMoveFromNativeDrag(
                nativeHeld, state.panelSimilarSounds.floating)) {
            const ImVec2 pos = ImGui::GetWindowPos();
            state.similarSoundsWindow.restorePos = pos;
            state.similarSoundsWindow.requestMove = true;
        }
    }
    const bool IsTitleBarHovered = ImGui::GetCurrentWindowRead() &&
        ImGui::GetCurrentWindowRead()->TitleBarRect().Contains(ImGui::GetMousePos());
    if (IsTitleBarHovered && ImGui::IsMouseDragging(0)) {
        state.panelSimilarSounds.wasDragging = true;
    }
    DrawSimilarSoundsBody(state, windowFocused);
    aura::ui::EndStandaloneWindow();
}

} // namespace aura::ui
