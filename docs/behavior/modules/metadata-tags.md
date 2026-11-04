# Metadata / Tags Behavior Contract

## Header

**ID:** BDD-METADATA-TAGS
**Module:** Metadata / Tags
**Status:** Active
**Priority:** P1
**Owner:** Engineering
**Created:** 2026-06-03
**Last Updated:** 2026-07-11

## Product Behavior

**User promise:** Metadata and tags can be inspected, normalized, edited, and searched without losing field meaning.
**Primary workflow:** User reviews fields, edits metadata, maps vocabulary, and expects search/tag surfaces to reflect normalized values.
**Non-goals:** This contract does not define external cloud AI tagging behavior, and F-006 P1 metadata edits do not write changes back into source audio files.

## 中文语义

**用户承诺：** 用户在元数据面板里看到、修改、保存并搜索字段时，BPM、year、ISRC、tag 等字段含义不会互相串线。
**核心流程：** 用户从结果列表打开 metadata editor，修改字段并保存；Aris 写入数据库、发出 patch 诊断、刷新相关列表/标签面，并让搜索能找到更新后的值。
**非目标：** 这份契约不定义外部云端 AI 打标行为，也不定义把修改写回源音频文件的完整文件标签写入策略。

## BDD Scenarios

### Happy Path

- Given 已满足以下前置条件：a file has metadata fields and tag vocabulary
- When 执行以下操作：the user opens the metadata editor, edits a field, saves, and searches for the new value
- Then 应得到以下结果：the edited field appears in the UI, the DB patch succeeds, and search/tag surfaces reflect the normalized value without changing unrelated fields

### Edge Cases

- Given 已满足以下前置条件：invalid BPM, year, ISRC, empty fields, or mixed vocabulary
- When 执行以下操作：metadata validation runs
- Then 应得到以下结果：invalid data is rejected or normalized without corrupting unrelated fields
- Given 已满足以下前置条件：multiple selected files have conflicting values for editable fields
- When 执行以下操作：the user opens the bulk metadata editor
- Then 应得到以下结果：each field shows the correct bulk state: same value, mixed values, or blank-overwrite-all, and the generated patch only edits fields the user explicitly chose

### Failure / Recovery

- Given 已满足以下前置条件：a metadata patch references a missing file or unsupported field
- When 执行以下操作：Aris applies the patch
- Then 应得到以下结果：the operation fails safely and leaves existing metadata unchanged
- Given 已满足以下前置条件：a bulk metadata edit includes files that disappeared or cannot be updated
- When 执行以下操作：the user previews and submits the patch
- Then 应得到以下结果：Aris reports success, skipped, and DB failure groups with file path and reason code, without reporting any source-file writeback dimension in F-006 P1

### Non-Functional

- Given 已满足以下前置条件：bulk metadata/tag operations
- When 执行以下操作：applying many rows
- Then 应得到以下结果：DB updates use batch plans without per-row SQL prepare churn and emit patch summaries for success/rejection counts.

### Preview / Report

- Given 已满足以下前置条件：the user prepares a bulk metadata patch
- When 执行以下操作：Aris shows the preview before commit
- Then 应得到以下结果：the preview includes selected count, patch intent summary, estimated updatable row count, and missing paths, with no "write back to file" statistic in F-006 P1

### Security

- Given 已满足以下前置条件：a metadata field contains quotes, separators, or SQL-like text
- When 执行以下操作：Aris applies the metadata patch
- Then 应得到以下结果：user-provided values are bound through the whitelist patch plan, never concatenated into raw SQL

## Interface Binding

| Interface / Entry | Kind | Source | Expected behavior |
| --- | --- | --- | --- |
| `MetadataFields` / `MetadataFieldManager` | API | Graphify | Defines field schemas and validation |
| `MetadataEditPatch` | DB | Graphify | Carries safe metadata edit intent |
| `TagSystem` | API | source scan | Owns tag classification and lookup |
| `TagVocabularyMapper` | API | tests | Maps vocabulary without one-way drift |
| metadata diagnostics | diagnostics | product semantics | Exposes patch success/rejection summaries for metadata edit flows |

## Graphify Binding

**Source graph:** `graphify-out/main/interface-call-graph.json` and `graphify-out/main/audit-call-graph.json`
**Impact target(s):** `MetadataFields`, `MetadataEditPatch`, `TagSystem`, `TagVocabularyMapper`
**Audit classification requirement:** metadata DB patch unknowns must be reviewed before completion.
**Dynamic calls:** review metadata container/lookup calls.
**Unknown calls:** review `MetadataFields.*`, `TagSystem.*`, `Database.cpp` metadata regions.

## TDD / QA Proof

| Proof ID | Kind | Artifact / command / event | Pass signal | Status |
| --- | --- | --- | --- | --- |
| TEST-METADATA-FIELDS | unit | `TestMetadataFields` | field schema and validation pass | active |
| TEST-TAG-SYSTEM | unit | `TestTagSystem` | tag behavior passes | active |
| TEST-TAG-VOCAB | unit | `TestTagVocabularyMapper` | vocabulary mapping passes | active |
| DIAG-METADATA | diagnostics | temporary evidence in `TestMultiTabSync`: this test drives the `MetadataEditPatch` path and asserts `PatchBatchComplete`, `PatchRejected`, and `PatchManyComplete` metadata diagnostics | patch success/rejection events observed | active |
| TEST-MAC-METADATA-TAGS | real Mac app / isolated diagnostics | `tools/macos_metadata_tags_smoke.py` | One-file patch, unrelated-field preservation, invalid-input rejection, tag and category search, source WAV hash, and reopen agree | active |

## Missing Coverage

- Missing proof: full UI flow from metadata editor save to search result refresh is not yet asserted end-to-end.
- The Mac isolated diagnostic action exercises the shared database patch and real result view; it does not click the metadata editor's Save button or prove that fields below the first Info panel viewport are visible.
- Missing proof: dedicated `TestMetadataPatchDiagnostics`; `TestMultiTabSync` is accepted as temporary diagnostics evidence only because it currently drives the metadata patch path.
- Residual gap: field editor UI ergonomics are still human-verified.
- Suspected stale proof: unsupported-field UX is not yet asserted end-to-end.
- Human-only verification: field editor UX.

## Change Gate

- [x] Refresh Graphify main and audit graphs with `python tools/graphify_auto_refresh.py --once`.
- [x] Re-run metadata/tag tests.
- [x] Review metadata `UNKNOWN_BLOCKING` entries.
- [x] Update this contract if metadata field or tag semantics change.

## Decision Log

| Date | Decision | Reason | Decider |
| --- | --- | --- | --- |
| 2026-06-03 | Reframed metadata BDD around the editor save/search workflow and clarified the `TestMultiTabSync` diagnostics proof. | Review found this module was too internal and the diagnostics reference looked like a copy error without event names. | Engineering |
| 2026-06-03 | Added F-006 bulk three-state, preview, report, and P1 no-source-file-writeback semantics. | F-006 design requires single-file and bulk editor behavior to be visible in the contract, not hidden behind DB patch internals. | Engineering |
