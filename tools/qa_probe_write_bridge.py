"""Probe the Harken-side write bridge directly, without the MCP connector.

Launches the isolated EXE, prepares one authorized library, then speaks the pipe
protocol by hand so a failure is either "Harken never answered" or "Harken
answered with a refusal" — never a guess.
"""
from pathlib import Path
import argparse
import json
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_external_ai_settings import library_id  # noqa: E402
from qa_required_library_setup import PrivateDesktop, wait_state  # noqa: E402
from qa_transport_themes import Driver, synthetic_audio  # noqa: E402

CAPABILITIES = {
    "searchAudio": "read", "findSimilar": "read", "findFolders": "read",
    "readFileName": "read", "readMetadata": "read", "editMetadata": "none",
    "aiNamingMetadata": "none", "readPlaylist": "read", "editPlaylist": "edit",
}
TOKEN = "e2e0" * 16


def pipe_name(library):
    value = 1469598103934665603
    for byte in library.encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "\\\\.\\pipe\\harken-external-ai-%016x" % value


def run(args):
    import win32file
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runtime = root / "profile" / "runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    fixtures = root / "fixtures" / "audio"
    fixtures.mkdir(parents=True, exist_ok=True)
    for index in range(4):
        synthetic_audio(fixtures / f"sfx-{index + 1}.wav")

    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process, pid = desktop.start(exe)
    report = {"steps": []}
    try:
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        wait_state(driver, lambda s: s["main_ui_ready"], "startup", timeout=90)
        state = driver.command({"cmd": "get_state"})
        if state["startup_needs_database_setup"]:
            driver.command({"cmd": "info_edit_input", "action": "new_db_name", "text": "Probe-" + str(pid)})
            driver.command({"cmd": "info_edit_input", "action": "new_db_create"})
            state = wait_state(driver, lambda s: bool(s["db_path"]) and not s["db_switch_in_progress"]
                               and not s["new_db_dialog_open"], "library")
        database = Path(state["db_path"])
        if state["total_count"] < 4:
            driver.command({"cmd": "scan_folder", "path": str(fixtures), "recursive": True})
            state = wait_state(driver, lambda s: s["total_count"] >= 4, "scan", timeout=120)
        identity_id = library_id(database)
        settings_ini = next(root.rglob("settings.ini"), None)
        assert settings_ini is not None, "settings.ini not found under the isolation root"
        document = settings_ini.parent / "external-ai" / f"{identity_id}.json"
        document.parent.mkdir(parents=True, exist_ok=True)
        document.write_text(json.dumps({
            "libraryId": identity_id, "enabled": True, "sampleLibrary": False,
            "roots": [{"path": str(fixtures), "enabled": True}],
            "bridgeToken": TOKEN, "capabilities": CAPABILITIES,
        }, ensure_ascii=False, indent=2), encoding="utf-8")
        report["steps"].append({"document": str(document), "library": identity_id,
                                "database": str(database)})
        # The app reconciles the bridge every 120 frames (~2 s).
        time.sleep(5)
        name = pipe_name(identity_id)
        report["pipe"] = name
        try:
            handle = win32file.CreateFile(name, win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                                          0, None, win32file.OPEN_EXISTING, 0, None)
        except Exception as error:  # noqa: BLE001
            report["steps"].append({"open_pipe": f"{type(error).__name__}: {error}"})
            raise
        request = {
            "token": TOKEN, "requestId": "probe-1", "libraryId": identity_id,
            "playlistPath": "Game SFX/Combat", "fileIds": [1, 2],
        }
        win32file.WriteFile(handle, (json.dumps(request) + "\n").encode("utf-8"))
        _, data = win32file.ReadFile(handle, 8192)
        handle.Close()
        report["response"] = data.decode("utf-8", "replace").strip()
        report["ok"] = True
        return report
    finally:
        try:
            driver.command({"cmd": "info_edit_input", "action": "exit"}, timeout=10)
        except Exception:  # noqa: BLE001
            pass
        try:
            process.terminate()
            process.wait(timeout=15)
        except Exception:  # noqa: BLE001
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--isolation-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = run(args)
    except Exception as error:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": f"{type(error).__name__}: {error}"}, ensure_ascii=False))
        return 2
    print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
