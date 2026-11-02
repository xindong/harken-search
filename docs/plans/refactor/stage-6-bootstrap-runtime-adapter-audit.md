# Aris Bootstrap Windows runtime adapter boundary audit

**Updated:** 2026-07-29
**Owner slice:** `BOOTSTRAP-RUNTIME`
**Reviewed path:** `src/bootstrap/WindowsBootstrapRuntimeWin32.cpp`
**Disposition:** `platform-adapter`

## Ownership decision

The reviewed file is the native Windows port for Bootstrap process and channel
operations. It owns exact old-process identity observation, bounded wait and
termination of an already authenticated process, suspended child creation and
identity capture, inherited-handle allowlisting, health-pipe transport, and the
Windows entrypoint/root-binding helpers used by `ArisBootstrap.exe`. The pure
Bootstrap plan, invocation, transaction, and reducer contracts remain in the
portable Bootstrap files and depend on typed ports; application UI, update
runtime, database, audio, media, and release packaging do not depend on this
adapter.

Keeping these calls in one reviewed port preserves the process-identity and
handle-lifetime invariants. Moving individual calls into forwarding wrappers or
other files would not remove the native dependency and would make the
authenticated-channel boundary harder to audit.

## Audited native surface

The Stage 6 scanner reports exactly `351` findings in this reviewed path:

- `8` exact `native-include` findings (the existing Windows headers plus
  `knownfolders.h`, `softpub.h`, and `wintrust.h`);
- `343` exact `native-symbol` findings covering identity capture, process
  opening/wait/termination, suspended launch, inherited-handle selection,
  launch-image handle pinning and child rebind verification, health transport,
  known-folder root derivation, UTF conversion, and native error handling.

Every finding is recorded with its exact fingerprint, path, line, rule, token,
occurrence, owner, disposition, and this evidence path in
`stage-6-platform-audited-exceptions.json`. The validator independently freezes
the approved fingerprint set. A changed path, rule, token, normalized source
context, occurrence, owner, disposition, or evidence reference remains
unresolved. The policy has no directory glob, path prefix, token regular
expression, scanner suppression, or general native-symbol exemption.

The existing `11` `platform-entry` records and the original `64`
`BOOTSTRAP-FILESYSTEM` fingerprints remain separate exact policies; that
filesystem profile now has `102` findings after the scanner exposed `38`
additional calls in the same reviewed file. Neither category can be used to relabel
the other adapter or a future Bootstrap native file.

## Authority and rollback boundary

The adapter receives only typed, already validated process identities,
authenticated channel handles, and updater-owned root bindings from Bootstrap
core. It never trusts a PID, path text, command-line capability, or checksum by
itself. Process operations re-open and re-check the expected image, volume,
file identity, creation time, and same-user binding before a wait, termination,
or health acknowledgement. Child handles and inherited health handles are
explicitly owned and closed by the adapter.

The adapter reports observations and typed failures; the portable transaction
reducer decides commit, rollback, or quarantine. A timeout, identity mismatch,
launch/health failure, or ambiguous channel is therefore fail-closed and cannot
silently become a successful replacement. Restart recovery may use only the
durably bound journal and signed plan authority; this audit does not grant
authority to a caller-supplied staging or transaction root.

This exception does not claim complete automatic-update delivery. Upper-layer
invocation authentication, release-key provisioning, crash-cut coverage, and
the remaining filesystem privilege/TOCTOU limits are governed by the Bootstrap
design and its separate evidence.

## Reproducible gate

The regression contract was first run against the exact baseline validator at
`7167e3416cfb125e7eff3712bca2d12e6fe9e74c`. The runtime acceptance test failed,
and the owner/evidence substitution subtests failed to distinguish
`BOOTSTRAP-RUNTIME` from `BOOTSTRAP-FILESYSTEM`: three failures total. Running
the same three tests against the reviewed validator passes all three tests,
including path, category, rule, token, occurrence, fingerprint, owner, and
evidence substitutions.

The later child-rebind hardening also produced a deliberate snapshot RED: the
live repository regression expected raw=`203` but observed raw=`209`; the stale
register/allowlist classified only `99` findings and left `110` unresolved.
After regenerating the exact runtime metadata and reviewing the six new
fingerprints, all four focused runtime tests pass against the current source.

The terminal empty-job rebind repair then added four more exact scanner
occurrences without changing any existing fingerprint: two `CloseHandle`
occurrences, one `QueryInformationJobObject`, and one `GetLastError`. The live
repository contract recorded the required RED (`476` expected versus `480`
observed; only `235` entries classified while runtime line metadata was stale).
Regenerating only the exact `BOOTSTRAP-RUNTIME` metadata and adding those four
fingerprints restored the three-way equality at `351`; the original `11`
`platform-entry` and `102` filesystem-adapter policies were not changed.

```powershell
python -B tools/platform_boundary_check.py --root . `
  --check-ledger docs/plans/refactor/stage-6-platform-migration-ledger.json `
  --check-exceptions docs/plans/refactor/stage-6-platform-audited-exceptions.json `
  --require-empty
```

The runtime extension contributes `351` audited findings. Together with the
unchanged `11` platform-entry and the `102` filesystem-adapter records, the
register contains exactly `464` audited findings. The live runtime findings,
register fingerprints, and validator allowlist are three equal `351`-member
sets; the equivalent filesystem sets are three equal `102`-member sets and
retain the original `64`-member subset. Any source
drift intentionally makes the gate fail until this exact evidence and register
are reviewed again.

At the source-freeze snapshot, the repository command reports raw=`480`,
audited=`464`, unresolved=`16`, and ledger=`0`. The unresolved set contains
only findings from `src/update/UpdateCheckHttpsTransport.cpp`; none is silently
covered by this runtime exception. Consequently the repository-wide
`--require-empty` command exits nonzero at this snapshot. That residual is
recorded rather than authorized by this adapter review.

## Stage 6-B entrypoint safety evidence

The Windows entrypoint admits an updater handoff only when the inherited
standard-input handle is non-null, is not `INVALID_HANDLE_VALUE`, and
`GetFileType(handoff) == FILE_TYPE_PIPE`. A regular file, console handle, null
handle, or invalid handle therefore cannot be treated as an authenticated
handoff. This is the exact semantics recorded by fingerprint
`src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|bc7ccdeef83b4b32|1`.

The reinstall prompt is guarded by both `mode == BootstrapNativeMode::Normal`
and `result != 0`, and is additionally gated by `launchedFromHandoff`. Recovery
mode and ordinary non-handoff launches do not display the prompt. The message
instructs the user to uninstall and reinstall Aris and explicitly states that
the database, settings, and user data will not be deleted. This is the exact
semantics recorded by fingerprint
`src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|MessageBoxW|be5dacb15c6c5840|1`.

The focused composition contract in
`tests/test_update_product_composition.py` checks these branch guards and the
exact owner/disposition/evidence metadata. It is a source/boundary contract;
the prompt itself remains subject to real GUI QA and is not claimed as GUI
acceptance here.
