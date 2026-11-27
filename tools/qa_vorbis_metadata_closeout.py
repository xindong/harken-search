"""Closeout checks for the FLAC/OGG comment fixes, on an isolated real library.

Covers the three review findings that need the running product:
  * a Vorbis file that carries only music tags still imports its fields
  * selecting a FLAC/OGG row and rescanning refreshes its custom tags
  * a partially filled legacy row is completed without replacing the value the
    user already set, and the pass records completion only after a clean run

The remaining three findings (fill-only write guard, failed-write retry, cancel
between files) are exercised by TestPreviewPinyinDatabase --flac-search.
"""
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
TAGS_ONLY = {
    "COMPOSER": "OnlyScore",
    "BPM": "199",
    "INITIALKEY": "C#m",
    "VENDOR ONLY": "SoloValue",
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
TAGS_NEW_EDITED = dict(TAGS_NEW, **{"BPM": "199", "VENDOR LABEL": "CopperToken"})


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


def rewrite_audio(path, codec, tags):
    path.unlink(missing_ok=True)
    make_audio(path, codec, tags)


def run(args):
    import win32con
    import win32event
    import win32gui
    import win32process

    root, exe, output = (item.resolve() for item in (args.root, args.exe, args.output))
    assert output.is_relative_to(root)
    output.mkdir(parents=True, exist_ok=True)
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    db = root / "profile/Local/HarkenSearch/Database/VorbisCloseout/main.db"
    settings = root / "profile/Local/HarkenSearch/settings.ini"
    old = root / "fixtures/flac/OldRoom.flac"
    only = root / "fixtures/flac/MusicOnly.flac"
    new = root / "fixtures/ogg/NewRoom.ogg"
    make_audio(old, "flac", TAGS_OLD)
    make_audio(only, "flac", TAGS_ONLY)
    make_audio(new, "libvorbis", TAGS_NEW)
    source_names = (old, only, new)
    before = {item.name: digest(item) for item in source_names}
    (root / "source-hashes.json").write_text(json.dumps(before, indent=2), encoding="utf-8")
    if not settings.exists():
        db.parent.mkdir(parents=True, exist_ok=True)
        with sqlite3.connect(db) as connection:
            connection.execute("PRAGMA user_version=1")
        settings.write_text("viewMode=1\nshowWelcomeOnStartup=0\ncloseButtonAction=2\n"
                            "searchRealtime=1\nlanguage=0\ncurrentDbPath=" + str(db) + "\n",
                            encoding="utf-8")

    report = {"exe": str(exe), "sha256": digest(exe), "library": str(db),
              "cases": [], "source_before": before}

    def command(driver, action, timeout=25, **payload):
        return driver.command({"cmd": action, **payload}, timeout=timeout)

    def wait(label, callback, predicate, timeout=40):
        deadline = time.monotonic() + timeout
        value = None
        while time.monotonic() < deadline:
            value = callback()
            if predicate(value):
                return value
            time.sleep(.15)
        raise AssertionError((label, value))

    def record_for(filename):
        with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as connection:
            connection.row_factory = sqlite3.Row
            row = connection.execute(
                "SELECT title, description, composer, bpm, musical_key, track_year, id3_text_tags "
                "FROM audio_files WHERE file_name = ?", (filename,)).fetchone()
            return dict(row) if row else None

    def custom_tags(row):
        return json.loads(row["id3_text_tags"] or "{}")

    def shot(driver, name):
        # The scan-complete dialog can appear a beat after the status flips; it
        # is already acknowledged in most captures.
        try:
            command(driver, "scan_acknowledge_complete")
        except RuntimeError:
            pass
        time.sleep(.4)
        capture(driver, output, name)

    def search(driver, query, expected, screenshot=None):
        deadline = time.monotonic() + 24
        result = None
        attempts = 0
        while time.monotonic() < deadline:
            attempts += 1
            command(driver, "search", query=query)
            time.sleep(.55)
            wait("search ready", lambda: command(driver, "wait_ready"), lambda row: row["ready"], 12)
            result = command(driver, "get_results", limit=20)
            if sorted(item["name"] for item in result["items"]) == sorted(expected):
                break
            time.sleep(.6)
        assert sorted(item["name"] for item in result["items"]) == sorted(expected), (query, result)
        report["cases"].append({"query": query, "names": expected, "attempts": attempts})
        if screenshot:
            shot(driver, screenshot)
        return result

    def scan(driver, folder):
        started = command(driver, "scan_folder", path=str(folder), recursive=False,
                          update_existing=True, remove_missing=False, cache_waveforms=False,
                          build_similarity_index=False, worker_threads=2)
        report["cases"].append({"scan_start": started})
        time.sleep(.5)
        finished = wait("scan completed", lambda: command(driver, "scan_status"),
                        lambda row: not row["scanning"], 90)
        assert finished["errors"] == 0, finished
        report["cases"].append({"scan_end": finished})
        command(driver, "scan_acknowledge_complete")
        command(driver, "clear_folder_filter")

    def session(index):
        driver = Driver(runtime, output)
        desktop = PrivateDesktop()
        process = hwnd = None
        try:
            process, pid = desktop.start(exe, environment_overrides={
                "AURA_RUNTIME_ROOT": str(runtime), "AURA_HARNESS_LANE_KIND": "folder-search",
            })
            identity = command(driver, "magic_eq_identity", timeout=60)
            assert identity["pid"] == pid and Path(identity["exe_path"]).resolve() == exe
            assert Path(identity["isolation_root"]).resolve() == root
            report[f"identity_{index}"] = identity
            hwnd = identity["hwnd"]
            win32gui.SetWindowPos(hwnd, 0, 30, 30, 1500, 1000,
                                  win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
            win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
            state = wait("isolated library ready", lambda: command(driver, "get_state"),
                         lambda row: row["main_ui_ready"] and row.get("db_path"), 60)
            assert Path(state["db_path"]).resolve() == db
            command(driver, "set_view_mode", mode="database")
            command(driver, "clear_folder_filter")
            return driver, desktop, process, hwnd
        except BaseException:
            if process is not None:
                if hwnd is not None:
                    win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
                win32event.WaitForSingleObject(process, 4000)
                process.Close()
            desktop.close()
            raise

    def end(process, hwnd, desktop):
        if hwnd is not None:
            win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        if win32event.WaitForSingleObject(process, 4000) != win32con.WAIT_OBJECT_0:
            win32process.TerminateProcess(process, 1)
            win32event.WaitForSingleObject(process, 5000)
        process.Close()
        desktop.close()

    try:
        # ---- session 1: import + rescan -----------------------------------
        driver, desktop, process, hwnd = session(1)
        try:
            scan(driver, old.parent)
            only_row = wait("music-only row", lambda: record_for(only.name),
                            lambda row: row is not None and row["composer"] == "OnlyScore", 45)
            assert only_row["bpm"] == "199" and only_row["musical_key"] == "C#m", only_row
            assert custom_tags(only_row).get("VENDOR ONLY") == "SoloValue", only_row
            report["music_only_record"] = only_row
            search(driver, "OnlyScore", [only.name], "01-music-only-import")
            search(driver, "SoloValue", [only.name], "02-music-only-custom-tag")

            scan(driver, new.parent)
            new_row = wait("new ogg row", lambda: record_for(new.name),
                           lambda row: row is not None and row["composer"] == "CinderScore", 45)
            assert str(new_row["bpm"]) == "187", new_row
            report["new_record_before_rescan"] = new_row

            # Rewrite the file, then rescan the selected row: FLAC/OGG must save
            # the reread custom tags the way MP3 already does.
            rewrite_audio(new, "libvorbis", TAGS_NEW_EDITED)
            search(driver, "NewRoom", [new.name])
            command(driver, "rescan_metadata", row=0)
            shot(driver, "03-rescan-action")
            rescanned = wait("rescan saved custom tags", lambda: record_for(new.name),
                             lambda row: row is not None and
                             custom_tags(row).get("VENDOR LABEL") == "CopperToken", 45)
            assert str(rescanned["bpm"]) == "199", rescanned
            assert rescanned["title"] == "Embedded Title", rescanned
            report["new_record_after_rescan"] = rescanned
            search(driver, "CopperToken", [new.name], "04-rescan-custom-tag-hit")
            search(driver, "Opaltoken", [])
        finally:
            end(process, hwnd, desktop)
            # Only the OGG fixture is rewritten on purpose; the two FLAC sources
            # must come out byte-identical.
            after = {item.name: digest(item) for item in source_names}
            report["source_after_session_1"] = after
            assert after[old.name] == before[old.name], after
            assert after[only.name] == before[only.name], after

        # ---- prepare a partially filled legacy library ---------------------
        with sqlite3.connect(db) as connection:
            connection.execute(
                "UPDATE audio_files SET title='Library override', description='Manual description', "
                "composer=NULL, bpm=NULL, musical_key=NULL, track_year=NULL, id3_text_tags=NULL "
                "WHERE file_name=?", (old.name,))
            connection.execute(
                "UPDATE audio_files SET composer='LibraryComposer', bpm=NULL, musical_key=NULL "
                "WHERE file_name=?", (only.name,))
            connection.execute(
                "DELETE FROM app_meta WHERE key IN ('legacy_vorbis_metadata_refresh',"
                " 'legacy_vorbis_metadata_refresh_cursor')")
            connection.commit()
        time.sleep(.3)
        seeded = {name: record_for(name) for name in (old.name, only.name)}
        report["seeded_partial_state"] = seeded

        # ---- session 2: the one-time backfill ------------------------------
        driver, desktop, process, hwnd = session(2)
        try:
            backfilled = wait(
                "legacy row backfilled", lambda: record_for(old.name),
                lambda row: row is not None and row["composer"] == "ElderScore", 60)
            assert backfilled["title"] == "Library override", backfilled
            assert backfilled["description"] == "Manual description", backfilled
            assert str(backfilled["bpm"]) == "181" and backfilled["musical_key"] == "Dorian", backfilled
            assert str(backfilled["track_year"]) == "2034", backfilled
            assert custom_tags(backfilled).get("VENDOR LABEL") == "Zephyrtoken", backfilled
            report["legacy_record_after_backfill"] = backfilled

            partial = wait(
                "partially filled row completed", lambda: record_for(only.name),
                lambda row: row is not None and row["bpm"] == "199", 60)
            assert partial["composer"] == "LibraryComposer", partial
            assert partial["musical_key"] == "C#m", partial
            report["partial_record_after_backfill"] = partial

            # A custom tag that only the backfill could supply is searchable in
            # the same session; the metadata columns are re-read on the next
            # library open, which session 3 checks.
            search(driver, "Zephyrtoken", [old.name], "05-legacy-autofilled")
            def marker_value():
                with sqlite3.connect(db) as connection:
                    row = connection.execute(
                        "SELECT value FROM app_meta WHERE key='legacy_vorbis_metadata_refresh'"
                    ).fetchone()
                return None if row is None else str(row[0])
            marker = wait("backfill marker", marker_value, lambda value: value == "1", 30)
            report["backfill_marker"] = marker
        finally:
            end(process, hwnd, desktop)

        # ---- session 3: the completed library, reopened --------------------
        driver, desktop, process, hwnd = session(3)
        try:
            search(driver, "ElderScore", [old.name], "06-legacy-composer")
            search(driver, "181", [old.name])
            search(driver, "SoloValue", [only.name], "07-partial-custom-tag")
            search(driver, "LibraryComposer", [only.name], "08-partial-kept-composer")
        finally:
            end(process, hwnd, desktop)
    except BaseException as exc:
        report["error"] = repr(exc)
        raise
    finally:
        report["source_after"] = {item.name: digest(item) for item in source_names}
        (output / "receipt.json").write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                            encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for argument in ("exe", "root", "output"):
        parser.add_argument("--" + argument, type=Path, required=True)
    run(parser.parse_args())
