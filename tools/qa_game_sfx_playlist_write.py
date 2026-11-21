"""End-to-end: connector asks Harken to write a Playlist, Harken writes it.

Runs the isolated EXE on a private desktop (it owns the database and starts the
local write bridge), then drives the real connector over stdio: preview -> apply ->
repeat apply -> revoke -> apply. Verifies the result by reading the isolated
library database directly, so nothing about the claim depends on the app's own UI.
"""
from pathlib import Path
import argparse
import hashlib
import json
import sqlite3
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_external_ai_settings import library_id  # noqa: E402
from qa_required_library_setup import PrivateDesktop, wait_state  # noqa: E402
from qa_transport_themes import Driver, synthetic_audio  # noqa: E402

CAPABILITIES = {
    "searchAudio": "read", "findSimilar": "read", "findFolders": "read",
    "readFileName": "read", "readMetadata": "read", "editMetadata": "none",
    "aiNamingMetadata": "none", "readPlaylist": "read", "editPlaylist": "edit",
}
TOKEN = "e2e0" * 16


def write_authorization(path, library, root, edit_playlist="edit"):
    capabilities = dict(CAPABILITIES, editPlaylist=edit_playlist)
    document = {
        "libraryId": library,
        "enabled": True,
        "sampleLibrary": False,
        "roots": [{"path": str(root), "enabled": True}],
        "bridgeToken": TOKEN,
        "capabilities": capabilities,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, ensure_ascii=False, indent=2), encoding="utf-8")


class Connector:
    def __init__(self, exe, database, root, settings, library):
        self.process = subprocess.Popen(
            [str(exe), "--library-db", str(database), "--library-root", str(root),
             "--settings", str(settings), "--library-id", library],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def call(self, message_id, method, params):
        payload = {"jsonrpc": "2.0", "id": message_id, "method": method, "params": params}
        self.process.stdin.write(json.dumps(payload).encode("utf-8") + b"\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            return None
        return json.loads(line.decode("utf-8"))

    def tool(self, message_id, name, arguments):
        reply = self.call(message_id, "tools/call", {"name": name, "arguments": arguments})
        if reply is None:
            raise AssertionError(name + " -> no reply; connector exit=" +
                                 str(self.process.poll()) + " stderr=" +
                                 self.process.stderr.read().decode("utf-8", "replace")[:300])
        if "result" not in reply:
            raise AssertionError(name + " -> " + json.dumps(reply, ensure_ascii=False)[:500])
        return json.loads(reply["result"]["content"][0]["text"])

    def close(self):
        try:
            self.process.stdin.close()
            self.process.wait(timeout=15)
        except Exception:  # noqa: BLE001
            self.process.kill()


def playlists(database):
    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    try:
        rows = connection.execute(
            "SELECT p.id, p.name, p.parent_id, p.is_folder, "
            "(SELECT COUNT(*) FROM playlist_items i WHERE i.playlist_id = p.id) "
            "FROM playlists p ORDER BY p.id").fetchall()
        return rows
    finally:
        connection.close()


def run(args):
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    connector_exe = exe.parent / "HarkenExternalAiMcp.exe"
    assert connector_exe.is_file(), connector_exe
    runtime = root / "profile" / "runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    fixtures = root / "fixtures" / "audio"
    fixtures.mkdir(parents=True, exist_ok=True)
    for index in range(4):
        synthetic_audio(fixtures / f"sfx-{index + 1}.wav")

    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    process = None
    report = {"exe": str(exe), "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "connector_sha256": hashlib.sha256(connector_exe.read_bytes()).hexdigest(),
              "checks": [], "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}

    def check(name, **details):
        report["checks"].append(dict(case=name, **details))

    try:
        process, pid = desktop.start(exe)
        identity = driver.command({"cmd": "magic_eq_identity"}, timeout=60)
        assert identity["pid"] == pid, identity
        assert Path(identity["isolation_root"]).resolve() == root, identity
        wait_state(driver, lambda s: s["main_ui_ready"], "isolated startup", timeout=90)
        state = driver.command({"cmd": "get_state"})
        if state["startup_needs_database_setup"]:
            driver.command({"cmd": "info_edit_input", "action": "new_db_name", "text": "E2E-" + str(pid)})
            driver.command({"cmd": "info_edit_input", "action": "new_db_create"})
            state = wait_state(driver, lambda s: bool(s["db_path"]) and not s["db_switch_in_progress"]
                               and not s["new_db_dialog_open"], "created isolated library")
        database = Path(state["db_path"])
        assert database.is_relative_to(root), (database, root)
        if state["total_count"] < 4:
            driver.command({"cmd": "scan_folder", "path": str(fixtures), "recursive": True})
            state = wait_state(driver, lambda s: s["total_count"] >= 4, "scan fixtures", timeout=120)
        identity_id = library_id(database)

        settings_directory = database.parents[2] if database.parents else root
        # The app keeps its settings next to its profile; the document path is
        # derived the same way the settings page derives it.
        document = None
        for candidate in root.rglob("external-ai"):
            document = candidate
            break
        if document is None:
            document = runtime / "external-ai"
        document = document / f"{identity_id}.json"
        write_authorization(document, identity_id, fixtures)
        # Give the app up to 5 seconds (it reconciles the bridge twice a second).
        time.sleep(5)
        report["authorization"] = str(document)
        check("authorization-written", path=str(document))

        connector = Connector(connector_exe, database, fixtures, document, identity_id)
        try:
            hello = connector.call(1, "initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                                     "clientInfo": {"name": "e2e", "version": "1"}})
            assert "result" in hello, json.dumps(hello, ensure_ascii=False)[:400]
            connector.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
            connector.process.stdin.flush()

            before = playlists(database)
            preview = connector.tool(2, "harken_preview_playlist_pack",
                                     {"domain": "game_sfx", "playlist": "Game SFX", "query": "sfx", "limit": 10})
            assert preview.get("confirmation") == "required", preview
            after_preview = playlists(database)
            check("preview-writes-nothing", before=before, after=after_preview)
            assert before == after_preview, (before, after_preview)

            applied = connector.tool(3, "harken_apply_playlist_pack",
                                     {"proposal_id": preview["proposal_id"], "confirm": True})
            check("apply", result=applied)
            assert applied.get("status") == "ok", applied
            after_apply = playlists(database)
            report["playlists_after_apply"] = after_apply
            assert any(row[4] > 0 for row in after_apply), after_apply

            again = connector.tool(4, "harken_apply_playlist_pack",
                                   {"proposal_id": preview["proposal_id"], "confirm": True})
            check("apply-repeated", result=again)
            after_repeat = playlists(database)
            assert after_repeat == after_apply, (after_apply, after_repeat)
            assert again.get("status") in ("ok", "proposal_not_found"), again

            write_authorization(document, identity_id, fixtures, edit_playlist="none")
            time.sleep(3)
            fresh_preview = connector.tool(5, "harken_preview_playlist_pack",
                                           {"domain": "game_sfx", "playlist": "Game SFX", "query": "sfx", "limit": 10})
            refused = connector.tool(6, "harken_apply_playlist_pack",
                                     {"proposal_id": fresh_preview["proposal_id"], "confirm": True})
            check("apply-after-revoke", result=refused)
            assert refused.get("status") in ("not_authorized", "playlist_write_unavailable"), refused
            assert playlists(database) == after_apply, "revoked request must not change playlists"
        finally:
            connector.close()
        return report
    finally:
        try:
            driver.command({"cmd": "info_edit_input", "action": "exit"}, timeout=10)
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
                      "playlists": report.get("playlists_after_apply")}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
