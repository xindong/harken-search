#include "SelectedRecordRescanWorkflow.h"

namespace aura::record_rescan {
namespace {

bool IsMp3(const std::string& path) {
    if (path.size() < 4) return false;
    const auto offset = path.size() - 4;
    return path[offset] == '.' &&
           (path[offset + 1] == 'm' || path[offset + 1] == 'M') &&
           (path[offset + 2] == 'p' || path[offset + 2] == 'P') &&
           path[offset + 3] == '3';
}

void ApplyMetadata(
    SelectedRecordRescanUpdate& update,
    const SelectedRecordRescanMetadata& metadata,
    bool preserveExisting) {
    const auto apply = [preserveExisting](std::string& stored, const std::string& embedded) {
        if ((!preserveExisting || stored.empty()) && !embedded.empty()) stored = embedded;
    };
    apply(update.title, metadata.title);
    apply(update.artist, metadata.artist);
    apply(update.album, metadata.album);
    apply(update.description, metadata.description);
    apply(update.category, metadata.category.empty() ? metadata.genre : metadata.category);
    apply(update.keywords, metadata.keywords.empty() ? metadata.comment : metadata.keywords);
    for (const auto& [key, value] : metadata.extendedMetadata) {
        if (!value.empty()) apply(update.extendedMetadata[key], value);
    }
    if (preserveExisting && metadata.id3TextTags) update.rereadId3TextTags = metadata.id3TextTags;
}

}  // namespace

SelectedRecordRescanItemResult RescanSelectedRecord(
    const SelectedRecordRescanItem& item,
    SelectedRecordRescanOperationPort& port) {
    const auto file = port.observeFile(item.filePath);
    if (!file.regular || !file.sizeAvailable) {
        return SelectedRecordRescanItemResult::NotUpdated;
    }

    SelectedRecordRescanUpdate update;
    update.title = item.title;
    update.artist = item.artist;
    update.album = item.album;
    update.description = item.description;
    update.category = item.category;
    update.keywords = item.keywords;
    update.extendedMetadata = item.extendedMetadata;
    update.sampleRate = item.sampleRate;
    update.channels = item.channels;
    update.totalFrames = item.totalFrames;
    update.durationSeconds = item.durationSeconds;

    SelectedRecordRescanMetadata metadata;
    if (port.readMetadata(item.filePath, metadata)) {
        ApplyMetadata(update, metadata, IsMp3(item.filePath));
    }

    const bool oldAudioInfoMissing =
        item.sampleRate == 0 || item.channels == 0 || item.totalFrames == 0 ||
        item.durationSeconds <= 0.0f;
    const bool oldWaveformWasDecoderFailure =
        port.wasDecoderInitFailure(item.filePath);

    SelectedRecordRescanAudioFacts audio{
        item.sampleRate,
        item.channels,
        item.totalFrames,
        item.durationSeconds,
    };
    const bool audioInfoOk = port.probeAudio(item.filePath, audio);
    update.sampleRate = audio.sampleRate;
    update.channels = audio.channels;
    update.totalFrames = audio.totalFrames;
    update.durationSeconds = audio.durationSeconds;
    update.fileSize = file.size;
    update.fileMtime = port.lastWriteTimeUnixSeconds(item.filePath);
    update.lastIndexed = port.nowUnixSeconds();

    const bool updated = port.updateRecord(item, update);
    if (updated && audioInfoOk &&
        (oldAudioInfoMissing || oldWaveformWasDecoderFailure)) {
        port.clearWaveformCache(item.filePath);
    }
    return updated ? SelectedRecordRescanItemResult::Updated
                   : SelectedRecordRescanItemResult::NotUpdated;
}

SelectedRecordRescanBatchResult RescanSelectedRecords(
    const std::vector<SelectedRecordRescanItem>& items,
    SelectedRecordRescanBatchPort& port) {
    SelectedRecordRescanBatchResult result;
    if (items.empty()) return result;

    if (!port.beginTransaction()) {
        result.failed = static_cast<int>(items.size());
        return result;
    }
    for (const auto& item : items) {
        switch (port.processProtected(item)) {
            case SelectedRecordRescanItemResult::Updated:
                ++result.updated;
                break;
            case SelectedRecordRescanItemResult::Failed:
                ++result.failed;
                break;
            case SelectedRecordRescanItemResult::NotUpdated:
                ++result.failed;
                break;
        }
    }
    if (!port.commitTransaction()) {
        result.updated = 0;
        result.failed = static_cast<int>(items.size());
    }
    return result;
}

}  // namespace aura::record_rescan
