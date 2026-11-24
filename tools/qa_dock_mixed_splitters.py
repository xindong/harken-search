"""Check the actual mixed-T Dock fixture on its pinned private desktop.

Choose which window is on top and use a fresh --output directory.
0 = pass, 1 = reproduced failure, 2 = fixture, capture, or cleanup could not be verified.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from statistics import median

from PIL import Image

from qa_dock_grip_modal import shutdown_preview
from qa_dock_parallel_splitters import (
    COLUMNS, FILTER, INFO, KEYS, RESULTS, TRANSFER, SplitterQa, edge, grip_point, preflight,
)
from qa_required_library_setup import PrivateDesktop, wait_state
from qa_top_menu_font import absolute_path, exe_has_root, reject_reparse, require_idle_root
from qa_transport_themes import Driver, window_api


TRANSFER_SIDES = {"transfer-above": 2, "transfer-below": 3}
FILTER_START_WIDTH = 320
STACK_WIDTH = 480
STEP = 32
TOLERANCE = 3
EFFECTS = {
    "stack": {},
    "stack-info": {TRANSFER: {"w": 1}, FILTER: {"w": 1}, INFO: {"x": 1, "w": -1}},
    "info-results": {INFO: {"w": 1}, RESULTS: {"x": 1, "w": -1}},
}


def bottom(rect):
    return rect["y"] + rect["h"]


def stack_order(state):
    return sorted((FILTER, TRANSFER), key=lambda panel: state["panels"][panel]["y"])


def shape_errors(state):
    panels = state.get("panels", {})
    if set(panels) != set(COLUMNS):
        return [f"Expected only Transfer, Filter, Info and Results; observed {list(panels)}"]
    top, lower = (panels[name] for name in stack_order(state))
    results, info = panels[RESULTS], panels[INFO]
    if any(panel["floating"] for panel in panels.values()) or len({p["viewport"] for p in panels.values()}) != 1:
        return ["Expected four docked panels in one viewport"]
    if state["dragging"] or state["owner"] or state["input"]["modal"]:
        return ["Unexpected title gesture or modal"]
    errors = []
    if (abs(top["x"] - lower["x"]) > TOLERANCE or abs(edge(top) - edge(lower)) > TOLERANCE
            or not -3 <= lower["y"] - bottom(top) <= 16):
        errors.append("Transfer and Filter do not form a single left stack")
    for first_right, second_left in ((edge(top), info["x"]), (edge(lower), info["x"]),
                                      (edge(info), results["x"])):
        if not -3 <= second_left - first_right <= 18:
            errors.append(f"Info is not between the stack and Results: {first_right}, {second_left}")
    if any(abs(panel["y"] - top["y"]) > 12 or abs(bottom(panel) - bottom(lower)) > 12
           for panel in (info, results)):
        errors.append("Info or Results does not span the stack height")
    return errors


def fixture_errors(state, layout, scale):
    errors = shape_errors(state)
    if errors:
        return errors
    panels = state["panels"]
    requested = {TRANSFER: round(STACK_WIDTH * scale), FILTER: round(STACK_WIDTH * scale)}
    actual = {name: panels[name]["w"] for name in requested}
    if (any(abs(actual[name] - expected) > TOLERANCE for name, expected in requested.items())
            or abs(actual[FILTER] - actual[TRANSFER]) > 2):
        errors.append(f"{layout} requires widths {requested}, observed {actual}")
    expected_top = TRANSFER if TRANSFER_SIDES[layout] == 2 else FILTER
    if stack_order(state)[0] != expected_top:
        errors.append(f"{expected_top} should be above the other left pane")
    return errors


def tracks(state):
    panels = state["panels"]
    top, lower = (panels[name] for name in stack_order(state))
    results, info = panels[RESULTS], panels[INFO]
    stack_cross = sorted((bottom(top), lower["y"]))
    stack_info_cross = sorted((edge(top), edge(lower), info["x"]))
    info_results_cross = sorted((edge(info), results["x"]))
    top_y, bottom_y = max(top["y"], info["y"]), min(bottom(lower), bottom(info))
    return {
        "stack": ("x", (stack_cross[0] - 4, stack_cross[-1] + 4),
                  (top["x"] + 54, min(edge(top), edge(lower)) - 18)),
        "stack-info": ("y", (stack_info_cross[0] - 6, stack_info_cross[-1] + 6),
                       (top_y + 24, bottom_y - 24)),
        "info-results": ("y", (info_results_cross[0] - 3, info_results_cross[-1] + 3),
                         (max(info["y"], results["y"]) + 24, min(bottom(info), bottom(results)) - 24)),
    }


def triplets(image, axis, cross_bounds, along_bounds):
    """Find three equally spaced, locally contrasting dots on a quiet seam."""
    pixels = image.load()
    along_length, cross_length = image.size if axis == "x" else image.size[::-1]
    cross_range = range(max(0, round(cross_bounds[0])), min(cross_length, round(cross_bounds[1]) + 1))
    start, end = max(10, round(along_bounds[0])), min(along_length - 10, round(along_bounds[1]))
    candidates = []
    for cross in cross_range:
        def pix(along):
            return pixels[along, cross] if axis == "x" else pixels[cross, along]

        contrast = {pos: pix(pos) - median(pix(pos + offset) for offset in (-10, -4, -3, 3, 4, 10))
                    for pos in range(start, end)}
        for sign in (1, -1):
            scores = {pos: sign * value for pos, value in contrast.items()}
            peaks = {pos for pos in range(start + 1, end - 1)
                     if scores[pos] >= 11 and scores[pos] > scores[pos - 1] and scores[pos] >= scores[pos + 1]}
            for center in peaks:
                for gap in range(5, 11):
                    for drift in (-1, 0, 1):
                        first, last = center - gap, center + gap + drift
                        if first not in peaks or last not in peaks:
                            continue
                        strength = [scores[pos] for pos in (first, center, last)]
                        if min(strength) >= max(strength) * .4:
                            candidates.append((min(strength), cross, center, (first, center, last)))
    found = []
    for score, cross, center, dots in sorted(candidates, reverse=True):
        if any(abs(cross - saved["cross"]) <= 4 and abs(center - saved["along"]) <= 5 for saved in found):
            continue
        found.append(dict(cross=cross, along=center, dots=dots, contrast=score))
    return sorted(found, key=lambda mark: mark["along"])


def geometry_delta(before, after):
    return {name: {key: round(after["panels"][name][key] - before["panels"][name][key], 2)
                   for key in KEYS} for name in COLUMNS if name in after.get("panels", {})}


def change_errors(name, requested, before, after):
    errors = shape_errors(after)
    if errors:
        return errors
    delta = geometry_delta(before, after)
    effects = EFFECTS[name]
    if name == "stack":
        top, lower = stack_order(before)
        effects = {top: {"h": 1}, lower: {"y": 1, "h": -1}}
    primary = next(iter(effects))
    axis_extent = "h" if name == "stack" else "w"
    moved = delta[primary][axis_extent]
    if requested and (moved * requested <= 0 or abs(moved) < 5):
        errors.append(f"Requested {requested}px but primary edge moved {moved}px")
    if not requested:
        moved = 0
    for panel, changes in delta.items():
        for field, amount in changes.items():
            expected = effects.get(panel, {}).get(field, 0) * moved
            if abs(amount - expected) > TOLERANCE:
                errors.append(f"{panel}.{field} changed {amount}px, expected {expected}px")
    return errors


class MixedQa(SplitterQa):
    def place(self, module, target, side):
        record = dict(module=module, target=target, side=side, attempts=[])
        self.report["fixture"]["placements"].append(record)
        before = self.settled()
        anchor = before["panels"][target]
        self.send(cmd="dock_pointer", phase="focus")
        self.pointer("hover", None, title=module)
        self.pointer("down", None, title=module)
        for inset in (9, 15, 22, 32, 44, 54):
            if side == 1:
                point = (round(edge(anchor) - inset), round(anchor["y"] + anchor["h"] * .53))
            else:
                edge_y = anchor["y"] + inset if side == 2 else bottom(anchor) - inset
                point = (round(anchor["x"] + anchor["w"] * .51), round(edge_y))
            preview = self.pointer("move", point)
            record["attempts"].append({key: preview[key] for key in
                ("dragging", "drop_armed", "drop_too_small", "preview_target", "preview_side", "drawn_drop_rect")})
            record["attempts"][-1]["pointer"] = point
            if (preview["dragging"] and preview["preview_target"] == target
                    and preview["preview_side"] == side and preview["drawn_drop_rect"]["w"] > 0):
                record["screenshot"] = self.photo(f"setup-{module}-preview.png")
                record["landed"] = self.pointer("up", point)
                return
        record["screenshot"] = self.photo(f"setup-{module}-rejected.png")
        self.send(cmd="dock_pointer", phase="escape")
        self.pointer("up", point)
        raise RuntimeError(f"Could not place {module} next to {target} on side {side}")

    def fixture(self):
        fixture = self.report["fixture"]
        layout = self.report["layout"]
        initial = self.settled()
        fixture.update(initial=initial, initial_errors=fixture_errors(initial, layout, self.scale), placements=[])
        fixture["mode"] = "real-title-drops"
        for module in ("ucs-browser", "tag-browser", "workflow", "transfer"):
            self.send(cmd="function_window", module=module, action="close")
        self.send(cmd="dock_setup")
        self.send(cmd="similar_index", action="close")
        initial = self.settled(lambda state: set(state.get("panels", {})) == {FILTER, INFO, RESULTS})
        width = round(FILTER_START_WIDTH * self.scale)
        adjustment = round(width - initial["panels"][FILTER]["w"])
        if abs(adjustment) > TOLERANCE:
            held, moved, after = self.drag(grip_point(initial, FILTER, RESULTS), adjustment)
            fixture["filter_width_preparation"] = dict(held=held, moved=moved, after=after)
            if abs(after["panels"][FILTER]["w"] - width) > TOLERANCE:
                raise RuntimeError(f"Could not prepare Filter at {width}px")
        self.place(INFO, FILTER, 1)
        self.send(cmd="function_window", module="transfer", action="open", floating=True)
        self.settled(lambda state: state.get("panels", {}).get(TRANSFER, {}).get("floating"))
        self.place(TRANSFER, FILTER, TRANSFER_SIDES[layout])
        state = self.settled()
        fixture["rebuilt"] = state
        if errors := fixture_errors(state, layout, self.scale):
            fixture["invalid_layout_screenshot"] = self.photo("setup-invalid-layout.png")
            raise RuntimeError(f"Failed to rebuild the reference T layout: {errors}")
        return state

    def idle(self, name):
        self.send(cmd="dock_pointer", phase="focus")
        self.pointer("hover", None, title=FILTER)
        origin_x, origin_y, _, _ = self.bounds
        self.pointer("hover", (origin_x + 12, origin_y + 12))
        state = self.settled(lambda sample: not sample["input"]["down"])
        path = self.photo(f"{name}.png")
        readback = {}
        with Image.open(self.output / path) as frame:
            gray = frame.convert("L")
            for seam, (axis, cross, along) in tracks(state).items():
                cross_origin, along_origin = (origin_y, origin_x) if axis == "x" else (origin_x, origin_y)
                marks = triplets(gray, axis, tuple(value - cross_origin for value in cross),
                                 tuple(value - along_origin for value in along))
                for mark in marks:
                    mark["screen_center"] = ((mark["along"] + origin_x, mark["cross"] + origin_y)
                                             if axis == "x" else
                                             (mark["cross"] + origin_x, mark["along"] + origin_y))
                readback[seam] = dict(axis=axis, scan_cross=cross, scan_along=along, marks=marks)
        return dict(screenshot=path, state=state, seams=readback,
                    counts={key: len(entry["marks"]) for key, entry in readback.items()})

    def point(self, state, name, zone, idle):
        panels = state["panels"]
        top, lower = (panels[name] for name in stack_order(state))
        info = panels[INFO]
        if zone == "junction":
            return round((max(edge(top), edge(lower)) + info["x"]) / 2), round((bottom(top) + lower["y"]) / 2)
        if zone in ("above", "below"):
            segment = top if zone == "above" else lower
            return (round((edge(segment) + info["x"]) / 2),
                    round((max(segment["y"], info["y"]) + min(bottom(segment), bottom(info))) / 2))
        axis, current_cross, current_along = tracks(state)[name]
        old = idle["seams"][name]
        mark = max(old["marks"], key=lambda value: value["contrast"])
        cross_index, along_index = (1, 0) if axis == "x" else (0, 1)
        cross = mark["screen_center"][cross_index] + (sum(current_cross) - sum(old["scan_cross"])) / 2
        along = mark["screen_center"][along_index] + (sum(current_along) - sum(old["scan_along"])) / 2
        return (round(along), round(cross)) if axis == "x" else (round(cross), round(along))

    def stroke(self, name, zone, amount, idle, *, wrong_axis=False):
        case_name = f"{name}-{zone}-{'wrong-axis' if wrong_axis else ('plus' if amount > 0 else 'minus')}"
        before = self.settled()
        point = self.point(before, name, zone, idle)
        vertical_motion = (name == "stack") != wrong_axis
        target = (point[0], point[1] + amount) if vertical_motion else (point[0] + amount, point[1])
        held = self.grab(point)
        moved = self.pointer("move", target)
        after = self.pointer("up", target)
        errors = change_errors(name, 0, before, held)
        for sample in (moved, after):
            changed = any(abs(delta) > TOLERANCE
                          for panel in geometry_delta(before, sample).values() for delta in panel.values())
            requested = 0 if wrong_axis or (zone == "junction" and not changed) else amount
            errors.extend(change_errors(name, requested, before, sample))
        if zone != "junction" and not held["input"]["active_id"]:
            errors.append("No actual ImGui drag handle acquired")
        result = dict(name=case_name, seam=name, zone=zone, wrong_axis=wrong_axis, from_point=point,
                      to_point=target, active_id=held["input"]["active_id"],
                      screenshot=self.photo(f"{case_name}.png"),
                      geometry_delta=geometry_delta(before, after), errors=errors, passed=not errors,
                      before=before, held=held, moved=moved, after=after)
        self.report["cases"].append(result)
        print(json.dumps({key: result[key] for key in
                          ("name", "passed", "active_id", "geometry_delta", "errors")}), flush=True)

    def run(self):
        self.fixture()
        initial = self.report["idle_before"] = self.idle("01-idle-before")
        for name in EFFECTS:
            if not initial["seams"][name]["marks"]:
                continue
            step = -STEP if name == "stack" and TRANSFER_SIDES[self.report["layout"]] == 3 else STEP
            for amount in (step, -step):
                self.stroke(name, "mark", amount, initial)
            self.stroke(name, "mark", STEP, initial, wrong_axis=True)
        for zone in ("above", "below"):
            for amount in (STEP, -STEP):
                self.stroke("stack-info", zone, amount, initial)
        for name in ("stack", "stack-info"):
            self.stroke(name, "junction", STEP, initial)
        self.report["idle_after"] = self.idle("02-idle-after")
        required_cases = [case for case in self.report["cases"] if case["zone"] != "junction"]
        ids = {seam: sorted({case["active_id"] for case in required_cases
                            if case["seam"] == seam and case["active_id"]}) for seam in EFFECTS}
        self.report["hit_handles_per_seam"] = ids
        self.report["distinct_hit_handles"] = len({handle for handles in ids.values() for handle in handles})
        self.report["effective_seams"] = {
            seam: bool(initial["seams"][seam]["marks"])
            and all(case["passed"] for case in required_cases if case["seam"] == seam) for seam in EFFECTS}
        if self.report["distinct_hit_handles"] != 3 or any(len(handles) != 1 for handles in ids.values()):
            self.report["failures"].append(f"Expected one distinct hit handle per seam: {ids}")
        for key in ("idle_before", "idle_after"):
            counts = self.report[key]["counts"]
            if any(count != 1 for count in counts.values()):
                self.report["failures"].append(f"{key}: expected one rendered triplet per seam, saw {counts}")
            else:
                snapshot = self.report[key]
                left = snapshot["seams"]["stack-info"]["marks"][0]["screen_center"]
                right = snapshot["seams"]["info-results"]["marks"][0]["screen_center"]
                if abs(left[1] - right[1]) > TOLERANCE:
                    self.report["failures"].append(f"{key}: expected the vertical grips at the same height")
        if not all(case["passed"] for case in self.report["cases"]):
            self.report["failures"].append("One or more strokes changed the wrong regions, lost a handle, or did not move")


def run(args):
    import win32con
    import win32event
    import win32gui
    import win32process

    output, exe, root = map(absolute_path, (args.output, args.exe, args.isolation_root))
    runtime = root / "profile/runtime"
    database = root / "profile/Local/HarkenSearch/Database/Dock QA/main.db"
    for path in (root, output, exe):
        reject_reparse(path)
    if (exe != root / "verified/HarkenSearch.exe" or not output.is_relative_to(root) or output == root
            or any(output.is_relative_to(root / directory)
                   for directory in ("profile", "verified", "verified-final", "sounds", "fixture-backup"))):
        raise ValueError(f"Use the verified package and a new evidence directory within {root}")
    expected_hash = args.sha256.lower()
    if len(expected_hash) != 64 or any(char not in "0123456789abcdef" for char in expected_hash):
        raise ValueError("Provide the confirmed 64-digit SHA-256 of the final candidate")
    binary = exe.read_bytes()
    digest = hashlib.sha256(binary).hexdigest()
    if expected_hash != digest:
        raise ValueError(f"Candidate SHA-256 mismatch: {digest}")
    if not binary.startswith(b"MZ") or not exe_has_root(binary, root) or not all(
            token in binary for token in (b"dock_state\0", b"dock_pointer\0", b"folder_search_pointer\0",
                                          b"capture_preview_frame\0", b"magic_eq_identity\0")):
        raise ValueError("Missing pinned isolation root or required QA commands in the candidate")
    output.mkdir(parents=True, exist_ok=False)
    lock = root / "dock-mixed-splitters.lock"
    driver = Driver(runtime, output)
    report = dict(root=str(root), exe=str(exe), sha256=digest, layout=args.layout, fixture={}, cases=[],
                  failures=[], result="ERROR", test_exit_code=2)
    verified = owns_lock = False
    desktop = process = None
    try:
        with lock.open("x", encoding="utf-8"):
            owns_lock = True
        require_idle_root(root)
        preflight(root, runtime, database)
        user32 = window_api()
        desktop = PrivateDesktop()
        process, pid = desktop.start(exe, environment_overrides={
            "AURA_RUNTIME_ROOT": str(runtime), "AURA_HARNESS_LANE_KIND": "folder-search",
        })
        identity = report["identity"] = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        hwnd = identity["hwnd"]
        if (identity["pid"] != pid or Path(identity["exe_path"]).resolve() != exe
                or Path(identity["isolation_root"]).resolve() != root
                or Path(identity["runtime_root"]).resolve() != runtime
                or win32process.GetWindowThreadProcessId(hwnd)[1] != pid):
            raise RuntimeError(f"Candidate identity mismatch: {identity}")
        verified = True
        state = wait_state(driver, lambda sample: sample["main_ui_ready"] and sample["db_path"]
                           and sample["total_count"] == 24, "isolated 24-WAV Dock QA library", 60)
        if Path(state["db_path"]).resolve() != database:
            raise RuntimeError(f"Candidate loaded the wrong database: {state['db_path']}")
        _, _, width, height = win32gui.GetClientRect(hwnd)
        bounds = (*win32gui.ClientToScreen(hwnd, (0, 0)), width, height)
        user32.GetDpiForWindow.argtypes = user32.GetClientRect.argtypes[:1]
        dpi = user32.GetDpiForWindow(hwnd)
        if not dpi or width <= 0 or height <= 0:
            raise RuntimeError("Could not read the private-window bounds and DPI")
        report.update(client_bounds=bounds, dpi=dpi, library=str(database), sample_count=state["total_count"])
        MixedQa(driver, output, bounds, dpi / 96, report).run()
        report.update(result="FAIL" if report["failures"] else "PASS",
                      test_exit_code=1 if report["failures"] else 0)
    except (Exception, KeyboardInterrupt) as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        cleanup = []
        if process is not None:
            try:
                exit_code, shutdown_error = shutdown_preview(process, driver, verified, win32con, win32event, win32process)
                report["app_exit_code"] = exit_code
                if shutdown_error or exit_code:
                    cleanup.append(f"App exit={exit_code}: {shutdown_error}")
            except Exception as error:
                cleanup.append(f"Process cleanup: {error}")
        if desktop is not None:
            try:
                desktop.close()
            except Exception as error:
                cleanup.append(f"Desktop cleanup: {error}")
        if owns_lock and not cleanup:
            try:
                require_idle_root(root)
                if (runtime / "qa_cmd.json").exists():
                    raise RuntimeError("An isolated QA command is still pending")
                lock.unlink()
                report["fixed_root_idle"] = True
            except Exception as error:
                cleanup.append(f"Fixed root cleanup: {error}")
        if cleanup:
            report.update(result="ERROR", test_exit_code=2, cleanup_errors=cleanup)
        path = output / "report.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps({key: report.get(key) for key in ("result", "test_exit_code", "error", "fixed_root_idle",
                         "distinct_hit_handles", "failures", "cleanup_errors")}), flush=True)
        print(f"Report: {path}", flush=True)
    return report["test_exit_code"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--isolation-root", type=Path, required=True)
    parser.add_argument("--sha256", required=True, help="Confirmed SHA-256 of the final candidate")
    parser.add_argument("--layout", choices=tuple(TRANSFER_SIDES), required=True)
    parser.add_argument("--output", type=Path, required=True)
    try:
        raise SystemExit(run(parser.parse_args()))
    except Exception as error:
        print(f"QA setup failed: {error}", file=sys.stderr)
        raise SystemExit(2)
