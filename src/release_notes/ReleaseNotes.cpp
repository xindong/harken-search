#include "release_notes/ReleaseNotes.h"

#include "nlohmann_json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string>
#include <unordered_set>

namespace aris::release_notes {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaximumReleases = 256;
constexpr std::size_t kMaximumEntries = 4096;
constexpr std::size_t kMaximumTextBytes = 16 * 1024;
constexpr std::size_t kMaximumPageSize = 20;
constexpr std::string_view kPreferredFilename = "aris_release_notes.json";
constexpr std::string_view kLegacyFilename = "auro_release_notes.json";

ParseResult Failure(ParseError error, const char* detail) {
    ParseResult result;
    result.error = error;
    result.detail = detail;
    return result;
}

bool IsValidUtf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        std::size_t continuation = 0;
        std::uint32_t codePoint = 0;
        if (first <= 0x7F) {
            ++index;
            continue;
        }
        if ((first & 0xE0) == 0xC0) {
            continuation = 1;
            codePoint = first & 0x1F;
            if (codePoint == 0) return false;
        } else if ((first & 0xF0) == 0xE0) {
            continuation = 2;
            codePoint = first & 0x0F;
        } else if ((first & 0xF8) == 0xF0) {
            continuation = 3;
            codePoint = first & 0x07;
        } else {
            return false;
        }
        if (index + continuation >= text.size()) return false;
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const unsigned char next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0) != 0x80) return false;
            codePoint = (codePoint << 6) | (next & 0x3F);
        }
        if ((continuation == 2 && codePoint < 0x800) ||
            (continuation == 3 && codePoint < 0x10000) ||
            codePoint > 0x10FFFF ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            return false;
        }
        index += continuation + 1;
    }
    return true;
}

struct SemVersion {
    std::array<std::uint32_t, 3> parts{};
    bool operator<(const SemVersion& other) const { return parts < other.parts; }
    bool operator==(const SemVersion& other) const { return parts == other.parts; }
};

bool ParseVersion(std::string_view text, SemVersion& output) {
    std::size_t begin = 0;
    for (std::size_t index = 0; index < output.parts.size(); ++index) {
        const std::size_t end = index + 1 == output.parts.size()
            ? text.size()
            : text.find('.', begin);
        if (end == std::string_view::npos || end == begin) return false;
        const std::string_view token = text.substr(begin, end - begin);
        if (token.size() > 1 && token.front() == '0') return false;
        std::uint64_t value = 0;
        for (char digit : token) {
            if (digit < '0' || digit > '9') return false;
            value = value * 10 + static_cast<unsigned>(digit - '0');
            if (value > std::numeric_limits<std::uint32_t>::max()) return false;
        }
        output.parts[index] = static_cast<std::uint32_t>(value);
        begin = end + 1;
    }
    return begin == text.size() + 1;
}

