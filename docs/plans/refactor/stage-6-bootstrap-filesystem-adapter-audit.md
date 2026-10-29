# Aris Bootstrap Windows filesystem adapter boundary audit

**Updated:** 2026-07-29
**Owner slice:** `BOOTSTRAP-FILESYSTEM`
**Reviewed path:** `src/bootstrap/WindowsBootstrapFilesystemWin32.cpp`
**Disposition:** `platform-adapter`

## Ownership decision

The reviewed file is the native Windows implementation of the Bootstrap
filesystem port. It owns no-follow root pinning and identity observation,
security-descriptor capture and reapplication, journal/backup publication,
replacement, rollback, and restart-recovery filesystem primitives. The pure
transaction policy and durable-journal codec remain in the adjacent Bootstrap
core files and depend on the abstract backend declared by
`WindowsBootstrapFilesystem.h`; product entrypoints, the update runtime, UI,
process control, health IPC, and signature verification do not depend on this
adapter in this slice.

Moving the Win32 calls behind another forwarding wrapper would not remove a
native dependency or improve ownership. It would split one already reviewed
platform adapter across files and obscure the handle-relative mutation and
rebind invariants. The narrow exception therefore applies to this exact file
and these exact scanner fingerprints only.

## Audited native surface

The Stage 6 scanner reports `102` findings in the reviewed path. The original
`64` fingerprints are preserved exactly as a subset; `38` additional calls
became visible when the scanner vocabulary was made exhaustive for the two
reviewed Bootstrap adapters:

- `2` `native-include` findings: `Windows.h` and `bcrypt.h`;
- `100` `native-symbol` findings covering BCrypt hashing, pinned-handle
  lifecycle, UTF-8 conversion, exact-root normalization, journal-candidate
  enumeration, and native error capture.

Every finding is recorded with its exact fingerprint, path, line, rule, token,
occurrence, owner, disposition, and this evidence path in
`stage-6-platform-audited-exceptions.json`. The validator independently freezes
the approved fingerprint set. A new or changed path, rule, token, normalized
source context, occurrence, owner, disposition, or evidence reference remains
unresolved. No directory glob, path prefix, token regular expression, scanner
suppression, or general native-symbol exemption is used.

The pre-existing `11` `platform-entry` exceptions remain a separate exact
allowlist and retain their existing evidence and owner semantics. A Bootstrap
include cannot be relabelled as `platform-entry`, and an application-entry
finding cannot inherit the adapter category.

## Security and rollback boundary

The adapter treats the validated plan and trusted, identity-bound roots as
inputs. It returns typed observations and mutation outcomes to the Bootstrap
transaction core. Staging identity/content and tamper-relevant file shape
(including hard links, alternate streams, extended attributes, and unsupported
attributes) are reobserved. Staging owner, DACL, SACL, and integrity label are
neither journal-bound nor copied into the installed payload. Replacement
preserves the verified target metadata; creation relies on Windows
parent-default inheritance plus parent-stability and prepublication security
checks on the temporary file, not a universal proof of every child ACL outcome. Rollback
restores content and the supported journal-bound metadata envelope; ordinary
audit SACLs are excluded. Reparse, final-path, FileId, volume, metadata, or
durable-publication ambiguity fails
closed into the core recovery or quarantine decision.

This audit does not authenticate the upper-layer invocation and does not claim
that the Bootstrap executable or automatic update is complete. Remaining
limits are recorded in `stage-4-report.md`, including non-handle-relative
`FindFirstFileW` enumeration, SACL and privilege constraints, inheritance proof
limits, trusted-process TOCTOU, and incomplete crash-cut/quarantine execution
coverage.

## Reproducible gate

The closure command is:

```powershell
python -B tools/platform_boundary_check.py --root . `
  --check-ledger docs/plans/refactor/stage-6-platform-migration-ledger.json `
  --check-exceptions docs/plans/refactor/stage-6-platform-audited-exceptions.json `
  --require-empty
```

At the current source-freeze snapshot the repository scanner reports `480`
raw findings. The exact register contains `464` audited exceptions (`11`
`platform-entry`, `102` `BOOTSTRAP-FILESYSTEM`, and `351`
`BOOTSTRAP-RUNTIME`), with `16` unrelated unresolved findings confined to
`src/update/UpdateCheckHttpsTransport.cpp` and `0` migration-ledger findings.
The executable contract is
`tests/test_stage6_platform_boundary.py`.
