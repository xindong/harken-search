"""Verify day-palette editing in an isolated native package on a private desktop.

Real controls open/save/cancel the editor. The existing diagnostic draft-color
seam supplies exact RGBA values; rendered colors and saved files are checked.
No shared mouse, system clock, or user profile is changed.
"""
from pathlib import Path
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import time
import traceback
from PIL import Image
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_settings_day_theme import require_saved_palette
from qa_transport_themes import Driver


def validate_package(exe, root, receipt):
    if not __debug__:
        raise RuntimeError("Optimized Python is unsupported: acceptance checks must remain enabled")
    evidence = json.loads(receipt.read_text(encoding="utf-8-sig"))
    assert Path(evidence["exePath"]).resolve() == exe.resolve(), "Package executable differs from its receipt"
    assert Path(evidence["isolationRoot"]).resolve() == root.resolve(), "Package isolation root differs from its receipt"
    binary = exe.read_bytes()
    assert hashlib.sha256(binary).hexdigest() == evidence["exeSha256"], "Executable digest differs from its receipt"
    # harken_user_data_layout.cpp embeds this exact NUL-terminated UTF-8 root.
    # Check the executable itself, so a mistyped receipt cannot redirect it.
    compiled_root = (root.resolve().as_posix().rstrip("/")+chr(0)).encode("utf-8")
    assert compiled_root.lower() in binary.lower(), "Executable does not embed this preview isolation root"


