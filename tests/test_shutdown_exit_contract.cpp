#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef AURA_SOURCE_DIR
#define AURA_SOURCE_DIR "."
#endif

namespace {
std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "Failed to open " << path.string() << "\n";
        std::exit(2);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void RequireContains(const std::string& text, const std::string& needle, const char* label) {
    if (text.find(needle) == std::string::npos) {
        std::cerr << "Missing " << label << ": " << needle << "\n";
        std::exit(1);
    }
}

std::string SliceAfter(const std::string& text, const std::string& marker, size_t length) {
    const size_t pos = text.find(marker);
    if (pos == std::string::npos) return {};
    return text.substr(pos, std::min(length, text.size() - pos));
}

std::string TrimLeft(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return std::string(line.substr(i));
}

bool ConditionMentions(const std::string& condition, const char* token) {
    return condition.find(token) != std::string::npos;
}

void RequireCleanupDrainUnconditional(const std::string& windowsMain) {
    const std::string cleanupMarker = "    // Cleanup";
    const size_t cleanupPos = windowsMain.find(cleanupMarker);
    if (cleanupPos == std::string::npos) {
        std::cerr << "Missing Cleanup marker in WindowsMain.cpp\n";
        std::exit(1);
    }

    const char* requiredCalls[] = {
        "state.requestCancelScanTask();",
        "state.waveformWorker.stop();",
        "state.searchWorker.stop();",
        "aura::GlobalBackfillWorkerPool().stop();",
    };
    bool found[4] = {};

    std::vector<std::string> stack;
    size_t lineStart = 0;
    bool inCleanup = false;
    int lineNo = 1;
    auto failWrapped = [&](const char* call, const std::string& cond) {
        std::cerr << "Cleanup drain is wrapped by preprocessor condition at line " << lineNo
                  << ": " << call << " inside " << cond << "\n";
        std::exit(1);
    };

    while (lineStart < windowsMain.size()) {
        size_t lineEnd = windowsMain.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = windowsMain.size();
        const std::string line = windowsMain.substr(lineStart, lineEnd - lineStart);
        if (lineStart <= cleanupPos && cleanupPos < lineEnd) {
            inCleanup = true;
        }

        const std::string trimmed = TrimLeft(line);
        if (trimmed.rfind("#if", 0) == 0 || trimmed.rfind("#ifdef", 0) == 0 ||
            trimmed.rfind("#ifndef", 0) == 0) {
            stack.push_back(trimmed);
        } else if (trimmed.rfind("#elif", 0) == 0) {
            if (!stack.empty()) stack.back() = trimmed;
        } else if (trimmed.rfind("#else", 0) == 0) {
            if (!stack.empty()) stack.back() += " | #else";
        } else if (trimmed.rfind("#endif", 0) == 0) {
            if (!stack.empty()) stack.pop_back();
        }

        if (inCleanup) {
            for (int i = 0; i < 4; ++i) {
                if (line.find(requiredCalls[i]) == std::string::npos) continue;
                found[i] = true;
                for (const auto& cond : stack) {
                    if (ConditionMentions(cond, "AURA_BUILD_DIAGNOSTICS") ||
                        ConditionMentions(cond, "AURA_INTERNAL_BETA") ||
                        ConditionMentions(cond, "SM_HAS_WEBVIEW2")) {
                        failWrapped(requiredCalls[i], cond);
                    }
                }
            }
        }

        if (lineEnd == windowsMain.size()) break;
        lineStart = lineEnd + 1;
        ++lineNo;
    }

    for (int i = 0; i < 4; ++i) {
        if (!found[i]) {
            std::cerr << "Missing unconditional Cleanup drain: " << requiredCalls[i] << "\n";
            std::exit(1);
        }
    }
}

void RequireWaveformStopWakesIdleWorkers(const std::string& asyncWorker) {
    const std::string stop = SliceAfter(asyncWorker, "void WaveformWorker::stop()", 1000);
    const size_t lock = stop.find("std::lock_guard<std::mutex> lk(m_requestMutex);");
    const size_t publish = stop.find("m_running.store(false");
    const size_t unlock = stop.find("\n    }", lock);
    const size_t notify = stop.find("m_requestCv.notify_all();");
    const size_t join = stop.find("t.join()");
    if (!(lock < publish && publish < unlock && unlock < notify &&
          notify < join && join != std::string::npos)) {
        std::cerr << "Waveform shutdown must publish stop under the request mutex "
                     "before notifying and joining the idle workers\n";
        std::exit(1);
    }
}
} // namespace

