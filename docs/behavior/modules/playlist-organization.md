# Playlist Organization Behavior Contract

## Header

**ID:** BDD-PLAYLIST-ORGANIZATION
**Module:** Playlist Organization
**Status:** Active
**Priority:** P1
**Owner:** Engineering
**Created:** 2026-06-03
**Last Updated:** 2026-07-11

## Product Behavior

**User promise:** Playlists, folders, sub-playlists, and quick actions organize sound assets without losing parent/child or search context.
**Primary workflow:** User adds files to playlists, nests folders, filters/searches within playlists, or imports a REAPER FileList, and expects hierarchy to remain stable.
**Non-goals:** This contract does not define external DAW delivery behavior.

## 中文语义

**用户承诺：** 用户组织播放列表时，文件不会因为移动、搜索、折叠展开或删除列表而失踪、重复或跑到错误父级。
**核心流程：** 用户创建播放列表/文件夹、加入文件、嵌套子列表、在播放列表内搜索，或选择 REAPER FileList 导入新列表，再用 quickbar 或右键菜单移动/删除；Aris 保持层级和聚合视图一致。
**非目标：** 这份契约不定义外部 DAW 交付行为，也不负责导出文件本身。

## BDD Scenarios

### Happy Path

- Given 已满足以下前置条件：a user creates folders and playlists
- When 执行以下操作：files are added, moved, nested, or searched
- Then 应得到以下结果：playlist hierarchy and aggregated file views remain consistent
- Given 已满足以下前置条件：a user searches inside a selected playlist or folder
- When 执行以下操作：Aris applies playlist-scoped search
- Then 应得到以下结果：results include only files reachable from that playlist/folder hierarchy while preserving the global search semantics for keywords and filters

### Edge Cases

- Given a REAPER FileList contains repeated rows, relative paths, and missing files
- When the user imports it into a playlist
- Then the imported list preserves unique file order and reports missing files without modifying source audio
- Given REAPER FileList selection is cancelled
- When the native file panel closes
- Then no import scan starts and the current library remains unchanged

- Given 已满足以下前置条件：a playlist has nested children or an expanded folder state
- When 执行以下操作：drag/drop, rename, or parent changes occur
- Then 应得到以下结果：the same expanded nodes remain expanded by ID, moved children report the new parent ID, and unrelated branches are unchanged

### Failure / Recovery

- Given 已满足以下前置条件：a playlist operation conflicts with an invalid parent or missing file
- When 执行以下操作：the operation is applied
- Then 应得到以下结果：it fails safely without orphaning children or duplicating file entries
- Given 已满足以下前置条件：a user deletes a playlist that still contains files or child playlists
- When 执行以下操作：Aris applies the destructive operation
- Then 应得到以下结果：the operation requires the destructive path, removes only playlist membership/hierarchy records, and never deletes source audio files

### Non-Functional

- Given 已满足以下前置条件：large playlist contents
- When 执行以下操作：context menus or quick actions open
- Then 应得到以下结果：UI work stays within the 16ms frame budget and avoids full-list synchronous traversal on menu open.

## Interface Binding

| Interface / Entry | Kind | Source | Expected behavior |
| --- | --- | --- | --- |
| playlist DB APIs | DB | source scan | Own playlist/folder hierarchy persistence |
| sub-playlist helpers | API | tests | Preserve child aggregation and parent semantics |
| playlist quickbar | UI | tests | Keeps movement/selection stable |
| aggregated playlist search APIs | DB | Graphify via search execution | Apply search semantics within playlist context |
| playlist diagnostics | diagnostics | product semantics | Exposes hierarchy apply/reject diagnostics for parent changes |
| REAPER FileList selection and import | UI/API | platform dialog and shared workflow | Opens a real native picker, then imports only the selected file into a new playlist |

## Graphify Binding

**Source graph:** `graphify-out/main/interface-call-graph.json` and `graphify-out/main/audit-call-graph.json`
**Impact target(s):** playlist DB APIs, aggregated playlist search APIs, sub-playlist helpers
**Audit classification requirement:** playlist DB hierarchy unknowns must be reviewed before completion.
**Dynamic calls:** review UI/context-menu paths in `DYNAMIC_CALLS.md`.
**Unknown calls:** review playlist-related `Database.cpp` and UI unknowns.

## TDD / QA Proof

| Proof ID | Kind | Artifact / command / event | Pass signal | Status |
| --- | --- | --- | --- | --- |
| TEST-SUB-PLAYLISTS | integration | `TestSubPlaylists` | nested playlist behavior passes | active |
| TEST-REAPER-FILELIST | integration/boundary | `TestSubPlaylists`, `TestReaperFileListDialogBoundary` | import counts/order and platform picker boundary pass | active |
| TEST-PLAYLIST-FOLDER-TREE | unit/integration | `TestPlaylistFolderTree` | folder hierarchy passes | active |
| TEST-SET-PLAYLIST-PARENT | unit | `TestSetPlaylistParent` | parent updates pass | active |
| TEST-PLAYLIST-QUICKBAR | UI model | `TestPlaylistQuickBarMotion` | quickbar motion behavior passes | active |
| DIAG-PLAYLIST-PARENT | diagnostics | `TestSetPlaylistParent` diagnostics assertions | parent success/reject events observed | active |

## Missing Coverage

- Missing proof: destructive delete flow with populated playlist/folder is not yet asserted end-to-end.
- Suspected stale proof: UI-only operations not represented in model tests.
- Human-only verification: drag/drop ergonomics.
- Missing proof: Mac actual menu click and selected FileList in a full product package remain pending while the QA Mac entry is still a Demo.

## Change Gate

- [x] Refresh Graphify main and audit graphs with `python tools/graphify_auto_refresh.py --once`.
- [x] Re-run playlist hierarchy tests.
- [x] Review playlist `UNKNOWN_BLOCKING` entries.
- [x] Update this contract if playlist hierarchy or aggregation semantics change.

## Decision Log

| Date | Decision | Reason | Decider |
| --- | --- | --- | --- |
| 2026-06-03 | Added playlist-scoped search and destructive populated-playlist delete to BDD. | Review identified these as user-sensitive flows missing from the contract. | Engineering |
| 2026-11-09 | Bind REAPER FileList import to the playlist contract on Mac. | The shared importer already exists, but the Mac menu and file picker lacked a usable path. | Cindy |
