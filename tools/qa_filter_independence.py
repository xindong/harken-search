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
import uuid
import wave

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
    from PIL import Image

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
        AURA_DISABLE_SINGLE_INSTANCE="1", HARKEN_QA_SEARCH_DELAY_MS="500"))
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
        raw = runtime / ("frame-" + uuid.uuid4().hex + ".ppm")
        driver.command({"cmd": "capture_preview_frame", "path": str(raw)})
        deadline = time.monotonic() + 15
        while True:
            try:
                with Image.open(raw) as captured:
                    captured.load()  # The renderer publishes the path before the pixels finish writing.
                    image = captured.convert("RGB")
                raw.unlink()
                break
            except (FileNotFoundError, OSError, PermissionError):
                assert time.monotonic() < deadline, "native frame capture did not finish"
                time.sleep(.1)
        assert len(image.getcolors(image.width * image.height) or []) > 16, "native frame is blank"
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
        with Image.open(output / "actual-library-before.png") as visible_window:
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
        click("refresh")
        state = until("manual refresh resets all filters", lambda s: s["search_text"] == "" and
                      not s["folder_filter"]["active"] and not s["cover_filter"]["active"] and
                      s["cover_filter"]["mode"] == "all" and s["metadata_filters"]["format"] == "" and
                      s["qa_filter_extras"]["key"] == "" and
                      s["qa_filter_extras"]["genre_count"] == 0 and
                      s["qa_filter_extras"]["instrument_count"] == 0 and
                      s["qa_filter_extras"]["all_file_types"] and s["total_count"] >= 11000)
        assert state["tabs"]["active_name"] == "全部文件", state["tabs"]
        record("manual-refresh-resets-all", state)

        driver.command({"cmd": "folder_search_seed_extra_filters", "enabled": True})
        click("clear_all")
        state = until("explicit clear all", lambda s: s["search_text"] == "" and
                      not s["folder_filter"]["active"] and not s["cover_filter"]["active"] and
                      s["cover_filter"]["mode"] == "all" and s["metadata_filters"]["format"] == "" and
                      s["qa_filter_extras"]["key"] == "" and
                      s["qa_filter_extras"]["genre_count"] == 0 and
                      s["qa_filter_extras"]["instrument_count"] == 0 and
                      s["qa_filter_extras"]["all_file_types"] and s["total_count"] >= 11000)
        record("explicit-clear-all", state)

        full_total = state["total_count"]
        driver.command({"cmd": "folder_search_collection", "kind": "recently_played"})
        state = until("recently played collection", lambda s:
                      s["qa_collection"]["smart"] == 2 and s["total_count"] < full_total)
        record("recently-played-before-refresh", state)
        click("refresh")
        state = until("refresh exits recently played", lambda s:
                      s["qa_collection"] == {"smart": 0, "user_index": -1} and
                      s["total_count"] == full_total and s["tabs"]["active_name"] == "全部文件")
        record("refresh-exits-recently-played", state)

        driver.command({"cmd": "folder_search_collection", "kind": "user"})
        state = until("user collection", lambda s:
                      s["qa_collection"]["user_index"] >= 0 and s["total_count"] == 0)
        record("user-collection-before-refresh", state)
        click("refresh")
        state = until("refresh exits user collection", lambda s:
                      s["qa_collection"] == {"smart": 0, "user_index": -1} and
                      s["total_count"] == full_total and s["tabs"]["active_name"] == "全部文件")
        record("refresh-exits-user-collection", state)

        # Click Refresh before a folder's deferred selection applies.
        # The product debounce is 150 ms; the QA command extends this window
        # for stable event ordering without changing the selection logic.
        driver.command({"cmd": "set_metadata_filter", "format": "WAV"})
        state = until("format before queued folder", lambda s: s["metadata_filters"]["format"] == "WAV")
        rect = state["qa_filter_ui_rects"]["refresh"]
        x, y = (rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2
        started = time.monotonic()
        driver.command({"cmd": "queue_folder_tree_selection", "path": str(isolated_folder),
                        "defer_for_refresh": True})
        driver.command({"cmd": "folder_search_pointer", "phase": "down", "x": x, "y": y})
        time.sleep(.015)
        driver.command({"cmd": "folder_search_pointer", "phase": "up", "x": x, "y": y})
        elapsed_ms = (time.monotonic() - started) * 1000
        assert elapsed_ms < 300, f"refresh missed the queued-folder window: {elapsed_ms:.1f} ms"
        time.sleep(.55)
        state = state_ready()
        record("refresh-cancels-queued-folder", state)
        cases[-1]["queued_refresh_elapsed_ms"] = elapsed_ms
        assert state["metadata_filters"]["format"] == "", "Refresh did not clear the active format filter"
        assert not state["folder_filter"]["active"], "queued folder filter reapplied after Refresh"
        assert state["total_count"] == full_total, "Refresh did not restore all files"

        # A completed full scan may refresh results, but only the later manual
        # click should clear the user's active filters.
        fixture = root / ("refresh-full-scan-fixture-" + uuid.uuid4().hex[:8])
        fixture.mkdir(exist_ok=True)
        for index in range(2):
            sample = fixture / f"refresh-sample-{index}.wav"
            if not sample.exists():
                with wave.open(str(sample), "wb") as sound:
                    sound.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
                    sound.writeframes(bytes(16000 * 2))
        search(query)
        driver.command({"cmd": "set_folder_filter", "path": actual_folder})
        state = until("filters before full scan", lambda s: s["search_text"] == query and
                      s["folder_filter"]["active"] and s["result_count"] > 0)
        record("before-full-scan", state)
        previous_scan = driver.command({"cmd": "scan_status"})["scan_started_at_ms"]
        driver.command({"cmd": "qa_rightclick_scan", "path": str(fixture), "cache_waveforms": True})
        deadline = time.monotonic() + 120
        while True:
            scan = driver.command({"cmd": "scan_status"})
            if scan["scan_started_at_ms"] != previous_scan and not scan["scanning"]:
                assert scan["scan_workflow_stage"] == "completed", scan
                assert (scan["scan_strategy"] == "full" and scan["added"] == 2 and
                        scan["errors"] == 0 and scan["mini_waveforms_completed"] == 2), scan
                break
            assert time.monotonic() < deadline, ("full scan did not complete", scan)
            time.sleep(.1)
        driver.command({"cmd": "open_scan_dialog"})
        driver.command({"cmd": "info_page_state"})
        for phase in ("hover", "down", "up"):
            driver.command({"cmd": "info_page_pointer", "key": "info.qaScanDone", "phase": phase})
        state = until("scan completion kept filters", lambda s: s["search_text"] == query and
                      s["folder_filter"]["active"] and
                      Path(s["folder_filter"]["path"]).resolve() == fixture.resolve())
        record("full-scan-kept-filters-before-click", state)
        cases[-1]["full_scan"] = scan
        click("refresh")
        state = until("manual refresh after full scan", lambda s: s["search_text"] == "" and
                      not s["folder_filter"]["active"] and s["tabs"]["active_name"] == "全部文件" and
                      s["total_count"] >= 11000)
        record("manual-refresh-after-full-scan", state)
        full_total = state["total_count"]
        click("refresh")
        state = until("repeat manual refresh", lambda s: s["search_text"] == "" and
                      not s["folder_filter"]["active"] and s["tabs"]["active_name"] == "全部文件" and
                      s["total_count"] == full_total)
        record("repeat-manual-refresh", state)

        # A locked tab still needs its saved filters to match the refreshed list
        # when the user switches away and returns.
        driver.command({"cmd": "set_metadata_filter", "format": "WAV"})
        until("locked-tab filter baseline", lambda s: s["metadata_filters"]["format"] == "WAV")
        driver.command({"cmd": "set_tab_lock", "locked": True})
        click("refresh")
        state = until("locked-tab refresh", lambda s: s["metadata_filters"]["format"] == "" and
                      s["total_count"] == full_total and s["tabs"]["active_locked"])
        assert not state["tabs"]["explicit_refresh_pending"], "locked tab stayed writable after Refresh settled"
        record("locked-tab-refresh", state)
        locked_index = state["tabs"]["active_index"]
        if state["tabs"]["count"] > 1:
            other_index = 0 if locked_index != 0 else 1
            driver.command({"cmd": "activate_tab", "tab_index": other_index})
        else:
            new_tab = driver.command({"cmd": "new_tab"})
            assert new_tab["ok"] and new_tab["tab_index"] != locked_index
            other_index = new_tab["tab_index"]
        until("switch away from locked tab", lambda s: s["tabs"]["active_index"] == other_index)
        driver.command({"cmd": "activate_tab", "tab_index": locked_index})
        state = until("return to locked tab", lambda s: s["tabs"]["active_index"] == locked_index)
        record("locked-tab-return-after-refresh", state)
        assert state["tabs"]["active_locked"], "manual refresh removed the tab lock"
        assert state["metadata_filters"]["format"] == "", "locked tab restored stale filters after Refresh"
        assert state["total_count"] == full_total, "locked tab did not keep the refreshed full list"

        # Supersede a locked tab's explicit Refresh before its query completes.
        # Returning to the tab must rebuild the saved reset view, not old WAV filters.
        driver.command({"cmd": "set_tab_lock", "locked": False})
        driver.command({"cmd": "set_metadata_filter", "format": "WAV"})
        until("supersede baseline", lambda s: s["metadata_filters"]["format"] == "WAV")
        driver.command({"cmd": "set_tab_lock", "locked": True})
        rect = state_ready()["qa_filter_ui_rects"]["refresh"]
        x, y = (rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2
        driver.command({"cmd": "folder_search_pointer", "phase": "down", "x": x, "y": y})
        time.sleep(.08)
        driver.command({"cmd": "folder_search_pointer", "phase": "up", "x": x, "y": y})
        deadline = time.monotonic() + 3
        while True:
            mid = driver.command({"cmd": "get_state"})
            if mid["tabs"]["explicit_refresh_pending"] and mid["pending_search_request_id"]:
                break
            assert time.monotonic() < deadline, "could not observe pending locked-tab Refresh"
            time.sleep(.01)
        driver.command({"cmd": "set_metadata_filter", "format": "MP3"})
        state_ready()
        driver.command({"cmd": "activate_tab", "tab_index": other_index})
        until("supersede switch away", lambda s: s["tabs"]["active_index"] == other_index)
        driver.command({"cmd": "activate_tab", "tab_index": locked_index})
        state = state_ready()
        record("locked-tab-refresh-superseded", state)
        assert state["metadata_filters"]["format"] == "", "superseded locked refresh restored old WAV filter"
        assert state["total_count"] == full_total, "superseded locked refresh did not restore full list"
    except Exception as exc:
        failure = repr(exc)
        raise
    finally:
        if hwnd and win32gui.IsWindow(hwnd):
            win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        try:
            exit_code = process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.terminate()
            exit_code = process.wait(timeout=10)
        report = {"exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                  "mode": mode, "cases": cases, "failure": failure, "exit_code": exit_code,
                  "input": "native ImGui cover click and RenderUI keyword submission"}
        (output / "result.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        if failure is None:
            assert exit_code == 0, f"native package exited with {exit_code}"
    print("PASS: " + mode + " filter independence, manual reset, and full-scan refresh")


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
