from __future__ import annotations

"""Check covered-field clicks and popup stacking in an isolated floating sidebar."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from qa_folder_shortcuts import Driver
from qa_required_library_setup import PrivateDesktop
from qa_sidebar_metadata_picker import capture


def run(exe: Path, root: Path, output: Path, scenario: str) -> None:
    import win32api
    import win32con
    import win32event
    import win32gui
    import win32process

    settings = root / "profile/Local/HarkenSearch/settings.ini"
    backup = settings.read_bytes()
    imgui_ini = settings.with_name("imgui.ini")
    imgui_backup = imgui_ini.read_bytes() if imgui_ini.exists() else None
    text = backup.decode("utf-8")
    for key, value in {
        "panelSidebar_visible": "1", "panelSidebar_floating": "0" if scenario == "duplicate" else "1",
        "folderBrowserDetached": "1" if scenario == "duplicate" else "0",
        "panelSidebar_floatX": "100", "panelSidebar_floatY": "65",
        "panelSidebar_floatW": "480", "panelSidebar_floatH": "500",
        "activeSidebarSection": "2", "showMetadataFilter": "1",
    }.items():
        pattern = "^" + re.escape(key) + "=.*$"
        text, count = re.subn(pattern, key + "=" + value, text, flags=re.M)
        if not count:
            text += chr(10) + key + "=" + value
    desktop = process = hwnd = result = None
    completed = shutdown_timed_out = False
    exit_code = None
    try:
        settings.write_text(text, encoding="utf-8")
        if scenario == "duplicate" and imgui_backup is not None:
            layout = imgui_backup.decode("utf-8")
            layout, count = re.subn(r"(\[Window\]\[筛选器\]\r?\n)Pos=[^\r\n]+",
                                    r"\g<1>Pos=370,70", layout, count=1)
            assert count == 1, "detached sidebar window position unavailable"
            imgui_ini.write_text(layout, encoding="utf-8")
        output.mkdir(parents=True, exist_ok=True)
        driver = Driver(root / "profile/runtime", output)
        desktop = PrivateDesktop()
        startup = win32process.STARTUPINFO()
        startup.lpDesktop = "WinSta0" + chr(92) + desktop.name
        startup.dwFlags = win32con.STARTF_USESHOWWINDOW
        startup.wShowWindow = win32con.SW_HIDE
        environment = dict(os.environ, AURA_HARNESS_MULTI_INSTANCE="1",
                           AURA_HARNESS_LANE_KIND="folder-search",
                           AURA_DISABLE_SINGLE_INSTANCE="1")
        process, thread, pid, _ = win32process.CreateProcess(str(exe),
            subprocess.list2cmdline([str(exe)]), None, None, False,
            win32con.CREATE_UNICODE_ENVIRONMENT, environment, str(exe.parent), startup)
        thread.Close()
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        assert identity["pid"] == pid
        hwnd = identity["hwnd"]
        win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
        win32gui.SetWindowPos(hwnd, 0, 30, 30, 800, 700,
            win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            state = driver.command({"cmd": "get_state"})
            if not state.get("main_ui_ready"):
                time.sleep(.1)
                continue
            dock = driver.command({"cmd": "dock_state"})
            rects = state.get("qa_filter_ui_rects", {})
            dual_ready = all(key in rects for key in (
                "metadata_LeftSidebar_Artist_input", "metadata_FloatSidebar_Artist_input"))
            if state.get("main_ui_ready") and (dual_ready if scenario == "duplicate" else
                                                 dock.get("panels", {}).get("filter", {}).get("floating")):
                break
            time.sleep(.1)
        else:
            raise TimeoutError("floating filter did not appear")
        win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
        win32gui.SetWindowPos(hwnd, 0, 30, 30, 800, 700,
            win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            state = driver.command({"cmd": "get_state"})
            if state["window_width"] <= 820:
                break
            time.sleep(.1)
        else:
            raise TimeoutError("main viewport did not resize")
        driver.command({"cmd": "folder_search_sidebar", "section": 2})
        time.sleep(.2)
        state = driver.command({"cmd": "get_state"})
        def click(x: float, y: float) -> None:
            driver.command({"cmd": "folder_search_pointer", "phase": "down", "x": x, "y": y})
            time.sleep(.09)
            driver.command({"cmd": "folder_search_pointer", "phase": "up", "x": x, "y": y})
            time.sleep(.2)
        if scenario == "duplicate":
            rects = state["qa_filter_ui_rects"]
            field = rects["metadata_LeftSidebar_Artist_input"]
            click(field[0] + 12, (field[1] + field[3]) / 2)
            after = driver.command({"cmd": "get_state"})
            debug_dock = driver.command({"cmd": "dock_state"})
            result = {"scenario": scenario,
                      "docked_input": field,
                      "detached_input": rects["metadata_FloatSidebar_Artist_input"],
                      "docked_popup_open": "metadata_LeftSidebar_Artist_popup" in after["qa_filter_ui_rects"],
                      "detached_popup_open": "metadata_FloatSidebar_Artist_popup" in after["qa_filter_ui_rects"],
                      "pointer_windows": debug_dock.get("input", {}).get("windows_at_pointer", []),
                      "clicked": [field[0] + 12, (field[1] + field[3]) / 2]}
            result["screenshot"] = str(capture(driver, output, "dual-sidebar-docked-picker"))
        else:
            arrow = state["qa_filter_ui_rects"]["metadata_Artist_arrow"]
            click((arrow[0] + arrow[2]) / 2, (arrow[1] + arrow[3]) / 2)
            state = driver.command({"cmd": "get_state"})
            dock = driver.command({"cmd": "dock_state"})
            rects = state["qa_filter_ui_rects"]
            assert "metadata_Artist_popup" in rects
        if scenario == "overlap":
            album = rects["metadata_Album_input"]
            first = rects["metadata_Artist_item_0"]
            left, top = max(album[0], first[0]), max(album[1], first[1])
            right, bottom = min(album[2], first[2]), min(album[3], first[3])
            assert left < right and top < bottom, (album, first)
            click((left + right) / 2, (top + bottom) / 2)
            after = driver.command({"cmd": "get_state"})
            result = {"scenario": scenario, "album_popup_open":
                      "metadata_Album_popup" in after["qa_filter_ui_rects"],
                      "artist_popup_open": "metadata_Artist_popup" in after["qa_filter_ui_rects"],
                      "selected_artist": after["metadata_filters"]["artist"]}
        elif scenario == "zorder":
            field = rects["metadata_Artist_input"]
            click((field[0] + field[2]) / 2, (field[1] + field[3]) / 2)
            after = driver.command({"cmd": "get_state"})
            screenshot = capture(driver, output, "floating-query-after-arrow")
            driver.command({"cmd": "dock_pointer", "phase": "hover", "module": "filter", "part": "body"})
            time.sleep(.15)
            hover_state = driver.command({"cmd": "dock_state"})
            windows_at_pointer = [row["name"] for row in
                                  hover_state["input"].get("windows_at_pointer", [])]
            result = {"scenario": scenario,
                      "artist_popup_open": "metadata_Artist_popup" in after["qa_filter_ui_rects"],
                      "screenshot": str(screenshot),
                      "popup_rect": after["qa_filter_ui_rects"].get("metadata_Artist_popup"),
                      "windows_at_pointer": windows_at_pointer}
            popup_rank = next((i for i, name in enumerate(windows_at_pointer)
                               if name == "##SidebarMetadataPicker_FloatSidebar_Artist"), -1)
            sidebar_rank = next((i for i, name in enumerate(windows_at_pointer)
                                 if name.endswith("##FloatingSidebar")), -1)
            result["popup_above_sidebar"] = popup_rank > sidebar_rank >= 0
        result["exe_sha256"] = hashlib.sha256(exe.read_bytes()).hexdigest()
        if scenario == "overlap":
            assert not result["album_popup_open"], result
        elif scenario == "zorder":
            assert result["artist_popup_open"] and result["popup_above_sidebar"], result
        else:
            assert result["docked_popup_open"] and not result["detached_popup_open"], result
        completed = True
    finally:
        try:
            if hwnd and win32gui.IsWindow(hwnd):
                win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
            if process:
                if win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_TIMEOUT:
                    shutdown_timed_out = True
                    win32api.TerminateProcess(process, 1)
                    win32event.WaitForSingleObject(process, 5000)
                exit_code = win32process.GetExitCodeProcess(process)
                process.Close()
            if desktop:
                desktop.close()
        finally:
            settings.write_bytes(backup)
            if imgui_backup is not None:
                imgui_ini.write_bytes(imgui_backup)
        if result is not None:
            result.update(exit_code=exit_code, shutdown_timed_out=shutdown_timed_out,
                          status="pass" if completed and exit_code == 0 and not shutdown_timed_out else "incomplete")
            (output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
            print(json.dumps(result, ensure_ascii=False))
        if completed and (exit_code != 0 or shutdown_timed_out):
            raise RuntimeError(f"application did not close cleanly: exit_code={exit_code}, timeout={shutdown_timed_out}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scenario", choices=["overlap", "zorder", "duplicate"], required=True)
    args = parser.parse_args()
    run(args.exe.resolve(), args.root.resolve(), args.output.resolve(), args.scenario)
