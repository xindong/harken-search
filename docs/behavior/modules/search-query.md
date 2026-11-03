# Search Query Behavior Contract

## Header

**ID:** BDD-SEARCH-QUERY
**Module:** Search Query
**Status:** Active
**Priority:** P0
**Owner:** Engineering
**Created:** 2026-06-03
**Last Updated:** 2026-07-11

## Product Behavior

**User promise:** A search string means the same thing in parser output, visible chips, and downstream execution.
**Primary workflow:** User enters mixed intent in the search box, edits visible chips, and expects predictable recall/filter behavior.
**Non-goals:** This module does not own async freshness or DB execution ordering.

## 中文语义

**用户承诺：** 搜索框里的每个词、引号、排除项和筛选 chip 都只表达用户输入的那一层意思，不会在解析和显示之间被悄悄改写。
**核心流程：** 用户输入 `war gun -explosion tag:impact` 这类混合查询，Aris 生成可移除 chips；用户移除某个 chip 后，其余关键词、排除项和字段筛选保持原样。
**非目标：** 这里不决定数据库何时返回，也不裁决异步结果的新旧，只保证“输入语义 -> chip -> 执行参数”的含义一致。

词库设置中的“合并”按钮在 Mac 上选择 JSON 后沿用已有规则追加和保存；取消选择时不修改现有词库。它不改变查询解析或 JSON 格式。

## BDD Scenarios

### Happy Path

- Given 已满足以下前置条件：a user enters text with keywords, quoted terms, field filters, and removable chips
- When 执行以下操作：Aris parses the search input
- Then 应得到以下结果：parser lanes and visible chips preserve the user's search intent without collapsing everything into a generic keyword

### Edge Cases

- Given 已满足以下前置条件：the query contains negative terms, path filters, tag filters, or color filters
- When 执行以下操作：chips are built and one chip is removed
- Then 应得到以下结果：removing the `-explosion` chip from `war gun -explosion` leaves the query text as `war gun`, with both positive tokens preserved in order
- Given 已满足以下前置条件：the user turns on exact search and enters a bare token such as `car`
- When 执行以下操作：Aris parses the search input
- Then 应得到以下结果：it must not add implicit prefix, synonym, or trigram substring widening; `card` should only match if the user explicitly asked for broader matching
- Given the user enters CJK, pinyin, or initial-letter search terms such as `枪`, `qiang`, or `q`
- When 执行以下操作：Aris parses the search input
- Then 应得到以下结果：parser lanes preserve those recall modes distinctly so downstream execution can test CJK, pinyin, and initials without treating them as a generic malformed token
- Given 已满足以下前置条件：文件名里有基本区汉字（U+4E00 到 U+9FFF）
- When 执行以下操作：Aris builds search tokens
- Then 应得到以下结果：each character with a Unihan kMandarin reading contributes its own syllable and initial, and a run of readable characters also contributes the joined pinyin. `汽车` is indexed as `汽 车 qi che qiche qc`. Characters outside that reading table do not erase the syllables of neighboring characters. The old 82-character hardcoded list is not the coverage limit.

### Failure / Recovery

- Given 用户在 Mac 设置页打开词库合并文件选择器
- When 用户取消选择
- Then 现有词库保持不变；若选择可读取的 JSON，则追加并保存解析出的规则

- Given 已满足以下前置条件：the query is malformed or operator-only
- When 执行以下操作：Aris parses it
- Then 应得到以下结果：it falls back to safe query behavior without SQL string injection or UI chip corruption

### Non-Functional

- Given 已满足以下前置条件：repeated search edits under large-library assumptions
- When 执行以下操作：parsing runs on every debounce cycle
- Then 应得到以下结果：parsing remains pure CPU work with no DB, file I/O, or AppState mutation

## Interface Binding

| Interface / Entry | Kind | Source | Expected behavior |
| --- | --- | --- | --- |
| `ParseSearchQueryToFts5()` | API | Graphify + semantic card | Produces stable search intent lanes |
| `BuildSearchQueryChips()` | API | Graphify | Produces removable visible query chips |
| `RemoveSearchQueryChipFromText()` | API | source scan | Removes only the selected chip source token |
| Search box / active chips | UI | product semantics | Displays and edits query intent without semantic drift |
| `OpenThesaurusJsonFileDialogUtf8()` | API | src/platform/thesaurus_json_dialog_macos.mm | Selects a file and reuses the shared merge path; empty selection is a no-op |
| `SearchQueryResult::*` | API | source scan | Carries FTS, trigram, path, tag, color, and exclude lanes |

## Graphify Binding

**Source graph:** `graphify-out/main/interface-call-graph.json` and `graphify-out/main/audit-call-graph.json`
**Impact target(s):** `SearchQuery`, `BuildSearchQueryChips`, `RemoveSearchQueryChipFromText`
**Audit classification requirement:** module-relevant `UNKNOWN_BLOCKING` entries must be reviewed before completion.
**Dynamic calls:** review `DYNAMIC_CALLS.md` for `SearchQuery` paths.
**Unknown calls:** review `UNKNOWN_CALLS.md` for `SearchQuery` paths.

## TDD / QA Proof

| Proof ID | Kind | Artifact / command / event | Pass signal | Status |
| --- | --- | --- | --- | --- |
| TEST-SEARCH-MATRIX | contract | `ctest --test-dir build_dbg -C Debug -R TestSearchSemanticMatrix --output-on-failure` | parser lane expectations pass | active |
| TEST-SEARCH-QUERY-TRIGRAM | unit | `TestSearchQueryTrigram` | trigram query behavior passes | active |
| TEST-SEARCH-RECALL | integration | `TestSearchRecall` | recall fixtures pass | active |
| TEST-THESAURUS-BIDIRECTIONAL | unit | `TestThesaurusBidirectional` | synonym expansion and exactMode suppression pass | active |
| TEST-THESAURUS-DIALOG | contract / runtime | `tests/test_thesaurus_json_dialog_boundary.py` and isolated Mac chooser probe | Mac UI reaches the native chooser; cancel returns empty, selected path is UTF-8 | active |
| DIAG-SEARCH-LANE | diagnostics | `SearchLane` | lane event observed for runtime query families | missing automatic assertion |

## Missing Coverage

- Missing proof: automatic `SearchLane` assertion for every major query family; this matches `DIAG-SEARCH-LANE` in TDD / QA Proof.
- Suspected stale proof: any test that only checks parser output without DB execution pairing.
- Human-only verification: visual chip edit ergonomics.

## Change Gate

- [x] Refresh Graphify main and audit graphs with `python tools/graphify_auto_refresh.py --once`.
- [x] Re-run search parser and recall tests.
- [x] Review SearchQuery entries in `UNKNOWN_CALLS.md`.
- [x] Update this contract if query meaning or chip behavior changes.

## Decision Log

| Date | Decision | Reason | Decider |
| --- | --- | --- | --- |
| 2026-06-03 | Treat chip removal and CJK/pinyin/initial lanes as explicit BDD behavior. | Search-query review found parser tests covered these cases but the user-facing contract did not name them. | Engineering |
