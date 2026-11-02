#include "PlaylistQuickBar.h"

#include "AppState.h"
#include "DiagnosticsLogger.h"
#include "PlaylistDisplayedFileCountPolicy.h"
#include "PlaylistQuickBarMotion.h"
#include "SidebarActivityBar.h"
#include "TransientScrollbarHost.h"
#include "Theme.h"

#include "imgui.h"
#include "HarkenSearchWidgets.h"

#include <algorithm>
#include <cstdio>

struct ReadingTextScope {
    bool prev;
    ReadingTextScope();
    ~ReadingTextScope();
};

bool EscClosePopup();
void RefreshSearchResults(AppState& state, bool resetPagination);
void SaveAppSettings(const AppState& state);
void LoadPlaylistFiltered(AppState& state, int64_t playlistId, bool resetFolderFilter = false);
void ActivatePlaylistView(
    AppState& state,
    int64_t playlistId,
    bool aggregatedView,
    bool resetFolderFilter = true,
    int64_t expandedFolderId = -1);
void ApplyPlaylistQuickBarOrder(AppState& state);
void PersistPlaylistQuickBarOrderFromCurrent(AppState& state);
void UpdatePlaylistQuickDragActivation();
void ClearPlaylistQuickDrag();
void DrawPlaylistQuickDragPreview();
void DrawPlaylistDropTargetHighlight(
    ImDrawList* dl,
    const ImVec2& p0,
    const ImVec2& p1,
    const ImVec4& accent,
    float rounding,
    bool expandCue);
bool IsPlaylistContentDragActive();
bool ShouldSuppressPlaylistContextPopup();
void SetPlaylistQuickBarVisible(AppState& state, bool visible);
bool IsPlaylistShownInQuickBar(const AppState& state, int64_t playlistId);
bool SetPlaylistShownInQuickBar(AppState& state, int64_t playlistId, bool visible);
int64_t CopyPlaylistFromUi(AppState& state, const Playlist& playlist);
bool OpenPlaylistRenameDialog(AppState& state, const Playlist& playlist);
void MaybePausePlaybackOnInScopeDragStart();
void AddPlaylistQuickDragToPlaylist(AppState& state, int64_t playlistId);
bool AddFileIdsToPlaylist(
    AppState& state,
    int64_t playlistId,
    const int64_t* ids,
    int count,
    const char* diagLabel);
bool AddFolderPathToPlaylist(
    AppState& state,
    int64_t playlistId,
    const char* folderPath,
    const char* diagLabel);
bool AddCoverTileMembershipToPlaylist(
    AppState& state,
    int64_t playlistId,
    const std::string& playlistName,
    const std::string& tileIdentityKey,
    const char* diagLabel);
bool MenuItemWithTooltip(
    const char* label,
    const char* shortcut,
    const char* tooltip,
    bool selected = false,
    bool enabled = true);

PlaylistQuickBarReorderAnimState g_playlistQuickBarReorderAnim;
PlaylistQuickDragState g_playlistQuickDrag;

PlaylistQuickBarReorderAnimState::Item& EnsurePlaylistQuickBarAnimItem(
    int64_t playlistId,
    float initialX)
{
    for (auto& entry : g_playlistQuickBarReorderAnim.items) {
        if (entry.playlistId == playlistId) return entry;
    }
    PlaylistQuickBarReorderAnimState::Item item;
    item.playlistId = playlistId;
    item.currentX = initialX;
    item.targetX = initialX;
    item.initialized = true;
    g_playlistQuickBarReorderAnim.items.push_back(item);
    return g_playlistQuickBarReorderAnim.items.back();
}

void PrunePlaylistQuickBarAnimItems(const std::vector<Playlist>& playlists) {
    g_playlistQuickBarReorderAnim.items.erase(
        std::remove_if(
            g_playlistQuickBarReorderAnim.items.begin(),
            g_playlistQuickBarReorderAnim.items.end(),
            [&](const PlaylistQuickBarReorderAnimState::Item& entry) {
                return std::none_of(
                    playlists.begin(),
                    playlists.end(),
                    [&](const Playlist& pl) { return pl.id == entry.playlistId; });
            }),
        g_playlistQuickBarReorderAnim.items.end());
}

