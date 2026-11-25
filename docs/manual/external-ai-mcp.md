# External AI: local library search, metadata edits and Playlist writing

The optional Windows `HarkenExternalAiMcp.exe` lets a local MCP client find sounds,
folders and same-kind substitutes in an explicitly chosen Harken library. With the
separate metadata permissions enabled, it can also preview and apply a confirmed
metadata proposal for one audio file. With the Playlist permissions enabled it can
add the files of one confirmed Game SFX classification to Harken Playlists.
Harken does not start or enable this connector automatically. It opens no network
port.

## Connect from the app (recommended)

1. Open **Settings -> External AI (MCP)** and switch on “allow external AI”.
2. Add the folders the client may see and tick each one. Unticked folders are never
   readable and never writable.
3. Under per-action grants choose the level. For Playlist writing, *read playlists*
   must be at least readable and *edit playlists* must be editable.
4. Click **Save this library**.
5. In **Connected apps**, click **Connect** on Cindy (Codex) or standalone Codex,
   then open a new session in that client. Cindy's Codex engine and standalone Codex
   use separate configuration directories. A standalone Pi configuration is not
   Cindy's desktop configuration.

**Disconnect** revokes that app's managed link, including requests made by its
already-open sessions after disconnection. Other clients are unaffected.
Reconnecting creates a new link; start a new session to use it. Switching off
**Allow external AI** revokes library access for all clients without clicking
**Save this library**. Other unsaved changes remain drafts. Neither operation
retracts results that a client already received.

Each connection uses one launch root. If the selected directories span drives,
choose directories on one drive before creating a new connection. An existing
connection can still be disconnected.

Harken keeps a local write bridge for the open library while those grants are
active; the connector asks Harken to write, so Playlist writes happen in Harken's
own transaction instead of by editing the database from a second process.

## Build and connect manually

From a Harken checkout with the Windows build prerequisites, build the separate
connector. This does not install it or change Harken settings:

```powershell
cmake -S . -B build_debug -G 'Visual Studio 17 2022' -A x64
cmake --build build_debug --config Debug --target HarkenExternalAiMcp
```

In a **local** Codex CLI, add the stdio connector. Replace the example paths
and library ID. The launch root is the indexed audio folder you want to share,
not the folder that stores the database. Find the authorization document and
library ID in the setting page's connection details.
If the database uses SQLite WAL, keep that library open in Harken while using
the connector. The connector refuses to recover or create missing WAL sidecars.

```powershell
codex mcp add harken -- 'C:\Harken Dev\HarkenExternalAiMcp.exe' --library-db 'C:\Harken Data\Library.db' --library-root 'D:\Sounds\Selected Library' --settings 'C:\Harken Data\external-ai\example.json' --library-id 'example-library-id'
```

The connector also requires `--settings <authorization document>` and `--library-id <library identity>`.
It refuses to start when the document is missing, malformed or belongs to another library,
and re-reads the document for every tool call.

Start a new local session and try: “Find metal door sounds in my Harken library.
Show me their folders. Pick a result and find three same-kind substitutes.”
To revoke access immediately for manual or legacy links, turn off the library's
**Allow external AI** switch. You can then remove the `harken` server
(`codex mcp remove harken`) and end any sessions that loaded it.

## What the connector can do

- `harken_search_audio` matches terms in the existing library; it does not listen
  to audio or generate a new search index.
- `harken_preview_playlist_pack` returns a Game SFX classification list (target
  child Playlist, confidence, reason, whether review is needed). It writes nothing.
- `harken_apply_playlist_pack` applies one confirmed proposal. Harken performs the
  write in one transaction, so a failure leaves no partial Playlist, and repeating
  the same confirmation does not add a second copy. It fails closed when Harken is
  not running or the Playlist grants are closed.
- `harken_find_folders` searches indexed folders under the authorized root.
- `harken_find_similar` starts from a returned `file_id` and uses only existing
  compatible same-kind analysis. No results and missing analysis are different
  responses. Match scores rank a bounded set of cached substitutes; they are not
  classification confidence or an exhaustive match across an entire large library.
- `harken_get_metadata` reads the current title, description, category and keywords
  for one authorized `file_id`.
- `harken_preview_metadata_update` accepts one file and a proposed set of those
  four metadata fields. It returns `before`, `after` and a session-scoped
  `proposal_id`; it never writes.
- `harken_apply_metadata_update` accepts only that `proposal_id`. Before writing,
  it re-reads the authorization document, checks the current database and path,
  verifies the original values are unchanged, and requires an explicit client
  confirmation step.

### Game SFX Playlist preview

`harken_preview_playlist_pack` is the first `game_sfx` domain workflow. It
accepts `domain=game_sfx`, a main Playlist name, optional style/theme, and an
authorized search query or explicit file IDs. It proposes a main Playlist plus
taxonomy child Playlists using the versioned `game_sfx.v1` profile. The profile
uses `Gameplay Feedback`, `Character & Creature`, `Combat & Ability`, `World &
Props`, `UI & System`, `Environment & Foley`, `Cinematic & Transition` and
`Review Needed`.

The preview reports `playlist_path`, `confidence`, `reason`, `source` and
`needs_review` for every selected file. Music, stems, BPM/tempo/key material
and complete tracks are excluded; the connector does not classify by file
format, duration or frequency alone. Playlist is the output object: files are
not copied, moved, renamed or exported. The profile and evidence contract live
in `docs/manual/game-sfx-profile.md`.

