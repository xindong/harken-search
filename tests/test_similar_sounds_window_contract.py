from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
widgets = (ROOT / "src" / "HarkenSearchWidgets.cpp").read_text(encoding="utf-8")
header = (ROOT / "src" / "HarkenSearchWidgets.h").read_text(encoding="utf-8")

start = widgets.index("bool BeginStandaloneWindow")
end = widgets.index("void EndStandaloneWindow", start)
body = widgets[start:end]

assert "ThemeColors::current()" in body
assert "ImGui::SetNextWindowSize(" in body
assert "ImGui::SetNextWindowSizeConstraints(" in body
assert "StandaloneWindowAction::Minimize" in body
assert "StandaloneWindowAction::ToggleMaximize" in body
assert "StandaloneWindowAction::Close" in body
assert "ImGuiWindowFlags_NoSavedSettings" not in body
assert "0x" not in body and "IM_COL32(" not in body

# Normal and maximized windows need separate ini identities so the viewport
# work rectangle never overwrites the user's restored geometry.
assert '"__Maximized"' in body
assert '"StandaloneWindow"' not in body
assert "normalizedId" in body
assert "ImGui::PushID(normalizedId.c_str())" in body

# Drag/maximize/restore geometry is queued before the following Begin. No
# geometry mutation is allowed after drawing for the current frame starts.
assert "bool requestMove = false;" in header
begin_call = body.index("ImGui::Begin(")
pre_begin = body[:begin_call]
post_begin = body[begin_call:]
assert "state->requestMove" in pre_begin
assert "ImGui::SetWindowPos(" not in post_begin
assert "ImGui::SetWindowSize(" not in post_begin
assert "ImGui::SetNextWindowPos(" not in post_begin
assert "ImGui::SetNextWindowSize(" not in post_begin

main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
app_state = (ROOT / "src" / "AppState.h").read_text(encoding="utf-8")
production = main + "\n" + app_state
window_header_path = ROOT / "src" / "SimilarSoundsWindow.h"
window_source_path = ROOT / "src" / "SimilarSoundsWindow.cpp"

assert window_header_path.is_file(), "Similar Sounds must expose a narrow window boundary header"
assert window_source_path.is_file(), "Similar Sounds implementation must leave main.cpp"

window_header = window_header_path.read_text(encoding="utf-8")
window = window_source_path.read_text(encoding="utf-8")

for declaration in (
    "void OpenSimilarSoundsWindow(AppState& state);",
    "void InvalidateSimilarSoundsWindow(AppState& state);",
    "void RenderSimilarSoundsWindow(AppState& state);",
):
    assert declaration in window_header

assert '#include "SimilarSoundsWindow.h"' in main
assert '#include "SimilarityQueryService.h"' not in main
assert main.count("aura::ui::OpenSimilarSoundsWindow(infoState)") == 1
assert main.count("aura::ui::InvalidateSimilarSoundsWindow(state)") == 1
assert main.count("aura::ui::RenderSimilarSoundsWindow(state)") == 1

for forbidden in (
    "BEGIN SIMILAR SOUNDS WINDOW",
    "DrawSimilarSoundsActionRow",
    "DrawSimilarSoundsResultsTable",
    "SimilarityWorkflowSnapshot",
    '"##SimilarResultsTable"',
    "MakeSimilarityQueryHandler(state.similaritySearch)",
):
    assert forbidden not in main

assert '#include "WindowsMain.h"' not in window
assert '#include "platform/' not in window

# Task 8A retires the legacy central-list similarity mode. The standalone
# Similar Sounds window keeps its own workflow and row state below.
for retired in (
    "similarModeActive",
    "similarScoreMap",
    "showSimilarPanel",
    "##SimilarBanner",
    "similarQueryFilePath",
    "similarSearchTimeMs",
):
    assert retired not in production

results_start = window.index("static void DrawSimilarSoundsResultsTable")
results_end = window.index("void RenderSimilarSoundsWindow", results_start)
results_table = window[results_start:results_end]

