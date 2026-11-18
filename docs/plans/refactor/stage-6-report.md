# Current platform-boundary record

Updated 2026-11-18 after the bootstrap launcher audit. This is a technical
inventory, not a workflow approval or a claim that an old release was retested.

The current tree has `645` exact audited entries: 13 native entry includes,
`94` `BOOTSTRAP-FILESYSTEM`, `480` `BOOTSTRAP-RUNTIME`, 6 BOOTSTRAP-EXIT,
8 BOOTSTRAP-MAIN and 44 BOOTSTRAP-PROGRESS. The migration ledger is empty. The new progress
adapter review is in docs/architecture/bootstrap-progress-native-audit.md,
and the launcher review is in docs/architecture/bootstrap-main-native-audit.md.

The performance manifest retains historical measured baseline values. Validating
that manifest verifies its structure and thresholds, not new performance samples.
