"""Real FLAC/OGG metadata search checks, using only app-local input and synthetic files."""
from __future__ import annotations

import argparse
import hashlib
import json
import sqlite3
import subprocess
import time
from pathlib import Path

from qa_folder_shortcuts import Driver
from qa_required_library_setup import PrivateDesktop
from qa_sidebar_metadata_picker import capture

TAGS_OLD = {
    "TITLE": "Embedded Title",
    "COMPOSER": "ElderScore",
    "BPM": "181",
    "INITIALKEY": "Dorian",
    "DATE": "2034",
    "DESCRIPTION": "RoomToneDescription",
    "KEYWORDS": "FabricRipple",
    "VENDOR LABEL": "Zephyrtoken",
}
TAGS_NEW = {
    "TITLE": "Embedded Title",
    "COMPOSER": "CinderScore",
    "BPM": "187",
    "INITIALKEY": "Dorian",
    "DATE": "2035",
    "DESCRIPTION": "RoomToneDescription",
    "KEYWORDS": "FabricRipple",
    "VENDOR LABEL": "Opaltoken",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_audio(path, codec, tags):
    if path.exists():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-n", "-f", "lavfi",
               "-i", "sine=frequency=430:sample_rate=44100:duration=1.2", "-c:a", codec]
    for key, value in tags.items():
        command += ["-metadata", f"{key}={value}"]
    subprocess.run(command + [str(path)], check=True)


