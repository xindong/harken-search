# Airs Stage 6 native-entry boundary audit

**Updated:** 2026-07-23
**Code evidence:** `ff9e9ff`
**Scope:** the 11 remaining `native-include` findings in the Stage 6 scanner.

These findings are retained as exact, evidence-backed `platform-entry` entries
in `stage-6-platform-audited-exceptions.json`. They remain visible to the raw
scanner; only the unresolved migration ledger is empty. They are not silently
removed or hidden behind a forwarding header.

| Include | Direct surface in the consumer | Why a same-semantics narrow PAL move is not currently safe | Evidence |
| --- | --- | --- | --- |
| `src/WindowsMain.cpp` `windows.h` | WinMain, window classes, message loop, tray, HWND/HDC/HGLRC setup, Win32 window procedure | This is the Windows application entrypoint and owns the window/event lifecycle; extracting it would be an entrypoint architecture change, not a local PAL substitution. | `TestArisProcessGate`, `TestArisGate`, `TestStage6PlatformBoundary`, full CTest |
| `src/WindowsMain.cpp` `shellapi.h` | `HDROP`, `DragFinish`, `DragAcceptFiles` | File-drop message handling is coupled to the entrypoint window procedure and startup file routing. | `TestArisGate`, `TestMainEncodingContract`, full CTest |
| `src/WindowsMain.cpp` `ole2.h` | `OleInitialize`, `OleUninitialize`, OLE/WebView2 declarations | A focused lifecycle-only extraction was tested and rejected: `WebView2.h` then failed to compile because it relies on OLE interface declarations from this include. | Failed clean `Aris` compile, then restored; full CTest at `ff9e9ff` |
| `src/WindowsMain.cpp` `dbghelp.h` | `MiniDumpWriteDump`, `SymInitialize`, `SymFromAddr`, `SymCleanup` | Crash-time stack/minidump code crosses process state, exception context, and symbol-handler ownership; a safe PAL contract needs a dedicated crash artifact design. | `TestBehaviorContractCheck`, `TestBehaviorRegressionSuite`, full CTest |
| `src/WindowsMain.cpp` `WebView2.h` | WebView2 controller, navigation, settings, callback and loading-screen interfaces | The login/loading surface is a COM callback graph embedded in the Windows entrypoint; moving only the include would not create an ownership-safe seam. | `TestArisGateCompatibility`, full CTest |
| `src/WindowsMain.cpp` `wrl.h` | `Microsoft::WRL::ComPtr` and `Callback` for WebView2 | Same callback/COM ownership boundary as `WebView2.h`; requires a complete WebView host owner, not a header relocation. | `TestArisGateCompatibility`, full CTest |
| `src/coverwall_lab/CoverWallLabMain.cpp` `windows.h` | WGL window creation, Win32 message pump and HWND/HDC/HGLRC lifetime | This is a standalone OpenGL lab entrypoint. A PAL would need to own the entire lab window/context contract and is outside the current product slice. | `TestCoverWallLabContract`, full CTest |
| `src/main.cpp` `windows.h` | Win32 drag/drop, window handles, file-dialog types and host UI integration | The file is the primary UI/application module with several coupled Win32 surfaces; narrow replacement would cross multiple product flows. | `TestMainEncodingContract`, full CTest |
| `src/main.cpp` `shlobj.h` | `IDataObject` format enumeration and `SHGetSpecialFolderPathW` | OLE drag/drop implementation directly calls `SHCreateStdEnumFmtEtc`; the AppData call alone cannot be separated without redesigning the shared data-object path. | `TestMainEncodingContract`, full CTest |
| `src/main.cpp` `commdlg.h` | Multiple `OPENFILENAMEW` open/save dialogs with distinct filters, defaults and call-site behavior | Existing generic dialog PAL does not cover all flags, initial paths, multi-select and export semantics used here; a safe migration requires a structured dialog contract and broad UI regression. | `TestMainEncodingContract`, `TestFolderBrowserContract`, full CTest |
| `src/main.cpp` `ole2.h` | `IDataObject`, `FORMATETC`, `STGMEDIUM`, `CF_HDROP`, OLE status values | This is the actual Pro Tools drag/drop data-object implementation; removing OLE headers requires moving the complete COM object and ownership graph. | `TestCubaseDirectInsertContract`, `TestProToolsToolsContract`, full CTest |

## Historical closure evidence

- Scanner raw findings: `11/11`, all `native-include`.
- Audited platform-entry exceptions: `11/11`, with exact fingerprints and this
  document as evidence.
- Unresolved findings: `0`; migration ledger findings: `0`.
- Closure command with `--check-ledger`, `--check-exceptions`, and
  `--require-empty`: passed.
- Platform boundary suite: `380/380` with 18 subtests (included in full CTest).
- Full CTest at closure commit `b78a9a1`: `172/172`, `354.12s`.
- Debug product link: passed; current `Aris.exe` SHA-256 is `B9060218E931780DE238F8C869DE9A69FA3F4A18CDEABC8971AC2AAE509FD444`.
- Graphify audit: `64053` call sites, `7501` `UNKNOWN_BLOCKING`.
- The four non-GUI probes had five-round evidence in the historical headless-r3 run (original logs retained privately); the index is intentionally `headless_only` and is not promoted to the five-probe baseline until the frame probe has five valid rounds.
- Frame preflight remains an environment gate (`window_invariant`) when no dedicated foreground window is available. No frame probe was started in this audit because it would compete with the user's desktop.

This document records an explicit, reproducible boundary decision. The PAL
migration backlog is closed; Stage 6 still remains incomplete until the fresh
five-probe frame evidence passes.

## Public historical reference

The adjacent performance baseline preserves the original five-round measurements and limits. Its machine name is anonymized; historical-evidence/five-probe-r3/ identifies archived records, not files distributed with this repository. Original private locations are retained in local audit evidence. These historical measurements do not claim a fresh performance run.