bool IsValidDate(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (std::size_t index : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        if (!std::isdigit(static_cast<unsigned char>(text[index]))) return false;
    }
    const int month = (text[5] - '0') * 10 + (text[6] - '0');
    const int day = (text[8] - '0') * 10 + (text[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

bool ReadString(const json& object, const char* key, std::string& output,
                bool required = true) {
    const auto found = object.find(key);
    if (found == object.end()) return !required;
    if (!found->is_string()) return false;
    output = found->get<std::string>();
    return output.size() <= kMaximumTextBytes;
}

bool ReadStringArray(const json& object, const char* key,
                     std::vector<std::string>& output) {
    const auto found = object.find(key);
    if (found == object.end()) return true;
    if (!found->is_array()) return false;
    for (const auto& value : *found) {
        if (!value.is_string()) return false;
        const std::string text = value.get<std::string>();
        if (text.empty() || text.size() > kMaximumTextBytes) continue;
        if (std::find(output.begin(), output.end(), text) == output.end()) {
            output.push_back(text);
        }
    }
    return true;
}

bool ParseChannel(std::string_view value, ReleaseNotesChannel& output) {
    if (value == "stable") {
        output = ReleaseNotesChannel::Stable;
        return true;
    }
    if (value == "testing") {
        output = ReleaseNotesChannel::Testing;
        return true;
    }
    return false;
}

bool ParseSourceKind(std::string_view value, ReleaseNoteSourceKind& output) {
    if (value == "pull_request") output = ReleaseNoteSourceKind::PullRequest;
    else if (value == "commit") output = ReleaseNoteSourceKind::Commit;
    else if (value == "release_note") output = ReleaseNoteSourceKind::ReleaseNote;
    else if (value == "manual") output = ReleaseNoteSourceKind::Manual;
    else return false;
    return true;
}

bool ParseSources(const json& entryJson, ReleaseNoteEntry& entry) {
    const auto found = entryJson.find("sources");
    if (found == entryJson.end()) {
        entry.sources.push_back({ReleaseNoteSourceKind::ReleaseNote,
                                 "aris_release_notes.json", {}});
        return true;
    }
    if (!found->is_array()) return false;
    for (const auto& sourceJson : *found) {
        if (!sourceJson.is_object()) return false;
        std::string kind;
        std::string ref;
        std::string url;
        if (!ReadString(sourceJson, "kind", kind) ||
            !ReadString(sourceJson, "ref", ref) || ref.empty() ||
            !ReadString(sourceJson, "url", url, false)) {
            return false;
        }
        ReleaseNoteSourceKind sourceKind;
        if (!ParseSourceKind(kind, sourceKind)) return false;
        entry.sources.push_back({sourceKind, std::move(ref), std::move(url)});
    }
    return !entry.sources.empty();
}

bool ParseV3Entries(const json& releaseJson, ReleaseRecord& release,
                    std::size_t& entryCount) {
    const auto found = releaseJson.find("entries");
    if (found == releaseJson.end() || !found->is_array()) return false;
    for (const auto& entryJson : *found) {
        if (!entryJson.is_object()) return false;
        ReleaseNoteEntry entry;
        if (!ReadString(entryJson, "title", entry.title) ||
            !ReadString(entryJson, "description", entry.description) ||
            entry.title.empty() || entry.description.empty() ||
            !ReadStringArray(entryJson, "contributors", entry.contributors) ||
            !ParseSources(entryJson, entry)) {
            return false;
        }
        if (entry.contributors.empty()) entry.contributors = release.contributors;
        if (entry.contributors.empty()) entry.contributors.push_back("Harken Search Team");
        release.entries.push_back(std::move(entry));
        if (++entryCount > kMaximumEntries) return false;
    }
    return !release.entries.empty();
}

bool ParseLegacySections(const json& releaseJson, ReleaseRecord& release,
                         std::size_t& entryCount) {
    const auto found = releaseJson.find("sections");
    if (found == releaseJson.end()) return true;
    if (!found->is_array()) return false;
    for (const auto& sectionJson : *found) {
        if (!sectionJson.is_object()) return false;
        std::string sectionTitle;
        if (!ReadString(sectionJson, "title", sectionTitle) || sectionTitle.empty()) {
            return false;
        }
        const auto groups = sectionJson.find("items");
        if (groups == sectionJson.end() || !groups->is_array()) return false;
        for (const auto& groupJson : *groups) {
            if (!groupJson.is_object()) return false;
            std::string author;
            if (!ReadString(groupJson, "name", author)) return false;
            const auto list = groupJson.find("list");
            if (list == groupJson.end() || !list->is_array()) return false;
            if (!author.empty() && std::find(release.contributors.begin(),
                                             release.contributors.end(), author) ==
                                       release.contributors.end()) {
                release.contributors.push_back(author);
            }
            for (const auto& itemJson : *list) {
                ReleaseNoteEntry entry;
                std::string sourceRef = "aris_release_notes.json";
                if (itemJson.is_string()) {
                    entry.title = sectionTitle;
                    entry.description = itemJson.get<std::string>();
                    if (author.empty() && !release.contributors.empty()) {
                        entry.group = sectionTitle;
                    }
                } else if (itemJson.is_object()) {
                    if (!ReadString(itemJson, "title", entry.title) ||
                        !ReadString(itemJson, "description", entry.description) ||
                        !ReadString(itemJson, "source", sourceRef, false)) {
                        return false;
                    }
                    if (entry.title.empty()) entry.title = sectionTitle;
                    if (sourceRef.empty()) sourceRef = "aris_release_notes.json";
                } else {
                    return false;
                }
                if (entry.description.empty()) continue;
                if (!author.empty()) entry.contributors.push_back(author);
                if (entry.contributors.empty() && entry.group.empty()) {
                    entry.contributors.push_back("Harken Search Team");
                }
                entry.sources.push_back({ReleaseNoteSourceKind::ReleaseNote,
                                         std::move(sourceRef), {}});
                release.entries.push_back(std::move(entry));
                if (++entryCount > kMaximumEntries) return false;
            }
        }
    }
    return true;
}

bool ParseLegacyItems(const json& releaseJson, ReleaseRecord& release,
                      std::size_t& entryCount) {
    const auto found = releaseJson.find("items");
    if (found == releaseJson.end()) return true;
    if (!found->is_array()) return false;
    for (const auto& itemJson : *found) {
        if (!itemJson.is_object()) return false;
        ReleaseNoteEntry entry;
        std::string category;
        std::string text;
        std::string author;
        if (!ReadString(itemJson, "category", category, false) ||
            !ReadString(itemJson, "text", text) ||
            !ReadString(itemJson, "by", author, false) || text.empty()) {
            return false;
        }
        entry.title = category.empty() ? "更新" : category;
        entry.description = text;
        if (!author.empty()) entry.contributors.push_back(author);
        if (entry.contributors.empty()) entry.contributors = release.contributors;
        if (entry.contributors.empty()) entry.contributors.push_back("Harken Search Team");
        entry.sources.push_back({ReleaseNoteSourceKind::ReleaseNote,
                                 "aris_release_notes.json", {}});
        release.entries.push_back(std::move(entry));
        if (++entryCount > kMaximumEntries) return false;
    }
    return true;
}

}  // namespace

std::optional<int> CompareVersions(std::string_view left, std::string_view right) {
    SemVersion a, b;
    if (!ParseVersion(left, a) || !ParseVersion(right, b)) return std::nullopt;
    return a == b ? 0 : (a < b ? -1 : 1);
}

ParseResult ParseDocument(std::string_view jsonBytes) {
    if (jsonBytes.empty()) return Failure(ParseError::JsonInvalid, "release notes are empty");
    if (!IsValidUtf8(jsonBytes)) return Failure(ParseError::InvalidUtf8, "release notes are not valid UTF-8");

    const json root = json::parse(jsonBytes, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return Failure(ParseError::JsonInvalid, "release notes are not a JSON object");
    }
    const auto schema = root.find("schema_version");
    const auto releasesJson = root.find("releases");
    if (schema == root.end() || !schema->is_number_integer() ||
        releasesJson == root.end() || !releasesJson->is_array()) {
        return Failure(ParseError::MissingField, "release notes require schema_version and releases");
    }
    const int schemaVersion = schema->get<int>();
    if (schemaVersion < 1 || schemaVersion > 3) {
        return Failure(ParseError::SchemaUnsupported, "release notes schema is unsupported");
    }

    ReleaseNotesDocument document;
    document.schemaVersion = schemaVersion;
    if (const auto product = root.find("product"); product != root.end() &&
        (!product->is_string() || product->get<std::string>() != "aris")) {
        return Failure(ParseError::ProductMismatch, "release notes product is not aris");
    }
    if (const auto channel = root.find("channel"); channel != root.end()) {
        if (!channel->is_string() || !ParseChannel(channel->get<std::string>(), document.channel)) {
            return Failure(ParseError::ChannelMismatch, "release notes channel is invalid");
        }
    }

    if (releasesJson->size() > kMaximumReleases) {
        return Failure(ParseError::EntryLimitExceeded, "release history has too many releases");
    }

    std::unordered_set<std::string> versions;
    std::optional<SemVersion> previousVersion;
    std::size_t entryCount = 0;
    for (const auto& releaseJson : *releasesJson) {
        if (!releaseJson.is_object()) return Failure(ParseError::MissingField, "release entry is not an object");
        ReleaseRecord release;
        if (!ReadString(releaseJson, "version", release.version) ||
            !ReadString(releaseJson, "release_date", release.releaseDate) ||
            !ReadString(releaseJson, "headline", release.headline, false) ||
            !ReadStringArray(releaseJson, "contributors", release.contributors) ||
            !ReadStringArray(releaseJson, "notes", release.notes) ||
            !IsValidDate(release.releaseDate)) {
            return Failure(ParseError::MissingField, "release fields are missing or invalid");
        }
        SemVersion parsedVersion;
        if (!ParseVersion(release.version, parsedVersion)) {
            return Failure(ParseError::InvalidVersion, "release version is not strict semver");
        }
        if (!versions.insert(release.version).second) {
            return Failure(ParseError::DuplicateVersion, "release version is duplicated");
        }
        if (previousVersion && !(parsedVersion < *previousVersion)) {
            return Failure(ParseError::InvalidVersionOrder, "releases must be sorted newest first");
        }
        previousVersion = parsedVersion;

        for (const auto* countKey : {"pull_request_count", "commit_count"}) {
            const auto count = releaseJson.find(countKey);
            if (count != releaseJson.end()) {
                if (!count->is_number_unsigned()) return Failure(ParseError::MissingField, "release counts must be unsigned");
                const auto value = count->get<std::uint64_t>();
                if (std::string_view(countKey) == "pull_request_count") release.pullRequestCount = value;
                else release.commitCount = value;
            }
        }

        const bool hasV3Entries = releaseJson.find("entries") != releaseJson.end();
        if (hasV3Entries) {
            if (!ParseV3Entries(releaseJson, release, entryCount)) {
                return Failure(ParseError::MissingField, "schema v3 entries are invalid");
            }
        } else if (!ParseLegacySections(releaseJson, release, entryCount) ||
                   !ParseLegacyItems(releaseJson, release, entryCount)) {
            return Failure(ParseError::MissingField, "legacy release entries are invalid");
        }
        if (release.entries.empty()) {
            return Failure(ParseError::MissingField, "release must contain at least one entry");
        }
        if (release.contributors.empty()) release.contributors.push_back("Harken Search Team");
        document.releases.push_back(std::move(release));
    }

    if (document.releases.empty()) return Failure(ParseError::MissingField, "release history is empty");
    ParseResult result;
    result.document = std::move(document);
    return result;
}

ReleaseNotesRepository::ReleaseNotesRepository(ReleaseNotesFilePort& files)
    : files_(files) {}

RepositoryResult ReleaseNotesRepository::Load() const {
    std::string jsonBytes;
    if (files_.ReadText(kPreferredFilename, jsonBytes)) {
        RepositoryResult result;
        result.filename = std::string(kPreferredFilename);
        result.parsed = ParseDocument(jsonBytes);
        if (!result.parsed.ok()) {
            result.error = RepositoryError::ParseFailed;
            result.detail = result.parsed.detail;
        }
        return result;
    }

    if (!files_.ReadText(kLegacyFilename, jsonBytes)) {
        RepositoryResult result;
        result.error = RepositoryError::NotFound;
        result.detail = "release notes file was not found";
        return result;
    }

    RepositoryResult result;
    result.filename = std::string(kLegacyFilename);
    result.parsed = ParseDocument(jsonBytes);
    if (!result.parsed.ok()) {
        result.error = RepositoryError::ParseFailed;
        result.detail = result.parsed.detail;
    }
    return result;
}

ReleaseNotesService::ReleaseNotesService(ReleaseNotesDocument document,
                                         ReadStatePort& readState)
    : document_(std::move(document)), readState_(readState) {}

AutomaticOpenResult ReleaseNotesService::OpenAutomatic(std::string_view currentVersion,
                                                       std::size_t maxUnreadVersions) {
    AutomaticOpenResult result;
    result.currentVersion = std::string(currentVersion);
    automaticVisibleVersion_.clear();
    maxUnreadVersions = std::max<std::size_t>(1, std::min(maxUnreadVersions, kMaximumPageSize));

    const auto current = std::find_if(document_.releases.begin(), document_.releases.end(),
                                      [&](const ReleaseRecord& release) {
                                          return release.version == currentVersion;
                                      });
    if (current == document_.releases.end()) {
        result.detail = "current version has no release notes";
        return result;
    }

    const ReadState state = readState_.Load();
    if (state.lastReadVersion.empty()) {
        result.releases.push_back(*current);
    } else {
        SemVersion currentParsed;
        SemVersion lastParsed;
        if (!ParseVersion(currentVersion, currentParsed) ||
            !ParseVersion(state.lastReadVersion, lastParsed)) {
            result.detail = "read state contains an invalid version";
            return result;
        }
        for (auto it = current; it != document_.releases.end() &&
                                result.releases.size() < maxUnreadVersions; ++it) {
            SemVersion releaseParsed;
            if (!ParseVersion(it->version, releaseParsed)) continue;
            if (lastParsed < releaseParsed) result.releases.push_back(*it);
            if (releaseParsed == lastParsed) break;
        }
    }

    if (result.releases.empty()) {
        result.status = OpenStatus::NoUnread;
        return result;
    }
    automaticVisibleVersion_ = result.releases.front().version;
    result.status = OpenStatus::Ready;
    return result;
}

ReleaseNotesPage ReleaseNotesService::OpenManual(const ReleaseNotesCursor& cursor,
                                                 std::size_t pageSize) const {
    ReleaseNotesPage page;
    page.totalReleaseCount = document_.releases.size();
    pageSize = std::max<std::size_t>(1, std::min(pageSize, kMaximumPageSize));

    std::size_t start = 0;
    if (!cursor.beforeVersion.empty()) {
        const auto found = std::find_if(document_.releases.begin(), document_.releases.end(),
                                        [&](const ReleaseRecord& release) {
                                            return release.version == cursor.beforeVersion;
                                        });
        if (found == document_.releases.end()) return page;
        start = static_cast<std::size_t>(std::distance(document_.releases.begin(), found)) + 1;
    }

    const std::size_t end = std::min(start + pageSize, document_.releases.size());
    page.releases.insert(page.releases.end(), document_.releases.begin() + start,
                          document_.releases.begin() + end);
    page.hasMore = end < document_.releases.size();
    if (page.hasMore) page.next.beforeVersion = document_.releases[end - 1].version;
    return page;
}

bool ReleaseNotesService::AcknowledgeAutomatic(std::string_view version) {
    if (automaticVisibleVersion_.empty() || automaticVisibleVersion_ != version) return false;
    ReadState next = readState_.Load();
    if (!next.lastReadVersion.empty()) {
        SemVersion existing;
        SemVersion acknowledged;
        if (!ParseVersion(next.lastReadVersion, existing) || !ParseVersion(version, acknowledged)) return false;
        if (!(existing < acknowledged)) return true;
    }
    next.lastReadVersion = std::string(version);
    return readState_.Save(next);
}

}  // namespace aris::release_notes
