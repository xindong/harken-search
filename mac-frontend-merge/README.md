# macOS front end on qa/current — evidence

Built from `integrate/mac-frontend-79c2d82` (PR #262), which is `79c2d82`
("macOS AppKit front end with an open-library and search shell") merged with
`origin/qa/current` `1a80cf8`. The front-end commit is kept as an ancestor and
its content is untouched.

| file | what it shows |
| --- | --- |
| `01-open-library-shell.png` | first frame of the merged macOS build: the real open-library + search shell. `ShowDemoWindow` and the commented-out `AuroMainLoop` placeholder are gone. |
| `02-library-open-24-results.png` | an isolated test library (`QA Demo.db`, 24 audio files) opened through the native panel: library path, folder tree, search field, rename-prefix field and the results list. |
| `mac-frontend-merge-qa.mov` | 44 s window recording of the same build: results, reopening the library through the native panel, results again. 1552x968 pt (3104x1936 px), ~57 fps. |

`06-merged-qa-current-shell.png` and `07-merged-qa-current-library-open.png` are the
same check re-run from the merge commit GitHub produced (`d92f6c6`, whose base had
moved on to `ced244f`): it rebuilds clean and shows the same shell and the same
24 results, so the merged tree — not just the branch tip — is what was verified.

Local build notes:

* Apple Silicon, `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release`, AppleClang 21, macOS 26.5.1.
* The user's real library was never touched. The app ran with `HOME` pointed at a
  scratch directory and a copy of a test library, never the live `Application Support` data.
* The generated `HarkenSearch.app/Contents/Info.plist` ships an empty
  `CFBundleIdentifier`; with an empty identifier macOS refuses to service the
  out-of-process open panel, so the panel opened silently-nothing. Setting an
  identifier in the local bundle (build artifact only, no source change) makes the
  panel work. This is a packaging gap worth its own card.
