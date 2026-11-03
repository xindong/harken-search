# Scan / Indexing Behavior Contract

## Header

**ID:** BDD-SCAN-INDEXING
**Module:** Scan / Indexing
**Status:** Active
**Priority:** P0
**Owner:** Engineering
**Created:** 2026-06-03
**Last Updated:** 2026-07-16

## Product Behavior

**User promise:** Aris distinguishes `ready to work` from `import complete`, and the scan dialog's foreground import-completion gate follows the selected scan strategy. A user may pause and keep committed batches for a later idempotent rescan, while explicit cancel still rolls the current scan session back.
**Primary workflow:** User adds or reconnects a library, can see indexing progress, and stays in the scan dialog's foreground finishing state until the selected strategy's current-scan debt is clear; `极速模式` is allowed to release `Done` before full-library mini waveform cache exists, but it must keep visible-mini and current-file main-wave lanes progressive and non-blocking.
**Non-goals:** Import completion does not wait for unrelated future watcher/reconcile work, metadata-only refreshes outside the current scanned scope, source files outside the current scanned scope, or fast-mode mini waveform debt that was intentionally excluded from the foreground completion gate.

## 中文语义

**用户承诺：** Aris 会区分“可开始工作”和“本次入库完成”。用户可选择“暂停并保留”，下次重扫同一文件夹时跳过已入库文件并继续剩余工作；“取消并回滚”仍恢复到本次扫描前状态。
**核心流程：** 用户添加或重连音效库后，扫描会先把基础记录分批提交并尽快变成可搜索；随后按当前策略决定前台完成门的范围。`极速模式` 允许 mini 波形改走按需或后台渐进补齐，但不能把首轮滚动或当前文件主波形重新推回同步阻塞路径。
**非目标：** 完成门不等待本次扫描范围之外的 watcher/reconcile、metadata-only refresh、无关 cache prewarm，也不要求 `极速模式` 在 `Done` 前补齐全库 mini 波形。

## BDD Scenarios

### Happy Path

- Given 已满足以下前置条件：a user scans a large sound library
- When 执行以下操作：Aris indexes files
- Then 应得到以下结果：records committed in the current scan batch flush cycle become searchable while durable waveform/metadata backfill can continue after the initial scan stage

### Ready-To-Work Gate

- Given 已满足以下前置条件：a scan or reconnect is about to present an "import complete" state
- When 执行以下操作：Aris decides whether the current library is ready
- Then 应得到以下结果：the current result set is searchable and trustworthy, imported-batch cover dedup is finished, first-screen covers are already usable, the currently inspected file can produce its main waveform, and the visible/current-result-set mini waveform path is warm enough that immediate scrolling does not depend on synchronous first-use decoding on the interaction path

### Import Completion Gate

- Given Aris is about to say `本次入库完成`
- When the current scan strategy is `极速模式` and any file in the scanned scope still owes durable metadata or cover-aggregation terminal state
- Then 应得到以下结果：Aris must remain in the foreground scan-finishing surface even if the current result set is already usable
- When the current scan strategy is `完整模式` and any file in the scanned scope still owes durable metadata, mini-waveform cache, or cover-aggregation terminal state
- Then 应得到以下结果：Aris must remain in the foreground scan-finishing surface even if the current result set is already usable, and only render the final completion panel after every scanned file is terminal for the selected strategy's hidden gates
- When 执行以下操作：Aris reports the scan completion duration
- Then 应得到以下结果：the duration must measure monotonic wall-clock time from the scan task start to the moment the foreground completion gate opens, not database ingest time alone

### Foreground Scan Dialog Gate

- Given 已满足以下前置条件：the scan thread has finished discovering/importing files but the scanned scope still owes work that is inside the selected scan strategy's foreground completion gate
- When 执行以下操作：the scan dialog renders its final controls
- Then 应得到以下结果：the result/completion panel must not render yet, the `Done` action must not be available as a completion exit, and the dialog must present a foreground finishing state instead of letting current-scan metadata/mini/cover work continue invisibly
- Given the current strategy is `极速模式`
- When 执行以下操作：the scan dialog opens its completion gate
- Then 应得到以下结果：mini waveform debt may continue outside the foreground completion gate only if visible-mini and current-file waveform paths remain progressive and non-blocking
- And the completion surface should explicitly disclose that mini waveform fill may continue on-demand or in the background, instead of implying every mini waveform is already ready
- Given 已满足以下前置条件：the `Done` action is enabled or visible as an actionable completion control
- When 执行以下操作：the user clicks it
- Then 应得到以下结果：the selected strategy's full-scope completion debt must already be clear for the current scan

### Edge Cases

- Given 已满足以下前置条件：startup or watcher reconcile detects changed files
- When 执行以下操作：reconcile runs in lightweight mode
- Then 应得到以下结果：it updates file records without forcing a complete expensive import path
- Given 已满足以下前置条件：a scan encounters damaged, unreadable, or unsupported audio files among otherwise valid files
- When 执行以下操作：indexing reaches those files
- Then 应得到以下结果：Aris skips and records the bad-file failure while continuing the scan for the remaining library, instead of aborting the whole import

