"""Verify the real Similar Sounds button processes only all-failing pending files.

Uses an existing isolated library and a pinned package receipt. Original media
is read-only; the app runs on a private desktop without switching user input.
"""
from pathlib import Path
from datetime import datetime, timezone
import argparse
import hashlib
import json
import sqlite3
import time
import traceback

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver
from qa_shared_buttons import audit
import win32con
import win32event
import win32gui
import win32process


def snapshot(database):
    with sqlite3.connect(database.as_uri()+"?mode=ro", uri=True, timeout=10) as connection:
        connection.execute("PRAGMA query_only=ON")
        connection.execute("BEGIN")
        rows = {}
        for row in connection.execute("SELECT * FROM harken_similarity_v2 ORDER BY file_id"):
            digest = hashlib.sha256()
            for value in row:
                value = value if isinstance(value, bytes) else str(value).encode("utf-8")
                digest.update(len(value).to_bytes(8, "little"))
                digest.update(value)
            rows[str(row[0])] = digest.hexdigest()
        segments = hashlib.sha256()
        for row in connection.execute("SELECT * FROM harken_similarity_segments_v2 ORDER BY file_id,ordinal"):
            for value in row:
                value = value if isinstance(value, bytes) else str(value).encode("utf-8")
                segments.update(len(value).to_bytes(8, "little"))
                segments.update(value)
        return {"files": connection.execute("SELECT COUNT(*) FROM audio_files").fetchone()[0],
                "rows": rows, "segments_sha256": segments.hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-receipt", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-pending", type=int, required=True)
    parser.add_argument("--expected-failures", type=int, default=0)
    args = parser.parse_args()
    if args.expected_pending <= 0 or args.expected_failures != args.expected_pending:
        parser.error("This repeatability check requires an all-failing pending fixture: expected-pending > 0 and expected-failures == expected-pending")
    package = json.loads(args.package_receipt.read_text(encoding="utf-8"))
    exe, root, database = (Path(package[key]).resolve() for key in ("exe", "root", "database"))
    assert database.is_relative_to(root) and database.is_file()
    assert hashlib.sha256(exe.read_bytes()).hexdigest() == package["sha256"]
    import win32api
    for existing_pid in win32process.EnumProcesses():
        try:
            handle = win32api.OpenProcess(win32con.PROCESS_QUERY_INFORMATION|win32con.PROCESS_VM_READ, False, existing_pid)
        except Exception:
            continue
        try:
            existing_exe = Path(win32process.GetModuleFileNameEx(handle, 0)).resolve()
        except Exception:
            continue
        finally:
            handle.Close()
        assert existing_exe != exe, "The isolated QA package is already running; leave its command channel untouched"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    driver = Driver(root/"profile/runtime", output)
    report = {"package": package, "started_at": datetime.now(timezone.utc).isoformat(),
              "passed": False, "attempts": [], "checks": []}
    desktop = process = None
    verified = False

    def save():
        report["updated_at"] = datetime.now(timezone.utc).isoformat()
        (output/"report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    def command(**request):
        return driver.command(request, timeout=60)

    def state():
        return command(cmd="similar_index", action="state")

    def click(key):
        audit(driver, command(cmd="get_state")["theme"])
        for phase in ("hover", "down", "up"):
            bounds = state()["controls"][key]
            command(cmd="similar_index", action="pointer", phase=phase,
                    x=(bounds[0]+bounds[2])/2, y=(bounds[1]+bounds[3])/2)
            time.sleep(.15)

    try:
        before = snapshot(database)
        assert before["rows"] and before["files"] > args.expected_pending
        report["before"] = before
        desktop = PrivateDesktop()
        process, pid = desktop.start(exe)
        report["started_pid"] = pid
        identity = command(cmd="magic_eq_identity")
        report["identity"] = identity
        assert identity["pid"] == pid and Path(identity["exe_path"]).resolve() == exe
        assert Path(identity["isolation_root"]).resolve() == root
        verified = True
        save()
        ready = wait_state(driver, lambda s:s["main_ui_ready"] and not s["db_switch_in_progress"], "ready", 60)
        win32gui.SetWindowPos(identity["hwnd"], 0, 40, 40, 1480, 940, win32con.SWP_NOACTIVATE|win32con.SWP_NOZORDER)
        if not ready["db_path"]:
            command(cmd="info_edit_input", action="open_existing_db", text=str(database))
        elif Path(ready["db_path"]).resolve() != database:
            switched = command(cmd="switch_db", path=str(database))
            assert switched.get("started"), switched
        wait_state(driver, lambda s:bool(s["db_path"]) and Path(s["db_path"]).resolve()==database and not s["db_switch_in_progress"], "isolated-library", 60)
        command(cmd="similar_index", action="open", floating=True, width=900, height=620, language=0)
        command(cmd="set_theme", theme=2)
        time.sleep(.5)
        initial = state()
        assert initial["total"]-min(initial["indexed"],initial["semantic_indexed"]) == args.expected_pending
        report["initial"] = initial
        capture(driver, output/"before-button.png")
        save()
        for attempt in (1, 2):
            started = time.monotonic()
            generation_before = state()["batch_start_generation"]
            click("similar.index.start")
            deadline = time.monotonic()+300
            observed = []
            while time.monotonic() < deadline:
                current = state()
                observed.append({key:current[key] for key in ("batch_start_generation","running","batch_total","progress","reused","analyzed")})
                if (current["batch_start_generation"] > generation_before
                        and not current["running"] and current["batch_total"] > 0):
                    break
                time.sleep(.1)
            else:
                raise TimeoutError("pending analysis did not finish")
            summary = current["batch_summary"]
            assert current["batch_total"] == args.expected_pending and current["progress"] == args.expected_pending, current
            assert current["reused"] == 0 and summary["failed"] == args.expected_failures, current
            assert not summary["cancelled"] and not summary["error"] and not summary["model_error"], current
            after = snapshot(database)
            assert all(after["rows"].get(key)==value for key,value in before["rows"].items()), "completed analysis was changed"
            assert after["segments_sha256"] == before["segments_sha256"], "completed segments were changed"
            report["attempts"].append({"number":attempt, "seconds":time.monotonic()-started,
                                       "start_generation_before":generation_before, "final":current, "observed":observed})
            report["after"] = after
            capture(driver, output/("after-attempt-"+str(attempt)+".png"))
            save()
        # Even a permanent pending failure must leave explicit source-content
        # validation available. This click is separate from supplementing.
        generation_before = state()["batch_start_generation"]
        report["explicit_recheck_start_generation_before"] = generation_before
        click("similar.index.recheck")
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline:
            rechecked = state()
            if (rechecked["batch_start_generation"] > generation_before
                    and not rechecked["running"] and rechecked["batch_total"] == before["files"]):
                break
            time.sleep(.1)
        else:
            raise TimeoutError("explicit full recheck did not finish")
        assert rechecked["reused"] == len(before["rows"]) and rechecked["analyzed"] == 0, rechecked
        assert rechecked["batch_summary"]["failed"] == args.expected_failures, rechecked
        assert snapshot(database) == before, "Explicit validation changed unchanged analysis or segments"
        report["explicit_recheck"] = rechecked
        capture(driver, output/"after-explicit-recheck.png")
        click("similar.index.report")
        time.sleep(.3)
        capture(driver, output/"analysis-report.png")
        report["checks"] = ["real-button-selected-only-pending-files-twice", "completed-records-and-segments-unchanged", "source-failures-reported-without-false-success", "explicit-full-validation-still-available-with-pending-failure"]
        report["passed"] = True
    except BaseException as error:
        report["error"] = repr(error)
        report["traceback"] = traceback.format_exc()
        raise
    finally:
        if process is not None:
            if win32event.WaitForSingleObject(process,0) == win32con.WAIT_TIMEOUT:
                if verified:
                    try:
                        command(cmd="similar_index", action="cancel")
                    except Exception:
                        pass
                    try:
                        command(cmd="info_edit_input", action="exit")
                    except Exception:
                        win32gui.PostMessage(report["identity"]["hwnd"],win32con.WM_CLOSE,0,0)
                else:
                    def close_own(hwnd, _):
                        if win32process.GetWindowThreadProcessId(hwnd)[1] == pid:
                            win32gui.PostMessage(hwnd,win32con.WM_CLOSE,0,0)
                    win32gui.EnumWindows(close_own,None)
            if win32event.WaitForSingleObject(process,30000) == win32con.WAIT_OBJECT_0:
                report["app_exit_code"] = win32process.GetExitCodeProcess(process)
                if report["app_exit_code"] != 0:
                    report["passed"] = False
                process.Close()
            else:
                report["passed"] = False
                report["exit_error"] = "owned QA app did not exit"
        if desktop:
            desktop.close()
        save()
    print(json.dumps({"passed":report["passed"],"attempts":len(report["attempts"]),"report":str(output/"report.json")}))
    return 0 if report["passed"] and report.get("app_exit_code") == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
