#include "SimilarityWorkflow.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace std::chrono_literals;
int g_failures = 0;
void Check(bool condition, const char* message) {
    if (condition) std::cout << "PASS: " << message << '\n';
    else { std::cerr << "FAIL: " << message << '\n'; ++g_failures; }
}
template <typename Predicate>
bool WaitUntil(HarkenSearch::SimilarityWorkflow& workflow, Predicate predicate) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        workflow.Poll();
        if (predicate(workflow.Snapshot())) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
HarkenSearch::SimilarityCompletion Complete(
    const HarkenSearch::SimilarityRequest& request, int64_t rowId,
    HarkenSearch::SimilarityWorkflowError error = HarkenSearch::SimilarityWorkflowError::None) {
    HarkenSearch::SimilarityCompletion completion;
    completion.generation = request.generation;
    completion.reference = request.reference;
    completion.error = error;
    if (error == HarkenSearch::SimilarityWorkflowError::None) {
        HarkenSearch::SimilarityDisplayRow row;
        row.fileId = rowId;
        row.filePath = "C:/results/" + std::to_string(rowId) + ".wav";
        row.fileName = std::to_string(rowId) + ".wav";
        row.durationSeconds = 1.25f;
        row.format = "WAV";
        row.similarity = 0.9f;
        completion.rows.push_back(std::move(row));
    }
    return completion;
}

void TestNoSelectionRejectsSearch() {
    std::atomic<int> calls{0};
    HarkenSearch::SimilarityWorkflow workflow(
        [&calls](const HarkenSearch::SimilarityRequest& request,
                 const HarkenSearch::SimilarityWorkflow::CancelCheck&) {
            ++calls;
            return Complete(request, 10);
        });
    Check(!workflow.RequestSearch(SimilarityPreset::Balanced),
          "BDD: search is rejected when no reference sound is selected");
    Check(workflow.Snapshot().status == HarkenSearch::SimilarityWorkflowStatus::NoSelection,
          "BDD: rejected search preserves the no-selection state");
    std::this_thread::sleep_for(10ms);
    Check(calls.load() == 0,
          "BDD: rejected search never reaches the background handler");
}

void TestRequestsOwnImmutableInputs() {
    std::promise<HarkenSearch::SimilarityRequest> observedPromise;
    auto observed = observedPromise.get_future();
    HarkenSearch::SimilarityWorkflow workflow(
        [&observedPromise](const HarkenSearch::SimilarityRequest& request,
                           const HarkenSearch::SimilarityWorkflow::CancelCheck&) mutable {
            observedPromise.set_value(request);
            return Complete(request, 70);
        });
    std::string databasePath = "C:/library/one.db";
    HarkenSearch::SimilarityReference reference{7, "C:/audio/one.wav", "one.wav"};
    workflow.SetDatabasePath(databasePath);
    workflow.SetReference(reference);
    Check(workflow.RequestSearch(SimilarityPreset::TimbreFocused, 23),
          "BDD: a ready reference accepts an explicit request");
    databasePath = "C:/library/mutated.db";
    reference.filePath = "C:/audio/mutated.wav";
    Check(observed.wait_for(1s) == std::future_status::ready,
          "BDD: the worker receives the immutable request");
    if (observed.wait_for(0s) == std::future_status::ready) {
        const auto request = observed.get();
        Check(request.databasePath == "C:/library/one.db" &&
                  request.reference.filePath == "C:/audio/one.wav" &&
                  request.preset == SimilarityPreset::TimbreFocused &&
                  request.topK == 23,
              "BDD: path, reference, preset and limit are copied into the request");
    }
    Check(WaitUntil(workflow, [](const auto& snapshot) {
              return snapshot.status == HarkenSearch::SimilarityWorkflowStatus::ResultsReady;
          }), "BDD: the immutable request completes");
    const auto before = workflow.Snapshot();
    workflow.SetDatabasePath("C:/library/one.db");
    workflow.SetReference(
        HarkenSearch::SimilarityReference{7, "C:/audio/one.wav", "one.wav"});
    const auto after = workflow.Snapshot();
    Check(after.status == before.status && after.rows.size() == before.rows.size(),
          "BDD: unchanged database and reference synchronization is a no-op");
}

