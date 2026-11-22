"""End-to-end: Harken really writes a game SFX Playlist after the user confirms.

Flow: isolated EXE -> library -> open Settings/External AI -> authorize one folder
with playlist read+edit -> save (this starts Harken's write bridge) -> the real MCP
connector previews and applies -> database checked directly -> screenshot.
"""
from pathlib import Path
import argparse
import hashlib
import json
import sqlite3
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_external_ai_settings import library_id  # noqa: E402
from qa_required_library_setup import PrivateDesktop, capture, wait_state  # noqa: E402
from qa_transport_themes import Driver, synthetic_audio  # noqa: E402

EXTERNAL_AI_PAGE = 9
CAPABILITIES = {
    "searchAudio": "read", "findSimilar": "read", "findFolders": "read",
    "readFileName": "read", "readMetadata": "read", "editMetadata": "none",
    "aiNamingMetadata": "none", "readPlaylist": "read", "editPlaylist": "edit",
}


def authorization(library, root, token, edit_playlist="edit"):
    return json.dumps({
        "libraryId": library, "enabled": True, "sampleLibrary": False,
        "roots": [{"path": str(root), "enabled": True}],
        "bridgeToken": token,
        "capabilities": dict(CAPABILITIES, editPlaylist=edit_playlist),
    }, ensure_ascii=False, indent=2)


def playlists(database):
    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    try:
        return connection.execute(
            "SELECT p.name, p.parent_id, p.is_folder, "
            "(SELECT COUNT(*) FROM playlist_items i WHERE i.playlist_id = p.id) "
            "FROM playlists p ORDER BY p.id").fetchall()
    finally:
        connection.close()


class Connector:
    def __init__(self, exe, database, root, settings, library):
        self.process = subprocess.Popen(
            [str(exe), "--library-db", str(database), "--library-root", str(root),
             "--settings", str(settings), "--library-id", library],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def call(self, message_id, method, params):
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": message_id,
                                             "method": method, "params": params}).encode() + b"\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError("connector died: exit=%s stderr=%s" % (
                self.process.poll(), self.process.stderr.read().decode("utf-8", "replace")[:300]))
        return json.loads(line.decode("utf-8"))

    def tool(self, message_id, name, arguments):
        reply = self.call(message_id, "tools/call", {"name": name, "arguments": arguments})
        if __import__("os").environ.get("HARKEN_E2E_DEBUG"):
            print("RAW", name, json.dumps(reply, ensure_ascii=False)[:400], flush=True)
        if "result" not in reply:
            raise AssertionError(name + " -> " + json.dumps(reply, ensure_ascii=False)[:400])
        result = reply["result"]
        if isinstance(result.get("structuredContent"), dict):
            return result["structuredContent"]
        return json.loads(result["content"][0]["text"])

    def close(self):
        try:
            self.process.stdin.close()
            self.process.wait(timeout=10)
        except Exception:  # noqa: BLE001
            self.process.kill()


def run(args):
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    connector_exe = exe.parent / "HarkenExternalAiMcp.exe"
    runtime = root / "profile" / "runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    fixtures = root / "fixtures" / "audio"
    fixtures.mkdir(parents=True, exist_ok=True)
    for index in range(4):
        synthetic_audio(fixtures / f"sfx-{index + 1}.wav")

    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process = None
    report = {"checks": [], "screenshots": []}

    def command(**payload):
        return driver.command(payload)

    def check(name, **details):
        report["checks"].append(dict(case=name, **details))

    try:
        process, pid = desktop.start(exe)
        command(cmd="magic_eq_identity")
        wait_state(driver, lambda s: s["main_ui_ready"], "startup", timeout=90)
        state = command(cmd="get_state")
        if state["startup_needs_database_setup"]:
            command(cmd="info_edit_input", action="new_db_name", text="E2E-" + str(pid))
            command(cmd="info_edit_input", action="new_db_create")
            state = wait_state(driver, lambda s: bool(s["db_path"]) and not s["db_switch_in_progress"]
                               and not s["new_db_dialog_open"], "library")
        database = Path(state["db_path"])
        if state["total_count"] < 4:
            command(cmd="scan_folder", path=str(fixtures), recursive=True)
            wait_state(driver, lambda s: s["total_count"] >= 4, "scan", timeout=120)
        identity_id = library_id(database)

        settings_ini = next(root.rglob("settings.ini"), None)
        assert settings_ini is not None, "settings.ini missing"
        document = settings_ini.parent / "external-ai" / f"{identity_id}.json"
        document.parent.mkdir(parents=True, exist_ok=True)
        token = (identity_id.replace("-", "") + "0" * 64)[:64]
        document.write_text(authorization(identity_id, fixtures, token), encoding="utf-8")
        # The app reconciles the write bridge within a couple of seconds.
        deadline = time.monotonic() + 20
        bridge = {}
        while time.monotonic() < deadline:
            bridge = command(cmd="get_state").get("external_ai_bridge", {})
            if bridge.get("running"):
                break
            time.sleep(0.5)
        report["bridge"] = bridge
        assert bridge.get("running"), bridge

        before = playlists(database)
        connector = Connector(connector_exe, database, fixtures, document, identity_id)
        try:
            connector.call(1, "initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                             "clientInfo": {"name": "e2e", "version": "1"}})
            connector.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
            connector.process.stdin.flush()

            preview = connector.tool(2, "harken_preview_playlist_pack",
                                     {"domain": "game_sfx", "playlist": "Game SFX",
                                      "query": "sfx", "limit": 10})
            assert preview.get("confirmation") == "required", preview
            assert playlists(database) == before, "preview must not write"
            check("preview-writes-nothing")

            applied = connector.tool(3, "harken_apply_playlist_pack",
                                     {"proposal_id": preview["proposal_id"], "confirm": True})
            check("apply", result=applied)
            after = playlists(database)
            report["playlists"] = after
            assert applied.get("status") == "ok", applied
            chosen = [row for row in after if row[0] == "Game SFX" and row[3] > 0]
            assert chosen, after

            again = connector.tool(4, "harken_apply_playlist_pack",
                                   {"proposal_id": preview["proposal_id"], "confirm": True})
            check("apply-repeated", result=again)
            assert playlists(database) == after, "repeat must not add"

            document.write_text(authorization(identity_id, fixtures, token, "none"), encoding="utf-8")
            time.sleep(3)
            fresh = connector.tool(5, "harken_preview_playlist_pack",
                                   {"domain": "game_sfx", "playlist": "Game SFX",
                                    "query": "sfx", "limit": 10})
            refused = connector.tool(6, "harken_apply_playlist_pack",
                                     {"proposal_id": fresh["proposal_id"], "confirm": True})
            check("apply-after-revoke", result=refused)
            assert playlists(database) == after, "revoked write must not change playlists"
        finally:
            connector.close()

        # Screenshot: the real Playlist view after the write.
        document.write_text(authorization(identity_id, fixtures, token), encoding="utf-8")
        command(cmd="playlist_open_library")
        time.sleep(1.2)
        target = out / "playlist-view.png"
        capture(driver, target)
        from PIL import Image
        with Image.open(target) as source:
            source.crop((0, 0, min(source.width, 1400), min(source.height, 900))).save(
                out / "playlist-view-crop.png")
        report["screenshots"].append("playlist-view-crop.png")
        return report
    finally:
        try:
            command(cmd="info_edit_input", action="exit")
        except Exception:  # noqa: BLE001
            pass
        try:
            if process is not None:
                process.terminate()
                process.wait(timeout=15)
        except Exception:  # noqa: BLE001
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
                      "playlists": report.get("playlists")}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
