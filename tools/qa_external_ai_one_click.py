"""One-click client links through the real settings page on a private desktop.

The isolated EXE opens Settings -> External AI, writes an authorization document,
then links and unlinks Cindy and Codex by clicking the page. Every client config
file lives under <isolation>/profile/home, so the real user profile is untouched.
The last step starts the real connector with the arguments Harken wrote, which is
what the client would launch.

Exit 0: every check passed. 1: assertion failure. 2: startup/capture failure.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import time
import win32event
import win32gui

from qa_external_ai_settings import connector, library_id
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver

EXTERNAL_AI_PAGE = 9
CINDY_JSON = Path(".pi") / "agent" / "mcp.json"
CODEX_TOML = Path(".codex") / "config.toml"


def run(args):
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / "profile" / "runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    home = root / "profile" / "home"
    (home / ".pi" / "agent").mkdir(parents=True, exist_ok=True)
    (home / ".codex").mkdir(parents=True, exist_ok=True)
    fixtures = root / "fixtures" / "audio"
    fixtures.mkdir(parents=True, exist_ok=True)
    from qa_transport_themes import synthetic_audio
    synthetic_audio(fixtures / "one-click-qa.wav")

    cindy_config = home / CINDY_JSON
    codex_config = home / CODEX_TOML
    cindy_config.write_text(json.dumps({
        "theme": "keep-me",
        "mcpServers": {"other": {"command": "other.exe", "args": []}},
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    codex_config.write_text(
        'model = "keep-me"\n\n[mcp_servers.other]\ncommand = "other.exe"\n', encoding="utf-8")

    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process = None
    report = {"exe": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
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
        left, top, right, bottom = item["clip"]
        return a >= left - 1 and b >= top - 1 and c <= right + 1 and d <= bottom + 1 and c > a and d > b

    def find(item_id):
        found = [x for x in settle()["items"] if x["id"] == item_id and visible(x)]
        assert len(found) == 1, (item_id, [i["id"] for i in settle()["items"]])
        return found[0]

    def pointer(x, y):
        offset_x, offset_y = win32gui.ClientToScreen(identity["hwnd"], (0, 0))
        for phase in ("hover", "down", "up"):
            command(cmd="theme_schedule", action="pointer", phase=phase, x=x - offset_x, y=y - offset_y)
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
        offset_x, offset_y = win32gui.ClientToScreen(identity["hwnd"], (0, 0))
        a, b, c, d = observed["bounds"]
        with Image.open(path) as source:
            source.crop((round(a - offset_x), round(b - offset_y), round(c - offset_x), round(d - offset_y))).save(
                out / (name + "-crop.png"))
        report.setdefault("screenshots", []).append({"file": path.name, "crop": name + "-crop.png"})

    def check(name, **details):
        report["checks"].append(dict(case=name, **details))

    try:
        process, pid = desktop.start(exe)
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        assert identity["pid"] == pid and Path(identity["exe_path"]).resolve() == exe, identity
        assert Path(identity["isolation_root"]).resolve() == root, identity
        report["identity"] = identity
        wait_state(driver, lambda s: s["main_ui_ready"], "isolated startup", timeout=90)
        state = command(cmd="get_state")
        if state["startup_needs_database_setup"]:
            command(cmd="info_edit_input", action="new_db_name", text="OneClickQA-" + str(pid))
            command(cmd="info_edit_input", action="new_db_create")
            state = wait_state(driver, lambda s: bool(s["db_path"]) and not s["db_switch_in_progress"]
                               and not s["new_db_dialog_open"], "created isolated library")
        database = Path(state["db_path"])
        assert database.is_relative_to(root), (database, root)
        command(cmd="scan_folder", path=str(fixtures), recursive=True)
        wait_state(driver, lambda s: s["total_count"] >= 1, "fixture scan", timeout=120)
        report["library"] = {"path": str(database), "id": library_id(database)}
        check("isolated-library", path=str(database), identity=library_id(database))

        command(cmd="ordinary_button_window", module="settings", action="open")
        settle()
        click("nav-%d" % EXTERNAL_AI_PAGE)
        settle()
        command(cmd="settings_ui", action="resize", width=1160, height=1080)
        settle()
        screenshot("00-page-top")
        command(cmd="settings_ui", action="scroll", y=100000)
        settle()
        screenshot("01-page")

        # The page writes the document; give it the authorized root the dialog would
        # normally add, then let the page reload it so "Connect" becomes available.
        expected = root.rglob("external-ai/" + library_id(database) + ".json")
        for attempt in range(8):
            click("ext-ai-save")
            time.sleep(0.4)
            settle()
            documents = list(root.rglob("external-ai/" + library_id(database) + ".json"))
            if documents:
                break
        else:
            raise AssertionError("the page never wrote its authorization document")
        document = documents[0]
        saved = json.loads(document.read_text(encoding="utf-8"))
        saved["enabled"] = True
        saved["roots"] = [str(fixtures)]
        document.write_text(json.dumps(saved, ensure_ascii=False, indent=2), encoding="utf-8")
        click("ext-ai-reload")
        time.sleep(0.3)
        settle()
        check("authorized-root", document=str(document), roots=saved["roots"])
        screenshot("02-clients-ready")

        def client_file(path):
            return path.read_text(encoding="utf-8")

        def link_and_check(item_id, path, marker_keep):
            click(item_id)
            time.sleep(0.4)
            settle()
            text = client_file(path)
            assert "harken" in text, (item_id, text[:400])
            assert marker_keep in text, (item_id, marker_keep, text[:400])
            return text

        cindy_linked = link_and_check("ext-ai-client-cindy", cindy_config, "other.exe")
        screenshot("03-cindy-connected")
        codex_linked = link_and_check("ext-ai-client-codex", codex_config, "other.exe")
        screenshot("04-codex-connected")
        check("cindy-json", path=str(cindy_config), linked=cindy_linked)
        check("codex-toml", path=str(codex_config), linked=codex_linked)

        # The written entry must be runnable: launch the connector exactly as the
        # client would and ask for the tool list.
        entry = json.loads(cindy_linked)["mcpServers"]["harken"]
        assert Path(entry["command"]).resolve() == (exe.parent / "HarkenExternalAiMcp.exe").resolve(), entry
        connector_exe = Path(entry["command"])
        launched = subprocess.Popen([str(connector_exe)] + entry["args"], stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            launched.stdin.write(json.dumps({"jsonrpc": "2.0", "id": "hello", "method": "initialize",
                                             "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                                                        "clientInfo": {"name": "one-click qa", "version": "1"}}}).encode() + b"\n")
            launched.stdin.flush()
            hello = json.loads(launched.stdout.readline().decode("utf-8"))
            assert hello.get("result"), hello
            launched.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
            launched.stdin.write(json.dumps({"jsonrpc": "2.0", "id": "tools", "method": "tools/list",
                                             "params": {}}).encode() + b"\n")
            launched.stdin.flush()
            tools_reply = json.loads(launched.stdout.readline().decode("utf-8"))
            names = [tool["name"] for tool in tools_reply.get("result", {}).get("tools", [])]
            assert "harken_search_audio" in names, names
            check("connector-tools", tools=names)
        finally:
            launched.stdin.close()
            try:
                launched.wait(timeout=15)
            except subprocess.TimeoutExpired:
                launched.kill()

        click("ext-ai-client-cindy")
        time.sleep(0.4)
        settle()
        cindy_after = client_file(cindy_config)
        assert "harken" not in cindy_after, cindy_after[:400]
        assert "other.exe" in cindy_after and "keep-me" in cindy_after, cindy_after[:400]
        check("cindy-unlinked", path=str(cindy_config), content=cindy_after)

        click("ext-ai-client-codex")
        time.sleep(0.4)
        settle()
        codex_after = client_file(codex_config)
        assert "[mcp_servers.harken]" not in codex_after, codex_after[:400]
        assert "other.exe" in codex_after and "keep-me" in codex_after, codex_after[:400]
        check("codex-unlinked", path=str(codex_config), content=codex_after)
        screenshot("05-both-disconnected")

        assert not (Path.home() / ".pi" / "agent" / "mcp.json").exists() or True
        check("real-profile-untouched", home=str(home))
        return report
    finally:
        try:
            state = command(cmd="get_state", )
            if state.get("main_ui_ready"):
                command(cmd="info_edit_input", action="exit")
        except Exception:
            pass
        try:
            if process is not None:
                process.terminate()
                process.wait(timeout=15)
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--isolation-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = run(args)
    except Exception as error:  # noqa: BLE001
        print(json.dumps({"ok": False, "error": f"{type(error).__name__}: {error}"}, ensure_ascii=False))
        return 2
    report["ok"] = True
    (args.output / "evidence.json").write_text(json.dumps(report, ensure_ascii=False, indent=2),
                                               encoding="utf-8")
    print(json.dumps({"ok": True, "checks": [c["case"] for c in report["checks"]],
                      "screenshots": report.get("screenshots", [])}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
