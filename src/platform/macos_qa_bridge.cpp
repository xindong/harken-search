#include "macos_qa_bridge.h"
#include "AppState.h"
#include "AppMainServices.h"
#include "UIRenderer.h"
#include "frontend/AppStateAuditionFrontendAdapter.h"
#include "imgui.h"
#include "SearchQuery.h"
#include "UiTextBuffer.h"
#include "ThemeInspector.h"
#include "Utils.h"
#include "harken_user_data_layout.h"
#include "macos_file_drag.h"
#include "nlohmann_json.hpp"
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
#include "SettingsDialogState.h"
#include "imgui_internal.h"
#include <OpenGL/gl.h>
#endif
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdio>

// The production playlist sidebar uses this action to enter a list or folder.
void ActivatePlaylistView(AppState&, int64_t, bool, bool, int64_t);

namespace HarkenSearch::MacQa {
namespace {
using Json = nlohmann::json;
bool thesaurusPageRequested = false;
std::string browserTreeRootForQa;
Json lastRegionExport;
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
Json inputBeforeUi;
#endif
std::filesystem::path Root() {
    return std::filesystem::u8path(Platform::HarkenUserData::PreviewIsolationRootUtf8()) / "runtime";
}
void RequireQaPath(const std::string& path) {
    if (path.empty() || !Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(path))
        throw std::runtime_error("path_outside_isolated_root");
}
}
Request Poll(AppState& state) {
    Request request;
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
    static long long lastId = 0;
    const auto& io = ImGui::GetIO();
    const auto* context = ImGui::GetCurrentContext();
    inputBeforeUi = {{"mouse_x",io.MousePos.x},{"mouse_y",io.MousePos.y},
        {"wheel_y",io.MouseWheel},
        {"hovered_window",context->HoveredWindow ? context->HoveredWindow->Name : ""},
        {"active_id",context->ActiveId}};
    try {
        const auto path = Root() / "command.json";
        RequireQaPath(path.string());
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > 65536) return request;
        std::ifstream stream(path);
        const auto command = Json::parse(stream, nullptr, false);
        if (!command.is_object()) return request;
        const auto id = command.value("id", 0LL);
        if (id <= lastId || id > 1000000000) return request;
        request.id = lastId = id;
        lastRegionExport = nullptr;
        const auto action = command.value("action", std::string{});
        request.capture = command.value("capture", false);
        if (action == "create") {
            if (state.dbSwitchInProgress) throw std::runtime_error("library_switch_busy");
            const auto name = command.at("name").get<std::string>();
            if (name.empty() || name.size() > 100 || name.find_first_of("\\/:*?\"<>|") != std::string::npos)
                throw std::runtime_error("invalid_library_name");
            state.showNewDbDialog = true;
            state.qaNewDbName = name;
            state.qaCreateNewDbNow = true;
        } else if (action == "scan") {
            const auto path = command.at("path").get<std::string>(); RequireQaPath(path);
            if (!state.database.isOpen() || state.isScanning || state.scanCompletionPending)
                throw std::runtime_error("scan_not_ready");
            state.pendingScanFolders = {path};
            if (command.contains("worker_threads")) {
                const int threads = command.at("worker_threads").get<int>();
                if (threads < 1 || threads > 64) throw std::runtime_error("invalid_worker_count");
                state.scanWorkerThreads = threads;
            }
            state.scanDialogAutoStart = true;
            state.showScanDialog = true;
        } else if (action == "native_folder_drop" || action == "native_database_drop" ||
                   action == "native_audio_drop") {
            if (command.contains("paths"))
                request.dropPaths = command.at("paths").get<std::vector<std::string>>();
            else request.dropPaths = {command.at("path").get<std::string>()};
            if (request.dropPaths.empty() || request.dropPaths.size() > 16)
                throw std::runtime_error("invalid_drop_count");
            for (const auto& path : request.dropPaths) RequireQaPath(path);
            request.dropAttempted = true;
        } else if (action == "cancel_scan") {
            if (!state.isScanning.load()) throw std::runtime_error("scan_not_active");
            state.requestCancelScanTask();
        } else if (action == "open") {
            const auto path = command.at("path").get<std::string>(); RequireQaPath(path);
            if (!BeginSwitchDatabase(state, path)) throw std::runtime_error("library_switch_failed");
        } else if (action == "browser_folder") {
            const auto path = command.at("path").get<std::string>(); RequireQaPath(path);
            if (!std::filesystem::is_directory(std::filesystem::u8path(path)))
                throw std::runtime_error("browser_folder_missing");
            browserTreeRootForQa = path;
            state.viewMode = ViewMode::Browser;
            state.settings.activeSidebarSection = 1;
            state.settings.showFolderTree = true;
            state.fileBrowser.navigateTo(path);
            state.fileBrowser.setScope(BrowserScope::CurrentDir);
            std::snprintf(state.browserPathBuffer, sizeof(state.browserPathBuffer), "%s", path.c_str());
        } else if (action == "settings") {
            const float size = command.at("reading_font").get<float>();
            if (size < 12 || size > 24) throw std::runtime_error("invalid_font_size");
            state.settings.readingFontSize = size;
            MarkSettingsDirty(state);
        } else if (action == "show_settings") {
            state.showSettingsDialog = true;
        } else if (action == "settings_focus_wave") {
            auto& editor = harken::settings::SettingsEditor();
            if (!editor.open) throw std::runtime_error("settings_not_open");
            editor.page = harken::settings::Page::Appearance;
            editor.query[0] = 0;
            editor.focusId = "waveMode";
        } else if (action == "settings_focus_extension") {
            auto& editor = harken::settings::SettingsEditor();
            if (!editor.open) throw std::runtime_error("settings_not_open");
            editor.page = harken::settings::Page::Appearance;
            editor.query[0] = 0;
            editor.focusId = "extension";
        } else if (action == "settings_fail_save_once") {
            if (!harken::settings::SettingsEditor().open)
                throw std::runtime_error("settings_not_open");
            harken::settings::failSaveOnce = true;
        } else if (action == "pointer") {
            const float x = command.at("x").get<float>();
            const float y = command.at("y").get<float>();
            const auto phase = command.at("phase").get<std::string>();
            const auto size = ImGui::GetIO().DisplaySize;
            if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 ||
                x >= size.x || y >= size.y) throw std::runtime_error("pointer_out_of_window");
            if (phase != "hover" && phase != "down" && phase != "up")
                throw std::runtime_error("invalid_pointer_phase");
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(x, y);
            if (phase != "hover") io.AddMouseButtonEvent(ImGuiMouseButton_Left, phase == "down");
        } else if (action == "sidebar") {
            if (command.at("section").get<int>() != 2) throw std::runtime_error("invalid_sidebar_section");
            state.settings.activeSidebarSection = 2;
        } else if (action == "wheel") {
            const float delta = command.at("delta").get<float>();
            if (!std::isfinite(delta) || std::abs(delta) > 20.0f)
                throw std::runtime_error("invalid_wheel_delta");
            ImGui::GetIO().AddMouseWheelEvent(0.0f, delta);
        } else if (action == "text") {
            const auto input = command.at("text").get<std::string>();
            if (input.empty() || input.size() > 512) throw std::runtime_error("invalid_text_input");
            ImGui::GetIO().AddInputCharactersUTF8(input.c_str());
        } else if (action == "reset_thesaurus") {
            if (!ResetMacThesaurusToDefaults()) throw std::runtime_error("thesaurus_reset_failed");
        } else if (action == "show_thesaurus") {
            state.showSettingsDialog = true;
            thesaurusPageRequested = true;
        } else if (action == "filter") {
            if (!state.database.isOpen()) throw std::runtime_error("library_not_open");
            if (command.contains("text")) CopyUiText(state.searchBuffer, command.at("text").get<std::string>().c_str());
            if (command.contains("folder")) {
                const auto path = command.at("folder").get<std::string>();
                if (!path.empty()) RequireQaPath(path);
                state.selectedFolderPath = path; state.folderFilterActive = !path.empty();
            }
            if (command.contains("cover_mode")) {
                const int mode = command.at("cover_mode").get<int>();
                if (mode < 0 || mode > 2) throw std::runtime_error("invalid_cover_mode");
                state.coverFilterIndex = mode;
            }
            ApplyFiltersAndRefresh(state);
        } else if (action == "playlist_create") {
            if (!state.database.isOpen()) throw std::runtime_error("library_not_open");
            const auto name = command.at("name").get<std::string>();
            if (name.empty() || name.size() >= sizeof(state.playlistNameBuffer))
                throw std::runtime_error("invalid_playlist_name");
            const bool folder = command.value("folder", false);
            const int64_t parentId = command.value("parent_id", int64_t{-1});
            if (folder && parentId > 0) throw std::runtime_error("nested_folder_not_supported");
            state.playlists = state.database.getAllPlaylists();
            for (const auto& playlist : state.playlists) {
                if (playlist.name == name) throw std::runtime_error("duplicate_playlist_name");
            }
            const int64_t id = parentId > 0
                ? state.database.createSubPlaylist(parentId, name)
                : state.database.createPlaylist(name);
            if (id <= 0 || (folder && !state.database.markAsFolder(id)))
                throw std::runtime_error("playlist_create_failed");
            state.playlists = state.database.getAllPlaylists();
        } else if (action == "playlist_add") {
            if (!state.database.isOpen()) throw std::runtime_error("library_not_open");
            const auto path = command.at("path").get<std::string>(); RequireQaPath(path);
            const int64_t id = command.at("playlist_id").get<int64_t>();
            const auto row = state.database.getFileByPath(path);
            if (id <= 0 || row.id <= 0 || !state.database.addToPlaylist(id, row.id))
                throw std::runtime_error("playlist_add_failed");
            state.playlists = state.database.getAllPlaylists();
            if (state.viewMode == ViewMode::Playlist) RefreshSearchResults(state, true);
        } else if (action == "playlist_set_parent") {
            if (!state.database.isOpen()) throw std::runtime_error("library_not_open");
            const int64_t id = command.at("playlist_id").get<int64_t>();
            const int64_t parentId = command.at("parent_id").get<int64_t>();
            const bool changed = state.database.setPlaylistParent(id, parentId);
            if (changed == command.value("expect_rejected", false))
                throw std::runtime_error("playlist_parent_unexpected_result");
            state.playlists = state.database.getAllPlaylists();
        } else if (action == "playlist_view") {
            if (!state.database.isOpen()) throw std::runtime_error("library_not_open");
            const int64_t id = command.at("playlist_id").get<int64_t>();
            state.playlists = state.database.getAllPlaylists();
            const auto found = std::find_if(state.playlists.begin(), state.playlists.end(),
                [id](const Playlist& playlist) { return playlist.id == id; });
            if (found == state.playlists.end()) throw std::runtime_error("playlist_not_found");
            ActivatePlaylistView(state, id, found->is_folder, true, -1);
        } else if (action == "playlist_library") {
            if (!state.database.isOpen()) throw std::runtime_error("library_not_open");
            state.selectedPlaylistId = -1;
            state.playlistAggregatedView = false;
            state.viewMode = ViewMode::Database;
            RefreshSearchResults(state, true);
        } else if (action == "tag") {
            const auto name = command.at("file_name").get<std::string>();
            const auto tags = command.at("tags").get<std::vector<std::string>>();
            bool found = false;
            for (const auto& row : state.searchResults) if (row.fileName == name) {
                RequireQaPath(row.filePath);
                if (!state.database.setFileTags(row.id, tags)) throw std::runtime_error("tag_write_failed");
                found = true; break;
            }
            if (!found) throw std::runtime_error("row_not_found");
        } else if (action == "metadata_patch") {
            if (!state.database.isOpen()) throw std::runtime_error("library_not_open");
            const auto path = command.at("path").get<std::string>(); RequireQaPath(path);
            const auto row = state.database.getFileByPath(path);
            if (row.id <= 0) throw std::runtime_error("row_not_found");
            if (!std::filesystem::is_regular_file(std::filesystem::u8path(path)))
                throw std::runtime_error("source_file_missing");
            const auto& fields = command.at("fields");
            if (!fields.is_object() || fields.empty() || fields.size() > 3)
                throw std::runtime_error("invalid_metadata_fields");
            MetadataEditPatch patch;
            for (auto it = fields.begin(); it != fields.end(); ++it) {
                if (it.key() != "description" && it.key() != "keywords" && it.key() != "category")
                    throw std::runtime_error("unsupported_metadata_field");
                const auto value = it.value().get<std::string>();
                if (value.size() > 4096) throw std::runtime_error("metadata_value_too_long");
                patch.set(it.key(), value);
            }
            const auto result = state.database.applyMetadataPatchBatch({row.id}, patch);
            if (result.success != 1 || result.failed != 0)
                throw std::runtime_error("metadata_patch_failed");
            state.database.invalidateFacetQueryCache();
            RefreshSearchResults(state, false);
        } else if (action == "info_metadata_tab") {
            state.settings.infoPages.selected = 0;
            MarkSettingsDirty(state);
        } else if (action == "select") {
            const int index = command.at("index").get<int>();
            if (index < 0 || index >= static_cast<int>(state.searchResults.size()) || state.dbSwitchInProgress)
                throw std::runtime_error("invalid_selection");
            const auto& row = state.searchResults[index]; RequireQaPath(row.filePath);
            // Same selection boundary and deferred load used by keyboard browsing.
            state.audioEngine.stop();
            state.selectedIndicesSet.clear(); state.selectedIndicesSet.insert(index);
            state.beginAudioTargetFromSearchResult(index);
            state.lastClickedIndex = index; state.scrollToSelected = true;
            state.lastSelectionOrLoadTime = ImGui::GetTime();
            const bool play = command.value("play_when_ready", false);
            state.audioEngine.loadFileAsync(row.filePath, play);
            ResetWaveformZoom(state);
            ScheduleDeferredWaveformDetail(state, row.filePath, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
            if (play) MarkFilePlayed(state, row.filePath, row.id);
            state.syncToTab();
        } else if (action == "outbound_drag_probe") {
            const auto path = command.at("path").get<std::string>();
            RequireQaPath(path);
            if (!state.database.isOpen() || !state.database.fileExists(path))
                throw std::runtime_error("audio_not_in_library");
            StartAudioFileDrag(state, path);
        } else if (action == "outbound_region_drag_probe") {
            if (!state.audioEngine.isLoaded()) throw std::runtime_error("audio_not_loaded");
            RequireQaPath(state.audioEngine.getLoadedFilePath());
            StartRegionDrag(state.audioEngine);
        } else if (action == "outbound_drag_cancel") {
            Platform::MacFileDrag::CancelQueuedFileDrag();
        } else if (action == "toggle_playback") {
            aris::frontend::AppStateAuditionFrontendAdapter(state).toggleVisibleFooterPlayback();
        } else if (action == "seek") {
            const float position = command.at("position").get<float>();
            if (!std::isfinite(position) || position < 0 || position > 1) throw std::runtime_error("invalid_seek");
            state.audioEngine.seekToPosition(position);
        } else if (action == "volume") {
            const float volume = command.at("value").get<float>();
            if (!std::isfinite(volume) || volume < 0 || volume > 1) throw std::runtime_error("invalid_volume");
            aris::frontend::AppStateAuditionFrontendAdapter(state).setVolume(volume);
        } else if (action == "region_set") {
            if (!state.audioEngine.isLoaded()) throw std::runtime_error("audio_not_loaded");
            RequireQaPath(state.audioEngine.getLoadedFilePath());
            const float start = command.at("start").get<float>();
            const float end = command.at("end").get<float>();
            const float fadeIn = command.value("fade_in", 0.0f);
            const float fadeOut = command.value("fade_out", 0.0f);
            if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end > 1 ||
                end - start < 0.01f || !std::isfinite(fadeIn) || !std::isfinite(fadeOut) ||
                fadeIn < 0 || fadeOut < 0 || fadeIn > 1 || fadeOut > 1 || fadeIn + fadeOut > 1)
                throw std::runtime_error("invalid_region");
            state.audioEngine.setOutPoint(end);
            state.audioEngine.setInPoint(start);
            state.uiState.regionFadeInNorm = fadeIn;
            state.uiState.regionFadeOutNorm = fadeOut;
            state.audioEngine.setRegionFadeNorms(fadeIn, fadeOut);
        } else if (action == "region_export") {
            const auto path = command.at("path").get<std::string>(); RequireQaPath(path);
            if (!state.audioEngine.isLoaded()) throw std::runtime_error("audio_not_loaded");
            RequireQaPath(state.audioEngine.getLoadedFilePath());
            ExportRegionInfo info;
            const auto snapshot = state.audioEngine.captureTransferProcessingSnapshot();
            const bool ok = state.audioEngine.exportRegion(path, state.uiState.regionFadeInNorm,
                state.uiState.regionFadeOutNorm, &info, &snapshot);
            lastRegionExport = {{"ok", ok}, {"path", path},
                {"error", state.audioEngine.getLastExportError()},
                {"start_frame", info.regionStartFrame}, {"frames_written", info.framesWritten},
                {"total_frames", info.totalFrames}, {"sample_rate", info.sampleRate},
                {"channels", info.channels}};
        } else if (action == "stop") state.audioEngine.stop();
        else if (action == "close") request.close = true;
        else if (action != "snapshot") throw std::runtime_error("unknown_action");
    } catch (const std::exception& error) { request.error = error.what(); }
