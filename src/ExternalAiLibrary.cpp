#include "ExternalAiLibrary.h"
#include "SimilarityAnalysisStore.h"
#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#endif

namespace HarkenSearch {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
using Clock = std::chrono::steady_clock;
constexpr auto QueryBudget = std::chrono::milliseconds(1250);
constexpr int MaxResults = 50;
constexpr int MaxCandidateRows = 4096;
constexpr float MinSubstituteScore = 0.35f;
constexpr std::size_t MaxPathBytes = 4096;
constexpr std::size_t MaxPathComponents = 128;

struct LibraryScope {
    fs::path root;
    bool foldRootCase = false;
};

Json Payload(const char* status, Json rows = Json::array()) {
    return {{"status", status}, {"results", std::move(rows)}};
}

bool CleanComponent(const fs::path& component) {
    if (component.empty() || component == "." || component == "..") {
        return false;
    }
    const auto& name = component.native();
#ifdef _WIN32
    if (name.back() == L'.' || name.back() == L' ') {
        return false;
    }
#endif
    return std::none_of(name.begin(), name.end(), [](const auto unit) {
        return (unit >= 0 && unit < 32) || unit == ':' || unit == '\\' || unit == '/';
    });
}

enum class Entry { Present, Missing, Rejected };

Entry CheckLocalEntry(const fs::path& path, bool directory) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Entry::Missing : Entry::Rejected;
    }
    return !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_DEVICE)) &&
        static_cast<bool>(attributes & FILE_ATTRIBUTE_DIRECTORY) == directory ? Entry::Present : Entry::Rejected;
#elif defined(__APPLE__)
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found) {
        return Entry::Missing;
    }
    struct statfs volume{};
    return !error && (directory ? fs::is_directory(status) : fs::is_regular_file(status)) &&
        statfs(path.c_str(), &volume) == 0 && (volume.f_flags & MNT_LOCAL) ? Entry::Present : Entry::Rejected;
#else
    return Entry::Rejected;
#endif
}

bool LocalEntry(const fs::path& path, bool directory) {
    return CheckLocalEntry(path, directory) == Entry::Present;
}

std::optional<fs::path> LocalAbsolutePath(const std::string& value, bool directory) {
    if (value.empty() || value.size() > MaxPathBytes || value.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    auto path = fs::u8path(value).make_preferred();
    if (!path.is_absolute()) {
        return std::nullopt;
    }
#ifdef _WIN32
    const auto drive = path.root_name().native();
    if (drive.size() != 2 || drive[1] != L':' ||
        !((drive[0] >= L'A' && drive[0] <= L'Z') || (drive[0] >= L'a' && drive[0] <= L'z'))) {
        return std::nullopt;
    }
    const auto kind = GetDriveTypeW(path.root_path().c_str());
    if (kind != DRIVE_FIXED && kind != DRIVE_REMOVABLE) {
        return std::nullopt;
    }
#else
    if (value.size() > 1 && value[1] == '/') {
        return std::nullopt;
    }
#endif
    const auto relative = path.relative_path();
    auto resolved = path.root_path();
    for (auto component = relative.begin(); component != relative.end(); ++component) {
        const auto next = std::next(component);
        if (directory && component->empty() && next == relative.end()) {
            break;
        }
        if (!CleanComponent(*component)) {
            return std::nullopt;
        }
        resolved /= *component;
        if (!LocalEntry(resolved, directory || next != relative.end())) {
            return std::nullopt;
        }
    }
    if (!LocalEntry(resolved, directory)) {
        return std::nullopt;
    }
    return resolved;
}

bool CanFoldRootCase(const fs::path& path) {
#ifdef _WIN32
    auto parent = path.root_path();
    for (const auto& part : path.relative_path()) {
        HANDLE handle = CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        FILE_CASE_SENSITIVE_INFO caseInfo{};
        const bool insensitive = GetFileInformationByHandleEx(handle, FileCaseSensitiveInfo,
            &caseInfo, sizeof(caseInfo)) && !(caseInfo.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR);
        CloseHandle(handle);
        if (!insensitive) {
            return false;
        }
        parent /= part;
    }
    return true;
#else
    return false;
#endif
}

bool SamePathPart(const fs::path& left, const fs::path& right, bool foldCase) {
    if (!foldCase) {
        return left == right;
    }
    const auto fold = [](const fs::path::value_type character) {
        return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
    };
    const auto& leftName = left.native();
    const auto& rightName = right.native();
    return std::equal(leftName.begin(), leftName.end(), rightName.begin(), rightName.end(),
        [&fold](const auto leftUnit, const auto rightUnit) { return fold(leftUnit) == fold(rightUnit); });
}

std::string ReadOnlyUri(const fs::path& path) {
    std::string uri = "file:";
    constexpr char Hex[] = "0123456789ABCDEF";
    for (const unsigned char byte : path.generic_u8string()) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '/') {
            uri += static_cast<char>(byte);
        } else {
            uri += '%';
            uri += Hex[byte >> 4];
            uri += Hex[byte & 0x0f];
        }
    }
    return uri + "?mode=ro&readonly_shm=1";
}