# A result row can be taller than its file-name label because the waveform
# has a minimum height. The selectable must therefore own that explicit row
# height and allow later cell content to overlap without shrinking hit scope.
assert "const float similarRowHeight" in results_table
assert "ImGui::TableNextRow(ImGuiTableRowFlags_None, similarRowHeight)" in results_table
assert "ImGuiSelectableFlags_AllowOverlap" in results_table
assert "ImVec2(0.0f, similarRowHeight)" in results_table
assert "const float waveHeight = similarRowHeight" in results_table
assert "ImGui::Dummy(ImVec2(waveWidth, waveHeight))" in results_table
assert "DrawMiniWaveform(cacheEntry, playPos, waveWidth, waveHeight)" in results_table

# Similar-result rows follow the same playback preference policy as the main
# list. The waveform column owns its click so one gesture cannot both toggle
# row playback and seek.
for required in (
    "const int previousSelectedRow",
    "ImGuiTableColumnFlags_IsHovered",
    "if (!waveColumnHovered)",
    "GetPlaybackAction(state.settings",
    "PlaybackAction::Pause",
    "PlaybackAction::Play",
):
    assert required in results_table

# The mini waveform is not decorative: it overlays one stable hit target over
# both cached and pending waveform states and supports click/drag seek.
for required in (
    "const ImVec2 waveCellMin = ImGui::GetCursorScreenPos()",
    "ImGui::SetCursorScreenPos(waveCellMin)",
    'ImGui::InvisibleButton("##SimilarWaveHit"',
    "const bool waveClicked = ImGui::IsItemClicked()",
    "if (waveClicked) state.similarSoundsSelectedRow = index",
    "ImGui::IsItemActive() || ImGui::IsItemClicked()",
    "state.uiState.lastMiniWaveSeekPos = seekPos",
    "state.audioEngine.seekToPosition(seekPos)",
):
    assert required in results_table

# RequestSearch clears the workflow snapshot immediately. Refresh the local
# render snapshot in the same frame so the previous result set cannot flash
# beneath the newly-started request.
render_start = window.index("void RenderSimilarSoundsWindow")
render = window[render_start:]
body_start = window.index("static void DrawSimilarSoundsBody")
body_end = window.index("static void PollSimilarSoundsWorkflow", body_start)
body = window[body_start:body_end]
action_row = body.index("DrawSimilarSoundsActionRow")
snapshot_refresh = body.index("snapshot = workflow.Snapshot()", action_row)
results_draw = body.index("DrawSimilarSoundsResultsTable", action_row)
assert action_row < snapshot_refresh < results_draw

# RQ-20261022-01: indexing belongs with Similar Sounds, without a second preset.
# Starting the batch invalidates results synchronously, so refresh the render
# snapshot after the index controls too (including a batch that ends this frame).
assert "ImGui::BeginTabItem(L(Main_SimilarSearchTab))" not in main
index_controls = body.index("DrawSimilarityIndexPanel(state)")
index_snapshot_refresh = body.index("snapshot = workflow.Snapshot()", index_controls)
assert action_row < index_controls < index_snapshot_refresh < results_draw
assert main.count('##SimilarPreset') == 0
assert window.count('##SimilarPreset') == 1
index_start = main.index("void DrawSimilarityIndexPanel(AppState& state)")
index_end = main.index("void RenderUI(AppState& state)", index_start)
index_panel = main[index_start:index_end]
for required in (
    "L(Main_IndexedProgressFmt)",
    "L(Main_BatchAnalyzeFmt)",
    "StartBatchFeatureExtraction(state)",
    "state.batchFeatureProgress.load()",
    "state.batchFeatureCancelRequested.store(true)",
    "L(Main_IndexAllDone)",
    "state.dbSwitchInProgress",
    "state.backfillDbGeneration",
):
    assert required in index_panel, f"Missing migrated index behavior: {required}"
assert "GetAIEngine().GetAllTasks()" not in index_panel
assert "L(Main_BackgroundTasksFmt)" in main
# Folder-index totals can briefly be stale after switching libraries. The
# analysis controls must count the same audio records as the extraction task.
assert "tryGetFeatureIndexCounts" not in index_panel, "Full-library SQL must not execute on the UI thread"
assert "statistics.SetDatabase(state.currentDbPathUtf8, state.backfillDbGeneration)" in index_panel
assert "statistics.Poll()" in index_panel and "statistics.Request()" in index_panel
assert "state.database.getFileCount()" not in index_panel

# The isolated QA executable must exercise the same batch entry as the button.
qa_host = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
assert 'cmd == "similar_index"' in qa_host
qa_index = qa_host[qa_host.index('cmd == "similar_index"'):
                   qa_host.index('cmd == "get_state"')]
