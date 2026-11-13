#include "SimilarityAnalysisStore.h"
#include "platform/filesystem.h"
#include "platform/file_hash.h"
#include "sqlite3.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
namespace HarkenSearch {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement Prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    if (db) sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr);
    return Statement(raw, sqlite3_finalize);
}
void Text(sqlite3_stmt* s, int at, const std::string& text) { sqlite3_bind_text(s, at, text.c_str(), int(text.size()), SQLITE_TRANSIENT); }
std::string Text(sqlite3_stmt* s, int at) { const auto* p = sqlite3_column_text(s, at); return p ? reinterpret_cast<const char*>(p) : ""; }
SimilarityFile File(sqlite3_stmt* s) {
    return {sqlite3_column_int64(s,0), Text(s,1), Text(s,2), Text(s,3), float(sqlite3_column_double(s,4))};
}
bool IdentityValid(const SimilarityFileIdentity& identity) {
    return identity.bytes <= uint64_t(std::numeric_limits<int64_t>::max()) && identity.sha256.size() == 64 &&
        std::all_of(identity.sha256.begin(), identity.sha256.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
}
bool Row(sqlite3_stmt* s, SimilarityAnalysisEntry& out, bool detailed) {
    SimilarityAnalysisEntry e; e.file = File(s);
    e.identity = {uint64_t(sqlite3_column_int64(s,5)), sqlite3_column_int64(s,6), Text(s,7)};
    if (!IdentityValid(e.identity) || !AcousticAnalysis::Deserialize(sqlite3_column_blob(s,8),
        size_t(sqlite3_column_bytes(s,8)), e.acoustic, detailed)) return false;
    if (Text(s,10) == kSimilarityModelRevision && sqlite3_column_bytes(s,9) == sizeof(AudioEmbedding)) {
        std::memcpy(e.embedding.data(), sqlite3_column_blob(s,9), sizeof(AudioEmbedding));
        e.hasSemantic = IsValidAudioEmbedding(e.embedding);
    }
    out = std::move(e); return true;
}
std::string Select(bool detailed) {
    return std::string("SELECT f.id,f.file_path,f.file_name,f.format,f.duration,a.file_bytes,a.modified,a.fingerprint,a.") +
        (detailed ? "acoustic" : "summary") + ",a.embedding,a.model_version FROM harken_similarity_v2 a JOIN audio_files f ON f.id=a.file_id AND f.file_path=a.file_path WHERE a.version=2";
}
}
bool SimilarityFileIdentity::operator==(const SimilarityFileIdentity& r) const {
    // Modification time is diagnostic only. The PAL converts file-clock time
    // through wall-clock time; a whole-second boundary can round either way.
    // Content hashes before and after analysis are the publication authority.
    return bytes == r.bytes && sha256 == r.sha256;
}
SimilarityAnalysisStore::~SimilarityAnalysisStore() { Close(); }
bool SimilarityAnalysisStore::Open(const std::string& path, const FeatureExtractionCancelCheck& cancel) {
    Close(); cancel_ = cancel; error_.clear();
    if ((cancel_ && cancel_()) || path.empty()) { error_ = "cancelled_or_empty_path"; return false; }
    if (sqlite3_open_v2(path.c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        error_ = database_ ? sqlite3_errmsg(database_) : "database_open_failed"; Close(); return false;
    }
    sqlite3_busy_handler(database_, [](void* context, int attempt) {
        const auto* self = static_cast<SimilarityAnalysisStore*>(context);
        if (attempt >= 300 || (self->cancel_ && self->cancel_())) return 0;
        sqlite3_sleep(10); return 1;
    }, this);
    sqlite3_progress_handler(database_, 1000, [](void* context) {
        const auto* self = static_cast<SimilarityAnalysisStore*>(context);
        return self->cancel_ && self->cancel_() ? 1 : 0;
    }, this);
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS harken_similarity_revision(singleton INTEGER PRIMARY KEY CHECK(singleton=1), revision INTEGER NOT NULL);
INSERT OR IGNORE INTO harken_similarity_revision VALUES(1,0);
CREATE TABLE IF NOT EXISTS harken_similarity_identity(singleton INTEGER PRIMARY KEY CHECK(singleton=1), identity TEXT NOT NULL);
INSERT OR IGNORE INTO harken_similarity_identity VALUES(1,lower(hex(randomblob(16))));
CREATE TABLE IF NOT EXISTS harken_similarity_v2(
 file_id INTEGER PRIMARY KEY,file_path TEXT NOT NULL,file_bytes INTEGER NOT NULL,modified INTEGER NOT NULL,
 fingerprint TEXT NOT NULL,version INTEGER NOT NULL,summary BLOB NOT NULL,acoustic BLOB NOT NULL,
 embedding BLOB,model_version TEXT NOT NULL DEFAULT '');
CREATE TABLE IF NOT EXISTS harken_similarity_segments_v2(
 file_id INTEGER NOT NULL,ordinal INTEGER NOT NULL,start REAL NOT NULL,end REAL NOT NULL,embedding BLOB NOT NULL,
 PRIMARY KEY(file_id,ordinal));
CREATE TRIGGER IF NOT EXISTS harken_similarity_insert AFTER INSERT ON harken_similarity_v2 BEGIN
 UPDATE harken_similarity_revision SET revision=revision+1 WHERE singleton=1; END;
CREATE TRIGGER IF NOT EXISTS harken_similarity_update AFTER UPDATE ON harken_similarity_v2 BEGIN
 UPDATE harken_similarity_revision SET revision=revision+1 WHERE singleton=1; END;
CREATE TRIGGER IF NOT EXISTS harken_similarity_delete AFTER DELETE ON harken_similarity_v2 BEGIN
 DELETE FROM harken_similarity_segments_v2 WHERE file_id=OLD.file_id;
 UPDATE harken_similarity_revision SET revision=revision+1 WHERE singleton=1; END;
CREATE TRIGGER IF NOT EXISTS harken_similarity_audio_delete AFTER DELETE ON audio_files BEGIN
 DELETE FROM harken_similarity_v2 WHERE file_id=OLD.id;
 UPDATE harken_similarity_revision SET revision=revision+1 WHERE singleton=1; END;
CREATE TRIGGER IF NOT EXISTS harken_similarity_audio_update AFTER UPDATE OF file_path,file_size,duration ON audio_files
 WHEN OLD.file_path IS NOT NEW.file_path OR OLD.file_size IS NOT NEW.file_size OR OLD.duration IS NOT NEW.duration BEGIN
 DELETE FROM harken_similarity_v2 WHERE file_id=OLD.id;
 UPDATE harken_similarity_revision SET revision=revision+1 WHERE singleton=1; END;
CREATE TRIGGER IF NOT EXISTS harken_similarity_audio_insert AFTER INSERT ON audio_files BEGIN
 UPDATE harken_similarity_revision SET revision=revision+1 WHERE singleton=1; END;
)SQL";
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE", nullptr,nullptr,nullptr) != SQLITE_OK ||
        sqlite3_exec(database_, schema, nullptr,nullptr,nullptr) != SQLITE_OK ||
        sqlite3_exec(database_, "COMMIT", nullptr,nullptr,nullptr) != SQLITE_OK) {
        error_ = sqlite3_errmsg(database_); sqlite3_exec(database_, "ROLLBACK",nullptr,nullptr,nullptr); Close(); return false;
    }
    return true;
}
void SimilarityAnalysisStore::Close() { if (database_) sqlite3_close(database_); database_ = nullptr; cancel_ = {}; }
bool SimilarityAnalysisStore::Files(std::vector<SimilarityFile>& files) {
    files.clear(); auto s = Prepare(database_, "SELECT id,file_path,file_name,format,duration FROM audio_files ORDER BY id");
    if (!s) return false;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) { if (cancel_ && cancel_()) { files.clear(); return false; } files.push_back(File(s.get())); }
    return rc == SQLITE_DONE;
}
bool SimilarityAnalysisStore::GetFile(int64_t id, SimilarityFile& file) {
    auto s = Prepare(database_, "SELECT id,file_path,file_name,format,duration FROM audio_files WHERE id=?");
    if (!s) return false; sqlite3_bind_int64(s.get(),1,id);
    if (sqlite3_step(s.get()) != SQLITE_ROW) return false;
    file = File(s.get()); return true;
}
bool SimilarityAnalysisStore::CountValidCoverage(sqlite3* readOnlyDatabase, int& acoustic, int& semantic,
                                                const FeatureExtractionCancelCheck& cancel) {
    acoustic = semantic = 0;
    auto s = Prepare(readOnlyDatabase, Select(false));
    if (!s) return false;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
        if (cancel && cancel()) return false;
        SimilarityAnalysisEntry entry;
        if (!Row(s.get(), entry, false)) continue;
        ++acoustic;
        if (entry.hasSemantic) ++semantic;
    }
    return rc == SQLITE_DONE && !(cancel && cancel());
}
bool SimilarityAnalysisStore::PendingFiles(std::vector<SimilarityFile>& files, bool requireSemantic) {
    files.clear();
    auto s = Prepare(database_,
        "SELECT f.id,f.file_path,f.file_name,f.format,f.duration,a.file_bytes,a.modified,a.fingerprint,"
        "a.summary,a.embedding,a.model_version,a.version FROM audio_files f "
        "LEFT JOIN harken_similarity_v2 a ON a.file_id=f.id AND a.file_path=f.file_path ORDER BY f.id");
    if (!s) return false;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
        if (cancel_ && cancel_()) { files.clear(); return false; }
        SimilarityAnalysisEntry entry;
        if (sqlite3_column_int(s.get(),11) != 2 || !Row(s.get(),entry,false) ||
            (requireSemantic && !entry.hasSemantic)) files.push_back(File(s.get()));
    }
    return rc == SQLITE_DONE;
}
bool SimilarityAnalysisStore::Publish(const SimilarityAnalysisEntry& e) {
    SimilarityFile current;
    if (!database_ || (cancel_ && cancel_()) || !GetFile(e.file.id,current) || current.path != e.file.path ||
        !e.acoustic.IsValid() || !IdentityValid(e.identity)) return false;
    if (e.hasSemantic) {
        if (!IsValidAudioEmbedding(e.embedding) || e.segments.empty() || e.segments.size() > 4096) return false;
        float covered = e.acoustic.activeStart, previous = -1;
        for (const auto& segment : e.segments) {
            if (!std::isfinite(segment.startSeconds) || !std::isfinite(segment.endSeconds) ||
                segment.startSeconds < e.acoustic.activeStart - 0.002f || segment.startSeconds <= previous ||
                segment.startSeconds > covered + 0.002f || segment.endSeconds <= segment.startSeconds ||
                segment.endSeconds > e.acoustic.activeStart + e.acoustic.activeSeconds + 0.002f ||
                !IsValidAudioEmbedding(segment.embedding)) return false;
            previous = segment.startSeconds; covered = std::max(covered,segment.endSeconds);
        }
        if (covered < e.acoustic.activeStart + e.acoustic.activeSeconds - 0.002f) return false;
    }
    const auto acoustic = e.acoustic.Serialize(), summary = e.acoustic.Serialize(false);
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE",nullptr,nullptr,nullptr) != SQLITE_OK) return false;
    auto rollback = [&] { error_ = sqlite3_errmsg(database_); sqlite3_exec(database_,"ROLLBACK",nullptr,nullptr,nullptr); return false; };
    auto s = Prepare(database_, "INSERT INTO harken_similarity_v2(file_id,file_path,file_bytes,modified,fingerprint,version,summary,acoustic,embedding,model_version) VALUES(?,?,?,?,?,2,?,?,?,?) ON CONFLICT(file_id) DO UPDATE SET file_path=excluded.file_path,file_bytes=excluded.file_bytes,modified=excluded.modified,fingerprint=excluded.fingerprint,version=excluded.version,summary=excluded.summary,acoustic=excluded.acoustic,embedding=excluded.embedding,model_version=excluded.model_version");
    if (!s) return rollback();
    sqlite3_bind_int64(s.get(),1,e.file.id); Text(s.get(),2,e.file.path); sqlite3_bind_int64(s.get(),3,int64_t(e.identity.bytes));
    sqlite3_bind_int64(s.get(),4,e.identity.modified); Text(s.get(),5,e.identity.sha256);
    sqlite3_bind_blob(s.get(),6,summary.data(),int(summary.size()),SQLITE_TRANSIENT);
    sqlite3_bind_blob(s.get(),7,acoustic.data(),int(acoustic.size()),SQLITE_TRANSIENT);
    if (e.hasSemantic) sqlite3_bind_blob(s.get(),8,e.embedding.data(),sizeof(e.embedding),SQLITE_TRANSIENT);
    Text(s.get(),9,e.hasSemantic ? kSimilarityModelRevision : "");
    if (sqlite3_step(s.get()) != SQLITE_DONE) return rollback();
    auto remove = Prepare(database_, "DELETE FROM harken_similarity_segments_v2 WHERE file_id=?");
    if (!remove) return rollback(); sqlite3_bind_int64(remove.get(),1,e.file.id);
    if (sqlite3_step(remove.get()) != SQLITE_DONE) return rollback();
    if (e.hasSemantic) {
        auto segment = Prepare(database_, "INSERT INTO harken_similarity_segments_v2 VALUES(?,?,?,?,?)");
        if (!segment) return rollback(); int ordinal = 0;
        for (const auto& part : e.segments) {
            if (cancel_ && cancel_()) return rollback();
            sqlite3_reset(segment.get()); sqlite3_clear_bindings(segment.get());
            sqlite3_bind_int64(segment.get(),1,e.file.id); sqlite3_bind_int(segment.get(),2,ordinal++);
            sqlite3_bind_double(segment.get(),3,part.startSeconds); sqlite3_bind_double(segment.get(),4,part.endSeconds);
            sqlite3_bind_blob(segment.get(),5,part.embedding.data(),sizeof(part.embedding),SQLITE_TRANSIENT);
            if (sqlite3_step(segment.get()) != SQLITE_DONE) return rollback();
        }
    }
    if ((cancel_ && cancel_()) || sqlite3_exec(database_,"COMMIT",nullptr,nullptr,nullptr) != SQLITE_OK) return rollback();
    return true;
}
bool SimilarityAnalysisStore::Load(int64_t id, SimilarityAnalysisEntry& entry, bool detailed) {
    auto s = Prepare(database_, Select(detailed) + " AND a.file_id=?");
    if (!s) return false; sqlite3_bind_int64(s.get(),1,id);
    SimilarityAnalysisEntry parsed;
    if (sqlite3_step(s.get()) != SQLITE_ROW || !Row(s.get(),parsed,detailed)) return false;
    if (detailed && parsed.hasSemantic) {
        auto parts = Prepare(database_, "SELECT start,end,embedding FROM harken_similarity_segments_v2 WHERE file_id=? ORDER BY ordinal");
        if (!parts) return false; sqlite3_bind_int64(parts.get(),1,id);
        int rc; float covered = parsed.acoustic.activeStart, previous = -1;
        while ((rc = sqlite3_step(parts.get())) == SQLITE_ROW) {
            AudioEmbeddingSegment part; part.startSeconds = float(sqlite3_column_double(parts.get(),0)); part.endSeconds = float(sqlite3_column_double(parts.get(),1));
            if (sqlite3_column_bytes(parts.get(),2) != sizeof(part.embedding)) return false;
            std::memcpy(part.embedding.data(),sqlite3_column_blob(parts.get(),2),sizeof(part.embedding));
            if (!IsValidAudioEmbedding(part.embedding) || !std::isfinite(part.startSeconds) || !std::isfinite(part.endSeconds) ||
                part.startSeconds < parsed.acoustic.activeStart - 0.002f || part.startSeconds <= previous || part.startSeconds > covered + 0.002f ||
                part.endSeconds <= part.startSeconds || part.endSeconds > parsed.acoustic.activeStart + parsed.acoustic.activeSeconds + 0.002f || parsed.segments.size() >= 4096) return false;
            previous = part.startSeconds; covered = std::max(covered,part.endSeconds); parsed.segments.push_back(part);
        }
        if (rc != SQLITE_DONE || parsed.segments.empty() || covered < parsed.acoustic.activeStart + parsed.acoustic.activeSeconds - 0.002f) return false;
    }
    entry = std::move(parsed); return true;
}
bool SimilarityAnalysisStore::LoadSummaries(std::vector<SimilarityAnalysisEntry>& entries) {
    entries.clear(); auto s = Prepare(database_, Select(false) + " ORDER BY a.file_id"); if (!s) return false; int rc;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
        if (cancel_ && cancel_()) { entries.clear(); return false; }
        SimilarityAnalysisEntry entry; if (Row(s.get(),entry,false)) entries.push_back(std::move(entry));
    }
    return rc == SQLITE_DONE;
}
int64_t SimilarityAnalysisStore::SegmentCount() {
    auto s = Prepare(database_, "SELECT COUNT(*) FROM harken_similarity_segments_v2 s JOIN harken_similarity_v2 a ON a.file_id=s.file_id JOIN audio_files f ON f.id=a.file_id AND f.file_path=a.file_path WHERE a.version=2 AND a.model_version=?");
    if (!s) return -1; Text(s.get(),1,kSimilarityModelRevision);
    return sqlite3_step(s.get()) == SQLITE_ROW ? sqlite3_column_int64(s.get(),0) : -1;
}
bool SimilarityAnalysisStore::VisitSegments(const std::function<bool(int64_t, const AudioEmbeddingSegment&)>& visitor) {
    auto s = Prepare(database_, "SELECT s.file_id,s.start,s.end,s.embedding FROM harken_similarity_segments_v2 s JOIN harken_similarity_v2 a ON a.file_id=s.file_id JOIN audio_files f ON f.id=a.file_id AND f.file_path=a.file_path WHERE a.version=2 AND a.model_version=? ORDER BY s.file_id,s.ordinal");
    if (!s) return false; Text(s.get(),1,kSimilarityModelRevision); int rc;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
        if (cancel_ && cancel_()) return false;
        AudioEmbeddingSegment part;
        part.startSeconds = float(sqlite3_column_double(s.get(),1)); part.endSeconds = float(sqlite3_column_double(s.get(),2));
        if (sqlite3_column_bytes(s.get(),3) != sizeof(part.embedding)) continue;
        std::memcpy(part.embedding.data(),sqlite3_column_blob(s.get(),3),sizeof(part.embedding));
        if (!IsValidAudioEmbedding(part.embedding) || !std::isfinite(part.startSeconds) || !std::isfinite(part.endSeconds) ||
            part.startSeconds < 0 || part.endSeconds <= part.startSeconds) continue;
        if (!visitor(sqlite3_column_int64(s.get(),0),part)) return false;
    }
    return rc == SQLITE_DONE;
}
bool SimilarityAnalysisStore::Invalidate(int64_t id) {
    auto s = Prepare(database_, "DELETE FROM harken_similarity_v2 WHERE file_id=?");
    if (!s) return false; sqlite3_bind_int64(s.get(),1,id); return sqlite3_step(s.get()) == SQLITE_DONE;
}
int64_t SimilarityAnalysisStore::ClearAll() {
    if (!database_) { error_ = "database_not_open"; return -1; }
    if (cancel_ && cancel_()) { error_ = "cancelled"; return -1; }
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE",nullptr,nullptr,nullptr) != SQLITE_OK) {
        error_ = sqlite3_errmsg(database_); return -1;
    }
    auto rollback = [&]() -> int64_t {
        error_ = sqlite3_errmsg(database_);
        sqlite3_exec(database_,"ROLLBACK",nullptr,nullptr,nullptr);
        return -1;
    };
    // Delete both tables in one transaction, including any orphan segments.
    if (sqlite3_exec(database_,"DELETE FROM harken_similarity_segments_v2",nullptr,nullptr,nullptr) != SQLITE_OK ||
        sqlite3_exec(database_,"DELETE FROM harken_similarity_v2",nullptr,nullptr,nullptr) != SQLITE_OK) return rollback();
    const int64_t deleted = sqlite3_changes(database_);
    if ((cancel_ && cancel_()) || sqlite3_exec(database_,"COMMIT",nullptr,nullptr,nullptr) != SQLITE_OK) return rollback();
    error_.clear();
    return deleted;
}
int64_t SimilarityAnalysisStore::Revision() {
    auto s = Prepare(database_, "SELECT revision FROM harken_similarity_revision WHERE singleton=1");
    return s && sqlite3_step(s.get()) == SQLITE_ROW ? sqlite3_column_int64(s.get(),0) : -1;
}
std::string SimilarityAnalysisStore::LibraryIdentity() {
    auto s = Prepare(database_, "SELECT identity FROM harken_similarity_identity WHERE singleton=1");
    return s && sqlite3_step(s.get()) == SQLITE_ROW ? Text(s.get(),0) : std::string{};
}
SimilarityCoverage SimilarityAnalysisStore::Coverage() {
    SimilarityCoverage result;
    auto s = Prepare(database_, "SELECT (SELECT COUNT(*) FROM audio_files),(SELECT COUNT(*) FROM harken_similarity_v2 a JOIN audio_files f ON f.id=a.file_id AND f.file_path=a.file_path WHERE a.version=2),(SELECT COUNT(*) FROM harken_similarity_v2 a JOIN audio_files f ON f.id=a.file_id AND f.file_path=a.file_path WHERE a.version=2 AND a.embedding IS NOT NULL AND a.model_version=?)");
    if (s) { Text(s.get(),1,kSimilarityModelRevision); if (sqlite3_step(s.get()) == SQLITE_ROW) result = {sqlite3_column_int64(s.get(),0), sqlite3_column_int64(s.get(),1), sqlite3_column_int64(s.get(),2)}; }
    return result;
}
bool ReadSimilarityFileIdentity(const std::string& path, SimilarityFileIdentity& value, const FeatureExtractionCancelCheck& cancel) {
    if (cancel && cancel()) return false;
    const auto before = Platform::FileSystem::ProbeRegularFile(path);
    const auto modified = Platform::FileSystem::LastWriteTimeUnixSeconds(path);
    if (!before.isRegularFile || !before.sizeAvailable) return false;
    auto hash = Platform::Crypto::ComputeSha256File(path);
    const auto after = Platform::FileSystem::ProbeRegularFile(path);
    if ((cancel && cancel()) || hash.status != Platform::Crypto::FileHashStatus::Success || !after.isRegularFile ||
        !after.sizeAvailable || before.sizeBytes != after.sizeBytes) return false;
    value = {before.sizeBytes, modified, std::move(hash.hexDigest)}; return true;
}
} // namespace HarkenSearch
