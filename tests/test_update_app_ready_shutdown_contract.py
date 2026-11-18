from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WINDOWS_MAIN = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
COMPOSITION_H = (ROOT / "src/update/HarkenSearchUpdateComposition.h").read_text(
    encoding="utf-8"
)
COMPOSITION_CPP = (ROOT / "src/update/HarkenSearchUpdateComposition.cpp").read_text(
    encoding="utf-8"
)
BOOTSTRAP_RUNTIME = (ROOT / "src/bootstrap/HarkenSearchBootstrapRuntime.cpp").read_text(
    encoding="utf-8"
)


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise AssertionError(f"unclosed function body: {signature}")


def test_entry_only_captures_bootstrap_challenge() -> None:
    entry_start = WINDOWS_MAIN.index("int WINAPI WinMain")
    entry = WINDOWS_MAIN[entry_start:entry_start + 2500]
    assert "CaptureHarkenSearchBootstrapHealthChallenge()" in entry
    assert "AcknowledgeHarkenSearchBootstrapHealthReady()" not in entry

    capture = function_body(
        COMPOSITION_CPP, "bool CaptureHarkenSearchBootstrapHealthChallenge()"
    )
    acknowledgement = function_body(
        COMPOSITION_CPP, "bool AcknowledgeHarkenSearchBootstrapHealthReady()"
    )
    assert "CreateUpdateBootstrapHealthPort" in capture
    assert "CaptureChallenge()" in capture
    assert "g_bootstrapHealthCaptureAttempted" in capture
    assert "AcknowledgeReady()" not in capture
    assert "CreateUpdateBootstrapHealthPort" not in acknowledgement
    assert "AcknowledgeReady()" in acknowledgement
    assert "std::move(g_bootstrapHealthPort)" in acknowledgement


def test_ready_ack_waits_for_the_first_main_ui_frame_and_is_one_shot() -> None:
    helper = function_body(
        WINDOWS_MAIN, "static void AcknowledgeBootstrapHealthAfterUiPresented()"
    )
    assert "AcknowledgeHarkenSearchBootstrapHealthReady()" in helper
    assert "bootstrapHealthAcknowledged" in helper
    assert WINDOWS_MAIN.count("AcknowledgeHarkenSearchBootstrapHealthReady()") == 1

    login = function_body(WINDOWS_MAIN, "static bool RunWebView2LoginScreen")
    assert "AcknowledgeBootstrapHealthAfterUiPresented()" not in login

    splash = function_body(WINDOWS_MAIN, "static void RunBetaSplashScreen")
    assert "AcknowledgeBootstrapHealthAfterUiPresented()" not in splash

    bypass = WINDOWS_MAIN.index(
        "Platform::WindowShow(hwnd, SW_SHOWMAXIMIZED); // deferred show"
    )
    startup_ready = WINDOWS_MAIN.index(
        "s_mainUiReady.store(true, std::memory_order_release);", bypass
    )
    assert "AcknowledgeBootstrapHealthAfterUiPresented()" not in WINDOWS_MAIN[
        bypass:startup_ready
    ]

    swap = WINDOWS_MAIN.index("framePresented = SwapBuffers(hdc);")
    fallback_ack = WINDOWS_MAIN.index(
        "AcknowledgeBootstrapHealthAfterUiPresented()", swap
    )
    assert swap < fallback_ack
    guard = WINDOWS_MAIN[swap:fallback_ack]
    assert "framePresented" in guard
    assert "startupEntryTransitionActive" in guard
    assert "s_mainUiReady" in guard


def test_startup_health_ack_does_not_depend_on_runtime_legal_dialog() -> None:
    entry = WINDOWS_MAIN.split("int WINAPI WinMain", 1)[1]
    assert "EnsureRequiredConsent" not in entry
    assert "AcknowledgeBootstrapHealthAfterUiPresented()" in entry
    assert "BOOL framePresented = FALSE;" in entry
    assert "framePresented = SwapBuffers(hdc);" in entry


