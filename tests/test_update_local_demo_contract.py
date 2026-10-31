"""TDD contract for the QA-only local update demonstration fixture.

The helper is intentionally exercised as a pure, disposable-root boundary.  No
test in this file is allowed to touch the fixed QA payload, Cuevi, or a user
data root.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
from urllib.error import URLError
from urllib.request import urlopen
import zipfile

import pytest

try:
    from tools import prepare_local_update_demo as demo
except ImportError:  # RED baseline: the helper is intentionally absent.
    demo = None  # type: ignore[assignment]


DEMO_ROOT = demo.FIXED_DEMO_ROOT if demo is not None else Path.cwd().parent / "aris-local-update-demo-20260803"
MIRROR = "http://127.0.0.1:4328/harken-search-demo/"
OLD_VERSION = "1.0.2"
NEW_VERSION = "1.0.3"
OLD_BUILD = "local-demo-old-b76dd15d"
NEW_BUILD = "local-demo-new-b76dd15d"


@pytest.fixture(autouse=True)
def disposable_demo_signing_identity(monkeypatch):
    """Tests own their signing identity and never read production signing keys."""
    _require_demo()
    seed = base64.b64encode(os.urandom(32)).decode("ascii")
    monkeypatch.setenv("ARIS_UPDATE_LOCAL_DEMO_PRIVATE_KEY_B64", seed)
    monkeypatch.setenv("ARIS_UPDATE_LOCAL_DEMO_PUBLIC_KEY_HEX", demo.derive_public_key_hex(seed))
    monkeypatch.setenv("ARIS_UPDATE_LOCAL_DEMO_KEY_ID", "isolated-demo-test-key")
    monkeypatch.setenv("ARIS_UPDATE_SIGNING_KEY_B64", seed)
    monkeypatch.setenv("ARIS_UPDATE_PACKAGE_KEY_ID", "isolated-demo-test-key")


def _key_material() -> tuple[str, str, str]:
    _require_demo()
    private_b64 = os.environ.get("ARIS_UPDATE_LOCAL_DEMO_PRIVATE_KEY_B64", "")
    public_hex = os.environ.get("ARIS_UPDATE_LOCAL_DEMO_PUBLIC_KEY_HEX", "")
    key_id = os.environ.get("ARIS_UPDATE_LOCAL_DEMO_KEY_ID", "")
    assert private_b64 and public_hex and key_id
    return private_b64, public_hex, key_id


def _write_runtime_pair(
    root: Path, *, aris_bytes: bytes, bootstrap_bytes: bytes, build_id: str,
) -> dict[str, Path]:
    _require_demo()
    aris = root / "HarkenSearch.exe"
    aris_signature = root / "HarkenSearch.exe.sig"
    bootstrap = root / "HarkenSearchBootstrap.exe"
    signature = root / "HarkenSearchBootstrap.exe.sig"
    receipt = root / "harken-runtime-pair.json"
    root.mkdir(parents=True, exist_ok=True)
    aris.write_bytes(aris_bytes)
    aris_signature.write_bytes(hashlib.sha512(aris_bytes).digest()[:32])
    bootstrap.write_bytes(bootstrap_bytes)
    (root / "runtime" / "demo-runtime.dll").parent.mkdir(parents=True, exist_ok=True)
    (root / "runtime" / "demo-runtime.dll").write_bytes(
        b"runnable-runtime-asset-" + build_id.encode("ascii")
    )
    private_b64, public_hex, key_id = _key_material()
    del private_b64
    generated = subprocess.run(
        [
            sys.executable,
            "-B",
            str(Path(demo.__file__).with_name("generate_runtime_pair.py")),
            "finalize",
            "--aris", str(aris),
            "--bootstrap", str(bootstrap),
            "--bootstrap-sig", str(signature),
            "--receipt", str(receipt),
            "--source-commit", "b76dd15d5b36c25c7998cd6fa2e16ccfaab2045a",
            "--build-id", build_id,
            "--channel", "testing",
            "--config", "Release",
            "--key-id", key_id,
            "--public-key-hex", public_hex,
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    assert generated.returncode == 0, generated.stderr
    return {
        "aris": aris,
        "aris_signature": aris_signature,
        "bootstrap": bootstrap,
        "signature": signature,
        "receipt": receipt,
    }


def _write_pair(root: Path) -> dict[str, Path]:
    old_pair = _write_runtime_pair(
        root / "old",
        aris_bytes=b"local-demo-old-executable",
        bootstrap_bytes=b"local-demo-old-bootstrap",
        build_id=OLD_BUILD,
    )
    new_pair = _write_runtime_pair(
        root / "new",
        aris_bytes=b"local-demo-new-executable",
        bootstrap_bytes=b"local-demo-new-bootstrap",
        build_id=NEW_BUILD,
    )
    old = root / "old" / "HarkenSearch.exe"
    new = root / "new" / "HarkenSearch.exe"
    payload = root / "payload"
    payload.mkdir(parents=True, exist_ok=True)
    (payload / "HarkenSearch.exe").write_bytes(new.read_bytes())
    return {"old_pair": old_pair, "new_pair": new_pair, "payload": payload}


def _config(tmp_path: Path, *, root: Path | None = None, **overrides: object):
    _require_demo()
    private_b64, public_hex, key_id = _key_material()
    values: dict[str, object] = {
        "demo_root": root or (tmp_path / "demo-root"),
        "mirror_base_url": MIRROR,
        "old_version": OLD_VERSION,
        "new_version": NEW_VERSION,
        "old_build_id": OLD_BUILD,
        "new_build_id": NEW_BUILD,
        "private_key_b64": private_b64,
        "public_key_hex": public_hex,
        "key_id": key_id,
        "source_commit": "b76dd15d5b36c25c7998cd6fa2e16ccfaab2045a",
        "repository": "xindong/aris",
    }
    values.update(overrides)
    return demo.DemoConfig(**values)


def _require_demo() -> None:
    if demo is None:
        pytest.fail("prepare_local_update_demo.py is missing: local demo contract is not implemented")


def test_off_mode_preserves_canonical_stable_identity() -> None:
    _require_demo()
    root = Path(demo.__file__).parents[1]
    source = (root / "src/update/HarkenSearchUpdateComposition.cpp").read_text(encoding="utf-8")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "ARIS_UPDATE_LOCAL_DEMO" in cmake
    assert "CompanyCloudStableManifestUrl()" in source
    assert "UpdateChannel::Stable" in source
    config = (root / "src/update/UpdateReleaseConfig.h").read_text(encoding="utf-8")
    assert "manifest-harken-search-win-x64-stable.json" in config
    assert "ARIS_UPDATE_GITHUB_LATEST_DOWNLOAD_PREFIX" in config


def test_demo_persistence_root_is_compile_time_bound_and_off_mode_is_unchanged() -> None:
    root = Path(__file__).parents[1]
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    persistence = (root / "src/platform/update_persistence_port.cpp").read_text(
        encoding="utf-8"
    )
    assert "ARIS_UPDATE_LOCAL_DEMO_ROOT" in cmake
    assert "ARIS_UPDATE_LOCAL_DEMO_ROOT" in persistence
    assert "SHGetKnownFolderPath" in persistence
    assert "FOLDERID_LocalAppData" in persistence


def test_demo_persistence_root_reaches_authority_and_bootstrap_runtime() -> None:
    root = Path(__file__).parents[1]
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    authority = (root / "src/platform/update_bootstrap_authority_win32.cpp").read_text(
        encoding="utf-8"
    )
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    assert "ARIS_UPDATE_LOCAL_DEMO_ROOT" in cmake
    assert "ARIS_UPDATE_LOCAL_DEMO_ROOT" in authority
    assert "ARIS_UPDATE_LOCAL_DEMO_ROOT" in runtime


def test_demo_records_the_exact_trusted_root_failure_stage() -> None:
    root = Path(__file__).parents[1]
    filesystem = (
        root / "src/bootstrap/WindowsBootstrapFilesystemWin32.cpp"
    ).read_text(encoding="utf-8")

    assert "bootstrap-demo-roots.txt" in filesystem
    assert "WriteLocalDemoTrustedRootsDiagnostic" in filesystem


def test_demo_records_the_exact_filesystem_mutation_failure() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/HarkenSearchBootstrapRuntime.cpp").read_text(
        encoding="utf-8"
    )

    assert "bootstrap-demo-mutation.txt" in runtime
    assert "writeLocalDemoMutationDiagnostic" in runtime


def test_demo_records_every_committed_invocation_cleanup_result() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/HarkenSearchBootstrapRuntime.cpp").read_text(
        encoding="utf-8"
    )

    assert "bootstrap-demo-cleanup.txt" in runtime
    assert "cleanupCommittedInvocationWithDiagnostics" in runtime
    assert "status=%d backend=%d native=%u" in runtime
    assert runtime.count("dependencies.filesystem.CleanupCommittedInvocation()") == 0


def test_terminal_cleanup_successor_uses_local_user_only_least_privilege_transport() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    begin = runtime.index("class NativeTerminalCleanupPort")
    end = runtime.index("\n};", begin) + len("\n};")
    successor = runtime[begin:end]

    assert "PIPE_REJECT_REMOTE_CLIENTS" in successor
    assert "CurrentUserPipeSecurity" in successor
    assert "PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE" in successor
    assert "DUPLICATE_SAME_ACCESS" not in successor
    assert "SetHandleInformation(client.get(), HANDLE_FLAG_INHERIT, 0U)" in successor
    assert "SetHandleInformation(parentHandle.get(), HANDLE_FLAG_INHERIT, 0U)" in successor


def test_terminal_cleanup_handoff_is_exactly_framed_and_bounded() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    successor_begin = runtime.index("class NativeTerminalCleanupPort")
    successor_end = runtime.index("\n};", successor_begin) + len("\n};")
    successor = runtime[successor_begin:successor_end]
    channel_begin = runtime.index(
        "BootstrapTerminalChannelResult ReadAuthenticatedBootstrapTerminalChannel"
    )
    channel_end = runtime.index(
        "BootstrapTerminalParentWaitResult WaitForBootstrapTerminalParentExit",
        channel_begin,
    )
    channel = runtime[channel_begin:channel_end]

    assert "encodeTerminalCleanupFrame" in successor
    assert "FILE_FLAG_OVERLAPPED" in successor
    assert "FlushFileBuffers(server.get())" not in successor
    assert "server.reset()" in successor
    assert "readTerminalCleanupFrame" in channel
    assert "DecodeBootstrapTerminalCleanupHandoff" in channel


def test_product_entrypoint_injects_the_terminal_cleanup_successor() -> None:
    root = Path(__file__).parents[1]
    entrypoint = (root / "src/bootstrap/HarkenSearchBootstrapMain.cpp").read_text(
        encoding="utf-8"
    )
    begin = entrypoint.index("int RunHarkenSearchBootstrapEntrypoint")
    end = entrypoint.index("int RunHarkenSearchBootstrapTerminalCleanupEntrypoint", begin)
    product = entrypoint[begin:end]

    assert "CreateNativeBootstrapTerminalCleanupPort()" in product
    assert "terminalCleanup.get()" in product
    assert "terminal-cleanup-port" in product


def test_local_demo_records_terminal_channel_status_and_native_error() -> None:
    root = Path(__file__).parents[1]
    main = (root / "src/bootstrap/HarkenSearchBootstrapMain.cpp").read_text(
        encoding="utf-8"
    )
    terminal_begin = main.index("int RunHarkenSearchBootstrapTerminalCleanupEntrypoint")
    terminal_end = main.index("}  // namespace aris::bootstrap", terminal_begin)
    terminal = main[terminal_begin:terminal_end]

    assert "bootstrap-demo-terminal-channel.txt" in main
    assert "writeLocalDemoTerminalChannelDiagnostic(channel)" in terminal
    assert "channel.status" in main
    assert "channel.nativeError" in main
    assert "ARIS_UPDATE_LOCAL_DEMO" in main


def test_local_demo_records_terminal_filesystem_admission_details() -> None:
    root = Path(__file__).parents[1]
    main = (root / "src/bootstrap/HarkenSearchBootstrapMain.cpp").read_text(
        encoding="utf-8"
    )
    terminal_begin = main.index("int RunHarkenSearchBootstrapTerminalCleanupEntrypoint")
    terminal_end = main.index("}  // namespace aris::bootstrap", terminal_begin)
    terminal = main[terminal_begin:terminal_end]

    assert "recovery.filesystem.status" in main
    assert "recovery.filesystem.backendStatus" in main
    assert "recovery.filesystem.nativeError" in main
    assert "recovery.disposition" in main
    assert "writeLocalDemoTerminalRecoveryDiagnostic(recovery)" in terminal
    assert "terminal-filesystem-admission" in terminal


def test_local_demo_records_successor_create_context_before_launch() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    begin = runtime.index("class NativeTerminalCleanupPort")
    end = runtime.index("\n};", begin) + len("\n};")
    successor = runtime[begin:end]

    assert "bootstrap-demo-successor-create-context.txt" in runtime
    assert "writeLocalDemoSuccessorCreateContextDiagnostic" in successor
    assert "IsProcessInJob(GetCurrentProcess()" in runtime
    assert "QueryInformationJobObject(" in runtime
    assert "JobObjectExtendedLimitInformation" in runtime
    assert "GetHandleInformation" in runtime
    assert "GetFileType" in runtime
    assert "firstCreateProcessError" in successor
    assert successor.index("writeLocalDemoSuccessorCreateContextDiagnostic") < \
        successor.index("CreateProcessW")


def test_local_demo_successor_diagnostic_uses_real_newline_escapes() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )

    # The C++ source must contain one escape (\\n), so the file on disk gets
    # actual line breaks. Two escapes (\\\\n) leak a literal backslash into
    # the diagnostic and make the evidence hard to read or parse.
    assert 'first_create_process_error=%lu\\n' in runtime
    assert r'first_create_process_error=%lu\\n' not in runtime


def test_terminal_successor_is_verified_while_suspended_before_handoff() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    begin = runtime.index("class NativeTerminalCleanupPort")
    end = runtime.index("\n};", begin) + len("\n};")
    successor = runtime[begin:end]

    flags_begin = runtime.index("DWORD TerminalCleanupCreateFlags")
    flags_end = runtime.index("\n}", flags_begin) + len("\n}")
    create_flags = runtime[flags_begin:flags_end]

    assert "CREATE_SUSPENDED" in create_flags
    assert "return 0U" in create_flags
    assert "const DWORD successorCreateFlags = TerminalCleanupCreateFlags();" in successor
    assert "if (successorCreateFlags == 0U)" in successor
    assert "captureIdentity(successor.get(), successorIdentity)" in successor
    assert "imageIdentity.VolumeSerialNumber" in successor
    assert "imageIdentity.FileId.Identifier" in successor
    assert "ResumeThread(successorThread.get())" in successor
    assert successor.index("captureIdentity(successor.get(), successorIdentity)") < \
        successor.index("ResumeThread(successorThread.get())") < \
        successor.index("connectTerminalCleanupPipe")


def test_terminal_successor_requires_a_pre_exit_ready_ack() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    main = (root / "src/bootstrap/HarkenSearchBootstrapMain.cpp").read_text(
        encoding="utf-8"
    )
    successor_begin = runtime.index("class NativeTerminalCleanupPort")
    successor_end = runtime.index("\n};", successor_begin) + len("\n};")
    successor = runtime[successor_begin:successor_end]
    terminal_begin = main.index("int RunHarkenSearchBootstrapTerminalCleanupEntrypoint")
    terminal_end = main.index("}  // namespace aris::bootstrap", terminal_begin)
    terminal = main[terminal_begin:terminal_end]

    assert "CreatePipe" in successor
    assert "readTerminalReadyFrame" in successor
    assert "processInfo.dwProcessId" in successor
    assert "SignalBootstrapTerminalReady" in terminal
    assert terminal.index("SignalBootstrapTerminalReady") < \
        terminal.index("WaitForBootstrapTerminalParentExit")


def test_terminal_successor_launch_does_not_accept_a_failed_process_probe() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    begin = runtime.index("class NativeTerminalCleanupPort")
    end = runtime.index("\n};", begin) + len("\n};")
    successor = runtime[begin:end]

    assert "successorState == WAIT_FAILED" in successor
    assert "ERROR_PROCESS_ABORTED" in successor


def test_terminal_handoff_pipe_name_uses_os_randomness() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    begin = runtime.index("class NativeTerminalCleanupPort")
    end = runtime.index("\n};", begin) + len("\n};")
    successor = runtime[begin:end]

    assert "createBrokerNonce" in successor
    assert "pipeNonceHex" in successor


def test_terminal_cleanup_retries_only_typed_access_denied_operations() -> None:
    root = Path(__file__).parents[1]
    main = (root / "src/bootstrap/HarkenSearchBootstrapMain.cpp").read_text(
        encoding="utf-8"
    )
    begin = main.index("int RunHarkenSearchBootstrapTerminalCleanupEntrypoint")
    end = main.index("}  // namespace aris::bootstrap", begin)
    terminal = main[begin:end]

    assert "runTerminalCleanupWithBoundedRetry" in terminal
    assert terminal.count("runTerminalCleanupWithBoundedRetry") >= 3
    assert "RetryableAccessDenied" in main


def test_terminal_successor_accepts_only_remove_backup_intents() -> None:
    root = Path(__file__).parents[1]
    main = (root / "src/bootstrap/HarkenSearchBootstrapMain.cpp").read_text(
        encoding="utf-8"
    )
    begin = main.index("int RunHarkenSearchBootstrapTerminalCleanupEntrypoint")
    end = main.index("}  // namespace aris::bootstrap", begin)
    terminal = main[begin:end]

    # Terminal cleanup is a narrow successor capability.  It may consume only
    # the signed RemoveBackup intent with no path or PID authority; restore,
    # replacement, quarantine, and every other intent must be rejected before
    # Apply() is reached.
    assert "intent.kind != BootstrapIntentKind::RemoveBackup" in terminal
    assert "!intent.relativePath.empty()" in terminal
    assert "intent.processId != 0U" in terminal
    gate = terminal.index("intent.kind != BootstrapIntentKind::RemoveBackup")
    apply_call = terminal.index("filesystem.Apply(intent)")
    assert gate < apply_call


def test_terminal_child_clears_inherited_handle_flags_before_processing() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    begin = runtime.index("int WINAPI wWinMain")
    end = runtime.index("#endif", begin)
    entrypoint = runtime[begin:end]

    assert "clearTerminalInheritedHandleFlags" in entrypoint
    clear = entrypoint.index("clearTerminalInheritedHandleFlags")
    invoke = entrypoint.index("RunHarkenSearchBootstrapTerminalCleanupEntrypoint")
    assert clear < invoke

    helper_begin = runtime.index("bool clearTerminalInheritedHandleFlags")
    helper_end = runtime.index("\n}\n", helper_begin) + len("\n}")
    helper = runtime[helper_begin:helper_end]
    assert helper.count("SetHandleInformation") == 3
    assert "HANDLE_FLAG_INHERIT, 0U" in helper


def test_terminal_frame_header_preserves_oversize_diagnostics() -> None:
    root = Path(__file__).parents[1]
    runtime = (root / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )

    assert "enum class TerminalCleanupHeaderStatus" in runtime
    assert runtime.count("TerminalCleanupHeaderStatus::TooLarge") >= 3
    assert "headerStatus == TerminalCleanupHeaderStatus::TooLarge" in runtime


def test_transaction_namespace_allows_only_the_owned_runtime_pair() -> None:
    root = Path(__file__).parents[1]
    filesystem = (
        root / "src/bootstrap/WindowsBootstrapFilesystemWin32.cpp"
    ).read_text(encoding="utf-8")

    assert "ValidateRuntimePairNamespace" in filesystem
    assert "kRuntimePairDirectoryName" in filesystem
    assert "kRuntimePairBootstrapName" in filesystem
    assert "EqualPathInsensitive(name, kRuntimePairDirectoryName)" in filesystem
    assert "EqualPathInsensitive(name, kRuntimePairBootstrapName)" in filesystem
    assert "ValidateSupportedMetadata(opened.handle.get())" in filesystem
    assert "ERROR_PATH_NOT_FOUND" in filesystem


def test_runtime_pair_authority_is_rechecked_before_each_journal_write() -> None:
    root = Path(__file__).parents[1]
    filesystem = (
        root / "src/bootstrap/WindowsBootstrapFilesystem.cpp"
    ).read_text(encoding="utf-8")

    assert "verifyRuntimePairAuthority" in filesystem
    assert "verifyRuntimePairAuthority(invocationCleanupStarted)" in filesystem


def test_runtime_pair_failure_cleanup_rechecks_exact_identity_before_delete() -> None:
    root = Path(__file__).parents[1]
    source = (
        root / "src/platform/update_bootstrap_authority_win32.cpp"
    ).read_text(encoding="utf-8")
    begin = source.index("class TransactionRuntimePairCleanup")
    end = source.index("class InvocationMutationCleanup", begin)
    cleanup = source[begin:end]

    assert "ObserveRuntimePairHandle" in cleanup
    assert "SameRuntimePairFile" in cleanup
    assert "DeleteFileByHandle" in cleanup


@pytest.mark.parametrize(
    "field,value",
    [
        ("mirror_base_url", "file:///tmp/harken-search-demo/"),
        ("mirror_base_url", "http://localhost:4328/harken-search-demo/"),
        ("mirror_base_url", "http://127.0.0.1:4328/harken-search-demo"),
        ("mirror_base_url", "http://0.0.0.0:4328/harken-search-demo/"),
        ("mirror_base_url", "http://192.168.1.5:4328/harken-search-demo/"),
        ("mirror_base_url", "http://127.0.0.1:4328/harken-search-demo/../other/"),
        ("new_version", "1.0.2"),
        ("new_version", "1.0.1"),
        ("new_build_id", OLD_BUILD),
        ("private_key_b64", ""),
        ("public_key_hex", "00" * 32),
    ],
)
def test_invalid_demo_identity_fails_closed(tmp_path: Path, field: str, value: object) -> None:
    _require_demo()
    config = _config(tmp_path, **{field: value})
    with pytest.raises(ValueError):
        demo.validate_config(config, repo_root=tmp_path / "repo")


@pytest.mark.parametrize(
    "url",
    [
        "HTTP://127.0.0.1:4328/harken-search-demo/",
        "http://127.0.0.1:04328/harken-search-demo/",
        "http://127.0.0.1:4328/%61ris-demo/",
        "http://127.0.0.1:4328/harken-search-demo/%2e%2e/",
    ],
)
def test_demo_mirror_requires_exact_byte_spelling(tmp_path: Path, url: str) -> None:
    _require_demo()
    with pytest.raises(ValueError):
        demo.validate_config(
            _config(tmp_path, mirror_base_url=url),
            repo_root=tmp_path / "repo",
        )


def test_demo_root_reparse_alias_is_rejected(tmp_path: Path) -> None:
    _require_demo()
    target = tmp_path / "real-demo-root"
    alias = tmp_path / "demo-root-alias"
    target.mkdir()
    try:
        alias.symlink_to(target, target_is_directory=True)
    except OSError as error:
        pytest.skip(f"directory symlink is unavailable: {error}")
    with pytest.raises(ValueError):
        demo.validate_config(_config(tmp_path, root=alias), repo_root=tmp_path / "repo")


def test_demo_build_ids_are_bound_to_the_approved_old_new_pair(tmp_path: Path) -> None:
    _require_demo()
    for field in ("old_build_id", "new_build_id"):
        config = _config(tmp_path, **{field: "local-demo-other-b76dd15d"})
        with pytest.raises(ValueError):
            demo.validate_config(config, repo_root=tmp_path / "repo")


@pytest.fixture(autouse=True)
def isolated_demo_signing_identity(monkeypatch: pytest.MonkeyPatch) -> None:
    # Every case owns disposable keys; never depend on real release credentials.
    _install_demo_identity(monkeypatch)


def _install_demo_identity(monkeypatch: pytest.MonkeyPatch) -> tuple[str, str, str]:
    _require_demo()
    seed = os.urandom(32)
    private_b64 = base64.b64encode(seed).decode("ascii")
    public_hex = demo.derive_public_key_hex(private_b64)
    key_id = "local-demo-key"
    monkeypatch.setenv("ARIS_UPDATE_LOCAL_DEMO_PRIVATE_KEY_B64", private_b64)
    monkeypatch.setenv("ARIS_UPDATE_LOCAL_DEMO_PUBLIC_KEY_HEX", public_hex)
    monkeypatch.setenv("ARIS_UPDATE_LOCAL_DEMO_KEY_ID", key_id)
    monkeypatch.setenv("ARIS_UPDATE_SIGNING_KEY_B64", private_b64)
    monkeypatch.setenv("ARIS_UPDATE_PACKAGE_KEY_ID", key_id)
    return private_b64, public_hex, key_id


def test_demo_root_cannot_overlap_cuevi_root_from_environment(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _require_demo()
    private_b64, public_hex, key_id = _install_demo_identity(monkeypatch)
    cuevi = tmp_path / "custom-cuevi" / ".cuevi"
    cuevi.mkdir(parents=True)
    monkeypatch.setenv("CUEVI_ROOT", str(cuevi))
    config = _config(
        tmp_path,
        root=cuevi / "nested-demo",
        private_key_b64=private_b64,
        public_key_hex=public_hex,
        key_id=key_id,
    )
    with pytest.raises(ValueError, match="overlaps"):
        demo.validate_config(config, repo_root=tmp_path / "repo")


def test_demo_payload_cannot_be_sourced_from_cuevi_root(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _require_demo()
    _install_demo_identity(monkeypatch)
    cuevi = tmp_path / "custom-cuevi" / ".cuevi"
    payload = cuevi / "payload"
    payload.mkdir(parents=True)
    (payload / "HarkenSearch.exe").write_bytes(b"payload")
    monkeypatch.setenv("CUEVI_ROOT", str(cuevi))
    repo = tmp_path / "repo"
    repo.mkdir()
    protected = demo._cuevi_protected_paths(repo)
    assert any(_paths_overlap(payload, item) for item in protected)


def _paths_overlap(left: Path, right: Path) -> bool:
    try:
        left.resolve().relative_to(right.resolve())
        return True
    except ValueError:
        try:
            right.resolve().relative_to(left.resolve())
            return True
        except ValueError:
            return False


def test_demo_root_cannot_be_inside_product_or_user_roots(tmp_path: Path) -> None:
    _require_demo()
    repo = tmp_path / "repo"
    for root in (repo / "dist_qa" / "Airs", repo / "source-audio"):
        config = _config(tmp_path, root=root)
        with pytest.raises(ValueError):
            demo.validate_config(config, repo_root=repo)

    roaming = tmp_path / "AppData"
    local = tmp_path / "LocalAppData"
    before_roaming = os.environ.get("APPDATA")
    before_local = os.environ.get("LOCALAPPDATA")
    try:
        os.environ["APPDATA"] = str(roaming)
        os.environ["LOCALAPPDATA"] = str(local)
        for root in (roaming / "AuroAudio", local / "Auro" / "Database"):
            config = _config(tmp_path, root=root)
            with pytest.raises(ValueError):
                demo.validate_config(config, repo_root=repo)
    finally:
        if before_roaming is None:
            os.environ.pop("APPDATA", None)
        else:
            os.environ["APPDATA"] = before_roaming
        if before_local is None:
            os.environ.pop("LOCALAPPDATA", None)
        else:
            os.environ["LOCALAPPDATA"] = before_local


def test_prepare_publishes_complete_generation_and_nonsecret_ledger(tmp_path: Path) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    fixture = demo.prepare_demo_fixture(
        config,
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )
    assert fixture.generation_dir.is_dir()
    required = {
        "manifest-win-x64-testing.json",
        "manifest-win-x64-testing.json.sig",
        "Aris-1.0.3-win-x64-update.zip",
        "ArisBootstrap.package.json",
        "ArisBootstrap.package.json.sig",
        "generation.json",
    }
    names = {path.name for path in fixture.generation_dir.rglob("*") if path.is_file()}
    assert required <= names
    ledger = json.loads((fixture.generation_dir / "generation.json").read_text(encoding="utf-8"))
    assert ledger["oldVersion"] == OLD_VERSION
    assert ledger["newVersion"] == NEW_VERSION
    assert ledger["oldBuildId"] == OLD_BUILD
    assert ledger["newBuildId"] == NEW_BUILD
    assert ledger["pair"]["old"]["receiptSha256"] == hashlib.sha256(
        inputs["old_pair"]["receipt"].read_bytes()
    ).hexdigest()
    assert ledger["pair"]["new"]["receiptSha256"] == hashlib.sha256(
        inputs["new_pair"]["receipt"].read_bytes()
    ).hexdigest()
    assert (
        fixture.generation_dir / "pair" / "new" / "HarkenSearchBootstrap.exe"
    ).read_bytes() == inputs["new_pair"]["bootstrap"].read_bytes()
    assert (
        fixture.generation_dir / "pair" / "old" / "runtime" / "demo-runtime.dll"
    ).read_bytes() == inputs["old_pair"]["aris"].parent.joinpath(
        "runtime", "demo-runtime.dll"
    ).read_bytes()
    assert config.private_key_b64 not in (fixture.generation_dir / "generation.json").read_text(encoding="utf-8")
    assert ledger["artifacts"]["zipSha256"] == hashlib.sha256(
        fixture.update_zip.read_bytes()
    ).hexdigest()


def test_signed_demo_inventory_replaces_the_complete_runtime_pair(
    tmp_path: Path,
) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    fixture = demo.prepare_demo_fixture(
        _config(tmp_path),
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )

    package = json.loads(fixture.package_plan.read_text(encoding="utf-8"))
    inventory = {
        item["relative_path"]: item
        for item in package["inventory"]
    }
    expected_sources = {
        "HarkenSearch.exe": inputs["new_pair"]["aris"],
        "HarkenSearch.exe.sig": inputs["new_pair"]["aris_signature"],
        "HarkenSearchBootstrap.exe": inputs["new_pair"]["bootstrap"],
        "HarkenSearchBootstrap.exe.sig": inputs["new_pair"]["signature"],
        "harken-runtime-pair.json": inputs["new_pair"]["receipt"],
    }
    assert set(expected_sources) <= set(inventory)
    with zipfile.ZipFile(fixture.update_zip) as archive:
        for relative, source in expected_sources.items():
            payload = archive.read(relative)
            assert payload == source.read_bytes()
            assert inventory[relative] == {
                "byte_size": len(payload),
                "relative_path": relative,
                "sha256": hashlib.sha256(payload).hexdigest(),
            }


def test_generation_tamper_and_stale_identity_are_rejected(tmp_path: Path) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    fixture = demo.prepare_demo_fixture(
        config,
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )
    fixture.update_zip.write_bytes(fixture.update_zip.read_bytes() + b"tamper")
    with pytest.raises(ValueError):
        demo.verify_generation(fixture.generation_dir, config)


def test_failed_publish_keeps_previous_generation(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    first = demo.prepare_demo_fixture(
        config,
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )
    before = first.update_zip.read_bytes()
    monkeypatch.setattr(demo, "_atomic_replace_generation", lambda *_args, **_kwargs: (_ for _ in ()).throw(OSError("injected")))
    with pytest.raises(OSError):
        demo.prepare_demo_fixture(
            config,
            old_pair_dir=inputs["old_pair"]["aris"].parent,
            new_pair_dir=inputs["new_pair"]["aris"].parent,
            payload_dir=inputs["payload"],
        )
    assert first.update_zip.read_bytes() == before


def test_post_commit_previous_cleanup_failure_does_not_report_publish_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _require_demo()
    current = tmp_path / "harken-search-demo"
    staged = tmp_path / (".generation-" + "a" * 64 + ".tmp")
    current.mkdir()
    staged.mkdir()
    (current / "identity.txt").write_bytes(b"old")
    (staged / "identity.txt").write_bytes(b"new")
    original_rmtree = shutil.rmtree

    def fail_previous_cleanup(path: object, *args: object, **kwargs: object) -> None:
        if Path(path).name == "harken-search-demo.previous":
            raise OSError("injected previous cleanup failure")
        original_rmtree(path, *args, **kwargs)

    monkeypatch.setattr(demo.shutil, "rmtree", fail_previous_cleanup)
    demo._atomic_replace_generation(staged, current)
    assert (current / "identity.txt").read_bytes() == b"new"
    assert (tmp_path / "harken-search-demo.previous" / "identity.txt").read_bytes() == b"old"
    assert not staged.exists()


def test_runtime_pair_receipt_tamper_and_stale_generation_are_rejected(
    tmp_path: Path,
) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    old_receipt = inputs["old_pair"]["receipt"]
    document = json.loads(old_receipt.read_text(encoding="utf-8"))
    document["buildId"] = NEW_BUILD
    old_receipt.write_text(
        json.dumps(document, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
    )
    with pytest.raises(ValueError):
        demo.prepare_demo_fixture(
            config,
            old_pair_dir=inputs["old_pair"]["aris"].parent,
            new_pair_dir=inputs["new_pair"]["aris"].parent,
            payload_dir=inputs["payload"],
        )
    assert not (config.demo_root / "http-root" / "harken-search-demo").exists()


def test_generation_ledger_must_bind_the_same_generation(tmp_path: Path) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    fixture = demo.prepare_demo_fixture(
        config,
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )
    ledger = json.loads(fixture.ledger.read_text(encoding="utf-8"))
    ledger["generation"] = "0" * 64
    fixture.ledger.write_text(
        json.dumps(ledger, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
    )
    with pytest.raises(ValueError):
        demo.verify_generation(fixture.generation_dir, config)


def test_generation_rejects_valid_pair_from_a_different_signed_zip(
    tmp_path: Path,
) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    fixture = demo.prepare_demo_fixture(
        config,
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )
    alternate = _write_runtime_pair(
        tmp_path / "alternate-new",
        aris_bytes=b"different-new-aris-bytes",
        bootstrap_bytes=b"different-new-bootstrap-bytes",
        build_id=NEW_BUILD,
    )
    published_new = fixture.generation_dir / "pair" / "new"
    shutil.rmtree(published_new)
    shutil.copytree(alternate["aris"].parent, published_new)
    ledger = json.loads(fixture.ledger.read_text(encoding="utf-8"))
    old_pair = demo._pair_artifacts(fixture.generation_dir / "pair" / "old")
    new_pair = demo._pair_artifacts(published_new)
    ledger["pair"]["new"] = {
        **demo._pair_hashes(new_pair),
        "files": demo._pair_tree_hashes(published_new),
    }
    ledger["generation"] = demo._generation_id(config, old_pair, new_pair)
    fixture.ledger.write_bytes(demo._canonical_json(ledger))
    with pytest.raises(ValueError):
        demo.verify_generation(fixture.generation_dir, config)


def test_package_plan_rejects_duplicate_zip_member(tmp_path: Path) -> None:
    _require_demo()
    config = _config(tmp_path)
    payload = tmp_path / "HarkenSearch.exe"
    bootstrap = tmp_path / "HarkenSearchBootstrap.exe"
    archive_path = tmp_path / "duplicate.zip"
    payload.write_bytes(b"current-aris")
    bootstrap.write_bytes(b"current-bootstrap")
    with zipfile.ZipFile(archive_path, "w") as archive:
        archive.writestr("HarkenSearch.exe", b"stale-aris")
        with pytest.warns(UserWarning, match="Duplicate name"):
            archive.writestr("HarkenSearch.exe", payload.read_bytes())
        archive.writestr("HarkenSearchBootstrap.exe", bootstrap.read_bytes())
    plan, signature = demo.build_package_plan(
        channel="testing",
        version=NEW_VERSION,
        build_id=NEW_BUILD,
        asset_path=archive_path,
        inventory=[{"relative_path": "HarkenSearch.exe", "source_path": payload}],
        output_dir=tmp_path,
        key_id=config.key_id,
    )
    with pytest.raises(ValueError):
        demo._verify_package(plan, signature, archive_path, config)


def test_generation_ledger_rejects_unknown_fields(tmp_path: Path) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    fixture = demo.prepare_demo_fixture(
        config,
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )
    original = json.loads(fixture.ledger.read_text(encoding="utf-8"))
    mutations = (
        lambda ledger: ledger.__setitem__("unexpected", True),
        lambda ledger: ledger["artifacts"].__setitem__("unexpected", "0" * 64),
        lambda ledger: ledger["pair"].__setitem__("unexpected", {}),
    )
    for mutate in mutations:
        ledger = json.loads(json.dumps(original))
        mutate(ledger)
        fixture.ledger.write_bytes(demo._canonical_json(ledger))
        with pytest.raises(ValueError):
            demo.verify_generation(fixture.generation_dir, config)


def test_loopback_server_serves_only_verified_generation_and_stops_cleanly(
    tmp_path: Path,
) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    fixture = demo.prepare_demo_fixture(
        config,
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )
    server = demo.create_loopback_server(config)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        manifest_url = MIRROR + "releases/latest/download/manifest-win-x64-testing.json"
        assert urlopen(manifest_url, timeout=2).read() == fixture.manifest.read_bytes()
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
    assert not thread.is_alive()
    with pytest.raises(URLError):
        urlopen(MIRROR + "generation.json", timeout=1)


def test_loopback_server_rejects_path_pollution_and_queries(tmp_path: Path) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    demo.prepare_demo_fixture(
        config,
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )
    server = demo.create_loopback_server(config)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for suffix in (
            "generation.json",
            "pair/old/HarkenSearch.exe",
            "releases/latest/download/manifest-win-x64-testing.json?polluted=1",
            "../harken-search-demo/releases/latest/download/manifest-win-x64-testing.json",
        ):
            with pytest.raises(URLError):
                urlopen(MIRROR + suffix, timeout=2)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def test_loopback_server_rejects_artifact_replaced_after_start(
    tmp_path: Path,
) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    fixture = demo.prepare_demo_fixture(
        config,
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )
    server = demo.create_loopback_server(config)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        served_manifest = (
            fixture.generation_dir
            / "releases" / "latest" / "download"
            / "manifest-win-x64-testing.json"
        )
        served_manifest.write_bytes(served_manifest.read_bytes() + b"tamper")
        with pytest.raises(URLError):
            urlopen(
                MIRROR + "releases/latest/download/manifest-win-x64-testing.json",
                timeout=2,
            )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def test_fixture_rejects_mismatched_generic_demo_signing_identity(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    _require_demo()
    inputs = _write_pair(tmp_path / "inputs")
    config = _config(tmp_path)
    monkeypatch.setenv(
        "ARIS_UPDATE_SIGNING_KEY_B64",
        base64.b64encode(b"x" * 32).decode("ascii"),
    )
    monkeypatch.setenv("ARIS_UPDATE_PACKAGE_KEY_ID", "wrong-demo-key")
    with pytest.raises(ValueError):
        demo.prepare_demo_fixture(
            config,
            old_pair_dir=inputs["old_pair"]["aris"].parent,
            new_pair_dir=inputs["new_pair"]["aris"].parent,
            payload_dir=inputs["payload"],
        )


def test_package_script_cancel_does_not_require_pair_arguments(tmp_path: Path) -> None:
    _require_demo()
    script = Path(demo.__file__).parents[1] / "scripts" / "package_local_update_demo.ps1"
    powershell = os.environ.get("SystemRoot", r"C:\Windows") + r"\System32\WindowsPowerShell\v1.0\powershell.exe"
    result = subprocess.run(
        [
            powershell, "-NoProfile", "-NonInteractive", "-File", str(script),
            "-Cancel", "-DemoRoot", str(tmp_path / "demo-root"),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr or result.stdout




def test_loopback_server_refuses_port_conflict_and_unverified_generation(
    tmp_path: Path,
) -> None:
    _require_demo()
    config = _config(tmp_path)
    with pytest.raises(ValueError):
        demo.create_loopback_server(config)
    inputs = _write_pair(tmp_path / "inputs")
    demo.prepare_demo_fixture(
        config,
        old_pair_dir=inputs["old_pair"]["aris"].parent,
        new_pair_dir=inputs["new_pair"]["aris"].parent,
        payload_dir=inputs["payload"],
    )
    blocker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    blocker.bind(("127.0.0.1", 4328))
    blocker.listen(1)
    try:
        with pytest.raises(OSError):
            demo.create_loopback_server(config)
    finally:
        blocker.close()


def test_cancel_removes_only_incomplete_generation(tmp_path: Path) -> None:
    _require_demo()
    config = _config(tmp_path)
    root = config.demo_root / "http-root"
    current = root / "harken-search-demo"
    incomplete = root / (".generation-" + "a" * 64 + ".tmp")
    current.mkdir(parents=True)
    incomplete.mkdir(parents=True)
    sentinel = current / "sentinel.bin"
    sentinel.write_bytes(b"keep-current")
    demo.cancel_demo(config)
    assert sentinel.read_bytes() == b"keep-current"
    assert not incomplete.exists()


def test_composition_has_demo_testing_identity_and_transport_only_switch() -> None:
    _require_demo()
    source = (Path(demo.__file__).parents[1] / "src/update/HarkenSearchUpdateComposition.cpp").read_text(encoding="utf-8")
    cmake = (Path(demo.__file__).parents[1] / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "UpdateChannel::Testing" in source
    assert "manifest-win-x64-testing.json" in source
    assert "ARIS_UPDATE_MIRROR_BASE_URL" in cmake
    assert "127.0.0.1:4328/harken-search-demo/" in cmake


def test_demo_build_provenance_is_cross_bound() -> None:
    _require_demo()
    root = Path(demo.__file__).parents[1]
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    script = (root / "scripts/package_local_update_demo.ps1").read_text(encoding="utf-8")
    assert "ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION STREQUAL ARIS_UPDATE_LOCAL_DEMO_OLD_VERSION" in cmake
    assert "ARIS_UPDATE_LOCAL_DEMO_BUILD_ID STREQUAL ARIS_UPDATE_LOCAL_DEMO_OLD_BUILD_ID" in cmake
    assert 'set(AIRS_BUILD_ID "${ARIS_UPDATE_LOCAL_DEMO_BUILD_ID}"' in cmake
    assert 'set(AIRS_RELEASE_CHANNEL "testing"' in cmake
    assert 'git status --porcelain' in cmake
    assert 'git status --porcelain' in script
    assert "SourceCommit does not match the clean worktree HEAD" in script


def _configure_demo_cmake(tmp_path: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    _require_demo()
    root = Path(demo.__file__).parents[1]
    private_b64, public_hex, key_id = _key_material()
    del private_b64
    args = [
        "cmake", "-S", str(root), "-B", str(tmp_path / "cmake-build"),
        "-DARIS_UPDATE_LOCAL_DEMO=ON",
        "-DARIS_UPDATE_LOCAL_DEMO_BASE_URL=http://127.0.0.1:4328/harken-search-demo/",
        "-DARIS_UPDATE_LOCAL_DEMO_OLD_VERSION=1.0.2",
        "-DARIS_UPDATE_LOCAL_DEMO_NEW_VERSION=1.0.3",
        "-DARIS_UPDATE_LOCAL_DEMO_OLD_BUILD_ID=local-demo-old-b76dd15d",
        "-DARIS_UPDATE_LOCAL_DEMO_NEW_BUILD_ID=local-demo-new-b76dd15d",
        "-DARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION=1.0.3",
        "-DARIS_UPDATE_LOCAL_DEMO_BUILD_ID=local-demo-new-b76dd15d",
        f"-DARIS_UPDATE_LOCAL_DEMO_SOURCE_COMMIT={subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()}",
        f"-DARIS_BOOTSTRAP_PACKAGE_KEY_ID={key_id}",
        f"-DARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX={public_hex}",
        *extra,
    ]
    return subprocess.run(args, capture_output=True, text=True, check=False)


def test_demo_on_locks_channel_and_build_id_without_explicit_airs_overrides(tmp_path: Path) -> None:
    del tmp_path
    cmake = (Path(demo.__file__).parents[1] / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'set(AIRS_RELEASE_CHANNEL "testing" CACHE STRING' in cmake
    assert 'set(AIRS_BUILD_ID "${ARIS_UPDATE_LOCAL_DEMO_BUILD_ID}" CACHE STRING' in cmake
    assert 'set(AIRS_BUILD_ID "${ARIS_UPDATE_LOCAL_DEMO_BUILD_ID}" CACHE STRING' in cmake


@pytest.mark.parametrize("override", [
    "-DAIRS_RELEASE_CHANNEL=stable",
    "-DAIRS_BUILD_ID=wrong-build-id",
])
def test_demo_on_rejects_wrong_airs_identity(tmp_path: Path, override: str) -> None:
    del tmp_path, override
    cmake = (Path(demo.__file__).parents[1] / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "ARIS_UPDATE_LOCAL_DEMO rejects an overridden AIRS_RELEASE_CHANNEL" in cmake
    assert "ARIS_UPDATE_LOCAL_DEMO rejects an overridden AIRS_BUILD_ID" in cmake


def test_demo_off_keeps_developer_airs_identity(tmp_path: Path) -> None:
    del tmp_path
    cmake = (Path(demo.__file__).parents[1] / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'set(AIRS_BUILD_ID "dev" CACHE STRING' in cmake
    assert 'set(AIRS_RELEASE_CHANNEL "dev" CACHE STRING' in cmake


def test_demo_cli_routes_real_update_state_machine_and_is_demo_gated() -> None:
    root = Path(demo.__file__).parents[1]
    main = (root / "src/main.cpp").read_text(encoding="utf-8")
    windows_main = (root / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    assert "--harken-search-demo-cli" in windows_main
    assert "ARIS_UPDATE_LOCAL_DEMO" in windows_main
    assert "RequestHarkenSearchUpdateCheck" in windows_main
    assert "ConfirmHarkenSearchUpdateRestart" in windows_main
    assert "ConsumeHarkenSearchUpdateGracefulExitRequest" in windows_main
    assert "StartHarkenSearchAutomaticUpdate" in windows_main
    assert "--harken-search-demo-cli" not in main


def test_demo_cli_run_is_single_process_real_confirm_flow() -> None:
    root = Path(demo.__file__).parents[1]
    windows_main = (root / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    assert 'command != "run"' in windows_main
    assert 'command == "run"' in windows_main
    assert "GetHarkenSearchUpdateUiState" in windows_main
    assert "ConfirmHarkenSearchUpdateRestart" in windows_main
    assert "ConsumeHarkenSearchUpdateGracefulExitRequest" in windows_main
    assert "confirmAccepted" in windows_main
    assert "appliedDisposition" in windows_main
    assert "gracefulObserved" in windows_main
    assert "appliedDetail" in windows_main


def test_update_composition_surfaces_bootstrap_submit_failure_as_error() -> None:
    root = Path(demo.__file__).parents[1]
    composition = (root / "src/update/HarkenSearchUpdateComposition.cpp").read_text(encoding="utf-8")
    assert "Bootstrap handoff failed" in composition
    assert "g_updateUiState.phase = HarkenSearchUpdateUiPhase::Error" in composition
    assert "applied.detail" in composition


@pytest.mark.parametrize("command", ["check", "status", "health", "confirm", "restart"])
def test_demo_cli_commands_have_machine_readable_contract(command: str) -> None:
    root = Path(demo.__file__).parents[1]
    windows_main = (root / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    assert f'"{command}"' in windows_main
    assert '\\"schema\\":1' in windows_main
    assert '\\"status\\"' in windows_main