class ReadOnlyVfs final : public sqlite3_vfs {
public:
    ReadOnlyVfs() : sqlite3_vfs{}, native_(sqlite3_vfs_find(nullptr)) {}
    ~ReadOnlyVfs() {
        if (registered_) {
            sqlite3_vfs_unregister(this);
        }
#ifdef _WIN32
        if (wal_ != INVALID_HANDLE_VALUE) {
            CloseHandle(wal_);
        }
        if (shm_ != INVALID_HANDLE_VALUE) {
            CloseHandle(shm_);
        }
#endif
    }

    bool Register() {
        if (!native_) {
            return false;
        }
        name_ = "harken-external-readonly-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        static_cast<sqlite3_vfs&>(*this) = *native_;
        zName = name_.c_str();
        pNext = nullptr;
        xOpen = OpenFile;
        xDelete = [](sqlite3_vfs*, const char*, int) { return SQLITE_READONLY; };
        registered_ = sqlite3_vfs_register(this, 0) == SQLITE_OK;
        return registered_;
    }

private:
    static int OpenFile(sqlite3_vfs* vfs, const char* name, sqlite3_file* file,
                        int flags, int* usedFlags) noexcept {
        auto& guarded = *static_cast<ReadOnlyVfs*>(vfs);
        file->pMethods = nullptr;
        try {
            if (flags & SQLITE_OPEN_WAL) {
                const char* mainPath = sqlite3_filename_database(name);
                if (!mainPath || !guarded.OpenExistingSidecars(name, std::string(mainPath) + "-shm")) {
                    return SQLITE_CANTOPEN;
                }
                flags = (flags & ~(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) | SQLITE_OPEN_READONLY;
            } else if ((flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_MAIN_JOURNAL)) == 0 ||
                       !(flags & SQLITE_OPEN_READONLY) ||
                       (flags & (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_DELETEONCLOSE)) ||
                       !name || !LocalAbsolutePath(name, false)) {
                return SQLITE_CANTOPEN;
            }
            return guarded.native_->xOpen(guarded.native_, name, file, flags, usedFlags);
        } catch (...) {
            return SQLITE_CANTOPEN;
        }
    }

    bool OpenExistingSidecars(const char* wal, const std::string& shm) {
        const auto walPath = fs::u8path(wal);
        const auto shmPath = fs::u8path(shm);
#ifdef _WIN32
        if (wal_ == INVALID_HANDLE_VALUE) {
            wal_ = OpenExistingDiskFile(walPath);
        }
        if (shm_ == INVALID_HANDLE_VALUE) {
            shm_ = OpenExistingDiskFile(shmPath);
        }
        return wal_ != INVALID_HANDLE_VALUE && shm_ != INVALID_HANDLE_VALUE;
#else
        return LocalEntry(walPath, false) && LocalEntry(shmPath, false);
#endif
    }

#ifdef _WIN32
    static HANDLE OpenExistingDiskFile(const fs::path& path) {
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle != INVALID_HANDLE_VALUE &&
            (!GetFileInformationByHandle(handle, &info) || GetFileType(handle) != FILE_TYPE_DISK ||
             (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                      FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_DEVICE)))) {
            CloseHandle(handle);
            return INVALID_HANDLE_VALUE;
        }
        return handle;
    }

    HANDLE wal_ = INVALID_HANDLE_VALUE;
    HANDLE shm_ = INVALID_HANDLE_VALUE;
