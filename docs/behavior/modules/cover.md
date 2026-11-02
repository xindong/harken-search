# Cover Behavior Contract

## Header

**ID:** BDD-COVER
**Module:** Cover
**Status:** Active
**Priority:** P0
**Owner:** Engineering
**Created:** 2026-06-03
**Last Updated:** 2026-07-11

## Product Behavior

**User promise:** Cover wall, cover filters, and current preview agree on identity while texture loading follows safe source-path rules.
**Primary workflow:** User browses artwork, clicks cover facets, previews current file art, and expects identity to remain stable.
**Non-goals:** Cover key is not a substitute for source path when reading real image bytes.

## 中文语义

**用户承诺：** 用户在封面墙点到的封面、筛选出来的文件和当前文件预览必须指向同一类真实 artwork，不会因为 hash 去重或缓存复用读错图片。
**核心流程：** 用户浏览封面墙、点击封面筛选、查看当前文件封面；Aris 用 `cover_key` 做身份/去重，但读取真实图片字节时必须回到对应 `src_path`。
**非目标：** `cover_key` 不是文件路径，不能被当成读取图片的来源，也不能替代源音频/图片路径做安全判断。

## BDD Scenarios

### Happy Path

- Given 已满足以下前置条件：a result set contains files with embedded, folder, or cached artwork
- When 执行以下操作：Aris builds cover wall entries and current preview
- Then 应得到以下结果：identity and filter behavior match while actual reads use the correct source path

### Edge Cases

- Given 已满足以下前置条件：legacy rows and new rows share or differ in cover hash
- When 执行以下操作：cover facets and preview resolve
- Then 应得到以下结果：de-duplication does not collapse unrelated source images or split identical identity incorrectly

### Non-blocking Preview and Prefetch

- Given 已满足以下前置条件：a file has no primed embedded-cover identity or needs source validation
- When 执行以下操作：the list prefetch or current-file preview requests its cover
- Then 应得到以下结果：the UI returns cached artwork or a placeholder immediately; file timestamps, artwork discovery, downscaling and thumbnail persistence run on the cover worker's database connection
- Given 已满足以下前置条件：a discovery request is pending or completed without artwork
- When 执行以下操作：successive UI frames request the same file
- Then 应得到以下结果：pending work is deduplicated and completed checks are reused for 30 seconds; explicit file invalidation bypasses the cooldown
- Given 已满足以下前置条件：the file is invalidated, caches are reset, or the request generation is cancelled
- When 执行以下操作：an old background discovery completes
- Then 应得到以下结果：its ticket cannot publish over newer identity state
- Verification: `TestCoverIdentityRefresh` and `tests/test_cover_identity_ui_boundary.py`.

### Failure / Recovery

- Given 已满足以下前置条件：a cover texture load fails or source art is missing
- When 执行以下操作：Aris retries or falls back
- Then 应得到以下结果：failure state is visible and does not trigger repeated synchronous I/O every frame

### Recovery After Partial Scan

- Given 已满足以下前置条件：a scan is stopped after records are committed and deferred cover backfill later writes cover keys
- When 执行以下操作：the file list can already show those row covers
- Then 应得到以下结果：the full-library cover wall invalidates its database cache and reloads the same cover identities
- Given 已满足以下前置条件：the user refreshes the global database view through the toolbar, menu, or F5
- When 执行以下操作：Aris reloads the current library after clearing filters
- Then 应得到以下结果：the full-library cover wall invalidates its database cache, reloads all current cover identities, and clears retry cooldowns so visible tiles can keep filling
- Given 已满足以下前置条件：the global database view is showing the current file list, including libraries larger than 50,000 files
- When 执行以下操作：the user refreshes and the full-library cover wall reloads
- Then 应得到以下结果：the visible wall follows first-seen covers in the currently loaded file-list rows, using the same CachedKeys path as a folder click, rather than registry/cover-key order, folder regrouping, or a full million-row scan
- Given 已满足以下前置条件：the global database view contains rows with `has_cover = 1` before folder/embedded cover keys are fully primed
- When 执行以下操作：cover wall or global cover facets must decide whether the registry is sparse
- Then 应得到以下结果：small-library fallback counts those rows as visible cover candidates and emits temporary folder-cover identities instead of truncating the wall to the stale registry footprint
- Given 已满足以下前置条件：a filtered result set already shows folder covers in the list column before folder cover keys are fully primed
- When 执行以下操作：the filtered cover wall falls back to `searchResults`
- Then 应得到以下结果：it still emits temporary folder-cover identities so the wall does not render as empty while waiting for cached keys

