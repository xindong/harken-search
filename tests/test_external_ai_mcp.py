import hashlib
import json
import os
import shutil
from contextlib import ExitStack, closing, contextmanager
from pathlib import Path
from queue import Queue
import sqlite3
import subprocess
import sys
import tempfile
from threading import Thread
import unittest


EXECUTABLE = Path(sys.argv.pop(1)).resolve()
LINK_FIXTURE = None
if len(sys.argv) > 1 and sys.argv[1].startswith("--link-fixture="):
    LINK_FIXTURE = Path(sys.argv.pop(1).split("=", 1)[1]).resolve()
VERSIONS = ("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25")
SEARCH = "harken_search_audio"
FOLDERS = "harken_find_folders"
SIMILAR = "harken_find_similar"
METADATA = "harken_get_metadata"
PREVIEW_METADATA = "harken_preview_metadata_update"
APPLY_METADATA = "harken_apply_metadata_update"
PREVIEW_PLAYLIST = "harken_preview_playlist_pack"
APPLY_PLAYLIST = "harken_apply_playlist_pack"
AUDIO_FIELDS = {"file_id", "name", "relative_path", "duration_seconds"}
RAW_AUDIO = b"synthetic raw audio sentinel"
LIBRARY_ID = "harken-test-library"
ALL_READ = {
    "searchAudio": "read",
    "findSimilar": "read",
    "findFolders": "read",
    "readFileName": "read",
    "readMetadata": "read",
    "editMetadata": "none",
    "aiNamingMetadata": "read",
    "readPlaylist": "read",
    "editPlaylist": "none",
}


def write_authorization(path, root, enabled=True, capabilities=None, library_id=LIBRARY_ID):
    document = {
        "libraryId": library_id,
        "enabled": enabled,
        "sampleLibrary": False,
        "roots": [str(root)],
        "capabilities": dict(ALL_READ if capabilities is None else capabilities),
    }
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def request(method, request_id=None, params=None):
    message = {"jsonrpc": "2.0", "method": method}
    if request_id is not None:
        message["id"] = request_id
    if params is not None:
        message["params"] = params
    return message


def initialize(version=VERSIONS[-1]):
    return request("initialize", "hello", {
        "protocolVersion": version,
        "capabilities": {},
        "clientInfo": {"name": "black-box test", "version": "1"},
    })


def call(name, arguments, request_id):
    return request("tools/call", request_id, {"name": name, "arguments": arguments})


def wire_line(message):
    if not isinstance(message, bytes):
        message = json.dumps(message, ensure_ascii=False).encode("utf-8")
    return message + b"\n"


class ExternalAiMcpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture = tempfile.TemporaryDirectory(prefix="harken-stdio-")
        cls.addClassCleanup(fixture.cleanup)
        cls.fixture = Path(fixture.name)
        cls.root = cls.fixture / "\u7d20\u6750 library"
        cls.database = cls.fixture / "\u641c\u7d22 catalog.db"
        cls.outside = cls.fixture / f"{cls.root.name}-outside"
        names = [f"wind-{index:02}.wav" for index in range(1, 14)] + ["wind%_literal.wav"]
        tracks = [(index, cls.root / "wind" / name) for index, name in enumerate(names, 1)]
        tracks.extend([
            (40, cls.root / "\u5927\u6d77" / "\u6d77\u98ce.wav"),
            (41, cls.root / "music" / "game-theme.wav"),
            (99, cls.outside / "wind" / "wind-outside.wav"),
        ])
        for file_id, path in tracks:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(RAW_AUDIO + str(file_id).encode("ascii"))
        with sqlite3.connect(cls.database) as connection:
            connection.execute("""
                CREATE TABLE audio_files (
                    id INTEGER PRIMARY KEY, file_path TEXT NOT NULL, file_name TEXT NOT NULL,
                    extension TEXT, duration REAL, title TEXT, description TEXT,
                    category TEXT, keywords TEXT, search_data TEXT,
                    harken_meta TEXT NOT NULL DEFAULT ''
                )
            """)
            connection.executemany(
                "INSERT INTO audio_files (id, file_path, file_name, extension, duration, title, description, category, keywords) "
                "VALUES (?, ?, ?, 'wav', ?, '', '', '', '')",
                [(file_id, str(path), path.name, file_id * 0.75) for file_id, path in tracks],
            )
            connection.execute(
                "UPDATE audio_files SET title = 'Game Theme', category = 'Music', keywords = 'score bpm' WHERE id = 41")
        connection.close()
        cls.settings = write_authorization(cls.fixture / "authorization.json", cls.root)

    @property
    def launch_flags(self):
        return ["--library-db", str(self.database), "--library-root", str(self.root),
                "--settings", str(self.settings), "--library-id", LIBRARY_ID]

    def command(self, flags):
        self.assertTrue(EXECUTABLE.is_file(), f"Missing HarkenExternalAiMcp executable: {EXECUTABLE}")
        return [str(EXECUTABLE), *(self.launch_flags if flags is None else flags)]

    def launch(self, messages, flags=None):
        return subprocess.run(
            self.command(flags), cwd=self.fixture,
            input=b"".join(wire_line(message) for message in messages),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10, check=False,
        )

    def assert_no_echo(self, stdout, stderr):
        raw_output = stdout + stderr
        self.assertNotIn(RAW_AUDIO, raw_output)
        output = raw_output.decode("utf-8")
        for path in (self.database, self.root, self.outside):
            for spelling in (path.as_posix(), str(path), path.name):
                self.assertNotIn(spelling, output)
                self.assertNotIn(json.dumps(spelling)[1:-1], output)

    def exchange(self, *messages, flags=None):
        completed = self.launch(messages, flags)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, b"")
        self.assertTrue(completed.stdout.endswith(b"\n"))
        self.assert_no_echo(completed.stdout, completed.stderr)
        replies = [json.loads(line) for line in completed.stdout.decode("utf-8").split("\n")[:-1]]
        for reply in replies:
            self.assertEqual(reply["jsonrpc"], "2.0")
            self.assertIn("id", reply)
            self.assertNotEqual("result" in reply, "error" in reply)
        return replies

    def with_session(self, *messages, version=VERSIONS[-1], flags=None):
        replies = self.exchange(initialize(version), request("notifications/initialized"), *messages, flags=flags)
        self.assertEqual(replies.pop(0)["id"], "hello")
        return replies

    @contextmanager
    def live_session(self, flags):
        with subprocess.Popen(self.command(flags), cwd=self.fixture, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            received = Queue()

            def pump():
                for line in process.stdout:
                    received.put(line)
                received.put(b"")

            reader = Thread(target=pump, daemon=True)
            reader.start()

            def exchange(message):
                process.stdin.write(wire_line(message))
                process.stdin.flush()
                reply = received.get(timeout=5)
                self.assertTrue(reply, process.stderr.read().decode("utf-8") if not reply else "")
                self.assert_no_echo(reply, b"")
                decoded = json.loads(reply)
                self.assertEqual(decoded["id"], message["id"])
                return decoded

            try:
                self.assertIn("result", exchange(initialize()))
                process.stdin.write(wire_line(request("notifications/initialized")))
                process.stdin.flush()
                yield exchange
            finally:
                process.stdin.close()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                reader.join(timeout=5)

    def configure_client(self, area, client, action):
        config = area / ("mcp.json" if client == "cindy" else "config.toml")
        spec = area / "link-fixture.json"
        spec.write_text(json.dumps({
            "client": client, "config": str(config), "executable": str(EXECUTABLE),
            "library": LIBRARY_ID, "database": str(self.database), "root": str(self.root),
            "settings": str(area / "authorization.json"),
        }), encoding="utf-8")
        subprocess.run([str(LINK_FIXTURE), "--fixture", str(spec), action], cwd=area, check=True, timeout=5)
        if action == "unlink":
            return None
        text = config.read_text(encoding="utf-8")
        if client == "cindy":
            return json.loads(text)["mcpServers"]["harken"]["args"]
        return json.loads(next(line.split("=", 1)[1] for line in text.splitlines() if line.startswith("args = ")))

    @unittest.skipIf(LINK_FIXTURE is None, "requires the isolated client-link fixture executable")
    def test_disconnect_revokes_only_that_clients_live_session(self):
        with tempfile.TemporaryDirectory(dir=self.fixture) as scratch, ExitStack() as sessions:
            area = Path(scratch)
            write_authorization(area / "authorization.json", self.root)
            cindy = sessions.enter_context(self.live_session(self.configure_client(area, "cindy", "link")))
            codex = sessions.enter_context(self.live_session(self.configure_client(area, "codex", "link")))
            query = call(SEARCH, {"query": "wind", "limit": 1}, "search")
            self.assertTrue(self.tool_payload(cindy(query))["results"])
            self.assertTrue(self.tool_payload(codex(query))["results"])

            self.configure_client(area, "cindy", "unlink")
            denied = cindy(query)
            self.assertIn("error", denied, "disconnect must deny the previously authorized live connector")
            self.assertIn("error", cindy(request("tools/list", "list")))
            self.assertTrue(self.tool_payload(codex(query))["results"])

    @unittest.skipIf(LINK_FIXTURE is None, "requires the isolated client-link fixture executable")
    def test_reconnect_cannot_resurrect_an_old_session_even_without_an_interim_call(self):
        with tempfile.TemporaryDirectory(dir=self.fixture) as scratch, ExitStack() as sessions:
            area = Path(scratch)
            write_authorization(area / "authorization.json", self.root)
            old_flags = self.configure_client(area, "codex", "link")
            old = sessions.enter_context(self.live_session(old_flags))
            query = call(SEARCH, {"query": "wind", "limit": 1}, "search")
            self.assertTrue(self.tool_payload(old(query))["results"])

            original = (area / "config.toml").read_bytes()
            self.configure_client(area, "codex", "unlink")
            new_flags = self.configure_client(area, "codex", "link")
            fresh = sessions.enter_context(self.live_session(new_flags))
            self.assertTrue(self.tool_payload(fresh(query))["results"])
            self.assertIn("error", old(query), "relink must not reauthorize the old connector")
            self.assertNotEqual(self.launch([initialize()], old_flags).returncode, 0)

            (area / "config.toml").write_bytes(original)
            self.assertIn("error", fresh(query), "restoring a stale client entry must not match the current link")
            self.assertIn("error", old(query), "restoring stale config bytes must not reactivate an old link")
            self.assertNotEqual(self.launch([initialize()], old_flags).returncode, 0)

    def tool_payload(self, reply, structured=True):
        result = reply["result"]
        self.assertEqual(len(result["content"]), 1)
        content = result["content"][0]
        self.assertEqual(set(content), {"type", "text"})
        self.assertEqual(content["type"], "text")
        if structured:
            self.assertIn("structuredContent", result)
            payload = result["structuredContent"]
            self.assertEqual(result["structuredContent"], payload)
            self.assertNotEqual(content["text"], json.dumps(payload, ensure_ascii=False))
        else:
            payload = json.loads(content["text"])
            self.assertNotIn("structuredContent", result)
        self.assertIn("status", payload)
        self.assertIn("results", payload)
        return payload

    def raw_tool_payload(self, reply):
        result = reply["result"]
        self.assertEqual(len(result["content"]), 1)
        content = result["content"][0]
        self.assertEqual(content["type"], "text")
        if "structuredContent" in result:
            return result["structuredContent"]
        return json.loads(content["text"])

    def snapshot(self):
        return {
            path.relative_to(self.fixture).as_posix():
            (path.stat().st_mtime_ns, hashlib.sha256(path.read_bytes()).hexdigest())
            for path in self.fixture.rglob("*") if path.is_file()
        }

    def test_audio_search_indexes_metadata_but_not_hidden_absolute_paths(self):
        with tempfile.TemporaryDirectory(dir=self.fixture) as scratch:
            catalog = Path(scratch) / "path-privacy.db"
            private_root = Path(scratch) / "privatecatalogmarker"
            private_root.mkdir()
            audio = private_root / "ambience.wav"
            with closing(sqlite3.connect(catalog)) as writer:
                writer.executescript("""
                    CREATE TABLE audio_files(
                        id INTEGER PRIMARY KEY, file_path TEXT NOT NULL, file_name TEXT NOT NULL,
                        folder_path TEXT, duration REAL, title TEXT, artist TEXT, album TEXT,
                        description TEXT, keywords TEXT, category TEXT, search_data TEXT);
                    CREATE VIRTUAL TABLE audio_files_fts USING fts5(
                        file_name,file_path,folder_path,title,artist,album,description,keywords,category,search_data,
                        content='audio_files',content_rowid='id');
                    CREATE VIRTUAL TABLE audio_files_name_trigram USING fts5(
                        file_name,folder_path,content='audio_files',content_rowid='id',tokenize='trigram');
                """)
                writer.execute(
                    "INSERT INTO audio_files VALUES (1, ?, ?, ?, 1, '', '', '', ?, '', '', ?)",
                    (str(audio), audio.name, str(private_root), "forest ambience", "privatecatalogmarker"),
                )
                writer.executescript("""
                    INSERT INTO audio_files_fts(audio_files_fts) VALUES('rebuild');
                    INSERT INTO audio_files_name_trigram(audio_files_name_trigram) VALUES('rebuild');
                """)
            completed = self.launch(
                [initialize(), request("notifications/initialized"),
                 call(SEARCH, {"query": "privatecatalogmarker"}, 1),
                 call(SEARCH, {"query": "forest"}, 2)],
                ["--library-db", str(catalog), "--library-root", str(private_root),
                 "--settings", str(write_authorization(Path(scratch) / "auth.json", private_root)),
                 "--library-id", LIBRARY_ID],
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            replies = [json.loads(line) for line in completed.stdout.splitlines()]
            self.assertEqual(self.tool_payload(replies[1]), {"status": "ok", "results": []})
            self.assertEqual([row["file_id"] for row in self.tool_payload(replies[2])["results"]], [1])

    def test_linked_folders_cannot_expose_catalog_entries_outside_the_root(self):
        with tempfile.TemporaryDirectory(dir=self.fixture) as scratch:
            area = Path(scratch)
            allowed = area / "allowed"
            private = area / "private"
            allowed.mkdir()
            private.mkdir()
            link = allowed / "linked"
            if os.name == "nt":
                link_arg = str(link).replace("'", "''")
                private_arg = str(private).replace("'", "''")
                command = f"New-Item -ItemType Junction -Path '{link_arg}' -Target '{private_arg}' -ErrorAction Stop"
                subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
                               capture_output=True, check=True, timeout=10)
            else:
                link.symlink_to(private, target_is_directory=True)
            try:
                real_audio = private / "unshared.wav"
                real_audio.write_bytes(RAW_AUDIO)
                catalog = area / "links.db"
                catalog.write_bytes(self.database.read_bytes())
                with closing(sqlite3.connect(catalog)) as writer:
                    writer.execute(
                        "INSERT INTO audio_files (id, file_path, file_name, extension, duration, title, description, category, keywords) VALUES (95, ?, 'unshared.wav', 'wav', 1, '', '', '', '')",
                        (str(link / real_audio.name),),
                    )
                    writer.commit()
                completed = self.launch(
                    [initialize(), request("notifications/initialized"),
                     call(SEARCH, {"query": "unshared"}, 1),
                     call(FOLDERS, {"query": "linked"}, 2),
                     call(SIMILAR, {"reference_id": 95}, 3)],
                    ["--library-db", str(catalog), "--library-root", str(allowed),
                     "--settings", str(write_authorization(area / "auth.json", allowed)),
                     "--library-id", LIBRARY_ID],
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                replies = [json.loads(line) for line in completed.stdout.splitlines()]
                self.assertEqual(self.tool_payload(replies[1]), {"status": "ok", "results": []})
                self.assertEqual(self.tool_payload(replies[2]), {"status": "ok", "results": []})
                self.assertEqual(self.tool_payload(replies[3]), {"status": "reference_not_found", "results": []})
                self.assertEqual(real_audio.read_bytes(), RAW_AUDIO)
            finally:
                if os.name == "nt":
                    link.rmdir()
                else:
                    link.unlink()

    def test_reads_active_wal_without_modifying_catalog_or_sidecars(self):
        with tempfile.TemporaryDirectory(dir=self.fixture) as scratch:
            database = Path(scratch) / "live.db"
            database.write_bytes(self.database.read_bytes())
            with closing(sqlite3.connect(database)) as writer:
                self.assertEqual(writer.execute("PRAGMA journal_mode=WAL").fetchone()[0], "wal")
                writer.execute("PRAGMA wal_autocheckpoint=0")
                sound = self.root / "wind" / "wal-only.wav"
                writer.execute(
                    "INSERT INTO audio_files (id, file_path, file_name, extension, duration, title, description, category, keywords) VALUES (?, ?, ?, 'wav', ?, '', '', '', '')",
                    (55, str(sound), sound.name, 1.25),
                )
                writer.commit()
                authorization = write_authorization(Path(scratch) / "auth.json", self.root)
                before = self.snapshot()
                for suffix in ("", "-wal", "-shm"):
                    self.assertTrue(database.with_name(database.name + suffix).is_file())
                completed = self.launch(
                    [initialize(), request("notifications/initialized"),
                     call(SEARCH, {"query": "wal-only"}, 1)],
                    ["--library-db", str(database), "--library-root", str(self.root),
                     "--settings", str(authorization),
                     "--library-id", LIBRARY_ID],
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assert_no_echo(completed.stdout, completed.stderr)
                replies = [json.loads(line) for line in completed.stdout.splitlines()]
                self.assertEqual(len(replies), 2)
                payload = self.tool_payload(replies[1])
                self.assertEqual(payload["status"], "ok")
                self.assertEqual([row["file_id"] for row in payload["results"]], [55])
                self.assertEqual(self.snapshot(), before)

    def test_launch_refuses_discovery_without_valid_explicit_authorization(self):
        missing_db = self.fixture / "missing.db"
        invalid_db = self.fixture / "not-a-database.db"
        invalid_db.write_bytes(b"not a SQLite database")
        self.addCleanup(invalid_db.unlink)
        malformed = Path(tempfile.mkdtemp(dir=self.fixture)) / "broken.json"
        malformed.write_text("{not json", encoding="utf-8")
        database = ["--library-db", str(self.database)]
        root = ["--library-root", str(self.root)]
        settings = ["--settings", str(self.settings)]
        identity = ["--library-id", LIBRARY_ID]
        for flags in (
            [],
            database, root, settings, identity,
            [*database, *root],
            [*database, *root, *settings],
            [*database, *root, *identity],
            [*database, *root, *settings, "--library-id", ""],
            [*database, *root, *settings, "--library-id", "another-library"],
            [*database, *root, "--settings", str(malformed), *identity],
            [*database, *root, "--settings", str(self.fixture / "missing-settings.json"), *identity],
            ["--library-db", self.database.name, *root, *settings, *identity],
            [*database, "--library-root", self.root.name, *settings, *identity],
            ["--library-db", str(missing_db), *root, *settings, *identity],
            ["--library-db", str(invalid_db), *root, *settings, *identity],
            [*database, "--library-root", str(self.fixture / "missing-root"), *settings, *identity],
            [*database, "--library-root", str(self.database), *settings, *identity],
            [*self.launch_flags, *database],
            [*self.launch_flags, "--port", "8080"],
        ):
            with self.subTest(flags=flags):
                completed = self.launch(
                    [initialize(), request("notifications/initialized"), request("tools/list", 1)],
                    flags,
                )
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(completed.stdout, b"")
                self.assertTrue(completed.stderr)
                self.assert_no_echo(completed.stdout, completed.stderr)
                self.assertNotIn(str(invalid_db).encode("utf-8"), completed.stderr)
        self.assertFalse(missing_db.exists())

    def test_negotiates_all_versions_and_uses_text_for_older_clients(self):
        for requested in (*VERSIONS, "2030-01-01"):
            with self.subTest(version=requested):
                selected = requested if requested in VERSIONS else VERSIONS[-1]
                greeting, reply = self.exchange(
                    wire_line(initialize(requested)).rstrip(b"\n") + b"\r",
                    request("notifications/initialized"),
                    call(SEARCH, {"query": "wind", "limit": 1}, 1),
                )
                self.assertEqual(greeting["id"], "hello")
                self.assertEqual(greeting["result"]["protocolVersion"], selected)
                self.assertEqual(greeting["result"]["capabilities"], {"tools": {}})
                self.assertIn("name", greeting["result"]["serverInfo"])
                self.assertIn("version", greeting["result"]["serverInfo"])
                payload = self.tool_payload(reply, structured=selected in VERSIONS[2:])
                self.assertEqual(payload["status"], "ok")
                self.assertEqual(len(payload["results"]), 1)

    def test_flushes_handshake_without_waiting_for_end_of_input(self):
        with subprocess.Popen(
            self.command(None), cwd=self.fixture,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ) as process:
            received = Queue()
            reader = Thread(target=lambda: received.put(process.stdout.readline()), daemon=True)
            reader.start()
            try:
                process.stdin.write(wire_line(initialize()))
                process.stdin.flush()
                self.assertEqual(json.loads(received.get(timeout=5))["id"], "hello")
                process.stdin.close()
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual(process.stdout.read(), b"")
                self.assertEqual(process.stderr.read(), b"")
            finally:
                if process.poll() is None:
                    process.kill()
                reader.join(timeout=5)

    def test_gates_tools_until_initialized_and_ignores_notifications(self):
        replies = self.exchange(
            request("notifications/initialized"),
            request("tools/list", 1),
            request("ping", 0),
            request("initialize", params=initialize()["params"]),
            request("tools/list", 2),
            initialize(),
            request("tools/list", 3),
            request("notifications/initialized", 4),
            request("tools/list", 5),
            request("notifications/initialized"),
            request("notifications/unknown"),
            request("tools/call", params={"name": SEARCH, "arguments": {"query": "wind"}}),
            request("ping", params={"ignored": True}),
            request("ping", "done"),
        )
        self.assertEqual([reply["id"] for reply in replies], [1, 0, 2, "hello", 3, 4, 5, "done"])
        for index in (0, 2, 4, 6):
            self.assertEqual(replies[index]["error"]["code"], -32002)
        self.assertEqual(replies[1]["result"], {})
        self.assertIn(replies[5]["error"]["code"], (-32601, -32602))
        self.assertEqual(replies[-1]["result"], {})

    def test_discovery_exposes_read_and_confirm_tools_with_explicit_write_boundary(self):
        discovery, no_resources, no_templates, no_write = self.with_session(
            request("tools/list", 1),
            request("resources/list", 2),
            request("resources/templates/list", 3),
            call("write_file", {"path": str(self.database), "data": "deny"}, 4),
        )
        tools = discovery["result"]["tools"]
        self.assertEqual(len(tools), 8)
        self.assertEqual({tool["name"] for tool in tools},
                         {SEARCH, FOLDERS, SIMILAR, METADATA, PREVIEW_METADATA, APPLY_METADATA,
                          PREVIEW_PLAYLIST, APPLY_PLAYLIST})
        for tool in tools:
            if tool["name"] in (APPLY_METADATA, APPLY_PLAYLIST):
                self.assertIs(tool["annotations"]["readOnlyHint"], False)
                self.assertIs(tool["annotations"]["destructiveHint"], True)
                required = ["proposal_id"] if tool["name"] == APPLY_METADATA else ["proposal_id", "confirm"]
                properties = {"proposal_id"} if tool["name"] == APPLY_METADATA else {"proposal_id", "confirm"}
                self.assertEqual(tool["inputSchema"]["required"], required)
                self.assertEqual(set(tool["inputSchema"]["properties"]), properties)
                continue
            self.assertIs(tool["annotations"]["readOnlyHint"], True)
            if tool["name"] == METADATA:
                self.assertEqual(tool["inputSchema"]["required"], ["file_id"])
                self.assertEqual(set(tool["inputSchema"]["properties"]), {"file_id"})
                continue
            if tool["name"] == PREVIEW_METADATA:
                self.assertEqual(tool["inputSchema"]["required"], ["file_id", "fields"])
                self.assertEqual(set(tool["inputSchema"]["properties"]), {"file_id", "fields"})
                continue
            if tool["name"] == PREVIEW_PLAYLIST:
                self.assertEqual(tool["inputSchema"]["required"], ["domain", "playlist"])
                self.assertEqual(
                    set(tool["inputSchema"]["properties"]),
                    {"domain", "style", "theme", "playlist", "query", "file_ids", "limit"})
                self.assertEqual(tool["inputSchema"]["properties"]["domain"]["enum"], ["game_sfx"])
                continue
            schema = tool["inputSchema"]
            required = "reference_id" if tool["name"] == SIMILAR else "query"
            self.assertEqual(schema["required"], [required])
            self.assertEqual(set(schema["properties"]), {required, "limit", "fields", "cursor"})
            self.assertIs(schema["additionalProperties"], False)
            self.assertEqual(
                {field: schema["properties"]["limit"][field] for field in ("minimum", "maximum", "default")},
                {"minimum": 1, "maximum": 50, "default": 5},
            )
            self.assertTrue(schema["properties"]["fields"]["items"]["enum"])
            self.assertTrue(schema["properties"]["fields"]["uniqueItems"])
        self.assertEqual(no_resources["error"]["code"], -32601)
        self.assertEqual(no_templates["error"]["code"], -32601)
        self.assertEqual(no_write["error"]["code"], -32602)

    def test_metadata_preview_then_explicit_apply_updates_one_file(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        document = write_authorization(
            scratch / "editable.json", self.root,
            capabilities={**ALL_READ, "editMetadata": "edit", "aiNamingMetadata": "edit"})
        flags = ["--library-db", str(self.database), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        with subprocess.Popen(
            self.command(flags), cwd=self.fixture,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ) as process:
            received = Queue()

            def pump():
                for line in iter(process.stdout.readline, b""):
                    received.put(line)

            reader = Thread(target=pump, daemon=True)
            reader.start()
            try:
                process.stdin.write(wire_line(initialize()))
                process.stdin.write(wire_line(request("notifications/initialized")))
                process.stdin.write(wire_line(call(METADATA, {"file_id": 1}, 1)))
                process.stdin.write(wire_line(call(PREVIEW_METADATA, {"file_id": 1, "fields": {
                    "title": "Wind Metal Door",
                    "category": "Foley",
                    "keywords": "wind, metal, door",
                    "description": "Metal door movement",
                }}, 2)))
                process.stdin.flush()
                self.assertEqual(json.loads(received.get(timeout=5))["id"], "hello")
                current = self.raw_tool_payload(json.loads(received.get(timeout=5)))
                preview = self.raw_tool_payload(json.loads(received.get(timeout=5)))
                self.assertEqual(current["status"], "ok")
                self.assertEqual(current["metadata"]["title"], "")
                self.assertEqual(preview["status"], "ok")
                self.assertEqual(preview["confirmation"], "required")
                self.assertEqual(preview["before"]["title"], "")
                self.assertEqual(preview["after"]["title"], "Wind Metal Door")
                self.assertEqual(preview["after"]["category"], "Foley")
                self.assertTrue(preview["proposal_id"])

                process.stdin.write(wire_line(call(
                    APPLY_METADATA, {"proposal_id": preview["proposal_id"]}, 3)))
                process.stdin.write(wire_line(call(METADATA, {"file_id": 1}, 4)))
                process.stdin.flush()
                applied = self.raw_tool_payload(json.loads(received.get(timeout=5)))
                updated = self.raw_tool_payload(json.loads(received.get(timeout=5)))
                self.assertEqual(applied["status"], "applied")
                self.assertEqual(updated["status"], "ok")
                self.assertEqual(updated["metadata"]["title"], "Wind Metal Door")
                self.assertEqual(updated["metadata"]["category"], "Foley")

                process.stdin.close()
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual(process.stderr.read(), b"")
            finally:
                if process.poll() is None:
                    process.kill()
                reader.join(timeout=5)

        with closing(sqlite3.connect(self.database)) as connection:
            row = connection.execute(
                "SELECT title, description, category, keywords FROM audio_files WHERE id = 1").fetchone()
        self.assertEqual(row, (
            "Wind Metal Door", "Metal door movement", "Foley", "wind, metal, door"))

    def test_game_sfx_preview_is_playlist_first_profiled_and_read_only(self):
        before = self.snapshot()
        replies = self.with_session(
            call(PREVIEW_PLAYLIST, {
                "domain": "game_sfx",
                "style": "cartoon",
                "theme": "forest adventure",
                "playlist": "Game SFX",
                "query": "wind",
                "limit": 5,
            }, 1),
            call(PREVIEW_PLAYLIST, {
                "domain": "game_sfx",
                "playlist": "Game SFX",
                "query": "game-theme",
            }, 2),
            call(PREVIEW_PLAYLIST, {
                "domain": "game_sfx",
                "playlist": "Game SFX",
                "file_ids": [1, 99],
            }, 3),
        )
        preview = self.raw_tool_payload(replies[0])
        music = self.raw_tool_payload(replies[1])
        filtered = self.raw_tool_payload(replies[2])
        self.assertEqual(preview["status"], "ok")
        self.assertEqual(preview["domain"], "game_sfx")
        self.assertEqual(preview["profile"]["version"], "game_sfx.v1")
        self.assertIn("Review Needed", preview["profile"]["taxonomy"])
        self.assertEqual(preview["style"], "cartoon")
        self.assertEqual(preview["theme"], "forest adventure")
        self.assertEqual(preview["confirmation"], "required")
        self.assertTrue(preview["proposal_id"])
        self.assertTrue(preview["selections"])
        for selection in preview["selections"]:
            self.assertTrue(selection["playlist_path"].startswith("Game SFX/"))
            self.assertGreaterEqual(selection["confidence"], 0.0)
            self.assertLessEqual(selection["confidence"], 1.0)
            self.assertTrue(selection["reason"])
            self.assertTrue(selection["source"])
            self.assertIsInstance(selection["needs_review"], bool)
        self.assertEqual(music["status"], "ok")
        self.assertEqual(music["selections"], [])
        self.assertEqual([selection["file_id"] for selection in filtered["selections"]], [1])
        self.assertEqual(self.snapshot(), before)

    def test_game_sfx_preview_requires_independent_playlist_read_grant(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        document = write_authorization(
            scratch / "no-playlist-read.json", self.root,
            capabilities={**ALL_READ, "readPlaylist": "none"})
        flags = ["--library-db", str(self.database), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        replies = self.with_session(call(PREVIEW_PLAYLIST, {
            "domain": "game_sfx", "playlist": "Game SFX", "query": "wind"}, 1), flags=flags)
        payload = self.raw_tool_payload(replies[0])
        self.assertEqual(payload["status"], "not_authorized")

    def test_game_sfx_apply_is_confirmation_gated_and_fails_closed_without_playlist_facade(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        document = write_authorization(
            scratch / "playlist-editable.json", self.root,
            capabilities={**ALL_READ, "editPlaylist": "edit"})
        flags = ["--library-db", str(self.database), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        before = self.snapshot()
        with subprocess.Popen(
            self.command(flags), cwd=self.fixture,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ) as process:
            received = Queue()

            def pump():
                for line in iter(process.stdout.readline, b""):
                    received.put(line)

            reader = Thread(target=pump, daemon=True)
            reader.start()
            try:
                process.stdin.write(wire_line(initialize()))
                process.stdin.write(wire_line(request("notifications/initialized")))
                process.stdin.write(wire_line(call(PREVIEW_PLAYLIST, {
                    "domain": "game_sfx", "playlist": "Game SFX", "query": "wind"}, 1)))
                process.stdin.flush()
                self.assertEqual(json.loads(received.get(timeout=5))["id"], "hello")
                preview = self.raw_tool_payload(json.loads(received.get(timeout=5)))
                process.stdin.write(wire_line(call(APPLY_PLAYLIST, {
                    "proposal_id": preview["proposal_id"], "confirm": False}, 2)))
                process.stdin.flush()
                self.assertEqual(self.raw_tool_payload(json.loads(received.get(timeout=5)))["status"],
                                 "confirmation_required")
                process.stdin.write(wire_line(call(APPLY_PLAYLIST, {
                    "proposal_id": preview["proposal_id"], "confirm": True}, 3)))
                process.stdin.flush()
                applied = self.raw_tool_payload(json.loads(received.get(timeout=5)))
                # Harken is not running in this test: the connector must stay
                # fail-closed instead of writing on its own.
                self.assertEqual(applied["status"], "playlist_write_unavailable")
                self.assertEqual(applied["blocker"], "harken_write_bridge_unavailable")
                process.stdin.close()
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual(process.stderr.read(), b"")
            finally:
                if process.poll() is None:
                    process.kill()
                reader.join(timeout=5)
        self.assertEqual(self.snapshot(), before)

    def test_game_sfx_apply_rejects_changed_source_collection(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        document = write_authorization(
            scratch / "playlist-editable.json", self.root,
            capabilities={**ALL_READ, "editPlaylist": "edit"})
        flags = ["--library-db", str(self.database), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        with subprocess.Popen(
            self.command(flags), cwd=self.fixture,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ) as process:
            received = Queue()

            def pump():
                for line in iter(process.stdout.readline, b""):
                    received.put(line)

            reader = Thread(target=pump, daemon=True)
            reader.start()
            try:
                process.stdin.write(wire_line(initialize()))
                process.stdin.write(wire_line(request("notifications/initialized")))
                process.stdin.write(wire_line(call(PREVIEW_PLAYLIST, {
                    "domain": "game_sfx", "playlist": "Game SFX", "file_ids": [1]}, 1)))
                process.stdin.flush()
                self.assertEqual(json.loads(received.get(timeout=5))["id"], "hello")
                preview = self.raw_tool_payload(json.loads(received.get(timeout=5)))
                with closing(sqlite3.connect(self.database)) as connection:
                    connection.execute("UPDATE audio_files SET title = 'Changed after preview' WHERE id = 1")
                    connection.commit()
                process.stdin.write(wire_line(call(APPLY_PLAYLIST, {
                    "proposal_id": preview["proposal_id"], "confirm": True}, 2)))
                process.stdin.flush()
                applied = self.raw_tool_payload(json.loads(received.get(timeout=5)))
                self.assertEqual(applied["status"], "proposal_stale")
                process.stdin.close()
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual(process.stderr.read(), b"")
            finally:
                if process.poll() is None:
                    process.kill()
                reader.join(timeout=5)
        with closing(sqlite3.connect(self.database)) as connection:
            connection.execute("UPDATE audio_files SET title = '' WHERE id = 1")
            connection.commit()

    def test_metadata_apply_rechecks_permission_after_preview(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        document = write_authorization(
            scratch / "editable.json", self.root,
            capabilities={**ALL_READ, "editMetadata": "edit", "aiNamingMetadata": "edit"})
        flags = ["--library-db", str(self.database), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        with subprocess.Popen(
            self.command(flags), cwd=self.fixture,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ) as process:
            received = Queue()

            def pump():
                for line in iter(process.stdout.readline, b""):
                    received.put(line)

            reader = Thread(target=pump, daemon=True)
            reader.start()
            try:
                process.stdin.write(wire_line(initialize()))
                process.stdin.write(wire_line(request("notifications/initialized")))
                process.stdin.write(wire_line(call(
                    PREVIEW_METADATA, {"file_id": 1, "fields": {"title": "Revoked"}}, 1)))
                process.stdin.flush()
                self.assertEqual(json.loads(received.get(timeout=5))["id"], "hello")
                preview = json.loads(received.get(timeout=5))
                proposal = self.raw_tool_payload(preview)["proposal_id"]

                write_authorization(document, self.root, enabled=False,
                                    capabilities={**ALL_READ, "editMetadata": "edit", "aiNamingMetadata": "edit"})
                process.stdin.write(wire_line(call(APPLY_METADATA, {"proposal_id": proposal}, 2)))
                process.stdin.flush()
                applied = json.loads(received.get(timeout=5))
                payload = self.raw_tool_payload(applied)
                self.assertEqual(payload["status"], "not_authorized")
                self.assertTrue(applied["result"]["isError"])

                process.stdin.close()
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual(process.stderr.read(), b"")
            finally:
                if process.poll() is None:
                    process.kill()
                reader.join(timeout=5)

        with closing(sqlite3.connect(self.database)) as connection:
            self.assertEqual(connection.execute(
                "SELECT title FROM audio_files WHERE id = 1").fetchone()[0], "")

    def test_harken_metadata_preview_apply_and_manual_skip(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        document = write_authorization(
            scratch / "editable.json", self.root,
            capabilities={**ALL_READ, "editMetadata": "edit", "aiNamingMetadata": "edit"})
        flags = ["--library-db", str(self.database), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        with self.live_session(flags) as exchange:
            preview = self.raw_tool_payload(exchange(call(PREVIEW_METADATA, {
                "file_id": 1,
                "fields": {"harken": {"category": "Weapons", "function": "Impact"}},
            }, 1)))
            self.assertEqual(preview["status"], "ok")
            self.assertEqual(preview["after"]["harken"]["category"], "Weapons")
            self.assertEqual(preview["after"]["harken"]["function"], "Impact")
            self.assertEqual(preview["after"]["harken"]["source"], "ai")
            applied = self.raw_tool_payload(exchange(call(APPLY_METADATA, {
                "proposal_id": preview["proposal_id"]}, 2)))
            self.assertEqual(applied["status"], "applied")
            current = self.raw_tool_payload(exchange(call(METADATA, {"file_id": 1}, 3)))
            self.assertEqual(current["metadata"]["harken"]["category"], "Weapons")
            self.assertEqual(current["metadata"]["harken"]["function"], "Impact")
            self.assertEqual(current["metadata"]["harken"]["source"], "ai")

        # closing() is required: the context manager only commits, so a bare
        # "with sqlite3.connect(...)" keeps the catalog open and the Windows
        # fixture cleanup then fails with WinError 32.
        with closing(sqlite3.connect(self.database)) as connection:
            connection.execute(
                "UPDATE audio_files SET harken_meta=? WHERE id=1",
                (json.dumps({"category": "Manual", "function": "Keep", "source": "manual"}),))
            connection.commit()

        with self.live_session(flags) as exchange:
            preview = self.raw_tool_payload(exchange(call(PREVIEW_METADATA, {
                "file_id": 1,
                "fields": {"harken": {"category": "AI Override"}},
            }, 1)))
            self.assertEqual(preview["status"], "ok")
            skipped = self.raw_tool_payload(exchange(call(APPLY_METADATA, {
                "proposal_id": preview["proposal_id"]}, 2)))
            self.assertEqual(skipped["status"], "skipped_manual")
            current = self.raw_tool_payload(exchange(call(METADATA, {"file_id": 1}, 3)))
            self.assertEqual(current["metadata"]["harken"]["category"], "Manual")
            self.assertEqual(current["metadata"]["harken"]["function"], "Keep")
            self.assertEqual(current["metadata"]["harken"]["source"], "manual")

    def test_legacy_library_without_harken_column_still_reads_metadata(self):
        """A library the desktop app has not upgraded yet must stay readable,
        and a Harken write must say that an upgrade is needed."""
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        legacy = scratch / "legacy catalog.db"
        shutil.copyfile(self.database, legacy)
        with closing(sqlite3.connect(legacy)) as connection:
            connection.execute("ALTER TABLE audio_files DROP COLUMN harken_meta")
            connection.commit()
        document = write_authorization(
            scratch / "editable.json", self.root,
            capabilities={**ALL_READ, "editMetadata": "edit", "aiNamingMetadata": "edit"})
        flags = ["--library-db", str(legacy), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        with self.live_session(flags) as exchange:
            metadata = self.raw_tool_payload(exchange(call(METADATA, {"file_id": 1}, 1)))
            self.assertEqual(metadata["status"], "ok")
            self.assertEqual(metadata["metadata"]["category"], "")
            self.assertEqual(metadata["metadata"]["harken"]["category"], "")
            preview = self.raw_tool_payload(exchange(call(PREVIEW_METADATA, {
                "file_id": 1,
                "fields": {"harken": {"category": "Weapons"}},
            }, 2)))
            self.assertEqual(preview["status"], "ok")
            applied = self.raw_tool_payload(exchange(call(APPLY_METADATA, {
                "proposal_id": preview["proposal_id"]}, 3)))
            self.assertEqual(applied["status"], "library_needs_upgrade")

    def test_metadata_apply_requires_edit_grants(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        document = write_authorization(
            scratch / "readonly.json", self.root,
            capabilities={**ALL_READ, "editMetadata": "read", "aiNamingMetadata": "read"})
        flags = ["--library-db", str(self.database), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        with subprocess.Popen(
            self.command(flags), cwd=self.fixture,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ) as process:
            received = Queue()

            def pump():
                for line in iter(process.stdout.readline, b""):
                    received.put(line)

            reader = Thread(target=pump, daemon=True)
            reader.start()
            try:
                process.stdin.write(wire_line(initialize()))
                process.stdin.write(wire_line(request("notifications/initialized")))
                process.stdin.write(wire_line(call(
                    PREVIEW_METADATA, {"file_id": 1, "fields": {"title": "Denied"}}, 1)))
                process.stdin.flush()
                self.assertEqual(json.loads(received.get(timeout=5))["id"], "hello")
                preview = self.raw_tool_payload(json.loads(received.get(timeout=5)))
                self.assertEqual(preview["status"], "ok")

                process.stdin.write(wire_line(call(
                    APPLY_METADATA, {"proposal_id": preview["proposal_id"]}, 2)))
                process.stdin.flush()
                denied = json.loads(received.get(timeout=5))
                self.assertEqual(self.raw_tool_payload(denied)["status"], "not_authorized")
                self.assertTrue(denied["result"]["isError"])

                process.stdin.close()
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual(process.stderr.read(), b"")
            finally:
                if process.poll() is None:
                    process.kill()
                reader.join(timeout=5)

        with closing(sqlite3.connect(self.database)) as connection:
            self.assertEqual(connection.execute(
                "SELECT title FROM audio_files WHERE id = 1").fetchone()[0], "")

    def test_search_and_folder_results_are_scoped_literal_and_deterministically_limited(self):
        before = self.snapshot()
        searches = [
            (SEARCH, {"query": "wind", "limit": 50}),
            (SEARCH, {"query": "wind"}),
            (SEARCH, {"query": "wind", "limit": 2}),
            (SEARCH, {"query": "wind", "limit": 2}),
            (SEARCH, {"query": "%_"}),
            (SEARCH, {"query": "' OR 1=1 --"}),
            (SEARCH, {"query": "\u6d77\u98ce"}),
            (FOLDERS, {"query": "wind", "limit": 1}),
            (SIMILAR, {"reference_id": 1}),
        ]
        replies = self.with_session(*[
            call(name, arguments, index) for index, (name, arguments) in enumerate(searches, 1)
        ])
        self.assertEqual(self.snapshot(), before)
        full, default, limited, repeated, literal, injected, unicode, folders, similar = [
            self.tool_payload(reply) for reply in replies
        ]
        for result in (full, default, limited, repeated, literal, injected, unicode, folders):
            self.assertEqual(result["status"], "ok")
        self.assertEqual(len(full["results"]), 14)
        self.assertEqual(default["results"], full["results"][:5])
        self.assertTrue(default["next_cursor"])
        self.assertEqual(limited["results"], full["results"][:2])
        self.assertEqual(repeated["results"], limited["results"])
        self.assertEqual([row["name"] for row in literal["results"]], ["wind%_literal.wav"])
        self.assertEqual(injected["results"], [])
        self.assertEqual([row["relative_path"] for row in unicode["results"]], ["\u5927\u6d77/\u6d77\u98ce.wav"])
        self.assertEqual(folders["results"], [{"relative_path": "wind", "file_count": 14}])
        self.assertEqual(similar, {"status": "index_required", "results": []})
        for row in full["results"] + unicode["results"]:
            self.assertEqual(set(row), AUDIO_FIELDS)
            self.assertIs(type(row["file_id"]), int)
            self.assertGreater(row["file_id"], 0)
            self.assertEqual(row["duration_seconds"], row["file_id"] * 0.75)
            self.assertIn(row["relative_path"], {f"wind/{row['name']}", "\u5927\u6d77/\u6d77\u98ce.wav"})
            self.assertNotIn("\\", row["relative_path"])
            self.assertNotEqual(row["file_id"], 99)

    def test_fields_reduce_rows_and_reject_unknown_selection(self):
        full_reply = self.with_session(call(SEARCH, {"query": "wind", "limit": 1}, 1))[0]
        compact_reply = self.with_session(call(
            SEARCH, {"query": "wind", "limit": 1, "fields": ["name", "relative_path"]}, 1))[0]
        full = self.tool_payload(full_reply)
        compact = self.tool_payload(compact_reply)
        self.assertEqual(set(full["results"][0]), AUDIO_FIELDS)
        self.assertEqual(set(compact["results"][0]), {"name", "relative_path"})
        self.assertLess(
            len(json.dumps(compact, ensure_ascii=False)),
            len(json.dumps(full, ensure_ascii=False)))

        replies = self.with_session(
            call(SEARCH, {"query": "wind", "fields": ["not_a_field"]}, 1),
            call(FOLDERS, {"query": "wind", "fields": []}, 2),
            call(SIMILAR, {"reference_id": 1, "fields": ["relative_path", "relative_path"]}, 3),
        )
        self.assertEqual([reply["error"]["code"] for reply in replies], [-32602, -32602, -32602])

    def test_cursor_pages_are_stable_and_single_use(self):
        replies = self.with_session(
            call(SEARCH, {"query": "wind", "limit": 5}, 1),
            call(SEARCH, {"query": "wind", "limit": 5, "cursor": "cursor-1"}, 2),
            call(SEARCH, {"query": "wind", "limit": 5, "cursor": "cursor-2"}, 3),
        )
        pages = [self.tool_payload(reply) for reply in replies]
        ids = [row["file_id"] for page in pages for row in page["results"]]
        self.assertEqual(ids, list(range(1, 15)))
        self.assertTrue(pages[0]["next_cursor"])
        self.assertTrue(pages[1]["next_cursor"])
        self.assertNotIn("next_cursor", pages[2])

        replies = self.with_session(
            call(SEARCH, {"query": "wind", "limit": 5}, 1),
            call(SEARCH, {"query": "wind-01", "limit": 5, "cursor": "cursor-1"}, 2),
            call(SEARCH, {"query": "wind", "limit": 5, "cursor": "cursor-1"}, 3),
            call(SEARCH, {"query": "wind", "limit": 5, "cursor": "not-issued"}, 4),
        )
        self.assertEqual(replies[1]["error"]["code"], -32602)
        self.assertEqual(self.tool_payload(replies[2])["results"][0]["file_id"], 6)
        self.assertEqual(replies[3]["error"]["code"], -32602)

    def test_authorization_filter_happens_before_cursor_paging(self):
        selected = self.root / "wind" / "selected"
        selected.mkdir(parents=True, exist_ok=True)
        sound = selected / "wind-selected.wav"
        sound.write_bytes(RAW_AUDIO)
        with closing(sqlite3.connect(self.database)) as connection:
            connection.execute(
                "INSERT INTO audio_files (id, file_path, file_name, extension, duration, title, description, category, keywords) VALUES (?, ?, ?, 'wav', ?, '', '', '', '')",
                (100, str(sound), sound.name, 1.0),
            )
            connection.commit()
        def cleanup_selected():
            with closing(sqlite3.connect(self.database)) as connection:
                connection.execute("DELETE FROM audio_files WHERE id = 100")
                connection.commit()
            sound.unlink(missing_ok=True)
            selected.rmdir()
        self.addCleanup(cleanup_selected)
        document = write_authorization(self.fixture / "selected.json", selected)
        flags = ["--library-db", str(self.database), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        replies = self.with_session(
            call(SEARCH, {"query": "wind", "limit": 5}, 1),
            flags=flags,
        )
        payload = self.tool_payload(replies[0])
        self.assertEqual([row["file_id"] for row in payload["results"]], [100])
        self.assertNotIn("next_cursor", payload)

    def test_modern_structured_content_does_not_duplicate_full_text_and_legacy_keeps_text(self):
        modern = self.with_session(call(SEARCH, {"query": "wind", "limit": 1}, 1))[0]
        modern_result = modern["result"]
        modern_payload = modern_result["structuredContent"]
        modern_text = modern_result["content"][0]["text"]
        self.assertLess(len(modern_text), len(json.dumps(modern_payload, ensure_ascii=False)))
        self.assertNotIn("wind-01.wav", modern_text)

        legacy = self.with_session(
            call(SEARCH, {"query": "wind", "limit": 1}, 1), version=VERSIONS[0])[0]
        legacy_result = legacy["result"]
        self.assertNotIn("structuredContent", legacy_result)
        self.assertEqual(json.loads(legacy_result["content"][0]["text"])["status"], "ok")

    def test_rejects_paths_unknown_fields_and_out_of_range_arguments(self):
        cases = [
            (SEARCH, {}), (FOLDERS, {"query": False}), (SEARCH, {"query": ""}),
            (SEARCH, {"query": "  "}), (SEARCH, {"query": "word\u0000word"}),
            (SEARCH, {"query": "a" * 257}), (SEARCH, {"query": "\u98ce" * 257}),
            (SEARCH, {"query": "wind", "limit": 0}),
            (SEARCH, {"query": "wind", "limit": 51}),
            (SEARCH, {"query": "wind", "limit": True}),
            (SEARCH, {"query": "wind", "limit": 10.0}),
            (SEARCH, {"query": "wind", "limit": 2**64}),
            (SEARCH, {"query": "wind", "path": str(self.database)}),
            (FOLDERS, {"query": "wind", "library": str(self.root)}),
            (SEARCH, {"query": "wind", "requestBody": "raw audio"}),
            (SEARCH, ["wind"]),
            (SIMILAR, {"reference_id": True}),
            (SIMILAR, {"reference_id": 0}),
            (SIMILAR, {"reference_id": -1}),
            (SIMILAR, {"reference_id": 2**63}),
        ]
        for start in range(0, len(cases), 10):
            group = cases[start:start + 10]
            replies = self.with_session(*[
                call(name, arguments, index) for index, (name, arguments) in enumerate(group, 1)
            ], request("ping", "still-alive"))
            self.assertEqual([reply["error"]["code"] for reply in replies[:-1]], [-32602] * len(group))
            self.assertEqual(replies[-1]["result"], {})

    def test_rejects_bad_frames_and_limits_consecutive_rejections(self):
        replies = self.with_session(
            b"{",
            b'{"jsonrpc":"2.0","id":2,"method":"\xff"}',
            b" " * (64 * 1024 + 1) + wire_line(request("ping", 3)).rstrip(b"\n"),
            wire_line(request("ping", 4)).rstrip(b"\n") + b"\x00",
            b'{"jsonrpc":"2.0","id":5,"method":"ping","params":{"_meta":{"deep":'
            + b"[" * 40 + b"null" + b"]" * 40 + b"}}}",
            b'[]',
            b'{"jsonrpc":"2.0","id":null,"method":"ping"}',
            request("ping", "recovered"),
        )
        self.assertEqual(
            [reply.get("error", {}).get("code") for reply in replies[:-1]],
            [-32700, -32700, -32700, -32700, -32700, -32600, -32600],
        )
        self.assertTrue(all(reply["id"] is None for reply in replies[:-1]))
        self.assertEqual(replies[-1]["id"], "recovered")
        completed = self.launch([b"{"] * 64 + [request("ping", 1)])
        self.assertNotEqual(completed.returncode, 0)
        errors = [json.loads(line) for line in completed.stdout.splitlines()]
        self.assertGreater(len(errors), 0)
        self.assertLess(len(errors), 64)
        self.assertTrue(all(reply["error"]["code"] == -32700 for reply in errors))
        self.assert_no_echo(completed.stdout, completed.stderr)


    def test_authorization_switch_and_ticked_roots_scope_results(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        base = ["--library-db", str(self.database), "--library-root", str(self.root),
                "--library-id", LIBRARY_ID]
        disabled = write_authorization(scratch / "disabled.json", self.root, enabled=False)
        replies = self.with_session(
            call(SEARCH, {"query": "wind", "limit": 5}, 1),
            flags=[*base, "--settings", str(disabled)],
        )
        denied = self.tool_payload(replies[0])
        self.assertEqual(denied, {"status": "not_authorized", "results": []})
        self.assertTrue(replies[0]["result"]["isError"])

        sub_only = write_authorization(scratch / "sub.json", self.root / "wind")
        replies = self.with_session(
            call(SEARCH, {"query": "wind", "limit": 50}, 1),
            call(FOLDERS, {"query": "wind"}, 2),
            flags=[*base, "--settings", str(sub_only)],
        )
        search = self.tool_payload(replies[0])
        self.assertEqual(search["status"], "ok")
        self.assertTrue(search["results"])
        for row in search["results"]:
            self.assertTrue(row["relative_path"].startswith("wind/"), row)
        folders = self.tool_payload(replies[1])
        self.assertEqual(folders["status"], "ok")
        self.assertTrue(folders["results"])
        for row in folders["results"]:
            self.assertEqual(row["relative_path"], "wind")

        no_folders = write_authorization(
            scratch / "no-folders.json", self.root, capabilities={**ALL_READ, "findFolders": "none"})
        replies = self.with_session(
            call(FOLDERS, {"query": "wind"}, 1),
            flags=[*base, "--settings", str(no_folders)],
        )
        self.assertEqual(self.tool_payload(replies[0]), {"status": "not_authorized", "results": []})

    def test_authorization_documents_are_bound_to_one_library(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        other_library = "harken-other-library"
        base = ["--library-db", str(self.database), "--library-root", str(self.root)]

        borrowed = write_authorization(scratch / "borrowed.json", self.root)
        completed = self.launch(
            [initialize()],
            flags=[*base, "--settings", str(borrowed), "--library-id", other_library],
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(completed.stdout, b"")
        self.assertIn(b"belongs to another library", completed.stderr)
        self.assert_no_echo(completed.stdout, completed.stderr)

        owned = write_authorization(scratch / "owned.json", self.root, library_id=other_library)
        replies = self.with_session(
            call(SEARCH, {"query": "wind", "limit": 3}, 1),
            flags=[*base, "--settings", str(owned), "--library-id", other_library],
        )
        self.assertEqual(self.tool_payload(replies[0])["status"], "ok")

        replies = self.with_session(call(SEARCH, {"query": "wind", "limit": 3}, 1))
        self.assertEqual(self.tool_payload(replies[0])["status"], "ok")

    def test_similar_reference_outside_ticked_roots_is_refused(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        document = write_authorization(scratch / "sub.json", self.root / "wind")
        flags = ["--library-db", str(self.database), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        replies = self.with_session(call(SIMILAR, {"reference_id": 40}, 1), flags=flags)
        self.assertEqual(self.tool_payload(replies[0]),
                         {"status": "reference_not_authorized", "results": []})

    def test_revocation_applies_to_next_call_in_a_live_session(self):
        scratch = Path(tempfile.mkdtemp(dir=self.fixture))
        document = write_authorization(scratch / "live.json", self.root)
        flags = ["--library-db", str(self.database), "--library-root", str(self.root),
                 "--settings", str(document), "--library-id", LIBRARY_ID]
        with subprocess.Popen(
            self.command(flags), cwd=self.fixture,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        ) as process:
            received = Queue()

            def pump():
                for line in iter(process.stdout.readline, b""):
                    received.put(line)

            reader = Thread(target=pump, daemon=True)
            reader.start()
            try:
                process.stdin.write(wire_line(initialize()))
                process.stdin.write(wire_line(request("notifications/initialized")))
                process.stdin.write(wire_line(call(SEARCH, {"query": "wind", "limit": 3}, 1)))
                process.stdin.flush()
                self.assertEqual(json.loads(received.get(timeout=5))["id"], "hello")
                first = json.loads(received.get(timeout=5))
                self.assertEqual(self.tool_payload(first)["status"], "ok")

                write_authorization(document, self.root, enabled=False)
                process.stdin.write(wire_line(call(SEARCH, {"query": "wind", "limit": 3}, 2)))
                process.stdin.flush()
                second = json.loads(received.get(timeout=5))
                self.assertEqual(self.tool_payload(second)["status"], "not_authorized")
                self.assertTrue(second["result"]["isError"])

                process.stdin.close()
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual(process.stderr.read(), b"")
            finally:
                if process.poll() is None:
                    process.kill()
                reader.join(timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