assert "StartBatchFeatureExtraction(state)" in qa_index
assert "PreviewIsolationRootUtf8()" in qa_index
assert "state.batchFeatureCancelRequested.store(true)" in qa_index
assert 'similar.index.start' in index_panel
assert 'similar.index.cancel' in index_panel

# Closing the standalone window is a result-lifecycle boundary. Minimize keeps
# the workflow alive, but an explicit close must invalidate rows so reopening
# the window cannot present the previous search before a new Find action.
assert "const bool windowWasOpen" in render
close_branch = render[render.index("if (!aura::ui::BeginStandaloneWindow"):
                      render.index("DrawSimilarSoundsBody")]
assert "windowWasOpen && !state.similarSoundsWindow.open" in close_branch
assert "workflow.Invalidate()" in close_branch
assert "state.similarSoundsLocalReference.reset()" in close_branch

# A selected local result becomes the next reference only when Find is
# explicitly clicked. It never writes the central-list selection, and a real
# main-list selection change clears the local override.
action_impl = window[window.index("static bool DrawSimilarSoundsActionRow"):
                     window.index("static const char* SimilarSoundsBodyMessage")]
for required in (
    "state.similarSoundsSelectedRow >= 0",
    "state.similarSoundsLocalReference =",
    "workflow.SetReference(state.similarSoundsLocalReference)",
    "workflow.RequestSearch(state.similarSoundsPreset, 50)",
):
    assert required in action_impl
for required in (
    "state.similarSoundsLocalReference.reset()",
    "state.similarSoundsLocalReference ?",
):
    assert required in window

# RQ-20260725-01: the action-row filename is the next Find reference.
# A valid similarSoundsSelectedRow must win over snapshot.reference.displayName
# for currentName. Selection-only display must not SetReference; Find still does.
current_name_start = action_impl.index("const char* currentName")
current_name_end = action_impl.index("ImVec2 currentNameSize", current_name_start)
current_name_init = action_impl[current_name_start:current_name_end]
button_click_start = action_impl.index("if (aura::ui::Button(actionLabel, options))")
pre_button = action_impl[:button_click_start]
button_click = action_impl[button_click_start:]
assert "similarSoundsSelectedRow" in current_name_init
assert "snapshot.rows[" in current_name_init
assert ".fileName" in current_name_init
assert "snapshot.reference.displayName" in current_name_init
assert current_name_init.index("similarSoundsSelectedRow") <     current_name_init.index("snapshot.reference.displayName")
assert "workflow.SetReference" not in pre_button
assert "workflow.SetReference(state.similarSoundsLocalReference)" in button_click

# A partial feature-index build cannot publish provisional results as if they
# were final. Starting a batch invalidates rows; Find remains disabled until
# the build finishes and the user explicitly requests a fresh search.
for required in (
    "state.batchFeatureExtracting.load()",
    "Similar_Indexing",
    "state.similarSoundsObservedBatchExtracting",
):
    assert required in window
poll_start = window.index("static void PollSimilarSoundsWorkflow")
poll_end = window.index("void RenderSimilarSoundsContent", poll_start)
batch_transition = window[poll_start:poll_end]
assert "workflow.Invalidate()" in batch_transition

# The main-thread start path also invalidates synchronously, so a very short
# batch cannot finish before the renderer samples its rising edge.
batch_start = main[main.index("state.batchFeatureExtracting.store(true)") - 600:
                   main.index("state.batchFeatureExtracting.store(true)") + 500]
for required in (
    "aura::ui::InvalidateSimilarSoundsWindow(state)",
    "state.batchFeatureExtracting.store(true)",
):
    assert required in batch_start

space_start = window.index("ImGui::IsKeyPressed(ImGuiKey_Space, false)")
space_end = window.index("ImGui::PopStyleColor(3)", space_start)
space_handler = window[space_start:space_end]

for required in (
    "Similar_CurrentAudio", "Similar_Find", "Similar_Results",
    "Similar_ColumnFileName", "Similar_ColumnWaveform",
    "Similar_ColumnDuration", "Similar_ColumnFormat", "Similar_ColumnScore",
    "similarSoundsSelectedRow", "DrawMiniWaveform", "addPendingWaveform",
    "audioEngine.loadFileAsync",
):
    assert required in window

