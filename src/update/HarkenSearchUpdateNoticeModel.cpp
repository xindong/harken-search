#include "update/HarkenSearchUpdateNoticeModel.h"
#include "release_notes/ReleaseNotes.h"
#include <algorithm>
#include <unordered_set>

namespace aris::update {
namespace {
std::string_view VersionWithoutPrefix(std::string_view version) {
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
        version.remove_prefix(1);
    return version;
}
std::string JoinContributors(const std::vector<std::string>& names) {
    std::string text;
    for (const auto& name : names) {
        if (name.empty()) continue;
        if (!text.empty()) text += u8" · ";
        text += name;
    }
    return text;
}
}

bool ParseHarkenSearchReleaseNotes(std::string_view bytes, std::vector<ReleaseNotesEntry>& out) {
    out.clear();
    // A display failure must never leak an exception into the main UI loop.
    try {
        const auto parsed = release_notes::ParseDocument(bytes);
        if (!parsed.ok()) return false;
        for (const auto& record : parsed.document.releases) {
            ReleaseNotesEntry entry;
            entry.version = record.version;
            entry.releaseDate = record.releaseDate;
            entry.headline = record.headline;
            entry.contributors = record.contributors;
            entry.notes = record.notes;
            for (const auto& item : record.entries) {
                entry.items.push_back({{}, item.title, item.description,
                                       JoinContributors(item.contributors)});
            }
            out.push_back(std::move(entry));
        }
        return !out.empty();
    } catch (...) {
        out.clear();
        return false;
    }
}

const ReleaseNotesEntry* FindHarkenSearchReleaseNotes(
    const std::vector<ReleaseNotesEntry>& releases, std::string_view version) {
    version = VersionWithoutPrefix(version);
    if (version.empty()) return nullptr;
    for (const auto& release : releases) {
        if (VersionWithoutPrefix(release.version) == version) return &release;
    }
    return nullptr;
}

void SyncHarkenSearchCandidateReleaseNotes(ReleaseNotesUiState& notes,
    std::string_view version, std::string_view json, bool readFailed) {
    if (notes.candidateNotesVersion == version && notes.candidateNotesSource == json &&
        notes.candidateSourceReadFailed == readFailed) return;
    notes.candidateNotesVersion = std::string(version);
    notes.candidateNotesSource = std::string(json);
    notes.candidateRelease.reset();
    notes.downloadedHistory.clear();
    notes.candidateNotesFailed = readFailed;
    notes.candidateSourceReadFailed = readFailed;
    if (json.empty() || version.empty()) return;
    std::vector<ReleaseNotesEntry> releases;
    if (!ParseHarkenSearchReleaseNotes(json, releases)) {
        notes.candidateNotesFailed = true;
        return;
    }
    if (const auto* release = FindHarkenSearchReleaseNotes(releases, version))
        notes.candidateRelease = *release;
    notes.downloadedHistory = std::move(releases);
}

const ReleaseNotesEntry* SelectHarkenSearchReleaseNotes(const ReleaseNotesUiState& notes,
    std::string_view version, bool candidate) {
    if (candidate && notes.candidateNotesVersion == version) {
        if (notes.candidateRelease) return &*notes.candidateRelease;
        // A fetched target document is authoritative even when it omits this
        // version. Never silently substitute a stale bundled copy.
        if (!notes.candidateNotesSource.empty() || notes.candidateNotesFailed) return nullptr;
    }
    return FindHarkenSearchReleaseNotes(notes.releases, version);
}
const ReleaseNotesEntry* SelectHarkenSearchLocalDisplayReleaseNotes(
    const ReleaseNotesUiState& notes, std::string_view version) {
    if (const auto* exact = FindHarkenSearchReleaseNotes(notes.releases, version))
        return exact;
    // The bundled history is validated newest first; falling back to the top
    // entry keeps the latest update content and contributors visible instead
    // of an empty panel when the installed version has no record of its own.
    if (notes.releases.empty()) return nullptr;
    return &notes.releases.front();
}
std::vector<const ReleaseNotesEntry*> BuildHarkenSearchReleaseNotesHistory(
    const ReleaseNotesUiState& notes, std::string_view installedVersion) {
    std::vector<const ReleaseNotesEntry*> result;
    installedVersion = VersionWithoutPrefix(installedVersion);
    std::unordered_set<std::string> seen;
    const auto append = [&](const std::vector<ReleaseNotesEntry>& source) {
        for (const auto& release : source) {
            const auto version = VersionWithoutPrefix(release.version);
            const auto order = release_notes::CompareVersions(version, installedVersion);
            if (order && *order <= 0 && seen.emplace(version).second)
                result.push_back(&release);
        }
    };
    append(notes.downloadedHistory);
    append(notes.releases);
    std::sort(result.begin(), result.end(), [](const auto* a, const auto* b) {
        return release_notes::CompareVersions(VersionWithoutPrefix(a->version),
                                              VersionWithoutPrefix(b->version)).value_or(0) > 0;
    });
    return result;
}
} // namespace aris::update
