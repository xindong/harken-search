"""Separate-process ArisBootstrap composition smoke test.

The fixture key is explicit and test-owned.  The production target has no
fallback key; this script only exercises the independently built fixture-key
composition so the real channel, process, health, filesystem, and journal
ports are traversed in one child process.
"""

from __future__ import annotations

import hashlib
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass

from nacl.signing import SigningKey

MONITOR_TIMEOUT_EXIT_CODE = 0xE195


@dataclass(frozen=True)
class Case:
    name: str
    expected_exit: int
    mode: str = "normal"
    payload: str = "health"
    build_id: str = "blackbox-build"
    signature_valid: bool = True
    tamper_staged: bool = False
    forged_roots: bool = False
    forged_target: bool = False
    expected_install: str = "old"
    sleep_ms: int | None = None
    expected_diagnostic: str | None = None
    pre_mutation_rejection: bool = False
    pre_mutation_marker: str | None = None
    max_elapsed_seconds: float | None = None
    inventory_relative_path: str | None = None
    new_inventory_files: tuple[str, ...] = ()
    install_parent_is_file: bool = False
    removals: tuple[str, ...] = ()
    installer_mutex_held: bool = False


def _hold_installer_mutex(install: Path):
    class FileIdInfo(ctypes.Structure):
        _fields_ = [("volume_serial", ctypes.c_uint64),
                    ("file_id", ctypes.c_ubyte * 16)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32,
                                   ctypes.c_uint32, ctypes.c_void_p,
                                   ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.GetFileInformationByHandleEx.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                                    ctypes.c_void_p, ctypes.c_uint32]
    kernel.GetFileInformationByHandleEx.restype = ctypes.c_int
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
    kernel.CreateMutexW.restype = ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.WaitForSingleObject.restype = ctypes.c_uint32
    kernel.ReleaseMutex.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    directory = kernel.CreateFileW(str(install), 0x80, 7, None, 3, 0x02000000, None)
    if directory == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), "open install directory")
    try:
        identity = FileIdInfo()
        if not kernel.GetFileInformationByHandleEx(directory, 18,
                                                   ctypes.byref(identity), ctypes.sizeof(identity)):
            raise OSError(ctypes.get_last_error(), "read install directory identity")
    finally:
        kernel.CloseHandle(directory)
    name = ("Global\\HarkenSearchInstallMutation_" +
            f"{identity.volume_serial:016X}" +
            "".join(f"{byte:02X}" for byte in identity.file_id))
    mutex = kernel.CreateMutexW(None, False, name)
    if not mutex or kernel.WaitForSingleObject(mutex, 0) not in (0, 0x80):
        raise OSError(ctypes.get_last_error(), "hold installer mutex")
    return kernel, mutex


def _case_payload(case: Case, health_fixture: Path, sleep_fixture: Path) -> bytes:
    if case.payload == "invalid-executable":
        return b"signed package payload that is not a Windows executable"
    source = health_fixture if case.payload == "health" else sleep_fixture
    return source.read_bytes()


def _file_identity(path: Path) -> tuple[int, int, int, int]:
    """Return the stable Windows file identity values exposed by pathlib."""
    stat = path.stat()
    return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)


