"""Save the external AI authorization document through the real settings page.

The isolated preview EXE runs on a private desktop. The script opens Settings,
switches to the External AI (MCP) page, flips the master switch, saves, reads
the document the page wrote, then starts the real connector against the same
library to confirm the document is bound to this library only.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import time

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver

EXTERNAL_AI_PAGE = 9
CAPABILITIES = ("searchAudio", "findSimilar", "findFolders", "readFileName",
                "readMetadata", "editMetadata")


def library_id(database):
    folded = []
    for unit in str(database):
        unit = "/" if unit == "\\" else unit
        folded.append(unit.lower() if "A" <= unit <= "Z" else unit)
    value = 1469598103934665603
    for byte in "".join(folded).encode("utf-8"):
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "lib-%016x" % value


def connector(exe, database, root, settings, identity):
    launch = [str(exe), "--library-db", str(database), "--library-root", str(root),
              "--settings", str(settings), "--library-id", identity]
    process = subprocess.Popen(launch, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    report = {"command": launch[1:]}
    try:
        def exchange(message):
            process.stdin.write(json.dumps(message, ensure_ascii=False).encode("utf-8") + b"\n")
            process.stdin.flush()
            line = process.stdout.readline()
            return json.loads(line.decode("utf-8")) if line else None
        hello = exchange({"jsonrpc": "2.0", "id": "hello", "method": "initialize",
                          "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                                     "clientInfo": {"name": "harken qa", "version": "1"}}})
        report["initialize"] = hello
        report["started"] = hello is not None
        if hello is not None:
            process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
            process.stdin.flush()
            reply = exchange({"jsonrpc": "2.0", "id": "search", "method": "tools/call",
                              "params": {"name": "harken_search_audio",
                                         "arguments": {"query": "tone"}}})
            report["search"] = json.loads(reply["result"]["content"][0]["text"]) if reply else None
        return report
    finally:
        process.stdin.close()
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
        report["stderr"] = process.stderr.read().decode("utf-8", "replace").strip()
        report["exit"] = process.returncode


def run(args):
    import win32event
    import win32gui
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / "profile/runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process = None
    identity = None
    report = {"exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "connector_sha256": None,
              "connector_checks": [],
              "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "checks": []}

    def command(**payload):
        return driver.command(payload)

    def settle():
        first = command(cmd="settings_ui")["frame"]
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            value = command(cmd="settings_ui")
            if value["frame"] > first + 2:
                assert not value.get("imgui_errors"), value.get("imgui_log")
                return value
            time.sleep(0.05)
        raise AssertionError("No fresh settings frame")

    def visible(item):
        a, b, c, d = item["bounds"]
        l, t, r, bot = item["clip"]
        return a >= l - 1 and b >= t - 1 and c <= r + 1 and d <= bot + 1 and c > a and d > b

    def find(item_id):
        found = [x for x in settle()["items"] if x["id"] == item_id and visible(x)]
        assert len(found) == 1, (item_id, found)
        return found[0]

    def pointer(x, y):
        ox, oy = win32gui.ClientToScreen(identity["hwnd"], (0, 0))
        for phase in ("hover", "down", "up"):
            command(cmd="theme_schedule", action="pointer", phase=phase, x=x - ox, y=y - oy)
            settle()

    def click(item_id, fraction=0.5):
        a, b, c, d = find(item_id)["bounds"]
        pointer(a + (c - a) * fraction, (b + d) / 2)
        settle()

    def screenshot(name):
        from PIL import Image
        observed = settle()
        path = out / (name + ".png")
        capture(driver, path)
        ox, oy = win32gui.ClientToScreen(identity["hwnd"], (0, 0))
        a, b, c, d = observed["bounds"]
        box = (round(a - ox), round(b - oy), round(c - ox), round(d - oy))
        with Image.open(path) as source:
            source.crop(box).save(out / (name + "-crop.png"))
        report.setdefault("screenshots", []).append({"file": path.name, "crop": name + "-crop.png",
                                                     "frame": observed["frame"]})

    def check(name, **details):
        report["checks"].append(dict(case=name, **details))

    def document_path():
        found = list(root.rglob("external-ai/*.json"))
        assert len(found) == 1, [str(p) for p in found]
        return found[0]

    def set_enabled(expected, attempts=3):
        history = []
        for attempt in range(1, attempts + 1):
            click("ext-ai-enabled")
            time.sleep(0.2)
            settle()
            click("ext-ai-save")
            time.sleep(0.45)
            settle()
            saved = json.loads(document_path().read_text(encoding="utf-8"))
            history.append({"attempt": attempt, "enabled": saved["enabled"],
                            "mtime": document_path().stat().st_mtime})
            if saved["enabled"] is expected:
                return saved, history
        raise AssertionError({"expected": expected, "history": history})

    try:
        process, pid = desktop.start(exe)
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        assert identity["pid"] == pid and Path(identity["exe_path"]).resolve() == exe, identity
        assert Path(identity["isolation_root"]).resolve() == root, identity
        report["identity"] = identity
        wait_state(driver, lambda s: s["main_ui_ready"], "isolated startup", timeout=90)
        state = command(cmd="get_state")
        if state["startup_needs_database_setup"]:
            command(cmd="info_edit_input", action="new_db_name", text="ExtAiQA-" + str(pid))
            command(cmd="info_edit_input", action="new_db_create")
            state = wait_state(driver, lambda s: bool(s["db_path"]) and not s["db_switch_in_progress"]
                               and not s["new_db_dialog_open"], "created isolated library")
        database = Path(state["db_path"])
        assert database.is_relative_to(root), (database, root)
        report["library"] = {"path": str(database), "id": library_id(database)}
        check("isolated-library", path=str(database), identity=library_id(database))

        command(cmd="ordinary_button_window", module="settings", action="open")
        observed = settle()
        assert observed["open"], observed
        click("nav-%d" % EXTERNAL_AI_PAGE)
        observed = settle()
        assert observed["page"] == EXTERNAL_AI_PAGE, observed["page"]
        check("external-ai-page", page=observed["page"],
              items=[i["id"] for i in observed["items"] if i["id"].startswith("ext-ai")])
        screenshot("01-external-ai-page")

        command(cmd="settings_ui", action="resize", width=1120, height=1080)
        settle()
        save_item = find("ext-ai-save")
        check("save-button-visible", bounds=save_item["bounds"], clip=save_item["clip"])
        screenshot("02-save-visible")

        document = document_path() if any(root.rglob("external-ai/*.json")) else None
        enabled_document = json.loads(document.read_text(encoding="utf-8")) if document else None
        if enabled_document is not None:
            check("pre-existing-document", document=enabled_document)
        saved, history = set_enabled(True)
        assert saved["libraryId"] == library_id(database), (saved["libraryId"], library_id(database))
        assert saved["roots"] == [], saved
        assert set(saved["capabilities"]) == set(CAPABILITIES), saved
        assert all(value == "none" for value in saved["capabilities"].values()), saved
        check("saved-document-enabled", document=saved, attempts=history)
        screenshot("03-saved-enabled")
        (out / "authorization-enabled-empty-roots.json").write_text(
            json.dumps(saved, ensure_ascii=False), encoding="utf-8")

        disabled, history = set_enabled(False)
        assert disabled["libraryId"] == saved["libraryId"], disabled
        check("saved-document-disabled", document=disabled, attempts=history)
        screenshot("04-saved-disabled")
        report["document_path"] = str(document_path())

        connector_exe = exe.parent / "HarkenExternalAiMcp.exe"
        report["connector_sha256"] = hashlib.sha256(connector_exe.read_bytes()).hexdigest()
        root_folder = database.parent
        disabled_run = connector(connector_exe, database, root_folder, Path(report["document_path"]),
                                 library_id(database))
        disabled_run["case"] = "disabled-master-switch"
        report["connector_checks"].append(disabled_run)
        assert disabled_run["started"] and disabled_run["search"]["status"] == "not_authorized", disabled_run
        enabled_run = connector(connector_exe, database, root_folder,
                                out / "authorization-enabled-empty-roots.json", library_id(database))
        enabled_run["case"] = "enabled-without-ticked-folders"
        report["connector_checks"].append(enabled_run)
        assert enabled_run["started"] and enabled_run["search"]["status"] == "not_authorized", enabled_run
        foreign = json.loads((out / "authorization-enabled-empty-roots.json").read_text(encoding="utf-8"))
        foreign["libraryId"] = library_id(database.parent / "another-library.db")
        (out / "authorization-foreign-library.json").write_text(
            json.dumps(foreign, ensure_ascii=False), encoding="utf-8")
        foreign_run = connector(connector_exe, database, root_folder,
                                out / "authorization-foreign-library.json", library_id(database))
        foreign_run["case"] = "document-bound-to-another-library"
        report["connector_checks"].append(foreign_run)
        assert not foreign_run["started"], foreign_run
        assert "another library" in foreign_run["stderr"], foreign_run
    finally:
        if process is not None:
            try:
                command(cmd="settings_ui", action="key", key="Escape", down=True)
                command(cmd="settings_ui", action="key", key="Escape", down=False)
                time.sleep(0.3)
                command(cmd="info_edit_input", action="exit")
                if win32event.WaitForSingleObject(process, 20000) != win32event.WAIT_OBJECT_0:
                    win32gui.PostMessage(identity["hwnd"], 0x0010, 0, 0)
                    win32event.WaitForSingleObject(process, 10000)
                report["app_exit"] = win32event.WaitForSingleObject(process, 1000)
            except Exception as error:
                report["app_exit_error"] = repr(error)
            finally:
                desktop.close()
                process.Close()

    report["passed"] = True
    (out / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--isolation-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    run(parser.parse_args())


if __name__ == "__main__":
    main()
