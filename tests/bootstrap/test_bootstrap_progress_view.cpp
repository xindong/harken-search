#include "bootstrap/BootstrapProgressView.h"
#include "nlohmann_json.hpp"
#include <iostream>
#include <stdexcept>

using namespace aris::bootstrap;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static nlohmann::json view(BootstrapProgressPresentation state) {
    return nlohmann::json::parse(BootstrapProgressJson(state));
}
int main() {
    try {
        BootstrapProgressPresentation state;
        state.snapshot = {BootstrapProgressPhase::WaitingForExit};
        state.targetVersion = "1.0.42";
        auto json = view(state);
        require(json["phase"] == "WaitingForExit" && json["busy"] == true, "real waiting phase");
        require(json["version"] == "1.0.42", "verified version");
        require(json["progress"] == nullptr, "unknown progress must not invent a percentage");
        state.snapshot = {BootstrapProgressPhase::ReplacingFiles, 3, 12};
        json = view(state);
        require(json["completedFiles"] == 3 && json["totalFiles"] == 12 && json["progress"] == 0.25,
                "use actual completed file count");
        state.snapshot = {BootstrapProgressPhase::CheckingStartup};
        require(view(state)["progress"] == nullptr && view(state)["busy"] == true,
                "all files copied is not update success");
        state.snapshot = {BootstrapProgressPhase::Completed};
        require(view(state)["mode"] != "complete", "only entrypoint result confirms completion");
        state.snapshot = {BootstrapProgressPhase::Failed};
        state.lastActive = BootstrapProgressPhase::CheckingStartup;
        json = view(state);
        require(json["mode"] == "working" && json["busy"] == true && json["phase"] == "CheckingStartup",
                "runtime failure observation cannot finish the entrypoint UI");
        state.finished = true;
        require(view(state)["mode"] == "complete" && view(state)["busy"] == false,
                "actual successful result is terminal");
        state.exitCode = 21;
        state.lastActive = BootstrapProgressPhase::WaitingForExit;
        json = view(state);
        require(json["mode"] == "failed" && json["busy"] == false && json["progress"] == nullptr,
                "failure stops animation");
        require(json["title"] == u8"程序尚未退出" && json["exitCode"] == 21,
                "exit timeout must remain actionable");
        state.exitCode = 50;
        state.lastActive = BootstrapProgressPhase::CheckingStartup;
        json = view(state);
        require(json["hint"].get<std::string>().find(u8"未被改动") == std::string::npos,
                "startup failure must not claim unchanged files");
        state.exitCode = 83;
        require(view(state)["hint"].get<std::string>().find(u8"重新安装") != std::string::npos,
                "forward-only failed health gives honest repair advice");
        std::cout << "progress presentation: passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