### Failure / Recovery

- Given 已满足以下前置条件：a scan is cancelled, stopped, or the DB closes
- When 执行以下操作：in-flight scan or watcher work observes cancellation
- Then 应得到以下结果：it stops without dereferencing stale DB handles or corrupting scan state
- Given 已满足以下前置条件：a user-cancellable library scan has already batch-committed some rows
- When 执行以下操作：the user presses cancel before the overall scan finishes
- Then 应得到以下结果：Aris rolls back this scan session's added/updated/removed rows plus scan-created waveform/backfill debt, and the library returns to its pre-scan state instead of keeping a partial import

### Recovery After Partial Scan

- Given 已满足以下前置条件：a stopped scan committed file rows before deferred cover jobs finished
- When 执行以下操作：cover backfill later resolves artwork through its worker DB connection
- Then 应得到以下结果：Aris treats that as a database-content change for the cover wall even though scan completion did not synchronously run every derived artifact
- Given 用户在至少一个批次提交后选择“暂停并保留”
- When Aris 重启后再次扫描同一文件夹
- Then 已提交前缀仍保留在数据库中，既有记录被幂等跳过，只插入剩余或新增文件，且暂停路径不执行缺失文件清理
- Given 用户改为选择“取消并回滚”
- When 扫描 worker 观察到显式回滚意图
- Then 原有回滚日志恢复本次扫描前的记录与 durable queue 状态

### Non-Functional

- Given 已满足以下前置条件：a 3M-file scale library
- When 执行以下操作：scan batches commit
- Then 应得到以下结果：batch work avoids per-row prepare churn and emits diagnostics for scan progress/failure
- Given 已满足以下前置条件：Aris has already told the user that import is complete
- When 执行以下操作：the user immediately scrolls the current result set or opens the first screen of the cover wall
- Then interaction-critical metadata, mini waveform, and cover presentation stay off the blocking path; in `极速模式`, any remaining mini waveform work must behave as bounded progressive fill rather than hidden completion debt

## Interface Binding

| Interface / Entry | Kind | Source | Expected behavior |
| --- | --- | --- | --- |
| `scanDirectory()` / `scanDirectoryEx()` | DB | rg fallback + semantic card | Scan records with explicit complete/import/reconcile semantics |
| `ScanStopDisposition` | API | `src/ScanInterruption.h` | Distinguishes keep-committed pause from rollback cancel before the cancel token is published |
| `FileSystemWatcher` | worker | Graphify | Coalesces file system events and emits need-rescan |
| `watcherRescanWorkerThread()` | worker | rg/source scan | Runs reconcile scans with cancellation guard |
| `precomputeWorkerThread()` | worker | rg/source scan | Hands durable derived work off after scan |
| `Scan.*` | diagnostics | QA protocol | Runtime scan integrity evidence |
| `PumpAppScanRefresh()` | host lifecycle | `src/AppRuntime.cpp` | Windows and macOS consume committed batches and final refresh through the same service; retain the owned scan thread until it can be joined |

## Graphify Binding

**Source graph:** `graphify-out/main/interface-call-graph.json` and `graphify-out/main/audit-call-graph.json`
**Impact target(s):** `scanDirectory`, `scanDirectoryEx` fallback via nearest functions, `FileSystemWatcher`
**Audit classification requirement:** `scanDirectoryEx` exact-node misses must be recorded as Graphify unavailable for exact symbol with source-scan fallback.
**Dynamic calls:** review watcher/background calls in `DYNAMIC_CALLS.md`.
**Unknown calls:** review `Database.cpp` scan-region and `FileSystemWatcher.*` unknowns.

## TDD / QA Proof

