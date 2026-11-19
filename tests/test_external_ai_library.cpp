#include "ExternalAiLibrary.h"
#include "SimilarityAnalysisStore.h"
#include "sqlite3.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace HarkenSearch;
using Json = nlohmann::json;
using Connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

int failures = 0;

void Check(bool condition, const char* description) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << description << '\n';
    failures += !condition;
}

void Exec(sqlite3* database, const char* sql) {
    if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
}

struct Fixture {
    fs::path directory;
    fs::path root;
    fs::path outside;
    fs::path path;

    explicit Fixture(const fs::path& scratch) {
        const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = scratch / ("external-ai-library-" + std::to_string(serial));
        root = directory / fs::u8path(u8"音效%_Root");
        outside = directory / fs::u8path(u8"音效%_Root-outside");
        path = directory / fs::u8path(u8"查询 #%.db");
        fs::create_directories(root / "wind");
        fs::create_directories(outside / "wind");
    }

    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }

    Connection Connect() const {
        sqlite3* raw = nullptr;
        const int result = sqlite3_open(path.u8string().c_str(), &raw);
        Connection database(raw, sqlite3_close);
        if (result != SQLITE_OK) {
            throw std::runtime_error("Could not open fixture");
        }
        return database;
    }

    void Add(sqlite3* database, int id, const fs::path& file, const char* title = "") const {
        sqlite3_stmt* raw = nullptr;
        const int prepared = sqlite3_prepare_v2(database, R"SQL(
            INSERT INTO audio_files(id,file_path,folder_path,file_name,title,duration,file_size,format)
            VALUES(?1,?2,?3,?4,?5,1.25,32,'wav')
        )SQL", -1, &raw, nullptr);
        Statement statement(raw, sqlite3_finalize);
        if (prepared != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
        sqlite3_bind_int(statement.get(), 1, id);
        sqlite3_bind_text(statement.get(), 2, file.u8string().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement.get(), 3, file.parent_path().u8string().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement.get(), 4, file.filename().u8string().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement.get(), 5, title, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
    }

    Connection Seed() const {
        auto database = Connect();
        Exec(database.get(), R"SQL(
            CREATE TABLE audio_files(
                id INTEGER PRIMARY KEY, file_path TEXT NOT NULL UNIQUE, folder_path TEXT NOT NULL,
                file_name TEXT NOT NULL, title TEXT, duration REAL, file_size INTEGER, format TEXT,
                relative_path TEXT DEFAULT 'not-a-scope', search_data TEXT DEFAULT 'hidden-derived-marker'
            );
            CREATE INDEX idx_files_folder_name ON audio_files(folder_path,file_name);
            CREATE TABLE folder_index(folder_path TEXT PRIMARY KEY,file_count INTEGER,last_updated INTEGER);
            CREATE VIRTUAL TABLE audio_files_fts USING fts5(
                file_name,file_path,folder_path,title,search_data,content='audio_files',content_rowid='id'
            );
            CREATE VIRTUAL TABLE audio_files_name_trigram USING fts5(
                file_name,folder_path,content='audio_files',content_rowid='id',tokenize='trigram'
            );
        )SQL");
        Add(database.get(), 1, root / "wind" / "wind%_.wav", "sea foam");
        Add(database.get(), 2, root / "wind" / "wind-b.wav");
        Add(database.get(), 3, root / "wind" / "wind-c.wav");
        Add(database.get(), 4, root / "rain" / "unindexed.wav");
        Add(database.get(), 5, root / "wind" / "stale.wav");
        Add(database.get(), 6, root / "wind" / "broken.wav");
        Add(database.get(), 7, root / "wind" / "wrong-size.wav");
        Add(database.get(), 8, root / fs::u8path(u8"海") / fs::u8path(u8"海风.wav"));
        Add(database.get(), 9, root / ".." / outside.filename() / "wind" / "wind-escape.wav");
        Add(database.get(), 10, root / "wind" / "bad-name.wav");
        Add(database.get(), 11, root / "rain" / "different.wav");
        Add(database.get(), 99, outside / "wind" / "wind-outside.wav");
#ifdef _WIN32
        Exec(database.get(), "UPDATE audio_files SET folder_path=replace(lower(folder_path), '/', char(92));");
#endif
        Exec(database.get(), R"SQL(
            UPDATE audio_files SET file_name='X:/private/bad-name.wav' WHERE id=10;
            INSERT INTO folder_index SELECT folder_path,count(*),0 FROM audio_files GROUP BY folder_path;
            INSERT INTO audio_files_fts(audio_files_fts) VALUES('rebuild');
            INSERT INTO audio_files_name_trigram(audio_files_name_trigram) VALUES('rebuild');
        )SQL");
        return database;
    }

    void Cache(int id, const fs::path& file, bool semantic = false, bool dissimilar = false) const {
        SimilarityAnalysisStore store;
        if (!store.Open(path.u8string())) {
            throw std::runtime_error("Could not initialize fixture analysis");
        }
        SimilarityAnalysisEntry entry;
        entry.file = {id, file.u8string(), file.filename().u8string(), "wav", 1.25f};
        entry.identity = {32, 0, std::string(64, static_cast<char>('0' + id % 10))};
        entry.acoustic.sourceSeconds = entry.acoustic.activeSeconds = 1.25f;
        entry.acoustic.frames.resize(2);
        if (semantic) {
            entry.hasSemantic = true;
            entry.embedding[0] = dissimilar ? -1.0f : 1.0f;
            entry.segments.push_back({0, 1.25f, entry.embedding});
        }
        if (dissimilar) {
            entry.acoustic.mean.fill(40000.0f);
            entry.acoustic.envelope.fill(1.0f);
            entry.acoustic.attack = entry.acoustic.decay = 1.0f;
        }
        if (!store.Publish(entry)) {
            throw std::runtime_error("Could not publish fixture analysis");
        }
    }

    using Snapshot = std::map<std::string, std::pair<fs::file_time_type, std::string>>;

    Snapshot State() const {
        Snapshot result;
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::ifstream input(entry.path(), std::ios::binary);
            if (!input) {
                throw std::runtime_error("Could not read fixture snapshot");
            }
            result.emplace(entry.path().lexically_relative(directory).generic_u8string(),
                std::make_pair(entry.last_write_time(),
                    std::string(std::istreambuf_iterator<char>(input), {})));
        }
        return result;
    }
};