### Non-Functional

- Given 已满足以下前置条件：fast scrolling or cover wall interaction
- When 执行以下操作：background cover filling is active
- Then 应得到以下结果：background cover work obeys the 16ms frame-budget policy, caps visible texture uploads during interaction, and does not compete with current preview or cover-click feedback
- Given 已满足以下前置条件：a keyword or filtered search has submitted a facet follow-up
- When 执行以下操作：the first result rows arrive before the final cover facets
- Then 应得到以下结果：the cover wall keeps the last stable visual wall instead of flashing through an empty wall or transient search fallback
- Given 已满足以下前置条件：a keyword-filtered cover wall is using large thumbnails
- When 执行以下操作：the wall is idle after the user finishes typing
- Then 应得到以下结果：the current visible band receives a bounded large-thumbnail warmup budget so black placeholders fill without stealing budget from active scrolling, playback, scanning, waveform rebuild, or DB switching
- Given 已满足以下前置条件：a keyword-filtered cover wall is using small thumbnails with many visible cells
- When 执行以下操作：the wall is idle after the user finishes typing
- Then 应得到以下结果：the fill budget scales to the current visible/prefetch tile count instead of leaving the lower rows black while only the first few cells load
- Given 已满足以下前置条件：the cover wall owns foreground cover-loading priority
- When 执行以下操作：the result-list cover column re-renders rows beside it
- Then 应得到以下结果：the list may reuse already-cached cover textures but must not enqueue new cover work or blank the whole cover column
- Given 已满足以下前置条件：background cover backfill commits new ready covers while the user is already browsing a stable cover wall
- When 执行以下操作：the UI consumes that backfill refresh signal
- Then 应得到以下结果：it may request a lightweight display refresh, but must not clear the stable wall snapshot or texture mirrors unless a structural database invalidation or explicit retry path also fires

## Interface Binding

| Interface / Entry | Kind | Source | Expected behavior |
| --- | --- | --- | --- |
| `CoverWallState` | API | Graphify | Tracks cover wall runtime state |
| cover DB/cache APIs | DB | semantic card | Preserve key/source-path distinction |
| `CoverLane` | diagnostics | semantic card | Runtime lane visibility |
| Cover wall click/filter | UI | product semantics | Clicked cover filters the same identity represented by wall/preview |
| texture upload path | UI | architecture rule | GPU upload stays on main thread |
| filtered cover-wall follow-up | UI | source contract | Stable wall remains visible until authoritative facet items are available |
| adaptive-thumbnail cover-wall warmup | UI | source contract | Idle keyword walls with large or dense thumbnail grids get a bounded first-screen fill boost while active/foreground-critical work remains capped |
| result-list cover column during cover-wall priority | UI | source contract | Reuses cached textures only; does not schedule new cover work or render as a blank column |

## Graphify Binding

**Source graph:** `graphify-out/main/interface-call-graph.json` and `graphify-out/main/audit-call-graph.json`
**Impact target(s):** `CoverWallState`, cover DB/cache APIs, `CoverLane`
**Audit classification requirement:** cover source-path and texture upload unknowns must be reviewed before completion.
**Dynamic calls:** review texture/container calls in cover paths.
**Unknown calls:** review cover-related AppState/DB/UI unknowns.

## TDD / QA Proof