void TestLatestRequestWinsAndInvalidateClears() {
    std::atomic<bool> firstWasCancelled{false};
    std::promise<void> enteredPromise;
    auto entered = enteredPromise.get_future();
    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();
    HarkenSearch::SimilarityWorkflow workflow(
        [&enteredPromise, release, &firstWasCancelled](
            const HarkenSearch::SimilarityRequest& request,
            const HarkenSearch::SimilarityWorkflow::CancelCheck& cancelled) mutable {
            if (request.reference.fileId == 1) {
                enteredPromise.set_value();
                release.wait();
                firstWasCancelled.store(cancelled());
                return Complete(request, 100);
            }
            return Complete(request, 200);
        });
    workflow.SetDatabasePath("C:/library/main.db");
    workflow.SetReference(
        HarkenSearch::SimilarityReference{1, "C:/audio/one.wav", "one.wav"});
    Check(workflow.RequestSearch(SimilarityPreset::Balanced),
          "BDD: generation one is accepted");
    Check(entered.wait_for(1s) == std::future_status::ready,
          "BDD: generation one can remain in flight");
    workflow.SetReference(
        HarkenSearch::SimilarityReference{2, "C:/audio/two.wav", "two.wav"});
    Check(workflow.RequestSearch(SimilarityPreset::TimbreFocused),
          "BDD: a newer request replaces pending work without blocking");
    releasePromise.set_value();
    Check(WaitUntil(workflow, [](const auto& snapshot) {
              return snapshot.status == HarkenSearch::SimilarityWorkflowStatus::ResultsReady &&
                  !snapshot.rows.empty();
          }), "BDD: the replacement request publishes a result");
    const auto snapshot = workflow.Snapshot();
    Check(snapshot.reference.fileId == 2 && !snapshot.rows.empty() &&
              snapshot.rows.front().fileId == 200,
          "BDD: stale generation one cannot overwrite generation two");
    Check(firstWasCancelled.load(),
          "BDD: the older handler observes replacement cancellation");
    workflow.Invalidate();
    const auto invalidated = workflow.Snapshot();
    Check(invalidated.status == HarkenSearch::SimilarityWorkflowStatus::Ready &&
              invalidated.rows.empty() &&
              invalidated.error == HarkenSearch::SimilarityWorkflowError::None,
          "BDD: database switch or window close clears publishable results");
}

void TestHandlerErrorsArePublished() {
    HarkenSearch::SimilarityWorkflow returnedError(
        [](const HarkenSearch::SimilarityRequest& request,
           const HarkenSearch::SimilarityWorkflow::CancelCheck&) {
            return Complete(request, -1,
                            HarkenSearch::SimilarityWorkflowError::ReferenceUnreadable);
        });
    returnedError.SetReference(
        HarkenSearch::SimilarityReference{8, "C:/audio/missing.wav", "missing.wav"});
    Check(returnedError.RequestSearch(SimilarityPreset::Balanced),
          "BDD: handler validation starts on the worker");
    Check(WaitUntil(returnedError, [](const auto& snapshot) {
              return snapshot.status == HarkenSearch::SimilarityWorkflowStatus::Error;
          }), "BDD: a handler error reaches the snapshot");
    Check(returnedError.Snapshot().error ==
              HarkenSearch::SimilarityWorkflowError::ReferenceUnreadable,
          "BDD: the actionable handler error is preserved");

    HarkenSearch::SimilarityWorkflow thrownError(
        [](const HarkenSearch::SimilarityRequest&,
           const HarkenSearch::SimilarityWorkflow::CancelCheck&) -> HarkenSearch::SimilarityCompletion {
            throw std::runtime_error("query failed");
        });
    thrownError.SetReference(
        HarkenSearch::SimilarityReference{9, "C:/audio/nine.wav", "nine.wav"});
    Check(thrownError.RequestSearch(SimilarityPreset::Balanced),
          "BDD: a throwing handler is accepted on the worker");
    Check(WaitUntil(thrownError, [](const auto& snapshot) {
              return snapshot.status == HarkenSearch::SimilarityWorkflowStatus::Error;
          }), "BDD: a handler exception becomes an error snapshot");
    Check(thrownError.Snapshot().error == HarkenSearch::SimilarityWorkflowError::SearchFailed,
          "BDD: handler exceptions map to search failed");
}

