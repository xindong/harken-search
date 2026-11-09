#include "platform/process_query.h"
#include "platform/system.h"

#include <spawn.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {

namespace fs = std::filesystem;

struct Child {
    pid_t pid = -1;
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept : pid(other.pid) { other.pid = -1; }
    void Stop() {
        if (pid <= 0) return;
        ::kill(pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
        pid = -1;
    }
    ~Child() { Stop(); }
};

Child StartChild(const fs::path& executable) {
    Child child;
    std::string name = executable.string();
    char childArg[] = "--child";
    char* arguments[] = {name.data(), childArg, nullptr};
    const int result = ::posix_spawn(&child.pid, name.c_str(), nullptr, nullptr, arguments, environ);
    if (result != 0) child.pid = -1;
    return child;
}

bool Contains(const std::vector<uint32_t>& ids, pid_t pid) {
    return std::find(ids.begin(), ids.end(), static_cast<uint32_t>(pid)) != ids.end();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--child") {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        return 0;
    }
    if (argc != 2) return 2;
    int failed = 0;
    const auto check = [&](bool condition, const char* name) {
        if (!condition) {
            ++failed;
            std::cerr << "FAIL: " << name << '\n';
        }
    };

    const fs::path root = fs::path(argv[1]) / ("run-" + std::to_string(::getpid()));
    const std::string imageName = "HarkenProbe" + std::to_string(::getpid());
    const fs::path first = root / "中文 A" / imageName;
    const fs::path second = root / "B" / imageName;
    fs::create_directories(first.parent_path());
    fs::create_directories(second.parent_path());
    fs::copy_file(fs::path(argv[0]), first);
    fs::copy_file(fs::path(argv[0]), second);
    ::chmod(first.c_str(), 0755);
    ::chmod(second.c_str(), 0755);

    check(Platform::ReadProcessImagePath(0).status == Platform::ProcessImagePathStatus::InvalidArgument,
          "zero PID rejected");
    const auto self = Platform::ReadProcessImagePath(static_cast<uint32_t>(::getpid()));
    check(self.status == Platform::ProcessImagePathStatus::Success && !self.pathUtf8.empty(),
          "own executable path readable");
    check(Platform::FindProcessByImageName("", "").status == Platform::ProcessImageLookupStatus::InvalidArgument,
          "empty name rejected");
    check(Platform::FindProcessByImageName("a/b", "").status == Platform::ProcessImageLookupStatus::InvalidArgument,
          "path is not an image name");
    check(Platform::FindProcessByImageName(imageName, std::string("\xff", 1)).status ==
              Platform::ProcessImageLookupStatus::EncodingFailed,
          "invalid preferred path encoding rejected");
    check(Platform::FindProcessByImageName(imageName, "").status == Platform::ProcessImageLookupStatus::NotFound,
          "absent process not found");

    Child a = StartChild(first);
    Child b = StartChild(second);
    check(a.pid > 0 && b.pid > 0, "two isolated child processes started");
    std::vector<uint32_t> ids;
    for (int attempt = 0; attempt < 40; ++attempt) {
        ids = Platform::FindProcessIdsByImageName(imageName + ".EXE");
        if (Contains(ids, a.pid) && Contains(ids, b.pid)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(Contains(ids, a.pid) && Contains(ids, b.pid), "case-insensitive Windows alias finds both Mac processes");
    const auto firstPath = Platform::ReadProcessImagePath(static_cast<uint32_t>(a.pid));
    check(firstPath.status == Platform::ProcessImagePathStatus::Success && firstPath.pathUtf8 == first.string(),
          "UTF-8 process path returned");
    const auto preferred = Platform::FindProcessByImageName(imageName, second.string());
    check(preferred.status == Platform::ProcessImageLookupStatus::Found &&
              preferred.executablePathUtf8 == second.string(), "preferred instance wins");
    const auto fallback = Platform::FindProcessByImageName(imageName, (root / "missing").string());
    check(fallback.status == Platform::ProcessImageLookupStatus::Found &&
              (fallback.executablePathUtf8 == first.string() || fallback.executablePathUtf8 == second.string()),
          "missing preferred path falls back to a real instance");
    check(Platform::TerminateProcessById(static_cast<uint32_t>(a.pid), 0, 0).status ==
              Platform::ProcessTerminationStatus::Unsupported, "termination remains unsupported");
    a.Stop();
    b.Stop();
    check(Platform::FindProcessByImageName(imageName, "").status == Platform::ProcessImageLookupStatus::NotFound,
          "exited processes disappear");

    if (failed) return 1;
    std::cout << "Mac process discovery checks: PASS\n";
    return 0;
}
