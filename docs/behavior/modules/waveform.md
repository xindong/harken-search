# Waveform Behavior Contract

## Header

**ID:** BDD-WAVEFORM
**Module:** Waveform
**Status:** Active
**Priority:** P0
**Owner:** Engineering
**Created:** 2026-06-03
**Last Updated:** 2026-11-24

## Product Behavior

**User promise:** Visible rows and the currently inspected file become waveform-usable before Aris claims the current result set is ready for immediate work, while the final `import complete` signal waits for the scanned scope's mini-waveform cache debt to clear.
**Primary workflow:** User searches, scrolls, selects, auditions, and inspects waveform shape while background cache work continues without stealing the interaction budget from the current result set, and the final import-complete signal arrives only after scanned-scope mini cache generation finishes.
**Non-goals:** The import-complete signal does not wait for every current-file detail waveform or offscreen presentation texture, but it does wait for full scanned-scope mini-waveform cache generation.

## 中文语义

**用户承诺：** 用户滚到或选中的声音不应该长期空白；当前试听文件的主波形最终必须对应正在查看的同一个文件，旧请求不能覆盖新选择。
**核心流程：** 用户搜索、滚动、选中并试听文件；Aris 先给可见行补 mini waveform，再把当前文件的主波形细节补齐，后台缓存只能在不抢前台的情况下推进。
**非目标：** 后台 backfill 不能为了填满缓存而牺牲当前播放、当前检查文件或一帧 16ms 的 UI 预算。

### Mini Cache Contract (2026-06-26)

- New database selectable cache mode is MiniOnly only; the UI no longer offers MiniAndMain for newly created libraries.
- Legacy `WaveformCachePolicy::Mode::MiniAndMain` remains supported for existing databases and compatibility tests; do not remove the enum or read paths as part of mini-only UI cleanup.
- Mini waveform cache storage is a signed max/min envelope using the existing parallel `float` blob pair: `waveform_data` stores max peaks and `mini_min_data` stores min peaks. It is not the WPK2/int16 packed detail format.
- Opening an adaptive library without the `mini_waveform_format_v2` stamp preserves existing mini waveforms. The automatic upgrade uses the existing count of missing waveform caches to decide whether to rebuild; manual rebuild is unchanged.
- New/adaptive mini density is `ComputeAdaptiveMiniPeakCount(totalFrames)`: 1 peak per 1024 source frames, floor 512, cap 1024.
- Scan precompute, BackfillWorkerPool, and AsyncWorker visible decode paths should all prefer `saveMiniWaveformEnvelopeByPath(...)` when `minPeaks.size() == peaks.size()`, falling back to `saveWaveformByPath(...)` only for legacy max-only data.
- Main waveform detail remains real-time/on-demand or WPK2 detail depending on the existing policy; this mini-cache contract does not solve the independent BWF/FACT detail decode issue.

## BDD Scenarios

### Happy Path

- Given 已满足以下前置条件：visible search results and an actively selected file
- When 执行以下操作：Aris schedules mini and detail waveform work
- Then 应得到以下结果：visible rows start mini coverage within one worker scheduling pass, and current-file detail converges to the selected loaded file

### Edge Cases

- Given 已满足以下前置条件：a long file or cache-missing file becomes visible
- When 执行以下操作：persistent cache is missing or failed
- Then 应得到以下结果：Aris keeps the visible row retryable or promotes it to source decode instead of leaving a permanent blank
- Given 已满足以下前置条件：a BWF file has a FACT sample count that exceeds decodable PCM data
- When 执行以下操作：waveform rendering builds mini or main detail data
- Then 应得到以下结果：the displayed waveform spans the decodable region and does not become empty after the FACT/PCM mismatch point, such as the known about-44s truncation case

### Ready-To-Work Completion Gate

- Given 已满足以下前置条件：Aris is about to say that import or reconnect is complete
- When 执行以下操作：the current result set still contains mini waveform work
- Then 应得到以下结果：Aris may continue full-library waveform backfill in the background, but it must not leave the visible/current-result-set browsing path dependent on synchronous first-scroll decoding or other interaction-blocking catch-up work

### Import Completion Gate

- Given Aris is about to say `本次入库完成`
- When 执行以下操作：the scanned scope still has durable mini-waveform cache rows pending
- Then 应得到以下结果：Aris must stay in post-import optimization until those full-scope cache rows are drained, even if visible-row browsing is already smooth

### Failure / Recovery

- Given 已满足以下前置条件：the user switches files or libraries while waveform work is pending
- When 执行以下操作：stale results return
- Then 应得到以下结果：generation tokens and cancellation prevent stale waveform state from overwriting current state
- Given 已满足以下前置条件：a million-file library and the user starts a full mini waveform rebuild from the UI
- When 执行以下操作：the rebuild begins
- Then 应得到以下结果：the click path must not synchronously delete or compact the whole waveform sidecar; the rebuild must stream bounded batches through background workers and overwrite cache rows progressively

### Non-Functional

- Given 已满足以下前置条件：a large library and active browsing
- When 执行以下操作：waveform backfill is running
- Then 应得到以下结果：waveform backfill tasks yield to playback load and current-file inspection within one scheduler cycle, and main-thread presentation work stays within the 16ms frame budget
- Given 已满足以下前置条件：the user starts scrolling immediately after import-complete is shown
- When 执行以下操作：visible rows need mini waveform presentation
- Then 应得到以下结果：first-use mini waveform work for the current result set remains smooth enough that browsing does not show an obvious decode hitch, and any remaining misses are handled as bounded progressive fill rather than a blocking interaction stall
- Given 已满足以下前置条件：a keyword search result page has just landed
- When 执行以下操作：visible mini-wave cache misses are discovered for the new rows
- Then 应得到以下结果：text/list results get a short paint-first window and mini-wave decode promotion is bounded per frame instead of batch-promoting the whole visible set at once

