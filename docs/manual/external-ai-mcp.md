# External AI: local read-only library search

The optional Windows `HarkenExternalAiMcp.exe` lets a local MCP client find sounds,
folders and same-kind substitutes in an explicitly chosen Harken library. Harken
does not start or enable this connector automatically. It opens no network port.

## Build and connect

From a Harken checkout with the Windows build prerequisites, build the separate
connector. This does not install it or change Harken settings:

```powershell
cmake -S . -B build_debug -G 'Visual Studio 17 2022' -A x64
cmake --build build_debug --config Debug --target HarkenExternalAiMcp
```

In a **local** Codex CLI, add the stdio connector. Replace the three paths.
The second path is an existing Harken library database. The last path is the
indexed audio folder you want to share, not the folder that stores the database.
If the database uses SQLite WAL, keep that library open in Harken while using
the connector. The connector refuses to recover or create missing WAL sidecars.

```powershell
codex mcp add harken -- 'C:\Harken Dev\HarkenExternalAiMcp.exe' --library-db 'C:\Harken Data\Library.db' --library-root 'D:\Sounds\Selected Library'
```

Start a new local session and try: “Find metal door sounds in my Harken library.
Show me their folders. Pick a result and find three same-kind substitutes.”
To revoke access, remove the `harken` server (`codex mcp remove harken`) and end
sessions that have loaded it.

## What the connector can do

- `harken_search_audio` matches terms in the existing library; it does not listen
  to audio or generate a new search index.
- `harken_find_folders` searches indexed folders under the authorized root.
- `harken_find_similar` starts from a returned `file_id` and uses only existing
  compatible same-kind analysis. No results and missing analysis are different
  responses. Match scores rank a bounded set of cached substitutes; they are not
  classification confidence or an exhaustive match across an entire large library.

Results contain only the fields needed for search and location. Paths are relative
to your chosen audio folder, so you can locate a result in Harken or Explorer.
The connector never sends audio, scans the disk for sounds, or exposes database
writes, file changes, or playback operations. The **client** determines whether
returned names and paths are sent to a cloud model. Share only a library section
whose filenames and directory names you are comfortable returning to that client.

This is a local connector, not a Codex Cloud endpoint or a Harken chat panel.
Automatic tagging, batch metadata edits and direct integration with a cloud
runner are not included.
