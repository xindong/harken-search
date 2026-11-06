#include "platform/durable_file_ops.h"
#include "platform/settings_file.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/resource.h>

namespace fs = std::filesystem;
namespace files = Platform::FileSystem;
using Status = files::DurableFileOpStatus;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}
static std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
int main(int argc, char** argv) {
    try {
        require(argc == 2 && fs::path(argv[1]).is_absolute(), "explicit test directory supplied");
        const auto run = fs::path(argv[1]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path root = run / u8"设置与素材";
        require(fs::create_directories(root), "test directory is new");
        const auto source = root / "new.ini", target = root / "settings.ini";
        std::ofstream(source) << "new contents";
        std::ofstream(target) << "previous contents";
        auto result = files::FlushExistingFileToDisk(source.string());
        require(result.status == Status::Success, "file flush succeeds");
        result = files::CopyFileExclusiveAndFlush(source.string(), target.string());
        require(result.status == Status::TargetExists && result.systemError && read(target) == "previous contents", "exclusive copy preserves an existing target");
        const auto copied = root / "copied.ini";
        result = files::CopyFileExclusiveAndFlush(source.string(), copied.string());
        require(result.status == Status::Success && read(copied) == "new contents", "exclusive copy is flushed and readable");
        result = files::ReplaceFileWriteThrough((root / "missing.ini").string(), target.string());
        require(result.status == Status::ReplaceFailed && result.systemError && read(target) == "previous contents", "failed replacement preserves previous contents and reports errno");
        result = files::ReplaceFileWriteThrough(source.string(), target.string());
        require(result.status == Status::Success && read(target) == "new contents" && !fs::exists(source), "replacement commits new contents");
        result = files::DeleteFileIfPresent(copied.string());
        require(result.status == Status::Success && !fs::exists(copied), "durable deletion removes a file");
        require(files::DeleteFileIfPresent(copied.string()).status == Status::Success, "deletion tolerates already absent file");
        result = files::DeleteFileIfPresent((root / "missing-dir" / "file.ini").string());
        require(result.status == Status::DeleteFailed && result.systemError, "missing parent preserves delete error");
        result = files::SaveSettingsKeyValues(target.string(), {{"library", u8"中文素材库"}, {"theme", "night"}, {"volume", "0.42"}});
        require(result.status == Status::Success && read(target) == u8"# Aris settings (auto-generated)\nlibrary=中文素材库\ntheme=night\nvolume=0.42\n", "settings retain existing format and UTF-8 values");
        struct stat attributes {};
        require(stat(target.c_str(), &attributes) == 0 && (attributes.st_mode & 0777) == 0600, "settings are private to the user");
        const auto original = read(target);
        const auto parentLink = run / "linked-settings";
        fs::create_directory_symlink(root, parentLink);
        result = files::SaveSettingsKeyValues((parentLink / "settings.ini").string(), {{"library", "wrong"}});
        require(result.status != Status::Success && result.systemError && read(target) == original, "symlink parent cannot redirect settings writes");
        const auto fileLink = root / "link.ini";
        fs::create_symlink(target, fileLink);
        result = files::SaveSettingsKeyValues(fileLink.string(), {{"library", "wrong"}});
        require(result.status != Status::Success && fs::is_symlink(fileLink) && read(target) == original, "symlink settings target is rejected");
        result = files::SaveSettingsKeyValues(root.string(), {{"library", "wrong"}});
        require(result.status != Status::Success && fs::is_directory(root) && read(target) == original, "directory replacement fails without corrupting existing settings");
        struct rlimit previousLimit {};
        require(getrlimit(RLIMIT_FSIZE, &previousLimit) == 0, "file size limit can be read");
        auto restricted = previousLimit;
        restricted.rlim_cur = 1;
        const auto previousSignal = std::signal(SIGXFSZ, SIG_IGN);
        require(setrlimit(RLIMIT_FSIZE, &restricted) == 0, "real file write failure can be induced");
        result = files::SaveSettingsKeyValues(target.string(), {{"library", "must not replace the old file"}});
        const int restored = setrlimit(RLIMIT_FSIZE, &previousLimit);
        std::signal(SIGXFSZ, previousSignal);
        require(restored == 0 && result.status == Status::CopyFailed && result.systemError == EFBIG && read(target) == original, "failed temporary write preserves the previous settings");
        require(files::FlushExistingFileToDisk(std::string("bad\0path", 8)).status == Status::InvalidArgument, "embedded NUL is rejected");
        require(files::SaveSettingsKeyValues("", {}).status == Status::InvalidArgument, "empty settings path is rejected");
        std::cout << "Evidence retained at " << root << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
