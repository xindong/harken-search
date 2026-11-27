"""Drive the isolated Harken preview to exercise batch metadata rule editing.

Usage:
  python tools/qa_batch_metadata_rule_edit.py --exe <HarkenSearch.exe> \
      --root <isolation root> --out <evidence dir>

The helper creates its own library inside the pinned isolation root, seeds
titles/descriptions in that isolated database, then opens the real batch
metadata window on a private desktop. It never touches the user's desktop,
library or installed app.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import sqlite3
import sys
import time
import traceback
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver

FIXTURES = {
    "Step_Wood_01.wav": ("Step_Wood_01", "footstep on wood"),
    "Wind_03.wav": ("Wind_03", "Step_Wood_01"),
    "Step_Wood_04.wav": ("Step_Wood_04", ""),
}


def write_json(path: Path, data) -> None:
    temp = path.with_suffix(".tmp")
    temp.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temp, path)


def make_fixtures(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    for index, name in enumerate(FIXTURES, start=1):
        target = folder / name
        if target.exists():
            continue
        samples = (int(3000 * math.sin(2 * math.pi * (200 + 37 * index) * n / 22050))
                   for n in range(6615))
        with wave.open(str(target), "wb") as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(22050)
            audio.writeframes(b"".join(sample.to_bytes(2, "little", signed=True)
                                       for sample in samples))


def write_settings(root: Path) -> Path:
    settings_dir = root / "profile/Local/HarkenSearch"
    settings_dir.mkdir(parents=True, exist_ok=True)
    path = settings_dir / "settings.ini"
    existing = []
    if path.exists():
        existing = [line for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    path.write_text("\n".join(existing) + ("\n" if existing else ""), encoding="utf-8")
    return path


def main(args: argparse.Namespace) -> int:
    import win32con
    import win32event
    import win32process

    exe = args.exe.resolve()
    root = args.root.resolve()
    out = args.out.resolve()
    if not exe.is_file() or out.exists():
        raise ValueError("Expected an existing EXE and a fresh output directory")
    out.mkdir(parents=True)
    root.mkdir(parents=True, exist_ok=True)
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    (root / "temp").mkdir(parents=True, exist_ok=True)
    fixtures = root / "fixtures/Rule Edit QA"
    make_fixtures(fixtures)
    settings_path = write_settings(root)
    sentinel = root / "profile/Local/HarkenSearch/aris_sentinel.lock"
    if sentinel.exists():
        sentinel.unlink()

    evidence = out
    driver = Driver(runtime, evidence)
    request_path = evidence / "request.json"
    reply_path = evidence / "reply.json"
    report_path = evidence / "desktop.json"
    report = {
        "started_at": datetime.now(timezone.utc).isoformat(),
        "exe": str(exe),
        "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "isolation_root": str(root),
        "settings_path": str(settings_path),
        "fixtures": {name: hashlib.sha256((fixtures / name).read_bytes()).hexdigest()
                     for name in FIXTURES},
        "actions": [],
    }

    def rows(db_path: Path):
        with sqlite3.connect(db_path.as_uri() + "?mode=ro", uri=True) as in_db:
            in_db.row_factory = sqlite3.Row
            return [dict(row) for row in in_db.execute(
                "SELECT id, file_path, file_name, title, description, keywords, category "
                "FROM audio_files ORDER BY file_name")]

    def launch(environment_extra=None):
        desktop = PrivateDesktop()
        environment = {"AURA_RUNTIME_ROOT": str(runtime), "AURA_HARNESS_LANE_KIND": "folder-search"}
        environment.update(environment_extra or {})
        process, pid = desktop.start(exe, environment_overrides=environment)
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=65)
        assert identity["pid"] == pid, identity
        assert Path(identity["exe_path"]).resolve() == exe, identity
        assert Path(identity["isolation_root"]).resolve() == root, identity
        report["identity"] = identity
        state = wait_state(driver, lambda s: s["main_ui_ready"], "Native app ready", timeout=60)
        return desktop, process, state

    def shutdown(desktop, process):
        try:
            driver.command({"cmd": "info_edit_input", "action": "exit"}, timeout=6)
        except Exception as error:  # noqa: BLE001
            report.setdefault("exit_notes", []).append(str(error))
        win32event.WaitForSingleObject(process, 12000)
        if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
            win32process.TerminateProcess(process, 1)
            win32event.WaitForSingleObject(process, 8000)
            report.setdefault("exit_notes", []).append("Terminated preview process started by this script")
        report.setdefault("exit_codes", []).append(win32process.GetExitCodeProcess(process))
        process.Close()
        desktop.close()

    def seed_database(db_path: Path):
        with sqlite3.connect(db_path) as db:
            for name, (title, description) in FIXTURES.items():
                db.execute("UPDATE audio_files SET title = ?, description = ? WHERE file_name = ?",
                           (title, description, name))
            db.commit()

    def perform(request):
        action = request["op"]
        if action == "frame":
            path = evidence / (request["name"] + ".png")
            assert path.parent == evidence and not path.exists(), path
            capture(driver, path)
            return {"image": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        if action == "command":
            return driver.command(request["data"])
        if action == "click":
            for phase in ("hover", "down", "up"):
                result = driver.command({"cmd": "folder_search_pointer", "phase": phase,
                                         "x": request["x"], "y": request["y"],
                                         "button": request.get("button", "left")})
                assert result["ok"], result
                time.sleep(0.23)
            return {"point": [request["x"], request["y"]], "button": request.get("button", "left")}
        if action == "text":
            driver.command({"cmd": "info_edit_input", "text": request["text"]})
            return {"typed": request["text"]}
        if action == "rows":
            return rows(Path(report["database"]))
        if action == "exit":
            return {"exiting": True}
        raise ValueError(action)

    desktop = process = None
    try:
        desktop, process, state = launch()
        if not state["db_path"]:
            if not state["new_db_dialog_open"]:
                driver.command({"cmd": "info_edit_input", "action": "new_db_dialog"})
            wait_state(driver, lambda s: s["new_db_dialog_open"], "New test database")
            driver.command({"cmd": "info_edit_input", "action": "new_db_name", "text": "Batch Rule QA"})
            driver.command({"cmd": "info_edit_input", "action": "new_db_create"})
            state = wait_state(driver, lambda s: s["db_path"] and
                               not s["new_db_dialog_open"] and not s["db_switch_in_progress"],
                               "Isolated library created")
        database = Path(state["db_path"]).resolve()
        assert database.is_relative_to(root), state["db_path"]
        if database.is_dir():
            database = database / "main.db"
        report["database"] = str(database)
        if not rows(database):
            before = driver.command({"cmd": "scan_status"})["scan_started_at_ms"]
            driver.command({"cmd": "qa_rightclick_scan", "path": str(fixtures), "cache_waveforms": False})
            deadline = time.monotonic() + 120
            while True:
                scan = driver.command({"cmd": "scan_status"})
                if not scan["scanning"] and scan["scan_started_at_ms"] != before and len(rows(database)) == len(FIXTURES):
                    break
                assert time.monotonic() < deadline, scan
                time.sleep(0.2)
            report["scan"] = scan
            driver.command({"cmd": "open_scan_dialog"})
            for phase in ("hover", "down", "up"):
                time.sleep(0.18)
                driver.command({"cmd": "info_page_pointer", "key": "info.qaScanDone", "phase": phase})
        shutdown(desktop, process)
        desktop = process = None
        seed_database(Path(report["database"]))
        report["seeded_rows"] = rows(Path(report["database"]))

        desktop, process, _ = launch()
        report["relaunched_rows"] = rows(Path(report["database"]))
        print("READY", report["database"], flush=True)

        latest = None
        deadline = time.monotonic() + 25 * 60
        while time.monotonic() < deadline:
            if win32event.WaitForSingleObject(process, 0) != win32con.WAIT_TIMEOUT:
                raise RuntimeError("Private preview exited before tests ended")
            if not request_path.exists():
                time.sleep(0.15)
                continue
            try:
                request = json.loads(request_path.read_text(encoding="utf-8-sig"))
            except (OSError, ValueError):
                time.sleep(0.15)
                continue
            if request["key"] == latest:
                time.sleep(0.15)
                continue
            latest = request["key"]
            entry = {"key": latest, "request": request}
            try:
                entry["result"] = perform(request)
            except Exception as error:  # noqa: BLE001
                entry["error"] = str(error)
            report["actions"].append(entry)
            write_json(report_path, report)
            write_json(reply_path, entry)
            if request["op"] == "exit":
                break
        report["final_rows"] = rows(Path(report["database"]))
    except Exception:
        report["error"] = traceback.format_exc()
        raise
    finally:
        if process is not None and desktop is not None:
            try:
                remaining = win32event.WaitForSingleObject(process, 0)
                if remaining == win32con.WAIT_TIMEOUT:
                    shutdown(desktop, process)
                    desktop = process = None
            finally:
                if process is not None:
                    win32process.TerminateProcess(process, 1)
                    win32event.WaitForSingleObject(process, 8000)
                    process.Close()
                if desktop is not None:
                    desktop.close()
        report["finished_at"] = datetime.now(timezone.utc).isoformat()
        write_json(report_path, report)
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    main(parser.parse_args())

