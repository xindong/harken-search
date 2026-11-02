# Bootstrap exit explanation boundary

Technical inventory restored on 2026-10-29 from the six exact fingerprints
already pinned in tools/platform_boundary_check.py. The adapter uses Windows
module-path and KnownFolder queries, and releases the returned COM allocation.
These calls resolve the diagnostic output location. They grant no authority to
modify installation files or close other processes. New native calls remain
unapproved until their exact fingerprints are reviewed.