void RenderPlaylistQuickBar(AppState& state, float height) {
    if (height <= 1.0f) return;

    UpdatePlaylistQuickDragActivation();

    auto uiWithAlpha = [](ImVec4 c, float a) -> ImVec4 { c.w = a; return c; };
    auto uiMix = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        const float u = 1.0f - t;
        return ImVec4(
            a.x * u + b.x * t,
            a.y * u + b.y * t,
            a.z * u + b.z * t,
            a.w * u + b.w * t);
    };

    const auto& tcs = ThemeColors::current();
    ApplyPlaylistQuickBarOrder(state);
    ImGui::SetCursorPosX(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 0.0f));
    BeginTransientScrollbarChild("PlaylistQuickBar", ImVec2(0.0f, height), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 barMin = ImGui::GetWindowPos();
        const ImVec2 barMax(barMin.x + ImGui::GetWindowSize().x, barMin.y + ImGui::GetWindowSize().y);
        dl->AddRectFilledMultiColor(
            barMin,
            barMax,
            ImGui::GetColorU32(uiWithAlpha(uiMix(tcs.resultsBG, tcs.mainWindowBG, 0.32f), 0.96f)),
            ImGui::GetColorU32(uiWithAlpha(uiMix(tcs.resultsBG, tcs.mainWindowBG, 0.18f), 0.96f)),
            ImGui::GetColorU32(uiWithAlpha(uiMix(tcs.resultsBG, tcs.mainWindowBG, 0.42f), 0.96f)),
            ImGui::GetColorU32(uiWithAlpha(uiMix(tcs.resultsBG, tcs.mainWindowBG, 0.28f), 0.96f)));
        dl->AddLine(barMin, ImVec2(barMax.x, barMin.y), ImGui::GetColorU32(uiWithAlpha(tcs.resultsOutline, 0.28f)), 1.0f);
        dl->AddLine(ImVec2(barMin.x, barMax.y - 1.0f), ImVec2(barMax.x, barMax.y - 1.0f),
                    ImGui::GetColorU32(uiWithAlpha(tcs.resultsOutline, 0.22f)), 1.0f);

        const float itemH = height - 8.0f;
        const float playlistItemW = 128.0f;
        const float addItemW = 42.0f;
        const float hideItemW = 0.0f;
        const float itemGap = 6.0f;
        const float itemStep = playlistItemW + itemGap;
        const float startX = barMin.x + 8.0f;
        const float startY = barMin.y + 4.0f;
        const ImVec4 accent = tcs.resultsSelection;
        const bool contentDragActive = IsPlaylistContentDragActive();
        const ImU32 iconCol = ImGui::GetColorU32(uiWithAlpha(uiMix(tcs.resultsText, accent, 0.20f), 0.94f));
        const ImU32 textCol = ImGui::GetColorU32(uiWithAlpha(tcs.resultsText, 0.90f));
        const ImU32 mutedCol = ImGui::GetColorU32(uiWithAlpha(tcs.resultsText, 0.62f));
        const ImU32 badgeBgCol = ImGui::GetColorU32(uiWithAlpha(tcs.resultsBG, 0.46f));

        // Build id → index for parent-visibility lookup (mirrors ComputePlaylistChipPlan).
        std::unordered_map<int64_t, int> idToStateIdx;
        idToStateIdx.reserve(state.playlists.size());
        for (int i = 0; i < (int)state.playlists.size(); ++i) {
            idToStateIdx[state.playlists[(size_t)i].id] = i;
        }
        std::vector<int> visiblePlaylistIndices;
        visiblePlaylistIndices.reserve(state.playlists.size());
        for (int i = 0; i < (int)state.playlists.size(); ++i) {
            const auto& p = state.playlists[(size_t)i];
            if (!IsPlaylistShownInQuickBar(state, p.id)) continue;
            // Skip a child whose parent is also visible — chipPlan collapses it
            // into the parent's hover popup, so it must not consume an anim slot.
            if (p.parent_id > 0) {
                auto it = idToStateIdx.find(p.parent_id);
                if (it != idToStateIdx.end() &&
                    IsPlaylistShownInQuickBar(state, p.parent_id)) {
                    continue;
                }
            }
            visiblePlaylistIndices.push_back(i);
        }

        // Chip plan: drives visual variants (Folder / IndependentChild / Orphan).
        // Computed here so the render loop and add-button positioning both use it.
        const auto chipPlan = HarkenSearch::ComputePlaylistChipPlan(
            state.playlists, state.settings.playlistQuickBarHiddenIds);

        PrunePlaylistQuickBarAnimItems(state.playlists);

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            g_playlistQuickBarReorderAnim.draggedPlaylistId != 0) {
            if (g_playlistQuickBarReorderAnim.orderDirty) {
                PersistPlaylistQuickBarOrderFromCurrent(state);
                SaveAppSettings(state);
                DIAG_ACTION("Playlist", "QuickBar spring reorder committed");
            }
            g_playlistQuickBarReorderAnim.draggedPlaylistId = 0;
            g_playlistQuickBarReorderAnim.dragGrabOffsetX = 0.0f;
            g_playlistQuickBarReorderAnim.orderDirty = false;
        }

        int draggedVisibleIdx = -1;
        for (int visualIdx = 0; visualIdx < (int)visiblePlaylistIndices.size(); ++visualIdx) {
            const int i = visiblePlaylistIndices[(size_t)visualIdx];
            const float targetX = startX + visualIdx * itemStep;
            auto& anim = EnsurePlaylistQuickBarAnimItem(state.playlists[(size_t)i].id, targetX);
            anim.targetX = targetX;
            if (anim.playlistId == g_playlistQuickBarReorderAnim.draggedPlaylistId &&
                ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                draggedVisibleIdx = visualIdx;
                anim.currentX = ImGui::GetMousePos().x - g_playlistQuickBarReorderAnim.dragGrabOffsetX;
                anim.targetX = anim.currentX;
                anim.velocityX = 0.0f;
            } else {
                PlaylistQuickBarSpringStep(anim.currentX, anim.velocityX, anim.targetX, ImGui::GetIO().DeltaTime);
            }
        }

        int pendingSwapIdx = -1;
        int pendingSwapTargetIdx = -1;
        ImGui::SetCursorPosY(4.0f);
        if (false) {

        ImVec2 leadHideP0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##PlaylistQuickHideLead", ImVec2(hideItemW, itemH));
        const bool leadHideHovered = ImGui::IsItemHovered();
        const ImVec2 leadHideP1(leadHideP0.x + hideItemW, leadHideP0.y + itemH);
        dl->AddRectFilled(leadHideP0, leadHideP1,
            ImGui::GetColorU32(uiWithAlpha(tcs.resultsHeaderBG, leadHideHovered ? 0.34f : 0.18f)), 5.0f);
        dl->AddRect(leadHideP0, leadHideP1,
            ImGui::GetColorU32(uiWithAlpha(tcs.resultsOutline, leadHideHovered ? 0.44f : 0.20f)), 5.0f);
        const char* leadHideLabel = L(PLQ_LeadHide);
        const ImVec2 leadHideText = ImGui::CalcTextSize(leadHideLabel);
        dl->AddText(
            ImVec2(leadHideP0.x + (hideItemW - leadHideText.x) * 0.5f,
                   leadHideP0.y + (itemH - leadHideText.y) * 0.5f),
            ImGui::GetColorU32(uiWithAlpha(tcs.resultsText, leadHideHovered ? 0.96f : 0.74f)),
            leadHideLabel);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            SetPlaylistQuickBarVisible(state, false);
        }
        if (leadHideHovered) {
            HarkenSearch::SetTooltip("%s", L(PLQ_BarHideTip));
        }
        if (false) {
        ImGui::SameLine(0.0f, 6.0f);

        }
        }
        if (draggedVisibleIdx >= 0) {
            const float gapX = startX + draggedVisibleIdx * itemStep;
            const float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 10.0f);
            const ImVec2 gapMin(gapX + 4.0f, startY + 3.0f);
            const ImVec2 gapMax(gapX + playlistItemW - 4.0f, startY + itemH - 3.0f);
            dl->AddRectFilled(gapMin, gapMax, ImGui::GetColorU32(uiWithAlpha(accent, 0.07f + pulse * 0.04f)), 6.5f);
            dl->AddRect(gapMin, gapMax, ImGui::GetColorU32(uiWithAlpha(accent, 0.18f + pulse * 0.10f)), 6.5f, 0, 1.2f);
        }

        // Capture folder chip screen rects for hover-popup positioning.
        std::unordered_map<int64_t, ImVec2> chipBottomLeft;

        for (int visualIdx = 0; visualIdx < (int)chipPlan.size(); ++visualIdx) {
            const auto& chip = chipPlan[(size_t)visualIdx];

            // Look up original Playlist and its state.playlists index for swap logic.
            const Playlist* plPtr = nullptr;
            int i = -1;
            for (int si = 0; si < (int)state.playlists.size(); ++si) {
                if (state.playlists[(size_t)si].id == chip.id) {
                    plPtr = &state.playlists[(size_t)si];
                    i = si;
                    break;
                }
            }
            if (!plPtr) continue;
            const Playlist& pl = *plPtr;

            ImGui::PushID((const void*)(intptr_t)pl.id);
            const std::string safePlName = pl.name.empty() ? L(Nav_UnnamedList) : pl.name;
            auto& anim = EnsurePlaylistQuickBarAnimItem(pl.id, startX + visualIdx * itemStep);
            const bool reorderActive = (draggedVisibleIdx >= 0);
            const bool isDraggedItem = reorderActive && pl.id == g_playlistQuickBarReorderAnim.draggedPlaylistId;

            const float dragLift = isDraggedItem ? -3.0f : 0.0f;
            ImVec2 p0(anim.currentX, startY + dragLift);
            ImVec2 p1(p0.x + playlistItemW, p0.y + itemH);
            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton("##PlaylistQuickItem", ImVec2(playlistItemW, itemH));
            const bool hovered = ImGui::IsItemHovered();
            const bool selected = (state.viewMode == ViewMode::Playlist && state.selectedPlaylistId == pl.id);
            const bool dropRectHover = contentDragActive && ImGui::IsMouseHoveringRect(p0, p1, false);
            const bool folderDropHover = dropRectHover
                                         && (chip.kind == HarkenSearch::ChipKind::Folder);
            const bool directDropHover = dropRectHover
                                         && (chip.kind != HarkenSearch::ChipKind::Folder);

            // Folder chip: capture rect for popup positioning and trigger hover popup.
            if (chip.kind == HarkenSearch::ChipKind::Folder) {
                chipBottomLeft[chip.id] = ImVec2(p0.x, p1.y);
                // Use AllowWhenBlockedByPopup so user can drag mouse from chip into popup.
                const bool hoveredForPopup = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
                const bool popupCurrentlyOpen = (state.playlistHoverExpandedParentId == chip.id);
                if (hoveredForPopup && !popupCurrentlyOpen) {
                    state.playlistHoverExpandedParentId = chip.id;
                    state.playlistHoverPopupOpenTime = ImGui::GetTime();
                    ImGui::OpenPopup(("##plfolder_" + std::to_string(chip.id)).c_str());
                }
            }

            ImU32 bg;
            if (chip.kind == HarkenSearch::ChipKind::Folder) {
                bg = selected
                    ? ImGui::GetColorU32(uiWithAlpha(accent, 0.34f))
                    : folderDropHover
                        ? ImGui::GetColorU32(uiWithAlpha(accent, 0.34f))
                        : hovered
                            ? ImGui::GetColorU32(uiWithAlpha(accent, 0.26f))
                        : ImGui::GetColorU32(uiWithAlpha(accent, 0.10f));
            } else {
                bg = selected
                    ? ImGui::GetColorU32(uiWithAlpha(accent, 0.28f))
                    : (hovered || directDropHover)
                        ? ImGui::GetColorU32(uiWithAlpha(accent, directDropHover ? 0.34f : 0.18f))
                        : ImGui::GetColorU32(uiWithAlpha(tcs.resultsHeaderBG, 0.24f));
            }
            if (isDraggedItem) {
                dl->AddRectFilled(
                    ImVec2(p0.x + 1.0f, p0.y + 3.0f),
                    ImVec2(p1.x + 1.0f, p1.y + 3.0f),
                    ImGui::GetColorU32(uiWithAlpha(tcs.resultsOutline, 0.14f)),
                    6.0f);
                bg = ImGui::GetColorU32(uiWithAlpha(accent, 0.16f));
            }
            dl->AddRectFilled(p0, p1, bg, 5.0f);

            // Border: accent-coloured for folders, standard otherwise.
            const bool isFolder = (chip.kind == HarkenSearch::ChipKind::Folder);
            const ImU32 borderCol = isFolder
                ? ImGui::GetColorU32(uiWithAlpha(accent, selected || folderDropHover || isDraggedItem ? 0.72f : 0.44f))
                : ImGui::GetColorU32(uiWithAlpha(accent, selected || directDropHover || isDraggedItem ? 0.44f : 0.16f));
            dl->AddRect(p0, p1, borderCol, 5.0f, 0, isDraggedItem ? 1.4f : 1.0f);
            if (folderDropHover || directDropHover) {
                DrawPlaylistDropTargetHighlight(dl, p0, p1, accent, 5.0f, folderDropHover);
            }

            // Left-side icon area: folder glyph for folders, standard playlist icon otherwise.
            const ImU32 chipIconCol = isDraggedItem
                ? ImGui::GetColorU32(uiWithAlpha(tcs.resultsText, 0.36f))
                : iconCol;
            if (isFolder) {
                // Folder glyph: tab on top, body below — enlarged for visibility.
                const float fx = p0.x + 7.0f;
                const float fy = p0.y + (itemH - 13.0f) * 0.5f;
                dl->AddRectFilled(ImVec2(fx, fy + 3.5f), ImVec2(fx + 15.0f, fy + 13.0f), chipIconCol, 1.5f);
                dl->AddRectFilled(ImVec2(fx, fy),         ImVec2(fx + 7.0f,  fy + 4.5f),  chipIconCol, 1.5f);
            } else {
                DrawSidebarIcon(
                    dl,
                    ImVec2(p0.x + 17.0f, p0.y + itemH * 0.5f),
                    14.0f,
                    3,
                    chipIconCol);
            }

            // Side-marker for IndependentChild chips (parent is eye-hidden).
            if (chip.kind == HarkenSearch::ChipKind::IndependentChild) {
                const ImU32 markerCol = ImGui::GetColorU32(uiWithAlpha(accent, isDraggedItem ? 0.22f : 0.55f));
                dl->AddLine(
                    ImVec2(p0.x + 4.0f, p0.y + 4.0f),
                    ImVec2(p0.x + 4.0f, p1.y - 4.0f),
                    markerCol, 1.5f);
            }

            char countBuf[16];
            const int64_t displayedCount =
                aura::playlist::ResolvePlaylistDisplayedFileCount(
                    state.playlists, pl.id);
            if (displayedCount < 0) {
                snprintf(countBuf, sizeof(countBuf), "...");
            } else {
                snprintf(countBuf, sizeof(countBuf), "%lld", (long long)displayedCount);
            }
            ImVec2 countSize = ImGui::CalcTextSize(countBuf);
            const float badgeW = std::max(24.0f, countSize.x + 12.0f);
            const ImVec2 badgeMin(p1.x - badgeW - 6.0f, p0.y + (itemH - 17.0f) * 0.5f);
            const ImVec2 badgeMax(p1.x - 6.0f, badgeMin.y + 17.0f);
            dl->AddRectFilled(
                badgeMin,
                badgeMax,
                isDraggedItem ? ImGui::GetColorU32(uiWithAlpha(tcs.resultsBG, 0.22f)) : badgeBgCol,
                8.0f);
            dl->AddText(
                ImVec2(badgeMin.x + (badgeW - countSize.x) * 0.5f, badgeMin.y + 1.0f),
                isDraggedItem ? ImGui::GetColorU32(uiWithAlpha(tcs.resultsText, 0.32f)) : mutedCol,
                countBuf);

            // The quick bar intentionally shows only the file count badge; child-count badges duplicated folder hierarchy.
            float nameBadgeClipX = badgeMin.x - 6.0f;

            // Name text: start X offset depends on chip kind (folder glyph is narrower).
            const float nameTextX = isFolder
                ? (p0.x + 23.0f)   // shift right: folder glyph ends ~p0.x+19
                : (chip.kind == HarkenSearch::ChipKind::IndependentChild)
                    ? (p0.x + 38.0f)  // shift right: side-marker + small indent
                    : (p0.x + 34.0f); // standard
            const ImVec2 namePos(nameTextX, p0.y + (itemH - ImGui::GetTextLineHeight()) * 0.5f);
            const ImVec2 nameClipMin(namePos.x, p0.y);
            const ImVec2 nameClipMax(nameBadgeClipX, p1.y);
            dl->PushClipRect(nameClipMin, nameClipMax, true);
            dl->AddText(
                namePos,
                isDraggedItem ? ImGui::GetColorU32(uiWithAlpha(tcs.resultsText, 0.36f)) : textCol,
                safePlName.c_str());
            dl->PopClipRect();

            const bool chipDoubleClicked =
                hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            const bool chipLeftClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

            if (directDropHover && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                if (chip.kind != HarkenSearch::ChipKind::Folder) {
                    AddPlaylistQuickDragToPlaylist(state, pl.id);
                }
                ClearPlaylistQuickDrag();
            }

            if (!ShouldSuppressPlaylistContextPopup() && ImGui::BeginPopupContextItem("##PlaylistQuickContext")) {
                if (MenuItemWithTooltip(L(Nav_RenameList), nullptr, L(Nav_RenameListHint))) {
                    OpenPlaylistRenameDialog(state, pl);
                }
                ImGui::Separator();
                if (chip.kind != HarkenSearch::ChipKind::Folder) {
                    if (MenuItemWithTooltip(L(Nav_CopyList), nullptr, L(Nav_CopyListHint))) {
                        CopyPlaylistFromUi(state, pl);
                    }
                }
                if (MenuItemWithTooltip(L(PLQ_MenuHide), nullptr, L(PLQ_MenuHideHint))) {
                    SetPlaylistShownInQuickBar(state, pl.id, false);
                    ImGui::CloseCurrentPopup();
                }
                if (chip.kind == HarkenSearch::ChipKind::Orphan || chip.kind == HarkenSearch::ChipKind::Folder) {
                    bool canSubPlaylist = ProductEdition::hasFeature("sub_playlist");
                    if (canSubPlaylist) {
                        if (ImGui::MenuItem(L(Playlist_NewInFolder))) {
                            state.pendingNewSubParentId = chip.id;
                            state.pendingNewSubDialog = true;
                            state.pendingNewSubName[0] = '\0';
                        }
                    } else {
                        ImGui::BeginDisabled();
                        ImGui::MenuItem(L(Playlist_NewInFolderLocked), nullptr, false, false);
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            HarkenSearch::SetTooltip("%s", L(Playlist_UpgradeForFolder));
                        }
                    }
                }
                if (chip.kind == HarkenSearch::ChipKind::IndependentChild) {
                    if (ImGui::MenuItem(L(Playlist_MoveToFolder))) {
                        state.pendingMoveSubId = chip.id;
                        state.pendingMoveSubDialog = true;
                    }
                }
                if (chip.kind == HarkenSearch::ChipKind::Folder) {
                    ImGui::Separator();
                    if (ImGui::MenuItem(L(Playlist_DeleteFolder))) {
                        state.pendingDeleteFolderId = chip.id;
                        state.pendingDeleteFolderConfirm = true;
                    }
                }
                ImGui::EndPopup();
            }

            if (ImGui::IsItemActive() &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f) &&
                !g_playlistQuickDrag.active) {
                if (g_playlistQuickBarReorderAnim.draggedPlaylistId != pl.id) {
                    g_playlistQuickBarReorderAnim.draggedPlaylistId = pl.id;
                    MaybePausePlaybackOnInScopeDragStart();
                    g_playlistQuickBarReorderAnim.dragGrabOffsetX = ImGui::GetMousePos().x - anim.currentX;
                    g_playlistQuickBarReorderAnim.orderDirty = false;
                    anim.velocityX = 0.0f;
                }
            }

            if (isDraggedItem && pendingSwapTargetIdx < 0) {
                const float draggedCenter = anim.currentX + playlistItemW * 0.5f;
                float previousCenter = 0.0f;
                float nextCenter = 0.0f;
                if (visualIdx > 0) {
                    const int previousStateIdx = visiblePlaylistIndices[(size_t)visualIdx - 1];
                    auto& prevAnim = EnsurePlaylistQuickBarAnimItem(
                        state.playlists[(size_t)previousStateIdx].id,
                        startX + (visualIdx - 1) * itemStep);
                    previousCenter = prevAnim.currentX + playlistItemW * 0.5f;
                }
                if (visualIdx + 1 < (int)visiblePlaylistIndices.size()) {
                    const int nextStateIdx = visiblePlaylistIndices[(size_t)visualIdx + 1];
                    auto& nextAnim = EnsurePlaylistQuickBarAnimItem(
                        state.playlists[(size_t)nextStateIdx].id,
                        startX + (visualIdx + 1) * itemStep);
                    nextCenter = nextAnim.currentX + playlistItemW * 0.5f;
                }
                const int dir = PlaylistQuickBarSwapDirection(
                    visualIdx,
                    draggedCenter,
                    previousCenter,
                    nextCenter,
                    (int)visiblePlaylistIndices.size());
                if (dir != 0) {
                    pendingSwapIdx = i;
                    pendingSwapTargetIdx = visiblePlaylistIndices[(size_t)(visualIdx + dir)];
                }
            }

            // Drop target for playlist content drags.
            // Must be called while the InvisibleButton is still the last active item.
            bool playlistDropDelivered = false;
            if (ImGui::BeginDragDropTarget()) {
                if (chip.kind == HarkenSearch::ChipKind::Folder) {
                    // Folder chip: refuse direct drop; trigger hover popup so user
                    // can drag into a child sub-chip in the popup.
                    if (state.playlistHoverExpandedParentId != chip.id) {
                        state.playlistHoverExpandedParentId = chip.id;
                        state.playlistHoverPopupOpenTime = ImGui::GetTime();
                        ImGui::OpenPopup(("##plfolder_" + std::to_string(chip.id)).c_str());
                    }
                    ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
                    // Consume supported payloads so nothing else picks them up while
                    // the hover popup is the intended destination.
                    ImGui::AcceptDragDropPayload("AUDIO_FILES", ImGuiDragDropFlags_AcceptBeforeDelivery);
                    ImGui::AcceptDragDropPayload("FOLDER_PATH", ImGuiDragDropFlags_AcceptBeforeDelivery);
                    ImGui::AcceptDragDropPayload("ARTWORK_WALL_ITEM", ImGuiDragDropFlags_AcceptBeforeDelivery);
                } else {
                    // Orphan or IndependentChild: accept file rows, folder-tree drags,
                    // and cover-wall drags directly.
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("AUDIO_FILES")) {
                        const int64_t* ids = (const int64_t*)payload->Data;
                        const int count = (int)(payload->DataSize / sizeof(int64_t));
                        playlistDropDelivered =
                            AddFileIdsToPlaylist(state, chip.id, ids, count, "quickbar-audio-drag") ||
                            playlistDropDelivered;
                    }
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FOLDER_PATH")) {
                        const char* folderPath = (const char*)payload->Data;
                        playlistDropDelivered =
                            AddFolderPathToPlaylist(state, chip.id, folderPath, "quickbar-folder-drag") ||
                            playlistDropDelivered;
                    }
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ARTWORK_WALL_ITEM")) {
                        if (payload->Data && payload->DataSize > 0) {
                            const std::string coverKey((const char*)payload->Data);
                            playlistDropDelivered =
                                AddCoverTileMembershipToPlaylist(state, chip.id, safePlName, coverKey, "quickbar-cover-drag") ||
                                playlistDropDelivered;
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (!playlistDropDelivered &&
                !IsPlaylistContentDragActive() &&
                chipDoubleClicked) {
                OpenPlaylistRenameDialog(state, pl);
            } else if (!playlistDropDelivered &&
                       !IsPlaylistContentDragActive() &&
                       chipLeftClicked) {
                ActivatePlaylistView(state, pl.id, chip.kind == HarkenSearch::ChipKind::Folder, true);
            }

            // Tooltip: for IndependentChild show parent name.
            if (hovered) {
                if (chip.kind == HarkenSearch::ChipKind::IndependentChild && !chip.parentName.empty()) {
                    HarkenSearch::SetTooltip("%s\n(%s: %s)", safePlName.c_str(),
                        L(PLQ_ParentLabel), chip.parentName.c_str());
                } else {
                    HarkenSearch::SetTooltip("%s", safePlName.c_str());
                }
            }
            ImGui::PopID();
        }
        if (pendingSwapTargetIdx >= 0) {
            if (pendingSwapIdx >= 0 &&
                pendingSwapIdx < (int)state.playlists.size() &&
                pendingSwapTargetIdx < (int)state.playlists.size()) {
                if (PlaylistQuickBarSwapItems(state.playlists, pendingSwapIdx, pendingSwapTargetIdx)) {
                    PersistPlaylistQuickBarOrderFromCurrent(state);
                    g_playlistQuickBarReorderAnim.orderDirty = true;
                    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                }
            }
        }

        // Hover popups for folder chips: show their unhidden children horizontally.
        for (const auto& chip : chipPlan) {
            if (chip.kind != HarkenSearch::ChipKind::Folder) continue;
            ImGui::PushID((const void*)(intptr_t)chip.id);
            const std::string popupId = "##plfolder_" + std::to_string(chip.id);
            if (!ImGui::IsPopupOpen(popupId.c_str())) {
                if (state.playlistHoverExpandedParentId == chip.id) {
                    state.playlistHoverExpandedParentId = -1;
                }
                ImGui::PopID();
                continue;
            }

            auto rectIt = chipBottomLeft.find(chip.id);
            if (rectIt != chipBottomLeft.end()) {
                ImGui::SetNextWindowPos(rectIt->second);
            }

            if (BeginTransientScrollbarPopup(popupId.c_str(),
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_AlwaysAutoResize)) {

                // Collect this folder's visible (non-hidden) children.
                std::vector<const Playlist*> kids;
                for (const auto& pl2 : state.playlists) {
                    if (pl2.parent_id != chip.id) continue;
                    if (std::find(state.settings.playlistQuickBarHiddenIds.begin(),
                                  state.settings.playlistQuickBarHiddenIds.end(),
                                  pl2.id) != state.settings.playlistQuickBarHiddenIds.end()) continue;
                    kids.push_back(&pl2);
                }

                if (kids.empty()) {
                    ImGui::TextDisabled(L(PLQ_NoPlaylistsToShow));
                } else {
                    for (size_t k = 0; k < kids.size(); ++k) {
                        if (k > 0) ImGui::SameLine(0.0f, 4.0f);
                        ImGui::PushID(static_cast<int>(kids[k]->id));
                        const bool subChipPressed =
                            ImGui::Button(kids[k]->name.c_str(), ImVec2(playlistItemW, itemH));
                        const bool subChipLeftClicked =
                            ImGui::IsItemClicked(ImGuiMouseButton_Left);
                        const bool subChipHovered = ImGui::IsItemHovered();
                        const ImVec2 btnMin = ImGui::GetItemRectMin();
                        const ImVec2 btnMax = ImGui::GetItemRectMax();
                        const bool subChipDropHover = contentDragActive &&
                                                      ImGui::IsMouseHoveringRect(btnMin, btnMax, false);
                        if (!ShouldSuppressPlaylistContextPopup() && ImGui::BeginPopupContextItem("##SubChipCtx")) {
                            if (ImGui::MenuItem(L(PLQ_MoveToOtherParent))) {
                                state.pendingMoveSubId = kids[k]->id;
                                state.pendingMoveSubDialog = true;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }
                        // Sub-chip drop target: accept the same payloads as a visible
                        // playlist row so folder-tree and file-list drags can land in
                        // hover-expanded children.
                        bool subChipDropDelivered = false;
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("AUDIO_FILES")) {
                                const int64_t* ids = (const int64_t*)payload->Data;
                                const int count = (int)(payload->DataSize / sizeof(int64_t));
                                subChipDropDelivered =
                                    AddFileIdsToPlaylist(state, kids[k]->id, ids, count, "quickbar-popup-audio-drag") ||
                                    subChipDropDelivered;
                                ImGui::CloseCurrentPopup();
                            }
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FOLDER_PATH")) {
                                const char* folderPath = (const char*)payload->Data;
                                subChipDropDelivered =
                                    AddFolderPathToPlaylist(state, kids[k]->id, folderPath, "quickbar-popup-folder-drag") ||
                                    subChipDropDelivered;
                                ImGui::CloseCurrentPopup();
                            }
                            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ARTWORK_WALL_ITEM")) {
                                if (payload->Data && payload->DataSize > 0) {
                                    const std::string coverKey((const char*)payload->Data);
                                    subChipDropDelivered =
                                        AddCoverTileMembershipToPlaylist(state, kids[k]->id, kids[k]->name, coverKey, "quickbar-popup-cover-drag") ||
                                        subChipDropDelivered;
                                    ImGui::CloseCurrentPopup();
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        if (subChipDropHover) {
                            DrawPlaylistDropTargetHighlight(dl, btnMin, btnMax, accent, 4.0f, false);
                        }
                        if ((subChipPressed || subChipLeftClicked) &&
                            !subChipDropDelivered &&
                            !IsPlaylistContentDragActive()) {
                            DIAG_ACTION_FMT(
                                "Playlist",
                                "QuickBarSubChipClick parent=%lld child=%lld childName=%s via=%s",
                                (long long)chip.id,
                                (long long)kids[k]->id,
                                kids[k]->name.c_str(),
                                subChipPressed ? "button" : "click");
                            ActivatePlaylistView(state, kids[k]->id, false, true);
                            ImGui::CloseCurrentPopup();
                        }
                        // Right-click quick-drag drop target for sub-chips.
                        if (g_playlistQuickDrag.active && subChipHovered) {
                            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                                AddPlaylistQuickDragToPlaylist(state, kids[k]->id);
                                ClearPlaylistQuickDrag();
                                state.playlistHoverExpandedParentId = -1;
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        ImGui::PopID();
                    }
                }

                // Auto-close when mouse leaves both popup and parent chip.
                const bool popupHovered = ImGui::IsWindowHovered(
                    ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup);

                bool overParent = false;
                if (rectIt != chipBottomLeft.end()) {
                    const ImVec2 mp = ImGui::GetMousePos();
                    const ImVec2 parentMin(rectIt->second.x, rectIt->second.y - itemH);
                    const ImVec2 parentMax(rectIt->second.x + playlistItemW, rectIt->second.y);
                    overParent = (mp.x >= parentMin.x && mp.x <= parentMax.x &&
                                  mp.y >= parentMin.y && mp.y <= parentMax.y);
                }

                // Anti-flicker debounce: ignore close for first 50ms after open.
                const bool justOpened = (ImGui::GetTime() - state.playlistHoverPopupOpenTime) < 0.05;

                if (overParent &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                    !justOpened &&
                    !IsPlaylistContentDragActive()) {
                    DIAG_ACTION_FMT(
                        "Playlist",
                        "QuickBarFolderOverviewClick parent=%lld",
                        (long long)chip.id);
                    state.playlistHoverExpandedParentId = -1;
                    ImGui::CloseCurrentPopup();
                    ActivatePlaylistView(state, chip.id, true, true);
                }

                if (!popupHovered && !overParent && !justOpened && !IsPlaylistContentDragActive()) {
                    state.playlistHoverExpandedParentId = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            } else {
                // BeginPopup returned false: ImGui dropped the popup.
                if (state.playlistHoverExpandedParentId == chip.id) {
                    state.playlistHoverExpandedParentId = -1;
                }
            }
            ImGui::PopID();
        }

        ImVec2 addP0(startX + chipPlan.size() * itemStep, startY);
        ImGui::SetCursorScreenPos(addP0);
        ImGui::InvisibleButton("##PlaylistQuickAdd", ImVec2(addItemW, itemH));
        const bool addHovered = ImGui::IsItemHovered();
        ImU32 addBg = ImGui::GetColorU32(uiWithAlpha(accent, addHovered ? 0.22f : 0.10f));
        dl->AddRectFilled(addP0, ImVec2(addP0.x + addItemW, addP0.y + itemH), addBg, 5.0f);
        dl->AddRect(addP0, ImVec2(addP0.x + addItemW, addP0.y + itemH),
                    ImGui::GetColorU32(uiWithAlpha(accent, addHovered ? 0.42f : 0.22f)), 5.0f);
        const ImVec2 c(addP0.x + addItemW * 0.5f, addP0.y + itemH * 0.5f);
        const ImU32 plusCol = ImGui::GetColorU32(uiWithAlpha(tcs.resultsText, 0.90f));
        dl->AddLine(ImVec2(c.x - 6.0f, c.y), ImVec2(c.x + 6.0f, c.y), plusCol, 1.7f);
        dl->AddLine(ImVec2(c.x, c.y - 6.0f), ImVec2(c.x, c.y + 6.0f), plusCol, 1.7f);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            ImGui::OpenPopup("##PlaylistQuickAddMenu");
        }
        if (BeginTransientScrollbarPopup("##PlaylistQuickAddMenu")) {
            if (ImGui::MenuItem(L(PL_NewPlaylist))) {
                state.showPlaylistDialog = true;
                memset(state.playlistNameBuffer, 0, sizeof(state.playlistNameBuffer));
            }
            bool canFolder = ProductEdition::hasFeature("sub_playlist");
            if (canFolder) {
                if (ImGui::MenuItem(L(PL_NewFolder))) {
                    state.showCreateFolderDialog = true;
                    memset(state.folderNameBuffer, 0, sizeof(state.folderNameBuffer));
                }
            } else {
                ImGui::BeginDisabled();
                ImGui::MenuItem(L(PL_NewFolder), nullptr, false, false);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    HarkenSearch::SetTooltip("%s", L(Playlist_UpgradeForFolder));
                }
            }
            ImGui::EndPopup();
        }
        if (addHovered) HarkenSearch::SetTooltip(L(PLQ_AddNewTooltip));

        }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    DrawPlaylistQuickDragPreview();
    if (g_playlistQuickDrag.armed && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        ClearPlaylistQuickDrag();
    }

