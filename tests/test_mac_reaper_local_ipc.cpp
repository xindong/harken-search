#ifdef __APPLE__

#include "ReaperBridgeClient.h"
#include "platform/reaper_bridge_endpoint_macos.h"
#include "platform/named_pipe.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
int failures = 0;

void Check(bool pass, const char* label) {
    std::cout << (pass ? "PASS " : "FAIL ") << label << '\n';
    if (!pass) ++failures;
}

std::wstring WidePath(const std::string& path) {
    // Fixture paths are ASCII; protocol payloads below include UTF-8 paths.
    return {path.begin(), path.end()};
}

struct FakeHost {
    int listener = -1;
    std::string path;
    std::string response;
    std::string received;
    int responseDelayMs = 0;
    std::thread worker;

    FakeHost(std::string endpoint, std::string reply, int delayMs = 0)
        : path(std::move(endpoint)), response(std::move(reply)), responseDelayMs(delayMs) {
        listener = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener < 0) return;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) return;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            chmod(path.c_str(), 0600) != 0 || listen(listener, 1) != 0) return;
        worker = std::thread([this] {
            pollfd ready{listener, POLLIN, 0};
            if (poll(&ready, 1, 600) <= 0) return;
            const int peer = accept(listener, nullptr, nullptr);
            if (peer < 0) return;
            const int noSigpipe = 1;
            setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
            char buffer[4096];
            for (;;) {
                const ssize_t bytes = recv(peer, buffer, sizeof(buffer), 0);
                if (bytes <= 0) break;
                received.append(buffer, static_cast<std::size_t>(bytes));
                if (received.find('\n') != std::string::npos) break;
            }
            if (responseDelayMs) std::this_thread::sleep_for(std::chrono::milliseconds(responseDelayMs));
            if (!response.empty()) send(peer, response.data(), response.size(), 0);
            close(peer);
        });
    }

    ~FakeHost() {
        if (worker.joinable()) worker.join();
        if (listener >= 0) close(listener);
        // Socket files stay inside this unique isolated QA fixture as evidence.
    }

    void Join() {
        if (worker.joinable()) worker.join();
    }
};
} // namespace

