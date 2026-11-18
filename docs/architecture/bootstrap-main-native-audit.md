# Bootstrap launcher platform boundary

Reviewed 2026-11-18 against the integrated bootstrap launcher
(`src/bootstrap/HarkenSearchBootstrapMain.cpp`). The eight scanner findings in
that file are the launcher's install-mutation guard; no other Win32 surface is
exposed by this translation unit.

- `InstallMutationGuard` builds a per-install named mutex from the install
  directory's volume serial number and file id, so two launchers cannot mutate
  the same installation at once.
- `CreateFileW` opens only the install directory itself with
  `FILE_READ_ATTRIBUTES` and `FILE_FLAG_BACKUP_SEMANTICS`; it never opens user
  library files, the product database or user documents.
- `GetFileInformationByHandleEx(FileIdInfo)` reads that directory identity;
  a zero volume serial or all-zero file id fails closed and no guard is taken.
- `MultiByteToWideChar` only widens the UTF-8 install root to UTF-16 for the
  two calls above; invalid sequences fail closed.
- `CloseHandle` releases the directory handle and the mutex handle owned by the
  guard. `WaitForSingleObject` with a zero timeout only tests mutex ownership;
  a contended guard fails closed instead of blocking.
- No native handle, HWND, HRESULT or COM interface leaves this file: the
  launcher's business units keep using plain paths and results only.

The audit tool binds each full path, source-line fingerprint, occurrence, rule,
owner and this evidence file. Added calls, changed fingerprints or moves into an
unapproved file fail the check; moving an unchanged call within this approved
file does not by itself change the audited boundary.
