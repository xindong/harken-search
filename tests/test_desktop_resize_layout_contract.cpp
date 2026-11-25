#include <filesystem>

#include <fstream>

#include <iostream>

#include <sstream>

#include <string>



namespace {



std::string readText(const std::filesystem::path& path) {

    std::ifstream in(path, std::ios::binary);

    std::ostringstream ss;

    ss << in.rdbuf();

    std::string text = ss.str();

    std::string normalized;

    normalized.reserve(text.size());

    for (char ch : text) {

        if (ch != '\r') normalized.push_back(ch);

    }

    return normalized;

}



bool requireContains(const std::string& haystack, const std::string& needle, const char* message) {

    if (haystack.find(needle) == std::string::npos) {

        std::cerr << "FAIL: " << message << "\nmissing: " << needle << "\n";

        return false;

    }

    return true;

}



bool requireNotContains(const std::string& haystack, const std::string& needle, const char* message) {

    if (haystack.find(needle) != std::string::npos) {

        std::cerr << "FAIL: " << message << "\nforbidden: " << needle << "\n";

        return false;

    }

    return true;

}



bool requireOrder(const std::string& haystack, const std::string& first, const std::string& second, const char* message) {

    const size_t a = haystack.find(first);

    const size_t b = haystack.find(second);

    if (a == std::string::npos || b == std::string::npos || !(a < b)) {

        std::cerr << "FAIL: " << message << "\norder: " << first << " before " << second << "\n";

        return false;

    }

    return true;

}



} // namespace



