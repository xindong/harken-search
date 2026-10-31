#pragma once
#include "SimilarityAnalysisStore.h"
#include <functional>
namespace HarkenSearch {
enum class SimilarityIndexFailure { Unreadable, Silent, AudioChanged, ModelFailed, SaveFailed };
struct SimilarityIndexIssue { SimilarityFile file; SimilarityIndexFailure reason; };
struct SimilarityIndexProgress { int completed = 0, total = 0, saved = 0, skipped = 0, failed = 0; };
struct SimilarityIndexResult {
    bool ok = false, cancelled = false;
    SimilarityIndexProgress progress;
    std::vector<SimilarityIndexIssue> issues;
    std::string error, modelError;
};
bool AnalyzeSimilarityEntry(const SimilarityFile& file, AudioEmbeddingService* model,
                            SimilarityAnalysisEntry& entry, SimilarityIndexFailure& failure,
                            const FeatureExtractionCancelCheck& cancel = {});
SimilarityIndexResult BuildSimilarityIndex(const std::string& databasePath, const std::string& modelDirectory,
    const FeatureExtractionCancelCheck& cancel = {},
    const std::function<void(const SimilarityIndexProgress&)>& progress = {});
} // namespace HarkenSearch
