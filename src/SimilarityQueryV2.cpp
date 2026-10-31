#include "SimilarityQueryV2.h"
#include "SimilarityIndexer.h"
#include "platform/text_encoding.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#ifdef HARKEN_WITH_HNSW
#include <hnswlib/hnswlib.h>
#endif
namespace HarkenSearch {
namespace {
using Neighbors = std::vector<std::pair<float,size_t>>;
std::string PathKey(std::string path) {
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '/', '\\');
    std::wstring wide, upper; std::string folded;
    if (Platform::TextEncoding::UppercaseWindowsInvariantUtf8(path, folded, wide, upper)) path = std::move(folded);
    if (path.rfind("\\\\?\\UNC\\", 0) == 0) path = "\\\\" + path.substr(8);
    else if (path.rfind("\\\\?\\", 0) == 0) path.erase(0, 4);
#endif
    try { return std::filesystem::u8path(path).lexically_normal().u8string(); }
    catch (...) { return path; }
}
class EmbeddingIndex {
public:
    void Reset(size_t capacity) {
        count_ = 0;
#ifdef HARKEN_WITH_HNSW
        index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(&space_,std::max<size_t>(capacity,1),16,100,100);
        index_->setEf(300);
#else
        values_.clear(); values_.reserve(capacity);
#endif
    }
    void Add(const AudioEmbedding& value, size_t label) {
#ifdef HARKEN_WITH_HNSW
        index_->addPoint(value.data(),label);
#else
        values_.push_back({value,label});
#endif
        ++count_;
    }
    Neighbors Query(const AudioEmbedding& value, size_t k) {
        k = std::min(k,count_); Neighbors result; if (!k) return result;
#ifdef HARKEN_WITH_HNSW
        index_->setEf(std::max<size_t>(300,k * 2));
        auto found = index_->searchKnn(value.data(),k);
        while (!found.empty()) { result.emplace_back(std::clamp(1-found.top().first,-1.f,1.f),found.top().second); found.pop(); }
#else
        result.reserve(values_.size());
        for (const auto& entry : values_) result.emplace_back(AudioEmbeddingCosine(value,entry.first),entry.second);
        std::partial_sort(result.begin(),result.begin()+k,result.end(),std::greater<>()); result.resize(k);
#endif
        std::sort(result.begin(),result.end(),std::greater<>()); return result;
    }
private:
    size_t count_ = 0;
#ifdef HARKEN_WITH_HNSW
    hnswlib::InnerProductSpace space_{512};
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> index_;
#else
    std::vector<std::pair<AudioEmbedding,size_t>> values_;
#endif
};
float SemanticSequence(const SimilarityAnalysisEntry& a, const SimilarityAnalysisEntry& b,
                       const SimilarityWorkflow::CancelCheck& cancel) {
    if (a.segments.empty() || b.segments.empty()) return -1;
    const size_t n = std::min<size_t>(128,a.segments.size()), m = std::min<size_t>(128,b.segments.size());
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> previous(m+1,inf), current(m+1,inf); previous[0]=0;
    auto sample=[](const auto& parts,size_t at,size_t count)->const auto& { return parts[count>1 ? at*(parts.size()-1)/(count-1) : 0]; };
    for(size_t i=1;i<=n;++i) {
        if(cancel && cancel()) return -1;
        std::fill(current.begin(),current.end(),inf);
        for(size_t j=1;j<=m;++j) {
            if(std::fabs(float(i)/n-float(j)/m)>0.3f && n>2 && m>2) continue;
            const float distance=1-std::max(0.f,AudioEmbeddingCosine(sample(a.segments,i-1,n).embedding,sample(b.segments,j-1,m).embedding));
            current[j]=distance+std::min(previous[j-1],std::min(previous[j]+0.10f,current[j-1]+0.10f));
        }
        previous.swap(current);
    }
    return std::isfinite(previous[m]) ? std::exp(-3*previous[m]/std::max(n,m)) : 0;
}
}
struct SimilarityQueryV2::Impl {
    explicit Impl(std::string directory) : modelDirectory(std::move(directory)) {}
    std::string modelDirectory, path, libraryIdentity, modelError;
    bool modelAttempted = false;
    AudioEmbeddingService model;
    SimilarityAnalysisStore store;
    int64_t revision = -1, segmentRevision = -1;
    std::vector<SimilarityAnalysisEntry> entries;
    std::vector<std::string> pathKeys;
    EmbeddingIndex pooledIndex, segmentIndex;
    std::vector<size_t> segmentOwners;
    std::unordered_map<int64_t,size_t> positions;
    bool EnsureModel() {
        if (model.IsReady()) return true;
        modelAttempted=true; return model.Load(modelDirectory,modelError);
    }
    bool Refresh(bool deep, const SimilarityWorkflow::CancelCheck& cancel) {
        const auto current = store.Revision(); if (current < 0) return false;
        if (revision != current) {
            std::vector<SimilarityAnalysisEntry> fresh;
            if (!store.LoadSummaries(fresh)) return false;
            pooledIndex.Reset(fresh.size()); positions.clear(); pathKeys.clear(); pathKeys.reserve(fresh.size());
            for (size_t i=0;i<fresh.size();++i) {
                if (cancel && cancel()) { revision=-1; return false; }
                positions[fresh[i].file.id]=i;
                pathKeys.push_back(PathKey(fresh[i].file.path));
                if (fresh[i].hasSemantic) pooledIndex.Add(fresh[i].embedding,i);
            }
            if(store.Revision()!=current) { revision=-1; return false; }
            entries.swap(fresh); revision=current; segmentRevision=-1;
        }
        if(deep && segmentRevision!=current) {
            const auto count=store.SegmentCount();
            // Refuse an oversized graph explicitly; never silently drop later audio.
            if(count<0 || count>1000000) return false;
            segmentIndex.Reset(size_t(count)); segmentOwners.clear(); segmentOwners.reserve(size_t(count));
            if(!store.VisitSegments([&](int64_t id,const AudioEmbeddingSegment& segment) {
                if(cancel && cancel()) return false;
                const auto found=positions.find(id); if(found==positions.end()) return true;
                segmentIndex.Add(segment.embedding,segmentOwners.size()); segmentOwners.push_back(found->second); return true;
            }) || store.Revision()!=current) return false;
            segmentRevision=current;
        }
        return true;
    }
};
SimilarityQueryV2::SimilarityQueryV2(std::string directory) : impl_(std::make_unique<Impl>(std::move(directory))) {}
SimilarityQueryV2::~SimilarityQueryV2() = default;
SimilarityCompletion SimilarityQueryV2::Execute(const SimilarityRequest& request, const SimilarityWorkflow::CancelCheck& cancel) {
    SimilarityCompletion result; result.generation=request.generation; result.reference=request.reference; result.options=request.options;
    auto cancelled=[&]{return cancel && cancel();};
    const auto started=std::chrono::steady_clock::now();
    auto fail=[&](SimilarityWorkflowError error){ result.error=error; result.rows.clear(); return result; };
    if(cancelled()) return fail(SimilarityWorkflowError::Cancelled);
    if(request.analysisVersion!=SimilarityOptions::VERSION || request.generation==0 || request.topK<=0)
        return fail(SimilarityWorkflowError::SearchFailed);
    auto& p=*impl_; const bool substitute=request.options.mode==SimilarityMode::Substitute;
    if(substitute && !p.EnsureModel()) return fail(SimilarityWorkflowError::ModelUnavailable);
    if(p.path!=request.databasePath) { p.entries.clear(); p.positions.clear(); p.revision=p.segmentRevision=-1; p.path=request.databasePath; }
    if(!p.store.Open(request.databasePath,cancel)) return fail(cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::DatabaseUnavailable);
    struct CloseConnection { SimilarityAnalysisStore& store; ~CloseConnection() { store.Close(); } } closeConnection{p.store};
    const auto identityToken = p.store.LibraryIdentity();
    if(identityToken.empty()) return fail(SimilarityWorkflowError::DatabaseUnavailable);
    if(identityToken != p.libraryIdentity) { p.libraryIdentity=identityToken; p.revision=p.segmentRevision=-1; }
    SimilarityFile referenceFile;
    if(!p.store.GetFile(request.reference.fileId,referenceFile) || referenceFile.path!=request.reference.filePath)
        return fail(SimilarityWorkflowError::ReferenceUnreadable);
    SimilarityFileIdentity identity;
    if(!ReadSimilarityFileIdentity(referenceFile.path,identity,cancel)) return fail(cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::ReferenceUnreadable);
    SimilarityAnalysisEntry reference;
    if(!p.store.Load(referenceFile.id,reference) || !(reference.identity==identity) || (substitute && !reference.hasSemantic)) {
        if(!p.modelAttempted) p.EnsureModel();
        SimilarityIndexFailure reason;
        if(!AnalyzeSimilarityEntry(referenceFile,p.model.IsReady()?&p.model:nullptr,reference,reason,cancel))
            return fail(cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::ReferenceUnreadable);
        if(!p.store.Publish(reference)) return fail(cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::SearchFailed);
    }
    if(substitute && !reference.hasSemantic) return fail(SimilarityWorkflowError::ModelUnavailable);
    if(cancelled()) return fail(SimilarityWorkflowError::Cancelled);
    const bool semanticAvailable=reference.hasSemantic;
    if(!p.Refresh(request.options.deep && semanticAvailable,cancel))
        return fail(cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::SearchFailed);
    const auto coverage=p.store.Coverage(); result.totalFiles=coverage.total;
    for(const auto& entry:p.entries) if(!substitute || entry.hasSemantic) ++result.indexed;
    const size_t acousticBudget=request.options.deep?(semanticAvailable?500:2000):(semanticAvailable?150:300);
    const size_t semanticBudget=request.options.deep?500:150;
    using Scored = std::pair<float,size_t>;
    std::priority_queue<Scored,std::vector<Scored>,std::greater<Scored>> acoustic;
    SimilarityOptions coarse=request.options; coarse.deep=false;
    const std::string referenceKey = PathKey(reference.file.path);
    for(size_t i=0;i<p.entries.size();++i) {
        if(cancelled()) return fail(SimilarityWorkflowError::Cancelled);
        const auto& entry=p.entries[i];
        if(entry.file.id==reference.file.id || p.pathKeys[i]==referenceKey || (substitute && !entry.hasSemantic)) continue;
        const auto score=ScoreSimilarity(reference.acoustic,entry.acoustic,coarse,0,cancel);
        if(!score.valid) continue;
        acoustic.emplace(score.total,i); if(acoustic.size()>acousticBudget) acoustic.pop();
    }
    std::unordered_set<size_t> candidates;
    while(!acoustic.empty()) { candidates.insert(acoustic.top().second); acoustic.pop(); }
    if(semanticAvailable) {
        for(const auto& neighbor:p.pooledIndex.Query(reference.embedding,semanticBudget+1)) {
            if(p.entries[neighbor.second].file.id!=reference.file.id && p.pathKeys[neighbor.second]!=referenceKey) candidates.insert(neighbor.second);
        }
        if(request.options.deep) {
            std::unordered_map<size_t,float> segmentHits;
            const size_t queries=std::min<size_t>(32,reference.segments.size());
            for(size_t i=0;i<queries;++i) {
                if(cancelled()) return fail(SimilarityWorkflowError::Cancelled);
                const auto& segment=reference.segments[queries>1?i*(reference.segments.size()-1)/(queries-1):0];
                for(const auto& neighbor:p.segmentIndex.Query(segment.embedding,1001)) {
                    const size_t owner=p.segmentOwners[neighbor.second];
                    if(p.entries[owner].file.id==reference.file.id || p.pathKeys[owner]==referenceKey) continue;
                    auto at=segmentHits.find(owner);
                    if(at==segmentHits.end()) segmentHits.emplace(owner,neighbor.first); else at->second=std::max(at->second,neighbor.first);
                }
            }
            std::vector<Scored> hits; for(const auto& hit:segmentHits) hits.emplace_back(hit.second,hit.first);
            const auto count=std::min<size_t>(1000,hits.size());
            std::partial_sort(hits.begin(),hits.begin()+count,hits.end(),std::greater<>());
            for(size_t i=0;i<count;++i) candidates.insert(hits[i].second);
        }
    }
    std::vector<Scored> ranked;
    for(size_t i:candidates) {
        const auto& entry=p.entries[i];
        const float category=substitute?std::max(0.f,AudioEmbeddingCosine(reference.embedding,entry.embedding)):0;
        const auto score=ScoreSimilarity(reference.acoustic,entry.acoustic,coarse,category,cancel);
        if(score.valid) ranked.emplace_back(score.total,i);
    }
    std::sort(ranked.begin(),ranked.end(),[&](const Scored& a,const Scored& b){return a.first!=b.first?a.first>b.first:p.entries[a.second].file.id<p.entries[b.second].file.id;});
    if(request.options.deep && ranked.size()>200) ranked.resize(200);
    std::unordered_set<std::string> emittedPaths;
    for(const auto& candidate:ranked) {
        if(cancelled()) return fail(SimilarityWorkflowError::Cancelled);
        if(emittedPaths.count(p.pathKeys[candidate.second])) continue;
        const auto& summary=p.entries[candidate.second];
        SimilarityAnalysisEntry detailed;
        const SimilarityAnalysisEntry* entry=&summary;
        if(request.options.deep) {
            if(!p.store.Load(summary.file.id,detailed)) { ++result.staleFiles; continue; }
            entry=&detailed;
        }
        SimilarityFile file; SimilarityFileIdentity currentIdentity;
        if(!p.store.GetFile(entry->file.id,file) || file.path!=entry->file.path ||
            !ReadSimilarityFileIdentity(file.path,currentIdentity,cancel) || !(currentIdentity==entry->identity)) {
            if(cancelled()) return fail(SimilarityWorkflowError::Cancelled);
            ++result.staleFiles; p.store.Invalidate(entry->file.id); continue;
        }
        float category=substitute?std::max(0.f,AudioEmbeddingCosine(reference.embedding,entry->embedding)):0;
        if(request.options.deep && substitute) {
            const float sequence=SemanticSequence(reference,*entry,cancel);
            if(sequence<0) return fail(cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::SearchFailed);
            category=0.5f*category+0.5f*sequence;
        }
        const auto score=ScoreSimilarity(reference.acoustic,entry->acoustic,request.options,category,cancel);
        if(!score.valid) return fail(cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::SearchFailed);
        emittedPaths.insert(p.pathKeys[candidate.second]);
        SimilarityDisplayRow row; row.fileId=file.id; row.filePath=file.path; row.fileName=file.name;
        row.format=file.format; row.durationSeconds=file.seconds; row.similarity=score.total; row.breakdown=score;
        result.rows.push_back(std::move(row));
        if(!request.options.deep && result.rows.size()>=size_t(std::min(50,request.topK))) break;
    }
    if(cancelled()) return fail(SimilarityWorkflowError::Cancelled);
    std::sort(result.rows.begin(),result.rows.end(),[](const auto& a,const auto& b){return a.similarity!=b.similarity?a.similarity>b.similarity:a.fileId<b.fileId;});
    if(result.rows.size()>size_t(std::min(50,request.topK))) result.rows.resize(std::min(50,request.topK));
    result.indexed=std::max<int64_t>(0,result.indexed-result.staleFiles);
    if(result.rows.empty()) return fail(result.indexed<result.totalFiles?SimilarityWorkflowError::IndexRequired:SimilarityWorkflowError::NoCandidates);
    result.queryMilliseconds=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count();
    return result;
}
SimilarityWorkflow::Handler MakeSimilarityQueryV2Handler(const std::string& directory) {
    auto service=std::make_shared<SimilarityQueryV2>(directory);
    return [service](const SimilarityRequest& request,const SimilarityWorkflow::CancelCheck& cancel) { return service->Execute(request,cancel); };
}
} // namespace HarkenSearch
