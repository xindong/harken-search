"""Resize three real dock seams in a pinned Harken QA build on a private desktop.

Start with a fresh output directory and the 24-WAV Dock QA library copied
inside the run's fixed isolation root. Exit 0 = pass, 1 = observed regression,
2 = invalid fixture, QA tooling error, or unsafe cleanup.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sqlite3
import sys
import time
from contextlib import closing
from pathlib import Path

from qa_dock_grip_modal import shutdown_preview
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_top_menu_font import absolute_path, exe_has_root, reject_reparse, require_idle_root
from qa_transport_themes import Driver, window_api


TRANSFER, FILTER, RESULTS, INFO = COLUMNS = ("mod-transfer", "filter", "search-results", "info")
KEYS = ("x", "y", "w", "h")
TOLERANCE = 2
STEP = 40


def preflight(root, runtime, database):
    profile = root / "profile"
    settings = profile / "Local/HarkenSearch/settings.ini"
    for path in (runtime / "qa_cmd.json", runtime / "qa_result.json", settings,
                 database, database.with_suffix(".db-wal"), database.with_suffix(".db-shm")):
        reject_reparse(path)
    if (runtime / "qa_cmd.json").exists():
        raise RuntimeError("The fixed QA root has an outstanding command")
    settings_text = settings.read_text(encoding="utf-8-sig")
    current_paths = re.findall(r"(?m)^currentDbPath=(.+)$", settings_text)
    if len(current_paths) != 1 or Path(current_paths[0]).resolve(strict=True) != database:
        raise ValueError(f"Set currentDbPath to the new Dock QA fixture: {database}")
    for entry in re.findall(r"(?m)^recentDb\d+=(.+)$", settings_text):
        if not Path(entry).is_absolute() or not Path(entry).resolve().is_relative_to(profile):
            raise ValueError(f"Recent database escapes the new QA profile: {entry}")
    with closing(sqlite3.connect(f"{database.as_uri()}?mode=ro", uri=True)) as db:
        samples = [Path(row[0]) for row in db.execute("SELECT file_path FROM audio_files")]
    if len(samples) != 24:
        raise ValueError(f"Expected 24 Dock QA WAV records; found {len(samples)}")
    for sample in samples:
        if not sample.is_absolute() or not absolute_path(sample).is_relative_to(root):
            raise ValueError(f"Copy the WAV files here and repoint the QA database: {sample}")
        reject_reparse(sample)
        if sample.suffix.casefold() != ".wav" or not sample.is_file():
            raise ValueError(f"Missing Dock QA WAV: {sample}")


def edge(panel):
    return panel["x"] + panel["w"]


def grip_point(state, left, right):
    panels = state["panels"]
    first, second = panels[left], panels[right]
    overlap_top = max(first["y"], second["y"])
    overlap_bottom = min(first["y"] + first["h"], second["y"] + second["h"])
    return round((edge(first) + second["x"]) / 2), round((overlap_top + overlap_bottom) / 2)


def layout_errors(state):
    panels = state.get("panels", {})
    errors = []
    if set(panels) != set(COLUMNS) or any(panel["floating"] for panel in panels.values()):
        errors.append("expected exactly four docked panels")
    if state["dragging"] or state["owner"] or state["input"]["modal"]:
        errors.append("unexpected title drag or modal during resize")
    if not all(name in panels for name in COLUMNS):
        return errors
    if len({panels[name]["viewport"] for name in COLUMNS}) != 1:
        errors.append("panels are not in a single viewport")
    for left, right in zip(COLUMNS, COLUMNS[1:]):
        first, second = panels[left], panels[right]
        gap = second["x"] - edge(first)
        overlap = min(first["y"] + first["h"], second["y"] + second["h"]) - max(first["y"], second["y"])
        if not -TOLERANCE <= gap <= 16 or overlap < min(first["h"], second["h"]) - 12:
            errors.append(f"{left} and {right} are not side-by-side (gap={gap}, overlap={overlap})")
    return errors


def resize_errors(before, after, left, right, direction):
    start, end = before["panels"], after.get("panels", {})
    if any(name not in end for name in COLUMNS):
        return ["panels disappeared during resize"]
    delta = {name: end[name]["w"] - start[name]["w"] for name in COLUMNS}
    errors = []
    if direction and (direction * delta[left] < 4 or -direction * delta[right] < 4):
        errors.append(f"adjacent widths did not move in direction {direction}: {delta}")
    for name in COLUMNS:
        if name not in (left, right) or not direction:
            if any(abs(end[name][key] - start[name][key]) > TOLERANCE for key in KEYS):
                errors.append(f"{name} changed outside the requested adjustment")
        elif any(abs(end[name][key] - start[name][key]) > TOLERANCE for key in ("y", "h")):
            errors.append(f"{name} changed vertically")
    if direction and (abs(end[left]["x"] - start[left]["x"]) > TOLERANCE
                      or abs(edge(end[right]) - edge(start[right])) > TOLERANCE
                      or abs(delta[left] + delta[right]) > TOLERANCE):
        errors.append("the adjacent pair did not keep its outside edges and total width")
    return errors


class SplitterQa:
    def __init__(self, driver, output, bounds, scale, report):
        self.driver = driver
        self.output = output
        self.bounds = bounds
        self.scale = scale
        self.report = report

    def send(self, **request):
        return self.driver.command(request)

    def settled(self, predicate=lambda _: True):
        frame = self.send(cmd="dock_state")["frame"]
        deadline = time.monotonic() + 8
        while True:
            state = self.send(cmd="dock_state")
            if state["frame"] >= frame + 3 and predicate(state):
                return state
            if time.monotonic() >= deadline:
                raise RuntimeError(f"Dock state did not settle: {state}")

    def photo(self, name):
        capture(self.driver, self.output / name)
        return name

    def pointer(self, phase, point, title=None):
        if title is not None:
            result = self.send(cmd="dock_pointer", phase=phase, module=title, part="title")
            pos_x, pos_y = result["x"], result["y"]
        else:
            pos_x, pos_y = point
        if title is None and phase in ("hover", "down"):
            client_x, client_y, width, height = self.bounds
            local_x, local_y = pos_x - client_x, pos_y - client_y
            if not (0 <= local_x < width and 0 <= local_y < height):
                raise ValueError(f"Grip point is off the private client: {point}")
            result = self.send(cmd="folder_search_pointer", phase=phase, x=local_x, y=local_y)
            if any(abs(actual - expected) > 1 for actual, expected in
                   zip(result["viewport"], (client_x, client_y))):
                raise RuntimeError(f"QA viewport and client origins disagree: {result}")
        elif title is None:
            self.send(cmd="dock_pointer", phase=phase, x=pos_x, y=pos_y)
        if phase == "up":
            return self.settled(lambda state: not state["input"]["down"])
        return self.settled(lambda state: state["input"]["down"] == (phase != "hover")
                            and abs(state["input"]["x"] - pos_x) <= 1
                            and abs(state["input"]["y"] - pos_y) <= 1)

    def grab(self, point):
        self.send(cmd="dock_pointer", phase="focus")
        self.pointer("hover", point, title=FILTER)
        self.pointer("hover", point)
        return self.pointer("down", point)

    def move(self, point, delta):
        return self.pointer("move", (point[0] + delta, point[1]))

    def drag(self, point, delta):
        held = self.grab(point)
        moved = self.move(point, delta)
        after = self.pointer("up", (point[0] + delta, point[1]))
        return held, moved, after

    def fixture(self):
        fixture = self.report["fixture"]
        for module in ("ucs-browser", "tag-browser", "workflow", "transfer"):
            self.send(cmd="function_window", module=module, action="close")
        self.send(cmd="dock_setup")
        self.send(cmd="similar_index", action="close")
        initial = self.settled(lambda state: set(state.get("panels", {})) == {FILTER, RESULTS, INFO})
        fixture["before_dock"] = initial
        desired_info = round(200 * self.scale)
        shrink = round(initial["panels"][INFO]["w"] - desired_info)
        if abs(shrink) >= 8:
            _, _, initial = self.drag(grip_point(initial, RESULTS, INFO), shrink)
            fixture["after_info_preparation"] = initial

        self.send(cmd="function_window", module="transfer", action="open", floating=True)
        floating = self.settled(lambda state: state.get("panels", {}).get(TRANSFER, {}).get("floating"))
        anchor = floating["panels"][FILTER]
        self.send(cmd="dock_pointer", phase="focus")
        self.pointer("hover", None, title=TRANSFER)
        self.pointer("down", None, title=TRANSFER)
        fixture["preview_attempts"] = attempts = []
        for inset in (8, 12, 19, 26, 38):
            point = round(anchor["x"] + inset), round(anchor["y"] + anchor["h"] * .53)
            preview = self.pointer("move", point)
            attempts.append(dict(point=point, kind=preview["preview_kind"], target=preview["preview_target"],
                                 side=preview["preview_side"], too_small=preview["drop_too_small"],
                                 rect=preview["drawn_drop_rect"]))
            if (preview["dragging"] and preview["preview_target"] == FILTER
                    and preview["preview_side"] == 0 and preview["drawn_drop_rect"]["w"] > 0):
                fixture["preview"] = self.photo("00-transfer-dock-preview.png")
                docked = self.pointer("up", point)
                break
        else:
            fixture["rejected_drop_screenshot"] = self.photo("00-transfer-dock-rejected.png")
            self.send(cmd="dock_pointer", phase="escape")
            self.pointer("up", point)
            raise RuntimeError("No allowed Transfer drop to the left of Filter; see preview attempts")
        fixture["landed"] = docked
        if errors := layout_errors(docked):
            raise RuntimeError(f"Transfer did not land in the requested four columns: {errors}")

        floor = round(480 * self.scale)
        if docked["panels"][TRANSFER]["w"] > floor + TOLERANCE:
            adjustment = -round(docked["panels"][TRANSFER]["w"] - floor + STEP)
            held, moved, after = self.drag(grip_point(docked, TRANSFER, FILTER), adjustment)
            fixture["transfer_limit_drag"] = dict(held=held, moved=moved, after=after)
            docked = after
        fixture["ready"] = docked
        fixture["screenshot"] = self.photo("00-four-columns.png")
        panels = docked["panels"]
        center_floor = 520 if self.bounds[2] >= 1100 else 460
        if (layout_errors(docked) or abs(panels[TRANSFER]["w"] - floor) > TOLERANCE
                or panels[FILTER]["w"] < 160 * self.scale + STEP + 8
                or panels[RESULTS]["w"] < center_floor + STEP + 8):
            raise RuntimeError("Fixture needs Transfer at its minimum and Filter/Results width headroom")

    def record(self, name, pair, snapshots, images, errors, **extra):
        before, after = snapshots["before"], snapshots["after"]
        widths = {panel: round(after.get("panels", {}).get(panel, {}).get("w", 0)
                               - before["panels"][panel]["w"], 2) for panel in COLUMNS}
        point_delta = None
        if all(panel in after.get("panels", {}) for panel in pair):
            point_delta = grip_point(after, *pair)[0] - grip_point(before, *pair)[0]
        result = dict(name=name, pair=pair, snapshots=snapshots, screenshots=images,
                      width_delta_px=widths, seam_delta_px=point_delta, errors=errors,
                      passed=not errors, **extra)
        self.report["cases"].append(result)
        print(json.dumps({key: result[key] for key in
                          ("name", "passed", "width_delta_px", "seam_delta_px", "errors")}), flush=True)

    def resize(self, name, pair, delta):
        before = self.settled()
        images = {"before": self.photo(f"{name}-before.png")}
        point = grip_point(before, *pair)
        held = self.grab(point)
        moved = self.move(point, delta)
        if name == "01-filter-results-left":
            images["held_after_move"] = self.photo(f"{name}-held.png")
        after = self.pointer("up", (point[0] + delta, point[1]))
        images["after"] = self.photo(f"{name}-after.png")
        snapshots = dict(before=before, held=held, moved=moved, after=after)
        sign = 1 if delta > 0 else -1
        errors = [f"{stage}: {error}" for stage, state in snapshots.items() for error in layout_errors(state)]
        errors += [f"held: {error}" for error in resize_errors(before, held, *pair, 0)]
        for stage in ("moved", "after"):
            errors += [f"{stage}: {error}" for error in resize_errors(before, snapshots[stage], *pair, sign)]
        self.record(name, pair, snapshots, images, errors, pointer_dx_px=delta, start=point)

    def clamp_and_reverse(self):
        name = "07-filter-results-limit-and-reverse"
        pair = FILTER, RESULTS
        before = self.settled()
        images = {"before": self.photo(f"{name}-before.png")}
        point = grip_point(before, *pair)
        far_dx = self.bounds[0] + 12 - point[0]
        limit_dx = far_dx + 2 * STEP
        held = self.grab(point)
        limit = self.move(point, limit_dx)
        images["limit"] = self.photo(f"{name}-limit.png")
        overshot = self.move(point, far_dx)
        reversed_state = self.move(point, far_dx + STEP)
        after = self.pointer("up", (point[0] + far_dx + STEP, point[1]))
        images["after"] = self.photo(f"{name}-after.png")
        snapshots = dict(before=before, held=held, limit=limit, overshot=overshot,
                         reversed=reversed_state, after=after)
        errors = [f"{stage}: {error}" for stage, state in snapshots.items() for error in layout_errors(state)]
        for stage, start, end, direction in (("held", before, held, 0),
                ("limit", before, limit, -1), ("overshoot", limit, overshot, 0),
                ("reversed while held", overshot, reversed_state, 1), ("released", overshot, after, 1)):
            errors += [f"{stage}: {error}" for error in resize_errors(start, end, *pair, direction)]
        if FILTER in limit.get("panels", {}) and abs(limit["panels"][FILTER]["w"] - 160 * self.scale) > TOLERANCE:
            errors.append("left movement did not reach the Filter resize floor")
        self.record(name, pair, snapshots, images, errors, start=point,
                    pointer_steps_px=[limit_dx, far_dx, far_dx + STEP])

    def run(self):
        self.fixture()
        for name, pair, delta in (
            ("01-filter-results-left", (FILTER, RESULTS), -STEP),
            ("02-filter-results-right", (FILTER, RESULTS), STEP),
            ("03-transfer-filter-right", (TRANSFER, FILTER), STEP),
            ("04-transfer-filter-left", (TRANSFER, FILTER), -STEP),
            ("05-results-info-left", (RESULTS, INFO), -STEP),
            ("06-results-info-right", (RESULTS, INFO), STEP),
        ):
            self.resize(name, pair, delta)
        self.clamp_and_reverse()


def run(args):
    import win32con
    import win32event
    import win32gui
    import win32process

    root = absolute_path(args.isolation_root)
    output = absolute_path(args.output)
    for path in (root, output, absolute_path(args.exe)):
        reject_reparse(path)
    if not root.is_dir():
        raise ValueError(f"Use an existing, dedicated QA root: {root}")
    if output == root or not output.is_relative_to(root) or output.is_relative_to(root / "profile"):
        raise ValueError("Output must be a new evidence folder inside the run root and outside profile")
    exe = args.exe.resolve(strict=True)
    executable = exe.read_bytes()
    if (exe.name.casefold() != "harkensearch.exe" or not executable.startswith(b"MZ")
            or not exe_has_root(executable, root)
            or not all(token in executable for token in (b"dock_pointer\0", b"dock_state\0",
                       b"folder_search_pointer\0", b"capture_preview_frame\0", b"magic_eq_identity\0"))):
        raise ValueError("Expected HarkenSearch.exe built for this exact root with the dock QA seam")
    runtime = root / "profile/runtime"
    database = root / "profile/Local/HarkenSearch/Database/Dock QA/main.db"
    output.mkdir(parents=True, exist_ok=False)
    driver = Driver(runtime, output)
    report = dict(exe=str(exe), sha256=hashlib.sha256(executable).hexdigest(),
                  root=str(root), output=str(output), library=str(database),
                  fixture={}, cases=[], result="ERROR", test_exit_code=2)
    lock = root / "dock-qa.lock"
    desktop = process = None
    owns_lock = verified = False
    try:
        with lock.open("x", encoding="utf-8"):
            owns_lock = True
        require_idle_root(root)
        preflight(root, runtime, database)
        runtime.mkdir(exist_ok=True)
        user32 = window_api()
        desktop = PrivateDesktop()
        process, pid = desktop.start(exe, environment_overrides={
            "AURA_RUNTIME_ROOT": str(runtime), "AURA_HARNESS_LANE_KIND": "folder-search",
        })
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        report["identity"] = identity
        hwnd = identity["hwnd"]
        if (identity["pid"] != pid or Path(identity["exe_path"]).resolve() != exe
                or Path(identity["isolation_root"]).resolve() != root
                or Path(identity["runtime_root"]).resolve() != runtime
                or win32process.GetWindowThreadProcessId(hwnd)[1] != pid):
            raise RuntimeError(f"QA process/window identity mismatch: {identity}")
        verified = True
        state = wait_state(driver, lambda state: state["main_ui_ready"] and bool(state["db_path"])
                           and state["total_count"] == 24,
                           "isolated Dock QA library", 60)
        if Path(state["db_path"]).resolve() != database or state["total_count"] != 24:
            raise RuntimeError(f"Unexpected live library: {state['db_path']} ({state['total_count']} files)")
        origin = win32gui.ClientToScreen(hwnd, (0, 0))
        _, _, width, height = win32gui.GetClientRect(hwnd)
        user32.GetDpiForWindow.argtypes = user32.GetClientRect.argtypes[:1]
        dpi = user32.GetDpiForWindow(hwnd)
        if not dpi or width <= 0 or height <= 0:
            raise RuntimeError("Could not read the private window client bounds and DPI")
        bounds = (*origin, width, height)
        report.update(client_bounds=bounds, dpi=dpi, sample_count=state["total_count"])
        SplitterQa(driver, output, bounds, dpi / 96, report).run()
        passed = all(case["passed"] for case in report["cases"])
        report.update(result="PASS" if passed else "FAIL", test_exit_code=0 if passed else 1)
    except (Exception, KeyboardInterrupt) as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        cleanup = []
        if process is not None:
            try:
                app_exit, error = shutdown_preview(process, driver, verified, win32con, win32event, win32process)
                report["app_exit_code"] = app_exit
                if error or app_exit:
                    cleanup.append(f"App shutdown: exit={app_exit}, error={error}")
            except Exception as error:
                cleanup.append(f"Process cleanup: {error}")
        if desktop is not None:
            try:
                desktop.close()
            except Exception as error:
                cleanup.append(f"Desktop cleanup: {error}")
        if owns_lock and not cleanup:
            try:
                lock.unlink()
            except OSError as error:
                cleanup.append(f"QA lock cleanup: {error}")
        if cleanup:
            report.update(cleanup_errors=cleanup, result="ERROR", test_exit_code=2)
        path = output / "report.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps({key: report.get(key) for key in ("result", "error", "app_exit_code", "test_exit_code")},
                         ensure_ascii=False), flush=True)
        print(f"Report: {path}", flush=True)
    return report["test_exit_code"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--isolation-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    try:
        raise SystemExit(run(parser.parse_args()))
    except Exception as error:
        print(f"QA setup failed: {error}", file=sys.stderr)
        raise SystemExit(2)
