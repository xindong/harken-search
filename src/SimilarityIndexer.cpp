#include "SimilarityIndexer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
namespace HarkenSearch {
namespace {
void RecordSimilarityOutcome(SimilarityAnalysisStore& store, const SimilarityFile& file, bool modelReady,
                             bool analyzed, SimilarityIndexFailure failure, SimilarityAnalysisEntry& entry,
                             SimilarityIndexResult& result) {
    if (analyzed && store.Publish(entry)) {
        ++result.progress.saved;
        if (modelReady && !entry.hasSemantic) { ++result.progress.failed; result.issues.push_back({file,SimilarityIndexFailure::ModelFailed}); }
        return;
    }
    if (analyzed) failure = SimilarityIndexFailure::SaveFailed;
    // An unreadable/changed source must not continue to look indexed.
    if (failure != SimilarityIndexFailure::SaveFailed) store.Invalidate(file.id);
    ++result.progress.failed; result.issues.push_back({file,failure});
}
} // namespace
bool AnalyzeSimilarityEntry(const SimilarityFile& file, AudioEmbeddingService* model, SimilarityAnalysisEntry& out,
                            SimilarityIndexFailure& failure, const FeatureExtractionCancelCheck& cancel) {
    SimilarityAnalysisEntry entry; entry.file = file; failure = SimilarityIndexFailure::Unreadable;
    if (!ReadSimilarityFileIdentity(file.path,entry.identity,cancel)) return false;
    const auto analysis = AnalyzeSimilarityFile(file.path,cancel);
    if (!analysis.ok) {
        if (analysis.error == FeatureExtractionError::NoSamples) failure = SimilarityIndexFailure::Silent;
        return false;
    }
    entry.acoustic = analysis.analysis;
    if (model && model->IsReady()) {
        auto encoded = model->EncodeFile(file.path,entry.acoustic.activeStart,entry.acoustic.activeSeconds,cancel);
        if (encoded.ok) {
            entry.hasSemantic = true; entry.embedding = encoded.pooled; entry.segments = std::move(encoded.segments);
        } else failure = SimilarityIndexFailure::ModelFailed;
    }
    SimilarityFileIdentity after;
    if (!ReadSimilarityFileIdentity(file.path,after,cancel) || !(after == entry.identity)) {
        failure = SimilarityIndexFailure::AudioChanged; return false;
    }
    out = std::move(entry); return true;
}
SimilarityIndexResult BuildSimilarityIndex(const std::string& path, const std::string& modelDirectory,
    const FeatureExtractionCancelCheck& cancel, const std::function<void(const SimilarityIndexProgress&)>& progress,
    SimilarityIndexScope scope, int concurrency) {
    SimilarityIndexResult result;
    auto cancelled = [&] { return cancel && cancel(); };
    if (cancelled()) { result.cancelled = true; return result; }
    SimilarityAnalysisStore store;
    if (!store.Open(path,cancel)) { result.error = store.Error(); result.cancelled = cancelled(); return result; }
    AudioEmbeddingService model; model.Load(modelDirectory,result.modelError);
    std::vector<SimilarityFile> files;
    const bool selected = scope == SimilarityIndexScope::ValidateAll
        ? store.Files(files) : store.PendingFiles(files,model.IsReady());
    if (!selected) { result.error = "library_read_failed"; result.cancelled = cancelled(); return result; }
    result.progress.total = int(files.size()); if (progress) progress(result.progress);
    const int workers = std::clamp(concurrency, 1, kMaxSimilarityConcurrency);
    if (workers > 1 && files.size() > 1) {
        std::vector<SimilarityFile> tasks; tasks.reserve(files.size());
        for (const auto& file : files) {
            if (cancelled()) { result.cancelled = true; return result; }
            SimilarityAnalysisEntry previous; SimilarityFileIdentity identity;
            if (store.Load(file.id,previous,false) && (!model.IsReady() || previous.hasSemantic) &&
                ReadSimilarityFileIdentity(file.path,identity,cancel) && identity == previous.identity) {
                ++result.progress.skipped; ++result.progress.completed;
            } else tasks.push_back(file);
        }
        if (progress) progress(result.progress);
        if (!tasks.empty()) {
            // Each worker owns its ONNX session: no shared model state, and the
            // per-session thread budget stays bounded across the pool.
            const int threadsPerSession = std::max(1, 4 / workers);
            std::vector<std::unique_ptr<AudioEmbeddingService>> extraModels;
            std::vector<AudioEmbeddingService*> pool{&model};
            for (int i = 1; model.IsReady() && i < workers && i < int(tasks.size()); ++i) {
                auto service = std::make_unique<AudioEmbeddingService>();
                std::string error;
                if (!service->Load(modelDirectory,error,threadsPerSession)) break;
                extraModels.push_back(std::move(service));
                pool.push_back(extraModels.back().get());
            }
            struct Slot {
                bool analyzed = false;
                SimilarityIndexFailure failure = SimilarityIndexFailure::Unreadable;
                SimilarityAnalysisEntry entry;
            };
            std::vector<std::unique_ptr<Slot>> slots(tasks.size());
            std::mutex slotMutex; std::condition_variable slotReady;
            std::atomic<size_t> nextTask{0};
            std::vector<std::thread> threads;
            for (AudioEmbeddingService* workerModel : pool) {
                threads.emplace_back([&, workerModel] {
                    for (;;) {
                        const size_t index = nextTask.fetch_add(1);
                        if (index >= tasks.size() || cancelled()) break;
                        auto slot = std::make_unique<Slot>();
                        try {
                            slot->analyzed = AnalyzeSimilarityEntry(tasks[index],workerModel,slot->entry,slot->failure,cancel);
                        } catch (const std::exception&) { slot->analyzed = false; }
                        { std::lock_guard<std::mutex> lock(slotMutex); slots[index] = std::move(slot); }
                        slotReady.notify_all();
                    }
                    slotReady.notify_all();
                });
            }
            for (size_t index = 0; index < tasks.size(); ++index) {
                std::unique_ptr<Slot> slot;
                {
                    std::unique_lock<std::mutex> lock(slotMutex);
                    slotReady.wait_for(lock,std::chrono::milliseconds(100),[&]{ return slots[index] != nullptr || cancelled(); });
                    slot = std::move(slots[index]);
                }
                if (!slot) { result.cancelled = true; break; }
                RecordSimilarityOutcome(store,tasks[index],model.IsReady(),slot->analyzed,slot->failure,slot->entry,result);
                if (cancelled()) { result.cancelled = true; break; }
                ++result.progress.completed; if (progress) progress(result.progress);
            }
            for (auto& thread : threads) thread.join();
        }
        result.ok = !result.cancelled; return result;
    }
    for (const auto& file : files) {
        if (cancelled()) { result.cancelled = true; return result; }
        SimilarityAnalysisEntry previous;
        SimilarityFileIdentity identity;
        if (store.Load(file.id,previous,false) && (!model.IsReady() || previous.hasSemantic) &&
            ReadSimilarityFileIdentity(file.path,identity,cancel) && identity == previous.identity) {
            ++result.progress.skipped;
        } else {
            SimilarityIndexFailure failure = SimilarityIndexFailure::Unreadable;
            SimilarityAnalysisEntry entry;
            const bool analyzed = AnalyzeSimilarityEntry(file,&model,entry,failure,cancel);
            if (cancelled()) { result.cancelled = true; return result; }
            RecordSimilarityOutcome(store,file,model.IsReady(),analyzed,failure,entry,result);
        }
        ++result.progress.completed; if (progress) progress(result.progress);
    }
    result.ok = true; return result;
}
} // namespace HarkenSearch
