#include "src/platform/filesystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    namespace fs = std::filesystem;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::current_path() / ("region-export-path-safety-" + std::to_string(nonce));
    std::error_code error;
    if (!fs::create_directory(root, error)) {
        std::cerr << "Cannot create isolated test directory: " << error.message() << '\n';
        return 1;
    }
    const auto source = root / fs::u8path(u8"源音频.wav");
    const auto other = root / "other.wav";
    const auto alias = root / "source-alias.wav";
    { std::ofstream out(source, std::ios::binary); out << "source"; }
    { std::ofstream out(other, std::ios::binary); out << "other"; }
    fs::create_hard_link(source, alias, error);
    if (error) {
        std::cerr << "Cannot create source hard link: " << error.message() << '\n';
        return 1;
    }
    const auto sourceUtf8 = source.u8string();
    if (!Platform::FileSystem::Equivalent(sourceUtf8, sourceUtf8) ||
        !Platform::FileSystem::Equivalent(sourceUtf8, alias.u8string()) ||
        Platform::FileSystem::Equivalent(sourceUtf8, other.u8string()) ||
        Platform::FileSystem::Equivalent(sourceUtf8, (root / "new.wav").u8string()) ||
        Platform::FileSystem::Equivalent(sourceUtf8, "")) {
        std::cerr << "Region export source identity policy failed\n";
        return 1;
    }
    std::cout << "Region export source identity policy passed\n";
    return 0;
}
