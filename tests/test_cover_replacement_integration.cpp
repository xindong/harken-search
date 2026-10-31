#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        return false;
    }
    return true;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

int main() {
    const auto repoRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
    const std::string betaFlags = ReadText(repoRoot / "src" / "BetaFeatureFlags.h");
    const std::string appState = ReadText(repoRoot / "src" / "AppState.h");
    const std::string mainSource = ReadText(repoRoot / "src" / "main.cpp");
    const std::string browsePreviewRefresh = ReadText(
        repoRoot / "src" / "AsyncSearchBrowsePreviewRefreshWorkflow.cpp");
    const std::string browsePreviewRefreshAdapter = ReadText(
        repoRoot / "src" / "AsyncSearchBrowsePreviewRefreshStateAdapter.cpp");
    const std::string windowsMain = ReadText(repoRoot / "src" / "WindowsMain.cpp");
    const std::string qaDriver = ReadText(repoRoot / ".openclaw" / "harness" / "qa_driver.py");
    const std::string probeScript = ReadText(repoRoot / "scripts" / "qa_probe_cover_browse_surface.py");
    const std::string integrationProbe =
        ReadText(repoRoot / "scripts" / "qa_probe_cover_replacement_integration.py");
    const std::string retirementProbe =
        ReadText(repoRoot / "scripts" / "qa_probe_cover_replacement_retirement.py");

    if (!Check(!betaFlags.empty(),
               "BetaFeatureFlags.h should be readable for the integrated-mode default contract")) {
        return 1;
    }
    if (!Check(!appState.empty(),
               "AppState.h should be readable for the integrated-mode persistence contract")) {
        return 1;
    }
    if (!Check(!mainSource.empty() && !windowsMain.empty(),
               "main.cpp and WindowsMain.cpp should be readable for the integrated-mode routing contract")) {
        return 1;
    }

    if (!Check(betaFlags.find("kEnableReplacementCoverWallIntegratedModeByDefault = true") != std::string::npos,
               "replacement cover integrated mode should default on through BetaFeatureFlags")) {
        return 1;
    }
    if (!Check(appState.find("bool coverWallIntegratedMode = true;") != std::string::npos,
               "AppSettings should persist a coverWallIntegratedMode field")) {
        return 1;
    }
    if (!Check(mainSource.find("kv[\"coverWallIntegratedMode\"]") != std::string::npos &&
                   mainSource.find("{\"coverWallIntegratedMode\", state.settings.coverWallIntegratedMode ? \"1\" : \"0\"}") != std::string::npos,
               "coverWallIntegratedMode should load from and save to settings.ini")) {
        return 1;
    }
    if (!Check(
            mainSource.find("UseReplacementCoverWallIntegratedMode,") !=
                    std::string::npos &&
                browsePreviewRefreshAdapter.find(
                    "readIntegratedMode_(state_)") != std::string::npos &&
                browsePreviewRefresh.find("state.integratedMode") !=
                    std::string::npos,
               "preview restore should pass through the replacement integrated-mode gate")) {
        return 1;
    }
    if (!Check(mainSource.find("UseReplacementCoverWallIntegratedMode(state) &&") != std::string::npos &&
                   mainSource.find("cw.pinnedCoverFilterItem.key == state.coverFilterPath") != std::string::npos &&
                   mainSource.find("coverFilterPinned.empty()") != std::string::npos,
               "replacement-only pinned cover semantics should be gated in async refresh and wall rendering")) {
        return 1;
    }
    if (!Check(mainSource.find("state.coverWall.pinnedCoverFilterItem = item;") != std::string::npos &&
                   mainSource.find("state.coverWall.pinnedCoverFilterItem = CoverModule::CoverItem{};") != std::string::npos,
               "cover click path should either snapshot or clear replacement-only pinned state")) {
        return 1;
    }
    if (!Check(windowsMain.find("\\\"integrated_mode\\\":") != std::string::npos &&
                   windowsMain.find("cmd == \"set_cover_wall_integrated_mode\"") != std::string::npos,
               "QA state and command surface should expose the integrated-mode toggle")) {
        return 1;
    }
    if (!Check(windowsMain.find("\\\"cover_rebuild\\\":") != std::string::npos &&
                   windowsMain.find("cmd == \"start_cover_rebuild\"") != std::string::npos &&
                   windowsMain.find("cmd == \"stop_cover_rebuild\"") != std::string::npos,
               "QA state and command surface should expose cover rebuild control for retirement recovery proofs")) {
        return 1;
    }
    if (!Check(windowsMain.find("replacement integrated mode is disabled") != std::string::npos,
               "specific cover filter QA command should fail safely when integrated mode is disabled")) {
        return 1;
    }
    // The public desktop repository deliberately excludes the private harness.
    // Keep its checks when present, and still run every product routing check.
    if (qaDriver.empty()) {
        if (!Check(!std::filesystem::exists(repoRoot / ".openclaw"),
                   "an existing private harness must not be incomplete")) return 1;
        std::printf("NOT APPLICABLE: private harness API/CLI checks; .openclaw is not distributed\n");
    } else {
    if (!Check(qaDriver.find("def set_cover_wall_integrated_mode") != std::string::npos &&
                   qaDriver.find("sub.add_parser(\"set_cover_wall_integrated_mode\")") != std::string::npos,
               "QA driver should expose the integrated-mode command in API and CLI forms")) {
        return 1;
    }
    if (!Check(qaDriver.find("def start_cover_rebuild") != std::string::npos &&
                   qaDriver.find("def stop_cover_rebuild") != std::string::npos &&
                   qaDriver.find("def wait_cover_rebuild_complete") != std::string::npos &&
                   qaDriver.find("sub.add_parser(\"start_cover_rebuild\")") != std::string::npos &&
                   qaDriver.find("sub.add_parser(\"stop_cover_rebuild\")") != std::string::npos &&
                   qaDriver.find("sub.add_parser(\"wait_cover_rebuild_complete\")") != std::string::npos,
               "QA driver should expose cover rebuild control and wait helpers for retirement probes")) {
        return 1;
    }
    }
    if (!Check(probeScript.find("ensure_integrated_mode") != std::string::npos &&
                   probeScript.find("coverWallIntegratedMode") != std::string::npos &&
                   probeScript.find("browseSurfaceIntegratedMode") != std::string::npos,
               "replacement browse-surface probe should gain an integrated-mode evidence helper")) {
        return 1;
    }
    if (!Check(!integrationProbe.empty(),
               "cover replacement integration probe should exist as a durable Slice 4 runtime evidence entrypoint")) {
        return 1;
    }
    if (!Check(integrationProbe.find("PROOF-COVER-STARTUP-INTEGRATED") != std::string::npos &&
                   integrationProbe.find("PROOF-COVER-NOFILTER-INTEGRATED") != std::string::npos &&
                   integrationProbe.find("PROOF-COVER-KEYWORD-INTEGRATED") != std::string::npos &&
                   integrationProbe.find("PROOF-COVER-PATH-INTEGRATED") != std::string::npos &&
                   integrationProbe.find("PROOF-COVER-ROLLBACK-MODE-OFF") != std::string::npos,
               "Slice 4 integration probe should bind startup/no-filter/keyword/path/rollback proof IDs into one report")) {
        return 1;
    }
    if (!Check(integrationProbe.find("ensure_integrated_mode(driver, enabled=False") != std::string::npos &&
                   integrationProbe.find("set_specific_cover_filter") != std::string::npos &&
                   integrationProbe.find("coverWallIntegratedMode") != std::string::npos,
               "Slice 4 integration probe should exercise mode-off rollback and verify the QA-integrated mode contract")) {
        return 1;
    }
    if (!Check(!retirementProbe.empty(),
               "cover replacement retirement probe should exist as a durable Slice 5 runtime evidence entrypoint")) {
        return 1;
    }
    if (!Check(retirementProbe.find("PROOF-COVER-RETIRE-NOFILTER-PRIME-REPEAT") != std::string::npos &&
                   retirementProbe.find("PROOF-COVER-RETIRE-KEYWORD-PRIME-REPEAT") != std::string::npos &&
                   retirementProbe.find("PROOF-COVER-RETIRE-PATH-PRIME-REPEAT") != std::string::npos &&
                   retirementProbe.find("PROOF-COVER-RETIRE-FILTER") != std::string::npos &&
                   retirementProbe.find("PROOF-COVER-RETIRE-PREVIEW-PARITY") != std::string::npos &&
                   retirementProbe.find("PROOF-COVER-RETIRE-ROLLBACK") != std::string::npos &&
                   retirementProbe.find("PROOF-COVER-RETIRE-REBUILD") != std::string::npos &&
                   retirementProbe.find("PROOF-COVER-RETIRE-LEGACY-FALLBACK-AUDIT") != std::string::npos,
               "Slice 5 retirement probe should bind the retirement proof matrix into one durable report")) {
        return 1;
    }
    if (!Check(retirementProbe.find("run_cover_rebuild_proof_from_report") != std::string::npos &&
                   retirementProbe.find("qa_probe_cover_rebuild_8082") != std::string::npos &&
                   retirementProbe.find("--rebuild-proof-report") != std::string::npos,
               "Slice 5 retirement probe should be able to bind the standalone 8082 real-import rebuild proof into the retirement package")) {
        return 1;
    }
    if (!Check(retirementProbe.find("driver.start_cover_rebuild(") != std::string::npos &&
                   retirementProbe.find("driver.wait_cover_rebuild_complete(") != std::string::npos &&
                   retirementProbe.find("search_fallback") != std::string::npos,
               "Slice 5 retirement probe should exercise rebuild recovery and audit legacy SearchFallback usage")) {
        return 1;
    }

    std::printf("PASS: cover replacement integrated-mode contract holds\n");
    return 0;
}
