#pragma once

#include <string>

namespace Platform::SettingsDialogs {

// Runs on the main UI thread. The returned UTF-8 path is owned by the caller;
// an empty path means the user cancelled or the panel could not be shown.
std::string SaveSettingsExportFileDialogUtf8(void* ownerWindow);

} // namespace Platform::SettingsDialogs
