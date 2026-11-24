"""Compare the rendered dock highlight and actual landing in an isolated QA build."""
from __future__ import annotations

import argparse
import ctypes.wintypes
import hashlib
import json
import re
import time
from pathlib import Path

from qa_dock_grip_modal import shutdown_preview
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_top_menu_font import exe_has_root, require_idle_root
from qa_transport_themes import Driver, window_api


MODULES = ("filter", "info", "similar-sounds", "mod-ucs-browser",
           "mod-tag-browser", "mod-workflow", "mod-transfer")
PLACEMENTS = ("outer-left", "outer-right", "outer-bottom", "panel-left",
              "panel-right", "panel-top", "panel-bottom")
SIDES = {"left": 0, "right": 1, "top": 2, "bottom": 3}
RECT_KEYS = ("x", "y", "w", "h")


def set_scale(root, scale):
    settings = root / "profile/Local/HarkenSearch/settings.ini"
    text = settings.read_text(encoding="utf-8-sig")
    scale_line = f"uiScalePercent={scale}"
    updated, count = re.subn(r"(?m)^uiScalePercent=.*$", scale_line, text)
    if not count:
        updated = f"{updated.rstrip()}\n{scale_line}\n"
    settings.write_text(updated, encoding="utf-8")


def client_bounds(hwnd):
    user32 = window_api()
    rect = ctypes.wintypes.RECT()
    origin = ctypes.wintypes.POINT()
    if not user32.GetClientRect(hwnd, ctypes.byref(rect)):
        raise RuntimeError("Could not read the QA window size")
    if not user32.ClientToScreen(hwnd, ctypes.byref(origin)):
        raise RuntimeError("Could not read the QA window origin")
    return origin.x, origin.y, rect.right, rect.bottom


def target_points(placement, bounds, anchor, results):
    left, top, width, height = bounds
    scope, side = placement.split("-")
    if scope == "outer":
        if side == "bottom":
            middle = results["x"] + results["w"] / 2
            for inset in (6, 12, 22):
                yield middle, results["y"] + results["h"] - inset
            for inset in (8, 22, 40):
                yield middle, top + height - inset
        else:
            edge = left if side == "left" else left + width
            direction = 1 if side == "left" else -1
            for inset in (8, 16, 28):
                yield edge + direction * inset, top + height * .30
        return
    for inset in (6, 12, 19, 26):
        if side in ("left", "right"):
            point_x = anchor["x"] + (inset if side == "left" else anchor["w"] - inset)
            yield point_x, anchor["y"] + anchor["h"] * .53
        else:
            point_y = anchor["y"] + (inset if side == "top" else anchor["h"] - inset)
            yield anchor["x"] + anchor["w"] * .47, point_y


def run_exit_code(report, gestures_only):
    checks = report.get("gestures", []) if gestures_only else report["cases"]
    if report.get("app_exit") != 0 or report.get("shutdown_error"):
        return 1
    return 0 if checks and all(check["passed"] for check in checks) else 1


