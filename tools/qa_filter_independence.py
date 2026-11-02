"""Exercise filter combinations through the isolated native RenderUI path.

The fixed preview profile must already contain the authorized ActualLibrary copy.
This driver injects ImGui events inside that process; it never uses the desktop mouse.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from qa_folder_shortcuts import Driver


def all_results(driver):
    """Read every loaded native result before comparing filter intersections."""
    page = driver.command({"cmd": "get_results", "offset": 0, "limit": 100})
    total = page["total"]
    rows = list(page["items"])
    while len(rows) < total:
        page = driver.command({"cmd": "get_results", "offset": len(rows), "limit": 100})
        assert page["total"] == total, "result set changed while reading pages"
        assert page["items"], "native result page was empty before the end"
        rows.extend(page["items"])
    assert len(rows) == total, "native result page count changed"
    return rows


def run(exe, root, output, mode):
    import win32con
    import win32gui
    from PIL import ImageGrab

    exe, root, output = exe.resolve(), root.resolve(), output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    settings = root / "profile/Local/HarkenSearch/settings.ini"
    actual_db = root / "profile/Local/HarkenSearch/Database/ActualLibrary/main.db"
    assert settings.is_file() and actual_db.is_file(), "authorized QA library copy missing"
    original_settings = settings.read_text(encoding="utf-8")
    search_line = "searchRealtime=" + ("1" if mode == "live" else "0")
    lines = original_settings.splitlines()
    lines = [search_line if line.startswith("searchRealtime=") else line for line in lines]
    if not any(line.startswith("searchRealtime=") for line in original_settings.splitlines()):
        lines.append(search_line)
    settings.write_text("\n".join(lines) + "\n", encoding="utf-8")

    runtime = root / "profile/runtime"
    driver = Driver(runtime, output)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = win32con.SW_SHOWNOACTIVATE
    process = subprocess.Popen([str(exe)], cwd=exe.parent, startupinfo=startup, env=dict(
        os.environ, AURA_HARNESS_MULTI_INSTANCE="1", AURA_HARNESS_LANE_KIND="folder-search",
        AURA_DISABLE_SINGLE_INSTANCE="1"))
    hwnd = None
    cases = []
    failure = None

    def state_ready():
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            ready = driver.command({"cmd": "wait_ready"})
            state = driver.command({"cmd": "get_state"})
            if ready["ready"] and state["search_ready"]:
                return state
            time.sleep(.1)
        raise TimeoutError("native query did not settle")

    def screenshot(name):
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        image = ImageGrab.grab(window=hwnd)
        assert len(image.getcolors(image.width * image.height) or []) > 16
        path = output / (name + ".png")
        image.save(path)
        return str(path)

    def record(name, state):
        rows = driver.command({"cmd": "get_results", "offset": 0, "limit": 100})["items"]
        cases.append({"name": name, "state": state, "results": rows,
                      "screenshot": screenshot(name)})

    def pointer(x, y):
        driver.command({"cmd": "folder_search_pointer", "phase": "down", "x": x, "y": y})
        time.sleep(.1)
        driver.command({"cmd": "folder_search_pointer", "phase": "up", "x": x, "y": y})
        time.sleep(.25)

    def click(name):
        deadline = time.monotonic() + 15
        while True:
            state = state_ready()
            rect = state.get("qa_filter_ui_rects", {}).get(name)
            if rect:
                pointer((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2)
                return state_ready()
            assert time.monotonic() < deadline, "native UI target missing: " + name
            time.sleep(.1)

    def until(label, predicate):
        deadline = time.monotonic() + 25
        while True:
            state = state_ready()
            if predicate(state):
                return state
            assert time.monotonic() < deadline, (label, state)
            time.sleep(.1)

    def search(query):
        driver.command({"cmd": "search", "query": query})
        if mode == "enter":
            driver.command({"cmd": "search_enter_key", "phase": "down"})
            time.sleep(.08)
            driver.command({"cmd": "search_enter_key", "phase": "up"})
        time.sleep(.4)
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            state = state_ready()
            if state["search_text"] == query:
                return state
            time.sleep(.1)
        raise TimeoutError("search text did not settle")

    try:
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        assert identity["pid"] == process.pid
        assert Path(identity["exe_path"]).resolve() == exe
        assert Path(identity["isolation_root"]).resolve() == root
        hwnd = identity["hwnd"]
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        time.sleep(.5)
        deadline = time.monotonic() + 45
        while True:
            state = state_ready()
            if (state["main_ui_ready"] and state["active_view"] == "database" and
                    Path(state["active_library"]).resolve() == actual_db):
                break
            assert time.monotonic() < deadline, "ActualLibrary did not open"
            time.sleep(.2)
        # QA commands prepare a known baseline; assertions below use actual UI paths.
        driver.command({"cmd": "clear_cover_filter"})
        driver.command({"cmd": "clear_folder_filter"})
        driver.command({"cmd": "clear_metadata_filter"})
        if "qa_filter_extras" in state:
            driver.command({"cmd": "folder_search_seed_extra_filters", "enabled": False})
        search("")
        deadline = time.monotonic() + 45
        while True:
            state = state_ready()
            width = win32gui.GetClientRect(hwnd)[2]
            tile_rect = state.get("qa_filter_ui_rects", {}).get("cover_tile_0")
            tile_ready = tile_rect is None or tile_rect[2] <= width
            if (state["total_count"] >= 11000 and state["cover_wall"]["visible_tiles"] > 0 and
                    tile_ready and state["window_width"] == width):
                break
            assert time.monotonic() < deadline, "ActualLibrary baseline did not settle"
            time.sleep(.2)
        record("actual-library-before", state)
        with ImageGrab.grab(window=hwnd) as visible_window:
            visible_width = visible_window.width
        deadline = time.monotonic() + 20
        while state["window_width"] != visible_width:
            assert time.monotonic() < deadline, "RenderUI dimensions did not settle"
            time.sleep(.2)
            state = state_ready()
        # RenderUI reports its own tile rectangle; the pointer is still an ImGui click.
        tile = state.get("qa_filter_ui_rects", {}).get("cover_tile_0")
        if tile is None:
            width = win32gui.GetClientRect(hwnd)[2]
            tile = [width - 280, 110, width - 168, 232]  # red build lacks rect reporting
        pointer((tile[0] + tile[2]) / 2, (tile[1] + tile[3]) / 2)
        state = state_ready()
        assert state["cover_filter"]["active"], "real cover tile click did not apply"
        cover_key = state["cover_filter"]["path"]
        cover_rows = all_results(driver)
        assert cover_rows, "cover click yielded no rows"
        record("cover-click", state)
        words = re.findall(r"[A-Za-z]{4,}", cover_rows[0]["name"])
        assert words, "cover result has no searchable token"
        query = words[0]
        state = search(query)
        record("cover-then-keyword", state)
        assert state["cover_filter"]["active"] and state["cover_filter"]["path"] == cover_key, (
            "search cleared selected cover", state["cover_filter"])
        rows = all_results(driver)
        cover_paths = {row["path"] for row in cover_rows}
        assert rows, "matching cover keyword unexpectedly returned no results"
        assert {row["path"] for row in rows} <= cover_paths, "result is not filter intersection"
        state = search("zzzzharkenfilterimpossible2026")
        record("empty-intersection", state)
        assert state["cover_filter"]["active"] and state["result_count"] == 0, (
            "empty intersection changed cover or returned rows", state["cover_filter"], state["result_count"])
        click("chip_keyword")
        state = until("remove keyword", lambda s: s["search_text"] == "" and
                      s["cover_filter"]["active"] and s["result_count"] == len(cover_rows))
        record("remove-keyword-only", state)

        state = search(query)
        assert state["cover_filter"]["active"] and state["result_count"] > 0
        click("refresh")
        state = until("refresh kept filters", lambda s: s["search_text"] == query and
                      s["cover_filter"]["active"] and s["result_count"] > 0)
        record("refresh-keeps-cover-keyword", state)

        # The deferred folder-tree path is exercised with an isolated path. It
        # should yield an empty intersection without discarding cover/keyword.
        isolated_folder = root / "folder-filter-empty"
        isolated_folder.mkdir(exist_ok=True)
        driver.command({"cmd": "queue_folder_tree_selection", "path": str(isolated_folder)})
        state = until("deferred folder selection", lambda s: s["folder_filter"]["active"] and
                      s["cover_filter"]["active"] and s["search_text"] == query and s["result_count"] == 0)
        record("deferred-folder-keeps-cover-keyword", state)
        click("chip_folder")
        state = until("remove folder", lambda s: not s["folder_filter"]["active"] and
                      s["cover_filter"]["active"] and s["search_text"] == query and s["result_count"] > 0)
        record("remove-folder-only", state)

        click("chip_cover_key")
        state = until("remove cover key", lambda s: not s["cover_filter"]["active"] and
                      s["search_text"] == query and s["result_count"] > 0)
        record("remove-cover-only", state)
        query = max(words, key=len)
        state = search(query)
        assert state["result_count"] > 0
        keyword_paths = {row["path"] for row in all_results(driver)}
        click("cover_tile_0")
        state = until("keyword then cover", lambda s: s["cover_filter"]["active"] and
                      s["search_text"] == query and s["result_count"] > 0)
        rows = all_results(driver)
        assert {row["path"] for row in rows} <= keyword_paths
        record("keyword-then-cover", state)

        # Setup only: a DB-known path lets the actual cover selection remain
        # selected while the UI refreshes the three-way intersection.
        actual_folder = str(Path(rows[0]["path"]).parent)
        driver.command({"cmd": "set_folder_filter", "path": actual_folder})
        state = until("three filters", lambda s: s["folder_filter"]["active"] and
                      s["cover_filter"]["active"] and s["search_text"] == query and s["result_count"] > 0)
        record("folder-cover-keyword-intersection", state)
        click("refresh")
        state = until("refresh three filters", lambda s: s["folder_filter"]["active"] and
                      s["cover_filter"]["active"] and s["search_text"] == query and s["result_count"] > 0)
        record("refresh-keeps-three-filters", state)

        driver.command({"cmd": "folder_search_sidebar", "section": 2})
        click("cover_mode_selector")
        click("cover_mode_option_1")
        state = until("cover mode with key", lambda s: s["cover_filter"]["mode"] == "has" and
                      s["cover_filter"]["active"] and s["folder_filter"]["active"] and
                      s["search_text"] == query)
        assert state["result_count"] > 0
        record("cover-mode-keeps-other-filters", state)
        driver.command({"cmd": "set_metadata_filter", "format": "WAV"})
        query = words[0]
        state = search(query)
        assert (state["metadata_filters"]["format"] == "WAV" and
                state["cover_filter"]["mode"] == "has" and
                state["cover_filter"]["active"] and state["folder_filter"]["active"] and
                state["result_count"] > 0)
        record("format-kept-after-real-search", state)
        click("refresh")
        state = until("format kept after refresh", lambda s: s["metadata_filters"]["format"] == "WAV" and
                      s["cover_filter"]["mode"] == "has" and s["cover_filter"]["active"] and
                      s["folder_filter"]["active"] and s["search_text"] == query and s["result_count"] > 0)
        record("format-kept-after-refresh", state)
        click("chip_cover_key")
        state = until("remove cover key with cover mode", lambda s:
                      not s["cover_filter"]["active"] and s["cover_filter"]["mode"] == "has" and
                      s["metadata_filters"]["format"] == "WAV" and s["folder_filter"]["active"] and
                      s["search_text"] == query and s["result_count"] > 0)
        record("remove-cover-key-keeps-cover-mode-and-format", state)
        click("cover_tile_0")
        state = until("restore cover key with cover mode", lambda s:
                      s["cover_filter"]["active"] and s["cover_filter"]["mode"] == "has" and
                      s["metadata_filters"]["format"] == "WAV" and s["folder_filter"]["active"] and
                      s["search_text"] == query and s["result_count"] > 0)
        record("cover-click-keeps-cover-mode-and-format", state)
        click("chip_cover_mode")
        state = until("remove cover mode", lambda s: s["cover_filter"]["mode"] == "all" and
                      s["cover_filter"]["active"] and s["folder_filter"]["active"] and
                      s["metadata_filters"]["format"] == "WAV" and
                      s["search_text"] == query and s["result_count"] > 0)
        record("remove-cover-mode-only", state)

        driver.command({"cmd": "folder_search_seed_extra_filters", "enabled": True})
        state = state_ready()
        assert (state["qa_filter_extras"]["key"] == "C" and
                state["qa_filter_extras"]["genre_count"] == 1 and
                state["qa_filter_extras"]["instrument_count"] == 1 and
                not state["qa_filter_extras"]["all_file_types"])
        click("clear_all")
        state = until("explicit clear all", lambda s: s["search_text"] == "" and
                      not s["folder_filter"]["active"] and not s["cover_filter"]["active"] and
                      s["cover_filter"]["mode"] == "all" and s["metadata_filters"]["format"] == "" and
                      s["qa_filter_extras"]["key"] == "" and
                      s["qa_filter_extras"]["genre_count"] == 0 and
                      s["qa_filter_extras"]["instrument_count"] == 0 and
                      s["qa_filter_extras"]["all_file_types"] and s["total_count"] >= 11000)
        record("explicit-clear-all", state)
        print("PASS: " + mode + " cover/keyword/folder/mode intersections, chip removals, refresh, clear all")
    except Exception as exc:
        failure = repr(exc)
        raise
    finally:
        if hwnd and win32gui.IsWindow(hwnd):
            win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=10)
        report = {"exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                  "mode": mode, "cases": cases, "failure": failure,
                  "input": "native ImGui cover click and RenderUI keyword submission"}
        (output / "result.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--isolation-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("live", "enter"), required=True)
    args = parser.parse_args()
    try:
        run(args.exe, args.isolation_root, args.output, args.mode)
    except Exception as exc:
        print("FAIL:", repr(exc), file=sys.stderr)
        raise
