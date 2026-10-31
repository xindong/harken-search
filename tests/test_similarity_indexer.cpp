#include "SimilarityIndexer.h"
#include "platform/audio_file.h"
#include "sqlite3.h"
#include <filesystem>
#include <cmath>
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
    { SimilarityAnalysisStore reset; reset.Open(dbPath); reset.Invalidate(1); reset.Invalidate(2); }
    const auto first=BuildSimilarityIndex(dbPath,"missing-model");
    check(first.ok && first.progress.saved==1 && first.progress.failed==1 && !first.modelError.empty(), "model absence still builds honest acoustic coverage and reports the unreadable file");
    SimilarityAnalysisStore store; store.Open(dbPath); SimilarityAnalysisEntry entry;
    check(store.Load(1,entry) && !entry.hasSemantic, "missing model never manufactures category embeddings");
    const auto oldIdentity=entry.identity;
    const auto second=BuildSimilarityIndex(dbPath,"missing-model");
    check(second.progress.skipped==1 && second.progress.saved==0, "restart skips already completed unchanged files");
    const auto modified=std::filesystem::last_write_time(std::filesystem::u8path(audio));
    write(880); std::filesystem::last_write_time(std::filesystem::u8path(audio),modified);
    const auto third=BuildSimilarityIndex(dbPath,"missing-model");
    check(third.progress.saved==1 && store.Load(1,entry) && entry.identity.sha256!=oldIdentity.sha256,
        "same size and timestamp content change is reanalyzed");
    const auto revision=store.Revision();
    const auto cancelled=BuildSimilarityIndex(dbPath,"missing-model",[]{return true;});
    check(cancelled.cancelled && store.Revision()==revision, "cancellation preserves completed records without a false success");
    return failures ? 1 : 0;
}
