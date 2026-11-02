#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "nlohmann_json.hpp"

#include <cstdint>
#include <cwchar>
#include <iostream>
#include <string>
#include <string_view>

int wmain(const int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--old") {
        const auto value = std::wcstoull(argv[2], nullptr, 10);
        const HANDLE exitEvent = reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(value));
        if (exitEvent == nullptr || exitEvent == INVALID_HANDLE_VALUE) return 25;
        const DWORD waited = WaitForSingleObject(exitEvent, 90'000U);
        CloseHandle(exitEvent);
        return waited == WAIT_OBJECT_0 ? 0 : 26;
    }
    if (argc != 1) return 27;
    // The Bootstrap adapter supplies the challenge through inherited stdin;
    // no nonce or capability appears in argv or the process environment.
    std::string challenge;
    char buffer[4096]{};
    std::cin.read(buffer, sizeof(buffer) - 1);
    const std::streamsize count = std::cin.gcount();
    if (count <= 0) return 20;
    challenge.assign(buffer, static_cast<std::size_t>(count));
    const auto value = nlohmann::json::parse(challenge, nullptr, false);
    if (value.is_discarded() || !value.is_object() || value.size() != 3U ||
        !value.contains("build_id") || !value.contains("version") ||
        !value.contains("token") || !value["build_id"].is_string() ||
        !value["version"].is_string() || !value["token"].is_string()) {
        return 21;
    }
    const bool sendMismatchedAcknowledgement =
        value["build_id"].get<std::string>() == "build-health-bad";
    if (value["build_id"].get<std::string>() == "build-health-crash") {
        return 23;
    }
    const bool fragmentAcknowledgement =
        value["build_id"].get<std::string>() == "build-health-fragmented";
    nlohmann::json acknowledgement = {
        {"build_id", value["build_id"]},
        {"process_id", static_cast<std::uint64_t>(GetCurrentProcessId())},
        {"token", sendMismatchedAcknowledgement
                       ? nlohmann::json(std::string(64U, 'x'))
                       : value["token"]},
        {"ui_ready", !sendMismatchedAcknowledgement},
        {"version", value["version"]},
    };
    const std::string bytes = acknowledgement.dump() + "\n";
    if (value["build_id"] == "progress-real-update") Sleep(1800U);
    DWORD written = 0;
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (fragmentAcknowledgement) {
        const DWORD firstSize = static_cast<DWORD>(bytes.size() / 2U);
        if (!WriteFile(output, bytes.data(), firstSize, &written, nullptr) ||
            written != firstSize) {
            return 22;
        }
        Sleep(100U);
        const DWORD remaining = static_cast<DWORD>(bytes.size()) - firstSize;
        if (!WriteFile(output, bytes.data() + firstSize, remaining, &written,
                       nullptr) || written != remaining) {
            return 22;
        }
    } else if (!WriteFile(output, bytes.data(),
                          static_cast<DWORD>(bytes.size()), &written, nullptr) ||
               written != bytes.size()) {
        return 22;
    }
    // Keep a successfully acknowledged child alive through Bootstrap's
    // durable commit/ownership hand-off. The inherited challenge pipe is the
    // one-shot parent capability; its closure is the deterministic handoff
    // boundary, with a bounded fallback so a broken test cannot hang cleanup.
    if (!sendMismatchedAcknowledgement) {
        // The parent closes its read end immediately after consuming the
        // acknowledgement. A delayed heartbeat observes that closure without
        // adding bytes to the acknowledgement frame itself.
        Sleep(100U);
        for (unsigned int attempt = 0U; attempt < 200U; ++attempt) {
            if (output == nullptr || output == INVALID_HANDLE_VALUE) break;
            const char heartbeat = '\n';
            DWORD heartbeatWritten = 0U;
            if (!WriteFile(output, &heartbeat, 1U, &heartbeatWritten, nullptr) ||
                heartbeatWritten != 1U) {
                break;
            }
            Sleep(25U);
        }
    }
    return 0;
}