#endif
    sqlite3_vfs* native_;
    std::string name_;
    bool registered_ = false;
};

std::string_view SqlText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? std::string_view(reinterpret_cast<const char*>(value),
        static_cast<std::size_t>(sqlite3_column_bytes(statement, column))) : std::string_view{};
}

std::optional<std::string> CatalogRelativePath(const LibraryScope& scope, std::string_view value) {
    if (value.empty() || value.size() > MaxPathBytes || value.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto candidate = fs::u8path(value.begin(), value.end()).make_preferred();
    if (!candidate.is_absolute() ||
        !SamePathPart(candidate.root_path(), scope.root.root_path(), scope.foldRootCase)) {
        return std::nullopt;
    }
    const auto catalogParts = candidate.relative_path();
    const auto rootParts = scope.root.relative_path();
    auto component = catalogParts.begin();
    for (const auto& authorized : rootParts) {
        if (component == catalogParts.end() || !SamePathPart(*component, authorized, scope.foldRootCase)) {
            return std::nullopt;
        }
        ++component;
    }
    fs::path relative;
    fs::path absolute = scope.root;
    bool missing = false;
    std::size_t depth = 0;
    for (; component != catalogParts.end(); ++component) {
        if (++depth > MaxPathComponents || !CleanComponent(*component)) {
            return std::nullopt;
        }
        relative /= *component;
        if (!missing) {
            absolute /= *component;
            const auto entry = CheckLocalEntry(absolute, std::next(component) != catalogParts.end());
            if (entry == Entry::Rejected) {
                return std::nullopt;
            }
            missing = entry == Entry::Missing;
        }
    }
    if (relative.empty()) {
        return std::nullopt;
    }
    return relative.generic_u8string();
}

void ScopedPath(sqlite3_context* context, int, sqlite3_value** args) noexcept {
    try {
        const auto& scope = *static_cast<const LibraryScope*>(sqlite3_user_data(context));
        const auto* value = sqlite3_value_text(args[0]);
        const int size = sqlite3_value_bytes(args[0]);
        if (value && size > 0) {
            const auto relative = CatalogRelativePath(scope,
                {reinterpret_cast<const char*>(value), static_cast<std::size_t>(size)});
            if (relative) {
                sqlite3_result_text(context, relative->c_str(), static_cast<int>(relative->size()), SQLITE_TRANSIENT);
                return;
            }
        }
    } catch (const std::bad_alloc&) {
        sqlite3_result_error_nomem(context);
        return;
    } catch (...) {
    }
    sqlite3_result_null(context);
}

void ParentPath(sqlite3_context* context, int, sqlite3_value** args) noexcept {
    const auto* value = sqlite3_value_text(args[0]);
    if (value) {
        const std::string_view relative(reinterpret_cast<const char*>(value),
            static_cast<std::size_t>(sqlite3_value_bytes(args[0])));
        const auto separator = relative.rfind('/');
        if (separator != std::string_view::npos) {
            sqlite3_result_text(context, relative.data(), static_cast<int>(separator), SQLITE_TRANSIENT);
            return;
        }
    }
    sqlite3_result_null(context);
}

class ReadSession final {
public:
    bool Open(const std::string& uri, const fs::path& authorizedRoot) {
        const auto root = LocalAbsolutePath(authorizedRoot.u8string(), true);
        if (!root || !vfs_.Register()) {
            return false;
        }
        scope_ = {*root, CanFoldRootCase(*root)};
        sqlite3* raw = nullptr;
        const auto result = sqlite3_open_v2(uri.c_str(), &raw,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_NOFOLLOW, vfs_.zName);
        connection_.reset(raw);
        if (result != SQLITE_OK || sqlite3_db_readonly(raw, "main") != 1) {
            return false;
        }
        sqlite3_progress_handler(raw, 500, [](void* value) {
            return static_cast<ReadSession*>(value)->Expired() ? 1 : 0;
        }, this);
        sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, 16 * 1024 * 1024);
        constexpr int functionFlags = SQLITE_UTF8 | SQLITE_DIRECTONLY;
        return sqlite3_db_config(raw, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr) == SQLITE_OK &&
            sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) == SQLITE_OK &&
            sqlite3_db_config(raw, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) == SQLITE_OK &&
            sqlite3_exec(raw, "PRAGMA temp_store=MEMORY; PRAGMA query_only=ON;", nullptr, nullptr, nullptr) == SQLITE_OK &&
            sqlite3_create_function_v2(raw, "harken_scoped_path", 1, functionFlags,
                &scope_, ScopedPath, nullptr, nullptr, nullptr) == SQLITE_OK &&
            sqlite3_create_function_v2(raw, "harken_parent_path", 1, functionFlags | SQLITE_DETERMINISTIC,
                nullptr, ParentPath, nullptr, nullptr, nullptr) == SQLITE_OK &&
            sqlite3_exec(raw, "BEGIN;", nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    sqlite3* Database() const { return connection_.get(); }
    bool Expired() const { return Clock::now() >= deadline_; }

private:
    LibraryScope scope_;
    ReadOnlyVfs vfs_;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection_{nullptr, sqlite3_close};
    const Clock::time_point deadline_ = Clock::now() + QueryBudget;
};

Statement Prepare(sqlite3* database, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    sqlite3_prepare_v2(database, sql.c_str(), -1, &raw, nullptr);
    return Statement(raw, sqlite3_finalize);
}

bool BindText(sqlite3_stmt* statement, int parameter, std::string_view text) {
    return sqlite3_bind_text(statement, parameter, text.data(),
        static_cast<int>(text.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

std::optional<bool> HasTable(sqlite3* database, const char* name) {
    auto statement = Prepare(database, "SELECT 1 FROM main.sqlite_schema WHERE name=? AND type='table'");
    if (!statement || !BindText(statement.get(), 1, name)) {
        return std::nullopt;
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_ROW) {
        return true;
    }
    return step == SQLITE_DONE ? std::optional<bool>(false) : std::nullopt;
}

bool ValidQuery(const std::string& text, int limit) {
    return limit >= 1 && limit <= MaxResults && !text.empty() && text.size() <= 1024 &&
        text.find('\0') == std::string::npos && text.find_first_not_of(" \r\n\t") != std::string::npos;
}

std::string SearchPhrase(std::string_view text) {
    std::string phrase = "\"";
    for (const char character : text) {
        phrase += character;
        if (character == '"') {
            phrase += '"';
        }
    }
    return phrase + '"';
}

std::optional<std::string> FtsMetadataColumns(sqlite3* database) {
    constexpr std::array<std::string_view, 7> allowed = {
        "file_name", "title", "artist", "album", "description", "keywords", "category"
    };
    const auto statement = Prepare(database, "PRAGMA main.table_info(audio_files_fts)");
    if (!statement) {
        return std::nullopt;
    }
    std::string columns;
    int step = 0;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto name = SqlText(statement.get(), 1);
        const auto known = std::find(allowed.begin(), allowed.end(), name);
        if (known != allowed.end()) {
            columns += ' ';
            columns += *known;
        }
    }
    if (step != SQLITE_DONE) {
        return std::nullopt;
    }
    return columns.empty() ? columns : "{" + columns + " } : ";
}

std::string FolderScopePrefix(const fs::path& root) {
    auto prefix = root.generic_u8string();
#ifdef _WIN32
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char byte) {
        return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
    });
#endif
    if (prefix.back() != '/') {
        prefix += '/';
    }
    return prefix;
}

Json AudioRow(std::int64_t fileId, std::string_view relative, double duration) {
    const auto separator = relative.rfind('/');
    const auto name = separator == std::string_view::npos ? relative : relative.substr(separator + 1);
    return {{"file_id", fileId}, {"name", std::string(name)}, {"relative_path", std::string(relative)},
        {"duration_seconds", std::isfinite(duration) && duration >= 0 ? duration : 0.0}};
}

}

