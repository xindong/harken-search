#pragma once
#include "SimilarityWorkflow.h"
#include <memory>
namespace HarkenSearch {
class SimilarityQueryV2 final {
public:
    explicit SimilarityQueryV2(std::string modelDirectory);
    ~SimilarityQueryV2();
    SimilarityCompletion Execute(const SimilarityRequest&, const SimilarityWorkflow::CancelCheck&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
SimilarityWorkflow::Handler MakeSimilarityQueryV2Handler(const std::string& modelDirectory);
} // namespace HarkenSearch
