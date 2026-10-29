"""Check folder and keyword searches in an isolated native package."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time
import wave

from qa_folder_shortcuts import Driver


def wav(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as sound:
        sound.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        sound.writeframes(bytes(8000 * 2))


def run(exe, root, output, mode):
    import win32con
    import win32gui
    from PIL import ImageGrab

    exe, root, output = exe.resolve(), root.resolve(), output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for folder, name in (("A", "Bird_A.wav"), ("A", "Bell_A.wav"), ("B", "Bird_B.wav")):
        wav(root / "library" / folder / name)
    settings = root / "profile/Local/HarkenSearch/settings.ini"
    settings.parent.mkdir(parents=True, exist_ok=True)
    database = root / "profile/Local/HarkenSearch/Database/FolderSearch/main.db"
    database.parent.mkdir(parents=True, exist_ok=True)
    sqlite3.connect(database).close()
    settings.write_text("viewMode=1\nshowWelcomeOnStartup=0\ncloseButtonAction=2\n"
                        + "searchRealtime=" + ("1" if mode == "live" else "0") + "\n"
                        + "currentDbPath=" + str(database) + "\n", encoding="utf-8")
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, output)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = win32con.SW_SHOWNOACTIVATE
    process = subprocess.Popen([str(exe)], cwd=exe.parent, startupinfo=startup, env=dict(
        os.environ, AURA_HARNESS_MULTI_INSTANCE="1", AURA_HARNESS_LANE_KIND="folder-search",
        AURA_DISABLE_SINGLE_INSTANCE="1"))
    hwnd = None
    cases = []

    def settled():
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if driver.command({"cmd": "wait_ready"})["ready"]:
                return driver.command({"cmd": "get_state"})
            time.sleep(.1)
        raise TimeoutError("search did not settle")

    def check(name, folder, query, expected):
        deadline = time.monotonic() + 25
        while True:
            state = settled()
            rows = driver.command({"cmd": "get_results", "offset": 0, "limit": 100})["items"]
            names = sorted(row["name"] for row in rows)
            if (state["folder_filter"]["active"] == bool(folder) and
                    state["search_text"] == query and names == sorted(expected)):
                break
            if time.monotonic() > deadline:
                raise AssertionError((name, state["folder_filter"], state["search_text"], names, expected))
            time.sleep(.1)
        assert state["folder_filter"]["active"] == bool(folder), (name, state)
        assert state["search_text"] == query, (name, state)
        win32gui.ShowWindow(hwnd, win32con.SW_SHOWNOACTIVATE)
        image = ImageGrab.grab(window=hwnd)
        assert len(image.getcolors(image.width * image.height) or []) > 16
        shot = output / (name + ".png")
        image.save(shot)
        cases.append(dict(name=name, folder=folder, query=query, names=names,
                          state=state, screenshot=str(shot)))

    def search(query):
        driver.command({"cmd": "search", "query": query})
        if mode == "enter":
            driver.command({"cmd": "search_enter_key", "phase": "down"})
            time.sleep(.08)
            driver.command({"cmd": "search_enter_key", "phase": "up"})
        time.sleep(.3)

    def folder(path):
        driver.command({"cmd": "queue_folder_tree_selection", "path": path})
        time.sleep(.25)

    try:
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        assert identity["pid"] == process.pid
        assert Path(identity["exe_path"]).resolve() == exe
        assert Path(identity["isolation_root"]).resolve() == root
        hwnd = identity["hwnd"]
        win32gui.SetWindowPos(hwnd, 0, 40, 40, 1400, 900,
                             win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE)
        deadline = time.monotonic() + 30
        while not driver.command({"cmd": "wait_ready"})["main_ui_ready"]:
            assert time.monotonic() < deadline
            time.sleep(.1)
        if not (root / "library-ready.txt").exists():
            driver.command({"cmd": "scan_folder", "path": str(root / "library"),
                            "recursive": True, "scan_strategy": "fast"})
            deadline = time.monotonic() + 45
            while driver.command({"cmd": "scan_status"})["scanning"]:
                assert time.monotonic() < deadline
                time.sleep(.1)
            scan = driver.command({"cmd": "scan_status"})
            assert scan["errors"] == 0, scan
            (root / "library-ready.txt").write_text("Scan completed in isolated profile", encoding="utf-8")
        driver.command({"cmd": "set_view_mode", "mode": "database"})
        a, b = str(root / "library/A"), str(root / "library/B")
        folder(a)
        search("Bird")
        check(mode + "-folder-then-keyword", a, "Bird", ["Bird_A.wav"])
        driver.command({"cmd": "clear_folder_filter"})
        search("")
        search("Bird")
        folder(a)
        check(mode + "-keyword-then-folder", a, "Bird", ["Bird_A.wav"])
        folder(b)
        check(mode + "-switch-folder", b, "Bird", ["Bird_B.wav"])
        search("Bell")
        check(mode + "-switch-keyword", b, "Bell", [])
        driver.command({"cmd": "clear_folder_filter"})
        check(mode + "-clear-folder-only", "", "Bell", ["Bell_A.wav"])
    finally:
        if hwnd and win32gui.IsWindow(hwnd):
            win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=10)
    report = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  source="RenderUI keyword commit and deferred folder-tree selection; native ImGui Enter event",
                  mode=mode,
                  cases=cases)
    (output / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("PASS: folder and keyword order, switching, and separate clear")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--isolation-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("live", "enter"), required=True)
    args = parser.parse_args()
    run(args.exe, args.isolation_root, args.output, args.mode)