| Proof ID | Kind | Artifact / command / event | Pass signal | Status |
| --- | --- | --- | --- | --- |
| TEST-SCAN-RENAME-QA | integration | `TestScanRenameQa` | scan/rename semantics pass | active |
| TEST-BACKFILL-RECOVERY | integration | `TestBackfillRecoveryWorker` | durable backfill recovery passes | active |
| TEST-SCAN-WAVEFORM-CACHE | contract | `TestScanWaveformCacheContract` | scan-to-waveform cache contract passes | active |
| TEST-SCAN-PROGRESS-CLI | contract | `py -3 scripts\qa_scan_progress_contract.py --stop-auro` | visible total is file count, progress/speed sample hidden terminal throughput, metadata/mini/cover gate is locked | active |
| TEST-SCAN-READY-GATE-FAST | gui probe | `py -3 scripts\qa_probe_scan_ready_gate.py --scan-strategy fast --scan-root .openclaw\reports\scan_ready_probe_fixture_large` | launches in the harness multi-instance functional lane, reports `launch_mode=harness_multi_instance_functional` plus the lane env/throttle expectation, reports `scan_strategy=fast`, keeps mini waveform debt out of the foreground completion gate, observes `ScanBackgroundBackfillDeferred -> ScanBackgroundBackfillDeferredSatisfied -> ScanBackgroundBackfillResume threads<=2`, and also proves the same transition through `scan_background_backfill_phase / deferred_wait_ms / resume_threads` state snapshots while keeping browse-pressure progression for visible mini/current main-wave lanes | active |
| TEST-SCAN-CANCEL-ROLLBACK | contract | `TestScanWaveformCacheContract::testCancelledScanRollsBackCommittedRowsAndQueueDebt` | cancelled scan restores pre-scan DB/backfill state | active |
| TEST-SCAN-PAUSE-RESUME | integration | `TestScanPauseResume` | pause keeps a committed prefix, reopen/rescan inserts only remaining files, rollback cancel leaves zero rows | active |
| TEST-SCAN-PAUSE-UI-CONTRACT | source contract | `TestScanPauseResumeContract` | three user entry classes share one policy; stop choice and paused summary stay distinct | active |
| TEST-MULTI-FOLDER-SCAN-PROGRESS | contract | `TestScanWaveformCacheContract::testMultiFolderScanProgressAggregatesLocalCallbacks` | multi-folder drag scan progress remains aggregate and does not reset to 0 on the next folder | active |
| TEST-COVER-BACKFILL-WALL-REFRESH | contract | `TestBackgroundIoPolicy::TestDeferredCoverBackfillRefreshesCoverWallAfterStoppedScanContract` | stopped-scan deferred cover backfill refreshes cover wall data cache | active |
| DIAG-SCAN | diagnostics | `TestImportPrecomputeWaveform`, `Scan.*` | summary/cache report present and no rollback/flush failure | active |
| MAC-LIBRARY-SMOKE | real isolated app | `tools/macos_library_smoke.py --app <absolute-binary> --root <compiled-isolation-root> --out <new-proof-directory>` | create/open, valid and damaged audio, repeat scan, combined filters, settings/library restart, cancel and exit after committed batches, normal exits and framebuffer captures | automated; user acceptance separate |

The Mac smoke uses the existing UI create/scan intents and shared database-switch
workflow. Generated WAV files, a cover and one damaged file remain inside the
compiled preview root. It reads a separate read-only SQLite connection to prove
a committed prefix exists before cancellation or shutdown, then verifies rollback
and recovery. This test does not certify playback, waveform rendering, the native
file picker's interaction, large real libraries or user acceptance.

The shared scanner keeps a terminal failure record for damaged audio. Such a row
has no decoded audio metadata and is excluded from the successful count; its
failure reason remains available. Reopening restores the library and persistent
settings; transient result filters reset, as on Windows.

## Missing Coverage

- Missing proof: exact Graphify AST node for `scanDirectoryEx`.
- Suspected stale proof: old design docs that imply scan must synchronously complete every derived artifact.
- Human-only verification: real-library long-running fast-scan UX and perceived first-scroll smoothness; final signoff must use `docs/behavior/manual-acceptance-checklist.md` under `scan-indexing` and `waveform`.

## Change Gate

- [x] Refresh Graphify main and audit graphs with `python tools/graphify_auto_refresh.py --once`.
- [x] Re-run scan/backfill contract tests.
- [x] Review scan-region `UNKNOWN_BLOCKING` entries.
- [x] Update this contract if scan/import/backfill stage semantics change.

## Decision Log

| Date | Decision | Reason | Decider |
| --- | --- | --- | --- |
| 2026-06-05 | Added scan `极速模式` that excludes mini waveform cache from the foreground completion gate. | Product clarified that large-library import must prioritize metadata and cover completion, while mini waveforms may continue through progressive/on-demand paths as long as scroll and main-wave behavior do not regress. | Engineering |
| 2026-07-16 | Split scan interruption into `PauseKeepCommitted` and `CancelRollback`; resume re-enumerates and skips existing rows instead of persisting a filesystem cursor. | The sound-designer workflow needs safe cross-restart continuation without weakening the existing destructive-cancel rollback promise. | Engineering |
| 2026-06-04 | Count scan progress as file-count total with terminal-throughput movement. | Product clarified that the total should be `files * 1`, but progress must not appear frozen while hidden metadata, mini waveform, and cover aggregation terminal work is draining. | Engineering |
| 2026-06-04 | Make the scan dialog a foreground completion gate. | Product clarified that hidden post-completion mini/cover work is not allowed: if `Done` can end the dialog, the current scan's mini-waveform cache and cover aggregation must already be finished. | Engineering |
| 2026-06-04 | Split `ready to work` from `import complete`. | Product clarified that `本次入库完成` must wait for full scanned-scope mini-waveform cache and cover aggregation, while usability may converge earlier. | Engineering |
| 2026-06-04 | User-cancelled library scans now roll back the whole scan session instead of leaving partial imports visible. | Product clarified that cancel means the scan failed and must not keep newly imported or re-marked scan state. | Engineering |
| 2026-06-05 | User-facing scan duration is measured to the foreground completion gate. | Product clarified that previous ingest-only timing was not the accurate start-to-completion duration. | Engineering |
| 2026-06-03 | Added damaged-file continuation and separated partial-scan backfill recovery as its own scenario. | Review identified the common large-library damaged-file case and mixed cancellation/backfill recovery semantics. | Engineering |