bool Status(const Json& payload, const char* status) {
    return payload.is_object() && payload.size() == 2 && payload.value("status", "") == status &&
        payload.contains("results") && payload["results"].is_array();
}

bool Empty(const Json& payload, const char* status) {
    return Status(payload, status) && payload["results"].empty();
}

std::vector<std::int64_t> Ids(const Json& payload) {
    std::vector<std::int64_t> result;
    if (Status(payload, "ok")) {
        for (const auto& row : payload["results"]) {
            result.push_back(row.value("file_id", std::int64_t{0}));
        }
    }
    return result;
}

void TestReadOnlySearch(const fs::path& scratch) {
    Fixture fixture(scratch);
    fixture.Seed();
    const auto before = fixture.State();
    {
        ExternalAiLibrary library;
        std::string error;
        Check(library.Open(fixture.path.u8string(), fixture.root.u8string(), error) && error.empty(),
            "opens explicit local rollback database with Unicode and URI punctuation");
        const auto matches = library.SearchAudio("wind", 50);
        Check(Ids(matches) == std::vector<std::int64_t>({1, 2, 3}),
            "search stays in the exact root and excludes traversal rows");
        Check(Ids(library.SearchAudio("wind", 2)) == std::vector<std::int64_t>({1, 2}),
            "search is bounded and deterministic");
        const auto literal = library.SearchAudio("%_", 20);
        const Json literalRow = {{"file_id", 1}, {"name", "wind%_.wav"},
            {"relative_path", "wind/wind%_.wav"}, {"duration_seconds", 1.25}};
        Check(Ids(literal) == std::vector<std::int64_t>({1}) && literal["results"][0] == literalRow,
            "search treats wildcard characters literally and returns only allowed fields");
        Check(Ids(library.SearchAudio("sea", 20)) == std::vector<std::int64_t>({1}),
            "search uses indexed metadata as well as names");
        Check(Empty(library.SearchAudio("external-ai-library", 20), "ok") &&
            Empty(library.SearchAudio("hidden-derived-marker", 20), "ok") &&
            Empty(library.SearchAudio("private", 20), "ok"),
            "indexed text lookup does not match absolute paths, derived search text, or path-like names");
        Check(Ids(library.SearchAudio(u8"海风", 20)) == std::vector<std::int64_t>({8}),
            "search handles short Unicode names");
        Check(Empty(library.SearchAudio("' OR 1=1 --", 20), "ok"), "SQL syntax in a query is literal");
        const auto folders = library.FindFolders("wind", 10);
        const Json expected = Json::array({{{"relative_path", "wind"}, {"file_count", 7}}});
        Check(Status(folders, "ok") && folders["results"] == expected,
            "canonical folder index counts keep like-named folders outside the root separate");
        Check(matches.dump().find("private") == std::string::npos &&
            matches.dump().find(fixture.root.filename().u8string()) == std::string::npos,
            "absolute paths do not escape via a corrupted display name");
        Check(Empty(library.FindSimilar(1, 10), "index_required") &&
            Empty(library.FindSimilar(99, 10), "reference_not_found"),
            "no analysis differs from a missing or unauthorized reference");
        Check(!library.Open("", fixture.root.u8string(), error) &&
            Empty(library.SearchAudio("wind", 10), "query_failed"), "failed reopen revokes the earlier library");
        for (const auto& path : {fixture.path.filename().u8string(), std::string("file:catalog.db"),
             std::string("//not-accessed.invalid/share/catalog.db")}) {
            Check(!library.Open(path, fixture.root.u8string(), error) &&
                error.find(path) == std::string::npos, "rejects relative, URI or network DB path without echoing");
        }
        Check(!library.Open(fixture.path.u8string(), "", error), "requires an explicit audio root");
    }
    Check(fixture.State() == before, "rollback-mode queries leave all fixture bytes and times unchanged");
}