def run(args):
    import win32con
    import win32event
    import win32gui
    import win32process

    root, exe, output = (item.resolve() for item in (args.root, args.exe, args.output))
    assert output.is_relative_to(root)
    output.mkdir(parents=True, exist_ok=False)
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    db = root / "profile/Local/HarkenSearch/Database/VorbisQa/main.db"
    settings = root / "profile/Local/HarkenSearch/settings.ini"
    old = root / "fixtures/flac/OldRoom.flac"
    new = root / "fixtures/ogg/NewRoom.ogg"
    make_audio(old, "flac", TAGS_OLD)
    make_audio(new, "libvorbis", TAGS_NEW)
    before = {item.name: digest(item) for item in (old, new)}
    prior = root / "source-hashes.json"
    if prior.exists():
        assert json.loads(prior.read_text(encoding="utf-8")) == before
    else:
        prior.write_text(json.dumps(before, indent=2), encoding="utf-8")
    if not settings.exists():
        db.parent.mkdir(parents=True, exist_ok=True)
        with sqlite3.connect(db) as connection:
            connection.execute("PRAGMA user_version=1")
        settings.write_text("viewMode=1\nshowWelcomeOnStartup=0\ncloseButtonAction=2\n"
                            "searchRealtime=1\nlanguage=0\ncurrentDbPath=" + str(db) + "\n",
                            encoding="utf-8")
    driver = Driver(runtime, output)
    desktop = PrivateDesktop()
    process = hwnd = None
    report = {"phase": args.phase, "exe": str(exe), "sha256": digest(exe),
              "library": str(db), "cases": [], "source_before": before}

    def cmd(action, **payload):
        return driver.command({"cmd": action, **payload})

    def wait(label, callback, predicate, timeout=35):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = callback()
            if predicate(value):
                return value
            time.sleep(.12)
        raise AssertionError((label, value))

    def search(query, expected, screenshot=None):
        # Re-issue the query while waiting: the page can still be settling when a
        # scan or the background metadata pass just finished, and a real user
        # typing again must see the same final result.
        deadline = time.monotonic() + 24
        result = None
        attempts = 0
        while time.monotonic() < deadline:
            attempts += 1
            cmd("search", query=query)
            time.sleep(.55)
            wait("search ready", lambda: cmd("wait_ready"), lambda row: row["ready"], 12)
            result = cmd("get_results", limit=20)
            if sorted(item["name"] for item in result["items"]) == sorted(expected):
                break
            time.sleep(.6)
        assert sorted(item["name"] for item in result["items"]) == sorted(expected), (query, result)
        report["cases"].append({"query": query, "names": expected, "attempts": attempts})
        if screenshot:
            capture(driver, output, screenshot)
        return result

    def scan(folder):
        started = cmd("scan_folder", path=str(folder), recursive=False, update_existing=True,
                      remove_missing=False, cache_waveforms=False, build_similarity_index=False,
                      worker_threads=2)
        report["cases"].append({"scan_start": started})
        time.sleep(.5)
        finished = wait("scan completed", lambda: cmd("scan_status"),
                        lambda row: not row["scanning"], 90)
        assert finished["errors"] == 0, finished
        report["cases"].append({"scan_end": finished})
        cmd("scan_acknowledge_complete")
        # Scanning selects the scanned folder in the tree, which switches on the
        # folder filter. Clear it so the following queries cover the whole library.
        cmd("clear_folder_filter")

    def record_for(filename):
        with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as connection:
            connection.row_factory = sqlite3.Row
            row = connection.execute("SELECT title, description, keywords, composer, bpm, musical_key, "
                                     "track_year, id3_text_tags FROM audio_files WHERE file_name = ?",
                                     (filename,)).fetchone()
            return dict(row) if row else None

    def wait_record(filename, predicate, timeout=45):
        return wait("record " + filename, lambda: record_for(filename), predicate, timeout)

    try:
        process, pid = desktop.start(exe, environment_overrides={
            "AURA_RUNTIME_ROOT": str(runtime), "AURA_HARNESS_LANE_KIND": "folder-search",
        })
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        assert identity["pid"] == pid and Path(identity["exe_path"]).resolve() == exe
        assert Path(identity["isolation_root"]).resolve() == root
        report["identity"] = identity
        hwnd = identity["hwnd"]
        win32gui.SetWindowPos(hwnd, 0, 30, 30, 1500, 1000,
                              win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        state = wait("isolated library ready", lambda: cmd("get_state"),
                     lambda row: row["main_ui_ready"] and row.get("db_path"), 60)
        assert Path(state["db_path"]).resolve() == db
        cmd("set_view_mode", mode="database")
        cmd("clear_folder_filter")
        if args.phase == "baseline":
            scan(old.parent)
            search("OldRoom", [old.name], "01-baseline-filename")
            search("Zephyrtoken", [], "02-baseline-no-composer-hit")
            baseline_row = record_for(old.name)
            report["old_record_at_baseline"] = baseline_row
            assert baseline_row["composer"] in (None, ""), baseline_row
        elif args.phase == "candidate":
            backfilled = wait_record(
                old.name, lambda row: row is not None and row["composer"] == "ElderScore")
            report["old_record_after_backfill"] = backfilled
            assert backfilled["title"] == "Library override", backfilled
            assert backfilled["description"] == "Manual description", backfilled
            assert backfilled["musical_key"] == "Dorian" and str(backfilled["bpm"]) == "181", backfilled
            assert str(backfilled["track_year"]) == "2034", backfilled
            assert json.loads(backfilled["id3_text_tags"])["VENDOR LABEL"] == "Zephyrtoken", backfilled
            search("Zephyrtoken", [old.name], "03-old-library-autofilled")
            scan(new.parent)
            new_record = wait_record(
                new.name, lambda row: row is not None and row["composer"] == "CinderScore")
            assert str(new_record["bpm"]) == "187" and new_record["musical_key"] == "Dorian", new_record
            assert str(new_record["track_year"]) == "2035", new_record
            assert json.loads(new_record["id3_text_tags"])["VENDOR LABEL"] == "Opaltoken", new_record
            report["new_record"] = new_record
            for query in ("CinderScore", "187", "Opaltoken"):
                search(query, [new.name], "04-new-ogg-hit" if query == "Opaltoken" else None)
            search("Dorian", [old.name, new.name])
            search("2035", [new.name], "05-new-ogg-year")
        else:
            search("Opaltoken", [new.name])
            cmd("folder_search_pointer", phase="hover", x=args.pointer[0], y=args.pointer[1])
            time.sleep(2.4)
            capture(driver, output, "06-tooltip")
    except BaseException as exc:
        report["error"] = repr(exc)
        raise
    finally:
        if process is not None:
            if hwnd is not None:
                win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
            if win32event.WaitForSingleObject(process, 4000) != win32con.WAIT_OBJECT_0:
                win32process.TerminateProcess(process, 1)
                win32event.WaitForSingleObject(process, 5000)
            process.Close()
        desktop.close()
        after = {item.name: digest(item) for item in (old, new)}
        report["source_after"] = after
        report["source_unchanged"] = before == after
        (output / "receipt.json").write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                            encoding="utf-8")
        assert before == after, (before, after)
    if args.phase == "baseline":
        with sqlite3.connect(db) as connection:
            changed = connection.execute(
                "UPDATE audio_files SET title=?, description=? WHERE file_name=?",
                ("Library override", "Manual description", old.name)).rowcount
            assert changed == 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for argument in ("exe", "root", "output"):
        parser.add_argument("--" + argument, type=Path, required=True)
    parser.add_argument("--phase", choices=("baseline", "candidate", "tooltip"), required=True)
    parser.add_argument("--pointer", type=int, nargs=2)
    run(parser.parse_args())
