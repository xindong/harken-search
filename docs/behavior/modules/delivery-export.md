# Delivery / Export Behavior Contract

## Header

**ID:** BDD-DELIVERY-EXPORT
**Module:** Delivery / Export
**Status:** Active
**Priority:** P1
**Owner:** Engineering
**Created:** 2026-06-03
**Last Updated:** 2026-11-04

## Product Behavior

**User promise:** Selected sounds can be exported, processed, or delivered while preserving selected regions, metadata intent, and target profile rules.
**Primary workflow:** User chooses assets or regions, applies processing/export settings, and delivers files to a team/DAW handoff location.
**Non-goals:** This contract does not define installer packaging or product edition signing.

## 中文语义

**用户承诺：** 用户导出的文件只改变用户选择的目标副本，不改源音频；选区、格式、命名、元数据和交付目录都按导出配置执行。
**核心流程：** 用户选择素材或 waveform region，选择 WAV/MP3/FLAC 等目标格式和处理选项，然后批量导出到团队或 DAW handoff 目录。
**非目标：** 这份契约不定义安装包打包、产品版本签名或外部 DAW 导入后的表现。

## BDD Scenarios

### Happy Path

- Given 已满足以下前置条件：a user selects files or waveform regions
- When 执行以下操作：Aris exports or processes them
- Then 应得到以下结果：output files preserve intended region, naming, metadata, and destination behavior
- Given 已满足以下前置条件：a user chooses a target format such as WAV, MP3, or FLAC
- When 执行以下操作：transfer/export runs
- Then 应得到以下结果：the delivered file uses the requested container/extension and does not overwrite or transcode the source asset in place

### Edge Cases

- Given 已满足以下前置条件：a selected region has fade, channel, or processed-file settings
- When 执行以下操作：export runs
- Then 应得到以下结果：those settings are applied only to the selected output and do not mutate the source asset

- Given 已满足以下前置条件：the current file is dragged to a DAW with Magic EQ or other playback processing enabled
- When 执行以下操作：the existing DAW transfer preparation path renders the temporary delivery copy
- Then 应得到以下结果：the frozen DAW transfer snapshot is applied in the existing effect order; Magic EQ bypass leaves its stage untouched; only a successful render is handed off, and the source asset remains unchanged

### Failure / Recovery

- Given 已满足以下前置条件：destination path, permissions, or processing dependency fails
- When 执行以下操作：export is attempted
- Then 应得到以下结果：Aris reports the failure and does not partially overwrite source audio
- Given 已满足以下前置条件：WAV 选区导出目标是当前源音频，或是指向同一文件的硬链接
- When 执行以下操作：用户尝试导出选区
- Then 应得到以下结果：编码器打开目标前拒绝，明确提示目标不能是源文件，源音频字节不变

### Non-Functional

- Given 已满足以下前置条件：batch export
- When 执行以下操作：many assets are processed
- Then 应得到以下结果：progress and cancellation remain observable and output paths remain deterministic.
- Given 已满足以下前置条件：a 1000+ file export batch
- When 执行以下操作：Aris processes the queue
- Then 应得到以下结果：progress, cancellation, failure counts, and completed output paths remain observable without blocking the UI thread on a single long synchronous loop.

## Interface Binding

| Interface / Entry | Kind | Source | Expected behavior |
| --- | --- | --- | --- |
| region export helpers | API | tests | Preserve selected region/fade |
| processed-file export APIs | API | tests | Produce deterministic processed output |
| delivery profile/state | API | product cards | Applies target handoff rules |
| export menu/actions | UI | product semantics | Trigger same export contract |
| export diagnostics | diagnostics | product semantics | Exposes export start/completion/failure lifecycle events |

## Graphify Binding

**Source graph:** `graphify-out/main/interface-call-graph.json` and `graphify-out/main/audit-call-graph.json`
**Impact target(s):** export helpers, processed-file export, delivery profile helpers
**Audit classification requirement:** export path/file I/O unknowns must be reviewed because source audio safety is a red line.
**Dynamic calls:** review export callbacks and file operations.
**Unknown calls:** review export/processed-file entries in `UNKNOWN_CALLS.md`.

## TDD / QA Proof

| Proof ID | Kind | Artifact / command / event | Pass signal | Status |
| --- | --- | --- | --- | --- |
| TEST-EXPORT-REGION | integration | `TestExportRegionPreserve` | region/fade export behavior passes | active |
| TEST-PROCESSED-EXPORT | integration | `TestProcessedFileExport` | processed output behavior passes | active |
| TEST-VOICE-DELIVERY | acceptance | `TestVoiceDeliveryAcceptance` | delivery acceptance behavior passes | active |
| DIAG-EXPORT | diagnostics | `TestProcessedFileExport` + `TestExportRegionPreserve` export lifecycle assertions | start/completion/failure events observed | active |
| TEST-MAC-REGION-EXPORT | real Mac app / isolated diagnostics | tools/macos_region_export_smoke.py | Source-path and hard-link alias rejected; selected WAV frame count and frequency segments verified; source hash unchanged; exit and reopen succeed | active |
| TEST-REGION-PATH-SAFETY | cross-platform unit | TestRegionExportPathSafety | UTF-8 same path and hard-link alias compare equal; distinct and missing paths do not | active |
## Missing Coverage

- Mac 隔离实测通过真实应用的 QA 控制触发同一 AudioEngine 导出路径；未实际点击波形栏“导出”按钮或原生保存对话框，不能据此记界面验收完成。
- Missing proof: format-conversion acceptance for WAV -> MP3/FLAC and 1000+ file batch export is not yet asserted end-to-end.
- Residual gap: permission-specific failure is not yet asserted with a deterministic fixture; Mac isolated test covers a missing destination parent.
- Human-only verification: DAW/team handoff ergonomics.

## Change Gate

- [x] Refresh Graphify main and audit graphs with `python tools/graphify_auto_refresh.py --once`.
- [x] Re-run export/delivery tests.
- [x] Review export `UNKNOWN_BLOCKING` entries.
- [x] Update this contract if export safety, naming, or delivery profile semantics change.

## Decision Log

| Date | Decision | Reason | Decider |
| --- | --- | --- | --- |
| 2026-06-03 | Added target-format conversion and 1000+ batch export as BDD obligations. | Review found Transfer Engine's core user operations were absent from the contract. | Engineering |
