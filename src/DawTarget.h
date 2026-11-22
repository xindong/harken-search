#pragma once

#include "ProToolsBridge.h"

enum class DawSendMode : int {
    Clipboard  = 0,
    LaunchExe  = 1,
    HttpApi    = 2,
    NamedPipe  = 3,
};

struct DawTarget {
    std::string name;
    std::string exePath;
    std::string processName;
    DawSendMode mode = DawSendMode::Clipboard;
    int httpPort = 8080;
    std::string reaperActionId;
    bool enabled = true;
    bool fallbackToClipboard = true;
    bool autoDetect = true;
    HarkenSearch::ProTools::SpotMode ptSpotMode = HarkenSearch::ProTools::SpotMode::AtCursor;
    HarkenSearch::ProTools::TrackFallback ptTrackFallback = HarkenSearch::ProTools::TrackFallback::AutoCreateNew;
    bool isRunning = false;
    double lastDetectTime = 0.0;
};
