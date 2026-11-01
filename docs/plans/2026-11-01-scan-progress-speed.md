# Scan progress accuracy, then measured import performance

## Scope and order

First correct the progress/throughput presentation. Then measure the existing
import path and retain only a narrow, repeatably beneficial optimization. Do not
reduce metadata, cover or required mini-waveform work to make completion appear
faster. Keep pause/keep, cancel/rollback, result windows and source audio intact.
No visual redesign: reuse the approved compact scan dialog and existing tokens.

## Modules

1. **Progress projection** (`ScanDialogPresentation.h`, native callers): streaming
   discovery counts are not a fixed total. While enumeration is unfinished, show
   processed count with an indeterminate bar, not a denominator or percentage.
   Cleanup and fixed waveform stages retain real current/total progress.
2. **Rate sampling** (`ScanDialogPresentation.h`): use a bounded recent time
   window of completed-count samples. Short callback gaps must not repeatedly
   report zero. Genuine sustained inactivity must expire the old rate and show
   a concise no-new-progress state. Reset at task/stage/counter/clock boundaries.
   Elapsed time keeps its current completion latch and includes all foreground
   work, not time spent reading the result.
3. **Native rendering/localization**: keep speed and total elapsed visible; show
   only the relevant count/status. Background indicator uses the same semantics.
   Add a read-only isolated QA projection for assertions, not fake progress.
   The scan callback must also stop counting post-processing steps as audio
   files. A synchronous optional finalizing notification separates that stage;
   native callers retain their generation guard and cancel/rollback authority.
   It is not retained or invoked on a different thread. Existing non-UI callers
   keep the default notification and unchanged completion behavior.
4. **Measurement**: compare fresh isolated databases on identical fixture sets,
   settings, worker counts and completion criteria. Capture elapsed stages,
   errors, durable rows and required mini caches. No shared desktop input or
   edits to real libraries. Separate first import from unchanged rescan results.
5. **Performance change**: after the presentation passes, use measurements to
   choose one bounded bottleneck. Write its failing behavioral regression first,
   then compare repeated baseline/candidate runs. No gain means no retained
   speculative change; report limitations honestly.

## Verification / delivery

- RED before GREEN for dynamic totals, batched rate callbacks, genuine stalls,
  reset/phase transitions, fixed cleanup/waveform counts and terminal duration.
- Native isolated package: large scan, multi-folder path, waveform completion,
  pause/keep, rollback, failures, five themes and a narrow viewport.
- Report fixture identity, cold/warm limitations, repeats and untested cases.
- Deliver the exact QA package, real screenshots and measurements through one
  self-contained acceptance page. Technical checks do not substitute for user
  confirmation of the visible change; do not publish or merge before acceptance.

## Current status

- Verified: dynamic-total and callback-gap tests failed against the baseline;
  the minimal presentation fix passes these and the existing presentation tests.
- Verified: six related native test executables and 85 focused Python checks.
- Verified: real isolated five-theme smoke and outcomes covering pause/keep,
  rollback, failure/empty results, cleanup, similarity and narrow metrics.
  This is technical verification, not user visual acceptance.
- Verified: post-processing no longer adds fake files to progress totals. A
  three-file database regression failed before the fix and now passes. Long
  observation gaps reset only recent speed, not total elapsed time. Independent
  review found no remaining blocker in these changes.
- Measured: 2,000 identical synthetic 1-second 16-kHz mono PCM WAV files, fresh
  database each run, full completion with 2,000 persisted mini waveforms and zero
  scan errors. Baseline: 4.672 / 4.750 / 4.640 seconds (median 4.672).
- Rejected experiment: start the native full-scan backfill pool before ingestion,
  with an idempotent start and unchanged completion/cancel gates. Measured
  4.687 / 4.891 / 4.844 seconds (median 4.844): no repeatable improvement.
  The experimental product change and its source-order test were removed.
  Keep the repeatable benchmark tool; do not claim actual throughput improved.
  An A/B/A control using the original baseline binary returned 4.656 / 4.719 /
  4.563 seconds (median 4.656), with all 2,000 caches persisted each time.
- Measurement limits: synthetic local WAV first imports, warm OS caches and
  100-ms polling. These results do not establish performance on the user's
  MP3 library, network storage, long recordings or unchanged rescans.
- Test infrastructure incident: the first benchmark finished its full scan and
  durable readback but could not find a closed result dialog. The tool now opens
  the real dialog before clicking Done; 90 tool tests pass and repeated native
  runs pass. The original failed report is retained, not relabelled as passed.
- Known baseline failure: `test_network_path_contract.py::NetworkPathContract::
  test_scan_dialog_marks_readonly_network_library` still expects the older
  `Scan_MappedDriveHiddenHint` copy. Reproduced unchanged at 80ee617; not weakened
  or fixed as part of this task (126 other expanded checks passed).
- Fresh final source check: six focused native executables passed; 216 Python
  checks passed with only the independently reproduced baseline failure excluded.
  The full 518-target CTest suite was not built/run for this reporting-only
  callback/UI slice. No database schema, SQL, decoder or writer policy changed.
- In progress: final native regression, exact package identity and acceptance
  delivery. No user visual acceptance, merge or public release has occurred.

## 2026-11-01 QA follow-up: stale completion text during waveforms

- User screenshot from the delivered 518db3d package shows 1,034/7,093
  waveforms (14%) with an extra `ScanDone` line. That package is not accepted.
- Cause: the shared ingest text becomes `ScanDone` after the bounded backfill
  wait, although required waveform work can remain pending. The progress view
  incorrectly displays this ingest field during both ingest and waveform phases.
- Minimal correction: display the current ingest file only while ingesting.
  Keep waveform progress, speed, elapsed time, and all actual completion/stop
  behavior unchanged. Do not replace the line with another explanation.
- Regression: fail the source-wiring check before changing the guard; then test
  the real native boundary with 7,093 synthetic WAVs. Verify the file row exists
  during ingestion, disappears during pending waveforms, and completion still
  waits for every required waveform. Capture the actual pink-theme dialog.
- Isolation: retain the user-running original package and its profile. Build
  this revision for `D:/Harken-QA/OPT-20261101-SCAN-PROGRESS-SPEED/`
  `waveform-label-r2/data` and deliver a new package beside it. No automated
  commands, replacement or process control may target the old package/profile.
- Verified: regression reproduced RED; the minimal guard passes, with 61
  related Python checks and the native presentation tests. Package source is
  `2c308cf13aee2c559924f71551d3d3c918464029`.
- Verified: the actual package completed all 11 outcomes checks, including the
  pending-waveform boundary in pink, visible speed/elapsed, exact 7,093 audio
  rows and completed required waveforms, pause/keep, rollback and narrow UI.
  The screenshot shows no stale detail row even though read-only scan status
  still contains the previous ingest-stage `ScanDone` text.
- QA-script incident retained: the first check incorrectly required
  `is_scanning=false` while full-scan completion was pending. Full scanning
  intentionally stays active until required waveforms finish. The scan itself
  completed; the check missed its screenshot window and later timed out reading
  QA state. Only the script predicate was corrected; the identical product EXE
  passed the rerun. Reports: `waveform-label-r2/outcomes/report.json` (failed)
  and `waveform-label-r2/outcomes-rerun/report.json` (passed).
- This follow-up QA-script/document commit changes no product source after
  the tested package build. User visual acceptance and QA integration remain
  pending; no public release or user-data modification occurred.
