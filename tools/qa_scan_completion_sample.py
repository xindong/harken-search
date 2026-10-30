"""Capture scan completion in an already prepared, isolated QA profile.

This invokes the native folder-tree scan handoff through a QA-only command,
without shared mouse input, and captures the app after foreground completion.
It does not replace the user's visual approval of the scan dialog.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time
import uuid
import wave

from PIL import Image, ImageGrab
import win32con
import win32gui
import win32process

from qa_folder_shortcuts import Driver


def run(exe: Path, root: Path, output: Path, pause: bool) -> None:
    exe, root, output = exe.resolve(), root.resolve(), output.resolve()
    if not (root / "profile/Local/HarkenSearch/settings.ini").exists():
        raise RuntimeError("Prepare an isolated QA profile first")
    output.mkdir(parents=True, exist_ok=False)
    library = root / ("scan-pause-fixture-" + output.name if pause else "scan-finalize-fixture")
    library.mkdir(exist_ok=True)
    for index in range(160):
        with wave.open(str(library / f"sample-{index:04}.wav"), "wb") as sound:
            sound.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
            sound.writeframes(bytes(1600))

    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = win32con.SW_SHOWNOACTIVATE
    env = dict(os.environ, AURA_HARNESS_MULTI_INSTANCE="1",
               AURA_HARNESS_LANE_KIND="functional",
               AURA_DISABLE_SINGLE_INSTANCE="1")
    process = subprocess.Popen([str(exe)], cwd=exe.parent, env=env,
                               startupinfo=startup)
    driver = Driver(root / "profile/runtime", output)
    hwnd = None
    try:
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        assert identity["pid"] == process.pid, identity
        assert Path(identity["isolation_root"]).resolve() == root, identity
        hwnd = identity["hwnd"]
        deadline = time.monotonic() + 60
        while not driver.command({"cmd": "wait_ready"})["main_ui_ready"]:
            if time.monotonic() > deadline:
                raise TimeoutError("Main UI was not ready")
            time.sleep(0.1)
        driver.command({"cmd": "qa_rightclick_scan", "path": str(library)})
        start_deadline = time.monotonic() + 15
        while not driver.command({"cmd": "scan_status"})["scanning"]:
            if time.monotonic() > start_deadline:
                raise TimeoutError("Native right-click scan did not start")
            time.sleep(0.05)
        observed = []
        deadline = time.monotonic() + 90
        pause_sent = False
        while time.monotonic() < deadline:
            status = driver.command({"cmd": "scan_status"})
            observed.append({key: status[key] for key in (
                "scanning", "progress", "total", "errors",
                "scan_completion_pending", "scan_completion_cover_unresolved",
                "scan_background_backfill_phase")})
            if (pause and not pause_sent and status["scanning"] and
                    status["scan_background_backfill_phase"] == "deferred"):
                pause_result = driver.command({"cmd": "qa_scan_pause_keep"})
                if not pause_result["was_scanning"]:
                    raise AssertionError(pause_result)
                pause_sent = True
            if not status["scanning"]:
                break
            time.sleep(0.1)
        else:
            raise TimeoutError("Scan did not leave the foreground")
        if status["total"] != 160 or status["errors"] != 0:
            raise AssertionError(status)
        state = driver.command({"cmd": "get_state"})
        database = sqlite3.connect("file:" + state["db_path"] + "?mode=ro", uri=True)
        try:
            kept_file_count = database.execute(
                "SELECT COUNT(*) FROM audio_files WHERE file_path LIKE ?",
                ("%" + library.name + "%",)).fetchone()[0]
        finally:
            database.close()
        if pause and (not pause_sent or not state["was_paused"] or
                      status["scan_workflow_stage"] != "paused" or
                      kept_file_count != 160):
            raise AssertionError((pause_sent, state, status))
        if not pause and (state["was_paused"] or kept_file_count != 160):
            raise AssertionError(state)
        driver.command({"cmd": "open_scan_dialog"})
        time.sleep(0.6)
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        shot = output / ("scan-paused.png" if pause else "scan-complete.png")
        ImageGrab.grab(window=hwnd).save(shot)
        owned_windows = []
        def capture_owned_window(window, _):
            if (window == hwnd or not win32gui.IsWindowVisible(window)):
                return
            if win32process.GetWindowThreadProcessId(window)[1] != process.pid:
                return
            title = win32gui.GetWindowText(window)
            target = output / f"scan-popup-{len(owned_windows)}.png"
            ImageGrab.grab(window=window).save(target)
            owned_windows.append({"title": title, "screenshot": str(target)})
        win32gui.EnumWindows(capture_owned_window, None)
        frame = root / "profile/runtime" / f"scan-complete-{uuid.uuid4().hex}.ppm"
        driver.command({"cmd": "capture_preview_frame", "path": str(frame)})
        capture_deadline = time.monotonic() + 10
        while not frame.exists() and time.monotonic() < capture_deadline:
            time.sleep(0.05)
        if not frame.exists():
            raise TimeoutError("Preview frame was not captured")
        preview = output / ("scan-paused-frame.png" if pause else "scan-complete-frame.png")
        Image.open(frame).save(preview)
        report = {"exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                  "isolation_root": str(root), "final_status": status,
                  "pause_requested": pause_sent, "was_paused": state["was_paused"],
                  "kept_file_count": kept_file_count,
                  "observed": observed, "screenshot": str(shot),
                  "owned_windows": owned_windows,
                  "preview_frame": str(preview)}
        (output / "result.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print("PASS: isolated native scan left foreground; screenshot captured")
    finally:
        if hwnd and win32gui.IsWindow(hwnd):
            win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=10)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--isolation-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pause", action="store_true")
    args = parser.parse_args()
    run(args.exe, args.isolation_root, args.output, args.pause)
