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


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_mp3(path, composer, token, bpm):
    if path.exists():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = path.with_suffix(".raw.mp3")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-n", "-f", "lavfi",
         "-i", "sine=frequency=430:sample_rate=44100:duration=1.2",
         "-c:a", "libmp3lame", "-id3v2_version", "0", "-write_id3v1", "0", str(raw)],
        check=True,
    )

    def frame(kind, content):
        return kind.encode("ascii") + len(content).to_bytes(4, "big") + bytes(2) + content

    def text(kind, value):
        return frame(kind, b"\x01\xff\xfe" + value.encode("utf-16-le"))

    def custom(name, value):
        return frame("TXXX", b"\x01\xff\xfe" + name.encode("utf-16-le") + b"\x00\x00\xff\xfe" +
                     value.encode("utf-16-le"))

    tags = b"".join([
        text("TIT2", "Embedded Title"), text("TCOM", composer), text("TBPM", bpm),
        text("TKEY", "Dorian"), text("TYER", "2034"),
        custom("DESCRIPTION", "RoomToneDescription"), custom("KEYWORDS", "FabricRipple"),
        custom("源标签 Source", token),
    ])
    tag_size = bytes((len(tags) >> shift) & 127 for shift in (21, 14, 7, 0))
    path.write_bytes(b"ID3\x03\x00\x00" + tag_size + tags + raw.read_bytes())
    raw.unlink()


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
    db = root / "profile/Local/HarkenSearch/Database/Mp3Qa/main.db"
    settings = root / "profile/Local/HarkenSearch/settings.ini"
    old = root / "fixtures/old/OldRoom.mp3"
    new = root / "fixtures/new/NewRoom.mp3"
    make_mp3(old, "ElderScore", "Zephyrtoken", "181")
    make_mp3(new, "CinderScore", "Opaltoken", "187")
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
        cmd("search", query=query)
        time.sleep(.65)
        wait("search ready", lambda: cmd("wait_ready"), lambda row: row["ready"])
        result = wait(query, lambda: cmd("get_results", limit=20),
                      lambda row: sorted(item["name"] for item in row["items"]) == sorted(expected), 12)
        report["cases"].append({"query": query, "names": expected})
        if screenshot:
            capture(driver, output, screenshot)
        return result

    def scan(folder):
        result = cmd("scan_folder", path=str(folder), recursive=False, update_existing=True,
                     remove_missing=False, cache_waveforms=False, build_similarity_index=False,
                     worker_threads=2)
        report["cases"].append({"scan_start": result})
        time.sleep(.5)
        result = wait("scan completed", lambda: cmd("scan_status"), lambda row: not row["scanning"], 60)
        assert result["errors"] == 0, result
        report["cases"].append({"scan_end": result})
        cmd("scan_acknowledge_complete")

    def record_for(filename):
        with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as connection:
            connection.row_factory = sqlite3.Row
            row = connection.execute("SELECT title, description, keywords, composer, bpm, musical_key, "
                                     "track_year, id3_text_tags FROM audio_files WHERE file_name = ?",
                                     (filename,)).fetchone()
            return dict(row) if row else None

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
        if args.phase == "baseline":
            scan(old.parent)
            search("OldRoom", [old.name])
            search("Zephyrtoken", [], "01-before-no-custom-hit")
        elif args.phase == "candidate":
            search("Zephyrtoken", [])
            search("OldRoom", [old.name], "02a-before-reread")
            assert old.is_file()
            report["old_record_before_reread"] = record_for(old.name)
            cmd("rescan_metadata", row=0)
            capture(driver, output, "02b-after-reread-action")
            updated = wait("reread old MP3", lambda: record_for(old.name),
                           lambda row: row is not None and row["composer"] == "ElderScore")
            assert updated["title"] == "Library override", updated
            assert updated["description"] == "Manual description", updated
            assert json.loads(updated["id3_text_tags"])["源标签 Source"] == "Zephyrtoken"
            report["old_record_after_reread"] = updated
            search("Zephyrtoken", [old.name], "02-old-library-reread")
            scan(new.parent)
            for query in ("CinderScore", "187", "Opaltoken"):
                search(query, [new.name], "03-new-custom-hit" if query == "Opaltoken" else None)
            for query in ("Dorian", "2034", "FabricRipple"):
                search(query, [old.name, new.name])
            search("RoomToneDescription", [old.name, new.name])
            added = record_for(new.name)
            assert added["bpm"] in ("187", 187) and added["musical_key"] == "Dorian", added
            assert json.loads(added["id3_text_tags"])["源标签 Source"] == "Opaltoken"
            report["new_record"] = added
            search("Opaltoken", [new.name], "04-result")
        else:
            search("Opaltoken", [new.name])
            cmd("folder_search_pointer", phase="hover", x=args.pointer[0], y=args.pointer[1])
            time.sleep(1.25)
            capture(driver, output, "05-tooltip")
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
        (output / "receipt.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        assert before == after, (before, after)
    if args.phase == "baseline":
        with sqlite3.connect(db) as connection:
            changed = connection.execute("UPDATE audio_files SET title=?, description=? WHERE file_name=?",
                                         ("Library override", "Manual description", old.name)).rowcount
            assert changed == 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for argument in ("exe", "root", "output"):
        parser.add_argument("--" + argument, type=Path, required=True)
    parser.add_argument("--phase", choices=("baseline", "candidate", "tooltip"), required=True)
    parser.add_argument("--pointer", type=int, nargs=2)
    run(parser.parse_args())
