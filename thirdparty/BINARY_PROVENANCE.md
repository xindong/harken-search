# BINARY_PROVENANCE

This file tracks third-party or externally-derived runtime payloads and
link-only binary artifacts stored in the repository. Runtime DLLs are copied
beside `Aris.exe`; `.lib` / `.dll.lib` entries are build inputs or import
libraries and are not copied into the installer unless another row says so.

| Artifact | Source URL | Version/commit | Built by us | Build command | License | SHA256 |
|---|---|---|---|---|---|---|
| `WebView2Loader.dll` | <https://developer.microsoft.com/en-us/microsoft-edge/webview2/> | File version `1.0.2903.40`; vendored under `thirdparty/webview2/` | No (vendor binary) | Copied by CMake post-build from `thirdparty/webview2/build/native/x64/WebView2Loader.dll` | Microsoft WebView2 SDK license | `462b36fd1be6ca9f7563466a89e57c41ef4a4def3e0a84fa885d203aea4a3aaf` |
| `WebView2LoaderStatic.lib` | <https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.2903.40/microsoft.web.webview2.1.0.2903.40.nupkg> | Official SDK `1.0.2903.40`, matching the existing headers and DLL | No (vendor binary) | Statically linked into `HarkenSearchBootstrap.exe`; source record in `thirdparty/webview2/STATIC_LOADER_SOURCE.md` | Microsoft WebView2 SDK license | `e738055f2838022230fb0056c09ecff6395cb0138924fcd4f091748b41d12c73` |
| `rlottie.dll` | <https://github.com/Samsung/rlottie> | Vendored `rlottie` CMake project version `0.2` | Yes | `cmake --build build_public --config Release --target Aris -- /m` | MIT + permissive bundled sub-licenses | `eebf16e1fca7e8698d8da7634ce72f4b973fbf28ad0b373df70f80d026002843` |
| `rlottie-image-loader.dll` | <https://github.com/Samsung/rlottie> | Built from the vendored `rlottie` image-loader module that ships with `rlottie` `0.2` | Yes | `cmake --build build_public --config Release --target Aris -- /m` | MIT + permissive bundled sub-licenses | `3cee0f377fa33db45a158a94b9de5062cbc381c1fb71d1719c92a03916254d37` |
| `rlottie.lib` | <https://github.com/Samsung/rlottie> | Vendored `rlottie` `0.2`; link-only import/static artifact, not an installer payload | Yes | `cmake --build thirdparty/rlottie/build --config Release` | MIT + permissive bundled sub-licenses | `effd69d07a2d131864f9dd77f6958e7b32b3b272fb0de430fc519be5092bad39` |
| `rlottie-image-loader.dll.lib` | <https://github.com/Samsung/rlottie> | Vendored `rlottie` `0.2` image-loader import library; link-only, not an installer payload | Yes | `cmake --build thirdparty/rlottie/build --config Release` | MIT + permissive bundled sub-licenses | `3afae9090ddd3ff1403f767e3f4893e709a8a129392117fe639878ab094cc242` |
| `whisper.lib` | <https://github.com/ggerganov/whisper.cpp> | Vendored under `thirdparty/whisper.cpp/`; commit pinned in `CMakeLists.txt` | Yes | `cmake --build thirdparty/whisper.cpp/build --config Release` | MIT | `947355401b7b421e7cf3e3384a1b6c9e4d8de09b0ab965b3bb430d93a9088498` |
| `ggml.lib` | <https://github.com/ggerganov/ggml> | Bundled as part of `whisper.cpp`; built from `thirdparty/whisper.cpp/ggml/` | Yes | `cmake --build thirdparty/whisper.cpp/build --config Release` | MIT | `221df9e9040450150858120238499027321ea8433b179aed180bb6a15c954372` |
| `ggml-base.lib` | <https://github.com/ggerganov/ggml> | Bundled as part of `whisper.cpp`; built from `thirdparty/whisper.cpp/ggml/` | Yes | `cmake --build thirdparty/whisper.cpp/build --config Release` | MIT | `3e3cf7d283d0459ed177a9fcf6396aade27a6ca29852e189a35b528c717106eb` |
| `ggml-cpu.lib` | <https://github.com/ggerganov/ggml> | Bundled as part of `whisper.cpp`; built from `thirdparty/whisper.cpp/ggml/` | Yes | `cmake --build thirdparty/whisper.cpp/build --config Release` | MIT | `0091d9b6887a2e64028202ecf24ca242b3feb51309d5e43db2da41250d33511b` |
