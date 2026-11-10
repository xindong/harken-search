#ifdef __APPLE__
#include "settings_file.h"
#include "macos_file_io.h"

#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace Platform::FileSystem {
DurableFileOpResult SaveSettingsKeyValues(
    const std::string& pathUtf8,
    const std::vector<std::pair<std::string, std::string>>& items) {
    namespace io = MacFileIO;
    using Status = DurableFileOpStatus;
    const auto error = [](Status status) { return DurableFileOpResult{status, static_cast<uint32_t>(errno)}; };
    if (!io::ValidPath(pathUtf8)) return error(Status::InvalidArgument);
    std::string name;
    auto parent = io::OpenParent(pathUtf8, name);
    if (!parent) return error(Status::OpenFailed);
    if (!io::RegularOrMissing(parent.get(), name)) return error(Status::ReplaceFailed);
    std::string content = "# Aris settings (auto-generated)\n";
    for (const auto& item : items) {
        content += item.first;
        content += '=';
        content += item.second;
        content += '\n';
    }
    std::string temporary;
    io::Descriptor output;
    for (int attempt = 0; attempt < 8; ++attempt) {
        temporary = ".harken-settings-" + std::to_string(::arc4random()) + "-" + std::to_string(::arc4random()) + ".tmp";
        output = io::Descriptor(::openat(parent.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (output || errno != EEXIST) break;
    }
    if (!output) return error(Status::OpenFailed);
    // Failed temporary files are retained for diagnosis; never truncate the
    // previous settings and never turn a failed flush into a successful save.
    if (!io::WriteAll(output.get(), content.data(), content.size())) return error(Status::CopyFailed);
    if (!io::FlushFile(output.get()) || !output.close()) return error(Status::FlushFailed);
    if (!io::RegularOrMissing(parent.get(), name)) return error(Status::ReplaceFailed);
    if (::renameat(parent.get(), temporary.c_str(), parent.get(), name.c_str()) != 0) return error(Status::ReplaceFailed);
    if (::fsync(parent.get()) != 0) return error(Status::FlushFailed);
    return {Status::Success, 0};
}
}
#endif