assert "const bool loadedThis = state.audioEngine.isLoaded()" in space_handler
assert "if (!loadedThis)" in space_handler

# Similar-result cache misses participate in the same visible-work contract as
# central-list rows. Registration must happen before enqueueing, otherwise the
# post-render submission pump drops the urgent request as stale.
cache_miss = results_table[
    results_table.index("if (cacheIt == state.waveformCache.end())"):
    results_table.index("} else {", results_table.index("if (cacheIt == state.waveformCache.end())"))
]
assert "state.visibleFilePaths.insert(row.filePath)" in cache_miss
assert cache_miss.index("state.visibleFilePaths.insert(row.filePath)") < \
    cache_miss.index("state.addPendingWaveform(row.filePath, true)")

# Decode completion uses the frame's cross-window visibility set. Checking
# only central searchResults would discard successful Similar Sounds decodes.
windows_main = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
visibility_start = windows_main.index("bool isStillVisiblePageItem")
visibility_end = windows_main.index("const bool isLoadedFileResult", visibility_start)
completion_visibility = windows_main[visibility_start:visibility_end]
assert "state.visibleFilePaths.find(result.filePath)" in completion_visibility
assert "state.searchResults" in completion_visibility

# AI-panel tabs must remain readable in both dark and light workstation
# themes. Only color tokens change; typography and spacing stay untouched.
ai_tabs_start = main.index("// 标签页 — push readable tab colors")
ai_tabs_end = main.index("ImGui::PopStyleColor(6)", ai_tabs_start)
ai_tabs = main[ai_tabs_start:ai_tabs_end]
for required in (
    "ThemeColors::current()",
    "aiTheme.dialogPanelBG",
    "aiTheme.frameBgBase",
    "aiTheme.controlAccent",
    "ImGuiCol_Text, aiTheme.interfaceText",
):
    assert required in ai_tabs

# Task 7 keeps waveform rendering and audition local to the standalone result
# model while reusing the existing cache and mini renderer.
for required in (
    "state.waveformCache.find(row.filePath)",
    "state.addPendingWaveform(row.filePath, true)",
    "DrawMiniWaveform(cacheEntry, playPos, waveWidth, waveHeight)",
    "GetSmoothedPlaybackPosition(state)",
    "state.audioEngine.getLoadedFilePath() == row.filePath",
    "ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)",
    "ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)",
    "ImGui::IsKeyPressed(ImGuiKey_Space, false)",
    "ImGuiSelectableFlags_AllowDoubleClick",
    "state.audioEngine.loadFileAsync(row.filePath, true)",
    "ScheduleDeferredWaveformDetail(",
    "AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS",
    "state.audioEngine.pause()", "state.audioEngine.play()",
    "state.audioEngine.seekToPosition(0.0f)",
    "MarkFilePlayed(state, row.filePath, row.fileId, true)",
):
    assert required in window

for removed in (
    "similarModeActive", "similarScoreMap", "state.searchResults =",
    "similarSearchTimeMs", "lastSimilarSearchMsg",
    "database.countFilesWithFeatures", "GetShortcutString", "similarQueryFilePath",
    "selectedResultIndex =", "beginAudioTargetFromSearchResult",
    "selectedIndicesSet",
):
    assert removed not in window

# Task 8C keeps the million-record performance gate as an explicit Release
# benchmark. It is intentionally not part of the ordinary CTest suite.
benchmark = (ROOT / "tools" / "similarity_bench.cpp").read_text(encoding="utf-8")
cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

for required in (
    "1'000'000", "replaceFeatureCache", "kWarmupRuns = 5",
    "kMeasuredRuns = 20", "topK = 50", "std::sort", "p50", "p95", "max",
    "estimated cache bytes", "unicode_paths=100%", "cache_load", "peak_working_set",
    "kCacheLoadGateMs", "kPeakWorkingSetGateBytes", "std::isfinite", "100.0",
):
    assert required in benchmark

assert "add_executable(SimilarityBench" in cmake
assert "tools/similarity_bench.cpp" in cmake
assert "src/SimilaritySearch.cpp" in cmake
assert "tests/test_similarity_search_database_stub.cpp" in cmake
assert "target_link_libraries(SimilarityBench PRIVATE Threads::Threads)" in cmake
assert "add_test(NAME SimilarityBench" not in cmake