// F-017 v2: Playlist folder action modals

    // Dialog A: New playlist in folder
    if (state.pendingNewSubDialog) {
        ImGui::OpenPopup((std::string(L(Dlg_NewPlaylist)) + "##plnewsub").c_str());
        state.pendingNewSubDialog = false;
    }
    if (BeginTransientScrollbarPopupModal((std::string(L(Dlg_NewPlaylist)) + "##plnewsub").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        { ReadingTextScope _readingPopup;  // dialog body is a reading surface
        int64_t pid = state.pendingNewSubParentId;
        const Playlist* parent = nullptr;
        for (const auto& p : state.playlists) {
            if (p.id == pid) { parent = &p; break; }
        }
        if (!parent) {
            ImGui::CloseCurrentPopup();
        } else {
            // Auto-fill default name once
            if (state.pendingNewSubName[0] == '\0') {
                int idx = 1;
                for (;;) {
                    std::string trial = parent->name + u8" " + std::to_string(idx);
                    bool dup = false;
                    for (const auto& p : state.playlists) {
                        if (p.parent_id == pid && p.name == trial) { dup = true; break; }
                    }
                    if (!dup) {
                        std::snprintf(state.pendingNewSubName, sizeof(state.pendingNewSubName),
                                      "%s", trial.c_str());
                        break;
                    }
                    ++idx;
                    if (idx > 999) { state.pendingNewSubName[0] = ' '; break; }
                }
            }
            ImGui::Text(L(PLQ_NewPlaylistUnderFmt), parent->name.c_str());
            ImGui::InputText(L(PLQ_NewSubNameField), state.pendingNewSubName, sizeof(state.pendingNewSubName));

            bool canSubmit = (state.pendingNewSubName[0] != '\0');
            if (!canSubmit) ImGui::BeginDisabled();
            if (aura::ui::ActionButton(L(PLQ_NewSubConfirmBtn), ImVec2(0, 0), aura::ui::ButtonVariant::PrimarySearch)) {
                if (parent->is_folder) {
                    state.database.createSubPlaylist(pid, state.pendingNewSubName, "");
                } else {
                    int64_t fileCount = state.database.getPlaylistFileCount(pid);
                    if (fileCount == 0) {
                        state.database.markAsFolder(pid);
                        state.database.createSubPlaylist(pid, state.pendingNewSubName, "");
                    } else {
                        // Two-stage: stash the user's intended sub name + default name for original files
                        state.pendingPromoteOrphanId = pid;
                        std::snprintf(state.pendingPromoteDefaultChildName,
                                      sizeof(state.pendingPromoteDefaultChildName),
                                      L(PLQ_OriginalFmt), parent->name.c_str());
                        std::snprintf(state.pendingPromoteUserChildName,
                                      sizeof(state.pendingPromoteUserChildName),
                                      "%s", state.pendingNewSubName);
                        state.pendingPromoteOrphanDialog = true;
                    }
                }
                state.playlists = state.database.getAllPlaylists();
                ImGui::CloseCurrentPopup();
            }
            if (!canSubmit) ImGui::EndDisabled();
            ImGui::SameLine();
            if (aura::ui::ActionButton(L(PLQ_NewSubCancelBtn), ImVec2(0, 0), aura::ui::ButtonVariant::Quiet)) ImGui::CloseCurrentPopup();
        }
        }
        ImGui::EndPopup();
    }

    // Dialog B: Promote orphan with files (two-stage from Dialog A)
    if (state.pendingPromoteOrphanDialog) {
        ImGui::OpenPopup((std::string(L(PLQ_PromoteTitle)) + "##plpromote").c_str());
        state.pendingPromoteOrphanDialog = false;
    }
    if (BeginTransientScrollbarPopupModal((std::string(L(PLQ_PromoteTitle)) + "##plpromote").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        { ReadingTextScope _readingPopup;  // dialog body is a reading surface
        int64_t fileCount = state.database.getPlaylistFileCount(state.pendingPromoteOrphanId);
        ImGui::TextWrapped(L(PLQ_PromoteHint),
                          (long long)fileCount);
        ImGui::InputText(L(PLQ_PromoteOrigNameField),
                         state.pendingPromoteDefaultChildName,
                         sizeof(state.pendingPromoteDefaultChildName));
        if (aura::ui::ActionButton(L(PLQ_PromoteConfirmBtn), ImVec2(0, 0), aura::ui::ButtonVariant::PrimarySearch)) {
            int64_t defaultChild = state.database.promoteOrphanToFolderWithDefaultChild(
                state.pendingPromoteOrphanId, state.pendingPromoteDefaultChildName);
            if (defaultChild > 0) {
                state.database.createSubPlaylist(state.pendingPromoteOrphanId,
                                                state.pendingPromoteUserChildName, "");
            }
            state.playlists = state.database.getAllPlaylists();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (aura::ui::ActionButton(L(PLQ_PromoteCancelBtn), ImVec2(0, 0), aura::ui::ButtonVariant::Quiet)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Dialog C: Move playlist to other folder
    if (state.pendingMoveSubDialog) {
        ImGui::OpenPopup((std::string(L(PLQ_MoveTitle)) + "##plmovesub").c_str());
        state.pendingMoveSubDialog = false;
    }
    if (BeginTransientScrollbarPopupModal((std::string(L(PLQ_MoveTitle)) + "##plmovesub").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        { ReadingTextScope _readingPopup;  // dialog body is a reading surface
        ImGui::Text(L(PLQ_ChooseTargetFolder));
        if (ImGui::Selectable(L(PLQ_NoParentFolder))) {
            state.database.moveSubPlaylistToParent(state.pendingMoveSubId, -1);
            state.playlists = state.database.getAllPlaylists();
            ImGui::CloseCurrentPopup();
        }
        // Find current parent to exclude it
        int64_t currentParent = -1;
        for (const auto& p : state.playlists) {
            if (p.id == state.pendingMoveSubId) { currentParent = p.parent_id; break; }
        }
        for (const auto& p : state.playlists) {
            if (!p.is_folder) continue;
            if (p.id == currentParent) continue;  // skip current parent
            ImGui::PushID((const void*)(intptr_t)p.id);
            if (ImGui::Selectable(p.name.c_str())) {
                state.database.moveSubPlaylistToParent(state.pendingMoveSubId, p.id);
                state.playlists = state.database.getAllPlaylists();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (aura::ui::ActionButton(L(PLQ_MoveSubCancelBtn), ImVec2(0, 0), aura::ui::ButtonVariant::Quiet)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Dialog D: Delete playlist / folder confirmation.
    // Triggered from:
    //   - Bottom quick-bar folder chip context menu (folder cascade case)
    //   - Left sidebar playlist row context menu "删除列表" (any kind)
    if (state.pendingDeleteFolderConfirm) {
        ImGui::OpenPopup((std::string(L(PLQ_DeleteTitle)) + "##pldelfolder").c_str());
        state.pendingDeleteFolderConfirm = false;
    }
    {
        // Fixed comfortable width; height auto-fits content. Centred over viewport.
        const ImVec2 viewportCenter = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(viewportCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
        if (BeginTransientScrollbarPopupModal((std::string(L(PLQ_DeleteTitle)) + "##pldelfolder").c_str(), nullptr,
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            { ReadingTextScope _readingPopup;  // dialog body is a reading surface
            EscClosePopup();
            int64_t fid = state.pendingDeleteFolderId;
            const Playlist* target = nullptr;
            for (const auto& p : state.playlists) {
                if (p.id == fid) { target = &p; break; }
            }
            if (!target) {
                ImGui::CloseCurrentPopup();
            } else {
                // Warning header — accent-orange exclamation + headline.
                const ImVec4 warnCol(0.96f, 0.62f, 0.22f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, warnCol);
                ImGui::TextUnformatted(L(PLQ_CannotUndoWarn));
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Body — what gets deleted, in plain wording.
                if (target->is_folder) {
                    auto kids = state.database.getChildrenOf(fid);
                    ImGui::TextWrapped(L(PLQ_DelFolderConfirmFmt),
                                       target->name.c_str(), kids.size());
                    ImGui::Spacing();
                    ImGui::Indent(12.0f);
                    for (const auto& k : kids) {
                        const std::string label = k.name.empty() ? std::string(L(PLQ_Unnamed)) : k.name;
                        ImGui::BulletText(L(PLQ_DelFolderListItemFmt),
                                          label.c_str(), (long long)k.fileCount);
                    }
                    ImGui::Unindent(12.0f);
                } else {
                    ImGui::TextWrapped(L(PLQ_DelPlaylistConfirmFmt),
                                       target->name.c_str());
                    ImGui::Spacing();
                    ImGui::TextDisabled(L(PLQ_ContainsFilesFmt), (long long)target->fileCount);
                }

                ImGui::Spacing();
                ImGui::TextDisabled(L(PLQ_DeleteScopeNote));
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Right-aligned button row: [取消] [确认删除] (destructive on right, red).
                const float btnW = 110.0f;
                const float gap = 8.0f;
                const float rowW = btnW * 2 + gap;
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - rowW - 16.0f);
                if (aura::ui::ActionButton(L(PLQ_DelFolderCancelBtn), ImVec2(btnW, 0), aura::ui::ButtonVariant::Quiet)) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine(0.0f, gap);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.62f, 0.18f, 0.14f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.24f, 0.18f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.13f, 0.11f, 1.0f));
                if (aura::ui::ActionButton(L(PLQ_DelFolderConfirmBtn), ImVec2(btnW, 0), aura::ui::ButtonVariant::Danger)) {
                    state.pendingDeleteFolderSecondConfirm = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopStyleColor(3);
            }
            }
            ImGui::EndPopup();
        }
    }

    // Dialog E: Bulk add confirmation (封面拖拽文件数 > 500 时弹出)
    if (state.pendingDeleteFolderSecondConfirm) {
        ImGui::OpenPopup((std::string(L(PLQ_DeleteAgain)) + "##pldelete_second").c_str());
        state.pendingDeleteFolderSecondConfirm = false;
    }
    if (BeginTransientScrollbarPopupModal((std::string(L(PLQ_DeleteAgain)) + "##pldelete_second").c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        { ReadingTextScope _readingPopup;  // dialog body is a reading surface
        EscClosePopup();
        int64_t fid = state.pendingDeleteFolderId;
        const Playlist* target = nullptr;
        for (const auto& p : state.playlists) {
            if (p.id == fid) { target = &p; break; }
        }
        if (!target) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextWrapped(target->is_folder
                ? L(PLQ_DeleteFolderWarn)
                : L(PLQ_DeleteListWarn));
            ImGui::Spacing();
            ImGui::Text("%s", target->name.c_str());
            ImGui::Separator();
            if (aura::ui::ActionButton(L(PLQ_Del2ndCancelBtn), ImVec2(110, 0), aura::ui::ButtonVariant::Quiet)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.62f, 0.18f, 0.14f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.24f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.13f, 0.11f, 1.0f));
            if (aura::ui::ActionButton(L(PLQ_Del2ndConfirmBtn), ImVec2(150, 0), aura::ui::ButtonVariant::Danger)) {
                state.database.deletePlaylist(fid);
                state.playlists = state.database.getAllPlaylists();
                if (state.selectedPlaylistId == fid) {
                    state.playlistFolderHierarchy = FolderInfo{};
                    state.playlistFolderHierarchyPlaylistId = -1;
                    state.expandedPlaylistFolderTreePlaylistId = -1;
                    state.restoreDatabaseFolderHierarchy();
                    state.viewMode = ViewMode::Database;
                    state.preferDeferredCountForNextRefresh = true;
                    state.playlistAggregatedView = false;
                    RefreshSearchResults(state, true);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(3);
        }
        }
        ImGui::EndPopup();
    }

    if (state.pendingBulkAddPrompt) {
        ImGui::OpenPopup((std::string(L(PLQ_BulkTitle)) + "##plbulk").c_str());
        state.pendingBulkAddPrompt = false;
    }
    if (BeginTransientScrollbarPopupModal((std::string(L(PLQ_BulkTitle)) + "##plbulk").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        { ReadingTextScope _readingPopup;  // dialog body is a reading surface
        ImGui::TextWrapped(L(PLQ_BulkAddConfirmFmt),
                           state.pendingBulkAddFileIds.size(),
                           state.pendingBulkAddTargetName.c_str());
        if (aura::ui::ActionButton(L(PLQ_BulkConfirmBtn), ImVec2(0, 0), aura::ui::ButtonVariant::PrimarySearch)) {
            const auto bulk = state.database.addManyToPlaylist(
                state.pendingBulkAddTargetId,
                state.pendingBulkAddFileIds);
            const int added = bulk.addedFiles;
            char bulkMsg[512];
            std::snprintf(bulkMsg, sizeof(bulkMsg), L(PLQ_BulkDoneFmt), added,
                          state.pendingBulkAddTargetName.c_str());
            state.loadErrorToast = bulkMsg;
            state.loadErrorToastUntil = ImGui::GetTime() + 3.0;
            state.playlists = state.database.getAllPlaylists();
            if (state.viewMode == ViewMode::Playlist &&
                state.selectedPlaylistId == state.pendingBulkAddTargetId) {
                LoadPlaylistFiltered(state, state.pendingBulkAddTargetId, false);
            }
            state.pendingBulkAddFileIds.clear();
            state.pendingBulkAddTargetName.clear();
            state.pendingBulkAddTargetId = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (aura::ui::ActionButton(L(PLQ_BulkCancelBtn), ImVec2(0, 0), aura::ui::ButtonVariant::Quiet)) {
            state.pendingBulkAddFileIds.clear();
            state.pendingBulkAddTargetName.clear();
            state.pendingBulkAddTargetId = -1;
            ImGui::CloseCurrentPopup();
        }
        }
        ImGui::EndPopup();
    }
}

void RenderPlaylistQuickBarRestoreStrip(AppState& state, float height) {
    if (height <= 1.0f) return;

    const auto& tcs = ThemeColors::current();
    auto uiWithAlpha = [](ImVec4 c, float a) -> ImVec4 { c.w = a; return c; };
    auto uiMix = [](const ImVec4& a, const ImVec4& b, float t) -> ImVec4 {
        const float u = 1.0f - t;
        return ImVec4(
            a.x * u + b.x * t,
            a.y * u + b.y * t,
            a.z * u + b.z * t,
            a.w * u + b.w * t);
    };

    ImGui::SetCursorPosX(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 3.0f));
    BeginTransientScrollbarChild("PlaylistQuickBarRestore", ImVec2(0.0f, height), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1(p0.x + ImGui::GetWindowSize().x, p0.y + ImGui::GetWindowSize().y);
        dl->AddRectFilledMultiColor(
            p0, p1,
            ImGui::GetColorU32(uiWithAlpha(uiMix(tcs.resultsBG, tcs.mainWindowBG, 0.18f), 0.94f)),
            ImGui::GetColorU32(uiWithAlpha(uiMix(tcs.resultsBG, tcs.mainWindowBG, 0.10f), 0.94f)),
            ImGui::GetColorU32(uiWithAlpha(uiMix(tcs.resultsBG, tcs.mainWindowBG, 0.22f), 0.94f)),
            ImGui::GetColorU32(uiWithAlpha(uiMix(tcs.resultsBG, tcs.mainWindowBG, 0.14f), 0.94f)));
        dl->AddLine(p0, ImVec2(p1.x, p0.y), ImGui::GetColorU32(uiWithAlpha(tcs.resultsOutline, 0.20f)), 1.0f);
        dl->AddLine(ImVec2(p0.x, p1.y - 1.0f), ImVec2(p1.x, p1.y - 1.0f),
                    ImGui::GetColorU32(uiWithAlpha(tcs.resultsOutline, 0.18f)), 1.0f);

        const char* showLabel = L(PLQ_LeadShow);
        const ImVec2 textSize = ImGui::CalcTextSize(showLabel);
        const float buttonW = textSize.x + 28.0f;
        const float buttonH = height - 6.0f;
        const ImVec2 b0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##ShowPlaylistQuickBar", ImVec2(buttonW, buttonH));
        const bool hovered = ImGui::IsItemHovered();
        dl->AddRectFilled(
            b0, ImVec2(b0.x + buttonW, b0.y + buttonH),
            ImGui::GetColorU32(uiWithAlpha(tcs.resultsSelection, hovered ? 0.26f : 0.14f)), 5.0f);
        dl->AddRect(
            b0, ImVec2(b0.x + buttonW, b0.y + buttonH),
            ImGui::GetColorU32(uiWithAlpha(tcs.resultsSelection, hovered ? 0.42f : 0.26f)), 5.0f);
        dl->AddText(
            ImVec2(b0.x + 14.0f, b0.y + (buttonH - textSize.y) * 0.5f),
            ImGui::GetColorU32(uiWithAlpha(tcs.resultsText, hovered ? 0.98f : 0.88f)),
            showLabel);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            SetPlaylistQuickBarVisible(state, true);
        }
        if (hovered) {
            HarkenSearch::SetTooltip("%s", L(PLQ_BarShowTip));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

