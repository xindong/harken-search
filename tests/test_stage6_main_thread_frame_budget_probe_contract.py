import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_optional(relative: str) -> str:
    path = ROOT / relative
    return path.read_text(encoding="utf-8", errors="ignore") if path.is_file() else ""


class Stage6MainThreadFrameBudgetProbeContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = read_optional("src/FrameBudgetDiagnostics.h")
        cls.source = read_optional("src/FrameBudgetDiagnostics.cpp")
        cls.unit = read_optional("tests/test_frame_budget_diagnostics.cpp")
        cls.windows = read_optional("src/WindowsMain.cpp")
        cls.frame = read_optional("src/AppFrameServices.cpp")
        cls.main = read_optional("src/main.cpp")
        cls.floating_layout = read_optional("src/FloatingPanelLayout.h")
        cls.app_state = read_optional("src/AppState.h")
        cls.app_cmake = read_optional("CMakeLists.txt")
        cls.test_cmake = read_optional("cmake/harken_search_behavior_tests.cmake")

    def assert_tokens(self, text: str, tokens: tuple[str, ...], area: str):
        missing = [token for token in tokens if token not in text]
        self.assertFalse(missing, f"missing {area} contract tokens: {missing}")

    def test_future_component_files_and_cmake_wiring_exist(self):
        expected = {
            "src/FrameBudgetDiagnostics.h": self.header,
            "src/FrameBudgetDiagnostics.cpp": self.source,
            "tests/test_frame_budget_diagnostics.cpp": self.unit,
        }
        missing_files = [path for path, text in expected.items() if not text]
        wired = re.search(
            r"if\(AURA_BUILD_DIAGNOSTICS\).*src/FrameBudgetDiagnostics\.cpp.*else\(\)",
            self.app_cmake,
            re.S,
        )
        missing_wiring = []
        if not wired:
            missing_wiring.append("diagnostics-only app source")
        if "TestFrameBudgetDiagnostics" not in self.test_cmake:
            missing_wiring.append("focused unit target")
        self.assertFalse(missing_files or missing_wiring,
                         f"missing seam files={missing_files}, wiring={missing_wiring}")

    def test_diagnostics_guard_fixed_samples_and_statistics_contract(self):
        self.assert_tokens(self.header, (
            "#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA", "#else",
            "FrameBudgetDiagnostics", "isCaptureActive", "inline",
        ), "diagnostics guard/Release no-op")
        self.assert_tokens(self.source, (
            "kWarmupFrames = 60", "kSampleFrames = 240",
            "idle", "busy_scroll_mini_playback", "idleSamples", "busySamples",
            "cpuWorkSamplesMs", "presentBlockSamplesMs", "frameIntervalSamplesMs",
            "nearestRank", "median_ms", "p95_ms", "max_ms",
        ), "fixed raw samples and recomputable statistics")
        self.assert_tokens(self.unit, (
            "60", "240", "nearestRank", "median_ms", "p95_ms", "max_ms",
        ), "component unit coverage")

    def test_windows_main_has_ordered_real_loop_hooks(self):
        named_hooks = (
            "FrameBudgetDiagnostics::beginFrame",
            "FrameBudgetDiagnostics::beginCpuWork",
            "FrameBudgetDiagnostics::endCpuWork",
            "FrameBudgetDiagnostics::beginPresent",
            "FrameBudgetDiagnostics::endPresent",
            "FrameBudgetDiagnostics::endFrame",
        )
        named_positions = {hook: self.windows.find(hook) for hook in named_hooks}
        swap_phase = self.windows.find('DevTools_BeginFramePhase("SwapBuffers")')
        swap = self.windows.find("SwapBuffers(hdc)", swap_phase)
        positions = [
            named_positions[named_hooks[0]], named_positions[named_hooks[1]],
            named_positions[named_hooks[2]], named_positions[named_hooks[3]],
            swap, named_positions[named_hooks[4]], named_positions[named_hooks[5]],
        ]
        self.assertTrue(all(position >= 0 for position in positions),
                        f"missing future WindowsMain frame hooks: {named_positions}")
        self.assertEqual(positions, sorted(positions),
                         "frame hooks must bracket CPU work and real SwapBuffers in order")
        qa_section = self.windows.find("// QA Automation: file-based command channel")
        qa_poll = self.windows.find("PollQaCommandFile(state)", qa_section)
        self.assertTrue(qa_section < positions[0] < qa_poll,
                        "frame interval must begin before the per-frame QA control point")
        heartbeat = self.windows.find("// Periodic heartbeat log")
        last_throttle_sleep = self.windows.rfind("Sleep(", 0, heartbeat)
        self.assertTrue(last_throttle_sleep < positions[1] < heartbeat,
                        "CPU timing must begin immediately after deliberate throttle Sleeps")

    def test_capture_suppresses_qa_poll_and_proves_busy_workload(self):
        qa_suppressed = re.search(
            r"!FrameBudgetDiagnostics::isCaptureActive\(\).*PollQaCommandFile",
            self.windows,
            re.S,
        )
        self.assertTrue(qa_suppressed,
                        "QA file polling must be suppressed only while capture is active")
        self.assert_tokens(self.source + self.windows, (
            "deterministicScroll", "distinctScrollPositions",
            "playbackFramesObserved", "miniRequestsSubmitted", "miniResultsConsumed",
            "setVolume(0", "isPlaying",
        ), "busy scroll/mini/muted-playback proof")

    def test_release_configuration_keeps_qa_poll_and_cold_mini_symbols_compilable(self):
        self.assertIn(
            "#if !(AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA)",
            self.windows,
            "release builds need a no-op QA poll implementation for shared call sites",
        )
        self.assertIn(
            "static void PollQaCommandFile(AppState&) {}",
            self.windows,
            "release builds must retain a no-op QA poll symbol",
        )
        waveform_start = self.frame.find("const std::string loadedWaveformPath")
        waveform_block = self.frame[waveform_start:self.frame.find("if (!foregroundWaveformRebuild", waveform_start)]
        self.assertRegex(
            waveform_block,
            r"#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA[\s\S]*options.seedVisibleRequest[\s\S]*#endif",
            "cold-mini diagnostics state must be guarded in release builds",
        )
        self.assertNotIn(
            "inline void DevTools_LogFrameSpike(float, bool = false)",
            self.header,
            "the declaration already owns the default argument",
        )

    def test_json_environment_and_deadline_contract(self):
        self.assert_tokens(self.source, (
            "schema", "status", "measured", "environment_gate", "failed",
            "metric", "main_thread_frame_budget", "value_ms",
            "cpu_work_ms", "present_block_ms", "frame_interval_ms",
            "window_visible", "window_focused", "window_iconic",
            "client_width", "client_height", "1280", "720", "dpi_scale",
            "opengl", "vsync", "audio_device",
            "kInternalDeadlineSeconds = 90", "kExternalSubprocessTimeoutSeconds = 120",
            "resultJson", "start_frame_budget_capture", "frame_budget_result",
        ), "one-object JSON/control/environment/deadline")

    def test_abort_restore_phase_vsync_and_failure_payload_contract(self):
        self.assertRegex(
            self.windows,
            r"static void AbortFrameBudgetCapture[^}]*capture\.abort[^}]*RestoreFrameBudgetVolume",
            "abort helper must restore diagnostics-mutated playback and scroll state",
        )
        self.assertRegex(
            self.windows,
            r"if \(!running\)\s*\{[^}]*AbortFrameBudgetCapture\([^}]*break;",
            "WM_QUIT must abort and restore before leaving the loop",
        )
        hidden = self.windows[self.windows.index("const bool mainWindowHidden ="):
                              self.windows.index("// Frame pacing follows focus")]
        self.assertIn("IsIconic(hwnd) || !Platform::WindowIsVisible(hwnd)", hidden)
        self.assertIn("mainWindowHidden && !HarkenSearchHasHostedPlatformViewports()", hidden)
        self.assertRegex(hidden, r"AbortFrameBudgetCapture\([^}]*continue;")
        self.assertRegex(self.windows, r"\}\s*#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA"
                         r"[\s\S]*?writeDockTrace\(false\);\s*#endif\s*"
                         r"#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA\s*"
                         r"RestoreFrameBudgetVolume\(state, true\);\s*#endif\s*#endif\s*// Cleanup")
        loop = self.windows.find("// Main loop")
        render = self.windows.find("RenderUI(state)", loop)
        render_mark = self.windows.find("markRenderUi", render)
        gl = self.windows.find("ImGui_ImplOpenGL3_RenderDrawData", render_mark)
        gl_mark = self.windows.find("markGlRender", gl)
        swap = self.windows.find("SwapBuffers(hdc)", gl_mark)
        present_mark = self.windows.find("markPresent", swap)
        self.assertTrue(all(position >= 0 for position in
                            [render, render_mark, gl, gl_mark, swap, present_mark]))
        self.assertEqual([render, render_mark, gl, gl_mark, swap, present_mark],
                         sorted([render, render_mark, gl, gl_mark, swap, present_mark]))
        self.assertIn('wglGetProcAddress("wglGetSwapIntervalEXT")', self.windows)
        self.assertRegex(self.windows, r"getSwapInterval\(\)\s*>\s*0")
        self.assert_tokens(self.source + self.unit, (
            "renderUiFrames", "glRenderFrames", "presentFrames",
            "failureReason", "failureStage", "incomplete_frame",
        ), "real phases and non-measured reason/stage")
        self.assert_tokens(self.unit, (
            'find("\\\"scenarios\\\"") == std::string::npos',
            'find("\\\"value_ms\\\"") == std::string::npos',
        ), "non-measured payload omission coverage")

    def test_main_and_app_state_remain_outside_the_seam(self):
        self.assertNotIn("FrameBudgetDiagnostics", self.main,
                         "main.cpp must not host frame-budget seam code")
        self.assertNotIn("FrameBudgetDiagnostics", self.app_state,
                         "AppState schema must not carry frame-budget state")

    def test_floating_panels_clamp_to_resized_work_area(self):
        self.assert_tokens(self.floating_layout, (
            "struct FloatingPanelRect", "ClampFloatingPanelRect",
            "workWidth", "workHeight", "std::clamp", "return changed",
        ), "floating panel viewport clamp helper")
        self.assert_tokens(self.main, (
            '#include "FloatingPanelLayout.h"',
            "ClampFloatingPanelToViewport",
            "state.panelSidebar.floatX", "state.panelFileInfo.floatY",
            "g_searchResultsDockPanel.floatW",
            "sidebarClamped ? ImGuiCond_Always : ImGuiCond_Appearing",
            "fileInfoClamped ? ImGuiCond_Always : ImGuiCond_Appearing",
            "similarClamped ? ImGuiCond_Always : ImGuiCond_Appearing",
        ), "floating panel resize integration")

    def test_cold_mini_preparation_and_list_only_activity_contract(self):
        begin = self.windows.find('cmd == "prepare_frame_budget_cold_mini"')
        end = self.windows.find("FrameBudgetDiagnostics::startCommand()", begin)
        self.assertGreater(begin, 0, "missing cold-mini preparation command")
        prepare = self.windows[begin:end]
        self.assert_tokens(prepare, (
            "expected_runtime_root", "expected_db_path", "expected_files", "1000",
            "AURA_RUNTIME_ROOT", "IsHarkenSearchHarnessMultiInstanceEnabled",
            "GetHarkenSearchHarnessLaneKindUtf8", "currentDbPathUtf8", "getMainDbPath",
            "getFileCount", "currentGeneration", "countBackfillPending",
            "idleAssetBackfillEnabled", "GlobalBackfillWorkerPool().stop",
            "waveformWorker.cancelAllPending", "waveformWorker.stop",
            "purgeAllMiniWaveform", "countFilesNeedingWaveformCache",
            "countFilesNeedingDetailCache", "ReleaseWaveformEvictedEntry",
            "cold_mini_armed", 'status\\\":\\\"prepared',
            'worker_start_phase\\\":\\\"busy_setup',
        ), "isolated cold-mini preparation")
        self.assertNotIn("purgeAllWaveformDetail", prepare)
        self.assertNotIn("clearWaveformCache", prepare)
        self.assertRegex(
            self.windows,
            r"startCommand\(\)[\s\S]*ColdMiniArmMatches[\s\S]*startCapture",
            "capture start must require the matching isolated cold arm",
        )
        self.assertRegex(
            self.windows,
            r"AwaitingBusy[\s\S]*clearPendingWaveforms[\s\S]*waveformWorker\.start"
            r"[\s\S]*BusyScrollMiniPlayback",
            "worker may start only in busy setup after stale UI work is cleared",
        )
        self.assertRegex(
            self.windows,
            r"waveformFrameOptions.ignorePlaybackForCapture = s_frameBudgetColdMini.armed &&"
            r"[\s\S]*BusyWarmup[\s\S]*BusyCapture[\s\S]*PumpAppWaveforms",
        )
        self.assertRegex(
            self.frame,
            r"if \(options.ignorePlaybackForCapture\) miniIo.playing = false;"
            r"[\s\S]*ResolveMiniWaveformWorkerDecision\(miniIo\)",
            "busy capture must override only the copied mini policy context",
        )
        self.assert_tokens(self.windows + self.frame, (
            "listSubmitted", "listSuccessfulConsumed", "submitVisibleCacheRequest",
            "isCurrentListPath", "isLoadedFileResult", "observeMiniActivity",
            "RestoreFrameBudgetColdMini", "GlobalBackfillWorkerPool().start",
            "seedSubmitted", "submitVisibleRequest",
        ), "list-only proof and idempotent restoration")
        pump_begin = self.windows.find("static void PumpFrameBudgetWorkload")
        pump_end = self.windows.find("static void PollQaCommandFile", pump_begin)
        self.assertNotIn(
            "seedSubmitted", self.windows[pump_begin:pump_end],
            "direct-decode seed must not run during the BusyWarmup transition",
        )
        self.assertRegex(
            self.windows,
            r"waveformFrameOptions.seedVisibleRequest =\s*"
            r"miniCaptureState == aura::diagnostics::FrameCaptureState::BusyCapture && !s_frameBudgetColdMini.seedSubmitted;"
            r"[\s\S]*PumpAppWaveforms\(state, isMouseIdle, waveformFrameOptions\)"
            r"[\s\S]*if \(seededMini\) s_frameBudgetColdMini.seedSubmitted = true;",
        )
        self.assertRegex(
            self.frame,
            r"s_miniListWasForegroundBlocked = miniDecision\.cancelQueuedListWork;"
            r"[\s\S]*?options.seedVisibleRequest && submitBudget > 0"
            r"[\s\S]*?filePath == loadedWaveformPath[\s\S]*?submitVisibleRequest"
            r"[\s\S]*?seedSubmitted = true;",
            "seed must run after cancellation only in counted BusyCapture with positive budget",
        )
        self.assertEqual(self.windows.count("RestoreFrameBudgetColdMini(state)"), 1)

    def test_cold_mini_prepare_to_start_restore_lifecycle(self):
        restore_begin = self.windows.find("static void RestoreFrameBudgetVolume")
        restore_end = self.windows.find("static void AbortFrameBudgetCapture", restore_begin)
        restore = self.windows[restore_begin:restore_end]
        self.assert_tokens(restore, (
            "bool forceColdMini", "captureStarted", "forceColdMini ||",
            "RestoreFrameBudgetColdMini(state)",
        ), "normal/forced cold-mini restoration split")
        self.assertRegex(
            self.windows,
            r"if \(!FrameBudgetDiagnostics::isCaptureActive\(\)\)\s*"
            r"\{ RestoreFrameBudgetVolume\(state\); return; \}",
            "prepared arm must survive ordinary inactive workload pumping",
        )
        frame_end = self.windows.index("FrameBudgetDiagnostics::endFrame();")
        restoration = self.windows.index("RestoreFrameBudgetVolume(state);", frame_end)
        self.assertLess(frame_end, restoration)
        self.assertIn("#if AURA_BUILD_DIAGNOSTICS && !AURA_INTERNAL_BETA",
                      self.windows[frame_end:restoration])
        self.assertRegex(
            self.windows,
            r"static void AbortFrameBudgetCapture[\s\S]*?RestoreFrameBudgetVolume\(state, true\)",
            "abort must force cold-mini restoration before capture start too",
        )
        self.assertRegex(
            self.windows,
            r"!ColdMiniArmMatches\(state\)[\s\S]*?AbortFrameBudgetCapture",
            "DB identity loss must use the forced abort path",
        )
        self.assertRegex(
            self.windows,
            r"else AbortFrameBudgetCapture\(state, \"swap_buffers_failed\", \"present\"\)",
            "SwapBuffers failure must use the forced abort path",
        )
        control_begin = self.windows.find('cmd == "prepare_frame_budget_cold_mini"')
        control_end = self.windows.find("FrameBudgetDiagnostics::resultCommand()", control_begin)
        control = self.windows[control_begin:control_end]
        self.assertRegex(
            control,
            r"if \(failureReason\)[\s\S]*?RestoreFrameBudgetVolume\(state, true\)",
            "prepare failure must force restoration",
        )
        self.assertRegex(
            control,
            r"if \(!ColdMiniArmMatches\(state\)\)[\s\S]*?RestoreFrameBudgetVolume\(state, true\)"
            r"[\s\S]*?if \(!started\) RestoreFrameBudgetVolume\(state, true\)",
            "unarmed and failed capture starts must force restoration",
        )
        self.assertGreaterEqual(self.windows.count("RestoreFrameBudgetVolume(state, true)"), 4)
        self.assertRegex(
            self.windows,
            r"static void RestoreFrameBudgetColdMini[\s\S]*?if \(!s_frameBudgetColdMini\.holding\) return;",
            "forced restoration must remain idempotent",
        )

    def test_cold_mini_partial_cache_accounting_and_failure_diagnostics(self):
        begin = self.windows.find('cmd == "prepare_frame_budget_cold_mini"')
        end = self.windows.find("FrameBudgetDiagnostics::startCommand()", begin)
        prepare = self.windows[begin:end]
        self.assertNotIn("miniBefore == 0 ?", prepare)
        self.assert_tokens(prepare, (
            "int purged = -1", "if (identityBeforePurge && miniBefore >= 0 && miniBefore <= expectedFiles)",
            "purged = state.database.purgeAllMiniWaveform()",
            "miniBefore >= 0", "miniBefore <= expectedFiles", "purged >= 0",
            "miniBefore + purged == expectedFiles", "miniAfter == expectedFiles",
            "detailAfter == detailBefore", "waveformUrgentPending.size()",
            "waveformNormalPending.size()", "uiPending", "workerPending",
            "durablePending", "failedCount", "missCount", "visibleCount",
            "meterPendingCount", "pendingSetCount", "cacheEntries", "dbIdentityOk",
        ), "partial-cache conservation and actual cleared-state counts")
        self.assert_tokens(prepare, (
            "cold_mini_cache_accounting_mismatch", "cold_mini_detail_changed",
            "cold_mini_worker_pending", "cold_mini_queue_not_idle",
            "cold_mini_memory_not_cleared", "cold_mini_ui_not_cleared",
            "cold_mini_db_identity_lost", 'counts\\\":{',
        ), "fixed first-failure diagnostics")
        for hardcoded in ('purged_rows\\\":1000', 'mini_missing_before\\\":0',
                          'durable_pending\\\":0', 'ui_pending\\\":0'):
            self.assertNotIn(hardcoded, prepare,
                             f"prepared JSON must emit actual count: {hardcoded}")

    def test_cold_mini_blocks_pending_db_switch_and_prioritizes_identity(self):
        begin = self.windows.find('cmd == "prepare_frame_budget_cold_mini"')
        end = self.windows.find("FrameBudgetDiagnostics::startCommand()", begin)
        prepare = self.windows[begin:end]
        self.assert_tokens(prepare, (
            "const bool dbSwitchPending = state.dbSwitchInProgress ||",
            "!state.pendingDbPath.empty()", "cold_mini_db_switch_pending",
            "const bool identityBeforePurge = state.database.isOpen()",
            "!state.dbSwitchInProgress", "state.pendingDbPath.empty()",
            'db_switch_in_progress\\\":', 'pending_db_path_present\\\":',
        ), "pending DB-switch preflight and diagnostics")
        self.assertIn(
            r'{\"schema\":1,\"cmd\":\"prepare_frame_budget_cold_mini\",\"status\":\"failed\",'
            r'\"reason\":\"cold_mini_db_switch_pending\",\"stage\":\"preflight\"}',
            prepare,
        )
        holding = prepare.find("arm.holding = true")
        stop = prepare.find("GlobalBackfillWorkerPool().stop()")
        identity = prepare.find("const bool identityBeforePurge")
        purge = prepare.find("purged = state.database.purgeAllMiniWaveform()")
        self.assertTrue(0 <= prepare.find("if (dbSwitchPending)") < holding < stop < identity < purge,
                        "switch preflight and post-stop identity proof must precede purge")
        identity_proof = prepare[identity:prepare.find("const int64_t fileCount", identity)]
        self.assert_tokens(identity_proof, (
            "state.database.isOpen()", "state.currentDbPathUtf8",
            "state.database.getMainDbPath()", "state.backfillQueue.currentDbPath()",
            "state.backfillDbGeneration == arm.generation",
            "state.backfillQueue.currentGeneration() == arm.generation",
            "!state.dbSwitchInProgress", "state.pendingDbPath.empty()",
        ), "post-stop DB identity proof")
        self.assertRegex(
            prepare,
            r"if \(identityBeforePurge && miniBefore >= 0 && miniBefore <= expectedFiles\)\s*"
            r"purged = state\.database\.purgeAllMiniWaveform\(\)",
            "identity loss after worker stop must skip destructive purge",
        )
        failure = prepare.find("const char* failureReason")
        self.assertGreater(failure, 0)
        priority = prepare[failure:prepare.find("if (failureReason)", failure)]
        self.assertLess(priority.find("!dbIdentityOk"), priority.find("!accountingOk"),
                        "DB identity loss must be the first post-mutation failure")


if __name__ == "__main__":
    unittest.main()
