# Search Execution Behavior Contract

## Header

**ID:** BDD-SEARCH-EXECUTION
**Module:** Search Execution
**Status:** Active
**Priority:** P0
**Owner:** Engineering
**Created:** 2026-06-03
**Last Updated:** 2026-07-11

## Product Behavior

**User promise:** Search list, count, facets, playlist searches, and cover/path facets execute the same search meaning.
**Primary workflow:** User searches and filters a library, then trusts result count, list rows, facets, and pagination to agree.
**Non-goals:** Parser tokenization belongs to Search Query; this module owns SQL shape and execution consistency.

## 中文语义

**用户承诺：** 用户看到的结果数量、列表行、facets、播放列表内搜索和封面/路径筛选都来自同一套查询条件。
**核心流程：** 用户输入查询、勾选筛选、切换分页或查看 facets 时，Aris 的 list SQL、count SQL 和 facet SQL 使用同一组 parser lanes。
**非目标：** 这里不重新解释用户输入；Search Query 决定 token 含义，本模块只负责把含义安全、一致地落到数据库查询。

## BDD Scenarios

### Happy Path

- Given 已满足以下前置条件：a parsed query with recall and filter lanes
- When 执行以下操作：Aris builds SQL and fetches result count, pages, and facets
- Then 应得到以下结果：result count equals the number of rows available through the paged list, and filters applied in list SQL also appear identically in count and facet SQL

### Edge Cases

- Given 已满足以下前置条件：a query uses normalized genre or instrument fields
- When 执行以下操作：search execution applies filters
- Then 应得到以下结果：fielded filters bind to normalized columns instead of broad keyword false positives
- Given 已满足以下前置条件：a bare keyword query matches only opaque `search_data` extension text and none of the explainable core text fields
- When 执行以下操作：Aris builds the candidate set for list/count/facet execution
- Then 应得到以下结果：that row is excluded so unexplainable hidden-metadata hits do not appear in the result set
- Given 已满足以下前置条件：parser lanes include keyword, exclude, path, tag, and facet filters
- When 执行以下操作：Aris builds list, count, facet, playlist, and chip-visible execution paths
- Then 应得到以下结果：each lane maps to one explicit SQL where fragment or documented no-op, with no lane silently dropped

### Failure / Recovery

- Given 已满足以下前置条件：the DB is open but a query cannot be expressed by the current where-builder
- When 执行以下操作：search execution builds its where clause
- Then 应得到以下结果：it fails closed or returns a safe empty/fallback result without changing the user's query chips

### Security

- Given 已满足以下前置条件：a query contains quotes, SQL metacharacters, or operator-like text
- When 执行以下操作：search execution binds it into SQL
- Then 应得到以下结果：user text is passed through prepared-statement binding or safe FTS escaping, never raw SQL string concatenation

### Non-Functional

- Given 已满足以下前置条件：a large library
- When 执行以下操作：search executes after debounce
- Then 应得到以下结果：`Database.searchFiles` remains under 100ms for the QA search fixture and list/count paths do not diverge
- Given 已满足以下前置条件：a database-view keyword refresh also needs count and facets
- When 执行以下操作：Aris executes the async refresh
- Then 应得到以下结果：the first paged result rows are not blocked behind filtered facet warm-start; count and facets may follow after the result page lands

## Interface Binding

| Interface / Entry | Kind | Source | Expected behavior |
| --- | --- | --- | --- |
| `Database::buildSearchWhere()` | DB | Graphify | Single SQL-shape authority for search semantics |
| `searchFiles()` / `searchFilesCount()` | DB | Graphify | List/count apply same constraints |
| `searchPlaylistFileCount()` / playlist search APIs | DB | Graphify | Playlist search uses same where semantics |
| `getCoverFacets()` / `getPathFacets()` | DB | Graphify | Facets reflect current search/filter state |
| `SearchWorker::workerThread()` keyword refresh path | worker | Graphify + source | Keyword result rows are returned before secondary count/facet warm-start work |
| `Database.searchFiles` | diagnostics | QA protocol | Runtime performance evidence |

## Graphify Binding

**Source graph:** `graphify-out/main/interface-call-graph.json` and `graphify-out/main/audit-call-graph.json`
**Impact target(s):** `buildSearchWhere`, `searchFiles`, `searchFilesCount`
**Audit classification requirement:** DB helper unknowns must be reviewed because SQL lane drift is high risk.
**Dynamic calls:** review DB container method calls in `DYNAMIC_CALLS.md`.
**Unknown calls:** review `Database.cpp` entries in `UNKNOWN_CALLS.md`.

## TDD / QA Proof

| Proof ID | Kind | Artifact / command / event | Pass signal | Status |
| --- | --- | --- | --- | --- |
| TEST-DB-SEARCH-FACETS | integration | `TestDbSearchFacets` | list/count/facet filter semantics pass | active |
| TEST-TRIGRAM-E2E | integration | `TestTrigramE2E` | trigram recall path passes | active |
| TEST-BROWSER-SEARCH | integration | `TestBrowserSearch` | browser/search flow passes | active |
| TEST-FUZZY-SEARCH-PRD | integration | `TestFuzzySearchPrd` | CJK, pinyin, initials, path, and snippet fuzzy-search behavior passes | active |
| TEST-KEYWORD-ROWS-FIRST | contract | `TestDbSearchFacets` | async keyword refresh excludes keyword from filtered facet warm-start | active |
| DIAG-DB-SEARCH | diagnostics | `category:"Database.searchFiles"` | duration and event observed | active |

## Missing Coverage

- Missing proof: one explicit matrix connecting parser lane, SQL where lane, list/count lane, facet lane, and chip lane.
- Suspected stale proof: tests that assert only list rows but not count/facet sameness.
- Human-only verification: perceived search relevance.

## Change Gate

- [x] Refresh Graphify main and audit graphs with `python tools/graphify_auto_refresh.py --once`.
- [x] Re-run search execution tests.
- [x] Review `Database.cpp` `UNKNOWN_BLOCKING` entries touching search execution.
- [x] Update this contract if SQL shape or search/filter semantics change.

## Decision Log

| Date | Decision | Reason | Decider |
| --- | --- | --- | --- |
| 2026-06-03 | Split SQL-injection handling into a Security scenario and made list/count/facet sameness concrete. | Review flagged the old happy-path and failure wording as too abstract and misclassified. | Engineering |