struct ExternalAiLibrary::Impl {
    fs::path root;
    std::string uri;
};

ExternalAiLibrary::ExternalAiLibrary() = default;
ExternalAiLibrary::~ExternalAiLibrary() = default;

bool ExternalAiLibrary::Open(const std::string& databasePath, const std::string& libraryRoot, std::string& error) {
    implementation_.reset();
    error = "expected an explicit local database and audio root";
    try {
        const auto database = LocalAbsolutePath(databasePath, false);
        const auto root = LocalAbsolutePath(libraryRoot, true);
        if (!database || !root) {
            return false;
        }
        error = "database unavailable for read-only access or not an audio catalog";
        auto candidate = std::make_unique<Impl>();
        candidate->root = *root;
        candidate->uri = ReadOnlyUri(*database);
        ReadSession session;
        if (!session.Open(candidate->uri, candidate->root) ||
            !HasTable(session.Database(), "audio_files").value_or(false)) {
            return false;
        }
        const auto probe = Prepare(session.Database(), "SELECT id,file_path,file_name,duration FROM main.audio_files LIMIT 1");
        if (!probe) {
            return false;
        }
        const int step = sqlite3_step(probe.get());
        if (step != SQLITE_ROW && step != SQLITE_DONE) {
            return false;
        }
        implementation_ = std::move(candidate);
        error.clear();
        return true;
    } catch (...) {
        return false;
    }
}