void TestShortMetadataAndLiteralSearch(const fs::path& scratch) {
    Fixture fixture(scratch);
    {
        const auto writer = fixture.Seed();
        Exec(writer.get(), R"SQL(
            DELETE FROM audio_files WHERE id=1;
            ALTER TABLE audio_files ADD COLUMN keywords TEXT;
        )SQL");
        fixture.Add(writer.get(), 1, fixture.root / "wind" / "wind.wav");
        fixture.Add(writer.get(), 12, fixture.root / "wind" / "wind% literal.wav", u8"\u8f7b\u811a\u6b65");
        fixture.Add(writer.get(), 13, fixture.root / "wind" / "001.wav", "wind% gust");
        fixture.Add(writer.get(), 14, fixture.root / fs::u8path(u8"\u811a\u6b65\u96e8") / "privacy.wav");
        Exec(writer.get(), R"SQL(
            UPDATE audio_files SET keywords=char(26292,38632,22768) WHERE id=13;
            UPDATE audio_files SET search_data=char(33050,27493,38632) WHERE id=14;
            INSERT INTO audio_files_fts(audio_files_fts) VALUES('rebuild');
            INSERT INTO audio_files_name_trigram(audio_files_name_trigram) VALUES('rebuild');
        )SQL");
    }
    ExternalAiLibrary library;
    std::string error;
    Check(library.Open(fixture.path.u8string(), fixture.root.u8string(), error),
        "opens the literal-search fixture with optional catalog metadata");
    Check(Ids(library.SearchAudio(u8"\u811a\u6b65", 10)) == std::vector<std::int64_t>({12}) &&
        Ids(library.SearchAudio(u8"\u96e8", 10)) == std::vector<std::int64_t>({13}),
        "one- and two-character Chinese queries match title or keywords, not path or derived text");
    Check(Ids(library.SearchAudio("wind%", 1)) == std::vector<std::int64_t>({12}) &&
        Ids(library.SearchAudio("wind%", 10)) == std::vector<std::int64_t>({12, 13}),
        "long indexed queries verify literal punctuation before limit, including metadata");
}

