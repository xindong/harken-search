"""Capture the real close X surfaces in a pinned-isolation QA package.

Only this script's QA process is controlled. The shared desktop mouse is never moved.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

from PIL import Image, ImageDraw, ImageFont, ImageGrab
from qa_transport_themes import Driver, THEMES, window_api


def windows_for_pid(user32, pid):
    found = []
    callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)

    @callback
    def visit(hwnd, _):
        actual = w.DWORD()
        rect = w.RECT()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(actual))
        if actual.value == pid and user32.IsWindowVisible(hwnd) and user32.GetWindowRect(hwnd, ctypes.byref(rect)):
            if rect.right - rect.left > 200 and rect.bottom - rect.top > 100:
                found.append((hwnd, rect.left, rect.top, rect.right, rect.bottom))
        return True

    user32.EnumWindows(visit, 0)
    return found


def capture(user32, hwnd, pid, destination):
    actual = w.DWORD()
    rect = w.RECT()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(actual))
    if actual.value != pid or not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
        raise RuntimeError("QA window identity changed")
    # Keep the QA window above other desktop windows only during capture.
    # No foreground activation or shared mouse input is needed.
    flags = 0x0001 | 0x0002 | 0x0010 | 0x0040  # NOSIZE, NOMOVE, NOACTIVATE, SHOWWINDOW
    user32.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, w.UINT]
    user32.SetWindowPos.restype = w.BOOL
    if not user32.SetWindowPos(hwnd, w.HWND(-1), 0, 0, 0, 0, flags):
        raise RuntimeError("cannot show isolated QA window for capture")
    try:
        time.sleep(0.15)
        image = ImageGrab.grab(bbox=(rect.left, rect.top, rect.right, rect.bottom),
                               all_screens=True)
    finally:
        user32.SetWindowPos(hwnd, w.HWND(-2), 0, 0, 0, 0, flags)
    image.save(destination)
    return {"path": str(destination), "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
            "hwnd": hwnd, "rect": [rect.left, rect.top, rect.right, rect.bottom]}


def run(args):
    exe = args.exe.resolve()
    isolation = args.isolation_root.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runtime = isolation / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    if (runtime / "qa_cmd.json").exists():
        raise RuntimeError("pending QA command in isolation root")
    env = dict(os.environ, LOCALAPPDATA=str(isolation / "profile/Local"),
               APPDATA=str(isolation / "profile/Roaming"), AURA_RUNTIME_ROOT=str(runtime),
               TEMP=str(isolation / "temp"), TMP=str(isolation / "temp"),
               AURA_HARNESS_MULTI_INSTANCE="1", AURA_HARNESS_LANE_KIND="functional",
               AURA_DISABLE_SINGLE_INSTANCE="1")
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4
    process = subprocess.Popen([str(exe)], cwd=exe.parent, env=env, startupinfo=startup)
    user32 = window_api()
    user32.EnumWindows.argtypes = [ctypes.c_void_p, w.LPARAM]
    user32.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    user32.IsWindowVisible.argtypes = [w.HWND]
    driver = Driver(runtime, output)
    report = {"pid": process.pid, "exe": str(exe),
              "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "captures": {}}
    try:
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"QA process exited during startup: {process.returncode}")
            if windows_for_pid(user32, process.pid):
                break
            time.sleep(0.2)
        time.sleep(5)
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        if identity["pid"] != process.pid or Path(identity["isolation_root"]).resolve() != isolation:
            raise RuntimeError("QA process or isolation identity mismatch")
        main = identity["hwnd"]
        user32.GetWindowLongW.argtypes = [w.HWND, ctypes.c_int]
        user32.GetWindowLongW.restype = w.LONG
        style = user32.GetWindowLongW(main, -16)
        if style & 0x00C00000 != 0x00C00000:  # WS_CAPTION
            raise AssertionError(f"Windows system caption was changed: 0x{style:08x}")
        report["native_caption_style"] = f"0x{style:08x}"
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            if driver.command({"cmd": "get_state"}).get("main_ui_ready"):
                break
            time.sleep(0.4)
        else:
            raise TimeoutError("QA main UI did not become ready")
        user32.ShowWindow(main, 9)
        user32.SetWindowPos(main, None, 40, 40, 1100, 760, 0x0040 | 0x0004)
        time.sleep(0.8)
        report["captures"]["main"] = capture(user32, main, process.pid, output / "main-window.png")

        driver.command({"cmd": "update_notice", "action": "open"})
        time.sleep(1)
        update_windows = [win for win in windows_for_pid(user32, process.pid) if win[0] != main]
        update = max(update_windows, key=lambda item: (item[3]-item[1])*(item[4]-item[2]))[0] if update_windows else main
        report["captures"]["update"] = capture(user32, update, process.pid, output / "update-window.png")
        state = driver.command({"cmd": "update_notice", "action": "state"})
        report["update_close_area"] = state["controls"].get("close")
        if not report["update_close_area"]:
            raise AssertionError("update close control missing")
        driver.command({"cmd": "update_notice_pointer", "control": "close", "phase": "down"})
        time.sleep(0.15)
        driver.command({"cmd": "update_notice_pointer", "control": "close", "phase": "up"})
        time.sleep(0.6)
        if driver.command({"cmd": "update_notice", "action": "state"})["visible"]:
            raise AssertionError("update X did not close the window")

        similar = driver.command({"cmd": "similar_index", "action": "open",
                                  "floating": True, "width": 720, "height": 420})
        if not similar.get("visible"):
            raise AssertionError("similar window did not open")
        time.sleep(1)
        similar_windows = [win for win in windows_for_pid(user32, process.pid) if win[0] != main]
        window = max(similar_windows, key=lambda item: (item[3]-item[1])*(item[4]-item[2]))[0] if similar_windows else main
        report["captures"]["similar"] = capture(user32, window, process.pid, output / "similar-window.png")
        driver.command({"cmd": "similar_index", "action": "close"})

        driver.command({"cmd": "function_window", "module": "ucs-browser",
                        "action": "open", "floating": True,
                        "x": 1190, "y": 95, "w": 430, "h": 480})
        deadline = time.monotonic() + 10
        detached = []
        while time.monotonic() < deadline:
            detached = [win for win in windows_for_pid(user32, process.pid) if win[0] != main]
            if detached:
                break
            time.sleep(0.2)
        if not detached:
            raise AssertionError("detached module did not create a native window")
        report["captures"]["detached"] = capture(
            user32, detached[0][0], process.pid, output / "detached-window.png")
        driver.command({"cmd": "function_window", "module": "ucs-browser", "action": "close"})

        if args.themes:
            report["themes"] = {}
            for theme, name in THEMES:
                driver.command({"cmd": "set_theme", "theme": theme})
                driver.rendered(theme)
                theme_captures = {"name": name}
                theme_captures["main"] = capture(
                    user32, main, process.pid, output / f"theme-{theme}-main.png")
                driver.command({"cmd": "update_notice", "action": "open"})
                time.sleep(0.7)
                theme_captures["update"] = capture(
                    user32, main, process.pid, output / f"theme-{theme}-update.png")
                driver.command({"cmd": "update_notice", "action": "close"})
                time.sleep(0.35)
                driver.command({"cmd": "similar_index", "action": "open",
                                "floating": True, "width": 720, "height": 420})
                time.sleep(1.0)
                theme_captures["similar"] = capture(
                    user32, main, process.pid, output / f"theme-{theme}-similar.png")
                driver.command({"cmd": "similar_index", "action": "close"})
                time.sleep(0.5)
                driver.command({"cmd": "function_window", "module": "ucs-browser",
                                "action": "open", "floating": True,
                                "x": 1190, "y": 95, "w": 430, "h": 480})
                time.sleep(0.4)
                windows = [win for win in windows_for_pid(user32, process.pid) if win[0] != main]
                if not windows:
                    raise AssertionError(f"detached window missing for theme {theme}")
                theme_captures["detached"] = capture(
                    user32, windows[0][0], process.pid, output / f"theme-{theme}-detached.png")
                driver.command({"cmd": "function_window", "module": "ucs-browser", "action": "close"})
                report["themes"][str(theme)] = theme_captures
            sheet = Image.new("RGB", (770, 510), (28, 32, 40))
            draw = ImageDraw.Draw(sheet)
            font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 15)
            columns = [("系统标题栏原样", (1010, 0, 1095, 50)),
                       ("检查更新", (866, 55, 928, 110)),
                       ("相似音效", (725, 45, 790, 96)),
                       ("独立窗口", (350, 0, 428, 50))]
            for col, (label, _) in enumerate(columns):
                draw.text((118 + col * 165, 7), label, fill=(225, 230, 235), font=font)
            for row, (theme, label) in enumerate(THEMES):
                draw.text((8, 67 + row * 94), label, fill=(225, 230, 235), font=font)
                for col, (kind, box) in enumerate((
                        ("main", columns[0][1]), ("update", columns[1][1]),
                        ("similar", columns[2][1]), ("detached", columns[3][1]))):
                    cropped = Image.open(output / f"theme-{theme}-{kind}.png").convert("RGB").crop(box)
                    sheet.paste(cropped, (118 + col * 165, 50 + row * 94))
            sheet_path = output / "all-themes-close-x-contact-sheet.png"
            sheet.save(sheet_path)
            report["theme_contact_sheet"] = str(sheet_path)

        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(report, ensure_ascii=False))
        return 0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=15)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--isolation-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--themes", action="store_true", help="capture all five built-in themes")
    raise SystemExit(run(parser.parse_args()))
