#pragma once

// @stage7-contract
// Isolated diagnostic builds only: the UI thread consumes one bounded command
// from the compiled QA root. It uses product actions and reports real state.
// No external window input, arbitrary SQL, or non-QA path access is supported.
#include <string>
#include <vector>
struct AppState;
namespace HarkenSearch::MacQa {
struct Request {
    long long id = 0;
    bool capture = false;
    bool close = false;
    bool dropAttempted = false;
    bool dropAccepted = false;
    std::vector<std::string> dropPaths;
    std::string error;
};
Request Poll(AppState& state);
bool Reply(const AppState& state, const Request& request, bool captured);
bool ConsumeThesaurusPageRequest();
const std::string& BrowserTreeRootForQa();
#if defined(__APPLE__) && AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
// Read on the UI thread after rendering; no texture IDs or mutations escape.
std::vector<std::string> MiniTexturePaths();
#endif
}