`harken_apply_playlist_pack` requires the preview `proposal_id`,
`confirm=true`, a `readPlaylist` grant and an editable `editPlaylist` grant.
It rechecks the library, paths, metadata snapshots and authorization, then
returns `playlist_write_unavailable` without writing: no trusted host
connection for Playlist writes is available. Opening Harken alongside the
connector does not enable this operation. The connector never falls back to
direct writes to Playlist tables.

Search, folder and similar calls use a default `limit` of 5 and accept an
explicit limit from 1 through 50. Their result rows can be narrowed with an
optional `fields` array. The allowed fields are:

- Audio search: `file_id`, `name`, `relative_path`, `duration_seconds`.
- Folder search: `relative_path`, `file_count`.
- Similar search: the audio search fields plus `match_score`.

An unknown field, duplicate field or empty field list is a JSON-RPC invalid
parameters error. Field selection only changes the response shape; it never
changes the database query or authorization checks. The server authorizes rows
first and paginates second. When more rows are available, the response includes
an opaque `next_cursor`. Send that cursor with the same tool and query to get
the next page. Cursors are session-scoped, single-use and cannot be reused for
another query or tool; invalid, expired or cross-query cursors fail safely.
Result ordering remains stable: audio and similar rows use their existing
stable ranking/order, and folders use binary path order.

For protocol versions that support structured tool results (`2025-06-18` and
newer), the complete payload is returned in `structuredContent` and the text
content is only a short compatibility summary. Older negotiated versions keep
the complete JSON payload in `content[].text`. This avoids sending the same
large result twice while keeping older MCP clients parseable.

Results contain only the fields needed for search, location and the requested
metadata preview. Paths are relative to your chosen audio folder, so you can
locate a result in Harken or Explorer. The connector never sends audio, scans
the disk for sounds, or renames files, embeds tags, edits audio content, or
changes length, trimming or pitch. The **client** determines whether returned
names, paths and metadata are sent to a cloud model. Share only a library
section whose filenames, directory names and metadata you are comfortable
returning to that client.

The AI suggestion itself is produced by the connected client; the connector
does not call a model. This is a local connector, not a Codex Cloud endpoint or
a Harken chat panel. Batch metadata edits, file renaming and direct integration
with a cloud runner are not included.

## Authorization document (backend)

Each library keeps its own authorization document. The connector must load the
document for the database it was pointed at and check every request against it;
a document copied from another library never matches.

```json
{
  "libraryId": "<database identity>",
  "enabled": true,
  "sampleLibrary": false,
  "roots": ["D:/Sounds/Selected Library"],
  "capabilities": {
    "searchAudio": "read",
    "findSimilar": "read",
    "findFolders": "none",
    "readFileName": "read",
    "readMetadata": "read",
    "editMetadata": "none",
    "aiNamingMetadata": "read",
    "readPlaylist": "read",
    "editPlaylist": "none"
  }
}
```

- `enabled` is the master switch. When it is off, every capability is denied
  even if roots and grants are still saved.
- `roots` are only the indexed directories the user ticked. An empty list
  grants nothing; adding a path to the file is not the same as authorizing it.
- Each capability is `none`, `read` or `edit`. File names and library
  metadata are checked separately, so one can be shared while the other stays
  closed. A preview needs `readMetadata` and `aiNamingMetadata` at least
  at read level. Applying a proposal needs both `editMetadata` and
  `aiNamingMetadata` at edit level.
- A game SFX Playlist preview needs `readPlaylist` plus the filename/metadata
  reads used for classification. Applying a pack additionally needs
  `editPlaylist`; `editMetadata` never substitutes for Playlist permission.
- A request is allowed only if its canonical target stays inside a ticked root.
  Sibling folders with a shared name prefix, `..` components, relative paths
  and junctions or symlinks that resolve outside the root are denied.
- Revoking a root or lowering a capability applies to the next request even
  while the MCP client keeps the connector process alive. Results outside a ticked root are dropped from every response.
- `sampleLibrary` stays read-only; an `edit` grant cannot widen it. The
  connector never applies metadata writes to it, so configuration never
  substitutes for the sample-library safety boundary.

Malformed documents are rejected as a whole: unknown fields, misspelled
capability names and unknown access levels fail instead of silently granting
access. `src/ExternalAiAuthorization.cpp` implements these checks and
`tests/test_external_ai_authorization.cpp` covers them.


## Settings page
Harken's settings dialog has an **External AI (MCP)** page. It reads and writes the
same document the connector loads:
- The page shows the library ID and the document path, and prints the `codex mcp add`
  command with the connector, database, settings and library ID already filled in.
  Copy it into a local terminal.
- Tick only the folders the AI client may see. Removing a folder or turning the
  master switch off applies to the client's next request, even when the client
  keeps the connector process alive.
- Each capability is set to not allowed, read-only or editable. File names and
  metadata are separate switches. The AI naming and metadata path is still
  preview-first and confirmation-required.
- A database that has never been authorized starts closed: no folder is ticked and
  every capability is not allowed.
The library ID is derived from the canonical database path (`lib-` plus a hash).
The same database keeps the same ID across restarts, and moving the file gives it a
new ID that needs its own authorization document.

