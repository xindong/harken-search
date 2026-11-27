#include "Database.h"
#include "SearchTextIndex.h"
#include "platform/harken_user_data_layout.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kFixtureRoot =
    "D:/Aris-QA/test-databases/RQ-20261010-NONDOCK-PREVIEW-pinyin";

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

bool Expect(bool condition, const char* message) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << std::endl;
    return condition;
}

std::string Scalar(const Database::SqlResult& result) {
    if (!result.success || result.rows.empty() || result.rows[0].empty()) return {};
    return result.rows[0][0];
}

int ScalarInt(const Database::SqlResult& result) {
    const std::string text = Scalar(result);
    if (text.empty()) return 0;
    try { return std::stoi(text); } catch (...) { return 0; }
}

void TouchFile(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    const char bytes[] = "RIFF....WAVEfmt ";
    out.write(bytes, sizeof(bytes) - 1);
}

std::string SqlQuote(const std::string& value) {
    std::string out(1, char(39));
    for (char ch : value) {
        if (ch == 39) { out += char(39); out += char(39); }
        else out += ch;
    }
    out += char(39);
    return out;
}

std::string FileFingerprint(const fs::path& dbPath) {
    std::string out;
    const fs::path extras[] = {
        dbPath,
        fs::path(dbPath.native() + L"-wal"),
        fs::path(dbPath.native() + L"-shm")
    };
    for (const auto& p : extras) {
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        const auto sz = fs::file_size(p, ec);
        out += p.filename().u8string() + ":" + std::to_string(sz) + ";";
        std::ifstream in(p, std::ios::binary);
        std::vector<char> buf(4096);
        unsigned long long h = 1469598103934665603ull;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = in.gcount();
            for (std::streamsize i = 0; i < n; ++i) {
                h ^= static_cast<unsigned char>(buf[static_cast<size_t>(i)]);
                h *= 1099511628211ull;
            }
        }
        out += std::to_string(h) + "|";
    }
    return out;
}

bool SeedLegacyLibrary(const fs::path& dbPath, const fs::path& audioDir, int extraRows) {
    fs::create_directories(audioDir);
    Database db;
    if (!db.open(dbPath.u8string(), false)) {
        std::cout << "FAIL: could not create library " << dbPath.u8string()
                  << " err=" << db.getLastError() << std::endl;
        return false;
    }
    db.stopCoverPhashBackfillWorker();
    struct Row { const char* file; const char* name; const char* token; };
    const Row rows[] = {
        {"car.wav", "\xE6\xB1\xBD\xE8\xBD\xA6.wav", "oldtoken"},
        {"engine.wav", "\xE5\xBC\x95\xE6\x93\x8E.wav", "legacy"},
        {"whoosh.wav", "whoosh.wav", "whoosh"},
        {"laugh.wav", "\xE5\xA4\xA7\xE7\xAC\x91.wav", "oldlaugh"},
        {"repeat.wav", "\xE9\x87\x8D\xE5\xA4\x8D.wav", "oldrepeat"},
    };
    db.executeRawQuery("BEGIN;");
    for (const auto& row : rows) {
        const fs::path file = audioDir / row.file;
        TouchFile(file);
        const std::string sql =
            "INSERT INTO audio_files(file_path, file_name, search_data) VALUES(" +
            SqlQuote(file.u8string()) + ", " + SqlQuote(row.name) + ", " + SqlQuote(row.token) + ");";
        db.executeRawQuery(sql.c_str());
    }
    for (int i = 0; i < extraRows; ++i) {
        const fs::path file = audioDir / ("extra-" + std::to_string(i) + ".wav");
        TouchFile(file);
        const std::string name = std::string("\xE5\xA1\xAB\xE5\x85\x85") + std::to_string(i) + ".wav";
        const char* token = (i + 1 == extraRows) ? "keepoldtoken" : "oldpad";
        const std::string sql =
            "INSERT INTO audio_files(file_path, file_name, search_data) VALUES(" +
            SqlQuote(file.u8string()) + ", " + SqlQuote(name) + ", " + SqlQuote(token) + ");";
        db.executeRawQuery(sql.c_str());
    }
    db.executeRawQuery((std::string("INSERT INTO app_meta(key, value) VALUES(") + SqlQuote("search_data_migrated") +
                       ", " + SqlQuote("1") + ") ON CONFLICT(key) DO UPDATE SET value = " + SqlQuote("1") + ";").c_str());
    db.executeRawQuery((std::string("DELETE FROM app_meta WHERE key = ") + SqlQuote("search_data_recompute_cursor") + ";").c_str());
    db.executeRawQuery("COMMIT;");
    db.close();
    return true;
}

int MatchCount(Database& db, const char* token) {
    const auto result = db.executeRawQuery(
        (std::string("SELECT COUNT(*) FROM audio_files_fts WHERE audio_files_fts MATCH ") + SqlQuote(token) + ";").c_str());
    return ScalarInt(result);
}

std::string MetaValue(Database& db, const char* key) {
    const auto result = db.executeRawQuery(
        (std::string("SELECT value FROM app_meta WHERE key = ") + SqlQuote(key) + ";").c_str());
    return Scalar(result);
}

int WorkCount(Database& db) {
    const auto result = db.executeRawQuery("SELECT COUNT(*) FROM temp.search_data_recompute_work;");
    return ScalarInt(result);
}

int TotalRows(Database& db) {
    return ScalarInt(db.executeRawQuery("SELECT COUNT(*) FROM audio_files;"));
}

