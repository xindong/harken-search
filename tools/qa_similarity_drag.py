"""Exercise real Similar Sounds file gestures and native OLE entry on a private desktop.

Does not switch input desktops or drive the user's mouse. Native drags end without
a drop because no system mouse button is pressed. External DAW acceptance remains
a separate manual check; native drag paths and completion are checked from the
same app instance's diagnostic log, not a simulated export.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import time
import traceback

from PIL import Image
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_shared_buttons import audit
from qa_similarity_pending import snapshot
from qa_transport_themes import Driver


def main():
    import win32api, win32con, win32event, win32gui, win32process
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-receipt", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    receipt = json.loads(args.package_receipt.read_text(encoding="utf-8"))
    exe, root, database = (Path(receipt[k]).resolve() for k in ("exe", "root", "database"))
    assert database.is_file() and database.is_relative_to(root)
    assert hashlib.sha256(exe.read_bytes()).hexdigest() == receipt["sha256"]
    for existing_pid in win32process.EnumProcesses():
        try:
            handle = win32api.OpenProcess(win32con.PROCESS_QUERY_INFORMATION | win32con.PROCESS_VM_READ, False, existing_pid)
        except Exception:
            continue
        try:
            running = Path(win32process.GetModuleFileNameEx(handle, 0)).resolve()
        except Exception:
            continue
        finally:
            handle.Close()
        assert running != exe, "Isolated package already running; leave its command channel untouched"

    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    driver = Driver(root / "profile/runtime", out)
    settings = root / "profile/Local/HarkenSearch/settings.ini"
    settings.parent.mkdir(parents=True, exist_ok=True)
    lines = settings.read_text(encoding="utf-8-sig").splitlines() if settings.exists() else []
    old_volume = next((line for line in lines if line.startswith("volume=")), "volume=1.0")
    lines = [line for line in lines if not line.startswith(("volume=", "themeScheduleMode="))]
    settings.write_text("\n".join(lines + ["volume=0.0", "themeScheduleMode=0"]) + "\n", encoding="utf-8")
    report = {"passed": False, "package": receipt, "checks": [], "before": snapshot(database),
              "started_at": datetime.now(timezone.utc).isoformat(),
              "uncovered": ["Successful drop accepted by an external DAW or Explorer is not automated on the private desktop."]}
    desktop = process = None
    verified = False
    log = None
    log_offset = 0

    def save():
        (out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    def command(**request):
        return driver.command(request, timeout=30)

    def state():
        return command(cmd="similar_index", action="state")

    def until(probe, predicate, label, timeout=30):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = probe()
            if predicate(last):
                return last
            time.sleep(.12)
        raise AssertionError((label, last))

    def native_log():
        raw = log.read_bytes()[log_offset:] if log and log.exists() else b""
        return raw.decode("utf-16-le", errors="replace")

    def pointer(phase, point):
        audit(driver, command(cmd="get_state")["theme"])
        return command(cmd="similar_index", action="pointer", phase=phase, x=point[0], y=point[1])

    def point(key, offset=0):
        bounds = state()["controls"][key]
        assert bounds[2] - bounds[0] > 65, bounds
        return (bounds[0] + 20 + offset, (bounds[1] + bounds[3]) / 2)

    def click(key):
        for phase in ("hover", "down", "up"):
            pointer(phase, point(key))
            time.sleep(.12)

    def release_native_capture():
        # OLE waits for a native window message; ImGui's injected button-up is
        # not an OS input event. Post only to this verified process on our
        # private desktop, without touching the user's global cursor/buttons.
        import ctypes
        from ctypes import wintypes
        class GUIINFO(ctypes.Structure):
            _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD),
                        ("active", wintypes.HWND), ("focus", wintypes.HWND), ("capture", wintypes.HWND),
                        ("menu", wintypes.HWND), ("move", wintypes.HWND), ("caret", wintypes.HWND), ("rect", wintypes.RECT)]
        thread, actual_pid = win32process.GetWindowThreadProcessId(identity["hwnd"])
        assert actual_pid == identity["pid"]
        info = GUIINFO()
        info.cbSize = ctypes.sizeof(info)
        assert ctypes.windll.user32.GetGUIThreadInfo(thread, ctypes.byref(info))
        if not info.capture:
            return False
        assert win32process.GetWindowThreadProcessId(info.capture)[1] == actual_pid
        win32gui.PostMessage(info.capture, win32con.WM_MOUSEMOVE, 0, 0)
        win32gui.PostMessage(info.capture, win32con.WM_LBUTTONUP, 0, 0)
        return True

    try:
        desktop = PrivateDesktop()
        process, pid = desktop.start(exe)
        identity = command(cmd="magic_eq_identity")
        report["identity"] = identity
        assert identity["pid"] == pid and Path(identity["exe_path"]).resolve() == exe
        assert Path(identity["isolation_root"]).resolve() == root
        verified = True
        key = identity["runtime_root"].replace("/", "\\").lower().encode("utf-8")
        digest = 1469598103934665603
        for value in key:
            digest = ((digest ^ value) * 1099511628211) & ((1 << 64) - 1)
        log = root / "temp" / f"HarkenSearch_diag_{digest:016x}.log"
        log_offset = log.stat().st_size if log.exists() else 0
        report["native_log_path"] = str(log)
        wait_state(driver, lambda s: s["main_ui_ready"] and not s["db_switch_in_progress"], "Ready", 60)
        win32gui.SetWindowPos(identity["hwnd"], 0, 40, 40, 1480, 940, win32con.SWP_NOACTIVATE | win32con.SWP_NOZORDER)
        command(cmd="search", text="")
        wait_state(driver, lambda s: s["result_count"] == report["before"]["files"], "Existing library list", 60)
        command(cmd="select_row", row=0)
        command(cmd="similar_index", action="open", floating=True, width=640, height=640, language=0)
        command(cmd="set_theme", theme=2)
        command(cmd="similar_index", action="configure", mode=1, deep=True, preset=0)
        command(cmd="similar_index", action="find")
        result = until(state, lambda s: s["status"] in (3, 4), "Real query", 180)
        assert result["mode"] == 1 and result["error"] == 0 and result["rows"] > 1, result
        assert "similar.result.file.0" in result["controls"], "Result filename has no drag gesture entry"
        assert "similar.options.mode" not in result["controls"]
        baseline = command(cmd="get_state")
        reference = result["reference_id"]
        report["query"] = result
        # A click, a below-threshold movement, and waveform scrubbing must not
        # accidentally start native drag-out.
        click("similar.result.file.0")
        for phase, offset in (("hover", 0), ("down", 0), ("move", 4), ("up", 4)):
            pointer(phase, point("similar.result.file.0", offset))
            time.sleep(.12)
        for phase, offset in (("hover", 0), ("down", 0), ("move", 24), ("up", 24)):
            pointer(phase, point("similar.result.wave.0", offset))
            time.sleep(.12)
        assert "StartFileDrag: fileCount=" not in native_log(), native_log()
        report["checks"].append({"case": "click-jitter-and-waveform-seek-do-not-drag", "passed": True})

        # Row 1 has not been auditioned. Its own path must be transferred, even
        # while row 0 is loaded and the main list still selects the reference.
        dragged = result["matches"][1]
        original = Path(dragged["path"])
        source_sha = hashlib.sha256(original.read_bytes()).hexdigest()
        for attempt in (1, 2):
            for phase, offset in (("hover", 0), ("down", 0), ("move", 24)):
                pointer(phase, point("similar.result.file.1", offset))
                time.sleep(.12)
            report["native_release_posted"] = release_native_capture()
            recorded = until(native_log, lambda t: t.count("StartFileDrag: dropSucceeded=") == attempt,
                             "Native OLE drag returned without a drop", 10)
            pointer("up", point("similar.result.file.1", 24))
            paths = re.findall(r"\[\s*0\]\s+([^\r\n]+)", recorded)
            assert len(paths) == attempt and all(Path(path) == original for path in paths), (paths, original)
            assert recorded.count("StartFileDrag: fileCount=1") == attempt, recorded
            assert recorded.count("StartFileDrag: dropSucceeded=0") == attempt, recorded
            assert "DoDragDrop failed" not in recorded, recorded
            after = command(cmd="get_state")
            similar = state()
            assert after["selected_file_id"] == baseline["selected_file_id"]
            assert after["result_count"] == baseline["result_count"]
            assert similar["reference_id"] == reference and similar["rows"] == result["rows"]
            report["checks"].append({"case": "native-drag-cancel-and-repeat", "attempt": attempt,
                                      "path": str(original), "reference": reference, "passed": True})
            save()
        assert hashlib.sha256(original.read_bytes()).hexdigest() == source_sha
        report["dragged_file"] = {"id": dragged["id"], "path": str(original), "sha256_unchanged": source_sha}
        # Check that cancelling a native drag did not swallow later audition.
        click("similar.result.file.0")
        assert state()["reference_id"] == reference
        report["after"] = snapshot(database)
        assert report["before"] == report["after"]
        capture(driver, out / "results-after-drag.png")
        panel = command(cmd="dock_state")["panels"]["similar-sounds"]
        origin = win32gui.ClientToScreen(identity["hwnd"], (0, 0))
        x, y = round(panel["x"] - origin[0]), round(panel["y"] - origin[1])
        with Image.open(out / "results-after-drag.png") as full:
            full.crop((x, y, x + round(panel["w"]), y + round(panel["h"]))).save(out / "panel-results.png")
        report["checks"].append({"case": "cancel-keeps-list-usable-reference-and-analysis-unchanged", "passed": True})
        report["passed"] = True
    except Exception:
        report["error"] = traceback.format_exc()
        raise
    finally:
        save()
        try:
            if process is not None:
                if verified:
                    release_native_capture()
                    command(cmd="info_edit_input", action="exit")
                assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0, "Own QA process has not exited"
                report["exit_code"] = win32process.GetExitCodeProcess(process)
                process.Close()
            if settings.exists():
                lines = settings.read_text(encoding="utf-8-sig").splitlines()
                settings.write_text("\n".join(old_volume if line.startswith("volume=") else line for line in lines) + "\n", encoding="utf-8")
            assert report.get("exit_code", 0) == 0, f"Own QA process exited abnormally: {report['exit_code']}"
        except Exception:
            report["passed"] = False
            report["cleanup_error"] = traceback.format_exc()
            raise
        finally:
            if desktop is not None:
                desktop.close()
            (out / "native-drag.log").write_text(native_log(), encoding="utf-8")
            report["finished_at"] = datetime.now(timezone.utc).isoformat()
            save()
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]), "exit_code": report["exit_code"]}))


if __name__ == "__main__":
    main()
