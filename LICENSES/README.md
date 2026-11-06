# LICENSES bundle index

This directory is copied into the public release output and carries verbatim license / notice texts for bundled third-party components.

## Included files

- `ONNX-Runtime-MIT.txt` and `ONNX-Runtime-ThirdPartyNotices.txt` - optional ONNX Runtime 1.20.1 CPU SDK/runtime
- `hnswlib-Apache-2.0.txt` - optional hnswlib 0.8.0 headers
- `CLAP-Apache-2.0.txt` - optional pinned LAION CLAP HTSAT unfused audio model, converted to ONNX
- `Inno-Setup.txt` - Inno Setup installer and Simplified Chinese language messages

- `ImGui-MIT.txt` - Dear ImGui
- `IXWebSocket-BSD-3-Clause.txt` - IXWebSocket
- `whisper.cpp-MIT.txt` - whisper.cpp
- `rlottie-Licensing.txt` - rlottie license overview from upstream
- `rlottie-MIT.txt` - rlottie core MIT text
- `rlottie-COPYING.FTL.txt` - FreeType notice bundled by rlottie
- `rlottie-COPYING.MPL.txt` - MPL notice bundled by rlottie
- `rlottie-COPYING.PIX.txt` - Pixman notice bundled by rlottie
- `rlottie-COPYING.RPD.txt` - RapidJSON notice bundled by rlottie
- `rlottie-COPYING.SKIA.txt` - Skia notice bundled by rlottie
- `rlottie-COPYING.STB.txt` - stb notice bundled by rlottie
- `WebView2-LICENSE.txt` - Microsoft WebView2 SDK license
- `WebView2-NOTICE.txt` - Microsoft WebView2 SDK notice bundle
- `Lucide-LICENSE.txt` - Lucide upstream LICENSE (includes Feather-derived icon notice)
- `DiceBear-MIT.txt` - DiceBear core/library MIT license
- `CC0-1.0.txt` - Creative Commons CC0 1.0 text used for the shipped Lorelei Neutral avatar style
- `miniaudio-LICENSE.txt` - miniaudio Public Domain / MIT-0 (dual-license)
- `SQLite-LICENSE.txt` - SQLite Public Domain dedication
- `stb-LICENSE.txt` - stb (stb_image, stb_vorbis) Public Domain / MIT (dual-license)
- `xxHash-LICENSE.txt` - xxHash BSD 2-Clause
- `nlohmann-json-LICENSE.txt` - nlohmann/json MIT
- `Klangfreund-LUFSMeter-MIT.txt` - Klangfreund LUFSMeter MIT license
- `REAPER-SDK-zlib.txt` - Cockos REAPER Plug-in API SDK notice for the altered minimal header subset
- `TweetNaCl-PUBLIC-DOMAIN.txt` - TweetNaCl public-domain provenance and altered-subset notice
- `CLI11-BSD-3-Clause.txt` - CLI11 source-only transitive license
- `cpp-linenoise-BSD-2-Clause.txt` - cpp-linenoise source-only transitive license
- `msgpack11-MIT.txt` - msgpack11 source-only transitive license
- `Unicode-Terms-of-Use.txt` - Unicode Terms of Use for the generated Unihan kMandarin lookup table

## Mapping back to shipped components

- `THIRD_PARTY_NOTICES.md` declares which bundled components must retain notices.
- `ASSET_PROVENANCE.md` explains the DiceBear / Lorelei Neutral asset provenance.
- `BINARY_PROVENANCE.md` tracks shipped and link-only third-party binary hashes, including WebView2, rlottie, whisper.cpp, and ggml artifacts.

Keep this index synchronized with `THIRD_PARTY_NOTICES.md` whenever bundled dependencies change.