def _run_case(
    case: Case,
    bootstrap: Path,
    helper: Path,
    health_fixture: Path,
    sleep_fixture: Path,
) -> tuple[bool, str]:
    sandbox = Path.cwd().resolve() / (
        f"aris-bootstrap-blackbox-{case.name}-{os.getpid()}-{time.time_ns()}"
    )
    install = sandbox / "install"
    updates = sandbox / "updates"
    invocation_suffix = f"{os.getpid()}-{hashlib.sha256(sandbox.name.encode()).hexdigest()[:32]}"
    staging = updates / f"staging-invocation-{invocation_suffix}"
    transaction = updates / (
        "forged-transaction" if case.forged_roots else f"transaction-invocation-{invocation_suffix}"
    )
    data = sandbox / "data"
    updater_executable = updates / "ArisUpdater.exe"
    target_root = sandbox / ("install-forged" if case.forged_target else "install")
    package_path = sandbox / "package.json"
    signature_path = sandbox / "package.sig"
    result_path = sandbox / "bootstrap.exit"
    success = False
    diagnostic = ""
    try:
        directories = [install, staging, transaction, data]
        if target_root != install:
            directories.append(target_root)
        for directory in directories:
            directory.mkdir(parents=True, exist_ok=False)
        protected_names = {
            "settings.ini": b"settings-must-remain-unchanged",
            "imgui.ini": b"imgui-must-remain-unchanged",
            "wwise_transfer.json": b"wwise-transfer-must-remain-unchanged",
            "Auro.db": b"database-must-remain-unchanged",
            "Auro.db-wal": b"wal-must-remain-unchanged",
            "Auro.db-shm": b"shm-must-remain-unchanged",
            "favorite.sentinel": b"favorite-must-remain-unchanged",
            "source.wav": b"source-audio-must-remain-unchanged",
        }
        protected_files = {
            root / name: contents
            for root in (data, install)
            for name, contents in protected_names.items()
        }
        protected_before: dict[Path, tuple[bytes, tuple[int, int, int, int]]] = {}
        for protected_path, protected_bytes in protected_files.items():
            protected_path.write_bytes(protected_bytes)
            protected_before[protected_path] = (
                protected_bytes,
                _file_identity(protected_path),
            )
        staged_executable = staging / "Aris.exe"
        installed_executable = install / "Aris.exe"
        staged_executable.write_bytes(_case_payload(case, health_fixture, sleep_fixture))
        # The updater host is separate from the installed application image.
        # Bootstrap binds the parent to this fixed updater-owned path while
        # the old process is launched from the installed entrypoint below.
        shutil.copy2(helper, updater_executable)
        shutil.copy2(helper, installed_executable)
        if target_root != install:
            shutil.copy2(helper, target_root / "Aris.exe")
        payload = staged_executable.read_bytes()
        old_bytes = installed_executable.read_bytes()
        old_file_identity = _file_identity(installed_executable)
        digest = hashlib.sha256(payload).hexdigest()
        package = {
            "asset_sha256": digest,
            "asset_size": len(payload),
            "build_id": case.build_id,
            "channel": "testing",
            "entrypoint": "Aris.exe",
            "inventory": [
                {"byte_size": len(payload), "relative_path": "Aris.exe", "sha256": digest}
            ],
            "key_id": "fixture-2026",
            "product": "aris",
            "removals": list(case.removals),
            "schema_version": 1,
            "version": "1.2.3",
        }
        if case.inventory_relative_path is not None:
            package["inventory"].append(
                {
                    "byte_size": 1,
                    "relative_path": case.inventory_relative_path,
                    "sha256": "b" * 64,
                }
            )
        new_payloads = {path: f"new-asset:{path}".encode() for path in case.new_inventory_files}
        for relative_path, asset_bytes in new_payloads.items():
            staged_asset = staging / relative_path
            staged_asset.parent.mkdir(parents=True, exist_ok=True)
            staged_asset.write_bytes(asset_bytes)
            package["inventory"].append(
                {
                    "byte_size": len(asset_bytes),
                    "relative_path": relative_path,
                    "sha256": hashlib.sha256(asset_bytes).hexdigest(),
                }
            )
            if (install / relative_path).parent.exists():
                raise AssertionError("new-directory test must start without install parents")
        if case.install_parent_is_file:
            (install / "assets").write_bytes(b"existing-file-must-not-be-overwritten")
        package_bytes = json.dumps(package, separators=(",", ":"), ensure_ascii=False)
        signing_key = SigningKey(
            bytes.fromhex(
                "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
            )
        )
        signature = signing_key.sign(package_bytes.encode("utf-8")).signature.hex()
        if not case.signature_valid:
            # Keep the detached value correctly shaped so this reaches the
            # signature verifier rather than the channel parser.
            signature = "00" * 64
        package_path.write_text(package_bytes, encoding="utf-8", newline="")
        signature_path.write_text(signature, encoding="ascii", newline="")
        if case.tamper_staged:
            staged_executable.write_bytes(b"tampered-after-signed-plan")

        helper_timeout = 10 if case.mode == "harness-timeout-ownership" else 90
        child_environment = os.environ.copy()
        if case.sleep_ms is not None:
            child_environment["ARIS_BOOTSTRAP_SLEEP_MS"] = str(case.sleep_ms)
        run_started = time.monotonic()
        result_path.unlink(missing_ok=True)
        mutex = _hold_installer_mutex(install) if case.installer_mutex_held else None
        try:
            completed = subprocess.run(
                [
                    str(updater_executable),
                    "--handoff",
                    str(updater_executable),
                    str(bootstrap),
                    str(package_path),
                    str(signature_path),
                    str(target_root),
                    str(updates),
                    str(staging),
                    str(transaction),
                    str(data),
                    str(result_path),
                    case.mode,
                    str(installed_executable),
                ],
                cwd=str(sandbox),
                capture_output=True,
                text=True,
                timeout=helper_timeout,
                check=False,
                env=child_environment,
            )
        finally:
            if mutex is not None:
                kernel, handle = mutex
                kernel.ReleaseMutex(handle)
                kernel.CloseHandle(handle)
        run_elapsed = time.monotonic() - run_started
        if completed.returncode != 0:
            diagnostic = (
                f"helper failed exit={completed.returncode} "
                f"stdout={completed.stdout!r} stderr={completed.stderr!r}"
            )
        else:
            # The helper waits for its monitor before returning. This short
            # grace catches publication visibility without creating a second
            # timeout longer than the helper's owned-process deadline.
            deadline = time.monotonic() + 2.0
            published_exit = ""
            while time.monotonic() < deadline:
                if result_path.exists():
                    try:
                        published_exit = result_path.read_text(
                            encoding="ascii"
                        ).strip()
                    except (OSError, UnicodeError):
                        published_exit = ""
                    if published_exit:
                        break
                time.sleep(0.05)
            if not published_exit:
                diagnostic = "Bootstrap monitor did not publish an exit code"
            else:
                try:
                    bootstrap_exit = int(published_exit)
                except ValueError:
                    diagnostic = (
                        "Bootstrap monitor published a non-numeric exit code: "
                        f"{published_exit!r}"
                    )
                    bootstrap_exit = None
                if bootstrap_exit is None:
                    success = False
                    return success, diagnostic
                if bootstrap_exit != case.expected_exit:
                    diagnostic = (
                        f"Bootstrap exit={bootstrap_exit}, "
                        f"expected={case.expected_exit}; "
                        f"helper stderr={completed.stderr!r}"
                    )
                elif (
                    case.expected_diagnostic is not None
                    and case.expected_diagnostic not in completed.stderr
                ):
                    diagnostic = (
                        "Bootstrap did not report the expected lifecycle path "
                        f"{case.expected_diagnostic!r}; "
                        f"helper stderr={completed.stderr!r}"
                    )
                elif (
                    case.max_elapsed_seconds is not None
                    and run_elapsed > case.max_elapsed_seconds
                ):
                    diagnostic = (
                        "Bootstrap exceeded the bounded case deadline "
                        f"elapsed={run_elapsed:.3f}s "
                        f"limit={case.max_elapsed_seconds:.3f}s; "
                        f"helper stderr={completed.stderr!r}"
                    )
                elif (
                    case.expected_exit in {30, 40, 50}
                    and "runtime:rollback" in completed.stderr
                ):
                    diagnostic = (
                        "forward-only terminal case entered rollback; "
                        f"helper stderr={completed.stderr!r}"
                    )
                elif any(
                    not protected_path.is_file()
                    or protected_path.read_bytes() != protected_bytes
                    or _file_identity(protected_path) != protected_identity
                    for protected_path, (
                        protected_bytes,
                        protected_identity,
                    ) in protected_before.items()
                ):
                    diagnostic = (
                        "Bootstrap mutated user data outside the install "
                        f"payload; helper stderr={completed.stderr!r}"
                    )
                elif new_payloads and case.expected_install == "new" and any(
                    not (install / relative_path).is_file()
                    or (install / relative_path).read_bytes() != asset_bytes
                    for relative_path, asset_bytes in new_payloads.items()
                ):
                    diagnostic = "new nested inventory was not installed with its signed bytes"
                elif case.install_parent_is_file and (install / "assets").read_bytes() != (
                    b"existing-file-must-not-be-overwritten"
                ):
                    diagnostic = "directory preparation overwrote an existing file"
                elif case.pre_mutation_rejection and (
                    (
                        case.pre_mutation_marker is not None
                        and case.pre_mutation_marker not in completed.stderr
                    )
                    or "runtime:old-opened" in completed.stderr
                    or "runtime:old-exited" in completed.stderr
                    or "runtime:filesystem" in completed.stderr
                    or any(transaction.iterdir())
                    or staged_executable.read_bytes() != payload
                    or installed_executable.read_bytes() != old_bytes
                    or _file_identity(installed_executable) != old_file_identity
                ):
                    diagnostic = (
                        "broker substitution was not rejected before mutation; "
                        f"stderr={completed.stderr!r} "
                        f"transaction={list(transaction.iterdir())!r}"
                    )
                else:
                    # Forward-only failures after replacement keep the signed
                    # new payload. Pre-mutation/source rejection keeps the old
                    # payload naturally; neither path restores old bytes.
                    expected_bytes = (
                        payload
                        if case.expected_install == "new"
                        else old_bytes
                    )
                    if installed_executable.read_bytes() != expected_bytes:
                        observed_digest = hashlib.sha256(
                            installed_executable.read_bytes()
                        ).hexdigest()
                        expected_digest = hashlib.sha256(expected_bytes).hexdigest()
                        diagnostic = (
                            "installed payload did not match the expected "
                            f"forward-only bytes observed={observed_digest} "
                            f"expected={expected_digest}; "
                            f"helper stderr={completed.stderr!r}"
                        )
                    else:
                        success = True
    except Exception as error:  # pragma: no cover - diagnostic path
        diagnostic = f"fixture error: {error}"
    finally:
        cleanup_error: Exception | None = None
        for _ in range(200):
            try:
                shutil.rmtree(sandbox)
                cleanup_error = None
                break
            except FileNotFoundError:
                cleanup_error = None
                break
            except Exception as error:  # pragma: no cover - diagnostic path
                cleanup_error = error
                time.sleep(0.05)
        if cleanup_error is not None:
            success = False
            cleanup_diagnostic = (
                f"sandbox cleanup failed path={sandbox} error={cleanup_error}"
            )
            diagnostic = (
                f"{diagnostic}; {cleanup_diagnostic}"
                if diagnostic
                else cleanup_diagnostic
            )
        if sandbox.exists():
            success = False
            cleanup_diagnostic = f"sandbox still exists after cleanup: {sandbox}"
            diagnostic = (
                f"{diagnostic}; {cleanup_diagnostic}"
                if diagnostic
                else cleanup_diagnostic
            )
        if not success and not diagnostic:
            diagnostic = "case failed without a diagnostic"
    return success, diagnostic