int main() {
    const char* root = std::getenv("HARKEN_MAC_TEST_ROOT");
    if (!root || root[0] != '/') {
        std::cerr << "HARKEN_MAC_TEST_ROOT must be an absolute isolated QA directory\n";
        return 2;
    }
    fs::create_directories(root);
    const std::string templatePath = (fs::path(root) / "r-XXXXXX").string();
    std::vector<char> templateBytes(templatePath.begin(), templatePath.end());
    templateBytes.push_back('\0');
    const char* uniqueDir = mkdtemp(templateBytes.data());
    if (!uniqueDir) return 2;
    const fs::path testDir(uniqueDir);

    const std::string privateSocket =
        HarkenSearch::ReaperBridge::MacSocketPathInPrivateDirectory(testDir.string());
    Check(privateSocket == (testDir / "harken-reaper.sock").string(),
          "private QA directory produces a stable short socket path");
    Check(!HarkenSearch::ReaperBridge::MacDefaultSocketPath().empty(),
          "system user temp directory provides a read-only default endpoint");
    Check(HarkenSearch::ReaperBridge::MacSocketPathInPrivateDirectory("/tmp").empty(),
          "world-writable directory cannot host the default endpoint");
    const fs::path linkDir = testDir / "linked-parent";
    symlink(testDir.c_str(), linkDir.c_str());
    Check(HarkenSearch::ReaperBridge::MacSocketPathInPrivateDirectory(linkDir.string()).empty(),
          "symlink parent is rejected");
    const fs::path longDir = testDir / std::string(90, 'x');
    fs::create_directory(longDir);
    Check(HarkenSearch::ReaperBridge::MacSocketPathInPrivateDirectory(longDir.string()).empty(),
          "overlong Unix socket endpoint is rejected");

    {
        const std::string path = (testDir / "hello.sock").string();
        FakeHost host(path, HarkenSearch::ReaperBridge::BuildHelloResponse(41));
        const auto result = HarkenSearch::ReaperBridge::ProbeHandshake(WidePath(path), 300);
        Check(result.code == HarkenSearch::ReaperBridge::HandshakeCode::Ready &&
                  result.actionId == 41, "isolated Unix socket HELLO handshake");
    }
    {
        const std::string path = (testDir / "batch.sock").string();
        FakeHost host(path, HarkenSearch::ReaperBridge::BuildInsertBatchSuccessResponse(2, 4.0, 7.0));
        const std::vector<std::string> files{u8"/隔离素材/第一条 | hit.wav", u8"/隔离素材/第二条.wav"};
        const auto result = HarkenSearch::ReaperBridge::SendFiles(files, {}, WidePath(path), 300);
        Check(result.ok && result.sent == 2 && result.total == 2,
              "batch response parsed without claiming real host insertion");
        host.Join();
        std::vector<std::string> decoded;
        std::string error;
        Check(HarkenSearch::ReaperBridge::ParseInsertBatchRequest(host.received, decoded, error) &&
                  decoded == files, "UTF-8 batch payload reaches isolated host intact");
    }
    {
        const std::string path = (testDir / "default-route.sock").string();
        FakeHost host(path, HarkenSearch::ReaperBridge::BuildHelloResponse(41));
        setenv("HARKEN_REAPER_BRIDGE_TEST_SOCKET", path.c_str(), 1);
        Check(HarkenSearch::ReaperBridge::MacSelectedSocketPath() == path,
              "explicit QA override selects only its isolated socket");
        const auto result = HarkenSearch::ReaperBridge::ProbeHandshake();
        Check(result.code == HarkenSearch::ReaperBridge::HandshakeCode::Ready,
              "default client call reaches isolated Mac host endpoint");
        setenv("HARKEN_REAPER_BRIDGE_TEST_SOCKET", "", 1);
        Check(HarkenSearch::ReaperBridge::MacSelectedSocketPath().empty(),
              "empty QA override fails closed instead of reaching live user socket");
        unsetenv("HARKEN_REAPER_BRIDGE_TEST_SOCKET");
    }
    {
        const std::string path = (testDir / "missing.sock").string();
        const auto result = Platform::NamedPipe::ExchangeMessage(WidePath(path), "HELLO\n", 50);
        Check(result.status == Platform::NamedPipe::MessageExchangeStatus::ConnectFailed,
              "missing host has a typed connection failure");
    }
    {
        const std::string path = (testDir / "slow.sock").string();
        FakeHost host(path, "OK\n", 180);
        const auto result = Platform::NamedPipe::ExchangeMessage(WidePath(path), "HELLO\n", 40);
        Check(result.status == Platform::NamedPipe::MessageExchangeStatus::TimedOut,
              "silent host respects the exchange deadline");
    }
    {
        const std::string path = (testDir / "unsafe.sock").string();
        FakeHost host(path, "OK\n");
        chmod(path.c_str(), 0666);
        const auto result = Platform::NamedPipe::ExchangeMessage(WidePath(path), "HELLO\n", 50);
        Check(result.status == Platform::NamedPipe::MessageExchangeStatus::ConnectFailed,
              "world-writable socket is rejected before connection");
    }
    {
        const std::string path = (testDir / "regular.sock").string();
        std::ofstream(path) << "not a socket";
        const auto result = Platform::NamedPipe::ExchangeMessage(WidePath(path), "HELLO\n", 50);
        Check(result.status == Platform::NamedPipe::MessageExchangeStatus::ConnectFailed,
              "ordinary file is rejected before connection");
    }
    {
        const std::string target = (testDir / "target.sock").string();
        FakeHost host(target, "OK\n");
        const std::string link = (testDir / "link.sock").string();
        symlink(target.c_str(), link.c_str());
        const auto result = Platform::NamedPipe::ExchangeMessage(WidePath(link), "HELLO\n", 50);
        Check(result.status == Platform::NamedPipe::MessageExchangeStatus::ConnectFailed,
              "socket symlink is rejected before connection");
    }
    {
        const std::string path = (testDir / "invalid.sock").string();
        FakeHost host(path, "OK|HELLO|bad\n");
        const auto result = HarkenSearch::ReaperBridge::ProbeHandshake(WidePath(path), 300);
        Check(result.code != HarkenSearch::ReaperBridge::HandshakeCode::Ready,
              "malformed host handshake is not reported as ready");
    }
    std::cout << "Mac REAPER local IPC: " << (failures == 0 ? "PASS" : "FAIL") << '\n';
    return failures == 0 ? 0 : 1;
}

#endif // __APPLE__
