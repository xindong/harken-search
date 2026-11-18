#include "Database.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_pass = 0;
static int g_fail = 0;

static void expect(bool cond, const std::string& label) {
    if (cond) {
        std::cout << "PASS: " << label << "\n";
        ++g_pass;
    } else {
        std::cout << "FAIL: " << label << "\n";
        ++g_fail;
    }
}

static AudioRecord makeRec(const std::string& folder,
                           const std::string& name,
                           const std::string& genre,
                           const std::string& instruments,
                           const std::string& keywords = {},
                           const std::string& description = {}) {
    AudioRecord r;
    r.fileName = name;
    r.displayName = name;
    r.filePath = folder + "\\" + name;
    r.folderPath = folder;
    r.extension = ".wav";
    r.format = "WAV";
    r.durationSeconds = 1.0f;
    r.sampleRate = 48000;
    r.channels = 2;
    r.fileSize = 1024;
    r.totalFrames = 48000;
    r.dateAdded = 1;
    r.fileMtime = 1;
    r.normGenre = genre;
    r.normInstruments = instruments;
    r.keywords = keywords;
    r.description = description;
    return r;
}

static bool hasFile(const std::vector<AudioRecord>& rows, const std::string& name) {
    for (const auto& row : rows) {
        if (row.fileName == name) return true;
    }
    return false;
}

static bool hasMatchSource(const std::vector<AudioRecord>& rows,
                           const std::string& name,
                           SearchMatchSource source) {
    for (const auto& row : rows) {
        if (row.fileName == name && row.matchSource == source) return true;
    }
    return false;
}

static bool hasFacet(const std::vector<Database::TagFacetEntry>& facets,
                     const std::string& tag,
                     int count) {
    for (const auto& facet : facets) {
        if (facet.tag == tag && facet.count == count) return true;
    }
    return false;
}

