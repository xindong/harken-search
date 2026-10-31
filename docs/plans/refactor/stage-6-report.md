# Current platform-boundary record

Updated 2026-10-29 after the public repository migration. This is a technical
inventory, not a workflow approval or a claim that an old release was retested.

The current tree has `637` exact audited entries: 13 native entry includes,
`94` `BOOTSTRAP-FILESYSTEM`, `480` `BOOTSTRAP-RUNTIME`, 6 BOOTSTRAP-EXIT,
and 44 BOOTSTRAP-PROGRESS. The migration ledger is empty. The new progress
adapter review is in docs/architecture/bootstrap-progress-native-audit.md.

The performance manifest retains historical measured baseline values. Validating
that manifest verifies its structure and thresholds, not new performance samples.