int main() {
    const std::filesystem::path root = AURA_SOURCE_DIR;
    const std::string windowsMain = ReadText(root / "src" / "WindowsMain.cpp");
    const std::string windowsHeader = ReadText(root / "src" / "WindowsMain.h");
    const std::string mainCpp = ReadText(root / "src" / "main.cpp");

    RequireContains(windowsHeader,
                    "void RequestHarkenSearchExit(HWND hwnd, const wchar_t* reason);",
                    "exit helper declaration");
    RequireContains(windowsMain,
                    "std::atomic<bool> g_explicitExitRequested",
                    "explicit exit request state");
    RequireContains(windowsMain,
                    "void RequestHarkenSearchExit(HWND hwnd, const wchar_t* reason)",
                    "exit helper definition");

    const std::string helper = SliceAfter(windowsMain, "void RequestHarkenSearchExit(HWND hwnd, const wchar_t* reason)", 2200);
    RequireContains(helper, "g_explicitExitRequested.store(true", "exit helper marks explicit exit");
    RequireContains(helper, "kExitSettingsSaveAttempts", "exit helper bounds settings save retries");
    RequireContains(helper, "TrySaveAppSettings(*g_appState)", "exit helper attempts settings save before shutdown");
    RequireContains(helper, "RemoveTrayIconIfAny(hwnd);", "exit helper removes tray icon");
    RequireContains(helper, "DestroyWindow(hwnd);", "exit helper destroys window");
    RequireContains(helper, "PostQuitMessage(0);", "exit helper posts quit message");
    RequireContains(helper, "DIAG_SYSTEM(\"Shutdown\", \"ExitRequested\")", "exit helper diagnostic marker");

    const std::string trayBlock = SliceAfter(windowsMain, "if (cmd == TRAY_CMD_EXIT)", 240);
    RequireContains(trayBlock, "RequestHarkenSearchExit(hwnd, L\"tray-exit\")", "tray Exit uses unified exit helper");

    const std::string closeBlock = SliceAfter(windowsMain, "case WM_CLOSE:", 1500);
    RequireContains(closeBlock, "g_explicitExitRequested.load", "WM_CLOSE respects explicit exit flag");
    RequireContains(closeBlock, "RequestHarkenSearchExit(hwnd, L\"close-button-direct\")", "direct close action exits through helper");
    RequireContains(closeBlock, "MinimizeWindowNoAnimation(hwnd);", "minimize-to-tray preference remains available for close button only");

    RequireContains(mainCpp, "RequestHarkenSearchExit(g_hwnd, L\"close-choice-exit\")", "close-choice Exit uses helper");
    RequireContains(mainCpp, "RequestHarkenSearchExit(g_hwnd, L\"file-menu-exit\")", "File Exit uses helper");

    RequireContains(windowsMain, "auto logShutdownStage = [](const char* stage)", "shutdown stage logger");
    RequireContains(windowsMain, "logShutdownStage(\"Begin\")", "shutdown begin marker");
    RequireContains(windowsMain, "logShutdownStage(\"DawSendThreadJoinBegin\")", "DAW join begin marker");
    RequireContains(windowsMain, "logShutdownStage(\"WaveformWorkerStopBegin\")", "waveform worker stop begin marker");
    RequireContains(windowsMain, "logShutdownStage(\"DatabaseCloseBegin\")", "database close begin marker");
    RequireContains(windowsMain, "logShutdownStage(\"AudioEngineShutdownBegin\")", "audio shutdown begin marker");
    RequireContains(windowsMain, "logShutdownStage(\"Complete\")", "shutdown complete marker");

    RequireCleanupDrainUnconditional(windowsMain);
    RequireWaveformStopWakesIdleWorkers(ReadText(root / "src" / "AsyncWorker.cpp"));

    std::cout << "Shutdown exit contract passed\n";
    return 0;
}