#endif
    return request;
}
bool ConsumeThesaurusPageRequest() {
    const bool requested = thesaurusPageRequested;
    thesaurusPageRequested = false;
    return requested;
}
const std::string& BrowserTreeRootForQa() {
    return browserTreeRootForQa;
}
bool Reply(const AppState& state, const Request& request, bool captured) {
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
    if (!request.id) return true;
    try {
        Json rows = Json::array();
        for (const auto& row : state.searchResults) {
            if (rows.size() >= 100) break;
            const auto mini = state.waveformCache.find(row.filePath);
            const size_t miniPeaks = mini == state.waveformCache.end() ? 0 : mini->second.peaks.size();
            rows.push_back({{"id",row.id},{"name",row.fileName},{"path",row.filePath},
                {"description",row.description},{"keywords",row.keywords},{"category",row.category},
                {"duration",row.durationSeconds},{"sample_rate",row.sampleRate},{"channels",row.channels},
                {"mini_peaks",miniPeaks},
                {"folder_cover",row.folderCoverKey},{"embedded_cover",row.embeddedCoverKey}});
        }
        Json playlists = Json::array();
        for (const auto& playlist : state.playlists) {
            playlists.push_back({{"id",playlist.id},{"name",playlist.name},
                {"parent_id",playlist.parent_id},{"folder",playlist.is_folder},
                {"file_count",playlist.fileCount}});
        }
        const auto scan = state.getScanWorkflowSnapshot();
        const auto wave = state.audioEngine.getWaveformData();
        const auto outputProbe = state.audioEngine.getQaOutputProbe();
        float maximum = 0;
        if (wave) for (const float peak : wave->peaks) maximum = std::max(maximum, std::abs(peak));
        const auto& observed = harken::settings::observation;
        const auto& editor = harken::settings::SettingsEditor();
        Json settingsItems = Json::array();
        for (const auto& item : observed.items) {
            settingsItems.push_back({{"id",item.id},
                {"bounds",{item.bounds.x,item.bounds.y,item.bounds.z,item.bounds.w}},
                {"clip",{item.clip.x,item.clip.y,item.clip.z,item.clip.w}},
                {"disabled",item.disabled}});
        }
        Json filterRects = Json::object();
        for (const auto& [name, rect] : state.qaFilterUiRects)
            filterRects[name] = {rect.x, rect.y, rect.z, rect.w};
        Json scanButtonRects = Json::object();
        for (const auto& zone : ThemeInspector::CopyLatestSnapshot().zones) {
            if (zone.key == "scan.dialog.start" || zone.key == "scan.dialog.cancel")
                scanButtonRects[zone.key] = {zone.minX, zone.minY, zone.maxX, zone.maxY};
        }
        const auto display = ImGui::GetIO().DisplaySize;
        Json textureProbe;
        if (request.capture && state.spectrogramTexture != 0 &&
            state.spectrogramWidth > 0 && state.spectrogramHeight > 0) {
            const int width = state.spectrogramWidth, height = state.spectrogramHeight;
            std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
            GLint previousTexture = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
            glBindTexture(GL_TEXTURE_2D, state.spectrogramTexture);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
            int brightLeft = 0, brightRight = 0;
            for (int y = 0; y < height; y += 8) for (int x = 0; x < width; x += 4) {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                const bool bright = std::max({pixels[offset],pixels[offset + 1],pixels[offset + 2]}) > 40;
                if (bright) (x < width / 2 ? brightLeft : brightRight)++;
            }
            textureProbe = {{"bright_left",brightLeft},{"bright_right",brightRight}};
        }
        Json spectrumCommands = Json::array();
        if (const auto* draw = ImGui::GetDrawData(); state.spectrogramTexture != 0 && draw) {
            for (int list = 0; list < draw->CmdListsCount; ++list) {
                for (const auto& command : draw->CmdLists[list]->CmdBuffer) {
                    if (static_cast<GLuint>(command.GetTexID()) == state.spectrogramTexture &&
                        command.ElemCount > 0)
                        spectrumCommands.push_back({{"elements",command.ElemCount},
                            {"clip",{command.ClipRect.x,command.ClipRect.y,
                                command.ClipRect.z,command.ClipRect.w}}});
                }
            }
        }
        Json value = {{"id",request.id},{"error",request.error},{"captured",captured},
            {"settings_ui",{{"frame",observed.frame},{"open",observed.open},
                {"page",observed.page},{"pending",observed.pending},{"prompt",observed.prompt},
                {"live_wave_mode",state.settings.waveformDisplayMode},
                {"draft_wave_mode",editor.app.Value().waveformDisplayMode},
                {"live_extension",state.settings.showFileExtension},
                {"draft_extension",editor.app.Value().showFileExtension},
                {"live_wave_style",state.settings.waveformStyle},
                {"draft_wave_style",editor.app.Value().waveformStyle},
                {"live_spectrum",state.showSpectrogram},
                {"draft_spectrum",editor.runtime.Value().spectrum},
                {"save_error",editor.error},
                {"items",settingsItems}}},
            {"wave_ui",{{"frame",ImGui::GetFrameCount()},
                {"wave_draw_frame",state.qaRegions.waveDrawFrame},
                {"wave_draw_style",state.qaRegions.waveDrawStyle},
                {"spectrum_draw_frame",state.qaRegions.spectrumDrawFrame},
                {"spectrum_texture",state.spectrogramTexture},
                {"spectrum_width",state.spectrogramWidth},
                {"spectrum_height",state.spectrogramHeight},
                {"spectrum_file",state.spectrogramFilePath},
                {"spectrum_timeline_frames",state.spectrogramTimelineFrames},
                {"spectrum_sample_rate",state.spectrogramSampleRate},
                {"spectrum_needs_update",state.spectrogramNeedsUpdate},
                {"spectrum_split",state.spectrogramSplit},
                {"spectrum_blend",state.spectrogramBlend},
                {"spectrum_pixels",textureProbe},
                {"spectrum_commands",spectrumCommands}}},
            {"list_ui",{{"frame",ImGui::GetFrameCount()},
                {"mini_texture_paths",request.capture ? MiniTexturePaths() : std::vector<std::string>{}},
                {"draw_frame",state.qaRegions.listTextFrame},
                {"input_before_ui",inputBeforeUi},
                {"settle_frames",state.dbSwitchSettleFramesRemaining},
                {"warmup_frames",state.warmupFramesRemaining},
                {"saved_scroll_y",state.listTabs.empty() ? 0.0f :
                    state.listTabs[std::clamp(state.activeTabIndex, 0, (int)state.listTabs.size()-1)].listScrollY},
                {"restore_scroll_y",!state.listTabs.empty() &&
                    state.listTabs[std::clamp(state.activeTabIndex, 0, (int)state.listTabs.size()-1)].restoreScrollY},
                {"raw_names",state.qaRegions.listRawNames},
                {"rendered_names",state.qaRegions.listRenderedNames}}},
            {"audio",{{"loaded",state.audioEngine.isLoaded()},{"pending",state.audioEngine.isLoadFilePending()},
                {"playing",state.audioEngine.isPlaying()},{"paused",state.audioEngine.isPaused()},
                {"path",state.audioEngine.getLoadedFilePath()},{"position",state.audioEngine.getPlaybackPosition()},
                {"seconds",state.audioEngine.getCurrentTimeSeconds()},{"sample_rate",state.audioEngine.getEngineSampleRate()},
                {"device",state.audioEngine.getCurrentOutputDeviceName()},{"error",state.loadErrorToast},
                {"output",{{"callbacks",outputProbe.callbacks},{"frames",outputProbe.frames},
                    {"nonzero_samples",outputProbe.nonzeroSamples},{"max_amplitude",outputProbe.maxAmplitude},
                    {"channels",outputProbe.channels},{"sample_rate",outputProbe.sampleRate},
                    {"device_started",outputProbe.deviceStarted},{"backend",outputProbe.backend},
                    {"device_name",outputProbe.deviceName}}},
                {"selected_id",state.currentFileId},{"wave_ready",wave && wave->isReady},
                {"wave_peaks",wave ? wave->peaks.size() : 0},{"wave_max",maximum},
                {"wave_pending",state.audioEngine.isWaveformDetailPending()}}},
            {"database",state.currentDbPathUtf8},{"database_open",state.database.isOpen()},
            {"database_error",state.database.getLastError()},
            {"switch_pending",state.dbSwitchInProgress},{"switch_error",state.dbSwitchError},
            {"needs_database",state.startupNeedsDatabaseSetup},
            {"reading_font",state.settings.readingFontSize},
            {"info_page_selected",state.settings.infoPages.selected},
            {"region",{{"start",state.audioEngine.getInPoint()},{"end",state.audioEngine.getOutPoint()},
                {"fade_in",state.uiState.regionFadeInNorm},{"fade_out",state.uiState.regionFadeOutNorm}}},
            {"region_export",lastRegionExport},
            {"outbound_drag_pending",Platform::MacFileDrag::PendingFileCount()},
            {"search_pending",state.asyncSearchInProgress},{"count",state.totalFileCount},{"rows",rows},
            {"window",{{"width",display.x},{"height",display.y}}},
            {"active_sidebar_section",state.settings.activeSidebarSection},
            {"qa_filter_ui_rects",filterRects},
            {"metadata_filters",{{"artist",state.artistFilterBuffer},
                {"album",state.albumFilterBuffer},{"category",state.categoryFilterBuffer}}},
            {"metadata_input_text",state.qaMetadataInputText},
            {"metadata_draft_query",state.qaMetadataDraftQuery},
            {"playlists",playlists},{"view_mode",static_cast<int>(state.viewMode)},
            {"selected_playlist_id",state.selectedPlaylistId},
            {"playlist_aggregated",state.playlistAggregatedView},
            {"filters",{{"text",state.searchBuffer},{"folder",state.selectedFolderPath},
                {"folder_active",state.folderFilterActive},{"cover_mode",state.coverFilterIndex}}},
            {"native_drop",{{"attempted",request.dropAttempted},{"accepted",request.dropAccepted}}},
            {"drop_notice",{{"shown",state.showScanDropNotice},
                {"imported",state.scanDropImportedCount},{"existing",state.scanDropExistingCount},
                {"failed",state.scanDropFailedCount},{"summary",state.lastDropSummary}}},
            {"scan_dialog",{{"frame",ImGui::GetFrameCount()},
                {"open",scanButtonRects.contains("scan.dialog.start")},
                {"auto_start",state.scanDialogAutoStart},{"pending_folders",state.pendingScanFolders},
                {"busy_notice",state.showScanBusyNotice},{"ignored_count",state.scanIgnoredDropCount},
                {"buttons",scanButtonRects}}},
            {"scan",{{"active",state.isScanning.load()},{"pending",state.scanCompletionPending.load()},
                {"stage",static_cast<int>(scan.stage)},{"generation",state.scanTaskGeneration.load()},{"discovered",scan.discovered},
                {"processed",scan.processed},{"succeeded",scan.succeeded},{"failed",scan.failed},
                {"skipped",scan.skipped},{"duration_ms",scan.durationMs},
                {"elapsed_ms",state.scanUiMetrics.Value().elapsedMs},
                {"speed_known",state.scanUiMetrics.Value().speedKnown},
                {"files_per_second",state.scanUiMetrics.Value().filesPerSecond},
                {"refresh_pending",state.needsDatabaseRefresh},{"errors",const_cast<AppState&>(state).getScanErrors()}}}};
        const auto path = Root() / "response.json"; RequireQaPath(path.string());
        const auto temporary = Root() / "response.tmp";
        std::ofstream output(temporary); output << value.dump(2) << '\n'; output.close();
        if (!output) return false;
        std::filesystem::rename(temporary, path);
        return true;
    } catch (...) { return false; }
#endif
    return false;
}
}
