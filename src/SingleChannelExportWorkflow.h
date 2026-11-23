#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace aura::audio_export {

struct SingleChannelExportRequest {
    std::string sourcePath;
    std::string sourceFileName;
    std::string destinationDirectory;
    int channelIndex = 0;
    int totalChannels = 0;
    // 可选：写样本进度（已写帧 / 总帧），在导出线程内回调。
    std::function<void(std::uint64_t, std::uint64_t)> progress;
};

struct SingleChannelExportResult {
    bool ok = false;
    std::string outputPath;
    std::string errorMessage;
};

SingleChannelExportResult RunSingleChannelExport(
    SingleChannelExportRequest request);

}  // namespace aura::audio_export
