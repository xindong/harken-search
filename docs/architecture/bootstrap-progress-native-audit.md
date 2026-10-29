# Windows updater progress boundary

Reviewed 2026-10-29 against the integrated updater. Independent read-only review
checked the 44 exact scanner fingerprints in three Windows-only files. Existing
593 fingerprints remain unchanged; line metadata is refreshed against this tree.

- WindowsBootstrapProgress.cpp owns a separate STA, events, window/timers
  and UTF conversion. The business-facing observer accepts owned snapshots only.
- WindowsBootstrapWebView.cpp owns WebView2 and copies JSON to embedded HTML.
  Messages accept only ready and terminal close; no file replacement, process
  launch, permission or update-retry command is exposed to the page.
- WindowsBootstrapWebView.h is private to the Windows adapters. Native types
  do not leak into BootstrapProgress or BootstrapProgressView.
- Production WebView2 writes browser cache under the current user's
  LocalAppData/Harken Search/UpdateWebView. It does not open the user's library,
  product settings or installation manifest, or modify DACL/owner.
- The two ofstream fingerprints and test sampling calls are inside
  HARKEN_BOOTSTRAP_WEBVIEW_TESTING. Only test targets define that macro. No generic
  filesystem exception or directory-prefix suppression is granted.
- CSP blocks external resources and forms; permission requests are denied.

The audit tool binds each full path, source-line fingerprint, occurrence, rule,
owner and this evidence file. Added calls, changed fingerprints or moves into an
unapproved file fail the check; moving an unchanged call within its approved
file does not by itself change the audited boundary.
