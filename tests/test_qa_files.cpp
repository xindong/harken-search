#include "platform/qa_files.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
using Platform::QaFiles::ReadCommandJson;
using Platform::QaFiles::WriteResultAtomically;
namespace QaFilesTesting = Platform::QaFiles::Testing;

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << message << '\n'; }
}
std::string Utf8(const fs::path& path) { return path.u8string(); }
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}

int main() {
    const fs::path base = fs::current_path() / "test_qa_files_tmp";
    std::error_code error;
    fs::remove_all(base, error);
    fs::create_directories(base);

    std::string json = "stale";
    Check(!ReadCommandJson(Utf8(base / "missing.json"), json, 8) && json.empty(),
          "missing command clears output");

    const fs::path empty = base / "empty.json";
    Write(empty, {});
    json = "stale";
    Check(!ReadCommandJson(Utf8(empty), json, 8) && json.empty(),
          "empty command clears output");
    Check(fs::exists(empty), "invalid empty command is not consumed");

    const fs::path large = base / "large.json";
    Write(large, "123456789");
    json = "stale";
    Check(!ReadCommandJson(Utf8(large), json, 8) && json.empty(),
          "over-limit command clears output");
    Check(fs::exists(large), "over-limit command is not consumed");

    const fs::path command = base / "command.json";
    Write(command, "12345678");
    Check(ReadCommandJson(Utf8(command), json, 8) && json == "12345678",
          "bounded command read");
    Check(!fs::exists(command), "successful command is consumed before parsing");
    Check(!ReadCommandJson(Utf8(command), json, 8) && json.empty(),
          "successful command is consumed exactly once");

    const fs::path raced = base / "raced.json";
    Write(raced, "race");
    QaFilesTesting::RemoveBeforeRetirementOnce();
    Check(!ReadCommandJson(Utf8(raced), json, 8) && json.empty(),
          "lost retirement race does not expose duplicate command");

    const fs::path result = base / "result.json";
    const fs::path temporary = base / "result.tmp";
    Write(result, "old");
    const std::string payload("{\0}", 3);
    Check(WriteResultAtomically(Utf8(result), Utf8(temporary), payload),
          "atomic result write succeeds");
    Check(Read(result) == payload, "result write is binary safe");
    Check(!fs::exists(temporary), "successful replacement consumes temporary file");

    const fs::path blockedTarget = base / "blocked-target";
    const fs::path failedTemporary = base / "failed.tmp";
    fs::create_directories(blockedTarget);
    Check(!WriteResultAtomically(Utf8(blockedTarget), Utf8(failedTemporary), "new"),
          "failed replacement reports failure");
    Check(!fs::exists(failedTemporary), "failed replacement cleans temporary file");

#ifdef _WIN32
    {
        const fs::path lockedResult = base / "locked-result.json";
        const fs::path lockedTemporary = base / "locked.tmp";
        Write(lockedResult, "old");
        std::atomic<bool> locked{false};
        std::thread holder([&] {
            HANDLE handle = CreateFileW(lockedResult.wstring().c_str(),
                                        GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            locked.store(true);
            if (handle != INVALID_HANDLE_VALUE) {
                Sleep(50);
                CloseHandle(handle);
            }
        });
        while (!locked.load()) std::this_thread::yield();
        Check(WriteResultAtomically(Utf8(lockedResult), Utf8(lockedTemporary), "new"),
              "transiently locked result is retried");
        holder.join();
        Check(Read(lockedResult) == "new", "retried result replaces target");
        Check(!fs::exists(lockedTemporary), "retried result consumes temporary file");
    }
#endif

    fs::remove_all(base, error);
    return failures;
}
