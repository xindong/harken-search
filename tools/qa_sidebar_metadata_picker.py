"""Test metadata picker UI in a fixed isolation package on a private desktop.

The source library is copied with SQLite backup. Input goes through the app's
isolated ImGui QA channel, never the user's desktop mouse.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time
import uuid

from qa_folder_shortcuts import Driver
from qa_required_library_setup import PrivateDesktop
from qa_transport_themes import THEMES, contrast


def capture(driver, output, name):
    from PIL import Image
    raw = driver.runtime / ("frame-" + uuid.uuid4().hex + ".ppm")
    driver.command({"cmd": "capture_preview_frame", "path": str(raw)})
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            with Image.open(raw) as image:
                image.load()
                assert len(image.getcolors(image.width * image.height) or []) > 16
                path = output / (name + ".png")
                image.convert("RGB").save(path)
            raw.unlink()
            return path
        except (FileNotFoundError, OSError, PermissionError):
            time.sleep(.08)
    raise TimeoutError("rendered frame capture did not finish")


def run(args):
    import win32api
    import win32con
    import win32event
    import win32gui
    import win32process

    exe, root = args.exe.resolve(), args.isolation_root.resolve()
    source, output = args.source_library.resolve(), args.output.resolve()
    if not exe.is_file() or not source.is_file():
        raise FileNotFoundError("QA EXE or source library is missing")
    settings = root / "profile/Local/HarkenSearch/settings.ini"
    actual_db = root / "profile/Local/HarkenSearch/Database/ActualLibrary/main.db"
    if output.exists() or (not args.reuse and (settings.exists() or actual_db.exists())):
        raise RuntimeError("Use a fresh evidence directory and matching isolation root")
    output.mkdir(parents=True)
    if args.reuse:
        assert settings.is_file() and actual_db.is_file()
    else:
        actual_db.parent.mkdir(parents=True, exist_ok=True)
        with sqlite3.connect(source.as_uri() + "?mode=ro", uri=True) as original:
            with sqlite3.connect(actual_db) as destination:
                original.backup(destination)
    with sqlite3.connect(actual_db) as database:
        names = {field: [row[0] for row in database.execute(
            f"SELECT DISTINCT {column} FROM audio_files WHERE {column} IS NOT NULL "
            f"AND {column} <> '' ORDER BY {column} COLLATE NOCASE")]
            for field, column in (("Artist", "artist"), ("Album", "album"),
                                  ("Category", "category"))}
    assert all(len(values) > 20 for values in names.values()), names
    if not args.reuse:
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text("\n".join((
            "viewMode=1", "activeSidebarSection=2", "showWelcomeOnStartup=0",
            "closeButtonAction=2", "searchRealtime=1", f"currentDbPath={actual_db}", ""
        )), encoding="utf-8")
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, output)
    desktop = PrivateDesktop()
    process = hwnd = None
    checks, failure = [], None
    mutated_artist_row = None
    mutated_category_row = None

    def state():
        return driver.command({"cmd": "get_state"})

    def wait_until(label, predicate, timeout=30):
        deadline, last = time.monotonic() + timeout, None
        while time.monotonic() < deadline:
            last = state()
            if predicate(last):
                return last
            time.sleep(.08)
        detail = {k: last.get(k) for k in ("window_width", "window_height", "metadata_filters")}
        detail["picker_rects"] = {k: v for k, v in last.get("qa_filter_ui_rects", {}).items()
                                  if k.startswith("metadata_")}
        raise AssertionError(f"{label}: {detail}")

    def pointer(x, y):
        driver.command({"cmd": "folder_search_pointer", "phase": "down", "x": x, "y": y})
        time.sleep(.09)
        driver.command({"cmd": "folder_search_pointer", "phase": "up", "x": x, "y": y})
        time.sleep(.18)

    def click(key):
        current = wait_until(key, lambda s: key in s.get("qa_filter_ui_rects", {}))
        rect = current["qa_filter_ui_rects"][key]
        pointer((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2)
        return state()

    def popup(field):
        key = f"metadata_{field}_popup"
        current = wait_until(key, lambda s: key in s.get("qa_filter_ui_rects", {}))
        return current, current["qa_filter_ui_rects"][key]

    def close_outside(field):
        current, rect = popup(field)
        x = rect[2] + 25 if rect[2] + 25 < current["window_width"] else rect[0] - 25
        pointer(x, min(rect[1] + 100, current["window_height"] - 20))
        return wait_until("outside closes " + field,
            lambda s: f"metadata_{field}_popup" not in s.get("qa_filter_ui_rects", {}))

    def right_click_outside(field):
        current, rect = popup(field)
        x = int(rect[2] + 25 if rect[2] + 25 < current["window_width"] else rect[0] - 25)
        y = int(min(rect[1] + 100, current["window_height"] - 20))
        position = (y << 16) | x
        win32gui.PostMessage(hwnd, win32con.WM_MOUSEMOVE, 0, position)
        win32gui.PostMessage(hwnd, win32con.WM_RBUTTONDOWN, win32con.MK_RBUTTON, position)
        time.sleep(.09)
        win32gui.PostMessage(hwnd, win32con.WM_RBUTTONUP, 0, position)
        time.sleep(.25)
        return state()

    def record(name, current):
        shot = capture(driver, output, name)
        checks.append({"name": name, "screenshot": str(shot),
                       "filters": current["metadata_filters"],
                       "picker_rects": {k: v for k, v in current["qa_filter_ui_rects"].items()
                                        if k.startswith("metadata_")}})

    try:
        startup = win32process.STARTUPINFO()
        startup.lpDesktop = "WinSta0\\" + desktop.name
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
        assert Path(identity["exe_path"]).resolve() == exe
        assert Path(identity["isolation_root"]).resolve() == root
        hwnd = identity["hwnd"]
        win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
        win32gui.SetWindowPos(hwnd, 0, 30, 30, 1500, 1000,
            win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        wait_until("actual library ready", lambda s: s.get("main_ui_ready") and
            s.get("active_view") == "database" and
            Path(s.get("active_library") or ".").resolve() == actual_db and
            s.get("window_height", 0) >= 850, timeout=60)
        driver.command({"cmd": "folder_search_sidebar", "section": 2})
        driver.command({"cmd": "clear_metadata_filter"})
        for field, column in (("Artist", "artist"), ("Album", "album"),
                              ("Category", "category")):
            pick_index = next(i for i, name in enumerate(names[field][:20])
                if name.isprintable() and len(name) > 3 and any(c.isalnum() for c in name))
            click(f"metadata_{field}_arrow")
            current, rect = popup(field)
            assert rect[2] - rect[0] >= 400 and rect[3] - rect[1] >= 700, (field, rect)
            visible = [k for k in current["qa_filter_ui_rects"]
                       if k.startswith(f"metadata_{field}_item_")]
            assert len(visible) >= 20, (field, len(visible))
            record(field.lower() + "-expanded", current)
            if field == "Artist":
                for query, shot_name in (("Fusehive", "artist-damaged-control-cleaned"),
                                         ("Boris", "artist-damaged-character-marked")):
                    click("metadata_Artist_input")
                    driver.command({"cmd": "info_edit_input", "text": query})
                    matching = next(i for i, name in enumerate(names["Artist"]) if query in name)
                    current = wait_until(shot_name, lambda s:
                        f"metadata_Artist_item_{matching}" in s.get("qa_filter_ui_rects", {}))
                    record(shot_name, current)
                    close_outside("Artist")
                    click("metadata_Artist_arrow")
                    popup("Artist")
            click(f"metadata_{field}_item_{pick_index}")
            current = wait_until(field + " selection", lambda s:
                s["metadata_filters"][column] == names[field][pick_index])
            checks.append({"name": field.lower() + "-selected", "value": names[field][pick_index],
                           "filters": current["metadata_filters"]})
            if field == "Artist":
                applied_artist = names[field][pick_index]
                artist_index = pick_index
        driver.command({"cmd": "clear_metadata_filter"})
        damaged_album = next(name for name in names["Album"]
            if name.startswith("Robot Monster Vocalizations Vol. 3") and chr(26) in name)
        damaged_album_index = names["Album"].index(damaged_album)
        click("metadata_Album_input")
        popup("Album")
        driver.command({"cmd": "info_edit_input",
                        "text": "Robot Monster Vocalizations Vol. 3"})
        wait_until("damaged album option", lambda s:
            f"metadata_Album_item_{damaged_album_index}" in s.get("qa_filter_ui_rects", {}))
        click(f"metadata_Album_item_{damaged_album_index}")
        current = wait_until("damaged album applied exactly", lambda s:
            s["metadata_filters"]["album"] == damaged_album and
            s["search_ready"] and s["result_count"] > 0)
        record("album-damaged-selected-input", current)
        driver.command({"cmd": "clear_metadata_filter"})
        click("metadata_Artist_arrow")
        popup("Artist")
        click(f"metadata_Artist_item_{artist_index}")
        baseline = wait_until("artist-only results", lambda s:
            s["metadata_filters"]["artist"] == applied_artist and
            s["search_ready"] and s["result_count"] > 0)
        baseline_count = baseline["result_count"]
        click("metadata_Artist_input")
        popup("Artist")
        driver.command({"cmd": "info_edit_input", "text": "zzq9__no_match"})
        time.sleep(.2)
        current = state()
        assert current["metadata_filters"]["artist"] == applied_artist
        assert current["result_count"] == baseline_count
        assert not any(k.startswith("metadata_Artist_item_")
                       for k in current["qa_filter_ui_rects"])
        record("artist-search-draft", current)
        current = close_outside("Artist")
        assert current["metadata_filters"]["artist"] == applied_artist
        assert current["result_count"] == baseline_count
        record("artist-outside-restored", current)
        checks.append({"name": "outside-cancels-draft", "filters": current["metadata_filters"]})
        click("metadata_Artist_input")
        popup("Artist")
        driver.command({"cmd": "info_edit_input", "text": "zzq9__right_click"})
        current = right_click_outside("Artist")
        assert f"metadata_Artist_popup" not in current["qa_filter_ui_rects"]
        time.sleep(.25)
        current = state()
        assert f"metadata_Artist_popup" not in current["qa_filter_ui_rects"]
        assert current["metadata_filters"]["artist"] == applied_artist
        record("artist-right-click-outside-restored", current)
        click("metadata_Artist_input")
        popup("Artist")
        driver.command({"cmd": "info_edit_input", "text": "sound"})
        current = wait_until("positive name search", lambda s:
            any(k.startswith("metadata_Artist_item_")
                for k in s.get("qa_filter_ui_rects", {})))
        assert current["metadata_filters"]["artist"] == applied_artist
        assert current["result_count"] == baseline_count
        record("artist-search-matches", current)
        close_outside("Artist")
        click("metadata_Artist_arrow")
        popup("Artist")
        driver.command({"cmd": "settings_ui", "action": "key", "key": "K", "down": True})
        time.sleep(.1)
        driver.command({"cmd": "settings_ui", "action": "key", "key": "K", "down": False})
        k_index = next((i for i, name in enumerate(names["Artist"])
                        if name.lstrip().lower().startswith("k")), -1)
        if k_index >= 0:
            current = wait_until("K initial jump", lambda s:
                f"metadata_Artist_item_{k_index}" in s.get("qa_filter_ui_rects", {}))
            record("artist-K-jump", current)
        close_outside("Artist")
        driver.command({"cmd": "transport_audit_setup", "scenario": "selection"})
        driver.command({"cmd": "settings_ui", "action": "shortcut_test_background", "enabled": True})
        wait_until("shortcut test setup", lambda s: abs(s["region_out"] - .8) < .001)
        click("metadata_Artist_arrow")
        popup("Artist")
        driver.command({"cmd": "settings_ui", "action": "key", "key": "O", "down": True})
        time.sleep(.12)
        driver.command({"cmd": "settings_ui", "action": "key", "key": "O", "down": False})
        current = state()
        assert abs(current["region_out"] - .8) < .001, current["region_out"]
        checks.append({"name": "initial-jump-does-not-set-out-point",
                       "region_out": current["region_out"]})
        driver.command({"cmd": "settings_ui", "action": "shortcut_test_background", "enabled": False})
        close_outside("Artist")
        for theme, label in THEMES:
            driver.command({"cmd": "set_theme", "theme": theme})
            current = wait_until("metadata input colors rendered", lambda s:
                {"frame", "hint"} <= s.get("qa_metadata_input_colors", {}).keys())
            colors = current["qa_metadata_input_colors"]
            hint_ratio = contrast([round(channel) for channel in colors["hint"]],
                                  [round(channel) for channel in colors["frame"]])
            assert hint_ratio >= 4.5, (label, hint_ratio, colors)
            checks.append({"name": "metadata-hint-contrast", "theme": theme,
                           "ratio": hint_ratio, "colors": colors})
            click("metadata_Artist_arrow")
            current, _ = popup("Artist")
            record(f"theme-{theme}-artist-expanded", current)
            close_outside("Artist")
            checks.append({"name": "theme", "theme": theme, "label": label})
        win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
        win32gui.SetWindowPos(hwnd, 0, 30, 30, 1200, 620,
            win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        wait_until("short window resized", lambda s: s.get("window_height", 0) <= 650)
        driver.command({"cmd": "folder_search_sidebar", "section": 2})
        click("metadata_Artist_arrow")
        current, rect = popup("Artist")
        assert rect[0] >= 0 and rect[2] <= current["window_width"]
        assert rect[3] <= current["window_height"]
        record("artist-short-window", current)
        close_outside("Artist")
        click("metadata_Artist_arrow")
        popup("Artist")
        click("metadata_Artist_all")
        current = wait_until("All clears artist", lambda s:
            s["metadata_filters"]["artist"] == "")
        checks.append({"name": "artist-all-clears", "filters": current["metadata_filters"]})
        long_category = max(names["Category"], key=lambda name: len(name.encode("utf-8")))
        assert len(long_category.encode("utf-8")) > 127
        long_index = names["Category"].index(long_category)
        click("metadata_Category_input")
        popup("Category")
        driver.command({"cmd": "info_edit_input", "text": long_category[:28]})
        wait_until("long category name search", lambda s:
            f"metadata_Category_item_{long_index}" in s.get("qa_filter_ui_rects", {}))
        click(f"metadata_Category_item_{long_index}")
        current = wait_until("full category name applied", lambda s:
            s["metadata_filters"]["category"] == long_category and s["search_ready"] and
            "chip_category" in s.get("qa_filter_ui_rects", {}))
        chip = current["qa_filter_ui_rects"]["chip_category"]
        assert chip[2] - chip[0] <= 260, chip
        assert chip[2] <= current["window_width"], chip
        record("long-category-selected", current)

        # A library may gain a name after this picker was already opened.
        fresh_artist = "ZZQ Fresh Artist 20261103"
        with sqlite3.connect(actual_db) as database:
            mutated_artist_row = database.execute(
                "SELECT id, artist FROM audio_files WHERE artist IS NOT NULL AND artist <> '' LIMIT 1"
            ).fetchone()
            assert mutated_artist_row is not None
            database.execute("UPDATE audio_files SET artist = ? WHERE id = ?",
                             (fresh_artist, mutated_artist_row[0]))
            fresh_index = [row[0] for row in database.execute(
                "SELECT DISTINCT artist FROM audio_files WHERE artist IS NOT NULL "
                "AND artist <> '' ORDER BY artist COLLATE NOCASE")].index(fresh_artist)
        click("metadata_Artist_input")
        popup("Artist")
        driver.command({"cmd": "info_edit_input", "text": fresh_artist})
        wait_until("new artist appears after reopening", lambda s:
            f"metadata_Artist_item_{fresh_index}" in s.get("qa_filter_ui_rects", {}), timeout=5)
        click(f"metadata_Artist_item_{fresh_index}")
        current = wait_until("new artist can be selected", lambda s:
            s["metadata_filters"]["artist"] == fresh_artist)
        record("artist-added-after-first-open", current)

        # A selected database value must survive well beyond the old 1024-byte UI buffer.
        driver.command({"cmd": "clear_metadata_filter"})
        long_raw_category = "QA Long Category " + "A" * 1100 + "界"
        with sqlite3.connect(actual_db) as database:
            mutated_category_row = database.execute(
                "SELECT id, category FROM audio_files WHERE category IS NOT NULL AND category <> '' LIMIT 1"
            ).fetchone()
            assert mutated_category_row is not None
            database.execute("UPDATE audio_files SET category = ? WHERE id = ?",
                             (long_raw_category, mutated_category_row[0]))
            long_raw_index = [row[0] for row in database.execute(
                "SELECT DISTINCT category FROM audio_files WHERE category IS NOT NULL "
                "AND category <> '' ORDER BY category COLLATE NOCASE")].index(long_raw_category)
        click("metadata_Category_input")
        popup("Category")
        driver.command({"cmd": "info_edit_input", "text": "QA Long Category"})
        wait_until("long raw category option", lambda s:
            f"metadata_Category_item_{long_raw_index}" in s.get("qa_filter_ui_rects", {}))
        click(f"metadata_Category_item_{long_raw_index}")
        current = wait_until("long raw category exact filter", lambda s:
            s["metadata_filters"]["category"] == long_raw_category and
            s["search_ready"] and s["result_count"] > 0, timeout=5)
        record("long-raw-category-selected", current)
        click("metadata_Category_input")
        current = wait_until("long category input stays complete", lambda s:
            s.get("qa_metadata_input_text", {}).get("Category") == long_raw_category)
        checks.append({"name": "long-raw-category-input-complete",
                       "input_bytes": len(current["qa_metadata_input_text"]["Category"].encode("utf-8"))})
        driver.command({"cmd": "info_edit_input", "action": "home_down"})
        time.sleep(.12)
        driver.command({"cmd": "info_edit_input", "action": "home_up"})
        driver.command({"cmd": "info_edit_input", "text": "Q"})
        current = wait_until("long category query edit stays complete", lambda s:
            s.get("qa_metadata_draft_query", {}).get("Category") == "Q" + long_raw_category and
            s["metadata_filters"]["category"] == long_raw_category, timeout=5)
        checks.append({"name": "long-category-prefix-edit-complete",
                       "query_bytes": len(current["qa_metadata_draft_query"]["Category"].encode("utf-8"))})
        # Keep editing after the active ImGui input has outgrown its original
        # allocation; a larger caller buffer alone must not corrupt TextA.
        driver.command({"cmd": "info_edit_input", "text": "X" * 900})
        current = wait_until("long category first growth stays complete", lambda s:
            len(s.get("qa_metadata_draft_query", {}).get("Category", "").encode("utf-8")) == 2021 and
            s["qa_metadata_draft_query"]["Category"].endswith(long_raw_category))
        driver.command({"cmd": "info_edit_input", "text": "Y" * 200})
        current = wait_until("long category second growth stays complete", lambda s:
            len(s.get("qa_metadata_draft_query", {}).get("Category", "").encode("utf-8")) == 2221 and
            s["qa_metadata_draft_query"]["Category"].endswith(long_raw_category) and
            s["metadata_filters"]["category"] == long_raw_category, timeout=5)
        checks.append({"name": "long-category-growth-after-activation",
                       "query_bytes": len(current["qa_metadata_draft_query"]["Category"].encode("utf-8"))})
    except BaseException as exc:
        failure = repr(exc)
        raise
    finally:
        exit_code = None
        if hwnd and win32gui.IsWindow(hwnd):
            win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        if process:
            if win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_TIMEOUT:
                win32api.TerminateProcess(process, 1)
                win32event.WaitForSingleObject(process, 5000)
            exit_code = win32process.GetExitCodeProcess(process)
            process.Close()
        if mutated_artist_row is not None:
            with sqlite3.connect(actual_db) as database:
                database.execute("UPDATE audio_files SET artist = ? WHERE id = ?",
                                 (mutated_artist_row[1], mutated_artist_row[0]))
        if mutated_category_row is not None:
            with sqlite3.connect(actual_db) as database:
                database.execute("UPDATE audio_files SET category = ? WHERE id = ?",
                                 (mutated_category_row[1], mutated_category_row[0]))
        desktop.close()
        report = {"status": "pass" if failure is None and exit_code == 0 else "incomplete",
                  "failure": failure, "exit_code": exit_code, "exe": str(exe),
                  "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                  "source_library": str(source), "library_copy": str(actual_db),
                  "library_counts": {field: len(values) for field, values in names.items()},
                  "identity": identity if "identity" in locals() else None,
                  "checks": checks, "captured_at": datetime.now(timezone.utc).isoformat()}
        (output / "result.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        if failure is None:
            assert exit_code == 0, exit_code


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--isolation-root", type=Path, required=True)
    parser.add_argument("--source-library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reuse", action="store_true")
    run(parser.parse_args())
