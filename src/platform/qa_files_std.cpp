#include "qa_files.h"

#include "filesystem.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace Platform::QaFiles {
namespace {
#ifdef ARIS_QA_FILES_TESTING
bool g_removeBeforeRetirement = false;
#endif

void RemoveTemporary(const std::string& pathUtf8) {
    std::error_code error;
    std::filesystem::remove(std::filesystem::u8path(pathUtf8), error);
}

} // namespace

bool ReadCommandJson(const std::string& pathUtf8,
                     std::string& outJson,
                     size_t maxBytes) {
    outJson.clear();
    const std::vector<uint8_t> bytes =
        Platform::FileSystem::ReadFileBytes(pathUtf8, maxBytes);
    if (bytes.empty()) return false;

    std::error_code error;
#ifdef ARIS_QA_FILES_TESTING
    if (g_removeBeforeRetirement) {
        std::filesystem::remove(std::filesystem::u8path(pathUtf8), error);
        error.clear();
        g_removeBeforeRetirement = false;
    }
#endif
    const bool removed = std::filesystem::remove(std::filesystem::u8path(pathUtf8), error);
    if (error || !removed) return false;
    outJson.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

bool WriteResultAtomically(const std::string& resultPathUtf8,
                           const std::string& temporaryPathUtf8,
                           std::string_view json) {
    Platform::FileSystem::StreamWriter writer;
    if (!writer.Open(temporaryPathUtf8, false, true)) return false;
    const bool written = writer.Write(json) && writer.Flush();
    writer.Close();
    if (!written) {
        RemoveTemporary(temporaryPathUtf8);
        return false;
    }
    // The QA driver polls the result file while it runs. A poll that opens the
    // destination at the same moment as MoveFileEx can make the replace fail
    // even though the command itself succeeded. Retry briefly so a transient
    // reader does not discard the result and stall the driver until timeout.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (Platform::FileSystem::AtomicFileReplace(resultPathUtf8, temporaryPathUtf8))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    RemoveTemporary(temporaryPathUtf8);
    return false;
}

#ifdef ARIS_QA_FILES_TESTING
void Testing::RemoveBeforeRetirementOnce() { g_removeBeforeRetirement = true; }
#endif

} // namespace Platform::QaFiles
