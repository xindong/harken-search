#include "platform/application_lookup_macos.h"
#include <filesystem>
#include <future>
#include <iostream>

int main() {
    using namespace Platform;
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "PASS: " : "FAIL: ") << label << '\n';
        if (!ok) ++failed;
    };
    for (const auto& identifier : {std::string(), std::string("com.apple\0.TextEdit", 19),
                                   std::string("\xff", 1), std::string("bad bundle id")})
        check(FindMacApplicationByBundleIdentifier(identifier).status == MacApplicationLookupStatus::InvalidArgument,
              "invalid identifier is rejected before the system query");
    const auto missing = FindMacApplicationByBundleIdentifier("app.cindy.harken.qa.missing.20261111");
    check(missing.status == MacApplicationLookupStatus::NotFound && missing.bundlePathUtf8.empty() &&
              missing.executablePathUtf8.empty(), "unregistered identifier has no guessed path");
    const auto present = FindMacApplicationByBundleIdentifier("com.apple.TextEdit");
    check(present.status == MacApplicationLookupStatus::Found &&
              std::filesystem::is_directory(std::filesystem::u8path(present.bundlePathUtf8)) &&
              std::filesystem::is_regular_file(std::filesystem::u8path(present.executablePathUtf8)),
          "actual installed TextEdit is found without launching it");
    std::cout << "actual bundle=" << present.bundlePathUtf8 << " executable=" << present.executablePathUtf8 << '\n';
    auto worker = std::async(std::launch::async, [] {
        return FindMacApplicationByBundleIdentifier("com.apple.TextEdit");
    });
    const auto background = worker.get();
    check(background.status == MacApplicationLookupStatus::Found &&
              background.bundlePathUtf8 == present.bundlePathUtf8 &&
              background.executablePathUtf8 == present.executablePathUtf8,
          "actual background query returns the same application");
    return failed ? 1 : 0;
}