bool WaitRecomputeStarted(Database& db, std::atomic<float>* progress, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (progress && progress->load() >= 0.049f) return true;
        if (ScalarInt(db.executeRawQuery((std::string("SELECT value FROM app_meta WHERE key = ") +
                                         SqlQuote("search_data_recompute_cursor") + ";").c_str())) > 0) return true;
        if (WorkCount(db) > 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool WaitMigrated(Database& db, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (MetaValue(db, "search_data_migrated") == "3") return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void AppendVorbisLe32(std::string& bytes, uint32_t value) {
    bytes.push_back(static_cast<char>(value & 0xFF));
    bytes.push_back(static_cast<char>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<char>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<char>((value >> 24) & 0xFF));
}

std::string BuildVorbisFlacFixture(const std::vector<std::string>& entries) {
    std::string comment;
    const std::string vendor = "Harken Search test";
    AppendVorbisLe32(comment, static_cast<uint32_t>(vendor.size()));
    comment += vendor;
    AppendVorbisLe32(comment, static_cast<uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        AppendVorbisLe32(comment, static_cast<uint32_t>(entry.size()));
        comment += entry;
    }

    auto block = [](int type, bool last, const std::string& payload) {
        std::string bytes;
        bytes.push_back(static_cast<char>((last ? 0x80 : 0x00) | (type & 0x7F)));
        const uint32_t size = static_cast<uint32_t>(payload.size());
        bytes.push_back(static_cast<char>((size >> 16) & 0xFF));
        bytes.push_back(static_cast<char>((size >> 8) & 0xFF));
        bytes.push_back(static_cast<char>(size & 0xFF));
        bytes += payload;
        return bytes;
    };

    std::string bytes = "fLaC";
    bytes += block(0, false, std::string(34, '\0'));
    bytes += block(4, true, comment);
    bytes += std::string(64, '\0');
    return bytes;
}

bool InstallWriteFailTrigger(Database& db) {
    db.executeRawQuery("CREATE TABLE IF NOT EXISTS recompute_write_attempts(n INTEGER);");
    db.executeRawQuery("DELETE FROM recompute_write_attempts;");
    db.executeRawQuery("INSERT INTO recompute_write_attempts(n) VALUES(0);");
    db.executeRawQuery("DROP TRIGGER IF EXISTS abort_search_data_update;");
    const auto created = db.executeRawQuery(
        "CREATE TRIGGER abort_search_data_update BEFORE UPDATE OF search_data ON audio_files "
        "BEGIN UPDATE recompute_write_attempts SET n = n + 1; "
        "SELECT RAISE(FAIL, 'blocked search_data write'); END;");
    if (!created.success) {
        std::cout << "FAIL: could not install write-fail trigger err=" << created.error << std::endl;
        return false;
    }
    return true;
}

int WriteFailAttempts(Database& db) {
    return ScalarInt(db.executeRawQuery("SELECT n FROM recompute_write_attempts;"));
}

bool InstallFtsRebuildFailTrigger(Database& db, std::string* used) {
    const char* tables[] = {"audio_files_fts_data", "audio_files_fts_idx", "audio_files_fts"};
    for (const char* table : tables) {
        db.executeRawQuery("DROP TRIGGER IF EXISTS abort_fts_rebuild;");
        const std::string sql = std::string("CREATE TRIGGER abort_fts_rebuild BEFORE INSERT ON ") + table +
            " BEGIN SELECT RAISE(ABORT, 'blocked fts rebuild'); END;";
        const auto created = db.executeRawQuery(sql.c_str());
        if (created.success) {
            *used = table;
            return true;
        }
        std::cout << "NOTE: FTS fail trigger on " << table << " err=" << created.error << std::endl;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    bool ok = true;
    const std::string isolationRoot = Platform::HarkenUserData::PreviewIsolationRootUtf8();
    std::cout << "ISOLATION_ROOT=" << isolationRoot << std::endl;
    ok &= Expect(isolationRoot.empty() || isolationRoot == kFixtureRoot,
                 "optional compile-time isolation root is the card test-databases directory");

    const fs::path fixtureBase = isolationRoot.empty() ? fs::temp_directory_path() : fs::u8path(isolationRoot);
    const fs::path root = fixtureBase /
        ("bug07-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const fs::path audioDir = root / "audio";

    if (argc == 2 && std::string(argv[1]) == "--mp3-search") {
        Database database;
        if (!Expect(database.open((root / "mp3.db").u8string(), false), "open an isolated MP3 library")) return 1;
        database.stopCoverPhashBackfillWorker();
        AudioRecord record;
        record.filePath = (audioDir / "Tone.mp3").u8string();
        record.fileName = "Tone.mp3";
        record.extension = ".mp3";
        record.title = "Library title";
        record.meta = {{"composer", "Azurescore"}, {"bpm", "187"},
                       {"musical_key", "Dorian"}, {"track_year", "2034"}};
        record.id3TextTags = {{"Original 名称", "UniqueValue"}, {"Other 英文", "SecondValue"}};
        ok &= Expect(database.addFile(record), "add the MP3 record");
        SearchFilter filter;
        for (const char* value : {"Azurescore", "187", "Dorian", "2034", "UniqueValue"}) {
            filter.keyword = value;
            const auto results = database.searchFiles(filter, 10);
            ok &= Expect(results.size() == 1 && results.front().matchSource == SearchMatchSource::Metadata, value);
        }
        const auto custom = Database::findOptionalMetadataMatches(database.getFileByPath(record.filePath), "UniqueValue");
        ok &= Expect(custom.size() == 1 && custom.front().custom &&
            custom.front().field == "Original 名称" && custom.front().value == "UniqueValue",
            "custom tag stores the original field and explains the actual matching value");
        filter.keyword = "UniqueValue";
        filter.exactMode = true;
        ok &= Expect(database.searchFiles(filter, 10).empty(), "precise filename search does not gain custom-only results");
        filter.keyword = "Tone";
        filter.exactMode = true;
        ok &= Expect(database.searchFiles(filter, 10).size() == 1, "precise filename still matches");
        record.title = "Embedded title";
        record.meta["composer"] = "Embedded composer";
        record.keywords = "newly indexed";
        ok &= Expect(database.addFile(record), "reread existing MP3");
        const auto reread = database.getFileByPath(record.filePath);
        ok &= Expect(reread.title == "Library title" &&
            reread.getMeta("composer") == "Azurescore" && reread.keywords == "newly indexed",
            "reread adds missing metadata without replacing library edits");
        MetadataEditPatch edit;
        edit.fields["composer"] = "Borealscore";
        ok &= Expect(database.applyMetadataPatch(reread.id, edit), "update a library music tag");
        filter.exactMode = false;
        filter.keyword = "Borealscore";
        ok &= Expect(database.searchFiles(filter, 10).size() == 1, "edited composer immediately searchable");
        filter.keyword = "Azurescore";
        ok &= Expect(database.searchFiles(filter, 10).empty(), "edited composer no longer matches old tags");

        // A harken_meta written by the metadata patch must be searchable in the
        // same statement's search_data rebuild even though SQL SET expressions
        // read the pre-update row.
        MetadataEditPatch harkenPatch;
        harkenPatch.set("harken_meta",
                        R"({"category":"Weapons","function":"Impact","source":"manual"})");
        ok &= Expect(database.applyMetadataPatch(reread.id, harkenPatch), "update a Harken classification");
        for (const char* value : {"Weapons", "Impact"}) {
            filter.keyword = value;
            const auto results = database.searchFiles(filter, 10);
            const std::string caseName = std::string("Harken value immediately searchable: ") + value;
            ok &= Expect(results.size() == 1 && results.front().fileName == "Tone.mp3", caseName.c_str());
        }
        // A legacy library gets the column as '' (TEXT DEFAULT ''); reading it
        // must not fail the search on malformed JSON.
        database.executeRawQuery("UPDATE audio_files SET harken_meta = '';");
        filter.keyword = "Tone";
        ok &= Expect(database.searchFiles(filter, 10).size() == 1, "empty harken_meta still searches");
        database.executeRawQuery("UPDATE audio_files SET harken_meta = 'not json';");
        ok &= Expect(database.searchFiles(filter, 10).size() == 1, "damaged harken_meta still searches");
        database.executeRawQuery(
            "UPDATE audio_files SET harken_meta = '{\"category\":\"Weapons\",\"function\":\"Impact\",\"source\":\"manual\"}';");

        database.executeRawQuery("UPDATE audio_files SET search_data = 'Tone';");
        database.executeRawQuery(
            "INSERT INTO app_meta(key, value) VALUES('search_data_migrated', '2') "
            "ON CONFLICT(key) DO UPDATE SET value = '2';");
        ok &= Expect(MatchCount(database, "Dorian") == 0, "version 2 fixture lacks the key in the search index");
        database.close();
        if (!Expect(database.open((root / "mp3.db").u8string(), true), "reopen the version 2 library")) return 1;
        ok &= Expect(WaitMigrated(database, 5000), "opening the version 2 library refreshes the index in the background");
        for (const char* value : {"187", "Dorian", "2034"}) {
            filter.keyword = value;
            ok &= Expect(MatchCount(database, value) == 1 && database.searchFiles(filter, 10).size() == 1, value);
        }
        return ok ? 0 : 1;
    }

    if (argc == 2 && std::string(argv[1]) == "--flac-search") {
        const fs::path flacPath = root / "Synthetic.flac";
        {
            std::ofstream out(flacPath, std::ios::binary | std::ios::trunc);
            const std::string bytes = BuildVorbisFlacFixture({
                "TITLE=Embedded title",
                "COMPOSER=Azurescore",
                "BPM=181",
                "INITIALKEY=Dorian",
                "DATE=2034-05-01",
                "VENDOR LABEL=UniqueValue",
            });
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        Database database;
        if (!Expect(database.open((root / "flac.db").u8string(), false), "open an isolated FLAC library")) return 1;
        database.stopCoverPhashBackfillWorker();
        AudioRecord record;
        record.filePath = flacPath.u8string();
        record.fileName = "Synthetic.flac";
        record.extension = ".flac";
        record.title = "Library title";
        // The pre-Vorbis-parser build stored the year but nothing else, which is
        // exactly the row shape the one-time backfill must still pick up.
        record.meta = {{"track_year", "2034"}};
        ok &= Expect(database.addFile(record), "add a FLAC row with only the legacy year");
        database.close();

        if (!Expect(database.open((root / "flac.db").u8string(), true), "reopen the FLAC library")) return 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        AudioRecord refreshed;
        while (std::chrono::steady_clock::now() < deadline) {
            refreshed = database.getFileByPath(record.filePath);
            if (!refreshed.getMeta("composer").empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ok &= Expect(refreshed.getMeta("composer") == "Azurescore", "legacy FLAC row gains composer in the background");
        const auto bpmValue = Scalar(database.executeRawQuery(
            "SELECT bpm FROM audio_files WHERE id = 1;"));
        ok &= Expect(bpmValue == "181", "legacy FLAC row gains BPM");
        ok &= Expect(refreshed.getMeta("musical_key") == "Dorian", "legacy FLAC row gains key");
        ok &= Expect(refreshed.getMeta("track_year") == "2034", "legacy FLAC row keeps its year");
        ok &= Expect(refreshed.title == "Library title", "legacy backfill keeps the library title");
        ok &= Expect(refreshed.id3TextTags.count("VENDOR LABEL") == 1,
                     "legacy FLAC row gains its custom Vorbis tag");
        ok &= Expect(MetaValue(database, "legacy_vorbis_metadata_refresh") == "1",
                     "legacy FLAC backfill records that it finished");

        SearchFilter filter;
        for (const char* value : {"Azurescore", "181", "Dorian", "2034", "UniqueValue"}) {
            filter.keyword = value;
            const auto results = database.searchFiles(filter, 10);
            ok &= Expect(results.size() == 1 && results.front().matchSource == SearchMatchSource::Metadata, value);
        }
        const auto custom = Database::findOptionalMetadataMatches(
            database.getFileByPath(record.filePath), "UniqueValue");
        ok &= Expect(custom.size() == 1 && custom.front().custom &&
                         custom.front().field == "VENDOR LABEL" &&
                         custom.front().value == "UniqueValue",
                     "custom Vorbis tag explains the actual matching field");
        database.close();

        // A row the user already edited keeps that value while the columns that
        // are still empty are filled from the file.
        const fs::path partialPath = root / "Partial.flac";
        {
            std::ofstream out(partialPath, std::ios::binary | std::ios::trunc);
            const std::string bytes = BuildVorbisFlacFixture({
                "COMPOSER=FileComposer",
                "BPM=145",
                "INITIALKEY=Amin",
                "DATE=2019-02-03",
                "VENDOR LABEL=PartialValue",
            });
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        {
            Database partial;
            if (!Expect(partial.open((root / "flac-partial.db").u8string(), false),
                        "open the partially filled FLAC library")) return 1;
            partial.stopCoverPhashBackfillWorker();
            AudioRecord manual;
            manual.filePath = partialPath.u8string();
            manual.fileName = "Partial.flac";
            manual.extension = ".flac";
            manual.title = "Manual title";
            manual.meta = {{"composer", "ManualComposer"}};
            ok &= Expect(partial.addFile(manual), "add a FLAC row whose composer was already set");
            partial.close();
        }
        {
            Database partial;
            if (!Expect(partial.open((root / "flac-partial.db").u8string(), true),
                        "reopen the partially filled FLAC library")) return 1;
            const auto partialDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            AudioRecord partialRecord;
            while (std::chrono::steady_clock::now() < partialDeadline) {
                partialRecord = partial.getFileByPath(partialPath.u8string());
                if (!partialRecord.getMeta("bpm").empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ok &= Expect(partialRecord.getMeta("composer") == "ManualComposer",
                         "backfill keeps the composer the user already set");
            ok &= Expect(Scalar(partial.executeRawQuery(
                             "SELECT bpm FROM audio_files WHERE file_name = 'Partial.flac';")) == "145",
                         "backfill still fills BPM on a partially filled row");
            ok &= Expect(partialRecord.getMeta("musical_key") == "Amin",
                         "backfill still fills the key on a partially filled row");
            ok &= Expect(partialRecord.getMeta("track_year") == "2019",
                         "backfill still fills the year on a partially filled row");
            ok &= Expect(partialRecord.id3TextTags.count("VENDOR LABEL") == 1,
                         "backfill still adds custom tags on a partially filled row");
            SearchFilter partialFilter;
            partialFilter.keyword = "PartialValue";
            ok &= Expect(partial.searchFiles(partialFilter, 10).size() == 1,
                         "the partially filled row becomes searchable by its new tag");
            partial.close();
        }

        // A failed write keeps the row for the next pass and never records
        // completion, so the one-time marker cannot hide unfilled rows.
        const fs::path retryPath = root / "Retry.flac";
        {
            std::ofstream out(retryPath, std::ios::binary | std::ios::trunc);
            const std::string bytes = BuildVorbisFlacFixture({
                "COMPOSER=RetryComposer",
                "BPM=133",
            });
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        {
            Database retry;
            if (!Expect(retry.open((root / "flac-retry.db").u8string(), false),
                        "open the FLAC library used for the retry case")) return 1;
            retry.stopCoverPhashBackfillWorker();
            AudioRecord pending;
            pending.filePath = retryPath.u8string();
            pending.fileName = "Retry.flac";
            pending.extension = ".flac";
            pending.title = "Retry title";
            ok &= Expect(retry.addFile(pending), "add the row that will fail its first backfill write");
            const int64_t pendingId = retry.getFileByPath(pending.filePath).id;
            retry.executeRawQuery(
                "DELETE FROM app_meta WHERE key IN ('legacy_vorbis_metadata_refresh',"
                " 'legacy_vorbis_metadata_refresh_cursor');");
            ok &= Expect(InstallWriteFailTrigger(retry), "install the write-fail trigger");
            ok &= Expect(retry.refreshLegacyVorbisMetadataBatch(16) == -1,
                         "a failed backfill write stops the batch");
            ok &= Expect(retry.getFileById(pendingId).getMeta("composer").empty(),
                         "the failed row is left untouched");
            ok &= Expect(MetaValue(retry, "legacy_vorbis_metadata_refresh") != "1",
                         "a failed batch never records completion");
            retry.executeRawQuery("DROP TRIGGER IF EXISTS abort_search_data_update;");
            retry.close();
        }
        {
            // Only a primary connection starts the worker, so reopen as the app
            // does: the pass resumes from the row that failed instead of
            // skipping it.
            Database retry;
            if (!Expect(retry.open((root / "flac-retry.db").u8string(), true),
                        "reopen the FLAC library for the retry pass")) return 1;
            const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            std::string marker;
            AudioRecord retriedRecord;
            while (std::chrono::steady_clock::now() < retryDeadline) {
                marker = MetaValue(retry, "legacy_vorbis_metadata_refresh");
                retriedRecord = retry.getFileByPath(retryPath.u8string());
                if (marker == "1" && !retriedRecord.getMeta("composer").empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ok &= Expect(marker == "1",
                         "the next pass retries the failed row and finishes the backfill");
            ok &= Expect(retriedRecord.getMeta("composer") == "RetryComposer",
                         "the retried row gains its composer");
            ok &= Expect(retry.refreshLegacyVorbisMetadataBatch(16) == -1,
                         "a finished pass does not run again");
            // The fill-only guard: a value the user saved after the file was read
            // is never replaced by the backfill copy.
            const int64_t pendingId = retry.getFileByPath(retryPath.u8string()).id;
            MetadataEditPatch manualComposer;
            manualComposer.fields["composer"] = "LateUserEdit";
            ok &= Expect(retry.applyMetadataPatch(pendingId, manualComposer),
                         "save a user composer after the backfill finished");
            ok &= Expect(retry.fillLegacyVorbisColumns(pendingId, "FileComposer", "", "", "", nullptr),
                         "a fill-only write on an already filled column succeeds as a no-op");
            ok &= Expect(retry.getFileById(pendingId).getMeta("composer") == "LateUserEdit",
                         "the fill-only write keeps the user value");
            retry.close();
        }

        // Cancelling (closing the library) stops the pass between files and
        // leaves the row for the next open instead of skipping it.
        const fs::path cancelPath = root / "Cancel.flac";
        {
            std::ofstream out(cancelPath, std::ios::binary | std::ios::trunc);
            const std::string bytes = BuildVorbisFlacFixture({"COMPOSER=CancelComposer"});
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        {
            Database cancelled;
            if (!Expect(cancelled.open((root / "flac-cancel.db").u8string(), false),
                        "open the FLAC library used for the cancel case")) return 1;
            cancelled.stopCoverPhashBackfillWorker();
            AudioRecord pending;
            pending.filePath = cancelPath.u8string();
            pending.fileName = "Cancel.flac";
            pending.extension = ".flac";
            pending.title = "Cancel title";
            ok &= Expect(cancelled.addFile(pending), "add the row left for the cancelled pass");
            const int64_t pendingId = cancelled.getFileByPath(pending.filePath).id;
            cancelled.executeRawQuery(
                "DELETE FROM app_meta WHERE key IN ('legacy_vorbis_metadata_refresh',"
                " 'legacy_vorbis_metadata_refresh_cursor');");
            ok &= Expect(cancelled.refreshLegacyVorbisMetadataBatch(16) == -1,
                         "a cancelled pass stops before reading the next file");
            ok &= Expect(cancelled.getFileById(pendingId).getMeta("composer").empty(),
                         "a cancelled pass writes nothing");
            ok &= Expect(MetaValue(cancelled, "legacy_vorbis_metadata_refresh") != "1",
                         "a cancelled pass does not record completion");
            cancelled.close();
        }
        return ok ? 0 : 1;
    }

    {
        const fs::path dbPath = root / "product-small.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "small", 0)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) {
            std::cout << "FAIL: reopen product-small" << std::endl;
            return 1;
        }
        db.stopCoverPhashBackfillWorker();
        db.executeRawQuery((std::string("INSERT INTO app_meta(key, value) VALUES(") + SqlQuote("search_data_migrated") +
                           ", " + SqlQuote("1") + ") ON CONFLICT(key) DO UPDATE SET value = " + SqlQuote("1") + ";").c_str());
        db.executeRawQuery((std::string("DELETE FROM app_meta WHERE key = ") + SqlQuote("search_data_recompute_cursor") + ";").c_str());
        std::atomic<float> progress{-2.0f};
        std::atomic<bool> cancel{false};
        const bool productRebuilt = db.rebuildFtsIndexBatched(2, &progress, &cancel);
        ok &= Expect(productRebuilt, "product rebuild reports success");
        const auto ftsSql = db.executeRawQuery("SELECT sql FROM sqlite_master WHERE name = 'audio_files_fts';");
        std::cout << "product-small progress=" << progress.load()
                  << " migrated=" << MetaValue(db, "search_data_migrated")
                  << " cursor=" << MetaValue(db, "search_data_recompute_cursor")
                  << " matchQiche=" << MatchCount(db, "qiche") << std::endl;
        ok &= Expect(MetaValue(db, "search_data_migrated") == "3", "product rebuildFtsIndexBatched completes migrated=3");
        ok &= Expect(MetaValue(db, "search_data_recompute_cursor").empty(), "product rebuild clears the recompute cursor");
        ok &= Expect(Contains(Scalar(ftsSql), "content="), "product rebuild keeps content-synced FTS");
        ok &= Expect(MatchCount(db, "qiche") == 1, "product rebuild FTS MATCH full pinyin qiche");
        ok &= Expect(MatchCount(db, "qc") == 1, "product rebuild FTS MATCH initials qc");
        ok &= Expect(MatchCount(db, "whoosh") == 1, "product rebuild FTS MATCH English whoosh");
        ok &= Expect(MatchCount(db, "daxiao") == 1, "product rebuild FTS MATCH Chinese pinyin daxiao");
        db.executeRawQuery("INSERT INTO audio_files_fts(audio_files_fts) VALUES('delete-all');");
        ok &= Expect(MatchCount(db, "qiche") == 0, "fixture clears FTS before explicit rebuild");
        const bool explicitlyRebuilt = db.rebuildFtsIndexBatched(2, &progress, &cancel);
        ok &= Expect(explicitlyRebuilt, "explicit FTS rebuild reports success");
        ok &= Expect(MatchCount(db, "qiche") == 1, "explicit rebuild restores FTS after migration=3");
        std::string blockedTable;
        ok &= Expect(InstallFtsRebuildFailTrigger(db, &blockedTable),
                     "explicit rebuild failure trigger installs");
        const bool blockedRebuild = db.rebuildFtsIndexBatched(2, &progress, &cancel);
        ok &= Expect(!blockedRebuild, "explicit FTS SQL failure reports failure");
        std::cout << "NOTE: rebuildFtsIndexBatched uses progress=-1 as 745 done/stop signal; success is migrated=3 not progress>=0. observed="
                  << progress.load() << std::endl;
        db.close();
    }

    {
        const fs::path dbPath = root / "product-multibatch.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "multi", 2100)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) {
            std::cout << "FAIL: reopen product-multibatch" << std::endl;
            return 1;
        }
        db.stopCoverPhashBackfillWorker();
        db.executeRawQuery((std::string("INSERT INTO app_meta(key, value) VALUES(") + SqlQuote("search_data_migrated") +
                           ", " + SqlQuote("1") + ") ON CONFLICT(key) DO UPDATE SET value = " + SqlQuote("1") + ";").c_str());
        db.executeRawQuery((std::string("DELETE FROM app_meta WHERE key = ") + SqlQuote("search_data_recompute_cursor") + ";").c_str());
        std::atomic<float> progress{-2.0f};
        std::atomic<bool> cancel{false};
        db.rebuildFtsIndexBatched(256, &progress, &cancel);
        std::cout << "product-multibatch rows=" << TotalRows(db)
                  << " migrated=" << MetaValue(db, "search_data_migrated")
                  << " matchQiche=" << MatchCount(db, "qiche")
                  << " progress=" << progress.load() << std::endl;
        const auto multiFtsSql = db.executeRawQuery("SELECT sql FROM sqlite_master WHERE name = 'audio_files_fts';");
        ok &= Expect(TotalRows(db) > 2048, "multi-batch fixture has more than one 2048 recompute page");
        ok &= Expect(MetaValue(db, "search_data_migrated") == "3", "product rebuildFtsIndexBatched finishes a >2048-row library");
        ok &= Expect(Contains(Scalar(multiFtsSql), "content="), "multi-batch product rebuild keeps content-synced FTS");
        ok &= Expect(MatchCount(db, "qiche") == 1, "multi-batch product rebuild MATCH qiche");
        db.close();
    }

    {
        const fs::path dbPath = root / "worker-success.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "worker-ok", 0)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) return 1;
        db.startCoverPhashBackfillWorker();
        const bool finished = WaitMigrated(db, 20000);
        std::cout << "worker-success migrated=" << MetaValue(db, "search_data_migrated")
                  << " matchQiche=" << MatchCount(db, "qiche") << std::endl;
        ok &= Expect(finished && MetaValue(db, "search_data_migrated") == "3", "background worker finishes migrated=3");
        ok &= Expect(MatchCount(db, "qiche") == 1, "background worker FTS MATCH qiche");
        db.close();
    }

    {
        const fs::path dbPath = root / "write-fail.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "write-fail", 0)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) return 1;
        db.stopCoverPhashBackfillWorker();
        db.executeRawQuery((std::string("INSERT INTO app_meta(key, value) VALUES(") + SqlQuote("search_data_migrated") +
                           ", " + SqlQuote("1") + ") ON CONFLICT(key) DO UPDATE SET value = " + SqlQuote("1") + ";").c_str());
        if (!InstallWriteFailTrigger(db)) return 1;
        const int oldBefore = MatchCount(db, "oldtoken");
        std::atomic<float> progress{-2.0f};
        std::atomic<bool> cancel{false};
        const bool rebuilt = db.rebuildFtsIndexBatched(2, &progress, &cancel);
        ok &= Expect(!rebuilt, "write-fail rebuild reports failure");
        const int attempts = WriteFailAttempts(db);
        std::cout << "write-fail MATCH oldtoken before=" << oldBefore
                  << " after=" << MatchCount(db, "oldtoken")
                  << " migrated=" << MetaValue(db, "search_data_migrated")
                  << " attempts=" << attempts
                  << " progress=" << progress.load() << std::endl;
        ok &= Expect(MatchCount(db, "oldtoken") == 1, "write-fail keeps original FTS MATCH oldtoken");
        ok &= Expect(MetaValue(db, "search_data_migrated") != "3", "write-fail does not mark migrated=3");
        ok &= Expect(MetaValue(db, "search_data_recompute_cursor").empty(), "write-fail does not advance the recompute cursor");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ok &= Expect(MetaValue(db, "search_data_migrated") != "3", "write-fail does not busy-retry after the failed publish");
        ok &= Expect(progress.load() < 0.0f, "write-fail uses progress=-1 as stop/failure signal");
        db.close();
    }

    {
        const fs::path dbPath = root / "worker-fail.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "worker-fail", 0)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) return 1;
        db.stopCoverPhashBackfillWorker();
        if (!InstallWriteFailTrigger(db)) return 1;
        db.startCoverPhashBackfillWorker();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
        int attempts = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            attempts = WriteFailAttempts(db);
            if (attempts >= 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                attempts = WriteFailAttempts(db);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::cout << "worker-fail updates=" << attempts
                  << " migrated=" << MetaValue(db, "search_data_migrated") << std::endl;
        const std::string migratedAfterWait = MetaValue(db, "search_data_migrated");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        ok &= Expect(migratedAfterWait != "3" && MetaValue(db, "search_data_migrated") != "3",
                    "background worker does not busy-retry write failures");
        ok &= Expect(MetaValue(db, "search_data_migrated") != "3", "worker write-fail does not mark complete");
        ok &= Expect(MatchCount(db, "oldtoken") == 1, "worker write-fail keeps MATCH oldtoken");
        db.close();
    }

    {
        const fs::path dbPath = root / "readonly.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "readonly", 0)) return 1;
        Database db;
        if (!db.openReadOnly(dbPath.u8string())) {
            std::cout << "FAIL: openReadOnly" << std::endl;
            return 1;
        }
        std::atomic<bool> cancel{false};
        const auto first = db.recomputeSearchDataBatch(64, &cancel);
        const auto second = db.recomputeSearchDataBatch(64, &cancel);
        std::cout << "readonly first=" << static_cast<int>(first)
                  << " second=" << static_cast<int>(second)
                  << " migrated=" << MetaValue(db, "search_data_migrated") << std::endl;
        ok &= Expect(first == HarkenSearch::Search::SearchDataRecomputeStep::Failed, "openReadOnly recompute does not report success");
        ok &= Expect(second == HarkenSearch::Search::SearchDataRecomputeStep::Failed, "openReadOnly second call stays failed");
        ok &= Expect(MetaValue(db, "search_data_migrated") != "3", "openReadOnly does not mark migrated=3");
        ok &= Expect(MetaValue(db, "search_data_recompute_cursor").empty(), "openReadOnly does not advance cursor");
        db.close();
    }

    {
        const fs::path dbPath = root / "transaction-owner.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "transaction-owner", 0)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) return 1;
        db.stopCoverPhashBackfillWorker();
        ok &= Expect(db.executeRawQuery("BEGIN IMMEDIATE;").success, "fixture begins user transaction");
        ok &= Expect(db.executeRawQuery("UPDATE audio_files SET title='unsaved-user-edit' WHERE id=1;").success,
                     "fixture writes pending user edit");
        std::atomic<bool> cancel{true};
        const auto step = db.recomputeSearchDataBatch(64, &cancel);
        ok &= Expect(step == HarkenSearch::Search::SearchDataRecomputeStep::Failed,
                     "cancelled recompute reports failure while user transaction is active");
        ok &= Expect(Scalar(db.executeRawQuery("SELECT title FROM audio_files WHERE id=1;")) == "unsaved-user-edit",
                     "recompute cleanup preserves pending user edit");
        ok &= Expect(db.executeRawQuery("COMMIT;").success, "user can commit its own transaction after recompute failure");
        const auto staged = db.recomputeSearchDataBatch(2, nullptr);
        ok &= Expect(staged == HarkenSearch::Search::SearchDataRecomputeStep::Continue,
                     "recompute stages a batch before a user transaction starts");
        ok &= Expect(db.executeRawQuery("BEGIN IMMEDIATE;").success, "user begins transaction during staged recompute");
        ok &= Expect(db.executeRawQuery("UPDATE audio_files SET title='second-user-edit' WHERE id=1;").success,
                     "user edits while recompute is staged");
        const auto deferred = db.recomputeSearchDataBatch(2, nullptr);
        ok &= Expect(deferred == HarkenSearch::Search::SearchDataRecomputeStep::Continue,
                     "recompute defers while another transaction owns the connection");
        ok &= Expect(Scalar(db.executeRawQuery("SELECT title FROM audio_files WHERE id=1;")) == "second-user-edit",
                     "deferred recompute preserves pending edit");
        ok &= Expect(db.executeRawQuery("COMMIT;").success, "user commits after recompute defers");
        HarkenSearch::Search::SearchDataRecomputeStep resumed = HarkenSearch::Search::SearchDataRecomputeStep::Continue;
        for (int i = 0; i < 8 && resumed == HarkenSearch::Search::SearchDataRecomputeStep::Continue; ++i) {
            resumed = db.recomputeSearchDataBatch(2, nullptr);
        }
        ok &= Expect(resumed == HarkenSearch::Search::SearchDataRecomputeStep::Finished,
                     "deferred recompute finishes after user commit");
        ok &= Expect(Scalar(db.executeRawQuery("SELECT title FROM audio_files WHERE id=1;")) == "second-user-edit",
                     "recompute does not overwrite committed user edit");
        db.close();
    }

    {
        const fs::path dbPath = root / "transaction-interleaving.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "transaction-interleaving", 0)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) return 1;
        db.stopCoverPhashBackfillWorker();
        std::mutex gateMutex;
        std::condition_variable gateCv;
        bool publishStarted = false;
        bool releasePublish = false;
        db.setSearchRecomputePublishHookForTests([&]() {
            std::unique_lock<std::mutex> lock(gateMutex);
            publishStarted = true;
            gateCv.notify_all();
            gateCv.wait(lock, [&]() { return releasePublish; });
        });
        HarkenSearch::Search::SearchDataRecomputeStep step = HarkenSearch::Search::SearchDataRecomputeStep::Failed;
        std::thread worker([&]() {
            for (int i = 0; i < 8; ++i) {
                step = db.recomputeSearchDataBatch(64, nullptr);
                if (step != HarkenSearch::Search::SearchDataRecomputeStep::Continue) break;
            }
        });
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            const bool reached = gateCv.wait_for(lock, std::chrono::seconds(5), [&]() { return publishStarted; });
            ok &= Expect(reached, "background recompute reached its owned publish transaction");
        }
        const bool userBegan = db.beginTransaction();
        ok &= Expect(userBegan, "UI begins its own transaction while background publish is paused");
        if (userBegan) ok &= Expect(db.rollbackTransaction(), "UI ends only its own transaction");
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            releasePublish = true;
        }
        gateCv.notify_all();
        worker.join();
        db.setSearchRecomputePublishHookForTests({});
        ok &= Expect(step == HarkenSearch::Search::SearchDataRecomputeStep::Finished,
                     "background publish completes after UI transaction");
        db.close();
    }

    {
        const fs::path dbPath = root / "cancel-after-start.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "cancel", 2100)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) return 1;
        db.stopCoverPhashBackfillWorker();
        db.executeRawQuery((std::string("INSERT INTO app_meta(key, value) VALUES(") + SqlQuote("search_data_migrated") +
                           ", " + SqlQuote("1") + ") ON CONFLICT(key) DO UPDATE SET value = " + SqlQuote("1") + ";").c_str());
        db.executeRawQuery((std::string("DELETE FROM app_meta WHERE key = ") + SqlQuote("search_data_recompute_cursor") + ";").c_str());
        const int oldBefore = MatchCount(db, "oldtoken");
        const int keepBefore = MatchCount(db, "keepoldtoken");
        std::atomic<float> progress{-2.0f};
        std::atomic<bool> cancel{false};
        bool cancelledResult = true;
        std::thread runner([&]() { cancelledResult = db.rebuildFtsIndexBatched(64, &progress, &cancel); });
        const bool started = WaitRecomputeStarted(db, &progress, 8000);
        if (started) cancel.store(true);
        else cancel.store(true);
        runner.join();
        ok &= Expect(!cancelledResult, "cancelled rebuild reports failure");
        std::cout << "cancel-after-start started=" << started
                  << " migrated=" << MetaValue(db, "search_data_migrated")
                  << " oldBefore=" << oldBefore << " oldAfter=" << MatchCount(db, "oldtoken")
                  << " keepBefore=" << keepBefore << " keepAfter=" << MatchCount(db, "keepoldtoken")
                  << " qiche=" << MatchCount(db, "qiche")
                  << " progress=" << progress.load() << std::endl;
        ok &= Expect(started, "in-flight cancel waited until recompute actually started");
        ok &= Expect(MetaValue(db, "search_data_migrated") != "3", "in-flight cancel does not mark migrated=3");
        ok &= Expect(MatchCount(db, "oldtoken") == 1, "in-flight cancel still MATCH oldtoken on original early row");
        ok &= Expect(MatchCount(db, "keepoldtoken") == 1, "in-flight cancel still MATCH keepoldtoken on unprocessed late row");
        const auto ftsSql = db.executeRawQuery("SELECT sql FROM sqlite_master WHERE name = 'audio_files_fts';");
        ok &= Expect(Contains(Scalar(ftsSql), "content="), "in-flight cancel keeps content-synced FTS");
        ok &= Expect(progress.load() < 0.0f, "in-flight cancel uses progress=-1 stop signal");
        db.close();
    }

    {
        const fs::path dbPath = root / "compute-then-write-fail.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "compute-fail", 0)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) return 1;
        db.stopCoverPhashBackfillWorker();
        db.executeRawQuery((std::string("INSERT INTO app_meta(key, value) VALUES(") + SqlQuote("search_data_migrated") +
                           ", " + SqlQuote("1") + ") ON CONFLICT(key) DO UPDATE SET value = " + SqlQuote("1") + ";").c_str());
        std::atomic<bool> cancel{false};
        const auto first = db.recomputeSearchDataBatch(2, &cancel);
        const bool started = (first == HarkenSearch::Search::SearchDataRecomputeStep::Continue) || WorkCount(db) > 0 ||
                             !MetaValue(db, "search_data_recompute_cursor").empty();
        std::cout << "compute-then-write-fail firstStep=" << static_cast<int>(first)
                  << " work=" << WorkCount(db)
                  << " started=" << started << std::endl;
        ok &= Expect(started, "compute-then-write-fail observed a real first compute batch");
        if (!InstallWriteFailTrigger(db)) return 1;
        std::atomic<float> progress{-2.0f};
        db.rebuildFtsIndexBatched(2, &progress, &cancel);
        const int attempts = WriteFailAttempts(db);
        std::cout << "compute-then-write-fail oldAfter=" << MatchCount(db, "oldtoken")
                  << " migrated=" << MetaValue(db, "search_data_migrated")
                  << " attempts=" << attempts << std::endl;
        ok &= Expect(MatchCount(db, "oldtoken") == 1, "compute-then-write-fail keeps MATCH oldtoken");
        ok &= Expect(MetaValue(db, "search_data_migrated") != "3", "compute-then-write-fail does not publish migrated=3");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ok &= Expect(MetaValue(db, "search_data_migrated") != "3", "compute-then-write-fail does not busy-retry a failed publish");
        db.close();
    }

    {
        const fs::path dbPath = root / "fts-rebuild-fail.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "fts-fail", 0)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) return 1;
        db.stopCoverPhashBackfillWorker();
        db.executeRawQuery((std::string("INSERT INTO app_meta(key, value) VALUES(") + SqlQuote("search_data_migrated") +
                           ", " + SqlQuote("1") + ") ON CONFLICT(key) DO UPDATE SET value = " + SqlQuote("1") + ";").c_str());
        std::atomic<bool> cancel{false};
        const auto step = db.recomputeSearchDataBatch(2048, &cancel);
        ok &= Expect(step == HarkenSearch::Search::SearchDataRecomputeStep::Continue &&
                     MetaValue(db, "search_data_migrated") != "3",
                     "fts-rebuild-fail stages rows before the failing publish");
        std::string ftsTable;
        const bool installed = InstallFtsRebuildFailTrigger(db, &ftsTable);
        std::cout << "fts-rebuild-fail installed=" << installed << " table=" << ftsTable
                  << " preMigrated=" << MetaValue(db, "search_data_migrated")
                  << " work=" << WorkCount(db) << std::endl;
        ok &= Expect(installed, "fts-rebuild-fail installed a real INSERT abort trigger");
        std::atomic<float> progress{-2.0f};
        const bool rebuilt = db.rebuildFtsIndexBatched(2048, &progress, &cancel);
        ok &= Expect(!rebuilt, "FTS publish failure reports failure");
        std::cout << "fts-rebuild-fail oldAfter=" << MatchCount(db, "oldtoken")
                  << " migrated=" << MetaValue(db, "search_data_migrated")
                  << " progress=" << progress.load() << std::endl;
        ok &= Expect(MatchCount(db, "oldtoken") == 1, "fts-rebuild-fail keeps MATCH oldtoken");
        ok &= Expect(MetaValue(db, "search_data_migrated") != "3", "fts-rebuild-fail rolls back and does not mark migrated=3");
        db.close();
    }

    {
        const fs::path dbPath = root / "concurrent-edit.db";
        if (!SeedLegacyLibrary(dbPath, audioDir / "concurrent", 0)) return 1;
        Database db;
        if (!db.open(dbPath.u8string(), false)) return 1;
        db.stopCoverPhashBackfillWorker();
        db.executeRawQuery((std::string("INSERT INTO app_meta(key, value) VALUES(") + SqlQuote("search_data_migrated") +
                           ", " + SqlQuote("1") + ") ON CONFLICT(key) DO UPDATE SET value = " + SqlQuote("1") + ";").c_str());
        std::atomic<bool> cancel{false};
        db.recomputeSearchDataBatch(2048, &cancel);
        db.executeRawQuery("UPDATE audio_files SET search_data = 'usertoken', keywords = 'usertoken' WHERE file_name = 'whoosh.wav';");
        std::atomic<float> progress{-2.0f};
        db.rebuildFtsIndexBatched(2048, &progress, &cancel);
        const auto liveWhoosh = db.executeRawQuery("SELECT search_data FROM audio_files WHERE file_name = 'whoosh.wav';");
        std::cout << "concurrent-edit liveWhoosh=" << Scalar(liveWhoosh)
                  << " matchUser=" << MatchCount(db, "usertoken")
                  << " migrated=" << MetaValue(db, "search_data_migrated") << std::endl;
        ok &= Expect(Scalar(liveWhoosh) == "usertoken", "concurrent ordinary edit is not overwritten by a stale snapshot");
        ok &= Expect(MatchCount(db, "usertoken") == 1, "concurrent edit remains searchable through live triggers/FTS");
        ok &= Expect(MetaValue(db, "search_data_migrated") == "3", "concurrent-edit still allows a successful publish of unchanged rows");
        db.close();
    }

    {
        const fs::path db1 = root / "close-source.db";
        const fs::path db2 = root / "close-other.db";
        if (!SeedLegacyLibrary(db1, audioDir / "close1", 2100)) return 1;
        if (!SeedLegacyLibrary(db2, audioDir / "close2", 0)) return 1;
        std::string fpAfterClose;
        {
            Database live;
            if (!live.open(db1.u8string(), false)) return 1;
            live.startCoverPhashBackfillWorker();
            const bool started = WaitRecomputeStarted(live, nullptr, 8000);
            ok &= Expect(started, "close waited until worker actually started");
            live.close();
            fpAfterClose = FileFingerprint(db1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const std::string fpLater = FileFingerprint(db1);
        std::cout << "close-source migrated=" << "see-reopen"
                  << " fpEqual=" << (fpAfterClose == fpLater) << std::endl;
        ok &= Expect(!fpAfterClose.empty() && fpAfterClose == fpLater, "close joined without late writes after join");
        Database other;
        if (!other.open(db2.u8string(), false)) return 1;
        other.stopCoverPhashBackfillWorker();
        ok &= Expect(MetaValue(other, "search_data_migrated") != "3", "closed worker does not finish a different library");
        const auto otherQiche = other.executeRawQuery("SELECT COUNT(*) FROM audio_files WHERE search_data LIKE '%qiche%';");
        ok &= Expect(Scalar(otherQiche) == "0", "closed worker does not write pinyin into another library");
        other.close();
        Database source;
        if (!source.open(db1.u8string(), false)) return 1;
        source.stopCoverPhashBackfillWorker();
        std::cout << "close-source migrated=" << MetaValue(source, "search_data_migrated")
                  << " otherMigratedPrintedAfterReopenSkip";
        source.close();
    }

    std::cout << (ok ? "ALL_PASS" : "HAS_FAIL") << std::endl;
    return ok ? 0 : 1;
}
