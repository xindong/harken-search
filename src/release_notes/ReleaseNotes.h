#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aris::release_notes {

enum class ReleaseNotesChannel {
    Stable,
    Testing,
};

enum class ReleaseNoteSourceKind {
    PullRequest,
    Commit,
    ReleaseNote,
    Manual,
};

struct ReleaseNoteSource {
    ReleaseNoteSourceKind kind = ReleaseNoteSourceKind::ReleaseNote;
    std::string ref;
    std::string url;
};

struct ReleaseNoteEntry {
    std::string title;
    std::string description;
    std::vector<std::string> contributors;
    std::vector<ReleaseNoteSource> sources;
    std::string group;
};

struct ReleaseRecord {
    std::string version;
    std::string releaseDate;
    std::string headline;
    std::optional<std::uint64_t> pullRequestCount;
    std::optional<std::uint64_t> commitCount;
    std::vector<std::string> contributors;
    std::vector<ReleaseNoteEntry> entries;
    std::vector<std::string> notes;
};

struct ReleaseNotesDocument {
    int schemaVersion = 0;
    ReleaseNotesChannel channel = ReleaseNotesChannel::Stable;
    std::vector<ReleaseRecord> releases;
};

enum class ParseError {
    None,
    JsonInvalid,
    InvalidUtf8,
    SchemaUnsupported,
    ProductMismatch,
    ChannelMismatch,
    MissingField,
    InvalidVersion,
    InvalidVersionOrder,
    DuplicateVersion,
    InvalidDate,
    InvalidSource,
    EntryLimitExceeded,
};

struct ParseResult {
    ParseError error = ParseError::None;
    ReleaseNotesDocument document;
    std::string detail;

    bool ok() const { return error == ParseError::None; }
};

ParseResult ParseDocument(std::string_view jsonBytes);
// Strict numeric x.y.z ordering; an invalid input has no ordering.
std::optional<int> CompareVersions(std::string_view left, std::string_view right);

struct ReadState {
    std::string lastReadVersion;
};

class ReleaseNotesFilePort {
public:
    virtual ~ReleaseNotesFilePort() = default;
    virtual bool ReadText(std::string_view fileName, std::string& outText) = 0;
};

enum class RepositoryError {
    None,
    NotFound,
    ParseFailed,
};

struct RepositoryResult {
    RepositoryError error = RepositoryError::None;
    ParseResult parsed;
    std::string filename;
    std::string detail;

    bool ok() const { return error == RepositoryError::None && parsed.ok(); }
};

class ReleaseNotesRepository {
public:
    explicit ReleaseNotesRepository(ReleaseNotesFilePort& files);

    RepositoryResult Load() const;

private:
    ReleaseNotesFilePort& files_;
};

class ReadStatePort {
public:
    virtual ~ReadStatePort() = default;
    virtual ReadState Load() = 0;
    virtual bool Save(const ReadState& state) = 0;
};

struct ReleaseNotesCursor {
    std::string beforeVersion;
};

struct ReleaseNotesPage {
    std::vector<ReleaseRecord> releases;
    ReleaseNotesCursor next;
    bool hasMore = false;
    std::size_t totalReleaseCount = 0;
};

enum class OpenStatus {
    Ready,
    NoUnread,
    Failed,
};

struct AutomaticOpenResult {
    OpenStatus status = OpenStatus::Failed;
    std::string currentVersion;
    std::vector<ReleaseRecord> releases;
    std::string detail;
};

class ReleaseNotesService {
public:
    ReleaseNotesService(ReleaseNotesDocument document, ReadStatePort& readState);

    AutomaticOpenResult OpenAutomatic(std::string_view currentVersion,
                                       std::size_t maxUnreadVersions = 5);
    ReleaseNotesPage OpenManual(const ReleaseNotesCursor& cursor,
                                std::size_t pageSize = 5) const;
    bool AcknowledgeAutomatic(std::string_view version);

private:
    ReleaseNotesDocument document_;
    ReadStatePort& readState_;
    std::string automaticVisibleVersion_;
};

}  // namespace aris::release_notes