Json ExternalAiLibrary::SearchAudio(const std::string& query, int limit) {
    ReadSession session;
    if (!implementation_ || !ValidQuery(query, limit) ||
        !session.Open(implementation_->uri, implementation_->root)) {
        return Payload("query_failed");
    }
    auto* database = session.Database();
    const auto fts = HasTable(database, "audio_files_fts");
    const auto trigram = HasTable(database, "audio_files_name_trigram");
    if (!fts || !trigram) {
        return Payload("query_failed");
    }
    const auto characters = std::count_if(query.begin(), query.end(), [](const unsigned char byte) {
        return (byte & 0xc0) != 0x80;
    });
    std::string sql = "SELECT audio.id,harken_scoped_path(audio.file_path) AS relative_path,audio.duration "
        "FROM main.audio_files audio ";
    const auto ftsColumns = characters >= 3 && *fts ? FtsMetadataColumns(database) :
        std::optional<std::string>(std::string{});
    if (!ftsColumns) {
        return Payload("query_failed");
    }
    const bool useFts = !ftsColumns->empty();
    const bool useTrigram = characters >= 3 && *trigram;
    const bool useIndex = useFts || useTrigram;
    if (useIndex) {
        sql += "JOIN (";
        if (useFts) {
            sql += "SELECT rowid FROM main.audio_files_fts WHERE audio_files_fts MATCH ?1";
        }
        if (useFts && useTrigram) {
            sql += " UNION ";
        }
        if (useTrigram) {
            sql += "SELECT rowid FROM main.audio_files_name_trigram WHERE file_name MATCH ?2";
        }
        sql += ") hits ON hits.rowid=audio.id ";
    }
    sql += "WHERE audio.id>0 AND typeof(audio.id)='integer' "
        "AND instr(audio.file_name,'/')=0 AND instr(audio.file_name,char(92))=0 AND instr(audio.file_name,':')=0 "
        "AND harken_scoped_path(audio.file_path) IS NOT NULL ";
    if (!useIndex) {
        sql += "AND instr(lower(audio.file_name),lower(?2))>0 ";
    }
    sql += "ORDER BY audio.id LIMIT ?3";
    auto statement = Prepare(database, sql);
    const auto phrase = useIndex ? SearchPhrase(query) : std::string{};
    if (!statement ||
        (useFts && !BindText(statement.get(), 1, *ftsColumns + phrase)) ||
        ((useTrigram || !useIndex) && !BindText(statement.get(), 2, useTrigram ? phrase : query)) ||
        sqlite3_bind_int(statement.get(), 3, limit) != SQLITE_OK) {
        return Payload("query_failed");
    }
    Json results = Json::array();
    int step = 0;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto relative = SqlText(statement.get(), 1);
        if (!relative.empty()) {
            results.push_back(AudioRow(sqlite3_column_int64(statement.get(), 0), relative,
                sqlite3_column_double(statement.get(), 2)));
        }
    }
    return step == SQLITE_DONE && !session.Expired() ? Payload("ok", std::move(results)) : Payload("query_failed");
}

