// Waveform failure persistence queue behaviour.
// Verifies that failure writes follow the mounted library binding and that a
// stopped worker drops queued failures instead of writing them late.

#include "AsyncWorker.h"
#include "Database.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static bool check(bool condition, const char* label, int& failures) {
    std::cout << (condition ? "PASS" : "FAIL") << ": " << label << "\n";
    if (!condition) ++failures;
    return condition;
}

static fs::path makeTempRoot(const std::string& name) {
    fs::path root = fs::temp_directory_path() / ("aura_waveform_failure_queue_" + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

static bool waitForFailureResult(WaveformWorker& worker, WaveformResult& out,
                                 std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<WaveformResult> results;
    for (;;) {
        results.clear();
        worker.pollResults(results, 8);
        for (auto& result : results) {
            if (!result.success && result.failureObservedAt > 0) {
                out = result;
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

static bool waitForFailedState(Database& db, const std::string& filePath,
                               WaveformCacheState& state, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (db.getWaveformCacheStateByPath(filePath, state) && state.status == "failed") return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

static int testWaveformFailureQueueFollowsWorkerBindingAndStop() {
    using namespace std::chrono_literals;
    int failures = 0;
    const fs::path root = makeTempRoot("binding");
    const fs::path firstPath = root / "queue_first.db";
    const fs::path secondPath = root / "queue_second.db";
    Database first;
    Database second;
    AudioRecord firstRecord;
    firstRecord.filePath = (root / "missing_first.wav").u8string();
    AudioRecord staleRecord;
    staleRecord.filePath = (root / "stale_reject.wav").u8string();
    AudioRecord sharedRecord;
    sharedRecord.filePath = (root / "shared_old_library.wav").u8string();
    AudioRecord secondRecord;
    secondRecord.filePath = (root / "missing_second.wav").u8string();
    if (!check(first.open(firstPath.u8string()) && first.addFile(firstRecord) &&
                   first.addFile(staleRecord) && first.addFile(sharedRecord) &&
                   second.open(secondPath.u8string()) && second.addFile(sharedRecord) &&
                   second.addFile(secondRecord),
               "failure queue fixtures index both libraries", failures)) {
        std::cerr << "queue fixture detail: " << first.getLastError() << " / "
                  << second.getLastError() << "\n";
        return failures;
    }

    WaveformCacheState state;
    WaveformWorker worker;
    worker.start(&first, 1);
    check(worker.submitVisibleRequest(firstRecord.filePath),
          "mounted worker accepts a request for a missing media file", failures);
    WaveformResult failure;
    check(waitForFailureResult(worker, failure, 8000ms),
          "worker reports a decode failure with its own observation time", failures);
    check(failure.failureBinding != 0,
          "a mounted decoder binds its failures to the current library", failures);
    worker.enqueueFailurePersistence(failure);
    check(waitForFailedState(first, firstRecord.filePath, state, 5000ms),
          "a queued failure reaches the sidecar database", failures);

    WaveformResult unbound = failure;
    unbound.filePath = staleRecord.filePath;
    unbound.failureBinding = 0;
    worker.enqueueFailurePersistence(unbound);
    std::this_thread::sleep_for(300ms);
    check(!(first.getWaveformCacheStateByPath(staleRecord.filePath, state) &&
            state.status == "failed"),
          "a result without a library binding never reaches the database", failures);

    worker.stop();
    WaveformResult afterStop = failure;
    afterStop.filePath = sharedRecord.filePath;
    worker.enqueueFailurePersistence(afterStop);
    std::this_thread::sleep_for(300ms);
    check(!(first.getWaveformCacheStateByPath(sharedRecord.filePath, state) &&
            state.status == "failed"),
          "a stopped worker discards queued failures instead of writing them late", failures);

    worker.start(&second, 1);
    worker.enqueueFailurePersistence(afterStop);
    std::this_thread::sleep_for(300ms);
    check(!(second.getWaveformCacheStateByPath(sharedRecord.filePath, state) &&
            state.status == "failed"),
          "a failure produced before rebinding cannot land in the newly mounted library", failures);

    check(worker.submitVisibleRequest(secondRecord.filePath),
          "rebound worker accepts a request in the new library", failures);
    WaveformResult reboundFailure;
    check(waitForFailureResult(worker, reboundFailure, 8000ms),
          "rebound worker reports its own decode failure", failures);
    worker.enqueueFailurePersistence(reboundFailure);
    check(waitForFailedState(second, secondRecord.filePath, state, 5000ms),
          "a rebound worker writes failures into the current library", failures);

    worker.stop();
    first.close();
    second.close();
    fs::remove_all(root);
    return failures;
}

int main() {
    std::cout << std::unitbuf;
    std::cout << "=== Waveform Failure Queue Tests ===\n";
    const int failures = testWaveformFailureQueueFollowsWorkerBindingAndStop();
    std::cout << "\n=== Waveform Failure Queue: " << failures << " failure(s) ===\n";
    return failures == 0 ? 0 : 1;
}