void TestBatchHistorySurvivesFailureAndCancel() {
    std::mutex observedMutex;
    std::vector<HarkenSearch::SimilarityRequest> observed;
    std::atomic<int> calls{0}; std::atomic<bool> cancellationObserved{false};
    std::promise<void> enteredPromise,releasePromise;
    auto entered=enteredPromise.get_future(); auto release=releasePromise.get_future().share();
    HarkenSearch::SimilarityWorkflow workflow([&](const auto& request,const auto& cancelled) {
        const int call=++calls;
        { std::lock_guard<std::mutex> lock(observedMutex); observed.push_back(request); }
        if(call==2) throw std::runtime_error("transient query failure");
        if(call==4) { enteredPromise.set_value(); release.wait(); cancellationObserved.store(cancelled()); }
        auto completion=Complete(request,100+int64_t(request.shownSounds.size()));
        completion.options=request.options; completion.historyToken="controlled-library-reference";
        completion.rows.front().contentFingerprint=std::to_string(completion.rows.front().fileId);
        return completion;
    });
    workflow.SetDatabasePath("C:/library/batches.db");
    workflow.SetReference(HarkenSearch::SimilarityReference{1,"C:/audio/reference.wav","reference.wav"});
    HarkenSearch::SimilarityOptions options; options.mode=HarkenSearch::SimilarityMode::Substitute;
    auto run=[&] {
        workflow.RequestSearch(options,1);
        return WaitUntil(workflow,[](const auto& snapshot) { return snapshot.status!=HarkenSearch::SimilarityWorkflowStatus::Searching; });
    };
    Check(run() && workflow.Snapshot().batchNumber==1,"V2: only a displayed completion advances batch history");
    Check(run() && workflow.Snapshot().status==HarkenSearch::SimilarityWorkflowStatus::Error,"V2: an exception is visible without consuming a batch");
    Check(run(),"V2: a failed batch can be retried");
    { std::lock_guard<std::mutex> lock(observedMutex); Check(observed.size()>=3 && observed[2].shownSounds.size()==1,"V2: retry preserves the shown sounds and selected mode after an exception"); }
    workflow.RequestSearch(options,1); Check(entered.wait_for(1s)==std::future_status::ready,"V2: another batch can remain in flight");
    workflow.CancelSearch(); releasePromise.set_value();
    Check(run(),"V2: a cancelled batch can be retried");
    { std::lock_guard<std::mutex> lock(observedMutex); Check(observed.size()>=5 && observed[4].shownSounds.size()==2,"V2: cancelled and stale completions never consume unseen results"); }
    Check(cancellationObserved.load(),"V2: cancelled worker observes its stale generation");
    options.deep=true; Check(run(),"V2: changing depth starts another search");
    { std::lock_guard<std::mutex> lock(observedMutex); Check(observed.back().shownSounds.empty(),"V2: changing depth resets the batch history"); }
    workflow.SetDatabasePath("C:/library/another.db"); Check(run(),"V2: another library starts another search");
    { std::lock_guard<std::mutex> lock(observedMutex); Check(observed.back().shownSounds.empty(),"V2: changing libraries resets the batch history"); }
}

void TestDestructorJoinsInFlightHandler() {
    std::promise<void> enteredPromise;
    auto entered = enteredPromise.get_future();
    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();
    std::atomic<bool> handlerExited{false};
    auto workflow = std::make_unique<HarkenSearch::SimilarityWorkflow>(
        [&enteredPromise, release, &handlerExited](
            const HarkenSearch::SimilarityRequest& request,
            const HarkenSearch::SimilarityWorkflow::CancelCheck&) mutable {
            enteredPromise.set_value();
            release.wait();
            handlerExited.store(true);
            return Complete(request, 11);
        });
    workflow->SetReference(
        HarkenSearch::SimilarityReference{10, "C:/audio/ten.wav", "ten.wav"});
    Check(workflow->RequestSearch(SimilarityPreset::Balanced),
          "BDD: an in-flight request exists before destruction");
    Check(entered.wait_for(1s) == std::future_status::ready,
          "BDD: the handler is active before destruction");
    auto destroyed = std::async(std::launch::async, [&workflow] { workflow.reset(); });
    Check(destroyed.wait_for(20ms) == std::future_status::timeout,
          "BDD: destruction waits while the owned handler is running");
    releasePromise.set_value();
    Check(destroyed.wait_for(1s) == std::future_status::ready,
          "BDD: destruction returns after the worker joins");
    Check(handlerExited.load(), "BDD: no worker survives its workflow owner");
}
} // namespace

int main() {
    TestNoSelectionRejectsSearch();
    TestRequestsOwnImmutableInputs();
    TestLatestRequestWinsAndInvalidateClears();
    TestHandlerErrorsArePublished();
    TestBatchHistorySurvivesFailureAndCancel();
    TestDestructorJoinsInFlightHandler();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
