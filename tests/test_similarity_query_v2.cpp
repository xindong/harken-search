#include "SimilarityQueryV2.h"
#include "SimilarityAnalysisStore.h"
#include "platform/audio_file.h"
#include "sqlite3.h"
#include <filesystem>
#include <cmath>
#include <future>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>
using namespace HarkenSearch;
int main(int argc,char** argv) {
    if(argc<2 || argc>3) return 2;
    int failures=0;
    auto check=[&](bool value,const char* what){ std::cout<<(value?"PASS: ":"FAIL: ")<<what<<'\n'; failures+=!value; };
    const auto root=std::filesystem::temp_directory_path()/"harken-similarity-query-v2-tests"; std::filesystem::create_directories(root);
    const auto path=(root/"library.db").u8string();
    sqlite3* db=nullptr; sqlite3_open(path.c_str(),&db);
    sqlite3_exec(db,"DROP TABLE IF EXISTS audio_files; CREATE TABLE audio_files(id INTEGER PRIMARY KEY,file_path TEXT,file_name TEXT,format TEXT,duration REAL,file_size INTEGER);",nullptr,nullptr,nullptr);
    for(int id=1;id<=3;++id) {
        const auto file=(root/(std::to_string(id)+".wav")).u8string();
        std::vector<float> pcm(22050); for(size_t i=0;i<pcm.size();++i) pcm[i]=0.6f*std::sin(float(i)*6.2831853f*(id==3?900.f:(id==2?442.f:440.f))/22050);
        Platform::AudioFile::FloatWavWriter writer; writer.Open(file,22050,1); writer.Write(pcm.data(),pcm.size()); writer.Close();
        sqlite3_stmt* s=nullptr; sqlite3_prepare_v2(db,"INSERT INTO audio_files VALUES(?,?,'arbitrary-name','wav',1,0)",-1,&s,nullptr);
        sqlite3_bind_int(s,1,id); sqlite3_bind_text(s,2,file.c_str(),-1,SQLITE_TRANSIENT); sqlite3_step(s); sqlite3_finalize(s);
    }
    sqlite3_close(db);
    SimilarityAnalysisStore store; store.Open(path);
    for(int id=1;id<=3;++id) {
        store.Invalidate(id); SimilarityAnalysisEntry e; store.GetFile(id,e.file);
        ReadSimilarityFileIdentity(e.file.path,e.identity); e.acoustic=AnalyzeSimilarityFile(e.file.path).analysis;
        // Controlled unit vectors test routing/weights only, never listening accuracy.
        e.embedding[id==2?1:0]=1; e.hasSemantic=true;
        e.segments.push_back({e.acoustic.activeStart,e.acoustic.activeStart+e.acoustic.activeSeconds,e.embedding});
        if(!store.Publish(e)) return 2;
    }
    SimilarityRequest request; request.generation=1; request.databasePath=path;
    request.reference={1,(root/"1.wav").u8string(),"reference"}; request.analysisVersion=2;
    SimilarityQueryV2 service(argv[1]);
    auto result=service.Execute(request,{});
    check(result.error==SimilarityWorkflowError::None && result.rows.size()==2 && result.rows[0].fileId==2,
        "perceptual search retains an acoustically matching candidate outside the semantic neighborhood");
    check(result.rows.size()==2 && result.rows[0].fileId!=1 && result.rows[1].fileId!=1,"reference is excluded");
    request.options.mode=SimilarityMode::Substitute;
    result=service.Execute(request,{});
    check(result.error==SimilarityWorkflowError::None && !result.rows.empty() && result.rows[0].fileId==3 && result.rows[0].breakdown.weights[0]==0.4f,
        "substitute mode uses category evidence with fixed forty-percent weight");
    request.options.deep=true; result=service.Execute(request,{});
    check(result.error==SimilarityWorkflowError::None && result.rows.size()==2 && result.options.deep,"deep search completes with detailed scoring");
    const auto cancelled=service.Execute(request,[]{return true;});
    check(cancelled.error==SimilarityWorkflowError::Cancelled && cancelled.rows.empty(),"cancelled query never publishes results");
    SimilarityQueryV2 missing("missing-model"); result=missing.Execute(request,{});
    check(result.error==SimilarityWorkflowError::ModelUnavailable,"missing model explicitly blocks substitute mode");
    request.options={}; result=missing.Execute(request,{});
    check(result.error==SimilarityWorkflowError::None && result.rows.size()==2,"acoustic search works without a local semantic runtime");
    const float beforeTimbre = result.rows.empty() ? 0 : result.rows[0].breakdown.parts[1];
    SimilarityAnalysisEntry changed; store.Load(2,changed); changed.acoustic.mean.fill(500);
    check(store.Publish(changed), "replacement analysis commits during a warm query session");
    result=service.Execute(request,{});
    float updatedTimbre = 1;
    for (const auto& row : result.rows) if (row.fileId == 2) updatedTimbre = row.breakdown.parts[1];
    check(result.error==SimilarityWorkflowError::None && updatedTimbre < beforeTimbre - 0.3f,"same-count publication refreshes a warm cache");
    sqlite3_open(path.c_str(), &db);
    for (int original : {1, 2}) {
        SimilarityAnalysisEntry alias; store.Load(original, alias);
        alias.file.id = original + 3;
        alias.file.path = (root / "." / (std::to_string(original) + ".wav")).u8string();
        sqlite3_stmt* insert = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO audio_files VALUES(?,?,'alias','wav',1,0)", -1, &insert, nullptr);
        sqlite3_bind_int64(insert, 1, alias.file.id); sqlite3_bind_text(insert, 2, alias.file.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(insert); sqlite3_finalize(insert); store.Publish(alias);
    }
    sqlite3_close(db);
    for (bool deep : {false, true}) {
        request.options.deep = deep; result = service.Execute(request, {});
        check(result.error == SimilarityWorkflowError::None && result.rows.size() == 2 &&
            std::none_of(result.rows.begin(), result.rows.end(), [](const auto& row) { return row.fileId == 4; }),
            "reference path aliases are excluded and duplicate candidate paths appear once");
    }
    // A renamed copy must not crowd out another usable sound. These files and
    // controlled embeddings test retrieval behavior, not human relevance.
    SimilarityAnalysisEntry duplicate; store.Load(3, duplicate);
    duplicate.file.id = 6; duplicate.file.path = (root / "renamed-copy.wav").u8string();
    std::filesystem::copy_file(root / "3.wav", root / "renamed-copy.wav", std::filesystem::copy_options::overwrite_existing);
    auto insertEntry = [&](SimilarityAnalysisEntry entry) {
        sqlite3_open(path.c_str(), &db); sqlite3_stmt* insert = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO audio_files VALUES(?,?,'candidate','wav',1,0)", -1, &insert, nullptr);
        sqlite3_bind_int64(insert, 1, entry.file.id); sqlite3_bind_text(insert, 2, entry.file.path.c_str(), -1, SQLITE_TRANSIENT);
        const bool inserted = sqlite3_step(insert) == SQLITE_DONE; sqlite3_finalize(insert); sqlite3_close(db);
        return inserted && store.Publish(entry);
    };
    check(insertEntry(duplicate), "renamed identical-content fixture is published");
    request.options = {}; result = service.Execute(request, {});
    check(result.error == SimilarityWorkflowError::None && result.rows.size() == 2,
        "identical content under different filenames appears only once");
    std::filesystem::remove(root / "3.wav");
    result = service.Execute(request, {});
    check(result.error == SimilarityWorkflowError::None && std::any_of(result.rows.begin(), result.rows.end(), [](const auto& row) { return row.fileId == 6; }),
        "a missing duplicate path does not hide another readable copy of the same sound");
    std::filesystem::copy_file(root / "renamed-copy.wav", root / "3.wav", std::filesystem::copy_options::overwrite_existing);
    { auto original = duplicate; original.file.id = 3; original.file.path = (root / "3.wav").u8string(); store.Publish(original); }
    for (int id : {7, 8}) {
        SimilarityAnalysisEntry entry = duplicate; entry.file.id = id;
        entry.file.path = (root / (std::to_string(id) + ".wav")).u8string();
        std::vector<float> pcm(22050);
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = 0.6f * std::sin(float(i) * 6.2831853f * (id == 7 ? 1100.0f : 900.1f) / 22050);
        Platform::AudioFile::FloatWavWriter writer; writer.Open(entry.file.path, 22050, 1); writer.Write(pcm.data(), pcm.size()); writer.Close();
        ReadSimilarityFileIdentity(entry.file.path, entry.identity); entry.acoustic = AnalyzeSimilarityFile(entry.file.path).analysis;
        entry.segments = {{entry.acoustic.activeStart, entry.acoustic.activeStart + entry.acoustic.activeSeconds, entry.embedding}};
        check(insertEntry(entry), "distinct and near-duplicate real PCM fixtures are published");
    }
    request.topK = 50; result = service.Execute(request, {});
    {
        SimilarityAnalysisEntry a,b; store.Load(3,a); store.Load(8,b); const auto score=ScoreSimilarity(a.acoustic,b.acoustic,{});
        float mean=0,frame=0,energy=0;
        for(size_t i=0;i<13;++i) mean+=std::fabs(a.acoustic.mean[i]-b.acoustic.mean[i]);
        for(size_t i=0;i<16;++i) {
            const auto& x=a.acoustic.frames[i*(a.acoustic.frames.size()-1)/15]; const auto& y=b.acoustic.frames[i*(b.acoustic.frames.size()-1)/15];
            for(size_t j=0;j<13;++j) frame+=std::fabs(x.mfcc[j]-y.mfcc[j]); energy+=std::fabs(x.energy-y.energy);
        }
        std::cout << "near-fixture T/E/R=" << score.parts[1] << '/' << score.parts[2] << '/' << score.parts[3]
            << " mean=" << mean/13 << " frame=" << frame/(16*13) << " energy=" << energy/16
            << " pitch=" << a.acoustic.pitch << '/' << b.acoustic.pitch << " onsets=" << a.acoustic.onsets.size() << '/' << b.acoustic.onsets.size() << '\n';
    }
    check(std::count_if(result.rows.begin(), result.rows.end(), [](const auto& row) { return row.fileId == 3 || row.fileId == 6 || row.fileId == 8; }) == 1 &&
        std::any_of(result.rows.begin(), result.rows.end(), [](const auto& row) { return row.fileId == 7; }),
        "a near-identical variant does not crowd a distinct similar sound out of the first batch");
    SimilarityWorkflow batches(MakeSimilarityQueryV2Handler(argv[1]));
    batches.SetDatabasePath(path); batches.SetReference(request.reference);
    auto nextBatch = [&](const SimilarityOptions& opts) {
        batches.RequestSearch(opts, 1);
        for (int attempt = 0; attempt < 5000; ++attempt) {
            batches.Poll();
            if (batches.Snapshot().status != SimilarityWorkflowStatus::Searching) return batches.Snapshot();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return batches.Snapshot();
    };
    const auto firstBatch = nextBatch({}); const auto secondBatch = nextBatch({});
    check(firstBatch.rows.size() == 1 && secondBatch.rows.size() == 1 &&
        firstBatch.rows.front().fileId != secondBatch.rows.front().fileId,
        "repeating Find advances to an unseen similar sound around the same reference");
    check(secondBatch.reference == request.reference, "next batch keeps the original reference");
    std::set<int64_t> shown; bool repeated = false;
    for (const auto& row : firstBatch.rows) shown.insert(row.fileId);
    for (const auto& row : secondBatch.rows) repeated |= !shown.insert(row.fileId).second;
    bool exhausted = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto batch = nextBatch({});
        if (batch.rows.empty()) { exhausted = true; break; }
        for (const auto& row : batch.rows) repeated |= !shown.insert(row.fileId).second;
    }
    check(exhausted && !repeated, "batches finish rather than cycling through the same results");
    batches.Invalidate(); const auto restartedBatch = nextBatch({});
    check(!firstBatch.rows.empty() && !restartedBatch.rows.empty() &&
        firstBatch.rows.front().fileId == restartedBatch.rows.front().fileId, "closing and reopening starts a new result history");
    std::promise<SimilarityRequest> observed; auto future=observed.get_future();
    SimilarityWorkflow workflow([&](const SimilarityRequest& input,const SimilarityWorkflow::CancelCheck&){ observed.set_value(input); SimilarityCompletion c;c.generation=input.generation;c.reference=input.reference;return c; });
    workflow.SetDatabasePath(path); workflow.SetReference(request.reference);
    SimilarityOptions options; options.mode=SimilarityMode::Substitute;options.deep=true;options.preset=SimilarityPreset::RhythmFocused;
    workflow.RequestSearch(options,17);
    const auto received=future.get();
    check(received.analysisVersion==2 && received.options.deep && received.options.mode==options.mode && received.options.preset==options.preset && received.topK==17,
        "background request owns immutable mode, depth, preset and version");
    SimilarityAnalysisEntry temporalSeed; store.Load(1, temporalSeed);
    for (int id = 100; id <= 500; ++id) {
        auto entry = temporalSeed; entry.file.id = id;
        entry.file.path = (root / ("temporal-" + std::to_string(id) + ".wav")).u8string();
        std::filesystem::copy_file(root / "1.wav", std::filesystem::u8path(entry.file.path), std::filesystem::copy_options::overwrite_existing);
        { std::ofstream suffix(std::filesystem::u8path(entry.file.path), std::ios::binary | std::ios::app); suffix << id; }
        ReadSimilarityFileIdentity(entry.file.path, entry.identity);
        entry.hasSemantic = false; entry.segments.clear();
        for (auto& mean : entry.acoustic.mean) mean += id == 500 ? 0.2f : float(id - 99) * 0.0001f;
        if (id != 500) for (auto& frame : entry.acoustic.frames) for (auto& value : frame.mfcc) value += float(id * 50);
        if (!insertEntry(entry)) return 2;
    }
    request.options = {}; request.topK = 50;
    const auto quick = service.Execute(request, {});
    check(quick.error == SimilarityWorkflowError::None && quick.stats.detailedComparisons == 0 &&
        std::none_of(quick.rows.begin(), quick.rows.end(), [](const auto& row) { return row.fileId == 500; }),
        "ordinary search compares a smaller summary shortlist without temporal reranking");
    request.options.deep = true; const auto deeper = service.Execute(request, {});
    check(deeper.error == SimilarityWorkflowError::None && !deeper.rows.empty() && deeper.rows.front().fileId == 500 &&
        deeper.stats.detailedComparisons > 200 && deeper.stats.visitedSegments > 0,
        "deep search retrieves a better temporal match beyond the ordinary shortlist and old top-200 cutoff");
    if (argc == 3 && std::string(argv[2]) == "--scale") {
        // Synthetic, structurally valid cache data measures capacity only. It
        // is not a million real recordings or evidence of listening quality.
        SimilarityAnalysisEntry seed; store.Load(1, seed);
        for (int id = 20000; id < 20245; ++id) {
            SimilarityAnalysisEntry entry = seed; entry.file.id = id;
            entry.file.path = (root / ("scale-" + std::to_string(id) + ".wav")).u8string();
            std::filesystem::copy_file(root / "1.wav", std::filesystem::u8path(entry.file.path), std::filesystem::copy_options::overwrite_existing);
            { std::ofstream suffix(std::filesystem::u8path(entry.file.path), std::ios::binary | std::ios::app); suffix << id; }
            ReadSimilarityFileIdentity(entry.file.path, entry.identity);
            entry.acoustic.sourceSeconds = entry.acoustic.activeSeconds = 20485; entry.acoustic.activeStart = 0;
            entry.acoustic.mean.fill(3000); entry.acoustic.spectral.fill(10000); entry.acoustic.spectral[4] = 1;
            entry.acoustic.envelope.fill(0); entry.acoustic.pitch = 8000; entry.acoustic.attack = entry.acoustic.decay = 500;
            entry.embedding = {}; entry.embedding[1] = 1; entry.segments.clear();
            for (int ordinal = 0; ordinal < 4096; ++ordinal) entry.segments.push_back({float(ordinal * 5), float(ordinal * 5 + 10), entry.embedding});
            if (!insertEntry(entry)) return 2;
            if (id % 25 == 0) std::cout << "scale-files: " << id - 19999 << std::endl;
        }
        const auto count = store.SegmentCount();
        check(count > 1000000, "capacity fixture actually contains over one million persisted segments");
        request.options = {}; request.options.deep = true; request.topK = 1;
        result = service.Execute(request, {});
        check(result.error == SimilarityWorkflowError::None && !result.rows.empty() && result.stats.visitedSegments == uint64_t(count),
            "deep search streams every segment beyond one million without rejecting the library");
        std::cout << "scale-segments=" << count << " visited=" << result.stats.visitedSegments << " query-ms=" << result.queryMilliseconds << std::endl;
    }
    return failures?1:0;
}