void TestFoldersWithStaleIndex(const fs::path& scratch) {
    Fixture fixture(scratch);
    {
        const auto writer = fixture.Seed();
        fixture.Add(writer.get(), 12, fixture.root / "new-valve" / "first.wav");
        fixture.Add(writer.get(), 13, fixture.root / "new-valve" / "second.wav");
        fixture.Add(writer.get(), 14, fixture.outside / "new-valve" / "outside.wav");
        Exec(writer.get(), "INSERT INTO folder_index SELECT folder_path,1,0 FROM audio_files WHERE id=14;");
    }
    const auto before = fixture.State();
    ExternalAiLibrary library;
    std::string error;
    const Json expected = Json::array({{{"relative_path", "new-valve"}, {"file_count", 2}}});
    const bool opened = library.Open(fixture.path.u8string(), fixture.root.u8string(), error);
    const auto folders = library.FindFolders("new-valve", 1);
    Check(opened && Status(folders, "ok") && folders["results"] == expected,
        "a stale folder index does not hide a new authorized folder or include outside files");
    Check(fixture.State() == before, "folder lookup does not repair the cached index on disk");
}

void TestSimilarContentDeduplication(const fs::path& scratch) {
    Fixture fixture(scratch);
    fixture.Seed().reset();
    fixture.Cache(1, fixture.root / "wind" / "wind%_.wav", true);
    fixture.Cache(2, fixture.root / "wind" / "wind-b.wav", true);
    fixture.Cache(3, fixture.root / "wind" / "wind-c.wav", true);
    fixture.Cache(4, fixture.root / "rain" / "unindexed.wav", true);
    fixture.Cache(5, fixture.root / "wind" / "stale.wav", true);
    {
        const auto writer = fixture.Connect();
        Exec(writer.get(), R"SQL(
            UPDATE harken_similarity_v2 SET fingerprint=replace(fingerprint,'1','a') WHERE file_id=1;
            UPDATE harken_similarity_v2 SET fingerprint=upper(
                (SELECT fingerprint FROM harken_similarity_v2 WHERE file_id=1)) WHERE file_id=2;
            UPDATE harken_similarity_v2 SET fingerprint=
                (SELECT fingerprint FROM harken_similarity_v2 WHERE file_id=3) WHERE file_id=4;
        )SQL");
    }
    ExternalAiLibrary library;
    std::string error;
    Check(library.Open(fixture.path.u8string(), fixture.root.u8string(), error) &&
        Ids(library.FindSimilar(1, 2)) == std::vector<std::int64_t>({3, 5}),
        "substitutes exclude copies of the reference and deduplicate candidates before limit");
}

void TestCachedSimilarity(const fs::path& scratch) {
    Fixture fixture(scratch);
    fixture.Seed();
    fixture.Cache(1, fixture.root / "wind" / "wind%_.wav", true);
    fixture.Cache(4, fixture.root / "rain" / "unindexed.wav");
    fixture.Cache(11, fixture.root / "rain" / "different.wav", true, true);
    {
        ExternalAiLibrary library;
        std::string error;
        const bool opened = library.Open(fixture.path.u8string(), fixture.root.u8string(), error);
        Check(opened && Empty(library.FindSimilar(1, 10), "no_candidates"),
            "acoustic-only and below-threshold candidates do not fill same-kind results");
        Check(Empty(library.FindSimilar(4, 10), "index_required"),
            "acoustic-only reference requires a compatible semantic index");
    }
    fixture.Cache(2, fixture.root / "wind" / "wind-b.wav", true);
    fixture.Cache(3, fixture.root / "wind" / "wind-c.wav", true);
    fixture.Cache(5, fixture.root / "wind" / "stale.wav");
    fixture.Cache(6, fixture.root / "wind" / "broken.wav");
    fixture.Cache(7, fixture.root / "wind" / "wrong-size.wav");
    fixture.Cache(99, fixture.outside / "wind" / "wind-outside.wav", true);
    {
        auto writer = fixture.Connect();
        Exec(writer.get(), R"SQL(
            UPDATE harken_similarity_v2 SET file_path='stale-identity' WHERE file_id=5;
            UPDATE harken_similarity_v2 SET summary=X'01' WHERE file_id=6;
            UPDATE harken_similarity_v2 SET file_bytes=31 WHERE file_id=7;
        )SQL");
    }
    const auto before = fixture.State();
    {
        ExternalAiLibrary library;
        std::string error;
        Check(library.Open(fixture.path.u8string(), fixture.root.u8string(), error),
            "opens a catalog with V2 cached analysis");
        const auto similar = library.FindSimilar(1, 10);
        Check(Ids(similar) == std::vector<std::int64_t>({2, 3}),
            "cached ranking excludes self, stale, malformed, and out-of-root rows");
        if (Status(similar, "ok")) {
            for (const auto& row : similar["results"]) {
                const auto points = row.value("match_score", -1.0);
                Check(row.size() == 5 && points >= 0.0 && points <= 100.0,
                    "cached similarity exposes bounded ranking points only");
            }
        }
        Check(Empty(library.FindSimilar(5, 10), "index_required"), "stale reference requires valid local analysis");
        Check(Ids(library.FindSimilar(1, 1)) == std::vector<std::int64_t>({2}), "similar results respect limit");
    }
    Check(fixture.State() == before, "similarity only reads stored analysis, with no audio files present");
}

