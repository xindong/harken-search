#include "TaskProgressToastSources.h"

#include <algorithm>
#include <array>

namespace {

const char* const kKeys[] = {
    "scan",
    "similar",
    "remove-folder",
    "waveform-rebuild",
    "cover-rebuild",
    "asset-backfill",
};

const TaskProgressSourceState kEmptySource{};

std::array<const TaskProgressSourceState*, static_cast<int>(TaskProgressSource::Count)>
SnapshotSources(const TaskProgressSourcesSnapshot& snapshot) {
    return {&snapshot.scan,          &snapshot.similar,
            &snapshot.removeFolder,  &snapshot.waveformRebuild,
            &snapshot.coverRebuild,  &snapshot.assetBackfill};
}

} // namespace

const char* TaskProgressSourceKey(TaskProgressSource source) {
    const int index = static_cast<int>(source);
    if (index < 0 || index >= static_cast<int>(TaskProgressSource::Count)) {
        return "";
    }
    return kKeys[index];
}

const TaskProgressSourceState& TaskProgressSourceAt(
    const TaskProgressSourcesSnapshot& snapshot, TaskProgressSource source) {
    switch (source) {
    case TaskProgressSource::Scan: return snapshot.scan;
    case TaskProgressSource::Similar: return snapshot.similar;
    case TaskProgressSource::RemoveFolder: return snapshot.removeFolder;
    case TaskProgressSource::WaveformRebuild: return snapshot.waveformRebuild;
    case TaskProgressSource::CoverRebuild: return snapshot.coverRebuild;
    case TaskProgressSource::AssetBackfill: return snapshot.assetBackfill;
    case TaskProgressSource::Count: break;
    }
    return kEmptySource;
}

void SyncTaskProgressToast(TaskProgressToastStack& stack,
                           TaskProgressSyncState& sync,
                           const TaskProgressSourcesSnapshot& snapshot,
                           double now) {
    const auto sources = SnapshotSources(snapshot);
    for (int index = 0; index < static_cast<int>(TaskProgressSource::Count); ++index) {
        const TaskProgressSource source = static_cast<TaskProgressSource>(index);
        const TaskProgressSourceState& state = *sources[static_cast<std::size_t>(index)];
        const std::string key = TaskProgressSourceKey(source);

        if (state.running) {
            const float fraction =
                state.fraction < 0.0f ? -1.0f : std::min(1.0f, state.fraction);
            stack.Report(key, state.title, state.phase, fraction,
                         state.inlineProgressVisible, now);
            sync.seen[index] = true;
            continue;
        }

        if (!sync.seen[index]) continue;
        sync.seen[index] = false;

        if (state.failed) {
            stack.Fail(key, now);
            continue;
        }
        if (state.inlineProgressVisible) {
            stack.Dismiss(key);
            continue;
        }
        stack.Complete(key, now);
    }
}