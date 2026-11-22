#pragma once

#include "DawBridgeInstaller.h"

namespace HarkenSearch::DawBridge {

inline bool NeedsReaperSetupNotice(const InstallStatus& status) {
    return status.hostInstalled && status.targetResolved && !status.filePresent &&
           !status.alternateFilePresent && !status.legacyFilePresent && !status.bridgeLoaded;
}

inline bool CanInstallMissingReaperBridge(const InstallStatus& status) {
    return NeedsReaperSetupNotice(status) && status.bundledFilePresent &&
           !status.hostRunning && status.state == InstallState::NotInstalled;
}

}