void TestWalReadOnly(const fs::path& scratch) {
    Fixture fixture(scratch);
    {
        auto writer = fixture.Seed();
        Exec(writer.get(), "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
        fixture.Add(writer.get(), 45, fixture.root / "wal-only.wav");
        const auto interrupted = fixture.directory / "wal-no-shm.db";
        fs::copy_file(fixture.path, interrupted);
        auto wal = fixture.path;
        auto interruptedWal = interrupted;
        wal += "-wal";
        interruptedWal += "-wal";
        fs::copy_file(wal, interruptedWal);
        const auto before = fixture.State();
        {
            ExternalAiLibrary library;
            std::string error;
            Check(!library.Open(interrupted.u8string(), fixture.root.u8string(), error) && !error.empty(),
                "refuses WAL recovery when the shared-memory sidecar is missing");
        }
        Check(fixture.State() == before, "failed WAL recovery never creates or modifies a sidecar");
        fs::remove(interruptedWal);
        fs::remove(interrupted);
    }
    const auto before = fixture.State();
    {
        ExternalAiLibrary library;
        std::string error;
        const bool opened = library.Open(fixture.path.u8string(), fixture.root.u8string(), error);
        Check(opened ? Ids(library.SearchAudio("wind", 1)) == std::vector<std::int64_t>({1}) :
            !error.empty() && error.find(fixture.path.u8string()) == std::string::npos,
            "closed WAL either reads safely or fails closed without exposing the path");
    }
    Check(before.size() == 1 && fixture.State() == before, "closed WAL never creates persistent sidecars");
}

#ifdef _WIN32
decltype(&CreateFileW) originalCreateFile = nullptr;
const wchar_t* denyReadOnlyShmPath = nullptr;
int deniedShmPathOpens = 0;

HANDLE WINAPI DenyFixtureShmPathOpen(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                                   DWORD disposition, DWORD flags, HANDLE templateFile) noexcept {
    if (denyReadOnlyShmPath && name && std::wstring_view(name) == denyReadOnlyShmPath && access == GENERIC_READ) {
        ++deniedShmPathOpens;
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return originalCreateFile(name, access, share, security, disposition, flags, templateFile);
}

void TestWalShmReopensFromGuard(const fs::path& scratch) {
    Fixture fixture(scratch);
    fixture.Seed().reset();

    auto* native = sqlite3_vfs_find("win32");
    originalCreateFile = reinterpret_cast<decltype(originalCreateFile)>(native->xGetSystemCall(native, "CreateFileW"));
    if (!originalCreateFile ||
        native->xSetSystemCall(native, "CreateFileW", reinterpret_cast<sqlite3_syscall_ptr>(DenyFixtureShmPathOpen)) != SQLITE_OK) {
        throw std::runtime_error("Could not install the fixture's read-only path policy");
    }

    const auto writerPath = R"(\\?\)" + fs::path(fixture.path).make_preferred().u8string();
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2(writerPath.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    Connection writer(raw, sqlite3_close);
    if (opened != SQLITE_OK) {
        throw std::runtime_error("Could not open an independent SQLite name for the fixture writer");
    }
    Exec(writer.get(), "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
    fixture.Add(writer.get(), 45, fixture.root / "wind" / "wal-only.wav");
    Exec(writer.get(), "INSERT INTO audio_files_fts(audio_files_fts) VALUES('rebuild');");

    auto shm = fs::path(fixture.path).make_preferred();
    shm += "-shm";
    Check(fs::is_regular_file(shm), "the live writer has an existing SHM file");
    const auto before = fixture.State();
    ExternalAiLibrary library;
    std::string error;
    denyReadOnlyShmPath = shm.c_str();
    const bool searched = library.Open(fixture.path.u8string(), fixture.root.u8string(), error) &&
        Ids(library.SearchAudio("wal-only", 10)) == std::vector<std::int64_t>({45});
    denyReadOnlyShmPath = nullptr;
    Check(searched, "an active WAL remains readable when SQLite cannot reopen SHM by path");
    Check(deniedShmPathOpens == 0, "the readonly connection never asks the original syscall to open SHM by path");
    Check(fixture.State() == before, "the guarded WAL reader does not modify the catalog or sidecars");
}

void TestWalShmRejectsUnverifiedCachedMapping(const fs::path& scratch) {
    Fixture fixture(scratch);
    auto writer = fixture.Seed();
    Exec(writer.get(), "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0;");
    fixture.Add(writer.get(), 46, fixture.root / "wind" / "cached-shm.wav");
    ExternalAiLibrary library;
    std::string error;
    Check(!library.Open(fixture.path.u8string(), fixture.root.u8string(), error) && !error.empty(),
        "the reader does not trust a same-process SHM mapping with no verified shared handle");
    fixture.Add(writer.get(), 47, fixture.root / "wind" / "writer-still-active.wav");
}

void TestCatalogParentGuard(const fs::path& scratch) {
    Fixture fixture(scratch);
    fixture.Seed().reset();
    const auto writer = CreateFileW(fixture.directory.c_str(), FILE_ADD_FILE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (writer == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Could not reserve the fixture catalog directory");
    }
    std::unique_ptr<void, decltype(&CloseHandle)> reservation(writer, CloseHandle);
    ExternalAiLibrary library;
    std::string error;
    Check(!library.Open(fixture.path.u8string(), fixture.root.u8string(), error) && !error.empty(),
        "catalog access fails closed when the parent directory cannot be guarded");
    reservation.reset();
    Check(library.Open(fixture.path.u8string(), fixture.root.u8string(), error),
        "catalog access succeeds again after the directory reservation is released");
}

void TestCatalogIdentityPersistsUntilReauthorized(const fs::path& scratch) {
    Fixture fixture(scratch);
    fixture.Seed().reset();
    ExternalAiLibrary library;
    std::string error;
    Check(library.Open(fixture.path.u8string(), fixture.root.u8string(), error),
        "authorizes the initial catalog");
    const auto backup = fixture.directory / "catalog-backup.db";
    fs::rename(fixture.path, backup);
    fs::copy_file(backup, fixture.path);
    Check(Empty(library.SearchAudio("wind", 1), "query_failed"),
        "a catalog copy at the same path requires explicit reauthorization");
    Check(library.Open(fixture.path.u8string(), fixture.root.u8string(), error) &&
        Ids(library.SearchAudio("wind", 1)) == std::vector<std::int64_t>({1}),
        "explicit authorization accepts the replacement catalog");
}
#endif

void TestRootRemainsAuthorized(const fs::path& scratch) {
    Fixture fixture(scratch);
    {
        const auto writer = fixture.Seed();
        fixture.Add(writer.get(), 51, fixture.root / "linked" / "fixture-jump.wav");
        fixture.Add(writer.get(), 52, fixture.root / "unavailable" / "fixture-missing.wav");
        Exec(writer.get(), "INSERT INTO audio_files_fts(audio_files_fts) VALUES('rebuild');");
    }
    ExternalAiLibrary library;
    std::string error;
    Check(library.Open(fixture.path.u8string(), fixture.root.u8string(), error), "authorizes original fixture root");
    std::error_code linkError;
    const auto link = fixture.root / "linked";
    fs::create_directory_symlink(fixture.outside, link, linkError);
    if (!linkError) {
        Check(Empty(library.SearchAudio("fixture-jump", 10), "ok") &&
            Empty(library.FindSimilar(51, 10), "reference_not_found"),
            "a linked intermediate directory is outside the readable catalog scope");
        fs::remove(link);
    }
    Check(Ids(library.SearchAudio("fixture-missing", 10)) == std::vector<std::int64_t>({52}),
        "missing descendants retain safe catalog metadata");
    fs::remove(fixture.root / "wind");
    fs::remove(fixture.root);
    Check(Empty(library.SearchAudio("wind", 10), "query_failed"),
        "a removed authorized root is rejected by the next query");
}

void TestCandidateWindow(const fs::path& scratch) {
    Fixture fixture(scratch);
    fixture.Seed();
    fixture.Cache(1, fixture.root / "wind" / "wind%_.wav", true);
    fixture.Cache(11, fixture.root / "rain" / "different.wav", true, true);
    {
        const auto writer = fixture.Connect();
        Exec(writer.get(), R"SQL(
            WITH RECURSIVE ids(file_id) AS (
                VALUES(1000) UNION ALL SELECT file_id+1 FROM ids WHERE file_id<5094
            )
            INSERT INTO audio_files(id,file_path,folder_path,file_name,title,duration,file_size,format)
            SELECT file_id,file_path||'-'||file_id,folder_path,file_name,title,duration,file_size,format
            FROM ids CROSS JOIN audio_files WHERE audio_files.id=11;

            INSERT INTO harken_similarity_v2
            SELECT catalog.id,catalog.file_path,source.file_bytes,source.modified,source.fingerprint,
                   source.version,source.summary,source.acoustic,source.embedding,source.model_version
            FROM audio_files catalog CROSS JOIN harken_similarity_v2 source
            WHERE source.file_id=11 AND catalog.id>=1000;
        )SQL");
    }
    ExternalAiLibrary library;
    std::string error;
    Check(library.Open(fixture.path.u8string(), fixture.root.u8string(), error) &&
        Empty(library.FindSimilar(1, 10), "no_candidates"),
        "exactly 4096 below-threshold rows is an exhausted window, not a truncated search");
    {
        const auto writer = fixture.Connect();
        Exec(writer.get(), R"SQL(
            INSERT INTO audio_files(id,file_path,folder_path,file_name,title,duration,file_size,format)
            SELECT 5095,file_path||'-5095',folder_path,file_name,title,duration,file_size,format
            FROM audio_files WHERE id=11;
            INSERT INTO harken_similarity_v2
            SELECT catalog.id,catalog.file_path,source.file_bytes,source.modified,source.fingerprint,
                   source.version,source.summary,source.acoustic,source.embedding,source.model_version
            FROM audio_files catalog CROSS JOIN harken_similarity_v2 source
            WHERE source.file_id=11 AND catalog.id=5095;
        )SQL");
    }
    Check(Empty(library.FindSimilar(1, 10), "query_failed"),
        "an additional unscored row makes otherwise-empty results explicitly incomplete");
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    try {
        const auto scratch = fs::absolute(fs::u8path(argv[1]));
#ifdef _WIN32
        TestWalShmReopensFromGuard(scratch);
        TestWalShmRejectsUnverifiedCachedMapping(scratch);
#endif
        TestReadOnlySearch(scratch);
        TestShortMetadataAndLiteralSearch(scratch);
        TestFoldersWithStaleIndex(scratch);
        TestSimilarContentDeduplication(scratch);
        TestCachedSimilarity(scratch);
        TestWalReadOnly(scratch);
#ifdef _WIN32
        TestCatalogParentGuard(scratch);
        TestCatalogIdentityPersistsUntilReauthorized(scratch);
#endif
        TestRootRemainsAuthorized(scratch);
        TestCandidateWindow(scratch);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