| Proof ID | Kind | Artifact / command / event | Pass signal | Status |
| --- | --- | --- | --- | --- |
| TEST-COVER-PATH-NORM | unit | `TestCoverPathNorm` | source path normalization passes | active |
| TEST-COVER-LANE-ORDER | unit | `TestCoverLaneOrder` | lane priority passes | active |
| TEST-COVER-CLICK-PERF | perf | `TestCoverClickPerf` | click path avoids latency regression | active |
| TEST-COVER-BACKFILL-WALL-REFRESH | contract | `TestBackgroundIoPolicy::TestDeferredCoverBackfillRefreshesCoverWallAfterStoppedScanContract` | deferred cover backfill emits and UI consumes cover-wall invalidation | active |
| TEST-COVER-EXPLICIT-DATABASE-REFRESH | contract | `TestBackgroundIoPolicy::TestExplicitRefreshClearsCoverWallFailureCooldownContract`, `test_cover_wall_refresh_state_adapter_boundary.py` | toolbar/menu/F5 invalidate the full-library wall before reloading it and clear retry cooldowns | active |
| TEST-COVER-GLOBAL-HAS-COVER-NO-KEY-FACETS | contract | `TestDatabaseFeatures::testGlobalCoverFacetFallsBackWhenHasCoverRowsLackKeys` | global cover facets keep visible cover count even before key priming catches up | active |
| TEST-COVER-GLOBAL-HAS-COVER-NO-KEY-WALL | contract | `TestDatabaseFeatures::testGlobalCoverWallFallsBackWhenHasCoverRowsLackKeys` | global cover wall keeps visible cover count even before key priming catches up | active |
| TEST-COVER-GLOBAL-WALL-FOLLOWS-FILE-LIST-ORDER | contract | `TestDatabaseFeatures::testGlobalCoverWallPageFollowsCurrentFileListOrder`, `test_cover_wall_page_result_builder_boundary.py` | refreshed full-library cover wall follows first-seen covers in the currently loaded file list, including large libraries | active |
| TEST-COVER-FILTERED-FALLBACK-TEMP-KEY | contract | `TestBackgroundIoPolicy::TestFilteredCoverWallFallbackUsesTemporaryFolderKeysWhenRowsAlreadyHaveCoverContract` | filtered cover wall uses temporary folder identities before key priming catches up | active |
| TEST-COVER-ADAPTIVE-THUMB-WARMUP | contract | `TestBackgroundIoPolicy::TestAdaptiveThumbnailKeywordCoverWallWarmupBudgetContract` | large-thumbnail and dense small-thumbnail keyword walls get bounded idle first-screen fill budget | active |
| TEST-COVER-COLUMN-CACHE-ONLY-PRIORITY | contract | `TestBackgroundIoPolicy::TestCoverColumnUsesCachedTexturesDuringCoverWallPriorityContract` | result-list cover column reuses cache instead of blanking while cover-wall priority defers new cover loads | active |
| DIAG-COVER-LANE | diagnostics | `TestCoverDiagnostics`, `CoverLane`, `Cover.TextureUploaded` | lane/upload events are asserted automatically | active |

## Missing Coverage

- Missing proof: cover wall/filter/current-preview end-to-end sameness test; this is part of the primary user promise, so completion must be `INCOMPLETE_VERIFICATION` when this path is claimed but not tested.
- Suspected stale proof: tests that only verify hash generation without source-path read semantics.
- Human-only verification: artwork visual correctness.
- Missing proof: diagnostics-backed evidence that `ConsumeCoverBackfillUiRefresh()` stays on the lightweight refresh path (`BackfillLightRefreshConsumed`) without repeatedly flipping `texturesPendingClear` or emitting `TextureMirrorClearedAfterReset`.

## Change Gate

- [x] Refresh Graphify main and audit graphs with `python tools/graphify_auto_refresh.py --once`.
- [x] Re-run cover identity/perf tests.
- [x] Review cover-related `UNKNOWN_BLOCKING` entries.
- [x] Update this contract if cover identity or load-path semantics change.

## Decision Log

| Date | Decision | Reason | Decider |
| --- | --- | --- | --- |
| 2026-06-03 | Split partial-scan cover backfill recovery from texture-load failure and marked end-to-end sameness as a primary missing proof. | Review found two recovery behaviors were merged and the highest-risk user promise lacked automatic evidence. | Engineering |


## 原音频写封面确认（第4批）

本批只给两条已有入口加确认：选图导入封面写回原音频、右键「写入封面」。不扩到粘贴、延迟导入、删除封面或标签回写。

### 输入与输出

- 门决策：EvaluateOriginalAudioWriteConfirm / ShouldWriteOriginalAudio / TryInvokeOriginalAudioWrite。取消优于确认与 suppress。
- 导入：loadImageFile 成功后冻结 Paths/Image/Mime，Request 弹窗；只有 writeAlbumArt==true 的路径标 hasCover 并 assign；空成功集不改行。
- 嵌入批：快照 selectedRecords 后 Request，确认才 StartEmbedMetadataBatch。
- 换库/Reset 第一句 ClearPending，迟到 OK 不写。