def run(args):
    import win32con
    import win32event
    import win32process

    exe = args.exe.resolve(strict=True)
    root = args.isolation_root.resolve(strict=True)
    out = args.output.resolve()
    if not out.is_relative_to(root):
        raise ValueError("Output must be within this isolated QA run")
    executable = exe.read_bytes()
    if (not executable.startswith(b"MZ") or not exe_has_root(executable, root)
            or not all(channel in executable for channel in
                       (b"dock_pointer\0", b"capture_preview_frame\0", b"magic_eq_identity\0"))):
        raise ValueError("The EXE does not embed this fixed isolation root and the dock QA commands")
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / "profile/runtime"
    database = root / "profile/Local/HarkenSearch/Database/Dock QA/main.db"
    if not database.is_file():
        raise FileNotFoundError(database)
    lock = root / "dock-qa.lock"
    owns_lock = False
    desktop = None
    process = None
    verified = False
    report = dict(exe=str(exe), sha256=hashlib.sha256(executable).hexdigest(),
                  theme=args.theme, scale=args.scale, cases=[])

    def command(**payload):
        return driver.command(payload)

    def settled(predicate=lambda state: True, timeout=6):
        first = command(cmd="dock_state")
        last = first
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            last = command(cmd="dock_state")
            if last["frame"] >= first["frame"] + 3 and predicate(last):
                return last
        panels = last.get("panels", {})
        raise TimeoutError(f"dock state: dragging={last.get('dragging')}, "
                           f"preview={last.get('preview_kind')}, panels={list(panels)}")

    def start_drag(module):
        command(cmd="dock_pointer", phase="focus")
        command(cmd="dock_pointer", phase="hover", module=module, part="title")
        settled()
        command(cmd="dock_pointer", phase="down", module=module, part="title")
        settled()

    def float_for_empty_wall(module):
        initial = settled()
        if initial["panels"][module]["floating"]:
            return
        surface = initial["panels"]["search-results"]
        point_x = surface["x"] + surface["w"] / 2
        point_y = surface["y"] + surface["h"] / 2
        start_drag(module)
        command(cmd="dock_pointer", phase="move", x=point_x, y=point_y)
        settled(lambda state: state["dragging"] and state["preview_kind"] == 0)
        command(cmd="dock_pointer", phase="up", x=point_x, y=point_y)
        settled(lambda state: not state["dragging"] and state["panels"][module]["floating"])

    def fixture(module, placement):
        for window in MODULES[3:]:
            command(cmd="function_window", module=window.removeprefix("mod-"), action="close")
        command(cmd="similar_index", action="close")
        command(cmd="dock_setup")
        settled(lambda state: "filter" in state.get("panels", {}) and "info" in state["panels"])
        if placement == "outer-left":
            float_for_empty_wall("filter")
        if placement == "outer-right":
            command(cmd="similar_index", action="close")
            float_for_empty_wall("info")
        if module.startswith("mod-"):
            command(cmd="function_window", module=module.removeprefix("mod-"),
                    action="open", floating=True, x=500, y=110)
        if module == "similar-sounds":
            command(cmd="similar_index", action="open", floating=True)
        return settled(lambda state: module in state.get("panels", {})
                       and "filter" in state["panels"] and "info" in state["panels"])

    def scenario(module, placement, photo):
        before = fixture(module, placement)
        if before["input"]["modal"]:
            raise AssertionError(f"Unexpected modal: {before['input']['modal']}")
        scope, side = placement.split("-")
        other = "info" if module == "filter" else "filter"
        anchor = before["panels"][other]
        results = before["panels"]["search-results"]
        start_drag(module)
        points = []
        chosen = None
        preview = None
        for point_x, point_y in target_points(placement, bounds, anchor, results):
            command(cmd="dock_pointer", phase="move", x=point_x, y=point_y)
            state = settled()
            area = state["drawn_drop_rect"]
            points.append((point_x, point_y, state["preview_kind"], state["preview_side"],
                           state["preview_target"], state["drop_too_small"]))
            if (state["dragging"] and state["preview_side"] == SIDES[side]
                    and (state["preview_target"] == other if scope == "panel"
                         else state["preview_kind"] == 1)
                    and area["w"] > 0 and area["h"] > 0):
                chosen, preview = (point_x, point_y), state
                break
        if chosen is None:
            command(cmd="dock_pointer", phase="up")
            settled(lambda state: not state["dragging"])
            return dict(module=module, placement=placement, passed=False, points=points,
                        error="No active drop highlight for this placement")
        name = f"{module}-{placement}"
        if photo:
            capture(driver, out / f"{name}-preview.png")
        command(cmd="dock_pointer", phase="up", x=chosen[0], y=chosen[1])
        after = settled(lambda state: not state["dragging"])
        actual = after["panels"].get(module, {})
        highlight = preview["drawn_drop_rect"]
        errors = ({key: abs(actual[key] - highlight[key]) for key in RECT_KEYS}
                  if actual and not actual["floating"] else {})
        widths = {key: abs(after["panels"][key]["w"] - panel["w"])
                  for key, panel in before["panels"].items()
                  if not panel["floating"] and key not in (module, "search-results")
                  and key in after["panels"]}
        missing = sorted(key for key, panel in before["panels"].items()
                         if not panel["floating"] and key not in (module, "search-results")
                         and key not in after["panels"])
        stack_errors = {}
        if scope == "panel" and side in ("top", "bottom") and other in widths and errors:
            stacked_anchor = after["panels"][other]
            stack_errors = {
                "width": abs(actual["w"] - stacked_anchor["w"]),
                "left_edge": abs(actual["x"] - stacked_anchor["x"]),
                "existing_width_loss": max(0.0, anchor["w"] - stacked_anchor["w"]),
            }
            widths.pop(other)
        if photo:
            capture(driver, out / f"{name}-landed.png")
        return dict(module=module, placement=placement, highlight=highlight, actual=actual,
                    rectangle_error_px=errors, protected_width_error_px=widths, stack_error_px=stack_errors,
                    missing=missing,
                    passed=bool(errors) and max(errors.values()) <= 1 and not missing
                    and all(error <= .01 for error in stack_errors.values())
                    and all(error <= .01 for error in widths.values()))

    def check_gestures():
        results = report["gestures"] = []
        module = "mod-ucs-browser"
        landing = scenario(module, "panel-bottom", False)
        if not landing["passed"]:
            raise AssertionError(landing)
        docked = settled()
        center = docked["panels"]["search-results"]
        start_drag(module)
        command(cmd="dock_pointer", phase="move", x=center["x"] + center["w"] / 2,
                y=center["y"] + center["h"] / 2)
        settled(lambda state: state["dragging"] and state["panels"][module]["floating"])
        command(cmd="dock_pointer", phase="escape")
        restored = settled(lambda state: not state["dragging"]
                           and not state["panels"].get(module, {}).get("floating", True))
        command(cmd="dock_pointer", phase="up")
        settled(lambda state: not state["input"]["down"])
        errors = {panel: {key: abs(restored["panels"][panel][key] - docked["panels"][panel][key])
                          for key in RECT_KEYS} for panel in (module, "filter", "search-results")}
        result = dict(check="escape_restores_stacked_layout", errors=errors, passed=(
            restored["layout"] == docked["layout"]
            and all(max(panel.values()) <= .01 for panel in errors.values())))
        results.append(result)
        if not result["passed"]:
            raise AssertionError(result)
        if args.capture:
            capture(driver, out / "escape-restored.png")
        float_for_empty_wall(module)
        floated = settled(lambda state: state["panels"][module]["floating"])
        results.append(dict(check="detach_into_floating_window", actual=floated["panels"][module],
                            passed=True))

        initial = fixture(module, "panel-right")
        anchor = initial["panels"]["info"]
        target = (anchor["x"] + anchor["w"] / 2, anchor["y"] + anchor["h"] / 2)
        start_drag(module)
        command(cmd="dock_pointer", phase="move", x=target[0], y=target[1])
        settled(lambda state: state["dragging"])
        if args.capture:
            capture(driver, out / "tab-merge-preview.png")
        command(cmd="dock_pointer", phase="up", x=target[0], y=target[1])
        merged = settled(lambda state: not state["dragging"]
                         and {module, "info"}.issubset(state["dock_group_tabs"]))
        groups = merged["dock_tab_groups"]
        result = dict(check="merge_into_tab_group", groups=groups, passed=any(
            {module, "info"}.issubset(group["members"]) for group in groups))
        results.append(result)
        if not result["passed"]:
            raise AssertionError(result)
        if args.capture:
            capture(driver, out / "tab-merged.png")

    try:
        with lock.open("x", encoding="utf-8"):
            owns_lock = True
        require_idle_root(root)
        set_scale(root, args.scale)
        driver = Driver(runtime, out)
        desktop = PrivateDesktop()
        process, pid = desktop.start(exe, environment_overrides={"AURA_RUNTIME_ROOT": str(runtime)})
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        if (identity["pid"] != pid or Path(identity["exe_path"]).resolve() != exe
                or Path(identity["isolation_root"]).resolve() != root):
            raise RuntimeError(f"QA process identity mismatch: {identity}")
        verified = True
        report["identity"] = identity
        state = wait_state(driver, lambda state: state["main_ui_ready"] and bool(state["db_path"]),
                           "isolated sampled library", 60)
        if Path(state["db_path"]).resolve() != database.resolve():
            raise RuntimeError(f"Wrong QA library: {state['db_path']}")
        bounds = client_bounds(identity["hwnd"])
        report["library"] = str(database)
        report["client_bounds"] = bounds
        report["sample_count"] = state["total_count"]
        command(cmd="set_theme", theme=args.theme)
        photo = args.capture
        if args.gestures_only:
            check_gestures()
        else:
            for module in args.module or MODULES:
                for placement in args.placement or PLACEMENTS:
                    result = scenario(module, placement, photo)
                    report["cases"].append(result)
                    print("PASS" if result["passed"] else "FAIL", module, placement, flush=True)
                    if args.fail_fast and not result["passed"]:
                        raise AssertionError(result)
    finally:
        try:
            if process is not None:
                report["app_exit"], report["shutdown_error"] = shutdown_preview(
                    process, driver, verified, win32con, win32event, win32process)
        finally:
            try:
                if desktop is not None:
                    desktop.close()
                (out / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                                  encoding="utf-8")
            finally:
                if owns_lock:
                    lock.unlink()
    return run_exit_code(report, args.gestures_only)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--isolation-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--theme", type=int, choices=(0, 10), default=0)
    parser.add_argument("--scale", type=int, choices=(100, 125), default=100)
    parser.add_argument("--module", choices=MODULES, action="append")
    parser.add_argument("--placement", choices=PLACEMENTS, action="append")
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--gestures-only", action="store_true")
    raise SystemExit(run(parser.parse_args()))
