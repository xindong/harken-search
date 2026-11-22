import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src" / "main.cpp"
ENGINE_H = ROOT / "src" / "AudioEngine.h"
ENGINE_CPP = ROOT / "src" / "AudioEngine.cpp"


def region(source: str, start: str, end: str) -> str:
    left = source.index(start)
    right = source.index(end, left + len(start))
    return source[left:right]


def definition_after_prototype(source: str, start: str, end: str) -> str:
    first = source.index(start)
    left = source.index(start, first + len(start))
    right = source.index(end, left + len(start))
    return source[left:right]


def require_text(test: unittest.TestCase, body: str, expected: str, message: str) -> None:
    if expected not in body:
        test.fail(message)


class MagicEqDawDeliveryContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = MAIN.read_text(encoding="utf-8")
        cls.engine_h = ENGINE_H.read_text(encoding="utf-8")
        cls.engine_cpp = ENGINE_CPP.read_text(encoding="utf-8")

    def test_enabled_magic_eq_makes_the_daw_payload_a_processed_copy(self) -> None:
        processing = region(
            self.main,
            "static bool TransferHasProcessing(const AudioEngine::TransferProcessingSnapshot& snapshot)",
            "static bool TransferHasRegion(",
        )
        prepare = definition_after_prototype(
            self.main,
            "static bool PrepareCurrentAudioForTransfer(AudioEngine& engine,\n                                           std::vector<std::string>& outFiles,",
            "static bool TransferHasProcessing(",
        )
        build = region(
            self.main,
            "static bool BuildTransferPayloadForPaths(",
            "static std::vector<std::string> BuildBrowserTransferSourcePaths(",
        )
        require_text(self, processing, "snapshot.magicEQ.enabled",
                     "enabled Magic EQ is not part of the processed-copy decision")
        require_text(self, prepare, "processingSnapshot.magicEQ.enabled",
                     "single-file DAW drag does not snapshot Magic EQ")
        self.assertIn("hasProcessing", build)
        self.assertIn("TransferHasProcessing(processingSnapshot)", build)

    def test_region_and_full_file_render_apply_the_supplied_magic_eq_snapshot(self) -> None:
        require_text(self, self.engine_h, "const TransferProcessingSnapshot* transferSnapshot = nullptr",
                     "render APIs do not accept an explicit frozen DAW EQ snapshot")
        region_render = region(
            self.engine_cpp,
            "bool AudioEngine::exportRegion(",
            "bool AudioEngine::hasAudioAdjustments()",
        )
        file_render = region(
            self.engine_cpp,
            "bool AudioEngine::exportFileProcessed(",
            "void AudioEngine::ensureSeekWorkerStarted()",
        )
        for body in (region_render, file_render):
            require_text(self, body, "transferSnapshot",
                         "file render does not consume the provided EQ snapshot")
            require_text(self, body, "CenterSenseDsp::processMagicEQ",
                         "file render does not run the Magic EQ DSP stage")
            self.assertLess(body.index("processCenterSense"),
                            body.index("processMagicEQ"))

    def test_independent_region_export_passes_current_magic_eq_snapshot(self) -> None:
        toolbar = region(
            self.main,
            "if (exportRegionClicked) {",
            "if (HoveredForHelp())",
        )
        require_text(self, toolbar, "captureTransferProcessingSnapshot()",
                     "independent region export does not capture the current Magic EQ settings")
        require_text(self, toolbar, "&processingSnapshot",
                     "independent region export does not pass the captured settings to the renderer")

    def test_one_snapshot_flows_through_render_before_any_path_is_dispatched(self) -> None:
        render_helper = region(
            self.main,
            "static bool ExportFileWithCurrentTransferProcessing(",
            "static bool BuildTransferPayloadForPaths(",
        )
        build_payload = region(
            self.main,
            "static bool BuildTransferPayloadForPaths(",
            "static std::vector<std::string> BuildBrowserTransferSourcePaths(",
        )
        send = region(
            self.main,
            "static void SendToDAW(AppState& state,\n                      int dawIndex,",
            "static void DispatchPreparedSendToDAW(AppState& state,\n                                      DawTarget& target,",
        )
        require_text(self, render_helper, "TransferProcessingSnapshot& processingSnapshot",
                     "DAW render helper has no immutable processing snapshot input")
        require_text(self, build_payload, "captureTransferProcessingSnapshot()",
                     "multi-file drag does not freeze EQ once before rendering")
        self.assertLess(build_payload.index("captureTransferProcessingSnapshot()"),
                        build_payload.index("for (size_t i = 0;"))
        self.assertLess(render_helper.index("return false"),
                        render_helper.index("outFile = tempFile"))
        require_text(self, send, "const bool payloadReady = BuildTransferPayload(state, files, prepError);",
                     "Send to DAW is not gated on a complete transfer payload")
        self.assertLess(send.index("return;", send.index("if (!payloadReady)")),
                        send.index("DispatchPreparedSendToDAW"))

    def test_current_file_drag_uses_the_same_magic_eq_preparation_gate(self) -> None:
        prepare = definition_after_prototype(
            self.main,
            "static bool PrepareCurrentAudioForTransfer(AudioEngine& engine,\n                                           std::vector<std::string>& outFiles,",
            "static bool TransferHasProcessing(",
        )
        require_text(self, prepare, "engine.exportRegion(",
                     "current-file region drag no longer uses the existing region renderer")
        require_text(self, prepare, "processingSnapshot.magicEQ.enabled",
                     "current-file region drag omits Magic EQ from its render")
        require_text(self, prepare, "&processingSnapshot",
                     "current-file region render does not receive the frozen snapshot")
        require_text(self, prepare, "TrackTempDragFile(tempFile)",
                     "rendered region output is not registered for drag cleanup")
        self.assertLess(prepare.index("engine.exportRegion("),
                        prepare.index("outFiles.push_back(tempFile)"))

    def test_batch_browser_drag_stops_on_processed_export_failure(self) -> None:
        browser_drag = region(
            self.main, "rawPaths.push_back(entry.fullPath);",
            "if (g_pendingDeferredBrowserSelect == entryIndex)",
        )
        self.assertNotIn("StartFileDrag(rawPaths);", browser_drag)
        self.assertIn("state.loadErrorToast = dragErr.empty() ? L(Misc_DragFailed) : dragErr;",
                      browser_drag)
        self.assertIn("state.loadErrorToastUntil = ImGui::GetTime() + 6.0;", browser_drag)

    def test_batch_database_drag_stops_on_processed_export_failure(self) -> None:
        database_drag = region(
            self.main, "rawPaths.push_back(record.filePath);",
            "if (g_pendingDeferredSingleSelect == i)",
        )
        self.assertNotIn("StartFileDrag(rawPaths);", database_drag)
        self.assertIn("state.loadErrorToast = dragErr.empty() ? L(Misc_DragFailed) : dragErr;",
                      database_drag)
        self.assertIn("state.loadErrorToastUntil = ImGui::GetTime() + 6.0;", database_drag)

    def test_other_transfer_entrypoints_keep_their_existing_error_display(self) -> None:
        region_drag = region(self.main,
                             "void StartRegionDrag(AudioEngine& engine, float regionFadeInNorm,",
                             "void StartRegionDrag(AudioEngine& engine) {")
        send = region(self.main,
                      "static void SendToDAW(AppState& state,\n                      int dawIndex,",
                      "static void DispatchPreparedSendToDAW(AppState& state,")
        self.assertIn("Platform::ShowMessageBox(g_hwnd", region_drag)
        self.assertIn("PublishDawSendFailure(state, prepError);", send)
        self.assertLess(send.index("return;", send.index("if (!payloadReady)")),
                        send.index("DispatchPreparedSendToDAW"))

    def test_send_feedback_reads_one_snapshot_and_does_not_trim_detail(self) -> None:
        toast = region(self.main, "static void DrawSendToDAWToast(", "static bool ResolveUserDefaultConfigPaths(")
        require_text(self, toast, "state.dawSendFeedback.snapshot()", "worker results are not read as one snapshot")
        self.assertIn("ImGui::GetWindowViewport()", toast)
        self.assertNotIn("display.resize(", toast)
        self.assertNotIn("ImGui::GetWindowDrawList()", toast)

    def test_worker_uses_real_results_and_no_imgui(self) -> None:
        send = definition_after_prototype(
            self.main, "static void DispatchPreparedSendToDAW(AppState& state,",
            "static void SendToDAWForPaths(",
        )
        require_text(self, send, "if (!state.dawSendFeedback.begin(", "dispatch has no synchronized re-entry guard")
        self.assertIn("state.dawSendFeedback.setProgress(sent, total)", send)
        self.assertIn("ClassifyDawSendResult(result.ok, result.sent, result.total)", send)
        self.assertIn("state.dawSendFeedback.finish(", send)
        self.assertNotIn("ImGui::", send)
        self.assertNotIn("dawSendSent.store", send)

    def test_other_host_feedback_distinguishes_partial_and_fallback_from_insert(self) -> None:
        dispatch = region(self.main, "static bool SendFilesToProToolsTarget(", "// Top-level: Send =")
        require_text(self, dispatch, "ApplySteinbergSendReceipt(result, feedback)", "Steinberg feedback bypassed the real receipt classifier")
        self.assertIn("DawSendOutcome::Fallback", dispatch)
        self.assertIn("IsDawSendClipListFallback(r.errorCode)", dispatch)
        self.assertNotIn("CopyPathsToClipboard(files)", dispatch)

    def test_feedback_fixture_only_enters_via_the_fixed_preview_diagnostic_lane(self) -> None:
        windows = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
        start = '} else if (cmd == "send_feedback_preview")'
        require_text(self, windows, start, "the feedback renderer has no isolated fixture")
        fixture = region(windows, start, '} else if (cmd == "send_widget_menu")')
        self.assertIn("#if ARIS_PREVIEW_ISOLATION", fixture)
        self.assertIn("MagicEqQaIsolationReady()", fixture)
        self.assertIn("ControlDawSendFeedbackPreview", fixture)
        self.assertNotIn("DispatchToDaw", fixture)
        self.assertNotIn("PrepareReaperBridgeForSend", fixture)

    def test_feedback_lifecycle_and_utf8_foreground_layout_with_real_imgui(self) -> None:
        if not shutil.which("cmake"):
            self.skipTest("CMake is required for the headless DAW notice regression")
        with tempfile.TemporaryDirectory(prefix="harken-daw-send-notice-") as scratch:
            folder = Path(scratch)
            generated = folder / "generated"
            vendor = ROOT / "thirdparty" / "imgui"
            test_source = ROOT / "tests" / "test_daw_send_notice.cpp"
            localization = ROOT / "src" / "Localization.cpp"
            (folder / "CMakeLists.txt").write_text("\n".join([
                "cmake_minimum_required(VERSION 3.20)",
                "project(DawSendNoticeRegression LANGUAGES CXX)",
                "set(CMAKE_CXX_STANDARD 17)",
                "add_executable(daw_send_notice",
                f'  "{test_source.as_posix()}" "{localization.as_posix()}"',
                *(f'  "{(vendor / name).as_posix()}"' for name in
                  ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp")),
                ")",
                f'target_include_directories(daw_send_notice PRIVATE "{vendor.as_posix()}"'
                f' "{(ROOT / "src").as_posix()}" "{generated.as_posix()}")',
                "target_compile_definitions(daw_send_notice PRIVATE IMGUI_DISABLE_DEFAULT_IME_FUNCTIONS IMGUI_DISABLE_DEFAULT_CLIPBOARD_FUNCTIONS)",
                "if(MSVC)",
                "  target_compile_options(daw_send_notice PRIVATE /utf-8)",
                "endif()",
            ]), encoding="utf-8")
            for command in (
                [sys.executable, str(ROOT / "tools" / "generate_localization.py"),
                 "--generate", "--output-dir", str(generated)],
                ["cmake", "-S", str(folder), "-B", str(folder / "build"), "-DCMAKE_BUILD_TYPE=Release"],
                ["cmake", "--build", str(folder / "build"), "--config", "Release", "--parallel", "2"],
            ):
                result = subprocess.run(command, cwd=folder, capture_output=True, text=True, errors="replace", timeout=180)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            executable = next(path for path in (
                folder / "build" / "Release" / "daw_send_notice.exe",
                folder / "build" / "daw_send_notice.exe",
                folder / "build" / "daw_send_notice",
            ) if path.is_file())
            result = subprocess.run([str(executable)], cwd=folder, capture_output=True, text=True, errors="replace", timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