Json ExternalAiLibrary::FindFolders(const std::string& query, int limit) {
    ReadSession session;
    if (!implementation_ || !ValidQuery(query, limit) ||
        !session.Open(implementation_->uri, implementation_->root)) {
        return Payload("query_failed");
    }
    auto* database = session.Database();
    const auto folderIndex = HasTable(database, "folder_index");
    if (!folderIndex) {
        return Payload("query_failed");
    }
    std::string sql = "WITH scoped(folder) AS ("
        "SELECT harken_parent_path(harken_scoped_path(audio.file_path)) ";
    if (*folderIndex) {
        sql += "FROM main.folder_index folder CROSS JOIN main.audio_files audio ON audio.folder_path=folder.folder_path "
            "WHERE folder.file_count>0 AND ";
#ifdef _WIN32
        sql += "instr(lower(replace(folder.folder_path,char(92),'/')),?3)=1 AND ";
#else
        sql += "instr(folder.folder_path,?3)=1 AND ";
#endif
    } else {
        sql += "FROM main.audio_files audio WHERE ";
    }
    sql += "audio.id>0) SELECT scoped.folder,COUNT(*) FROM scoped "
        "WHERE instr(lower(scoped.folder),lower(?1))>0 "
        "GROUP BY scoped.folder ORDER BY scoped.folder COLLATE BINARY LIMIT ?2";
    auto statement = Prepare(database, sql);
    if (!statement || !BindText(statement.get(), 1, query) ||
        sqlite3_bind_int(statement.get(), 2, limit) != SQLITE_OK ||
        (*folderIndex && !BindText(statement.get(), 3, FolderScopePrefix(implementation_->root)))) {
        return Payload("query_failed");
    }
    Json results = Json::array();
    int step = 0;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto relative = SqlText(statement.get(), 0);
        if (!relative.empty()) {
            results.push_back({{"relative_path", std::string(relative)},
                {"file_count", sqlite3_column_int64(statement.get(), 1)}});
        }
    }
    return step == SQLITE_DONE && !session.Expired() ? Payload("ok", std::move(results)) : Payload("query_failed");
}

