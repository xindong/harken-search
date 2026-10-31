# Aris update application adapter boundary audit

**Owner slice:** `UPDATE-APPLICATION`
**Reviewed path:** `src/update/ArisUpdateComposition.cpp`
**Disposition:** `platform-adapter`

This file is the narrow native composition port for the updater handoff. It
owns the Windows named-channel setup, inherited-handle launch, exact process
handoff, and updater-owned staging/backup inventory probes needed to invoke the
independent Bootstrap executable. Pure update policy remains in the typed
update services; this exception does not authorize UI, database, audio, or
user-data paths.

The register records the exact current scanner fingerprints only. Any moved,
new, or changed native or filesystem operation is unresolved until this audit
and its tests are refreshed. No path prefix, token pattern, or scanner
suppression is implied.