static std::string readRepoFile(const fs::path& relativePath) {
    const fs::path repoRoot = fs::u8path(__FILE__).parent_path().parent_path();
    std::ifstream in(repoRoot / relativePath, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

static std::string extractSourceSlice(const std::string& source,
                                      const std::string& beginToken,
                                      const std::string& endToken) {
    const size_t begin = source.find(beginToken);
    if (begin == std::string::npos) return {};
    const size_t end = source.find(endToken, begin);
    if (end == std::string::npos) return source.substr(begin);
    return source.substr(begin, end - begin);
}

int main() {
    fs::path tmpDir = fs::temp_directory_path() / "auro_search_semantic_matrix";
    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir, ec);

    const std::string dbPath = (tmpDir / "test.db").string();

    Database db;
    if (!db.open(dbPath)) {
        std::cerr << "FAIL: db.open failed: " << db.getLastError() << "\n";
        return 1;
    }

    expect(db.addFile(makeRec("E:\\Lib\\Cinematic", "cinematic_boom.wav",
                              "impact", "drums", "metallic thunder", "huge boom impact")),
           "fixture add cinematic_boom");
    expect(db.addFile(makeRec("E:\\Lib\\Whoosh", "cinematic_whoosh.wav",
                              "cinematic", "synth", "air sweep", "fast whoosh")),
           "fixture add cinematic_whoosh");
    expect(db.addFile(makeRec("E:\\Lib\\Textures", "metallic_tail.wav",
                              "texture", "metal", "metallic shimmer", "ringing metal tail")),
           "fixture add metallic_tail");
    expect(db.addFile(makeRec("E:\\Lib\\IDs", "B30017_Gun_Shot.wav",
                              "impact", "drums", "gun crack", "catalog id shot")),
           "fixture add B30017_Gun_Shot");
    expect(db.addFile(makeRec("E:\\Lib\\Sound Ideas BBC\\Vol-07", "track_01.wav",
                              "archive", "voice", "broadcast archive", "bbc archive track")),
           "fixture add track_01");
    expect(db.addFile(makeRec("E:\\Lib\\Props", "card_shuffle.wav",
                              "vehicle", "foley", "paper card motion", "index card shuffle")),
           "fixture add card_shuffle");
    {
        AudioRecord hiddenMeta = makeRec("E:\\Lib\\Stealth", "plain_tail.wav",
                                         "texture", "cloth", "", "");
        hiddenMeta.meta["designer"] = "stealthhit";
        expect(db.addFile(hiddenMeta),
               "fixture add plain_tail with hidden searchable metadata");
    }

    {
        SearchFilter f;
        f.keyword = "30017";
        Database::BuiltSearchQuery bq = db.buildSearchWhere(f);
        auto rows = db.searchFiles(f, 20, 0);
        auto randomRows = db.searchFilesRandom(f, 10);
        int64_t count = db.searchFilesCount(f);

        expect(!bq.parseError, "matrix/union: parse succeeds");
        expect(bq.hasFtsJoin, "matrix/union: text-driven branch marks hasFtsJoin");
        expect(!bq.canUseFtsSnippet, "matrix/union: UNION branch disables FTS snippet");
        expect(bq.fromClause.find("UNION") != std::string::npos,
               "matrix/union: fromClause uses UNION for FTS + trigram");
        expect(bq.fromClause.find("audio_files_name_trigram") != std::string::npos,
               "matrix/union: fromClause references trigram sidecar");
        expect(count == static_cast<int64_t>(rows.size()),
               "matrix/union: count matches list size");
        expect(hasFile(rows, "B30017_Gun_Shot.wav"),
               "matrix/union: trigram candidate appears in list");
        expect(!randomRows.empty() && hasFile(randomRows, "B30017_Gun_Shot.wav"),
               "matrix/union: random branch shares same candidate set");
    }

    {
        SearchFilter f;
        f.keyword = "genre:impact";
        Database::BuiltSearchQuery bq = db.buildSearchWhere(f);
        auto rows = db.searchFiles(f, 20, 0);
        int64_t count = db.searchFilesCount(f);
        auto instrumentFacets = db.getTagFacets("norm_instruments", f, 20);

        expect(!bq.parseError, "matrix/power-field: parse succeeds");
        expect(!bq.hasFtsJoin, "matrix/power-field: pure power field does not use text branch");
        expect(bq.fromClause == "FROM audio_files af",
               "matrix/power-field: pure power field uses plain audio_files source");
        expect(bq.whereClause.find("norm_genre") != std::string::npos,
               "matrix/power-field: whereClause contains norm_genre filter");
        expect(bq.whereClause.find("audio_files_name_trigram") == std::string::npos,
               "matrix/power-field: pure power field does not use trigram sidecar");
        expect(rows.size() == 2 && hasFile(rows, "cinematic_boom.wav") && hasFile(rows, "B30017_Gun_Shot.wav"),
               "matrix/power-field: genre filter returns only impact rows");
        expect(count == static_cast<int64_t>(rows.size()),
               "matrix/power-field: count matches list size");
        expect(hasFacet(instrumentFacets, "drums", 2),
               "matrix/power-field: facets stay aligned with filtered candidate set");
        expect(hasMatchSource(rows, "cinematic_boom.wav", SearchMatchSource::Metadata) &&
               hasMatchSource(rows, "B30017_Gun_Shot.wav", SearchMatchSource::Metadata),
               "matrix/power-field: metadata-only rows surface metadata match source");
    }

    {
        SearchFilter f;
        f.keyword = "genre:impact OR metallic";
        Database::BuiltSearchQuery bq = db.buildSearchWhere(f);
        auto rows = db.searchFiles(f, 20, 0);
        int64_t count = db.searchFilesCount(f);

        expect(!bq.parseError, "matrix/power-or: parse succeeds");
        expect(!bq.hasFtsJoin && !bq.canUseFtsSnippet,
               "matrix/power-or: the outer query does not expose arm-local FTS helpers");
        expect(bq.whereClause.find("norm_genre") != std::string::npos &&
               bq.whereClause.find(" UNION ") != std::string::npos,
               "matrix/power-or: each OR arm retains its own field/text semantics");
        expect(rows.size() == 3,
               "matrix/power-or: genre-only and text-only hits are combined without duplicates");
        expect(hasFile(rows, "cinematic_boom.wav") &&
               hasFile(rows, "metallic_tail.wav") &&
               hasFile(rows, "B30017_Gun_Shot.wav"),
               "matrix/power-or: OR retains the metadata-only impact row and both metallic hits");
        expect(count == static_cast<int64_t>(rows.size()),
               "matrix/power-or: count matches list size");
        auto randomRows = db.searchFilesRandom(f, 20);
        expect(randomRows.size() == rows.size() &&
               hasFile(randomRows, "B30017_Gun_Shot.wav") &&
               hasFile(randomRows, "metallic_tail.wav"),
               "matrix/power-or: random results share the combined candidate set");
    }

    {
        SearchFilter f;
        f.keyword = "NOT metallic";
        Database::BuiltSearchQuery bq = db.buildSearchWhere(f);
        auto rows = db.searchFiles(f, 20, 0);
        int64_t count = db.searchFilesCount(f);

        expect(!bq.parseError, "matrix/negative: parse succeeds");
        expect(!bq.hasFtsJoin, "matrix/negative: pure negative query does not use text-driving branch");
        expect(bq.whereClause.find("NOT (") != std::string::npos,
               "matrix/negative: whereClause uses SQL NOT LIKE fallback");
        expect(!hasFile(rows, "cinematic_boom.wav") && !hasFile(rows, "metallic_tail.wav"),
               "matrix/negative: metallic rows are excluded");
        expect(hasFile(rows, "cinematic_whoosh.wav") &&
               hasFile(rows, "B30017_Gun_Shot.wav") &&
               hasFile(rows, "track_01.wav"),
               "matrix/negative: unrelated rows remain");
        expect(count == static_cast<int64_t>(rows.size()),
               "matrix/negative: count matches list size");
    }

    {
        SearchFilter f;
        f.keyword = "path:bbc";
        auto rows = db.searchFiles(f, 20, 0);
        int64_t count = db.searchFilesCount(f);

        expect(rows.size() == 1 && hasFile(rows, "track_01.wav"),
               "matrix/path-only: path filter returns BBC track");
        expect(count == static_cast<int64_t>(rows.size()),
               "matrix/path-only: count matches list size");
        expect(hasMatchSource(rows, "track_01.wav", SearchMatchSource::Path),
               "matrix/path-only: result surfaces path match source");
    }

    {
        SearchFilter f;
        f.keyword = "path:bbc track";
        Database::BuiltSearchQuery bq = db.buildSearchWhere(f);
        auto rows = db.searchFiles(f, 20, 0);
        int64_t count = db.searchFilesCount(f);

        expect(!bq.parseError, "matrix/path+text: parse succeeds");
        expect(bq.hasFtsJoin, "matrix/path+text: text term still drives text branch");
        expect(bq.whereClause.find("file_path") != std::string::npos,
               "matrix/path+text: whereClause includes path LIKE constraint");
        expect(rows.size() == 1 && hasFile(rows, "track_01.wav"),
               "matrix/path+text: path lane and text lane intersect on BBC track");
        expect(count == static_cast<int64_t>(rows.size()),
               "matrix/path+text: count matches list size");
    }

    {
        SearchFilter fuzzy;
        fuzzy.keyword = "car";
        Database::BuiltSearchQuery fuzzyBq = db.buildSearchWhere(fuzzy);
        auto fuzzyRows = db.searchFiles(fuzzy, 20, 0);
        int64_t fuzzyCount = db.searchFilesCount(fuzzy);
        auto fuzzyGenreFacets = db.getTagFacets("norm_genre", fuzzy, 20);

        expect(!fuzzyBq.parseError, "matrix/exact-vs-fuzzy fuzzy: parse succeeds");
        expect(fuzzyBq.fromClause.find("UNION") != std::string::npos,
               "matrix/exact-vs-fuzzy fuzzy: substring recall still uses UNION branch");
        expect(fuzzyBq.fromClause.find("audio_files_name_trigram") != std::string::npos,
               "matrix/exact-vs-fuzzy fuzzy: trigram sidecar remains active");
        expect(hasFile(fuzzyRows, "card_shuffle.wav"),
               "matrix/exact-vs-fuzzy fuzzy: bare token can recall card_shuffle");
        expect(fuzzyCount == static_cast<int64_t>(fuzzyRows.size()),
               "matrix/exact-vs-fuzzy fuzzy: count matches list size");
        expect(hasFacet(fuzzyGenreFacets, "vehicle", 1),
               "matrix/exact-vs-fuzzy fuzzy: facets align with widened candidate set");
        expect(hasMatchSource(fuzzyRows, "card_shuffle.wav", SearchMatchSource::FuzzyName),
               "matrix/exact-vs-fuzzy fuzzy: row surfaces fuzzy-name match source");
    }

    {
        SearchFilter exact;
        exact.keyword = "car";
        exact.exactMode = true;
        Database::BuiltSearchQuery exactBq = db.buildSearchWhere(exact);
        auto exactRows = db.searchFiles(exact, 20, 0);
        int64_t exactCount = db.searchFilesCount(exact);
        auto exactGenreFacets = db.getTagFacets("norm_genre", exact, 20);

        expect(!exactBq.parseError, "matrix/exact-vs-fuzzy exact: parse succeeds");
        expect(exactBq.fromClause.find("UNION") == std::string::npos,
               "matrix/exact-vs-fuzzy exact: exact mode skips UNION widening");
        expect(exactBq.fromClause.find("audio_files_name_trigram") == std::string::npos,
               "matrix/exact-vs-fuzzy exact: exact mode skips trigram sidecar");
        expect(exactRows.empty(),
               "matrix/exact-vs-fuzzy exact: bare token does not recall card_shuffle");
        expect(exactCount == 0,
               "matrix/exact-vs-fuzzy exact: count stays empty");
        expect(!hasFacet(exactGenreFacets, "vehicle", 1),
               "matrix/exact-vs-fuzzy exact: facets do not reuse fuzzy candidate set");
    }

    {
        SearchFilter f;
        f.keyword = "stealthhit";
        Database::BuiltSearchQuery bq = db.buildSearchWhere(f);
        auto rows = db.searchFiles(f, 20, 0);
        int64_t count = db.searchFilesCount(f);

        expect(!bq.parseError, "matrix/hidden-search-data: parse succeeds");
        expect(bq.whereClause.find("af.search_data") == std::string::npos,
               "matrix/hidden-search-data: candidate guards do not rely on opaque search_data");
        expect(rows.empty(),
               "matrix/hidden-search-data: hidden searchable metadata alone does not surface rows");
        expect(count == 0,
               "matrix/hidden-search-data: count stays aligned with visible candidate set");
    }

    {
        const std::string source = readRepoFile("src/main.cpp");
        expect(!source.empty(),
               "matrix/explainability-whitespace: main.cpp source is readable");
        const std::string inlineHighlightBlock = extractSourceSlice(
            source,
            "static void RenderInlineHighlightedText",
            "static bool FindAsciiCaseInsensitiveSpan");
        expect(source.find("const std::string trimmed = TrimCopy(term);") != std::string::npos,
               "matrix/explainability-whitespace: fallback snippet trims outer query whitespace");
        expect(source.find("if (FindAsciiCaseInsensitiveSpan(*label, term, start))") != std::string::npos,
               "matrix/explainability-whitespace: fallback snippet matches using trimmed term candidates");
        expect(source.find("for (const SearchQueryChip& chip : BuildSearchQueryChips(normalizedQuery))") != std::string::npos,
               "matrix/explainability-whitespace: fallback snippet considers per-chip terms for spaced queries");
        const std::string searchTooltipBlock = extractSourceSlice(
            source,
            "static bool BeginSearchExplainabilityTooltip()",
            "static bool HasBlockingSearchExplainabilityPopup()");
        expect(!searchTooltipBlock.empty() &&
               searchTooltipBlock.find("return false;") != std::string::npos &&
               searchTooltipBlock.find("ImGui::BeginTooltip()") == std::string::npos,
               "matrix/explainability-tooltip: local build suppresses search hover popups");
        expect(!inlineHighlightBlock.empty(),
               "matrix/explainability-tooltip-wrap: inline highlight render block is readable");
        expect(source.find("static void ParseInlineHighlightMarkers") != std::string::npos &&
               inlineHighlightBlock.find("ParseInlineHighlightMarkers(s, plainSnippet, highlightSpans);") != std::string::npos,
               "matrix/explainability-tooltip-wrap: tooltip snippet preserves explicit highlight markers");
        expect(inlineHighlightBlock.find("CalcWordWrapPosition(") != std::string::npos &&
               inlineHighlightBlock.find("AddRectFilled(") != std::string::npos &&
               inlineHighlightBlock.find("AddText(font, fontSize") != std::string::npos,
               "matrix/explainability-tooltip-wrap: tooltip snippet keeps wrap-aware highlight painting");
        expect(inlineHighlightBlock.find("SameLine(0, 0)") == std::string::npos,
               "matrix/explainability-tooltip-wrap: tooltip snippet no longer uses SameLine split rendering");
    }

    db.close();
    fs::remove_all(tmpDir, ec);

    std::cout << "\n=== Summary: " << g_pass << " pass, " << g_fail << " fail ===\n";
    return g_fail == 0 ? 0 : 1;
}