def test_consent_health_callback_is_deferred_until_dialog_is_visible_and_interactive() -> None:
    consent = (ROOT / "src" / "ConsentManager.cpp").read_text(encoding="utf-8")
    init_start = consent.index("if (message == WM_INITDIALOG)")
    init_end = consent.index("if (message == kConsentUiPresentedMessage)", init_start)
    init_body = consent[init_start:init_end]
    assert "onPresented()" not in init_body
    assert "PostMessageW(dialog, kConsentUiPresentedMessage" in init_body
    presented = consent[consent.index("if (message == kConsentUiPresentedMessage)"):]
    assert "Platform::WindowIsVisible(dialog)" in presented
    assert "IsWindowEnabled(dialog)" in presented
    assert "Platform::WindowIsVisible(documentControl)" in presented
    assert "IsWindowEnabled(documentControl)" in presented
    assert "onPresented()" in presented


def test_bootstrap_acceptance_requests_graceful_ui_thread_shutdown() -> None:
    assert "ConsumeHarkenSearchUpdateGracefulExitRequest" in COMPOSITION_H
    assert "PublishHarkenSearchUpdateGracefulExitRequest" in COMPOSITION_CPP
    assert COMPOSITION_CPP.count("PublishHarkenSearchUpdateGracefulExitRequest();") == 1
    assert "std::_Exit(" not in COMPOSITION_CPP
    assert "ExitProcess(" not in COMPOSITION_CPP


def test_update_owner_can_retry_without_exiting_after_failed_handoff() -> None:
    owner = function_body(COMPOSITION_CPP, "void StartHarkenSearchAutomaticUpdate()")
    handoff = function_body(owner, "const auto runHandoff =")
    assert "UpdateApplicationDisposition::BootstrapRetryable" in handoff
    assert "HarkenSearchUpdateUiPhase::Error" in handoff
    assert "return false;" in handoff
    assert "if (runHandoff()) return;" in owner
    assert "if (retryHandoff)" in owner


def test_update_owner_dispatches_due_download_retries_and_startup_resume() -> None:
    owner = function_body(COMPOSITION_CPP, "void StartHarkenSearchAutomaticUpdate()")
    assert "started.schedules" in owner
    assert "result.schedules" in owner
    assert "dueAtMonotonicMs()" in owner
    assert "service.ExecuteScheduled(" in owner
    assert "started.snapshot.phase != UpdatePhase::Downloading" in owner
    assert "TerminateProcess(" not in COMPOSITION_CPP

    consume = function_body(
        COMPOSITION_CPP, "bool ConsumeHarkenSearchUpdateGracefulExitRequest()"
    )
    assert ".exchange(false" in consume
    publish = function_body(
        COMPOSITION_CPP, "void PublishHarkenSearchUpdateGracefulExitRequest()"
    )
    assert ".store(true" in publish

    main_loop = WINDOWS_MAIN[WINDOWS_MAIN.index("while (running)"):]
    consume_offset = main_loop.index("ConsumeHarkenSearchUpdateGracefulExitRequest()")
    request_exit_offset = main_loop.index("RequestHarkenSearchExit", consume_offset)
    assert consume_offset < request_exit_offset


def test_composition_health_wire_stays_inside_platform_port() -> None:
    for token in (
        "GetStdHandle",
        "PeekNamedPipe",
        "ReadFile",
        "WriteFile",
        "HANDLE",
    ):
        assert token not in COMPOSITION_CPP
    for declaration in (
        "bool CaptureHarkenSearchBootstrapHealthChallenge();",
        "bool AcknowledgeHarkenSearchBootstrapHealthReady();",
        "bool ConsumeHarkenSearchUpdateGracefulExitRequest();",
    ):
        assert declaration in COMPOSITION_H


def test_immutable_bootstrap_timeout_prevents_filesystem_begin() -> None:
    wait = BOOTSTRAP_RUNTIME.index("dependencies.process.WaitForExit(")
    timeout = BOOTSTRAP_RUNTIME.index(
        "BootstrapApplicationStatus::OldProcessTimedOut", wait
    )
    filesystem_begin = BOOTSTRAP_RUNTIME.index(
        "dependencies.filesystem.Begin(", wait
    )
    assert wait < timeout < filesystem_begin
    assert "options.oldProcessWaitMs" in BOOTSTRAP_RUNTIME[wait:timeout]
    assert '"old-process-timeout"' in BOOTSTRAP_RUNTIME[wait:filesystem_begin]