### 五项所有权

1. Gate：identity=Request 三布尔；无 generation；cancelled 优先；无 IO；栈上纯函数。
2. pending：Kind+Paths/Records/Image/Mime；Esc/Cancel/关闭/SwitchDatabase/Reset 时 Clear；Apply 只读快照；不把 pending 指针传给异步。
3. Import：succeededPaths 是 pendingPaths 的 writeAlbumArt==true 子集；按 filePath 匹配，不按当前选中；UI 帧同步 writer，沿现有 .tmp+atomicSwap。
4. Embed：pendingRecords 到现 runtime；未确认不 Start；保留 ports 值捕获、无 this/blockedNetwork_。
5. popup：三键三语；仅 UI 帧 L()；后台不调 L()。

### 恢复

取消/失败不发布；失败项不标已有封面；原文件保持不变。不使用 coverImportIds 全部标成功。

### 测试组织与证据分类

- Gate Fake：门决策绿。
- 源码接线：边界 Python，不冒充 GUI。
- 真实合成 IO：TestOriginalAudioWriteConfirmIo，不跑 TestTransferEmbed。
- 未证：GUI/E2E/WebView/真 NAS/性能/合法可播 MP3/封面粘贴。
## SiliconFlow 下线（第9批）

本批只把已交付的第三方生图撤出 745 旧布局隔离预览。设置里不再要 SiliconFlow 密钥，封面提示词不再发到外部生图。不是换一家 AI，也不做自有生图。保留 Whisper、ai_tags 和 ai_engine。当前隔离预览不启动，不读取也不删除这台电脑上的真实凭据，不能说本机旧密钥已经删掉。

### 入口

- 设置页去掉密钥输入、对应搜索词和封面工作室。
- 右键封面菜单不再打开 AI 生图。
- CoverGeneration 与 GenerateCoverAsync 不再发送 HTTP。
- 退出时如果设置没有完整保存，只提示「设置未能完整保存；应用仍会退出。」

### 失败与网络库

- 两处网络路径仍先失败，并使用 Write_BlockedNetworkLibrary，排在本地失败之前。
- 本地路径失败关闭，原因键是 AI_CoverPathBusy。
- 不新增线程池或取消令牌。原来的 callback detach 不改。

### 五项所有权

1. identity：第三方生图通道和密钥。产品凭据名仍是 Aris/SiliconFlowApiKey，只删除，不改寻址。测试身份是本测试程序、只读开关和实际 main 源码。
2. generation：将来非隔离产品在 LoadAppSettings 清空内存并调用删除，同时停止生成 HTTP 任务。当前隔离里 Utils 三处 PreviewIsolationActive 短路全部保留。本批测试一开场只读，不生成凭据、DPAPI 或 ini。
3. cancel：CoverGeneration 与 GenerateCoverAsync 失败即关闭。只读测试同步退出。
4. stale-result：旧密钥和旧任务不得再 POST，也不得写封面。网络库继续拒写。旧的凭据 IO 通过不能代替这次安全运行。
5. 生命周期：Utils 的删除函数仍由 LoadAppSettings 调用。工作任务失败关闭且不发网。测试桩只活在这个测试程序里，不改产品布局、迁移、ports，也不把 this 留到下一帧。

### 测试的两层意思

- 源码合同：带 --source-contract-only 时，在任何加密、DPAPI、Cred*、临时目录或 settings.ini 之前，只读取 main.cpp。它能证明界面和保存路径不再写入新密钥，并且源码里仍看得到 ProtectSettingsValue。它不能证明这台电脑里的密钥已经删除。
- 安全 Python：证明域名、密钥框和生图入口已经撤掉，并且只读开关早于第一次凭据 IO。它不调用凭据、网络或产品。
- 默认的旧 IO 源码还留着，本批一次都不跑。输出里不能出现旧的凭据探针句子，也不能把那种输出当成通过。

### 未证

界面、人眼、整条使用路径、听感、内嵌网页、真实网络库、性能、合法可播放媒体和真实凭据删除都还没证。第4批真实文件读写这一轮不重跑，不能当成新证据。
