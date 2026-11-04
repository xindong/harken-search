#pragma once

#include "durable_file_ops.h"
#include <string>
#include <utility>
#include <vector>

namespace Platform::FileSystem {
// @stage7-contract
// macOS settings commit. Uses a private same-directory temporary file. Errors
// before replacement leave the previous settings intact; a flush error after
// replacement means the new file is visible but durability is not confirmed.
DurableFileOpResult SaveSettingsKeyValues(
    const std::string& pathUtf8,
    const std::vector<std::pair<std::string, std::string>>& items);
}
