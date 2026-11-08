#include <iostream>
#include <thread>
#include <vector>
#if __has_include("BatchFeatureResultState.h")
#include "BatchFeatureResultState.h"
using namespace HarkenSearch;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL: " << #x << "\n"; return 1; } } while (false)
int main() {
    BatchFeatureResultState results;
    CHECK(!results.Snapshot().available);
    results.Reset(700); results.SetCurrentFile("cat.wav");
    for (int i = 0; i < 390; ++i) results.RecordSuccess();
    for (int i = 0; i < 10; ++i) results.RecordFailure(BatchFeatureFailure::Unreadable);
    results.Finish(true);
    auto s = results.Snapshot();
    CHECK(s.available && !s.running && s.stopped);
    CHECK(s.succeeded == 390 && s.Failed() == 10 && s.Processed() == 400 && s.Remaining() == 300);
    CHECK(s.failures[1] == 10);
    results.Reset(0); results.SetTotal(1000);
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) workers.emplace_back([&] { for (int i = 0; i < 200; ++i) results.RecordSuccess(); });
    for (auto& worker : workers) worker.join();
    results.RecordFailure(BatchFeatureFailure::Save);
    results.Finish(false); s = results.Snapshot();
    CHECK(s.succeeded == 800 && s.failures[3] == 1 && s.Remaining() == 199);
    results.Clear(); CHECK(!results.Snapshot().available);
    // A newer indexer reports cumulative outcomes; importing them must not
    // double-count an acoustically saved file whose semantic analysis failed.
    BatchFeatureResultSnapshot indexed;
    indexed.available = true; indexed.running = true; indexed.total = 164;
    indexed.succeeded = 35;
    indexed.failures[static_cast<size_t>(BatchFeatureFailure::Empty)] = 1;
    indexed.failures[static_cast<size_t>(BatchFeatureFailure::Model)] = 1;
    indexed.failures[static_cast<size_t>(BatchFeatureFailure::Changed)] = 1;
    results.Publish(indexed); s = results.Snapshot();
    CHECK(s.running && s.Processed() == 38 && s.Remaining() == 126);
    indexed.running = false; indexed.stopped = true; indexed.error = "library_read_failed";
    results.Publish(indexed); s = results.Snapshot();
    CHECK(!s.running && s.stopped && s.succeeded == 35 && s.Failed() == 3);
    CHECK(s.error == "library_read_failed" && s.Remaining() == 126);
    CHECK(s.failures[static_cast<size_t>(BatchFeatureFailure::Model)] == 1);
    CHECK(s.failures[static_cast<size_t>(BatchFeatureFailure::Changed)] == 1);
    results.Reset(1); s = results.Snapshot();
    CHECK(s.running && s.total == 1 && s.Processed() == 0 && s.error.empty());
    std::cout << "PASS: real outcomes, cancellation remainder, reset and concurrent result counting\n";
}
#else
int main() { std::cerr << "FAIL: batch feature result reporting is not implemented\n"; return 1; }
#endif