def main() -> int:
    if len(sys.argv) != 5:
        print(
            "usage: test_aris_bootstrap_blackbox.py bootstrap helper health sleep",
            file=sys.stderr,
        )
        return 2
    bootstrap = Path(sys.argv[1]).resolve()
    helper = Path(sys.argv[2]).resolve()
    health_fixture = Path(sys.argv[3]).resolve()
    sleep_fixture = Path(sys.argv[4]).resolve()
    for path in (bootstrap, helper, health_fixture, sleep_fixture):
        if not path.is_file():
            print(f"missing fixture executable: {path}", file=sys.stderr)
            return 3

    cases = (
        Case("happy", 0, expected_install="new"),
        Case(
            "new-nested-directories", 0, expected_install="new",
            new_inventory_files=(
                "assets/icons/new-contributors/cindy.png",
                "assets/icons/new-contributors/second.png",
                "docs/new-section/readme.txt",
            ),
        ),
        Case(
            "new-directory-parent-file", 30, install_parent_is_file=True,
            new_inventory_files=("assets/icons/new-contributors/cindy.png",),
        ),
        Case("channel-consumed", 0, mode="channel-consumed", expected_install="new"),
        Case(
            "legacy-one-way-channel",
            0,
            mode="legacy-one-way-channel",
            expected_install="new",
        ),
        Case("bad-signature", 11, signature_valid=False),
        Case("installer-mutex-held", 30, installer_mutex_held=True,
             pre_mutation_rejection=True),
        Case(
            "signed-removal-denied",
            14,
            removals=("obsolete.dll",),
            pre_mutation_rejection=True,
        ),
        Case(
            "protected-inventory-denied",
            14,
            inventory_relative_path="Data\\SOURCE.WaV",
            pre_mutation_rejection=True,
        ),
        Case(
            "protected-db-suffix-denied",
            14,
            inventory_relative_path="Data\\Auro.dbx.db",
            pre_mutation_rejection=True,
        ),
        Case(
            "protected-db-wal-suffix-denied",
            14,
            inventory_relative_path="Data\\Auro.dbx.db-WAL",
            pre_mutation_rejection=True,
        ),
        Case("forged-roots", 13, forged_roots=True),
        Case("forged-target", 13, forged_target=True),
        Case(
            "old-identity-substitution",
            20,
            mode="identity-substitution",
        ),
        Case(
            "broker-identity-substitution",
            13,
            mode="broker-identity-substitution",
            pre_mutation_rejection=True,
            pre_mutation_marker="fixture:broker-substitute=1",
        ),
        Case("old-timeout", 21, mode="timeout"),
        Case(
            "replace-failure",
            30,
            tamper_staged=True,
        ),
        Case(
            "child-launch-failure",
            40,
            payload="invalid-executable",
            expected_install="new",
            expected_diagnostic="runtime:forward-terminal=updated-child-launch-failed",
        ),
        Case(
            "broker-timeout",
            40,
            mode="broker-timeout",
            expected_install="new",
            expected_diagnostic="runtime:forward-terminal=updated-child-launch-failed",
            max_elapsed_seconds=5.0,
        ),
        Case("health-mismatch", 50, build_id="build-health-bad", expected_install="new"),
        Case("health-crash", 50, build_id="build-health-crash", expected_install="new"),
        Case(
            "health-timeout",
            50,
            mode="health-timeout",
            payload="sleep",
            sleep_ms=10000,
            expected_install="new",
            expected_diagnostic="runtime:forward-terminal=health-timeout",
        ),
        # Suspend Bootstrap in the fixture so only the monitor's inherited
        # exact process handle can end it before the outer helper deadline.
        Case(
            "harness-timeout-ownership",
            MONITOR_TIMEOUT_EXIT_CODE,
            mode="harness-timeout-ownership",
        ),
    )
    selected_case = os.environ.get("ARIS_BOOTSTRAP_CASE")
    if selected_case:
        cases = tuple(case for case in cases if case.name == selected_case)
        if not cases:
            print(f"unknown black-box case: {selected_case}", file=sys.stderr)
            return 4
    failures = 0
    for case in cases:
        ok, diagnostic = _run_case(
            case, bootstrap, helper, health_fixture, sleep_fixture
        )
        if ok:
            print(f"PASS {case.name}")
        else:
            failures += 1
            print(f"FAIL {case.name}: {diagnostic}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
