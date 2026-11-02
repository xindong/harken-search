# Third-Party Notices

Harken Search public-release builds bundle, link, or intentionally reference the third-party materials listed below. Ship this file together with `LICENSE`, `ASSET_PROVENANCE.md`, `BINARY_PROVENANCE.md`, `PRIVACY.md`, `AI_FEATURE_DISCLOSURE.md`, and the `LICENSES/` directory.

## Bundled in the current public-release output

| Component | Source / path | License | Usage in Harken Search public release |
|---|---|---|---|
| Dear ImGui | `thirdparty/imgui/` | MIT | Immediate-mode desktop UI framework |
| miniaudio | `thirdparty/miniaudio.h` | Public Domain / MIT-0 dual | Audio decode and playback |
| SQLite | `thirdparty/sqlite/` amalgamation | Public Domain | Database and FTS indexing |
| stb image headers | `thirdparty/stb_image*.h` | Public Domain / MIT dual | Image decode / resize / write |
| xxHash | `thirdparty/xxhash.h` | BSD-2-Clause | Cover-cache and content hashing |
| nlohmann/json | `thirdparty/nlohmann_json.hpp` | MIT | JSON parsing |
| whisper.cpp | `thirdparty/whisper.cpp/` | MIT | Local speech-to-text support |
| IXWebSocket | `thirdparty/ixwebsocket/` | BSD-3-Clause | WAAPI and localhost bridge transport |
| rlottie | `thirdparty/rlottie/` | MIT + permissive bundled sub-licenses | Lottie animation renderer |
| Microsoft WebView2 Loader | `thirdparty/webview2/` + `build_public/Release/WebView2Loader.dll` + `HarkenSearchBootstrap.exe` | Microsoft WebView2 SDK license | Bundled loader DLL for startup/login and static loader for the independent updater; the full Microsoft Edge WebView2 Runtime must still be installed on target machines |
| REAPER Plug-in API header subset | `reaper_extension/reaper_plugin.h` | REAPER SDK zlib-style notice | Altered minimal ABI declarations used by the Harken Search REAPER extension; the source header retains the upstream notice and marks the alteration |
| TweetNaCl verify-only subset | `src/ed25519/tweetnacl.c` + `.h` | Public domain | Ed25519 signature verification; Harken Search removed keypair generation and signing and retains the original author attribution |
| Lucide icon font | `thirdparty/lucide/lucide.ttf` + generated `IconsLucide.h` | ISC | Shipped icon font used by the desktop UI |
| DiceBear Lorelei Neutral avatar pool | `assets/avatars/dicebear/lorelei-neutral/*.svg` | DiceBear core MIT; generated style assets described below | Local avatar pool shipped beside `HarkenSearch.exe`; startup must not call the public DiceBear API |
| Unicode Unihan kMandarin | generated `src/generated/mandarin_pinyin_table.inc` | Unicode Terms of Use | Mandarin syllable lookup for Chinese filename search; generated from Unicode 18.0.0, file date 2026-07-31, first listed reading only |
| Lorelei Neutral | Embedded metadata inside `assets/avatars/dicebear/lorelei-neutral/*.svg` | CC0 1.0 by Lisa Wischofsky | Source avatar style used for the shipped DiceBear-derived SVG pool |

## Source-derived / attribution-only references

The Windows installer uses [Inno Setup](https://jrsoftware.org/) and the upstream Simplified Chinese translation maintained by Zhenghan Yang (Kira). The original translation is retained in `installer/Languages/ChineseSimplified.isl` (upstream Git blob `d0d44258f51aa3127fc0d1167a7c8be328a9bdcf`); the Inno Setup license is bundled as `LICENSES/Inno-Setup.txt`.

| Component | License | Repository usage |
|---|---|---|
| Klangfreund LUFSMeter | MIT | `src/AudioEngine.cpp` keeps attribution because the BS.1770 loudness implementation was cross-checked against LUFSMeter; the upstream MIT text is bundled as `Klangfreund-LUFSMeter-MIT.txt` |

## Source-only transitive files not compiled into the current product

| Component | Repository path | License | Status |
|---|---|---|---|
| CLI11 2.0.0 | `thirdparty/ixwebsocket/third_party/cli11/` | BSD-3-Clause | Retained as part of the upstream IXWebSocket source tree; demos are disabled |
| cpp-linenoise | `thirdparty/ixwebsocket/third_party/cpp-linenoise/` | BSD-2-Clause | Retained as part of the upstream IXWebSocket source tree; demos are disabled |
| msgpack11 | `thirdparty/ixwebsocket/third_party/msgpack11/` | MIT | Retained as part of the upstream IXWebSocket source tree; demos are disabled |

## Optional, user-supplied, or excluded from default public redistribution

| Component | Source / path | License / policy | Release policy |
|---|---|---|---|
| aubio | Local `thirdparty/aubio/` checkout when explicitly supplied | GPL-3.0 | OFF by default; enabling it changes the resulting binary's license obligations |
| Avid PTSL SDK | Local install via `AURO_PROTOOLS_PTSL_SDK_ROOT` or `AURO_PROTOOLS_PTSL_DROP_DIR` | Avid developer agreement / click-through license | User-supplied SDK/runtime only; `PTSLC_CPP.dll` is not copied into default outputs. See RQ-20260803-13 for the GPL / proprietary SDK redistribution boundary. |
| Full REAPER SDK checkout | Optional local `AURO_REAPER_SDK_ROOT` checkout | REAPER SDK terms; individual example/plugin trees may carry separate terms | Not vendored or bundled; Harken Search consumes no `reaper_mp3` LGPL payload |

## Notes for release builders

- Public release packages should be produced from `build_public/Release/` with `-DARIS_PUBLIC_RELEASE=ON`.
- `WebView2Loader.dll` is only the loader; end-user systems still need the Microsoft Edge WebView2 Runtime.
- `HarkenSearchProToolsBridge.exe` is Harken Search-authored code, but any Avid SDK/runtime use remains subject to Avid's separate license terms.
- `PTSLC_CPP.dll` is not staged by default; keep `AURO_BUNDLE_AVID_PTSL=OFF` unless legal/licensing review explicitly approves redistribution for the package being built.
- Verbatim license and notice texts for bundled components are copied into `LICENSES/`; keep `THIRD_PARTY_NOTICES.md` and `LICENSES/README.md` in sync.