def run(args):
    import win32con, win32event, win32gui, win32process
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    validate_package(exe, root, args.receipt.resolve())
    assert exe.is_file() and (root / "profile/Local/HarkenSearch/settings.ini").is_file()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process = None
    identity = {}
    identity_verified = False
    profile = root / "profile/Local/HarkenSearch"
    assert profile.resolve() != (Path(os.environ["LOCALAPPDATA"])/"HarkenSearch").resolve(), "Use a disposable profile"
    day, night = profile / "theme_schedule_day.json", profile / "theme_schedule_night.json"
    report = {"exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "startedAt": datetime.now(timezone.utc).isoformat(), "checks": [],
              "input": "private desktop and app-owned ImGui input queue"}

    def cmd(**payload):
        return driver.command(payload)

    def theme():
        return cmd(cmd="theme_schedule")

    def ui():
        return cmd(cmd="settings_ui")

    def wait(read, predicate, label):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            value = read()
            if predicate(value):
                return value
            time.sleep(.06)
        raise AssertionError((label, value))

    def settle():
        first = theme()["frame"]
        return wait(theme, lambda x: x["frame"] > first + 3, "rendered frames")

    def check(name, **details):
        report["checks"].append({"case": name, **details})
        print(name, flush=True)

    def click_rect(rect):
        a, b, c, d = rect
        ox, oy = win32gui.ClientToScreen(identity["hwnd"], (0, 0))
        for phase in ("hover", "down", "up"):
            cmd(cmd="theme_schedule", action="pointer", phase=phase, x=(a+c)/2-ox, y=(b+d)/2-oy)
            settle()

    def find(identifier):
        settle()
        items = [x for x in ui()["items"] if x["id"] == identifier and not x["disabled"]]
        assert len(items) == 1, ("Missing real settings control", identifier, ui())
        item = items[0]
        a,b,c,d = item["bounds"]; l,t,r,bot = item["clip"]
        assert a >= l-1 and b >= t-1 and c <= r+1 and d <= bot+1, ("Clipped control", item)
        return item

    def click(identifier):
        click_rect(find(identifier)["bounds"])

    def button(label):
        cmd(cmd="ordinary_button_audit"); settle()
        observation = cmd(cmd="ordinary_button_audit")
        items = [x for x in observation["checks"] if x["label"] == label and not x["disabled"]]
        assert len(items) == 1, (label, items)
        click_rect(items[0]["bounds"])

    def settings():
        cmd(cmd="ordinary_button_window", module="settings", action="open"); settle()
        assert ui()["open"]
        cmd(cmd="settings_ui", action="resize", width=880, height=600); settle()
        a,b,c,d = ui()["bounds"]
        assert abs(c-a-880) <= 1 and abs(d-b-600) <= 1, ui()["bounds"]

    def apply():
        if ui()["pending"]:
            click("apply")
        settle()
        assert not ui()["error"], ui()

    def snapshot(path):
        return path.read_bytes() if path.exists() else None

    def pixels(name, rgba):
        if ui()["open"] and not theme()["editor_open"]:
            click("done"); settle()
        rgb = tuple(int(channel*255+.5) for channel in rgba[:3])
        deadline = time.monotonic()+6
        while True:
            capture(driver, out/(name+".png"))
            with Image.open(out/(name+".png")) as im:
                count = sum(n for n,color in im.convert("RGB").getcolors(im.width*im.height) if color == rgb)
            if count > 128 or time.monotonic() >= deadline:
                break
            time.sleep(.08)  # Wait for the existing modal dimming animation.
        assert count > 128, (name,"expected background missing from actual frame",rgb,count)
        report.setdefault("pixelChecks",[]).append(dict(name=name,rgb=rgb,matchingPixels=count))

    def saved():
        return dict(line.split("=",1) for line in (profile/"settings.ini").read_text(encoding="utf-8").splitlines() if "=" in line)

    def start(font):
        nonlocal process, identity, identity_verified
        identity_verified = False
        values = saved(); values["readingFontSize"] = str(font); values["language"] = "0"
        assert Path(values["currentDbPath"]).resolve().is_relative_to(root), "Library must remain inside this isolated root"
        (profile/"settings.ini").write_text("".join(k+"="+v+"\n" for k,v in values.items()), encoding="utf-8")
        process, pid = desktop.start(exe, environment_overrides={
            "HARKEN_THEME_QA_MINUTE":"600", "LOCALAPPDATA":str(root/"profile/Local"),
            "APPDATA":str(root/"profile/Roaming"), "AURA_RUNTIME_ROOT":str(runtime),
            "TEMP":str(root/"temp"), "TMP":str(root/"temp")})
        identity = driver.command({"cmd":"magic_eq_identity"}, timeout=60)
        assert identity["pid"] == pid and Path(identity["exe_path"]).resolve() == exe
        assert Path(identity["isolation_root"]).resolve() == root
        identity_verified = True
        state = wait_state(driver, lambda s:s["main_ui_ready"] and s["total_count"] > 0 and not s["db_switch_in_progress"], "populated library", 90)
        assert Path(state["db_path"]).resolve().is_relative_to(root)
        report["library"] = {"path":state["db_path"],"rows":state["total_count"]}
        cmd(cmd="dock_pointer", phase="focus"); settle()
        assert ui()["live_font"] == font

    def stop():
        nonlocal process
        if process is None:
            return
        forced = False
        try:
            if identity_verified and win32event.WaitForSingleObject(process,0) == win32con.WAIT_TIMEOUT:
                cmd(cmd="info_edit_input", action="exit")
            assert win32event.WaitForSingleObject(process,20000) == win32con.WAIT_OBJECT_0
            assert win32process.GetExitCodeProcess(process) == 0
        finally:
            try:
                if win32event.WaitForSingleObject(process,0) == win32con.WAIT_TIMEOUT:
                    forced = True
                    win32process.TerminateProcess(process,1)
                    assert win32event.WaitForSingleObject(process,5000) == win32con.WAIT_OBJECT_0
                report.setdefault("shutdowns",[]).append({"forced":forced,"exitCode":win32process.GetExitCodeProcess(process)})
            finally:
                process.Close(); process = None

    def editor():
        click("day-combo")
        click("day-editor")
        return wait(theme, lambda s:s["editor_open"] and s["editor_period"] == 0, "day editor opened through real button")

    try:
        night_before = require_saved_palette(profile,"night")
        night_colors = json.loads(night_before)["colors"]["mainWindowBG"]
        expected_night = "".join(f"{int(channel*255):02x}" for channel in (night_colors[3],*night_colors[:3]))
        start(18); settings()
        if ui()["schedule"] == 0:
            click("automatic"); apply()
        assert not theme()["night_warning"] and theme()["night_bg"] == expected_night, theme()
        manual_before = saved()["theme"]
        for index, builtin in enumerate((0,10,2,1,3)):
            click("day-combo")
            assert {x["id"] for x in ui()["items"] if x["id"].startswith("theme-option-")} == {"theme-option-"+str(i) for i in range(5)}
            click("theme-option-"+str(index)); apply()
            observed = wait(theme, lambda s:s["theme"] == builtin and s["period"] == "day", "chosen day preset rendered")
            assert snapshot(night) == night_before and saved()["theme"] == manual_before
            check("day-preset-"+str(builtin), active=observed["active_bg"])
        editor()
        before = snapshot(day)
        cmd(cmd="theme_schedule", action="draft_color", key="mainWindowBG", rgba=[.12,.21,.31,1]); settle()
        preview = theme(); assert preview["active_bg"] != preview["day_bg"]
        assert preview["imgui_bg"] == preview["active_bg"] and snapshot(day) == before
        expected_color = "ff1e354f"  # ThemeColors::toARGBHex truncates each 0..255 channel.
        assert preview["active_bg"] == expected_color, preview
        pixels("day-preview-18",[.12,.21,.31,1])
        button("保存配色"); wait(theme, lambda s:not s["editor_open"], "save closes editor"); settle()
        stored = snapshot(day); colors = json.loads(stored)["colors"]
        assert all(abs(a-b)<1e-6 for a,b in zip(colors["mainWindowBG"],[.12,.21,.31,1]))
        assert snapshot(night) == night_before and saved()["theme"] == manual_before
        saved_color = theme()
        assert saved_color["active_bg"] == saved_color["day_bg"] == saved_color["imgui_bg"] == expected_color, saved_color
        pixels("day-saved-18",[.12,.21,.31,1])
        check("save-persists-rendered-day-colors-without-changing-night-or-manual")
        settings(); editor()
        cmd(cmd="theme_schedule", action="draft_color", key="mainWindowBG", rgba=[.7,.1,.2,1]); settle()
        button("取消修改"); wait(theme, lambda s:not s["editor_open"], "cancel closes editor"); settle()
        assert snapshot(day) == stored and snapshot(night) == night_before
        canceled = theme()
        assert canceled["active_bg"] == canceled["day_bg"] and canceled["imgui_bg"] == canceled["day_bg"], canceled
        pixels("day-cancel-restored-18",[.12,.21,.31,1])
        check("cancel-discards-day-preview")
        stop(); start(24)
        assert snapshot(day) == stored and snapshot(night) == night_before
        loaded = theme()
        assert loaded["active_bg"] == loaded["day_bg"] == loaded["imgui_bg"] == expected_color and not loaded["day_warning"], loaded
        assert saved()["theme"] == manual_before
        pixels("day-restarted-24",[.12,.21,.31,1])
        check("saved-day-colors-survive-restart")
        settings()
        find("day-combo"); capture(driver, out/"day-controls-24-small.png")
        editor(); cmd(cmd="theme_schedule", action="draft_color", key="mainWindowBG", rgba=[.8,.2,.1,1]); settle()
        capture(driver, out/"day-preview-24.png"); stop(); start(18)
        assert snapshot(day) == stored and snapshot(night) == night_before
        loaded = theme()
        assert loaded["active_bg"] == loaded["day_bg"] == loaded["imgui_bg"] == expected_color and not loaded["day_warning"], loaded
        assert saved()["theme"] == manual_before
        pixels("day-unsaved-exit-restarted-18",[.12,.21,.31,1])
        check("exit-during-unsaved-preview-keeps-saved-palette")
        settings(); click("automatic"); apply(); settle()
        assert theme()["mode"] == 0 and theme()["theme"] == int(manual_before)
        check("disabling-schedule-restores-manual-theme")
        report["passed"] = True
    except Exception:
        report["passed"] = False; report["error"] = traceback.format_exc()
        try:
            if identity_verified and process is not None and win32event.WaitForSingleObject(process,0) == win32con.WAIT_TIMEOUT:
                capture(driver, out/"failure.png")
        except Exception: pass
        raise
    finally:
        try: stop()
        except Exception:
            report["passed"] = False; report["shutdownError"] = traceback.format_exc()
            raise
        finally:
            desktop.close()
            report["finishedAt"] = datetime.now(timezone.utc).isoformat()
            report["limits"] = ["Exact colors entered using existing diagnostic draft-color seam; real editor open/save/cancel and rendered output verified", "No physical sleep/resume or OS clock changes"]
            (out/"report.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("exe","isolation-root","output","receipt"):
        parser.add_argument("--"+name,type=Path,required=True)
    run(parser.parse_args())
