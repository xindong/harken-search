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
    Neighbors Query(const AudioEmbedding& value, size_t k, const std::vector<uint8_t>* allowed = nullptr) {
        k = std::min(k,count_); Neighbors result; if (!k) return result;
#ifdef HARKEN_WITH_HNSW
        index_->setEf(std::max<size_t>(300,k * 2));
        struct Filter final : hnswlib::BaseFilterFunctor {
            const std::vector<uint8_t>* values;
            explicit Filter(const std::vector<uint8_t>* input) : values(input) {}
            bool operator()(hnswlib::labeltype label) override { return !values || (label < values->size() && (*values)[label]); }
        } filter(allowed);
        auto found = index_->searchKnn(value.data(),k,allowed ? &filter : nullptr);
        while (!found.empty()) { result.emplace_back(std::clamp(1-found.top().first,-1.f,1.f),found.top().second); found.pop(); }
#else
        result.reserve(values_.size());
        for (const auto& entry : values_) if (!allowed || (*allowed)[entry.second]) result.emplace_back(AudioEmbeddingCosine(value,entry.first),entry.second);
        k = std::min(k, result.size());
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
// Stored vectors have already passed the unit-length and finite-value check.
float UnitDot(const AudioEmbedding& a, const AudioEmbedding& b) {
    float dot = 0; for (size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
    return std::clamp(dot, -1.0f, 1.0f);
}
bool SimilarSummary(const SimilarityAnalysisEntry& a, const SimilarityAnalysisEntry& b) {
    if (std::fabs(a.acoustic.activeSeconds - b.acoustic.activeSeconds) > 0.02f * std::max(a.acoustic.activeSeconds, b.acoustic.activeSeconds)) return false;
    if (a.acoustic.onsets.size() != b.acoustic.onsets.size()) return false;
    if (a.acoustic.pitch > 0 && b.acoustic.pitch > 0 && std::fabs(std::log2(a.acoustic.pitch / b.acoustic.pitch)) > 0.02f) return false;
    float meanDifference = 0;
    for (size_t i = 0; i < a.acoustic.mean.size(); ++i) meanDifference += std::fabs(a.acoustic.mean[i] - b.acoustic.mean[i]);
    if (meanDifference > 13.0f * 1.5f) return false;
    const auto score = ScoreSimilarity(a.acoustic, b.acoustic, {});
    // Small phase changes move spectral/attack summaries even for nearly
    // identical sounds. The ordered frame check below is the final authority.
    if (!score.valid || score.parts[1] < 0.95f || score.parts[2] < 0.93f || score.parts[3] < 0.985f) return false;
    return !a.hasSemantic || !b.hasSemantic || UnitDot(a.embedding, b.embedding) > 0.995f;
}
using SoundShape = std::array<AcousticFrame, 16>;
SoundShape SampleShape(const AcousticAnalysis& analysis) {
    SoundShape shape{};
    for (size_t i = 0; i < shape.size(); ++i) shape[i] = analysis.frames[i * (analysis.frames.size() - 1) / (shape.size() - 1)];
    return shape;
}
bool SameShape(const SoundShape& a, const SoundShape& b) {
    float timbre = 0, envelope = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        for (size_t j = 0; j < a[i].mfcc.size(); ++j) timbre += std::fabs(a[i].mfcc[j] - b[i].mfcc[j]);
        envelope += std::fabs(a[i].energy - b[i].energy);
        if (a[i].voiced > 0.1f && b[i].voiced > 0.1f && a[i].pitch > 0 && b[i].pitch > 0 &&
            std::fabs(std::log2(a[i].pitch / b[i].pitch)) > 0.02f) return false;
    }
    return timbre / (16 * 13) < 1.5f && envelope / 16 < 0.035f;
}
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
    int64_t revision = -1;
    std::vector<SimilarityAnalysisEntry> entries;
    std::vector<std::string> pathKeys;
    EmbeddingIndex pooledIndex;
    std::unordered_map<int64_t,size_t> positions;
    bool EnsureModel() {
        if (model.IsReady()) return true;
        modelAttempted=true; return model.Load(modelDirectory,modelError);
    }
    bool Refresh(const SimilarityWorkflow::CancelCheck& cancel) {
        const auto current = store.Revision(); if (current < 0) return false;
        if (revision != current) {
            std::vector<SimilarityAnalysisEntry> fresh;
            if (!store.LoadSummaries(fresh)) return false;
            pooledIndex.Reset(fresh.size()); positions.clear(); pathKeys.clear(); pathKeys.reserve(fresh.size());
            std::unordered_set<std::string> indexedContent;
            for (size_t i=0;i<fresh.size();++i) {
                if (cancel && cancel()) { revision=-1; return false; }
                positions[fresh[i].file.id]=i;
                pathKeys.push_back(PathKey(fresh[i].file.path));
                if (fresh[i].hasSemantic && indexedContent.insert(fresh[i].identity.sha256).second) pooledIndex.Add(fresh[i].embedding,i);
            }
            if(store.Revision()!=current) { revision=-1; return false; }
            entries.swap(fresh); revision=current;
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
    auto fail=[&](SimilarityWorkflowError error){ result.error=error; result.rows.clear();
        result.queryMilliseconds=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count(); return result; };
    if(cancelled()) return fail(SimilarityWorkflowError::Cancelled);
    if(request.analysisVersion!=SimilarityOptions::VERSION || request.generation==0 || request.topK<=0)
        return fail(SimilarityWorkflowError::SearchFailed);
    auto& p=*impl_; const bool substitute=request.options.mode==SimilarityMode::Substitute;
    if(substitute && !p.EnsureModel()) return fail(SimilarityWorkflowError::ModelUnavailable);
    if(p.path!=request.databasePath) { p.entries.clear(); p.positions.clear(); p.revision=-1; p.path=request.databasePath; }
    if(!p.store.Open(request.databasePath,cancel)) return fail(cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::DatabaseUnavailable);
    struct CloseConnection { SimilarityAnalysisStore& store; ~CloseConnection() { store.Close(); } } closeConnection{p.store};
    const auto identityToken = p.store.LibraryIdentity();
    if(identityToken.empty()) return fail(SimilarityWorkflowError::DatabaseUnavailable);
    if(identityToken != p.libraryIdentity) { p.libraryIdentity=identityToken; p.revision=-1; }
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
    if(!p.Refresh(cancel))
        return fail(cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::SearchFailed);
    const auto coverage=p.store.Coverage(); result.totalFiles=coverage.total;
    for(const auto& entry:p.entries) if(!substitute || entry.hasSemantic) ++result.indexed;
    result.historyToken = identityToken + ":" + reference.identity.sha256 + ":" +
        std::to_string(reference.file.id) + ":" + std::to_string(int(request.options.mode)) + ":" +
        std::to_string(request.options.deep) + ":" + std::to_string(int(request.options.preset)) + ":" + kSimilarityModelRevision;
    const bool continuing = request.historyToken == result.historyToken;
    std::unordered_set<std::string> shownContent, shownPaths;
    if (continuing) for (const auto& shown : request.shownSounds) {
        if (!shown.contentFingerprint.empty()) shownContent.insert(shown.contentFingerprint);
        else shownPaths.insert(PathKey(shown.filePath));
    }
    auto alreadyShown = [&](size_t index) {
        return shownContent.count(p.entries[index].identity.sha256) || shownPaths.count(p.pathKeys[index]);
    };
    const size_t acousticBudget=request.options.deep?(semanticAvailable?500:2000):(semanticAvailable?150:300);
    const size_t semanticBudget=request.options.deep?500:150;
    using Scored = std::pair<float,size_t>;
    std::vector<Scored> acoustic; acoustic.reserve(p.entries.size());
    std::vector<uint8_t> eligible(p.entries.size(), 0);
    std::unordered_set<std::string> acousticContent;
    SimilarityOptions coarse=request.options; coarse.deep=false;
    const std::string referenceKey = PathKey(reference.file.path);
    for(size_t i=0;i<p.entries.size();++i) {
        if(cancelled()) return fail(SimilarityWorkflowError::Cancelled);
        const auto& entry=p.entries[i];
        ++result.stats.scannedFiles;
        if(entry.file.id==reference.file.id || p.pathKeys[i]==referenceKey || entry.identity.sha256==reference.identity.sha256 ||
            alreadyShown(i) || (substitute && !entry.hasSemantic)) continue;
        eligible[i] = 1;
        const auto score=ScoreSimilarity(reference.acoustic,entry.acoustic,coarse,0,cancel);
        if(!score.valid) continue;
        acoustic.emplace_back(score.total,i);
    }
    auto better = [&](const Scored& a, const Scored& b) { return a.first != b.first ? a.first > b.first : p.entries[a.second].file.id < p.entries[b.second].file.id; };
    std::sort(acoustic.begin(), acoustic.end(), better);
    std::unordered_set<size_t> candidates;
    for (const auto& candidate : acoustic) {
        if (candidates.size() >= acousticBudget) break;
        if (acousticContent.insert(p.entries[candidate.second].identity.sha256).second) candidates.insert(candidate.second);
    }
    Neighbors semanticHits; std::vector<Scored> segmentHits;
    if(semanticAvailable) {
        semanticHits = p.pooledIndex.Query(reference.embedding,semanticBudget,&eligible);
        for(const auto& neighbor:semanticHits) {
            if(p.entries[neighbor.second].file.id!=reference.file.id && p.pathKeys[neighbor.second]!=referenceKey && !alreadyShown(neighbor.second)) candidates.insert(neighbor.second);
        }
        if(request.options.deep) {
            const size_t queries=std::min<size_t>(32,reference.segments.size());
            std::priority_queue<Scored,std::vector<Scored>,std::greater<Scored>> bestSegments;
            int64_t currentFile = -1; size_t owner = 0; bool allowed = false; float best = -1;
            auto finishFile = [&] {
                if (allowed && best > 0) { bestSegments.emplace(best,owner); if (bestSegments.size() > 1000) bestSegments.pop(); }
            };
            // Ordered SQLite streaming keeps only one file's maximum and the
            // best 1000 owners, regardless of the total segment count.
            if (!p.store.VisitSegments([&](int64_t id, const AudioEmbeddingSegment& segment) {
                if (cancelled()) return false;
                ++result.stats.visitedSegments;
                if (id != currentFile) {
                    finishFile(); currentFile = id; best = -1; const auto found = p.positions.find(id);
                    allowed = found != p.positions.end() && eligible[found->second];
                    if (allowed) owner = found->second;
                }
                if (allowed) for (size_t i=0;i<queries;++i) {
                    const auto& ref=reference.segments[queries>1?i*(reference.segments.size()-1)/(queries-1):0];
                    best = std::max(best,UnitDot(ref.embedding,segment.embedding));
                }
                return true;
            }) || p.store.Revision() != p.revision) return fail(cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::SearchFailed);
            finishFile();
            while (!bestSegments.empty()) { segmentHits.push_back(bestSegments.top()); bestSegments.pop(); }
            std::sort(segmentHits.begin(),segmentHits.end(),better);
            for (const auto& hit : segmentHits) candidates.insert(hit.second);
        }
    }
    std::vector<Scored> ranked;
    for(size_t i:candidates) {
        const auto& entry=p.entries[i];
        const float category=substitute?std::max(0.f,AudioEmbeddingCosine(reference.embedding,entry.embedding)):0;
        const auto score=ScoreSimilarity(reference.acoustic,entry.acoustic,coarse,category,cancel);
        if(score.valid) ranked.emplace_back(score.total,i);
    }
    std::sort(ranked.begin(),ranked.end(),better);
    if(request.options.deep && ranked.size()>800) {
        std::unordered_set<size_t> retained;
        for (size_t i=0; i<std::min<size_t>(544,ranked.size()); ++i) retained.insert(ranked[i].second);
        for (size_t i=0; i<std::min<size_t>(128,semanticHits.size()); ++i) retained.insert(semanticHits[i].second);
        for (size_t i=0; i<std::min<size_t>(128,segmentHits.size()); ++i) retained.insert(segmentHits[i].second);
        for (const auto& candidate : ranked) { if (retained.size()>=800) break; retained.insert(candidate.second); }
        ranked.erase(std::remove_if(ranked.begin(),ranked.end(),[&](const auto& candidate){return !retained.count(candidate.second);}),ranked.end());
    }
    result.minimumScore = continuing && std::isfinite(request.minimumScore) ? std::clamp(request.minimumScore,0.35f,1.0f) :
        std::max(0.35f, ranked.empty() ? 0.0f : ranked.front().first * 0.55f);
    std::vector<size_t> shownIndices;
    if (continuing) for (const auto& shown : request.shownSounds) {
        const auto found=p.positions.find(shown.fileId);
        if (found!=p.positions.end() && p.entries[found->second].identity.sha256==shown.contentFingerprint) shownIndices.push_back(found->second);
    }
    std::unordered_map<size_t,SoundShape> shapes;
    auto shapeFor = [&](size_t index, SoundShape& out) {
        const auto found=shapes.find(index);
        if (found!=shapes.end()) { out=found->second; return true; }
        SimilarityAnalysisEntry detailed;
        if (!p.store.Load(p.entries[index].file.id,detailed) || detailed.acoustic.frames.empty()) return false;
        out=SampleShape(detailed.acoustic);
        if (shapes.size()>=4096) shapes.clear();
        shapes.emplace(index,out); return true;
    };
    auto nearDuplicate = [&](size_t a, size_t b) {
        if (p.entries[a].identity.sha256==p.entries[b].identity.sha256 || p.pathKeys[a]==p.pathKeys[b]) return true;
        if (!SimilarSummary(p.entries[a],p.entries[b])) return false;
        SoundShape first,second; return shapeFor(a,first) && shapeFor(b,second) && SameShape(first,second);
    };
    struct Selected { size_t index; SimilarityDisplayRow row; };
    std::vector<Selected> selected; const size_t limit=size_t(std::min(50,request.topK));
    std::unordered_set<size_t> processed;
    std::unordered_set<std::string> processedContent;
    SimilarityWorkflowError processingError=SimilarityWorkflowError::None;
    auto process = [&](size_t index) {
        if (cancelled()) { processingError=SimilarityWorkflowError::Cancelled; return false; }
        if (!processed.insert(index).second) return true;
        const auto& summary=p.entries[index];
        if (processedContent.count(summary.identity.sha256)) return true;
        ++result.stats.candidates;
        for (size_t shown : shownIndices) if (nearDuplicate(index,shown)) return true;
        const float coarseCategory=substitute?std::max(0.f,AudioEmbeddingCosine(reference.embedding,summary.embedding)):0;
        const auto quickScore=ScoreSimilarity(reference.acoustic,summary.acoustic,coarse,coarseCategory,cancel);
        if (!quickScore.valid) { processingError=cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::SearchFailed; return false; }
        float possibleScore=quickScore.total;
        if (request.options.deep) {
            possibleScore=0;
            for (size_t part=0;part<6;++part) {
                float upper=quickScore.parts[part];
                if (part==0 || part==1 || part==2 || (part==4 && upper>0)) upper=(upper+1)*0.5f;
                possibleScore+=upper*quickScore.weights[part];
            }
        }
        // A score that cannot reach this query's floor needs no file re-read.
        if (possibleScore+1e-6f < result.minimumScore) return true;
        SimilarityAnalysisEntry detailed;
        const SimilarityAnalysisEntry* entry=&summary;
        if(request.options.deep) {
            if(!p.store.Load(summary.file.id,detailed)) { ++result.staleFiles; return true; }
            entry=&detailed;
        }
        SimilarityFile file; SimilarityFileIdentity currentIdentity;
        if(!p.store.GetFile(entry->file.id,file) || file.path!=entry->file.path ||
            !ReadSimilarityFileIdentity(file.path,currentIdentity,cancel) || !(currentIdentity==entry->identity)) {
            if(cancelled()) { processingError=SimilarityWorkflowError::Cancelled; return false; }
            ++result.staleFiles; p.store.Invalidate(entry->file.id); return true;
        }
        processedContent.insert(summary.identity.sha256);
        float category=substitute?std::max(0.f,AudioEmbeddingCosine(reference.embedding,entry->embedding)):0;
        if(request.options.deep && substitute) {
            const float sequence=SemanticSequence(reference,*entry,cancel);
            if(sequence<0) { processingError=cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::SearchFailed; return false; }
            category=0.5f*category+0.5f*sequence;
        }
        if(request.options.deep) ++result.stats.detailedComparisons;
        const auto score=ScoreSimilarity(reference.acoustic,entry->acoustic,request.options,category,cancel);
        if(!score.valid) { processingError=cancelled()?SimilarityWorkflowError::Cancelled:SimilarityWorkflowError::SearchFailed; return false; }
        if(score.total < result.minimumScore) return true;
        SimilarityDisplayRow row; row.fileId=file.id; row.filePath=file.path; row.fileName=file.name;
        row.format=file.format; row.durationSeconds=file.seconds; row.similarity=score.total; row.breakdown=score;
        row.contentFingerprint=entry->identity.sha256;
        bool replaced=false;
        for (auto& previous : selected) if (nearDuplicate(index,previous.index)) {
            if (row.similarity > previous.row.similarity || (row.similarity==previous.row.similarity && row.fileId < previous.row.fileId)) previous={index,std::move(row)};
            replaced=true; break;
        }
        if(!replaced) selected.push_back({index,std::move(row)});
        std::sort(selected.begin(),selected.end(),[](const auto& a,const auto& b){ return a.row.similarity!=b.row.similarity?a.row.similarity>b.row.similarity:a.row.fileId<b.row.fileId; });
        if(selected.size()>limit) selected.resize(limit);
        return true;
    };
    for (const auto& candidate : ranked) {
        if(!process(candidate.second)) return fail(processingError);
        if(!request.options.deep && selected.size()>=limit) break;
    }
    // Refill from the whole current library's lightweight ranking when copies
    // or prior batches consume the initial shortlist; never shuffle old rows.
    for (const auto& candidate : acoustic) {
        if(selected.size()>=limit) break;
        if(!process(candidate.second)) return fail(processingError);
    }
    if(cancelled()) return fail(SimilarityWorkflowError::Cancelled);
    for (auto& item : selected) result.rows.push_back(std::move(item.row));
    result.indexed=std::max<int64_t>(0,result.indexed-result.staleFiles);
    if(result.rows.empty()) return fail(continuing && !request.shownSounds.empty() ? SimilarityWorkflowError::NoMoreCandidates :
        (result.indexed<result.totalFiles?SimilarityWorkflowError::IndexRequired:SimilarityWorkflowError::NoCandidates));
    result.queryMilliseconds=std::chrono::duration<float,std::milli>(std::chrono::steady_clock::now()-started).count();
    return result;
}
SimilarityWorkflow::Handler MakeSimilarityQueryV2Handler(const std::string& directory) {
    auto service=std::make_shared<SimilarityQueryV2>(directory);
    return [service](const SimilarityRequest& request,const SimilarityWorkflow::CancelCheck& cancel) { return service->Execute(request,cancel); };
}
} // namespace HarkenSearch
