"""Real-run evidence for BUG-20261123-ANALYSIS-REPORT-RESIZE.

Opens the Similar Sounds "analysis report" modal in a compile-time pinned QA
build on a private Windows desktop, drags its bottom-right resize grip through
ImGui's own mouse queue, closes and reopens it, pushes it past the window edge
and finally restarts the app to prove the size was remembered.

Requires pywin32 and Pillow. No input and no window switch reaches the user's
desktop, and every fixture lives inside the isolation root.

Exit 0: every geometry assertion held. Exit 1: an assertion failed.
Exit 2: fixture, app or capture error.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sqlite3
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver

DEFAULTS = (1180.0, 780.0)
MARGIN = 8.0


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_settings(path: Path) -> dict:
    values = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def prepare_profile(root: Path, exe: Path) -> tuple[Path, Path]:
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    library = root / "fixture-library/main.db"
    library.parent.mkdir(parents=True, exist_ok=True)
    if not library.exists():
        with sqlite3.connect(library) as connection:
            connection.execute("PRAGMA user_version=1")
    settings = runtime / "settings_dev.ini"
    if not settings.exists():
        settings.write_text(
            "currentDbPath=" + library.as_posix() + "\nlanguage=0\n",
            encoding="utf-8")
    consent = settings.with_name(settings.name + ".legal-consent")
    if not consent.exists():
        consent.write_text("eulaVersion=1\nprivacyVersion=1\n"
                           "acceptedAtUtc=QA-FIXTURE-NOT-USER-CONSENT\n"
                           "appVersion=BUG-20261123-ANALYSIS-REPORT-RESIZE\n",
                           encoding="utf-8")
    (exe.parent / "dev_mode.txt").write_text(
        "BUG-20261123-ANALYSIS-REPORT-RESIZE isolated QA\n", encoding="utf-8")
    return runtime, settings


def run(args) -> int:
    import win32con
    import win32event
    import win32process

    exe = args.exe.resolve()
    root = args.isolation_root.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    runtime, settings_path = prepare_profile(root, exe)

    desktop = PrivateDesktop()
    process = None
    driver = Driver(runtime, out)
    checks: list[dict] = []
    failures: list[str] = []
    report = {
        "card": "BUG-20261123-ANALYSIS-REPORT-RESIZE",
        "exe": str(exe),
        "exe_sha256": sha256(exe),
        "isolation_root": str(root),
        "started_at": datetime.now(timezone.utc).isoformat(),
        "checks": checks,
    }

    def check(name: str, condition: bool, detail) -> None:
        checks.append({"case": name, "passed": bool(condition), "detail": detail})
        if not condition:
            failures.append(name)

    def geometry() -> dict:
        return driver.command({"cmd": "alert_dialog_preview", "action": "geometry"})

    def steady_geometry(previous_rect=None) -> dict:
        """Wait until the rect stops moving, so a drag or clamp has settled."""
        deadline = time.monotonic() + 12
        last = geometry()
        stable = 0
        while time.monotonic() < deadline:
            time.sleep(0.12)
            current = geometry()
            if current["drag_active"]:
                last = current
                continue
            same = last["rect"] == current["rect"]
            settled = previous_rect is None or current["rect"] != previous_rect
            if same and settled:
                return current
            stable = stable + 1 if same else 0
            last = current
            if stable >= 3:
                return current
        return last

    def start() -> dict:
        nonlocal process
        process, pid = desktop.start(exe, ())
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        assert identity["pid"] == pid, identity
        assert Path(identity["exe_path"]).resolve() == exe, identity
        assert Path(identity["isolation_root"]).resolve() == root, identity
        report["identity"] = identity
        wait_state(driver, lambda s: s["main_ui_ready"], "Startup must finish", timeout=90)
        return identity

    def stop() -> None:
        nonlocal process
        if process is None:
            return
        if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
            state = driver.command({"cmd": "get_state"})
            if state["main_ui_ready"]:
                driver.command({"cmd": "info_edit_input", "action": "exit"})
            else:
                import win32gui
                win32gui.PostMessage(report["identity"]["hwnd"], win32con.WM_CLOSE, 0, 0)
        assert win32event.WaitForSingleObject(process, 20000) == win32con.WAIT_OBJECT_0, \
            "QA process did not exit"
        assert win32process.GetExitCodeProcess(process) == 0, "QA process failed during exit"
        process.Close()
        process = None

    def open_report() -> None:
        driver.command({"cmd": "alert_dialog_preview", "action": "open",
                        "dialog": "similar-analysis-report"})
        time.sleep(0.4)

    try:
        start()
        open_report()
        opened = steady_geometry()
        capture(driver, out / "01-report-default-1180x780.png")
        check("default-size", abs(opened["rect"][2] - DEFAULTS[0]) <= 2 and
              abs(opened["rect"][3] - DEFAULTS[1]) <= 2, opened)
        check("dialog-resizable", opened["resizable"] and not opened["no_resize"] and
              not opened["always_auto_resize"], opened)
        check("dialog-in-main-viewport", not opened["viewport_owned"], opened)
        check("forty-four-failures", opened["failure_rows"] == 44, opened)
        check("failure-list-not-a-strip", opened["failure_list_height"] >= 600.0, opened)
        # One text line, not the old reason line plus path line (~2x this).
        check("failure-row-single-line", 8.0 < opened["failure_row_height"] < 36.0, opened)
        check("failure-rows-visible-without-scrolling",
              int(opened["failure_list_height"] // opened["failure_row_height"]) >= 20, opened)

        # Real edge drag on the bottom-right grip, through ImGui's mouse queue.
        before = opened["rect"]
        driver.command({"cmd": "alert_dialog_preview", "action": "resize_drag",
                        "dx": -220, "dy": -140, "steps": 10})
        dragged = steady_geometry(previous_rect=before)
        capture(driver, out / "02-report-edge-drag-resized.png")
        check("edge-drag-shrank-width", dragged["rect"][2] <= before[2] - 60, dragged)
        check("edge-drag-shrank-height", dragged["rect"][3] <= before[3] - 40, dragged)
        resized = dragged["rect"][2:4]

        # Close and reopen: the size must survive the reopen.
        driver.command({"cmd": "alert_dialog_preview", "action": "close"})
        time.sleep(0.3)
        open_report()
        reopened = steady_geometry()
        capture(driver, out / "03-report-reopened-same-size.png")
        check("size-survives-reopen",
              abs(reopened["rect"][2] - resized[0]) <= 2 and
              abs(reopened["rect"][3] - resized[1]) <= 2, reopened)

        # Push the dialog past the bottom edge, then reopen: it must come back
        # fully visible with the 8 px margin.
        driver.command({"cmd": "alert_dialog_preview", "action": "drag", "dx": 0, "dy": 560})
        time.sleep(0.4)
        pushed = geometry()
        capture(driver, out / "04-report-pushed-past-bottom-edge.png")
        work = pushed["work"]
        check("drag-reaches-past-edge",
              pushed["rect"][1] + pushed["rect"][3] > work[1] + work[3], pushed)
        driver.command({"cmd": "alert_dialog_preview", "action": "close"})
        time.sleep(0.3)
        open_report()
        clamped = steady_geometry()
        capture(driver, out / "05-report-reopened-clamped.png")
        bottom = clamped["rect"][1] + clamped["rect"][3]
        right = clamped["rect"][0] + clamped["rect"][2]
        check("reopen-clamps-bottom", bottom <= work[1] + work[3] - MARGIN + 1.0, clamped)
        check("reopen-clamps-right", right <= work[0] + work[2] - MARGIN + 1.0, clamped)
        check("reopen-keeps-size",
              abs(clamped["rect"][2] - resized[0]) <= 2 and
              abs(clamped["rect"][3] - resized[1]) <= 2, clamped)

        # The full path stays reachable on hover: park the synthetic pointer over
        # a failure row and capture the tooltip. The exact row band is not known
        # up front, so try a few heights inside the list.
        hover_receipt = {}
        hover_landed = False
        # Offsets step by non-multiples of the row pitch so the sweep cannot
        # sample the same gap between rows every time.
        for offset in (0, 7, 13, 3, 17, 10, 21):
            y = (clamped["rect"][1] + 46.0 +
                 clamped["failure_list_height"] * 0.5 + offset)
            driver.command({"cmd": "alert_dialog_preview", "action": "pointer",
                            "x": clamped["rect"][0] + 380.0, "y": y})
            time.sleep(0.45)
            hover_receipt = geometry()
            if hover_receipt.get("tooltip_visible"):
                hover_landed = True
                break
        capture(driver, out / "07-report-failure-row-tooltip.png")
        driver.command({"cmd": "alert_dialog_preview", "action": "pointer_clear"})
        time.sleep(0.2)
        check("failure-path-tooltip-visible", hover_landed, hover_receipt)

        # The size must be written into the settings file, not only in memory.
        driver.command({"cmd": "alert_dialog_preview", "action": "close"})
        time.sleep(2.4)
        settings = read_settings(settings_path)
        stored_width = float(settings.get("similarAnalysisReportWidth", "0") or 0)
        stored_height = float(settings.get("similarAnalysisReportHeight", "0") or 0)
        report["settings"] = {key: settings.get(key) for key in (
            "similarAnalysisReportWidth", "similarAnalysisReportHeight")}
        check("settings-write-back",
              abs(stored_width - resized[0]) <= 2 and abs(stored_height - resized[1]) <= 2,
              report["settings"])
        stop()

        # Restart on the same profile: the remembered size must come back.
        start()
        open_report()
        after_restart = steady_geometry()
        capture(driver, out / "06-report-size-after-restart.png")
        check("size-survives-restart",
              abs(after_restart["rect"][2] - resized[0]) <= 2 and
              abs(after_restart["rect"][3] - resized[1]) <= 2, after_restart)
        check("restart-dialog-inside-work-area",
              after_restart["rect"][1] + after_restart["rect"][3] <=
              after_restart["work"][1] + after_restart["work"][3] - MARGIN + 1.0,
              after_restart)
        driver.command({"cmd": "alert_dialog_preview", "action": "close"})
        time.sleep(0.3)
        stop()
    except Exception as error:  # noqa: BLE001 - the receipt must survive a failure
        report["error"] = repr(error)
        try:
            stop()
        except Exception as stop_error:  # noqa: BLE001
            report["stop_error"] = repr(stop_error)
        report["finished_at"] = datetime.now(timezone.utc).isoformat()
        report["failures"] = failures or ["exception"]
        (out / "evidence.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        raise

    report["finished_at"] = datetime.now(timezone.utc).isoformat()
    report["failures"] = failures
    (out / "evidence.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    desktop.close()
    if failures:
        print("FAILED: " + ", ".join(failures))
        return 1
    print("analysis report resize: all checks passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True,
                        help="HarkenSearch.exe built with ARIS_PREVIEW_ISOLATION=ON")
    parser.add_argument("--isolation-root", type=Path, required=True,
                        help="ARIS_PREVIEW_ISOLATION_ROOT this EXE was built with")
    parser.add_argument("--output", type=Path, required=True)
    try:
        return run(parser.parse_args())
    except AssertionError as error:
        print(f"FAILED: {error}")
        return 1
    except Exception as error:  # noqa: BLE001
        print(f"ERROR: {error!r}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
