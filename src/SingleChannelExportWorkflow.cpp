#include "SingleChannelExportWorkflow.h"

#include "AudioEngine.h"
#include "TransferEngine.h"
#include "platform/filesystem.h"

#include <filesystem>

namespace aura::audio_export {
namespace {

std::filesystem::path PathFromUtf8(const std::string& value) {
    return std::filesystem::u8path(value);
}

std::string PathToUtf8(const std::filesystem::path& value) {
    return value.u8string();
}

}  // namespace

SingleChannelExportResult RunSingleChannelExport(
    SingleChannelExportRequest request) {
    SingleChannelExportResult result;
    if (request.sourcePath.empty()) {
        result.errorMessage = "Source path is empty";
        return result;
    }
    if (request.destinationDirectory.empty()) {
        result.errorMessage = "Export folder is empty";
        return result;
    }

    const std::filesystem::path destinationRoot =
        PathFromUtf8(request.destinationDirectory);
    if (!Platform::FileSystem::EnsureDirectoryExists(
            request.destinationDirectory)) {
        result.errorMessage = "Cannot create export folder";
        return result;
    }
    if (!Platform::FileSystem::IsDirectory(request.destinationDirectory)) {
        result.errorMessage = "Export target is not a folder";
        return result;
    }
    if (request.sourceFileName.empty()) {
        request.sourceFileName =
            PathToUtf8(PathFromUtf8(request.sourcePath).filename());
    }

    const TransferPrefs prefs = TransferEngine::getDefaultPrefs();
    const std::string fileName = TransferEngine::buildSingleChannelExportFileName(
        request.sourceFileName,
        request.channelIndex,
        request.totalChannels,
        prefs);
    const std::filesystem::path requestedPath =
        destinationRoot / PathFromUtf8(fileName);
    const TransferDestinationResolution resolved =
        TransferEngine::resolveExistingDestinationPath(
            PathToUtf8(requestedPath),
            ExistingFileBehavior::AlwaysCreateNew);
    if (!resolved.errorMessage.empty()) {
        result.errorMessage = resolved.errorMessage;
        return result;
    }

    std::string exportError;
    result.outputPath = resolved.path;
    result.ok = AudioEngine::exportSingleChannel(
        request.sourcePath,
        request.channelIndex,
        result.outputPath,
        &exportError,
        request.progress ? &request.progress : nullptr);
    if (!result.ok) {
        result.errorMessage =
            exportError.empty() ? "Single-channel export failed" : exportError;
    }
    return result;
}

}  // namespace aura::audio_export
