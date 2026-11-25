"""Verify the DAW integration settings cards in an isolated native build.

The app runs on a private desktop with its own input queue; the user's desktop
and input are untouched. A real library snapshot (copied WAV files) is scanned
inside the isolation root so the settings page opens on a populated library.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import shutil
import time
import traceback
import wave

from PIL import Image

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_fresh_result_driver import FreshResultDriver as Driver


CARDS = ["daw-reaper", "daw-pro-tools", "daw-cubase", "daw-nuendo"]
DISABLED_CONFIG = [card + "-config-disabled" for card in CARDS[1:]]


def prepare_library(library: Path, source: Path | None, count: int) -> list[str]:
    library.mkdir(parents=True, exist_ok=True)
    copied: list[str] = []
    if source and source.is_dir():
        for path in sorted(source.rglob("*.wav")):
            if len(copied) >= count:
                break
            target = library / path.name
            if target.exists():
                copied.append(path.name)
                continue
            shutil.copy2(path, target)
            copied.append(path.name)
    index = len(copied)
    while len(copied) < count:
        index += 1
        name = f"daw-cards-synthetic-{index}.wav"
        samples = (int(6000 * math.sin(2 * math.pi * (240 + index * 20) * n / 22050))
                   for n in range(6615))
        with wave.open(str(library / name), "wb") as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(22050)
            audio.writeframes(b"".join(sample.to_bytes(2, "little", signed=True)
                                       for sample in samples))
        copied.append(name)
    return copied


def relative_luminance(argb_hex: str) -> float:
    value = argb_hex.lstrip("#")
    if len(value) == 8:
        value = value[2:]
    channels = [int(value[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    def linear(c: float) -> float:
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    red, green, blue = (linear(c) for c in channels)
    return 0.2126 * red + 0.7152 * green + 0.0722 * blue


def contrast_ratio(first: str, second: str) -> float:
    a, b = sorted((relative_luminance(first), relative_luminance(second)), reverse=True)
    return (a + 0.05) / (b + 0.05)


def run(args: argparse.Namespace) -> int:
    import win32con
    import win32event
    import win32gui
    import win32process

    exe = args.exe.resolve()
    root = args.isolation_root.resolve()
    out = args.output.resolve()
    if not exe.is_file():
        raise ValueError(f"missing EXE: {exe}")
    if not root.exists():
        raise ValueError(f"missing isolation root: {root}")
    out.mkdir(parents=True, exist_ok=True)
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    library = root / "library"
    names = prepare_library(library, args.audio_source, args.count)

    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    report = {
        "exe": str(exe),
        "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "isolation_root": str(root),
        "library_files": len(names),
        "started_at": datetime.now(timezone.utc).isoformat(),
        "checks": [],
        "screenshots": [],
    }
    process = None
    identity = None
    verified = False

    def command(**payload):
        return driver.command(payload)

    def state():
        return command(cmd="settings_ui")

    def settle():
        first = state()["frame"]
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            value = state()
            if value["frame"] > first + 2:
                return value
            time.sleep(0.04)
        raise AssertionError("no fresh settings frame")

    def item(item_id, snapshot=None):
        snapshot = snapshot or settle()
        found = next((entry for entry in snapshot["items"] if entry["id"] == item_id), None)
        assert found is not None, (item_id, sorted(entry["id"] for entry in snapshot["items"]))
        left, top, right, bottom = found["bounds"]
        clip_left, clip_top, clip_right, clip_bottom = found["clip"]
        assert left >= clip_left - 1 and top >= clip_top - 1, found
        assert right <= clip_right + 1 and bottom <= clip_bottom + 1, found
        return found

    def pointer(x, y):
        ox, oy = win32gui.ClientToScreen(identity["hwnd"], (0, 0))
        for phase in ("hover", "down", "up"):
            command(cmd="theme_schedule", action="pointer", phase=phase,
                    x=x - ox, y=y - oy)
            settle()

    def click(item_id):
        left, top, right, bottom = item(item_id)["bounds"]
        pointer((left + right) / 2, (top + bottom) / 2)

    def key(name):
        for down in (True, False):
            command(cmd="settings_ui", action="key", key=name, down=down)
            settle()

    def query(value):
        click("search")
        command(cmd="settings_ui", action="key", key="Ctrl", down=True)
        settle()
        key("A")
        command(cmd="settings_ui", action="key", key="Ctrl", down=False)
        settle()
        command(cmd="info_edit_input", action="text", text=value)
        settle()

    def open_settings():
        if not state()["open"]:
            command(cmd="ordinary_button_window", module="settings", action="open")
            settle()
        command(cmd="settings_ui", action="resize", width=1120, height=800)
        settle()

    def daw_page(width=1120, height=800, scroll=0):
        command(cmd="settings_ui", action="resize", width=width, height=height)
        settle()
        command(cmd="settings_ui", action="page", page="daw")
        settle()
        command(cmd="settings_ui", action="scroll", y=max(0, scroll))
        return settle()

    def ensure_visible(item_id, width, height, scroll):
        """Scroll the settings body until the control sits fully inside its clip rect."""
        for _ in range(8):
            snapshot = settle()
            found = next(entry for entry in snapshot["items"] if entry["id"] == item_id)
            _, top, _, bottom = found["bounds"]
            _, clip_top, _, clip_bottom = found["clip"]
            if top >= clip_top - 1 and bottom <= clip_bottom + 1:
                return snapshot, scroll
            if bottom > clip_bottom:
                scroll = scroll + (bottom - clip_bottom) + 32
            else:
                scroll = max(0, scroll - (clip_top - top) - 32)
            command(cmd="settings_ui", action="resize", width=width, height=height)
            settle()
            command(cmd="settings_ui", action="scroll", y=scroll)
        raise AssertionError(f"{item_id} never became fully visible at {width}x{height}")

    def set_theme(index):
        click("nav-1")
        settle()
        if state()["pending"]:
            click("revert")
            settle()
        previous = state()["live_theme"]
        click("stock-" + str(index))
        settle()
        if state()["pending"]:
            click("apply")
            settle()
        applied = state()["live_theme"]
        return {"previous": previous, "applied": applied}

    def set_language(index):
        if state()["live_language"] == index:
            return
        query("language")
        click("result-language")
        settle()
        click("language")
        settle()
        click("option-" + str(index))
        settle()
        if state()["pending"]:
            click("apply")
            settle()
        assert state()["live_language"] == index, state()["live_language"]

    def shot(name, snapshot=None):
        snapshot = snapshot or settle()
        path = out / (name + ".png")
        capture(driver, path)
        report["screenshots"].append(str(path))
        if snapshot["open"]:
            left, top, right, bottom = snapshot["bounds"]
            ox, oy = win32gui.ClientToScreen(identity["hwnd"], (0, 0))
            cropped = path.with_name(name + "-settings.png")
            with Image.open(path) as image:
                image.crop((round(left - ox), round(top - oy),
                            round(right - ox), round(bottom - oy))).save(cropped)
            report["screenshots"].append(str(cropped))
        return path

    def screen_rect(bounds):
        ox, oy = win32gui.ClientToScreen(identity["hwnd"], (0, 0))
        left, top, right, bottom = bounds
        return (round(left - ox), round(top - oy), round(right - ox), round(bottom - oy))

    def ink_level(path: Path, bounds):
        """Luminance span inside a control: text ink against the card fill."""
        with Image.open(path) as image:
            crop = image.convert("RGB").crop(screen_rect(bounds))
            values = sorted((0.2126 * r + 0.7152 * g + 0.0722 * b)
                            for r, g, b in crop.getdata())
        return (values[min(len(values) - 1, int(len(values) * 0.99))]
                - values[max(0, int(len(values) * 0.01))])

    def check(label, **details):
        report["checks"].append({"case": label, **details})
        print(label, flush=True)

    try:
        process, pid = desktop.start(exe, environment_overrides={
            "LOCALAPPDATA": str(root / "profile/Local"),
            "APPDATA": str(root / "profile/Roaming"),
            "AURA_RUNTIME_ROOT": str(runtime),
            "TEMP": str(root / "temp"),
            "TMP": str(root / "temp"),
        })
        identity = command(cmd="magic_eq_identity")
        assert identity["pid"] == pid, identity
        assert Path(identity["exe_path"]).resolve() == exe, identity
        assert Path(identity["isolation_root"]).resolve() == root, identity
        verified = True
        startup = wait_state(driver, lambda v: v["main_ui_ready"], "native startup", 90)
        if not startup["db_path"]:
            wait_state(driver, lambda v: v["new_db_dialog_open"], "new library dialog", 20)
            command(cmd="info_edit_input", action="new_db_name", text="DawCardsQA")
            time.sleep(0.15)
            command(cmd="info_edit_input", action="new_db_create")
            wait_state(driver, lambda v: bool(v["db_path"]) and not v["new_db_dialog_open"],
                       "created library", 40)
        if startup["total_count"] < len(names):
            command(cmd="scan_folder", path=str(library), recursive=False,
                    cache_waveforms=False, build_similarity_index=False)
            wait_state(driver, lambda v: v["total_count"] >= len(names),
                       "library scanned", 180)
        library_state = wait_state(driver, lambda v: v["total_count"] > 0, "library ready", 60)
        report["library"] = {"db_path": library_state["db_path"],
                             "total_count": library_state["total_count"]}

        open_settings()
        snapshot = daw_page()
        assert snapshot["page"] == 3, snapshot["page"]
        for card in CARDS:
            card_box = item(card, snapshot)
            name_box = item(card + "-name", snapshot)
            config = item(card + "-config", snapshot) if card == CARDS[0] else item(card + "-config-disabled", snapshot)
            assert name_box["bounds"][2] <= config["bounds"][0] - 1, (card, name_box, config)
            assert config["bounds"][0] >= card_box["bounds"][0], (card, config, card_box)
            assert config["bounds"][2] <= card_box["bounds"][2], (card, config, card_box)
        check("four DAW cards render with the name and the config control inside each card")

        for disabled in DISABLED_CONFIG:
            assert item(disabled, snapshot)["disabled"], disabled
        assert not item("daw-reaper-config", snapshot)["disabled"]
        check("only REAPER's config control is enabled; Pro Tools, Cubase and Nuendo stay disabled")

        assert not any(entry["id"] == "daw-reaper-path" for entry in snapshot["items"]), \
            "the collapsed REAPER card must not show its path row"
        collapsed_next_top = item("daw-pro-tools", snapshot)["bounds"][1]
        collapsed_card_bottom = item("daw-reaper-card", snapshot)["bounds"][3]
        click("daw-reaper-config")
        expanded = settle()
        path_box = item("daw-reaper-path", expanded)
        card_box = item("daw-reaper-card", expanded)
        assert path_box["bounds"][0] >= card_box["bounds"][0], (path_box, card_box)
        assert path_box["bounds"][2] <= card_box["bounds"][2], (path_box, card_box)
        header_box = item("daw-reaper", expanded)
        assert path_box["bounds"][1] >= header_box["bounds"][3] - 1, (path_box, header_box)
        assert path_box["bounds"][3] <= card_box["bounds"][3] + 1, (path_box, card_box)
        expanded_next_top = item("daw-pro-tools", expanded)["bounds"][1]
        assert expanded_next_top > collapsed_next_top, (collapsed_next_top, expanded_next_top)
        click("daw-reaper-config")
        collapsed_again = settle()
        assert not any(entry["id"] == "daw-reaper-path" for entry in collapsed_again["items"]), \
            "clicking Configure again must collapse the REAPER card"
        assert abs(item("daw-reaper-card", collapsed_again)["bounds"][3]
                   - collapsed_card_bottom) <= 1, collapsed_card_bottom
        click("daw-reaper-config")
        settle()
        check("REAPER expands and collapses through its own Configure control, "
              "and the executable path row stays inside the full card",
              next_card_top_collapsed=collapsed_next_top,
              next_card_top_expanded=expanded_next_top)

        for disabled in DISABLED_CONFIG:
            click(disabled)
            after = settle()
            assert after["open"] and after["page"] == 3, after["page"]
            assert any(entry["id"] == "daw-reaper-path" for entry in after["items"]), \
                "the expanded REAPER card must stay expanded"
            popups = [entry for entry in after["color_popups"]
                      if entry["name"] != "###HarkenSettings"]
            assert not popups, popups
        check("clicking a disabled card's config control changes nothing")

        samples = []
        for index in range(5):
            applied = set_theme(index)
            snapshot = daw_page(1120, 800)
            colors = snapshot["rendered_colors"]
            ratio = contrast_ratio(colors["text"], colors["bg"])
            assert ratio >= 4.5, (index, ratio, colors)
            samples.append({"index": index, "applied_theme": applied, "bg": colors["bg"],
                            "text": colors["text"],
                            "background_luminance": relative_luminance(colors["bg"]),
                            "contrast": ratio})
        report["theme_samples"] = samples
        assert len({sample["applied_theme"]["applied"] for sample in samples}) == 5, samples
        light_index = max(range(5), key=lambda i: samples[i]["background_luminance"])
        dark_index = min(range(5), key=lambda i: samples[i]["background_luminance"])
        assert samples[light_index]["background_luminance"] > 0.5, samples
        assert samples[dark_index]["background_luminance"] < 0.2, samples
        check("all five stock themes keep the DAW page readable",
              light=samples[light_index], dark=samples[dark_index])
        for theme, label in ((light_index, "light"), (dark_index, "dark")):
            applied = set_theme(theme)
            for width, height in ((1120, 800), (640, 900)):
                snapshot = daw_page(width, height)
                colors = snapshot["rendered_colors"]
                ratio = contrast_ratio(colors["text"], colors["bg"])
                background = relative_luminance(colors["bg"])
                if label == "light":
                    assert background > 0.5, (colors["bg"], background)
                else:
                    assert background < 0.2, (colors["bg"], background)
                report.setdefault("contrast", []).append(
                    {"theme": label, "width": width, "ratio": round(ratio, 2),
                     "background_luminance": round(background, 3), "applied_theme": applied})
                assert ratio >= 4.5, (label, width, ratio, colors)
                for card in CARDS:
                    card_box = item(card, snapshot)
                    name_box = item(card + "-name", snapshot)
                    left, top, right, bottom = name_box["bounds"]
                    assert left >= card_box["bounds"][0] and right <= card_box["bounds"][2]
                    config_id = card + "-config" if card == CARDS[0] else card + "-config-disabled"
                    assert item(config_id, snapshot)["bounds"][2] <= card_box["bounds"][2]
                shot_path = shot(f"daw-{label}-{width}x{height}", snapshot)
                check(f"{label} theme at {width}x{height}: cards, names and config controls fit",
                      contrast=round(ratio, 2))
                if width == 1120:
                    enabled_ink = ink_level(shot_path, item("daw-reaper-config", snapshot)["bounds"])
                    disabled_ink = [ink_level(shot_path, item(card + "-config-disabled", snapshot)["bounds"])
                                    for card in CARDS[1:]]
                    assert all(ink <= enabled_ink * 0.92 for ink in disabled_ink), \
                        (enabled_ink, disabled_ink)
                    report.setdefault("disabled_ink", []).append(
                        {"theme": label, "enabled": round(enabled_ink, 1),
                         "disabled": [round(ink, 1) for ink in disabled_ink]})
                    check(f"{label} theme: the three unavailable config labels render dimmer than REAPER's")

        for language, code in ((1, "en"), (2, "ja"), (0, "zh")):
            set_language(language)
            snapshot = daw_page(760, 720)
            scroll = 0
            for card in CARDS:
                snapshot, scroll = ensure_visible(card, 760, 720, scroll)
                card_box = item(card, snapshot)
                name_box = item(card + "-name", snapshot)
                config_id = card + "-config" if card == CARDS[0] else card + "-config-disabled"
                assert name_box["bounds"][2] <= item(config_id, snapshot)["bounds"][0] - 1
                assert name_box["bounds"][2] <= card_box["bounds"][2]
            shot(f"daw-narrow-{code}", snapshot)
            check(f"language {code} at 760x720: no card name overlaps its config control",
                  scroll=scroll)

        set_language(0)
        daw_page(1120, 800)
        command(cmd="settings_ui", action="daw_reaper_config", open=False)
        settle()
        if state()["pending"]:
            click("apply")
            settle()
        click("done")
        assert not settle()["open"], "the settings window must close through Done"
        check("the DAW page closes through the normal Done action")
    except Exception as error:  # noqa: BLE001 - the report must keep partial evidence
        report["error"] = f"{type(error).__name__}: {error}"
        report["traceback"] = traceback.format_exc()
        try:
            shot("failure-state")
        except Exception:  # noqa: BLE001
            pass
        raise
    finally:
        if process is not None:
            running = win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT
            if running and verified:
                try:
                    command(cmd="info_edit_input", action="tray_exit")
                except Exception:  # noqa: BLE001
                    pass
                running = win32event.WaitForSingleObject(process, 20000) != win32con.WAIT_OBJECT_0
            if running:
                win32process.TerminateProcess(process, 1)
                win32event.WaitForSingleObject(process, 10000)
                report["exit_failure"] = "exit timed out; process was terminated"
            else:
                exit_code = win32process.GetExitCodeProcess(process)
                if exit_code != 0:
                    report["exit_failure"] = f"process exited with code {exit_code}"
            process.Close()
        desktop.close()
        report["finished_at"] = datetime.now(timezone.utc).isoformat()
        (out / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False),
                                         encoding="utf-8")
        print(json.dumps({key: report[key] for key in ("checks", "error", "exit_failure")
                          if key in report}, ensure_ascii=False))
    return 1 if "exit_failure" in report else 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--isolation-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--audio-source", type=Path, default=None)
    parser.add_argument("--count", type=int, default=24)
    return parser.parse_args()


if __name__ == "__main__":
    raise SystemExit(run(parse_args()))
