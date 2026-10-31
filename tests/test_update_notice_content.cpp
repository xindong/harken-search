#include "update/HarkenSearchUpdateNoticeModel.h"
#include <cstdio>
#include <fstream>
#include <iterator>
using namespace aris::update;
namespace {
int failures = 0;
void check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); ++failures; }
}
}
int main(int argc, char** argv) {
    const std::string document = R"({"schema_version":2,"releases":[{"version":"1.0.37","release_date":"2026-10-22","headline":"New update","contributors":["Dev"],"sections":[{"title":"Changes","items":[{"name":"Dev","list":[{"title":"A title","description":"Complete details"}]}]}]}]})";
    ReleaseNotesUiState notes;
    check(ParseHarkenSearchReleaseNotes(document, notes.releases), "existing structured schema parses");
    auto* entry = FindHarkenSearchReleaseNotes(notes.releases, "v1.0.37");
    check(entry && entry->items[0].title == "A title" && entry->items[0].text == "Complete details" && entry->items[0].author == "Dev", "title, full text and author survive normalization");
    check(!FindHarkenSearchReleaseNotes(notes.releases, "1.0.38"), "missing target never selects newest record");
    SyncHarkenSearchCandidateReleaseNotes(notes, "1.0.37", document, false);
    check(SelectHarkenSearchReleaseNotes(notes, "1.0.37", true) != nullptr, "matching downloaded target can be previewed");
    SyncHarkenSearchCandidateReleaseNotes(notes, "1.0.38", document, false);
    check(!SelectHarkenSearchReleaseNotes(notes, "1.0.38", true), "target switch retires previous preview");
    SyncHarkenSearchCandidateReleaseNotes(notes, "1.0.37", "broken-json", false);
    check(notes.candidateNotesFailed && !SelectHarkenSearchReleaseNotes(notes, "1.0.37", true), "invalid fetched document cannot fall back to a stale local copy");
    check(SelectHarkenSearchReleaseNotes(notes, "1.0.37", false) != nullptr, "local history remains available after preview failure");
    check(SelectHarkenSearchLocalDisplayReleaseNotes(notes, "1.0.37") != nullptr &&
              SelectHarkenSearchLocalDisplayReleaseNotes(notes, "1.0.37")->version == "1.0.37",
          "local panel keeps the exact installed record when present");
    {
        ReleaseNotesUiState stale;
        const std::string staleDocument = R"({"schema_version":2,"releases":[{"version":"1.0.41","release_date":"2026-10-29","headline":"Newest","contributors":["Dev A","Dev B"],"sections":[{"title":"优化","items":[{"name":"Dev A","list":[{"title":"停靠","description":"停靠细节"}]}]}]},{"version":"1.0.40","release_date":"2026-10-28","headline":"Older","items":[{"text":"旧细节"}]}]})";
        check(ParseHarkenSearchReleaseNotes(staleDocument, stale.releases), "stale fallback document parses");
        const auto* exact = SelectHarkenSearchLocalDisplayReleaseNotes(stale, "1.0.41");
        check(exact && exact->version == "1.0.41", "stale fallback prefers the exact installed version");
        const auto* fallback = SelectHarkenSearchLocalDisplayReleaseNotes(stale, "1.0.42");
        check(fallback && fallback->version == "1.0.41" && !fallback->contributors.empty() &&
                  fallback->contributors.front() == "Dev A",
              "missing installed version falls back to the newest available record with contributors");
        ReleaseNotesUiState empty;
        check(!SelectHarkenSearchLocalDisplayReleaseNotes(empty, "1.0.42"), "no local data still shows no notes");
    }
    SyncHarkenSearchCandidateReleaseNotes(notes, "1.0.37", document, false);
    check(!notes.candidateNotesFailed && notes.candidateRelease.has_value(), "successful retry recovers failed notes");
    check(!ParseHarkenSearchReleaseNotes("{\"schema_version\":99999999999999999999}", notes.releases), "invalid input is a display error, not a crash");
    check(notes.releases.empty(), "failed parse clears previous entries");
    const std::string historyDocument = R"({"schema_version":2,"releases":[{"version":"1.0.37","release_date":"2026-10-22","headline":"Future","items":[{"text":"Upcoming"}]},{"version":"1.0.10","release_date":"2026-10-21","headline":"Ten","items":[{"text":"Ten details"}]},{"version":"1.0.9","release_date":"2026-10-20","headline":"Nine","items":[{"text":"Nine details"}]}]})";
    SyncHarkenSearchCandidateReleaseNotes(notes, "1.0.37", historyDocument, false);
    const auto history = BuildHarkenSearchReleaseNotesHistory(notes, "v1.0.10");
    check(history.size() == 2 && history[0]->version == "1.0.10" && history[1]->version == "1.0.9",
          "downloaded history is numeric newest-first and excludes uninstalled future versions");
    check(notes.candidateRelease && notes.candidateRelease->version == "1.0.37",
          "opening history does not replace the pending target or acknowledge an installation");
    ReleaseNotesEntry duplicate;
    duplicate.version = "1.0.10";
    duplicate.headline = "stale local title";
    notes.releases.push_back(duplicate);
    const auto merged = BuildHarkenSearchReleaseNotesHistory(notes, "1.0.37");
    check(merged.size() == 3 && merged[1]->headline == "Ten", "deduplicate by version with verified document priority");
    check(BuildHarkenSearchReleaseNotesHistory(notes, "unknown").empty(), "unknown installed version never advertises future history");
    SyncHarkenSearchCandidateReleaseNotes(notes, "1.0.38", "broken-json", false);
    const auto fallback = BuildHarkenSearchReleaseNotesHistory(notes, "1.0.10");
    check(fallback.size() == 1 && fallback[0]->headline == "stale local title",
          "bad replacement document retires old downloaded history and preserves installed notes");
    if (argc == 2) {
        std::ifstream file(argv[1], std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), {});
        check(ParseHarkenSearchReleaseNotes(bytes, notes.releases), "actual published release document parses");
        check(FindHarkenSearchReleaseNotes(notes.releases, "1.0.37") != nullptr, "actual published 1.0.37 is selectable");
    }
    std::printf("notice-content: %d failures\n", failures);
    return failures ? 1 : 0;
}
