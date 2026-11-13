#include "SimilarityIndexer.h"
#include "PerformanceMonitor.h"
namespace HarkenSearch {
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
    SimilarityIndexScope scope) {
    harken::performance::Scope activity(harken::performance::Module::Similarity);
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
            if (analyzed && store.Publish(entry)) {
                ++result.progress.saved;
                if (model.IsReady() && !entry.hasSemantic) { ++result.progress.failed; result.issues.push_back({file,SimilarityIndexFailure::ModelFailed}); }
            } else {
                if (analyzed) failure = SimilarityIndexFailure::SaveFailed;
                // An unreadable/changed source must not continue to look indexed.
                if (failure != SimilarityIndexFailure::SaveFailed) store.Invalidate(file.id);
                ++result.progress.failed; result.issues.push_back({file,failure});
            }
        }
        ++result.progress.completed; if (progress) progress(result.progress);
    }
    result.ok = true; return result;
}
} // namespace HarkenSearch
