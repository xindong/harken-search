#include "SimilarityQueryV2.h"
#include "SimilarityAnalysisStore.h"
#include "platform/audio_file.h"
#include "sqlite3.h"
#include <filesystem>
#include <cmath>
#include <future>
#include <iostream>
using namespace HarkenSearch;
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    int failures=0;
    auto check=[&](bool value,const char* what){ std::cout<<(value?"PASS: ":"FAIL: ")<<what<<'\n'; failures+=!value; };
    const auto root=std::filesystem::temp_directory_path()/"harken-similarity-query-v2-tests"; std::filesystem::create_directories(root);
    const auto path=(root/"library.db").u8string();
    sqlite3* db=nullptr; sqlite3_open(path.c_str(),&db);
    sqlite3_exec(db,"DROP TABLE IF EXISTS audio_files; CREATE TABLE audio_files(id INTEGER PRIMARY KEY,file_path TEXT,file_name TEXT,format TEXT,duration REAL,file_size INTEGER);",nullptr,nullptr,nullptr);
    for(int id=1;id<=3;++id) {
        const auto file=(root/(std::to_string(id)+".wav")).u8string();
        std::vector<float> pcm(22050); for(size_t i=0;i<pcm.size();++i) pcm[i]=0.6f*std::sin(float(i)*6.2831853f*(id==3?900.f:440.f)/22050);
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
    std::promise<SimilarityRequest> observed; auto future=observed.get_future();
    SimilarityWorkflow workflow([&](const SimilarityRequest& input,const SimilarityWorkflow::CancelCheck&){ observed.set_value(input); SimilarityCompletion c;c.generation=input.generation;c.reference=input.reference;return c; });
    workflow.SetDatabasePath(path); workflow.SetReference(request.reference);
    SimilarityOptions options; options.mode=SimilarityMode::Substitute;options.deep=true;options.preset=SimilarityPreset::RhythmFocused;
    workflow.RequestSearch(options,17);
    const auto received=future.get();
    check(received.analysisVersion==2 && received.options.deep && received.options.mode==options.mode && received.options.preset==options.preset && received.topK==17,
        "background request owns immutable mode, depth, preset and version");
    return failures?1:0;
}
