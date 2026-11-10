#include "reaper_plugin_macos.h"
#include "ReaperBridgeClient.h"
#include "ReaperBridgeProtocol.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef HARKEN_TEST_REAPER_DYLIB
#error HARKEN_TEST_REAPER_DYLIB is required
#endif

namespace fs = std::filesystem;
namespace Bridge = HarkenSearch::ReaperBridge;

static int RegisterFake(const char*, void*) { return 1; }
static void* GetFuncFake(const char*) { return nullptr; }

int main() {
    const char* root = std::getenv("HARKEN_MAC_TEST_ROOT");
    if (!root || root[0] != '/') {
        std::cerr << "HARKEN_MAC_TEST_ROOT must be an absolute QA directory\n";
        return 2;
    }
    std::string templatePath = (fs::path(root) / "host-XXXXXX").string();
    std::vector<char> temporary(templatePath.begin(), templatePath.end());
    temporary.push_back('\0');
    char* directory = mkdtemp(temporary.data());
    if (!directory) {
        std::cerr << "Cannot create isolated host directory: " << std::strerror(errno) << '\n';
        return 2;
    }
    const fs::path socketPath = fs::path(directory) / "bridge.sock";
    const std::string socketText = socketPath.string();
    const std::wstring socketName(socketText.begin(), socketText.end());
    int failures = 0;
    auto check = [&](bool condition, const char* label) {
        std::cout << (condition ? "PASS " : "FAIL ") << label << '\n';
        if (!condition) ++failures;
    };

    void* library = dlopen(HARKEN_TEST_REAPER_DYLIB, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::cerr << "Cannot load actual Mac extension: " << dlerror() << '\n';
        return 2;
    }
    auto entry = reinterpret_cast<int (*)(void*, reaper_plugin_info_t*)>(
        dlsym(library, "ReaperPluginEntry"));
    if (!entry) {
        std::cerr << "ReaperPluginEntry is not exported\n";
        dlclose(library);
        return 2;
    }
    reaper_plugin_info_t host{kReaperPluginVersion, nullptr,
                              RegisterFake, GetFuncFake};

    unsetenv("HARKEN_REAPER_BRIDGE_TEST_SOCKET");
    check(entry(nullptr, &host) == 0, "extension refuses missing QA endpoint");
    setenv("HARKEN_REAPER_BRIDGE_TEST_SOCKET", socketText.c_str(), 1);
    reaper_plugin_info_t incompatible = host;
    incompatible.caller_version = 0;
    check(entry(nullptr, &incompatible) == 0 && !fs::exists(socketPath),
          "extension refuses incompatible host ABI");

    const bool loaded = entry(nullptr, &host) == 1;
    check(loaded, "actual dylib loads with isolated fake REAPER ABI");
    struct stat endpoint{};
    check(loaded && lstat(socketText.c_str(), &endpoint) == 0 &&
              S_ISSOCK(endpoint.st_mode) && endpoint.st_uid == geteuid() &&
              (endpoint.st_mode & 0077) == 0,
          "endpoint is a private socket owned by this user");
    if (loaded) {
        const auto handshake = Bridge::ProbeHandshake(socketName, 1200);
        check(handshake.code == Bridge::HandshakeCode::ExtensionError &&
                  handshake.detail.find("ERR|UNAVAILABLE|") == 0,
              "existing Mac client sees host insertion is unavailable");
        const auto ping = Bridge::ExchangeMessage("PING\n", socketName, 1200);
        check(ping.ok && ping.response.find("ERR|UNAVAILABLE|") == 0,
              "PING does not claim a ready insert action");
        const auto wrongVersion = Bridge::ExchangeMessage(
            "HELLO|999|" + Bridge::CurrentArchitecture() + "|" +
                Bridge::kBridgeManifest + "\n", socketName, 1200);
        check(wrongVersion.ok && wrongVersion.response.find("ERR|PROTOCOL|") == 0,
              "wrong protocol version is rejected");
        const auto wrongArchitecture = Bridge::ExchangeMessage(
            "HELLO|" + std::to_string(Bridge::kProtocolVersion) +
                "|wrong-arch|" + Bridge::kBridgeManifest + "\n", socketName, 1200);
        check(wrongArchitecture.ok &&
                  wrongArchitecture.response.find("ERR|ARCH|") == 0,
              "wrong architecture is rejected");
        const auto wrongManifest = Bridge::ExchangeMessage(
            "HELLO|" + std::to_string(Bridge::kProtocolVersion) +
                "|" + Bridge::CurrentArchitecture() +
                "|wrong-manifest\n", socketName, 1200);
        check(wrongManifest.ok &&
                  wrongManifest.response.find("ERR|MANIFEST|") == 0,
              "wrong manifest is rejected");
        const auto send = Bridge::SendFiles({"/tmp/qa-only.wav"}, {}, socketName, 1200);
        check(!send.ok && send.sent == 0 && send.total == 1 &&
                  send.detail.find("INSERT|") == 0,
              "unimplemented insertion reports failure without false success");
        std::string request;
        std::string buildError;
        const bool encoded = Bridge::BuildInsertBatchRequest(
            {"/tmp/qa-only.wav"}, request, buildError);
        const auto rawInsert = encoded
            ? Bridge::ExchangeMessage(request, socketName, 1200)
            : Bridge::PipeExchangeResult{};
        check(encoded && rawInsert.ok &&
                  rawInsert.response.find("ERR|INSERT|") == 0,
              "wire response explicitly rejects unsupported insertion");
        check(entry(nullptr, nullptr) == 0 && !fs::exists(socketPath),
              "unload stops server and removes only its socket");
        const auto after = Bridge::ProbeHandshake(socketName, 200);
        check(after.code == Bridge::HandshakeCode::TransportUnavailable,
              "client cannot reconnect after unload");
    }

    {
        std::ofstream occupied(socketPath);
        occupied << "preserve this file";
    }
    check(entry(nullptr, &host) == 0 && fs::is_regular_file(socketPath),
          "extension refuses occupied non-socket path without replacing it");
    fs::remove(socketPath);
    const fs::path unsafeDirectory = fs::path(directory) / "unsafe";
    fs::create_directory(unsafeDirectory);
    chmod(unsafeDirectory.c_str(), 0777);
    const fs::path unsafeSocket = unsafeDirectory / "bridge.sock";
    setenv("HARKEN_REAPER_BRIDGE_TEST_SOCKET", unsafeSocket.c_str(), 1);
    check(entry(nullptr, &host) == 0 && !fs::exists(unsafeSocket),
          "extension refuses a group or world writable parent");
    chmod(unsafeDirectory.c_str(), 0700);
    unsetenv("HARKEN_REAPER_BRIDGE_TEST_SOCKET");
    dlclose(library);
    std::cout << "Mac REAPER host handshake failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
