# Static loader

WebView2LoaderStatic.lib is from the official Microsoft.Web.WebView2 1.0.2903.40
NuGet package, matching the existing header and DLL. The downloaded DLL was
compared byte for byte with the vendored DLL.

Source: https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.2903.40/microsoft.web.webview2.1.0.2903.40.nupkg

File: build/native/x64/WebView2LoaderStatic.lib

SHA256: e738055f2838022230fb0056c09ecff6395cb0138924fcd4f091748b41d12c73

License: the existing LICENSE.txt and NOTICE.txt apply. The updater links the
loader statically because only its EXE is copied into the authenticated runtime
pair; a loader DLL in the installation directory could be replaced mid-update.
