#include "SimilarityIndexer.h"
#include "platform/audio_file.h"
#include "sqlite3.h"
#include <filesystem>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
using namespace HarkenSearch;
int main() {
    int failures = 0;
    auto check = [&](bool value, const char* what) { std::cout << (value ? "PASS: " : "FAIL: ") << what << '\n'; failures += !value; };
    const auto root = std::filesystem::temp_directory_path() / "harken-similarity-indexer-tests";
    std::filesystem::create_directories(root);
    const auto dbPath = (root / "library.db").u8string(), audio = (root / "tone.wav").u8string();
    auto write = [&](float frequency) {
        std::vector<float> pcm(22050); for(size_t i=0;i<pcm.size();++i) pcm[i]=0.6f*std::sin(float(i)*6.2831853f*frequency/22050);
        Platform::AudioFile::FloatWavWriter writer; writer.Open(audio,22050,1); writer.Write(pcm.data(),pcm.size()); writer.Close();
    }; write(440);
    std::filesystem::last_write_time(std::filesystem::u8path(audio),
        std::chrono::time_point_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now()));
    bool stableIdentity = true; SimilarityFileIdentity firstIdentity;
    stableIdentity &= ReadSimilarityFileIdentity(audio, firstIdentity);
    for (int i = 0; i < 32; ++i) { SimilarityFileIdentity repeated; stableIdentity &= ReadSimilarityFileIdentity(audio,repeated) && repeated == firstIdentity; }
    check(stableIdentity, "unchanged audio has a stable fingerprint at whole-second timestamps");
    auto timestampOnly = firstIdentity; ++timestampOnly.modified;
    check(timestampOnly == firstIdentity, "content identity does not depend on rounded wall-clock conversion");
    sqlite3* db = nullptr; sqlite3_open(dbPath.c_str(), &db);
    sqlite3_exec(db, "DROP TABLE IF EXISTS audio_files; CREATE TABLE audio_files(id INTEGER PRIMARY KEY,file_path TEXT,file_name TEXT,format TEXT,duration REAL,file_size INTEGER);",nullptr,nullptr,nullptr);
    sqlite3_stmt* insert=nullptr; sqlite3_prepare_v2(db,"INSERT INTO audio_files VALUES(1,?,'tone','wav',1,0),(2,'missing.wav','missing','wav',1,0)",-1,&insert,nullptr);
    sqlite3_bind_text(insert,1,audio.c_str(),-1,SQLITE_TRANSIENT); sqlite3_step(insert); sqlite3_finalize(insert); sqlite3_close(db);
    { SimilarityAnalysisStore reset; reset.Open(dbPath); for (int64_t id=1;id<=4;++id) reset.Invalidate(id); }
    const auto first=BuildSimilarityIndex(dbPath,"missing-model");
    check(first.ok && first.progress.saved==1 && first.progress.failed==1 && !first.modelError.empty(), "model absence still builds honest acoustic coverage and reports the unreadable file");
    SimilarityAnalysisStore store; store.Open(dbPath); SimilarityAnalysisEntry entry;
    check(store.Load(1,entry) && !entry.hasSemantic, "missing model never manufactures category embeddings");
    const auto oldIdentity=entry.identity;
    const auto second=BuildSimilarityIndex(dbPath,"missing-model");
    check(second.progress.total==1 && second.progress.completed==1 && second.progress.skipped==0 && second.progress.saved==0,
        "incremental restart processes only the missing file, not the already indexed audio");
    check(store.Load(1,entry) && entry.identity==oldIdentity, "incremental restart preserves the completed result");
    const auto parkedAudio=audio+".offline";
    std::filesystem::rename(std::filesystem::u8path(audio),std::filesystem::u8path(parkedAudio));
    const auto offline=BuildSimilarityIndex(dbPath,"missing-model");
    std::filesystem::rename(std::filesystem::u8path(parkedAudio),std::filesystem::u8path(audio));
    check(offline.progress.total==1 && offline.progress.failed==1 && store.Load(1,entry) && entry.identity==oldIdentity,
        "supplementing a missing file never opens or invalidates already indexed offline audio");
    const auto modified=std::filesystem::last_write_time(std::filesystem::u8path(audio));
    write(880); std::filesystem::last_write_time(std::filesystem::u8path(audio),modified);
    const auto third=BuildSimilarityIndex(dbPath,"missing-model",{},{},SimilarityIndexScope::ValidateAll);
    check(third.progress.saved==1 && store.Load(1,entry) && entry.identity.sha256!=oldIdentity.sha256,
        "same size and timestamp content change is reanalyzed");
    const auto verified=BuildSimilarityIndex(dbPath,"missing-model",{},{},SimilarityIndexScope::ValidateAll);
    check(verified.progress.total==2 && verified.progress.skipped==1 && verified.progress.saved==0,
        "explicit recheck still validates and reuses unchanged content");
    const auto revision=store.Revision();
    const auto cancelled=BuildSimilarityIndex(dbPath,"missing-model",[]{return true;});
    check(cancelled.cancelled && store.Revision()==revision, "cancellation preserves completed records without a false success");
    sqlite3_open(dbPath.c_str(), &db);
    sqlite3_exec(db,"INSERT INTO audio_files SELECT 3,file_path,'third',format,duration,file_size FROM audio_files WHERE id=1; "
                    "INSERT INTO audio_files SELECT 4,file_path,'fourth',format,duration,file_size FROM audio_files WHERE id=1;",nullptr,nullptr,nullptr);
    sqlite3_close(db);
    store.Invalidate(1);
    std::atomic<bool> stopAfterFirst{false};
    const auto interrupted=BuildSimilarityIndex(dbPath,"missing-model",[&]{return stopAfterFirst.load();},
        [&](const SimilarityIndexProgress& progress){if(progress.saved==1) stopAfterFirst=true;});
    check(interrupted.cancelled && interrupted.progress.saved==1 && store.Load(1,entry),
        "mid-batch cancellation saves the completed file");
    const auto completedIdentity=entry.identity;
    const auto resumed=BuildSimilarityIndex(dbPath,"missing-model");
    check(resumed.ok && resumed.progress.total==3 && resumed.progress.saved==2 && resumed.progress.skipped==0 &&
          store.Load(1,entry) && entry.identity==completedIdentity,
        "interrupted batch resumes only unfinished files and keeps completed analysis");
    for (int64_t id = 1; id <= 4; ++id) store.Invalidate(id);
    const auto serialRun = BuildSimilarityIndex(dbPath,"missing-model",{},{},SimilarityIndexScope::ValidateAll,1);
    SimilarityAnalysisEntry serialEntry;
    const bool serialLoaded = store.Load(1,serialEntry);
    for (int64_t id = 1; id <= 4; ++id) store.Invalidate(id);
    bool parallelSaved = false;
    const auto parallelRun = BuildSimilarityIndex(dbPath,"missing-model",{},
        [&](const SimilarityIndexProgress& progress){ if(progress.saved>0) parallelSaved=true; },
        SimilarityIndexScope::ValidateAll,4);
    check(parallelRun.ok && !parallelRun.cancelled && parallelRun.progress.total==serialRun.progress.total &&
          parallelRun.progress.saved==serialRun.progress.saved && parallelRun.progress.failed==serialRun.progress.failed &&
          parallelRun.progress.completed==serialRun.progress.completed && parallelSaved,
        "parallel workers keep the serial counters and report saved work");
    SimilarityAnalysisEntry parallelEntry;
    check(serialLoaded && store.Load(1,parallelEntry) && parallelEntry.identity==serialEntry.identity &&
          parallelEntry.acoustic.activeStart==serialEntry.acoustic.activeStart,
        "parallel workers write the same records as the serial pass");
    for (int64_t id = 1; id <= 4; ++id) store.Invalidate(id);
    std::atomic<bool> stopParallel{false};
    const auto parallelCancelled = BuildSimilarityIndex(dbPath,"missing-model",[&]{return stopParallel.load();},
        [&](const SimilarityIndexProgress& progress){if(progress.saved==1) stopParallel=true;},
        SimilarityIndexScope::ValidateAll,4);
    check(parallelCancelled.cancelled && parallelCancelled.progress.saved>=1 && store.Load(1,entry),
        "parallel cancellation keeps finished records and reports cancellation");
    {
        SimilarityAnalysisEntry oldEntry;
        const bool oldLoaded = store.Load(1,oldEntry);
        write(1100);
        const auto caller = std::this_thread::get_id();
        std::atomic<bool> callerWaiting{false};
        std::atomic<bool> cancelDuringAnalysis{false};
        bool workerPhase = false;
        int initialCallbacks = 0;
        std::atomic<int> workerChecks{0};
        const auto stopped = BuildSimilarityIndex(dbPath,"missing-model",[&] {
            if (std::this_thread::get_id() == caller) {
                const bool requested = cancelDuringAnalysis.load();
                if (workerPhase) callerWaiting = true;
                return requested;
            }
            if (++workerChecks == 1) return false;
            while (!callerWaiting) std::this_thread::yield();
            cancelDuringAnalysis = true;
            return true;
        },[&](const SimilarityIndexProgress&) { workerPhase = ++initialCallbacks >= 2; },
            SimilarityIndexScope::ValidateAll,4);
        check(oldLoaded && stopped.cancelled && stopped.progress.failed == 0 &&
              store.Load(1,entry) && entry.identity == oldEntry.identity,
            "cancelling a parallel reanalysis preserves the old index without recording a failure");
    }
    {
        write(1320);
        bool caught = false;
        try {
            BuildSimilarityIndex(dbPath,"missing-model",{},[](const SimilarityIndexProgress& progress) {
                if (progress.saved > 0) throw std::runtime_error("test progress failure");
            },SimilarityIndexScope::ValidateAll,4);
        } catch (const std::runtime_error& error) {
            caught = std::string(error.what()) == "test progress failure";
        }
        check(caught, "a throwing progress callback joins workers and propagates the original exception");
    }
    // One real semantic analysis takes hundreds of milliseconds, so the consumer must
    // wait for a slow worker instead of treating one short poll as cancellation.
    {
        const auto slowDir = root / "slow";
        std::filesystem::create_directories(slowDir);
        constexpr int kSeconds = 60;
        std::vector<std::string> slowPaths;
        std::vector<float> frames(size_t(48000) * 2 * size_t(kSeconds));
        for (int file = 0; file < 4; ++file) {
            const auto path = (slowDir / ("long-" + std::to_string(file) + ".wav")).u8string();
            for (int frame = 0; frame < 48000 * kSeconds; ++frame) {
                const float value = 0.4f * std::sin(float(frame) * 6.2831853f *
                                                    (220.0f + 110.0f * float(file)) / 48000.0f);
                frames[size_t(frame) * 2] = value;
                frames[size_t(frame) * 2 + 1] = value * 0.5f;
            }
            Platform::AudioFile::FloatWavWriter writer; writer.Open(path,48000,2);
            writer.Write(frames.data(),48000 * kSeconds); writer.Close();
            slowPaths.push_back(path);
        }
        sqlite3_open(dbPath.c_str(), &db);
        sqlite3_exec(db,"DELETE FROM audio_files;",nullptr,nullptr,nullptr);
        sqlite3_close(db);
        sqlite3_open(dbPath.c_str(), &db);
        for (size_t index = 0; index < slowPaths.size(); ++index) {
            sqlite3_stmt* row=nullptr;
            sqlite3_prepare_v2(db,"INSERT INTO audio_files VALUES(?,?,'long','wav',60,0)",-1,&row,nullptr);
            sqlite3_bind_int64(row,1,int64_t(index)+1);
            sqlite3_bind_text(row,2,slowPaths[index].c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(row); sqlite3_finalize(row);
        }
        sqlite3_close(db);
        SimilarityAnalysisStore slowStore; slowStore.Open(dbPath);
        for (int64_t id = 1; id <= 4; ++id) slowStore.Invalidate(id);
        const auto slowParallel = BuildSimilarityIndex(dbPath,"missing-model",{},{},
            SimilarityIndexScope::ValidateAll,4);
        check(slowParallel.ok && !slowParallel.cancelled && slowParallel.progress.completed==4 &&
              slowParallel.progress.saved==4,
            "parallel workers wait for slow analyses instead of reporting a false cancellation");
        check(!slowParallel.modelError.empty() && slowParallel.peakConcurrentAnalyses > 1 &&
              slowParallel.peakConcurrentAnalyses <= 4,
            "acoustic-only analysis honors the worker limit without an ONNX model");
    }
    return failures ? 1 : 0;
}
