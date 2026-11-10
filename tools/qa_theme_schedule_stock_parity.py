"""Compare scheduled stock themes with selecting the same themes directly.

Runs the real isolated EXE on a private desktop with its own scanned library.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import time
import traceback

from PIL import Image, ImageChops
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver


THEMES = (0, 1, 2, 3, 10)


def run(args):
    import win32con
    import win32event
    import win32gui
    import win32process

    exe, root, output = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    fixture = root / "library"
    assert exe.is_file() and fixture.is_dir() and any(fixture.glob("*.wav"))
    output.mkdir(parents=True, exist_ok=False)
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    driver, desktop = Driver(runtime, output), PrivateDesktop()
    process = None
    report = {
        "exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "started_at": datetime.now(timezone.utc).isoformat(), "cases": [],
    }
    client_origin = (0, 0)

    def command(**payload):
        return driver.command(payload)

    def state():
        return command(cmd="theme_schedule")

    def settle():
        before = state()["frame"]
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            value = state()
            if value["frame"] > before + 3:
                return value
            time.sleep(.08)
        raise AssertionError("No rendered frames")

    def button(label):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            audit = command(cmd="ordinary_button_audit")
            choices = [item for item in audit["checks"]
                       if item["label"] == label and not item["disabled"]]
            if len(choices) == 1 and audit["current_frame"] - audit["frame"] <= 2:
                return choices[0]["bounds"]
            time.sleep(.08)
        raise AssertionError(f"Button unavailable: {label}")

    def click_rect(bounds):
        assert len(bounds) == 4 and bounds[2] > bounds[0] and bounds[3] > bounds[1], bounds
        x = (bounds[0] + bounds[2]) / 2 - client_origin[0]
        y = (bounds[1] + bounds[3]) / 2 - client_origin[1]
        for phase in ("hover", "down", "up"):
            command(cmd="theme_schedule", action="pointer", phase=phase, x=x, y=y)
            settle()

    def settings():
        command(cmd="ordinary_button_window", module="settings", action="open")
        settle()
        # Clear the settings filter so Appearance always remains visible.
        clear = button("清除")
        save = button("保存并关闭")
        click_rect((save[0] + 10, clear[1], save[0] + 30, clear[3]))
        settle()

    def automatic(enabled):
        if (state()["mode"] == 1) != enabled:
            click_rect(state()["auto_toggle_bounds"])
        assert (state()["mode"] == 1) == enabled

    def choose(period, theme):
        index = 0 if period == "day" else 1
        click_rect(settle()["ui"]["selector_buttons"][index])
        options = [item for item in settle()["ui"]["preset_options"]
                   if item["period"] == index and item["theme"] == theme]
        assert len(options) == 1, (period, theme, options)
        click_rect(options[0]["bounds"])
        assert state()["ui"]["selector_presets"][index] == theme

    def clock(minute, period):
        command(cmd="theme_schedule", action="clock", minute=minute)
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            value = settle()
            if (value["period"] == period and value["mode"] == 1
                    and value["active_bg"] == value[period + "_bg"]
                    and value["active_accent"] == value[period + "_accent"]):
                return value
        raise AssertionError(f"Scheduled {period} palette not applied")

    def stop():
        nonlocal process
        if process is None:
            return
        if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
            command(cmd="info_edit_input", action="exit")
        assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
        report["exit_code"] = win32process.GetExitCodeProcess(process)
        process.Close()
        process = None
        assert report["exit_code"] == 0

    try:
        previous = os.environ.get("HARKEN_THEME_QA_MINUTE")
        os.environ["HARKEN_THEME_QA_MINUTE"] = "480"
        try:
            process, pid = desktop.start(exe)
        finally:
            if previous is None:
                os.environ.pop("HARKEN_THEME_QA_MINUTE", None)
            else:
                os.environ["HARKEN_THEME_QA_MINUTE"] = previous
        identity = command(cmd="magic_eq_identity")
        assert identity["pid"] == pid and Path(identity["exe_path"]).resolve() == exe
        assert Path(identity["isolation_root"]).resolve() == root
        wait_state(driver, lambda value: value["main_ui_ready"], "startup", 60)
        current = command(cmd="get_state")
        if not current["db_path"]:
            command(cmd="info_edit_input", action="new_db_name", text="Theme parity library")
            command(cmd="info_edit_input", action="new_db_create")
            current = wait_state(driver, lambda value: value["db_path"]
                                 and not value["db_switch_in_progress"], "new library", 30)
        assert Path(current["db_path"]).resolve().is_relative_to(root / "profile")
        if current["total_count"] == 0:
            command(cmd="scan_folder", path=str(fixture), recursive=True,
                    scan_strategy="fast", cache_waveforms=False,
                    build_similarity_index=False)
            deadline = time.monotonic() + 60
            while command(cmd="scan_status")["scanning"]:
                assert time.monotonic() < deadline, "Scan did not finish"
                time.sleep(.1)
            command(cmd="scan_acknowledge_complete")
            current = wait_state(driver, lambda value: value["total_count"] > 0,
                                 "populated library", 30)
        report["library"] = {"path": current["db_path"], "count": current["total_count"]}
        client_origin = win32gui.ClientToScreen(identity["hwnd"], (0, 0))
        command(cmd="dock_pointer", phase="focus")
        settle()

        for theme in THEMES:
            settings()
            automatic(False)
            stock = [item for item in settle()["ui"]["stock"] if item["theme"] == theme]
            assert len(stock) == 1
            click_rect(stock[0]["bounds"])
            if theme == 2:
                capture(driver, output / "manual-settings-2.png")
            click_rect(button("保存并关闭"))
            manual = settle()
            assert manual["theme"] == theme and manual["mode"] == 0, manual
            manual_path = output / f"manual-{theme}.png"
            capture(driver, manual_path)

            settings()
            automatic(True)
            choose("day", theme)
            choose("night", theme)
            if theme == 2:
                capture(driver, output / "scheduled-settings-2.png")
            click_rect(button("保存并关闭"))
            cases = []
            for period, minute in (("day", 480), ("night", 960)):
                scheduled = clock(minute, period)
                screenshot = output / f"scheduled-{period}-{theme}.png"
                capture(driver, screenshot)
                with Image.open(manual_path) as original, Image.open(screenshot) as actual:
                    original, actual = original.convert("RGB"), actual.convert("RGB")
                    width, height = original.size
                    regions = {
                        "left": (0, int(height * .07), int(width * .10), int(height * .81)),
                        "center": (int(width * .12), int(height * .07),
                                   int(width * .87), int(height * .81)),
                        "right": (int(width * .88), int(height * .07), width, int(height * .81)),
                    }
                    matching_regions = {name: ImageChops.difference(
                        original.crop(bounds), actual.crop(bounds)).getbbox() is None
                        for name, bounds in regions.items()}
                ok = (scheduled["theme"] == theme
                      and scheduled["active_bg"] == manual["active_bg"]
                      and scheduled["active_accent"] == manual["active_accent"]
                      and scheduled["imgui_bg"] == manual["imgui_bg"]
                      and all(matching_regions.values()))
                cases.append({"period": period, "ok": ok, "manual": manual,
                              "scheduled": scheduled, "matching_regions": matching_regions,
                              "screenshot": str(screenshot)})
            report["cases"].append({"theme": theme, "manual_screenshot": str(manual_path),
                                    "periods": cases})
        assert all(case["ok"] for row in report["cases"] for case in row["periods"]), \
            "Scheduled stock themes use a different theme path from direct selection"
        report["passed"] = True
    except Exception:
        report["passed"] = False
        report["error"] = traceback.format_exc()
        raise
    finally:
        try:
            stop()
        finally:
            desktop.close()
            report["finished_at"] = datetime.now(timezone.utc).isoformat()
            report["not_covered"] = ["Custom edited day/night palettes", "User visual acceptance"]
            (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--isolation-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    run(parser.parse_args())
