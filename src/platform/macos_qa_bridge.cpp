#include "macos_qa_bridge.h"
#include "AppState.h"
#include "AppMainServices.h"
#include "UIRenderer.h"
#include "SimilarSoundsWindow.h"
#include "frontend/AppStateAuditionFrontendAdapter.h"
#include "imgui.h"
#include "SearchQuery.h"
#include "UiTextBuffer.h"
#include "Utils.h"
#include "harken_user_data_layout.h"
#include "nlohmann_json.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace HarkenSearch::MacQa {
namespace {
using Json = nlohmann::json;
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
        } else if (action == "cancel_scan") {
            if (!state.isScanning.load()) throw std::runtime_error("scan_not_active");
            state.requestCancelScanTask();
        } else if (action == "open") {
            const auto path = command.at("path").get<std::string>(); RequireQaPath(path);
            if (!BeginSwitchDatabase(state, path)) throw std::runtime_error("library_switch_failed");
        } else if (action == "open_similar") {
            if (!state.database.isOpen() || state.dbSwitchInProgress)
                throw std::runtime_error("library_not_ready");
            aura::ui::OpenSimilarSoundsWindow(state);
        } else if (action == "settings") {
            const float size = command.at("reading_font").get<float>();
            if (size < 12 || size > 24) throw std::runtime_error("invalid_font_size");
            state.settings.readingFontSize = size;
            MarkSettingsDirty(state);
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
        } else if (action == "stop") state.audioEngine.stop();
        else if (action == "close") request.close = true;
        else if (action != "snapshot") throw std::runtime_error("unknown_action");
    } catch (const std::exception& error) { request.error = error.what(); }
#endif
    return request;
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
                {"duration",row.durationSeconds},{"sample_rate",row.sampleRate},{"channels",row.channels},
                {"mini_peaks",miniPeaks},
                {"folder_cover",row.folderCoverKey},{"embedded_cover",row.embeddedCoverKey}});
        }
        const auto scan = state.getScanWorkflowSnapshot();
        const auto wave = state.audioEngine.getWaveformData();
        const auto outputProbe = state.audioEngine.getQaOutputProbe();
        float maximum = 0;
        if (wave) for (const float peak : wave->peaks) maximum = std::max(maximum, std::abs(peak));
        Json value = {{"id",request.id},{"error",request.error},{"captured",captured},
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
            {"similar_window",{{"visible",state.panelSimilarSounds.visible},
                {"floating",state.panelSimilarSounds.floating}}},
            {"switch_pending",state.dbSwitchInProgress},{"switch_error",state.dbSwitchError},
            {"needs_database",state.startupNeedsDatabaseSetup},
            {"reading_font",state.settings.readingFontSize},
            {"search_pending",state.asyncSearchInProgress},{"count",state.totalFileCount},{"rows",rows},
            {"filters",{{"text",state.searchBuffer},{"folder",state.selectedFolderPath},
                {"folder_active",state.folderFilterActive},{"cover_mode",state.coverFilterIndex}}},
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