int main() {

    std::filesystem::path root = std::filesystem::current_path();

#ifdef AURA_SOURCE_DIR

    if (!std::filesystem::exists(root / "src" / "main.cpp")) {

        root = AURA_SOURCE_DIR;

    }

#endif

    const std::string mainCpp = readText(root / "src" / "main.cpp");

    const std::string appStateH = readText(root / "src" / "AppState.h");

    const std::string audioEngineCpp = readText(root / "src" / "AudioEngine.cpp");

    const std::string audioEngineH = readText(root / "src" / "AudioEngine.h");

    if (mainCpp.empty() || appStateH.empty()) {

        std::cerr << "FAIL: src/main.cpp and AppState.h are readable for desktop resize layout contract\n";

        return 1;

    }

    if (audioEngineCpp.empty() || audioEngineH.empty()) {

        std::cerr << "FAIL: AudioEngine sources are readable for waveform solo contract\n";

        return 1;

    }



    bool ok = true;

    ok &= requireContains(mainCpp, "layoutCompactWidth = mainWorkW < 1100.0f",

                          "desktop resize has a compact-width breakpoint");

    ok &= requireContains(mainCpp, "layoutTinyWidth = mainWorkW < 760.0f",

                          "desktop resize has a tiny-width breakpoint");

    ok &= requireContains(mainCpp, "layoutShortHeight = mainWorkH < 620.0f",

                          "desktop resize adapts bottom waveform/status height");

    ok &= requireContains(mainCpp, "static constexpr float kWorkspaceModuleGutterPx = 8.0f",

                          "primary workspace modules share one 8px gutter token");

    ok &= requireContains(mainCpp, "const float splitterWidth = kWorkspaceModuleGutterPx",

                          "left/right workspace splitters use the same gutter token as layout math");

    ok &= requireContains(mainCpp, "showDockedWaveform ? kWorkspaceModuleGutterPx : 0.0f",

                          "bottom waveform reserve matches the workspace gutter token");

    ok &= requireContains(mainCpp, "workspaceWaveformSplitter ? kWorkspaceModuleGutterPx : 6.0f",

                          "bottom workspace splitter is 8px while internal subpane splitters remain compact");

    ok &= requireNotContains(mainCpp, "isRightInfoSplitter ? 16.0f : 12.0f",

                             "right info seam must not drift wider than the left workspace seam");

    ok &= requireContains(mainCpp, "searchInputW = std::clamp(searchAvailableW",

                          "top search width is content-driven, not fixed-only");

    ok &= requireContains(mainCpp, "Half-screen toolbar adaptation",

                          "top toolbar documents the half-screen fit policy");

    ok &= requireContains(mainCpp, "navReserveW = layoutTinyWidth ? 252.0f : (layoutCompactWidth ? 276.0f : 620.0f)",

                          "top search reserves enough reachable width for compact toolbar tools");

    ok &= requireContains(mainCpp, "bool navAllIconOnly = layoutTinyWidth",

                          "top toolbar has an all-icon fallback before tools are clipped");

    ok &= requireContains(mainCpp, "float navW = measureToolbarNav(navAllIconOnly)",

                          "top toolbar measures nav width before final placement");

    ok &= requireContains(mainCpp, "if (navW > navAvailableW)",

                          "top toolbar switches compression based on actual available width");

    ok &= requireContains(mainCpp, "navOrigin.x + navW + toolbarNavRightPad",

                          "top toolbar keeps rightmost tools inside the window");

    ok &= requireContains(mainCpp, "const bool iconOnly = navAllIconOnly || layoutTinyWidth || (layoutCompactWidth && i >= 4)",

                          "top toolbar uses the adaptive all-icon mode when half-screen space is tight");

    ok &= requireNotContains(mainCpp, "ImGui::SetNextItemWidth(300.0f);\n            //",

                             "top search must not return to an unconditional 300px width");

    ok &= requireContains(mainCpp, "ImGui::TextColored(kInfoLabel,",

                          "database breadcrumb keeps an explicit info-label prefix");

    ok &= requireContains(mainCpp, "ResolveFolderBreadcrumbDisplayLabel(",

                          "database breadcrumb still resolves the dedicated path label text before rendering");

    ok &= requireContains(mainCpp, "kBreadcrumbLabelIndentX = 0.0f",

                          "database breadcrumb removes the extra left indent so path content aligns with the table below");

    ok &= requireContains(mainCpp, "kBreadcrumbLabelGapX = 2.0f",

                          "database breadcrumb keeps a tight label-to-path gap instead of a loose offset");

    ok &= requireContains(mainCpp, "ImGui::AlignTextToFramePadding();",

                          "database breadcrumb label vertically aligns with the clickable path chips");

    ok &= requireContains(mainCpp, "const float fittedRightPanelWidth = std::max(",

                          "right info panel shrinks to the readable fit before layout hides it");

    ok &= requireContains(mainCpp, "state.panelFileInfo.dockedWidth = rightPanelWidth;",

                          "right info panel persists the fitted dock width after oversize dragging");

    ok &= requireContains(mainCpp, "constexpr float kFileNameColMinW = 240.0f",

                          "results table keeps a readable filename minimum width");

    ok &= requireContains(mainCpp, "if (fileNameColW < kFileNameColMinW)",

                          "results table protects the filename column from auto-fit collapse");

    ok &= requireContains(mainCpp, "if (fileNameReqW > 0.0f && fileNameReqW < kFileNameColMinW)",

                          "results table clamps header auto-fit or drag resize when filename width becomes unreadable");

    ok &= requireContains(mainCpp, "breadcrumbChipPadX = 2.0f",

                          "database breadcrumb uses compact horizontal padding without shrinking the font");

    ok &= requireContains(mainCpp, "breadcrumbSeparatorPadX = 4.0f",

                          "database breadcrumb option A gives the arrow 6 px per side with the 2 px chip inset");

    ok &= requireContains(mainCpp, "CalcTextSize(\">\").x + breadcrumbSeparatorPadX * 2.0f",

                          "database breadcrumb scroll extent includes both separator gaps");

    ok &= requireOrder(mainCpp, "ImGui::SameLine(0, breadcrumbSeparatorPadX);",

                       "ImGui::TextColored(kInfoMuted, \">\");",

                       "database breadcrumb reserves the gap before the original arrow");

    ok &= requireContains(mainCpp,

                          "ImGui::TextColored(kInfoMuted, \">\");\n"

                          "                                ImGui::SameLine(0, breadcrumbSeparatorPadX);",

                          "database breadcrumb reserves the same gap after the original arrow");

    ok &= requireContains(mainCpp, "ImGui::BeginChild(\"##DatabaseBreadcrumbPath\", ImVec2(breadcrumbViewportW, breadcrumbRowH)",

                          "database breadcrumb clips overflow in a single-row viewport");

    ok &= requireContains(mainCpp, "breadcrumbMaxChipW",

                          "database breadcrumb still caps individual overlong segments");

    ok &= requireContains(mainCpp, "ImGui::SetNextWindowScroll(ImVec2(breadcrumbScrollX, 0.0f))",

                          "database breadcrumb moves only on its horizontal axis");

    ok &= requireNotContains(mainCpp, "wrapBreadcrumbLineIfNeeded",

                             "database breadcrumb must not wrap path segments when they overflow");

    ok &= requireContains(mainCpp, "drawBreadcrumbPathChip",

                          "database breadcrumb draws restrained path chips");

    ok &= requireContains(mainCpp, "RenderTextEllipsis",

                          "database breadcrumb ellipsizes overlong path segments");

    ok &= requireContains(mainCpp, "##PathSegmentChip",

                          "database breadcrumb segment remains an interactive item");

    ok &= requireNotContains(mainCpp, "drawWrappedBreadcrumbSegment",

                             "database breadcrumb must not draw old narrow wrapped segment blocks");

    ok &= requireNotContains(mainCpp, "##WrappedPathSegment",

                             "database breadcrumb must not keep the old narrow wrapped segment item");

    ok &= requireNotContains(mainCpp, "ImGui::TextColored(kInfoLabel, u8\"文件夹: \")",

                             "database breadcrumb must not use the old folder label");

    ok &= requireContains(mainCpp, "allowDockedLeftColumn",

                          "left navigation panel is gated by responsive layout");

    ok &= requireContains(mainCpp, "allowDockedRightColumn",

                          "right info/content panel is gated by responsive layout");

    ok &= requireContains(mainCpp, "minReadableCenterWidth",

                          "center workspace keeps a named minimum readable width");

    ok &= requireContains(mainCpp, "compactStatusControls",

                          "bottom/status buttons have a compact layout path");

    ok &= requireContains(mainCpp, "ImGui::BeginPopupModal(L(Dlg_Settings), nullptr, ImGuiWindowFlags_None)",

                          "settings dialog must use a resizable popup modal");

    ok &= requireContains(mainCpp, "state.settings.settingsDialogWidth = clampedSettingsDialogW",

                          "settings dialog persists resized width");

    ok &= requireContains(mainCpp, "state.settings.settingsDialogHeight = clampedSettingsDialogH",

                          "settings dialog persists resized height");

    ok &= requireNotContains(mainCpp, "ImGui::BeginPopupModal(L(Dlg_Settings), nullptr, ImGuiWindowFlags_NoResize)",

                             "settings dialog must not keep the old NoResize modal flag");



    ok &= requireContains(mainCpp, "RenderSettingsBuiltinAvatar",

                          "settings user-info must render a real built-in avatar preview instead of color-only dots");

    ok &= requireContains(mainCpp, "DrawSettingsAvatarPreview",

                          "settings user-info must show the active avatar even when no custom photo is set");

    ok &= requireContains(mainCpp, "s_avatarTexRetryFrames",

                          "custom avatar preview retries async texture loading instead of staying blank after the first miss");

    ok &= requireContains(mainCpp, "state.settings.hasCustomAvatar = false;",

                          "selecting a built-in avatar must make it visible instead of letting the custom photo keep priority");

    ok &= requireContains(mainCpp, "kSettingsAvatarChangeLabelZh",

                          "settings user-info avatar button supports Chinese UI copy");

    ok &= requireContains(mainCpp, "kSettingsBuiltinAvatarLabelZh",

                          "settings built-in avatar section supports Chinese UI copy");

    ok &= requireContains(mainCpp, "kSettingsAvatarStatusLabelZh",

                          "settings built-in avatar status/tooltip supports Chinese UI copy");

    ok &= requireNotContains(mainCpp, "ImGui::SeparatorText(\"Built-in Avatar\")",

                             "settings built-in avatar section must not remain English-only");

    ok &= requireNotContains(mainCpp, "ImGui::Button(\"Change Avatar...\")",

                             "settings custom avatar action must not remain English-only");



    ok &= requireContains(mainCpp, "kSettingsAvatarPickerOpenLabelZh",

                          "settings built-in avatar picker opens from a dedicated compact action instead of inline crowding");

    ok &= requireContains(mainCpp, "BuiltInAvatarPickerModal",

                          "settings built-in avatar choices live in a separate picker modal");

    ok &= requireContains(mainCpp, "ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f)",

                          "settings built-in avatar picker modal is large enough for readable preview cells");

    ok &= requireContains(mainCpp, "kSettingsAvatarPickerCellSize = 72.0f",

                          "settings built-in avatar picker uses larger preview cells than the settings page row");

    ok &= requireContains(mainCpp, "kSettingsAvatarPendingPreviewSize = 104.0f",

                          "settings built-in avatar picker shows a large pending/actual preview");

    ok &= requireContains(mainCpp, "s_pendingBuiltInAvatarIndex",

                          "settings built-in avatar picker stages a pending selection before apply");

    ok &= requireContains(mainCpp, "RenderSettingsBuiltinAvatar(ImGui::GetWindowDrawList(), pendingPreviewCenter",

                          "settings built-in avatar pending preview uses the same renderer as picker cells");

    ok &= requireContains(mainCpp, "RenderSettingsBuiltinAvatar(ImGui::GetWindowDrawList(), center, radius, i, hov, pendingSel)",

                          "settings built-in avatar picker cells use the same renderer as the pending preview");

    ok &= requireNotContains(mainCpp, "constexpr float kCellSize = 42.f",

                             "settings built-in avatar picker must not remain an inline small grid in the settings page");



    ok &= requireContains(mainCpp, "sidebarRelatedPathsHeight",

                          "sidebar related-path pane height is persisted instead of fixed");

    ok &= requireContains(mainCpp, "##RelatedPathsTreeSplitter",

                          "related paths and folder tree have a draggable splitter");

    ok &= requireContains(mainCpp, "growWhenDraggingDown",

                          "horizontal splitter supports resizing a top pane by dragging down");

    ok &= requireContains(mainCpp, "sidebarTreeMinHeight = 140.0f",

                          "folder tree keeps a readable minimum height under the related-path pane");

    ok &= requireContains(mainCpp, "state.viewMode == ViewMode::Database) {",

                          "related-path pane keeps a stable default slot even when empty");

    ok &= requireContains(mainCpp, "int showCount = (std::min)(20, (int)state.currentPathFacets.size())",

                          "related-path suggestions keep the larger original row budget");

    ok &= requireContains(mainCpp, "RelatedPathFacetDisplayLabel(p.folderPath)",

                          "related-path suggestions show two-level folder context instead of basename-only labels");

    ok &= requireContains(mainCpp, "ImGui::Selectable(label.c_str(), false)",

                          "related-path suggestions keep clickable row affordance without selected-row state");

    ok &= requireContains(mainCpp, "state.suppressFacetsForNextRefresh = true;",

                          "related-path click filters results without recomputing related paths into a self-only list");

    ok &= requireContains(mainCpp, "state.preservePathFacetsForNextRefresh = true;",

                          "related-path click preserves the current related-path list while filtering results");

    ok &= requireContains(appStateH, "bool preservePathFacetsForNextRefresh = false;",

                          "AppState has a one-shot guard for preserving related-path facets across related-path clicks");

    ok &= requireContains(mainCpp, "if (!state.preservePathFacetsForNextRefresh) {\n                state.currentPathFacets.clear();",

                          "filtered refresh pre-clear must not erase related paths during related-path clicks");

    ok &= requireContains(mainCpp, "Folder-tree clicks remain the",

                          "related-path/folder-tree facet ownership is documented at the interaction site");

    ok &= requireNotContains(mainCpp, "BaseNameFromUtf8Path(p.folderPath) + \"  [\"",

                             "related-path suggestions must not render ambiguous basename-only facet labels");

    ok &= requireNotContains(mainCpp, "kRelatedPathsVisibleRows = 5",

                             "related-path suggestions must not regress to a five-row cap");

    ok &= requireNotContains(mainCpp, "kRelatedPathsCompactMaxHeight = 156.0f",

                             "related-path height must not shrink to content-driven compact cap");

    ok &= requireContains(mainCpp, "renderFolderTreeToolbar",

                          "folder-tree search/refresh/locate toolbar has a movable render call");

    ok &= requireOrder(mainCpp, "##RelatedPathsTreeSplitter", "renderFolderTreeToolbar();",

                       "folder-tree toolbar renders below the related-paths splitter and above the folder tree");

    ok &= requireNotContains(mainCpp, "ImVec2(0, 130), true, ImGuiWindowFlags_AlwaysVerticalScrollbar",

                             "related-path pane must not regress to a fixed 130px height");

    ok &= requireNotContains(mainCpp, "\"##RelatedPathsBox\",\n                ImVec2(0, state.settings.sidebarRelatedPathsHeight),\n                true,",

                             "related-path pane must not draw a child border at the split seam");

    ok &= requireNotContains(mainCpp, "ImGui::BeginChild(\"FolderTree\", ImVec2(-1, treeH), true)",

                             "folder tree must not draw a child border at the split seam");

    ok &= requireContains(mainCpp, "constexpr float kPlaylistFolderRowH = 40.0f",

                          "playlist folders keep a taller container row for hierarchy recognition");

    ok &= requireContains(mainCpp, "constexpr float kPlaylistChildRowH = 34.0f",

                          "playlist children keep a smaller nested row for hierarchy recognition");

    ok &= requireContains(mainCpp, "constexpr float kPlaylistChildIndent = 22.0f",

                          "playlist children are indented enough to distinguish parent/child rows");

    ok &= requireContains(mainCpp, "float itemH = rowIsFolder ? kPlaylistFolderRowH : kPlaylistChildRowH",

                          "playlist row height must depend on folder vs child identity");

    ok &= requireContains(mainCpp, "playlistSelectedFocusRail",

                          "selected playlist row keeps a strong focus rail");

    ok &= requireContains(mainCpp, "playlistFolderIdentityRail",

                          "playlist folder rows keep a dedicated identity rail");

    ok &= requireContains(mainCpp, "playlistChildConnector",

                          "playlist child rows keep a connector line/dot to show nesting");

    ok &= requireContains(mainCpp, "playlistPendingCountToken",

                          "playlist pending count state is visually distinct from a real zero");

    ok &= requireNotContains(mainCpp, "Folder (N) child-count badge",

                             "playlist folders must not show a redundant child-count badge next to the file-count badge");

    ok &= requireNotContains(mainCpp, "chip.visibleChildCount > 0",

                             "playlist quick bar must not render the redundant folder child-count badge");

    ok &= requireContains(appStateH, "std::string audioOutputDeviceName",

                          "persisted audio output selection must keep a stable device-name guard");

    ok &= requireContains(mainCpp, "OutputDeviceIndexResetToDefault",

                          "legacy or stale numeric audio output indexes must fall back to system default");

    ok &= requireContains(mainCpp, "savedOutputDeviceName.empty()",

                          "startup must not trust legacy numeric-only audio output device indexes");

    ok &= requireContains(mainCpp, "state.settings.audioOutputDeviceName = devices[(size_t)i].name",

                          "new explicit audio device selections persist a stable device name");

    ok &= requireContains(mainCpp, "state.settings.audioOutputDeviceName.clear()",

                          "system-default audio output selection clears stale persisted device identity");

    ok &= requireContains(mainCpp, "WaveformChannelSoloCapsule",

                          "main waveform owns the quiet hover capsule solo affordance");

    ok &= requireContains(mainCpp, "##WaveformSoloCh%d",

                          "hybrid waveform exposes channel labels as quiet hover capsules");

    ok &= requireContains(mainCpp, "##MainWaveformSoloCh%d",

                          "main waveform exposes channel labels as quiet hover capsules");

    ok &= requireContains(mainCpp, "ResolveWaveformSoloSemanticChannels",

                          "waveform solo labels use a dedicated semantic channel resolver");

    ok &= requireContains(mainCpp, "selectedRecord.channels > 0",

                          "selected/current AudioRecord channel metadata overrides expanded display waveform channels");

    ok &= requireContains(mainCpp, "if (sourceChannels == 1) return 1",

                          "source metadata 1ch must force a true mono M label even if the display waveform is expanded to 2ch");

    ok &= requireContains(mainCpp, "const ImVec2 capsuleSize(28.0f, 14.0f)",

                          "waveform solo capsules stay flat and compact instead of bulky pills");

    ok &= requireContains(mainCpp, "WaveformSoloLaneLabel(ch, soloLabelChannels)",

                          "waveform solo labels support multi-channel names from the source file");

    ok &= requireContains(mainCpp, "state.audioEngine.setChannelSolo(channelSolo == laneSoloMode ? 0 : laneSoloMode)",

                          "supported waveform capsules toggle the existing channel solo state");

    ok &= requireContains(mainCpp, "const bool channelSoloEnabled = soloLabelChannels >= 2",

                          "mono files do not expose clickable channel solo");

    ok &= requireContains(mainCpp, "if (totalChannels <= 1) return \"M\"",

                          "mono files show one M lane label instead of fake L/R labels");

    ok &= requireContains(mainCpp, "Mono file: channel solo is unavailable",

                          "disabled mono M label explains why solo cannot be clicked");

    ok &= requireContains(mainCpp, "const bool chromeVisible = revealChrome || hovered || isActive || !enabled",

                          "waveform lane labels must keep a translucent contrast capsule when waveform strokes would obscure them");

    ok &= requireContains(mainCpp, "const ImU32 textHalo = ImGui::GetColorU32(withAlpha(ThemeColors::current().waveformBG, 0.82f))",

                          "waveform lane labels need a local text halo so bright waveform lines do not hide L/R");

    ok &= requireContains(mainCpp, "dl->AddCircleFilled(ImVec2(min.x + 6.0f, center.y)",

                          "active solo state is shown as a stable edge light without moving the label");

    ok &= requireContains(mainCpp, "Channel labels are drawn by the dedicated clickable solo overlay",

                          "static waveform labels are removed so L/R is not duplicated");

    ok &= requireContains(mainCpp, "state.audioEngine.setChannelSolo(0);",

                          "channel solo is not restored as a default startup state");

    ok &= requireContains(mainCpp, "items.push_back({\"channelSolo\", \"0\"})",

                          "saved settings self-heal old persisted solo state");

    ok &= requireNotContains(mainCpp, "Only L/R solo is supported now",

                             "multi-channel waveform labels must not claim L/R-only support");

    ok &= requireContains(mainCpp, "waveformHovered && !waveformSoloCapsuleHovered",

                          "waveform drag/seek handling is suppressed while the solo capsule is hovered");

    ok &= requireContains(mainCpp, "##HybridWaveformOverlay\", ImVec2(waveformWidth, waveformHeight), ImGuiButtonFlags_AllowOverlap",

                          "main waveform overlay allows the later capsule buttons to receive hover/click");

    ok &= requireNotContains(mainCpp, "Wave_HeightLabel",

                             "transport toolbar must not restore the removed waveform-height slider UI");

    ok &= requireNotContains(mainCpp, "m5InlineW += 100.0f",

                             "transport toolbar width math must not reserve space for the removed height slider");

    ok &= requireContains(mainCpp, "Region selections must not suppress ruler labels",

                          "waveform ruler timecodes remain visible inside selected regions");

    ok &= requireContains(mainCpp, "float selectionY0 = waveformPos.y + kMainWaveformRegionRulerH",

                          "main waveform region overlay starts below the ruler instead of covering timecode text");

    ok &= requireNotContains(mainCpp, "Hide ruler labels inside IN/OUT region",

                             "selected region must not hide ruler timecodes");

    ok &= requireNotContains(mainCpp, "SquareLedButton(\"##SoloL\"",

                             "transport toolbar must not keep the duplicate left LR solo button");

    ok &= requireNotContains(mainCpp, "SquareLedButton(\"##SoloR\"",

                             "transport toolbar must not keep the duplicate right LR solo button");

    ok &= requireContains(mainCpp, "ImDrawList* csDl = ImGui::GetWindowDrawList();",

                          "soundstage inline fan must render in the transport window layer so secondary popups stay above it");

    ok &= requireContains(mainCpp, "CoverWallQuietThumbSliderFloat(\"##ArtworkThumb\"",

                          "cover wall bottom thumb-size slider uses the thin quiet control instead of acrylic synth chrome");

    ok &= requireContains(mainCpp, "constexpr float kArtworkFaderTrackH = 14.0f",

                          "cover wall artwork slider hit area stays slim and flat");

    ok &= requireContains(mainCpp, "constexpr float kArtworkSliderStripH = 22.0f",

                          "cover wall artwork slider strip is thinner than the old bulky footer");

    ok &= requireNotContains(mainCpp, "ui::synth::AcrySliderFloat(\"##ArtworkThumb\"",

                             "cover wall artwork slider must not regress to the thick dotted acrylic fader");

    ok &= requireNotContains(mainCpp, "constexpr float kArtworkFaderCap = 16.0f",

                             "cover wall artwork slider must not keep the old large acrylic cap");

    ok &= requireContains(mainCpp, "shellDl->AddRectFilled(\n                    ImVec2(stripPos.x + 2.0f, stripPos.y + 3.0f)",

                          "cover wall slider footer keeps only a restrained flat backing");

    ok &= requireContains(audioEngineCpp, "static constexpr int kMaxAuditionChannelSolo = 8",

                          "AudioEngine supports common multichannel solo lanes, not just L/R");

    ok &= requireContains(audioEngineCpp, "case 6: return (ma_channel)MA_CHANNEL_SIDE_RIGHT",

                          "5.1 side-right channel maps to a real solo output channel");

    ok &= requireContains(audioEngineCpp, "ApplyChannelSoloMap(decoderConfig, soloMode, soloChannelMap)",

                          "processed exports use the same multichannel solo channel map");

    ok &= requireContains(audioEngineCpp, "if (filePath != m_fileInfo.filePath)",

                          "switching to a different file clears transient channel solo");

    ok &= requireNotContains(audioEngineCpp, "std::clamp(mode, 0, 2)",

                             "AudioEngine must not clamp waveform solo back to L/R only");

    ok &= requireContains(audioEngineH, "1..8=common channel layout solo",

                          "AudioEngine public solo contract documents multichannel support");



    if (!ok) return 1;

    std::cout << "PASS: desktop resize layout contract\n";

    return 0;

}

