"""Capture scan completion in an already prepared, isolated QA profile.

This exercises the app's QA scan entry and captures the app after foreground
completion. It does not replace a right-click scan on a larger library or the
user's visual approval of the scan dialog.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import uuid
import wave

from PIL import Image, ImageGrab
import win32con
import win32gui

from qa_folder_shortcuts import Driver


def run(exe: Path, root: Path, output: Path) -> None:
    exe, root, output = exe.resolve(), root.resolve(), output.resolve()
    if not (root / "profile/Local/HarkenSearch/settings.ini").exists():
        raise RuntimeError("Prepare an isolated QA profile first")
    output.mkdir(parents=True, exist_ok=False)
    library = root / "scan-finalize-fixture"
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
        start = driver.command({"cmd": "scan_folder", "path": str(library),
                                "recursive": True, "scan_strategy": "fast"})
        if not start["started"]:
            raise AssertionError(start)
        observed = []
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            status = driver.command({"cmd": "scan_status"})
            observed.append({key: status[key] for key in (
                "scanning", "progress", "total", "errors",
                "scan_completion_pending", "scan_completion_cover_unresolved",
                "scan_background_backfill_phase")})
            if not status["scanning"]:
                break
            time.sleep(0.1)
        else:
            raise TimeoutError("Scan did not leave the foreground")
        if status["total"] != 160 or status["errors"] != 0:
            raise AssertionError(status)
        driver.command({"cmd": "open_scan_dialog"})
        time.sleep(0.6)
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        shot = output / "scan-complete.png"
        ImageGrab.grab(window=hwnd).save(shot)
        frame = root / "profile/runtime" / f"scan-complete-{uuid.uuid4().hex}.ppm"
        driver.command({"cmd": "capture_preview_frame", "path": str(frame)})
        capture_deadline = time.monotonic() + 10
        while not frame.exists() and time.monotonic() < capture_deadline:
            time.sleep(0.05)
        if not frame.exists():
            raise TimeoutError("Preview frame was not captured")
        preview = output / "scan-complete-frame.png"
        Image.open(frame).save(preview)
        report = {"exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                  "isolation_root": str(root), "final_status": status,
                  "observed": observed, "screenshot": str(shot),
                  "preview_frame": str(preview)}
        (output / "result.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print("PASS: isolated fast scan left foreground; screenshot captured")
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
    args = parser.parse_args()
    run(args.exe, args.isolation_root, args.output)