## Interface Binding

| Interface / Entry | Kind | Source | Expected behavior |
| --- | --- | --- | --- |
| `WaveformWorker` | worker | Graphify | Owns mini/detail async queue and UI-generation cancellation |
| `waveformWorkerLoop()` | worker | Graphify | Handles AudioEngine main detail work |
| `WaveformExtractor` | API | semantic card | Decodes mini/detail data |
| `PumpScanWaveformBackfillQueue()` | worker | semantic card | Bridges durable backfill rows to workers |
| `WaveformLane` / `MiniWave` | diagnostics | semantic card | Runtime lane evidence |
| `postSearchMiniWaveThrottleFrames` | worker | source + diagnostics | Search rows paint before mini-wave decode catch-up fills the new viewport |

## Graphify Binding

**Source graph:** `graphify-out/main/interface-call-graph.json` and `graphify-out/main/audit-call-graph.json`
**Impact target(s):** `WaveformWorker`, `WaveformRequest`, `WaveformResult`, `waveformWorkerLoop`
**Audit classification requirement:** waveform worker and DB cache unknowns must be reviewed because stale async writes are P0 risk.
**Dynamic calls:** review container and callback calls in waveform worker paths.
**Unknown calls:** review waveform-related `UNKNOWN_CALLS.md` entries.

## TDD / QA Proof

| Proof ID | Kind | Artifact / command / event | Pass signal | Status |
| --- | --- | --- | --- | --- |
| TEST-WAVEFORM-CACHE | integration | `TestScanWaveformCacheContract` | persistent/detail/mini/backfill behavior passes | active |
| TEST-WAVEFORM-PRESENTATION | unit | `TestWaveformPresentationPolicy` | progressive/final display policy passes | active |
| TEST-IMPORT-PRECOMPUTE | integration | `TestImportPrecomputeWaveform` | import/backfill waveform assertions pass | active |
| DIAG-WAVEFORM-LANE | diagnostics | `TestScanWaveformCacheContract`, `TestImportPrecomputeWaveform`, `MiniWave`, `ScanWaveformCacheReport` | runtime mini/cache events are asserted automatically | active |
| TEST-MINI-CACHE-UNIFIED | integration | `TestScanWaveformCacheContract`, `TestWaveformBucketMath`, `TestMiniWaveformCap` | scan precompute writes decoded max/min mini envelope and long files reach raised adaptive cap | active |
| TEST-FULL-REBUILD-NONBLOCKING | source contract | `TestImportPrecomputeWaveform` | full waveform rebuild click path does not call `clearWaveformCache()` and retries previous failed rows | active |

## Missing Coverage

- Missing proof: dedicated runtime proof for every `WaveformLane` cache-hit/progressive lane under an attached library `AppState`.
- Suspected stale proof: semantic flag tests that refer to removed waveform flags.
- Human-only verification: waveform readability.

## Change Gate

- [x] Refresh Graphify main and audit graphs with `python tools/graphify_auto_refresh.py --once`.
- [x] Re-run waveform policy/cache tests.
- [x] Review waveform `UNKNOWN_BLOCKING` and dynamic entries.
- [x] Update this contract if scheduling, cache state, or display semantics change.

## Decision Log

| Date | Decision | Reason | Decider |
| --- | --- | --- | --- |
| 2026-06-04 | Split waveform semantics into `ready to work` and `import complete`. | Product clarified that visible-row usability converges earlier, but `import complete` still waits for full scanned-scope mini-waveform cache completion. | Engineering |
| 2026-06-03 | Added BWF FACT/PCM mismatch as a named edge case and quantified foreground scheduling. | Review found a known P0 waveform truncation fix was absent from BDD and "quick" was not measurable. | Engineering |
## QA-only 长波形/电平（第10批）

本批把冻结 QA 线已经交付的长波形/电平算法带回 745 旧布局。音频报不出长度时，从已打开的 decoder 按块计算，不把整份 PCM 塞进内存。电平任务不保留没人用的 peaks/rms 数组，仍保留真 RMS、响度和 true peak。取消返回失败并清空结果。Decode 超过显式 maxFrames 时失败、关闭 decoder、不留采样缓冲。这不是新波形界面或新播放器。

未知长度收集显示峰值时仍是 O(peaks)。不能宣称任意时长都固定内存。测试里 peaks<=8 只针对合成的 1 秒文件。电平 collectPeaks=false 才承诺不堆显示数组。

### 五项所有权

1. identity：请求用文件路径和 generation。ForFile 拥有 decoder，测试钩只改长度报告。
2. generation：沿用已有 generation，过期结果不能发布。
3. cancel：沿用已有取消标志，helper 在块之间检查。取消清空结果并失败。
4. stale-result：过期 generation 丢弃。电平只回写 RMS/LUFS/true peak。空 peaks/rms 不能覆盖 mini 已完成的显示数组。
5. 生命周期：helper 只借用 decoder，不关闭。ForFile 在 helper 返回后关闭。Decode 超预算先关 decoder 再清空缓冲。无新线程池、future 或 schema。

### 回退

撤掉 ForFile 委派、helper、maxFrames 和 collectPeaks 参数即回到本批基线。

### 证据边界

已证是合成 1 秒 WAV 和源码合同。未证真实长音频、听感、性能、GUI、E2E。
