#include "SimilarityAnalysisStore.h"
#include "LibraryStatisticsWorker.h"
#include "sqlite3.h"
#include <filesystem>
#include <iostream>
#include <limits>
using namespace HarkenSearch;
int main() {
    int failures = 0;
    auto check = [&](bool value, const char* what) { std::cout << (value ? "PASS: " : "FAIL: ") << what << '\n'; failures += !value; };
    const auto root = std::filesystem::temp_directory_path() / "harken-similarity-store-tests";
    std::filesystem::create_directories(root);
    const auto path = (root / "store.db").u8string();
    sqlite3* db = nullptr; sqlite3_open(path.c_str(), &db);
    sqlite3_exec(db, "DROP TABLE IF EXISTS audio_files; CREATE TABLE audio_files(id INTEGER PRIMARY KEY,file_path TEXT,file_name TEXT,format TEXT,duration REAL,file_size INTEGER,audio_features BLOB,features_version INTEGER); INSERT INTO audio_files VALUES(1,'a.wav','a','wav',2,100,X'1234',1),(2,'b.wav','b','wav',2,100,NULL,0);", nullptr,nullptr,nullptr);
    SimilarityAnalysisStore store;
    check(store.Open(path), "opens an existing library with additive V2 tables");
    store.Invalidate(1); store.Invalidate(2);
    SimilarityAnalysisEntry entry; entry.file = {1,"a.wav","a","wav",2};
    entry.identity = {100,123,"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    entry.acoustic.sourceSeconds = entry.acoustic.activeSeconds = 2;
    entry.acoustic.frames.resize(2);
    entry.embedding[0] = 1; entry.hasSemantic = true;
    entry.segments.push_back({0,2,entry.embedding});
    const auto before = store.Revision();
    check(store.Publish(entry), "publishes a complete acoustic and semantic record atomically");
    const auto published = store.Revision();
    check(published > before, "publication changes revision");
    SimilarityAnalysisEntry loaded;
    check(store.Load(1, loaded) && loaded.identity == entry.identity && loaded.hasSemantic && loaded.segments.size() == 1,
        "roundtrip preserves content identity and real segment bounds");
    std::vector<SimilarityAnalysisEntry> summaries;
    check(store.LoadSummaries(summaries) && summaries.size() == 1 && summaries[0].acoustic.frames.empty(),
        "warm retrieval does not load all temporal frames into memory");
    auto broken = entry; broken.segments[0].endSeconds = 3;
    check(!store.Publish(broken) && store.Revision() == published && store.Load(1,loaded),
        "incomplete or out-of-bounds semantic analysis cannot replace a complete record");
    entry.identity.sha256[0] = 'f'; entry.acoustic.mean[0] = 3;
    check(store.Publish(entry) && store.Revision() > published && store.Load(1,loaded) && loaded.acoustic.mean[0] == 3,
        "same-count content replacement invalidates caches");
    auto coverage = store.Coverage();
    check(coverage.total == 2 && coverage.acoustic == 1 && coverage.semantic == 1, "partial coverage is explicit");
    std::vector<SimilarityFile> pending;
    check(store.PendingFiles(pending,true) && pending.size()==1 && pending[0].id==2,
        "pending selection excludes complete acoustic and semantic records");
    sqlite3_exec(db,"UPDATE harken_similarity_v2 SET model_version='old-model' WHERE file_id=1",nullptr,nullptr,nullptr);
    check(store.PendingFiles(pending,true) && pending.size()==2 && store.PendingFiles(pending,false) && pending.size()==1,
        "an obsolete semantic model is pending only when semantic analysis is available");
    check(store.Publish(entry), "restores complete current analysis after model-version check");
    const auto availableCount = MakeFeatureIndexCountHandler(true, [] { return true; });
    const auto unavailableCount = MakeFeatureIndexCountHandler(true, [] { return false; });
    auto available = availableCount(path, [] { return false; });
    auto unavailable = unavailableCount(path, [] { return false; });
    check(available.success && available.total == 2 && available.indexed == 1 &&
          available.semanticIndexed == 1 && available.pending == 1,
        "statistics agree with valid indexed coverage while semantic analysis is available");
    check(unavailable.success && unavailable.indexed == 1 && unavailable.pending == 1,
        "statistics agree with valid acoustic coverage when semantic analysis is unavailable");
    sqlite3_exec(db,"UPDATE harken_similarity_v2 SET fingerprint='invalid-identity' WHERE file_id=1",nullptr,nullptr,nullptr);
    available = availableCount(path, [] { return false; });
    unavailable = unavailableCount(path, [] { return false; });
    check(available.success && available.indexed == 0 && available.semanticIndexed == 0 && available.pending == 2,
        "malformed identity remains pending with semantic analysis available");
    check(unavailable.success && unavailable.indexed == 0 && unavailable.pending == 2,
        "malformed identity remains pending with semantic analysis unavailable");
    check(store.Publish(entry), "restores valid identity after malformed-identity check");
    sqlite3_exec(db,"UPDATE harken_similarity_v2 SET summary=X'00' WHERE file_id=1",nullptr,nullptr,nullptr);
    check(store.PendingFiles(pending,false) && pending.size()==2, "malformed acoustic records are included in pending work");
    available = availableCount(path, [] { return false; });
    unavailable = unavailableCount(path, [] { return false; });
    check(available.success && available.indexed == 0 && available.semanticIndexed == 0 && available.pending == 2,
        "malformed acoustic summary remains pending with semantic analysis available");
    check(unavailable.success && unavailable.indexed == 0 && unavailable.pending == 2,
        "malformed acoustic summary remains pending with semantic analysis unavailable");
    check(store.Publish(entry), "restores complete analysis after malformed-record check");
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db, "SELECT hex(audio_features),features_version FROM audio_files WHERE id=1", -1, &statement,nullptr);
    check(sqlite3_step(statement) == SQLITE_ROW && std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement,0))) == "1234" && sqlite3_column_int(statement,1) == 1,
        "legacy feature bytes and version remain unchanged");
    sqlite3_finalize(statement);
    sqlite3_exec(db, "UPDATE audio_files SET file_path='changed.wav' WHERE id=1", nullptr,nullptr,nullptr);
    check(!store.Load(1,loaded) && store.Coverage().acoustic == 0, "changed library identity invalidates old analysis");
    check(!store.Open(path, [] { return true; }), "cancelled open does not retain an old connection");
    store.Close(); sqlite3_close(db);
    return failures ? 1 : 0;
}