Json ExternalAiLibrary::FindSimilar(std::int64_t referenceId, int limit) {
    ReadSession session;
    if (!implementation_ || referenceId <= 0 || limit < 1 || limit > MaxResults ||
        !session.Open(implementation_->uri, implementation_->root)) {
        return Payload("query_failed");
    }
    auto* database = session.Database();
    auto catalogReference = Prepare(database,
        "SELECT 1 FROM main.audio_files WHERE id=? AND harken_scoped_path(file_path) IS NOT NULL");
    if (!catalogReference || sqlite3_bind_int64(catalogReference.get(), 1, referenceId) != SQLITE_OK) {
        return Payload("query_failed");
    }
    const int referenceStep = sqlite3_step(catalogReference.get());
    if (referenceStep != SQLITE_ROW) {
        return Payload(referenceStep == SQLITE_DONE ? "reference_not_found" : "query_failed");
    }
    const auto analysis = HasTable(database, "harken_similarity_v2");
    if (!analysis || session.Expired()) {
        return Payload("query_failed");
    }
    if (!*analysis) {
        return Payload("index_required");
    }
    const std::string selection = R"SQL(
        SELECT audio.id,audio.file_path,audio.file_name,audio.format,audio.duration,
            analysis.file_bytes,analysis.modified,analysis.fingerprint,analysis.summary,
            analysis.embedding,analysis.model_version,harken_scoped_path(audio.file_path) AS relative_path
        FROM main.harken_similarity_v2 analysis JOIN main.audio_files audio
            ON audio.id=analysis.file_id AND audio.file_path=analysis.file_path
            AND audio.file_size=analysis.file_bytes
        WHERE analysis.version=2 AND audio.id>0 AND harken_scoped_path(audio.file_path) IS NOT NULL
    )SQL";
    auto referenceStatement = Prepare(database, selection + " AND analysis.file_id=?1");
    if (!referenceStatement || sqlite3_bind_int64(referenceStatement.get(), 1, referenceId) != SQLITE_OK) {
        return Payload("query_failed");
    }
    const int indexedStep = sqlite3_step(referenceStatement.get());
    SimilarityAnalysisEntry reference;
    if (indexedStep == SQLITE_DONE || (indexedStep == SQLITE_ROW &&
        (!SimilarityAnalysisStore::DecodeStoredSummary(referenceStatement.get(), reference) || !reference.hasSemantic))) {
        return Payload("index_required");
    }
    if (indexedStep != SQLITE_ROW) {
        return Payload("query_failed");
    }
    const std::string referencePath(SqlText(referenceStatement.get(), 11));
    if (referencePath.empty()) {
        return Payload("reference_not_found");
    }
    auto candidates = Prepare(database, selection + " AND analysis.file_id<>?1 ORDER BY analysis.file_id LIMIT ?2");
    if (!candidates || sqlite3_bind_int64(candidates.get(), 1, referenceId) != SQLITE_OK ||
        sqlite3_bind_int(candidates.get(), 2, MaxCandidateRows + 1) != SQLITE_OK) {
        return Payload("query_failed");
    }
    struct Match {
        std::int64_t id;
        float score;
        Json row;
    };
    const auto better = [](const Match& left, const Match& right) {
        return left.score != right.score ? left.score > right.score : left.id < right.id;
    };
    std::vector<Match> matches;
    const auto cancelled = [&session] { return session.Expired(); };
    SimilarityOptions options;
    options.mode = SimilarityMode::Substitute;
    int step = 0;
    int examined = 0;
    bool truncated = false;
    while ((step = sqlite3_step(candidates.get())) == SQLITE_ROW) {
        if (cancelled()) {
            return Payload("query_failed");
        }
        if (examined == MaxCandidateRows) {
            truncated = true;
            break;
        }
        ++examined;
        SimilarityAnalysisEntry candidate;
        const auto relative = SqlText(candidates.get(), 11);
        if (relative.empty() || relative == referencePath ||
            !SimilarityAnalysisStore::DecodeStoredSummary(candidates.get(), candidate) || !candidate.hasSemantic) {
            continue;
        }
        const float category = std::clamp(AudioEmbeddingCosine(reference.embedding, candidate.embedding), 0.0f, 1.0f);
        const auto score = ScoreSimilarity(reference.acoustic, candidate.acoustic, options, category, cancelled);
        if (!score.valid || score.total < MinSubstituteScore) {
            continue;
        }
        auto row = AudioRow(candidate.file.id, relative, sqlite3_column_double(candidates.get(), 4));
        row["match_score"] = std::clamp(static_cast<double>(score.total) * 100.0, 0.0, 100.0);
        Match match{candidate.file.id, score.total, std::move(row)};
        matches.insert(std::lower_bound(matches.begin(), matches.end(), match, better), std::move(match));
        if (matches.size() > static_cast<std::size_t>(limit)) {
            matches.pop_back();
        }
    }
    if ((!truncated && step != SQLITE_DONE) || cancelled() || (truncated && matches.empty())) {
        return Payload("query_failed");
    }
    if (matches.empty()) {
        return Payload("no_candidates");
    }
    Json results = Json::array();
    for (auto& match : matches) {
        results.push_back(std::move(match.row));
    }
    return Payload("ok", std::move(results));
}

}
