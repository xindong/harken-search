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
    std::cout << "PASS: real outcomes, cancellation remainder, reset and concurrent result counting\n";
}
#else
int main() { std::cerr << "FAIL: batch feature result reporting is not implemented\n"; return 1; }
#endif
