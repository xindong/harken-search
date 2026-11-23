#pragma once

#include "TaskProgressToastModel.h"

#include <string>

// Bridges the six background task sources into the overlay stack
// (card REQ-20261123-TASK-PROGRESS-TOAST):
// scan/recheck, manual similar analysis, remove folder subtree, waveform
// rebuild, cover rebuild and the idle waveform/cover backfill.
//
// The caller fills a snapshot once per frame from AppState and keeps its own
// sync state. This unit stays free of AppState and ImGui so the dedupe and
// lifecycle rules can be verified by the behaviour tests.

struct TaskProgressSourceState {
    bool running = false;
    bool finished = false;  // source ended successfully during this frame
    bool failed = false;
    // True when the source already paints its own progress elsewhere; the
    // overlay must not duplicate it.
    bool inlineProgressVisible = false;
    float fraction = -1.0f; // negative means indeterminate
    std::string title;
    std::string phase;
};

struct TaskProgressSourcesSnapshot {
    TaskProgressSourceState scan;
    TaskProgressSourceState similar;
    TaskProgressSourceState removeFolder;
    TaskProgressSourceState waveformRebuild;
    TaskProgressSourceState coverRebuild;
    TaskProgressSourceState assetBackfill;
};

enum class TaskProgressSource {
    Scan,
    Similar,
    RemoveFolder,
    WaveformRebuild,
    CoverRebuild,
    AssetBackfill,
    Count,
};

struct TaskProgressSyncState {
    bool seen[static_cast<int>(TaskProgressSource::Count)] = {};
};

const char* TaskProgressSourceKey(TaskProgressSource source);

const TaskProgressSourceState& TaskProgressSourceAt(
    const TaskProgressSourcesSnapshot& snapshot, TaskProgressSource source);

// Applies one frame of source state: running sources are reported and
// refreshed, ended sources complete (or fail) once, and a source that showed
// its own inline progress is dropped instead of popping a stale success row.
void SyncTaskProgressToast(TaskProgressToastStack& stack,
                           TaskProgressSyncState& sync,
                           const TaskProgressSourcesSnapshot& snapshot,
                           double now);