#ifdef __APPLE__

#include "durable_file_ops.h"

namespace Platform::FileSystem {

DurableFileOpResult FlushExistingFileToDisk(const std::string&) {
    DurableFileOpResult result;
    result.status = DurableFileOpStatus::Unsupported;
    result.systemError = 0;
    return result;
}

DurableFileOpResult CopyFileExclusiveAndFlush(const std::string&, const std::string&) {
    DurableFileOpResult result;
    result.status = DurableFileOpStatus::Unsupported;
    result.systemError = 0;
    return result;
}

DurableFileOpResult ReplaceFileWriteThrough(const std::string&, const std::string&, bool) {
    DurableFileOpResult result;
    result.status = DurableFileOpStatus::Unsupported;
    result.systemError = 0;
    return result;
}

DurableFileOpResult DeleteFileIfPresent(const std::string&) {
    DurableFileOpResult result;
    result.status = DurableFileOpStatus::Unsupported;
    result.systemError = 0;
    return result;
}

} // namespace Platform::FileSystem

#endif // __APPLE__
