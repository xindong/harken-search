#include "platform/executable_file_dialog.h"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace Platform::ExecutableDialogs::Internal {
std::string ResolveExecutableSelectionUtf8(const std::string& selectedPath);
}

namespace {

namespace fs = std::filesystem;

void WriteFile(const fs::path& path, const std::string& content, mode_t mode) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();
    if (::chmod(path.c_str(), mode) != 0) throw std::runtime_error("chmod failed");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path root = fs::path(argv[1]) / ("run-" + std::to_string(::getpid()));
    fs::create_directories(root);
    int failed = 0;
    const auto check = [&](bool condition, const char* name) {
        if (!condition) {
            ++failed;
            std::cerr << "FAIL: " << name << '\n';
        }
    };

    const fs::path direct = root / "可执行 tool.sh";
    WriteFile(direct, "#!/bin/sh\nexit 0\n", 0755);
    check(Platform::ExecutableDialogs::Internal::ResolveExecutableSelectionUtf8(direct.string()) == direct.string(),
          "direct executable with UTF-8 and spaces");

    const fs::path nonExecutable = root / "plain.txt";
    WriteFile(nonExecutable, "not executable\n", 0644);
    check(Platform::ExecutableDialogs::Internal::ResolveExecutableSelectionUtf8(nonExecutable.string()).empty(),
          "reject non-executable file");
    check(Platform::ExecutableDialogs::Internal::ResolveExecutableSelectionUtf8(root.string()).empty(),
          "reject ordinary directory");

    const fs::path app = root / "音效工作站.app";
    const fs::path appExecutable = app / "Contents/MacOS/Launch Tool";
    WriteFile(app / "Contents/Info.plist",
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
              "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
              "<plist version=\"1.0\"><dict>"
              "<key>CFBundleIdentifier</key><string>test.harken.executable-picker</string>"
              "<key>CFBundlePackageType</key><string>APPL</string>"
              "<key>CFBundleExecutable</key><string>Launch Tool</string>"
              "</dict></plist>\n", 0644);
    WriteFile(appExecutable, "#!/bin/sh\nexit 0\n", 0755);
    check(Platform::ExecutableDialogs::Internal::ResolveExecutableSelectionUtf8(app.string()) == appExecutable.string(),
          "app bundle resolves its real executable");

    const fs::path brokenApp = root / "Broken.app";
    WriteFile(brokenApp / "Contents/Info.plist",
              "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
              "<key>CFBundleExecutable</key><string>Missing</string>"
              "</dict></plist>\n", 0644);
    check(Platform::ExecutableDialogs::Internal::ResolveExecutableSelectionUtf8(brokenApp.string()).empty(),
          "reject app without executable");
    check(Platform::ExecutableDialogs::Internal::ResolveExecutableSelectionUtf8((root / "missing").string()).empty(),
          "reject missing file");
    check(Platform::ExecutableDialogs::Internal::ResolveExecutableSelectionUtf8("").empty(),
          "empty selection leaves setting unchanged");
    std::string embeddedNul = direct.string() + std::string("\0suffix", 7);
    check(Platform::ExecutableDialogs::Internal::ResolveExecutableSelectionUtf8(embeddedNul).empty(),
          "reject embedded NUL");

    std::string fromWorker = "unexpected";
    std::thread worker([&] {
        fromWorker = Platform::ExecutableDialogs::OpenExecutableFileDialogUtf8(nullptr, L"", L"");
    });
    worker.join();
    check(fromWorker.empty(), "non-main-thread call cannot open a panel");

    if (failed) return 1;
    std::cout << "Mac executable dialog checks: PASS\n";
    return 0;
}
