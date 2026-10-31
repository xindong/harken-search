#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace aris::update {

struct ReleaseNotesItem {
    std::string category;
    std::string title;
    std::string text;
    std::string author;
};

struct ReleaseNotesEntry {
    std::string version;
    std::string releaseDate;
    std::string headline;
    std::vector<std::string> contributors;
    std::vector<ReleaseNotesItem> items;
    std::vector<std::string> notes;
};

struct ReleaseNotesUiState {
    bool wantOpen = false;
    bool showHistory = false;
    bool loadFailed = false;
    bool hasData = false;
    bool loadedOnce = false;
    int currentIndex = 0;
    std::string message;
    std::string summaryScrollKey;
    std::string errorScrollKey;
    std::string expandedReleaseVersion;
    std::vector<ReleaseNotesEntry> releases;
    std::string candidateNotesVersion;
    std::string candidateNotesSource;
    std::optional<ReleaseNotesEntry> candidateRelease;
    std::vector<ReleaseNotesEntry> downloadedHistory;
    bool candidateNotesFailed = false;
    bool candidateSourceReadFailed = false;
};

// The same parser is used for installed history and the verified update ZIP.
bool ParseHarkenSearchReleaseNotes(std::string_view bytes, std::vector<ReleaseNotesEntry>& out);
const ReleaseNotesEntry* FindHarkenSearchReleaseNotes(
    const std::vector<ReleaseNotesEntry>& releases, std::string_view version);
void SyncHarkenSearchCandidateReleaseNotes(ReleaseNotesUiState& notes,
    std::string_view version, std::string_view json, bool readFailed);
const ReleaseNotesEntry* SelectHarkenSearchReleaseNotes(const ReleaseNotesUiState& notes,
    std::string_view version, bool candidate);
// Display-only selection for the installed-version panel: exact record first,
// otherwise the newest bundled record so contributors and content stay visible.
const ReleaseNotesEntry* SelectHarkenSearchLocalDisplayReleaseNotes(
    const ReleaseNotesUiState& notes, std::string_view version);
std::vector<const ReleaseNotesEntry*> BuildHarkenSearchReleaseNotesHistory(
    const ReleaseNotesUiState& notes, std::string_view installedVersion);

}  // namespace aris::update
