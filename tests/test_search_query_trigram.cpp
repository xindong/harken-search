// test_search_query_trigram.cpp
// F-002-ext: trigram-backed file_name substring/suffix search — parser tests.
// Verifies SearchQueryResult::nameContains is populated correctly for positive
// AND-conjunctions of >=3-codepoint bare tokens, and NOT populated otherwise.
// Returns 0 if all pass, 1 if any fail.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/SearchQuery.h"
#include "../src/Utils.h"

static int passCount = 0;
static int failCount = 0;

#define CHECK(cond, name) \
    do { \
        if (cond) { std::printf("PASS: %s\n", name); ++passCount; } \
        else       { std::printf("FAIL: %s\n", name); ++failCount; } \
    } while(0)

static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

int main() {
    std::printf("=== F-002-ext Trigram Substring Search Parser Tests ===\n\n");

    // --- 1. Bare digit-only token routes to nameContains AND stays in FTS ---
    {
        auto r = ParseSearchQueryToFts5("30017");
        CHECK(r.ok, "digit-only query parses OK");
        CHECK(contains(r.ftsMatch, "30017*"), "ftsMatch retains prefix token (description/keywords coverage)");
        CHECK(r.nameContains.size() == 1 && r.nameContains[0] == "30017",
              "nameContains contains '30017'");
    }

    // --- 2. Bare alphanumeric token (library ID style) ---
    {
        auto r = ParseSearchQueryToFts5("B30017");
        CHECK(r.ok, "alphanumeric query parses OK");
        CHECK(contains(r.ftsMatch, "B30017*"), "ftsMatch still has B30017*");
        CHECK(r.nameContains.size() == 1 && r.nameContains[0] == "B30017",
              "nameContains contains 'B30017'");
    }

    // --- 3. Bare short word (3 chars, also 3 bytes) ---
    {
        auto r = ParseSearchQueryToFts5("gun");
        CHECK(r.ok, "3-char word parses OK");
        CHECK(contains(r.ftsMatch, "gun*"), "ftsMatch has gun*");
        CHECK(r.nameContains.size() == 1 && r.nameContains[0] == "gun",
              "nameContains contains 'gun'");
    }

    // --- 4. Bare 2-char word: too short for trigram, stays FTS-only ---
    {
        auto r = ParseSearchQueryToFts5("hi");
        CHECK(r.ok, "2-char word parses OK");
        CHECK(contains(r.ftsMatch, "hi*"), "ftsMatch has hi*");
        CHECK(r.nameContains.empty(), "nameContains empty for <3-byte token");
    }

    // --- 5. Multi-word AND conjunction: both ≥3 tokens collected ---
    {
        auto r = ParseSearchQueryToFts5("gun 30017");
        CHECK(r.ok, "multi-word query parses OK");
        CHECK(contains(r.ftsMatch, "gun*"), "ftsMatch has gun*");
        CHECK(contains(r.ftsMatch, "30017*"), "ftsMatch has 30017*");
        CHECK(r.nameContains.size() == 2, "nameContains has 2 entries");
        CHECK(has(r.nameContains, "gun"), "nameContains has 'gun'");
        CHECK(has(r.nameContains, "30017"), "nameContains has '30017'");
    }

    // --- 6. Mixed length: short + long → only long collected ---
    {
        auto r = ParseSearchQueryToFts5("hi gun");
        CHECK(r.ok, "mixed-length query parses OK");
        CHECK(r.nameContains.size() == 1 && r.nameContains[0] == "gun",
              "nameContains only has 'gun' (short 'hi' excluded)");
    }

    // --- 7. Quoted phrase: respect user scoping, no trigram routing ---
    {
        auto r = ParseSearchQueryToFts5("\"B30017\"");
        CHECK(r.ok, "phrase query parses OK");
        CHECK(contains(r.ftsMatch, "\"B30017\""), "ftsMatch preserves phrase");
        CHECK(r.nameContains.empty(), "phrase does NOT go to nameContains");
    }

    // --- 8. Field-prefixed: stays in scoped FTS column, no trigram routing ---
    {
        auto r = ParseSearchQueryToFts5("artist:30017");
        CHECK(r.ok, "field-prefixed query parses OK");
        CHECK(contains(r.ftsMatch, "artist:"), "ftsMatch has artist: prefix");
        CHECK(r.nameContains.empty(), "artist:30017 does NOT go to nameContains");
    }

    // --- 9. OR expression: NOT collected (would break OR semantics with AND filter) ---
    {
        auto r = ParseSearchQueryToFts5("30017 OR footstep");
        CHECK(r.ok, "OR query parses OK");
        CHECK(contains(r.ftsMatch, "30017*"), "ftsMatch retains 30017*");
        CHECK(contains(r.ftsMatch, "footstep*"), "ftsMatch retains footstep*");
        CHECK(r.nameContains.empty(),
              "OR expression skips trigram collection (preserves OR semantics)");
    }

    // --- 10. NOT/minus exclusion: NOT collected (would flip exclude semantics) ---
    {
        auto r = ParseSearchQueryToFts5("-bad");
        CHECK(r.ok, "NOT query parses OK");
        CHECK(r.nameContains.empty(),
              "NOT/exclusion expression skips trigram collection");
    }

    // --- 11. Mixed positive AND with excluded term: NOT collected (contains Not node) ---
    {
        auto r = ParseSearchQueryToFts5("fire -bad");
        CHECK(r.ok, "positive+negative query parses OK");
        CHECK(contains(r.ftsMatch, "fire*"), "positive+negative keeps positive term");
        CHECK(contains(r.ftsMatch, "NOT bad*"), "positive+negative emits FTS5 NOT term");
        CHECK(!contains(r.ftsMatch, "AND (NOT"), "positive+negative avoids invalid AND (NOT ...)");
        CHECK(r.nameContains.empty(),
              "presence of NOT anywhere prevents trigram collection");
    }

    // --- 12. path: extraction coexists with trigram collection ---
    {
        auto r = ParseSearchQueryToFts5("path:sfx gun");
        CHECK(r.ok, "path: + keyword parses OK");
        CHECK(r.pathContains.size() == 1 && r.pathContains[0] == "sfx",
              "path: still extracted");
        CHECK(contains(r.ftsMatch, "gun*"), "gun* in ftsMatch");
        // path:sfx produces an empty Term (handled in parseFieldTerm), but
        // the AST root is still a positive AND-conjunction, so 'gun' is collected.
        CHECK(has(r.nameContains, "gun"), "'gun' collected into nameContains");
    }

    // --- 13. Parenthesized group (positive, AND): still collected ---
    {
        auto r = ParseSearchQueryToFts5("(gun 30017)");
        CHECK(r.ok, "group query parses OK");
        CHECK(r.nameContains.size() == 2, "both tokens collected through group");
    }

    // --- 14. Empty query: nothing collected ---
    {
        auto r = ParseSearchQueryToFts5("");
        CHECK(r.ok, "empty query parses OK");
        CHECK(r.nameContains.empty(), "empty query → empty nameContains");
        CHECK(r.ftsMatch.empty(), "empty query → empty ftsMatch");
    }

    // --- 15. Exact mode: ≥3 tokens still collected, no FTS star ---
    {
        auto r = ParseSearchQueryToFts5("gun", /*exactMode*/true);
        CHECK(r.ok, "exact-mode query parses OK");
        CHECK(r.ftsMatch == "gun", "exact: no star");
        CHECK(r.nameContains.empty(),
              "exact: bare token does NOT enter nameContains");
    }

    // --- 16. Case preservation in nameContains (trigram layer does the case fold) ---
    // Bug regression guard: schema must use `tokenize='trigram case_sensitive 0'`
    // so that lowercase query "b30005" matches indexed "B30005". The parser layer
    // keeps the original case; the DB tokenizer is responsible for folding.
    {
        auto rLower = ParseSearchQueryToFts5("b30005");
        CHECK(rLower.ok, "lowercase query parses OK");
        CHECK(rLower.nameContains.size() == 1 && rLower.nameContains[0] == "b30005",
              "lowercase token preserved verbatim in nameContains");

        auto rUpper = ParseSearchQueryToFts5("B30005");
        CHECK(rUpper.ok, "uppercase query parses OK");
        CHECK(rUpper.nameContains.size() == 1 && rUpper.nameContains[0] == "B30005",
              "uppercase token preserved verbatim in nameContains");
    }

    // --- 17. Full-pinyin tokens expand to initials for old search_data rows ---
    {
        auto r = ParseSearchQueryToFts5("daxiao");
        CHECK(r.ok, "daxiao query parses OK");
        CHECK(contains(r.ftsMatch, "daxiao*"), "daxiao ftsMatch keeps full pinyin");
        CHECK(contains(r.ftsMatch, "dx*"), "daxiao ftsMatch expands to initials dx*");
        CHECK(r.nameContains.empty(), "daxiao does NOT go to trigram nameContains");
    }

    // --- 18. Long full-pinyin token expands to initials without trigram fallback ---
    {
        auto r = ParseSearchQueryToFts5("kongbuzhongsheng");
        CHECK(r.ok, "kongbuzhongsheng query parses OK");
        CHECK(contains(r.ftsMatch, "kongbuzhongsheng*"),
              "kongbuzhongsheng ftsMatch keeps full pinyin");
        CHECK(contains(r.ftsMatch, "kbzs*"),
              "kongbuzhongsheng ftsMatch expands to initials kbzs*");
        CHECK(r.nameContains.empty(),
              "kongbuzhongsheng does NOT go to trigram nameContains");
    }

    // --- 19. Non-pinyin SFX abbreviation still uses trigram fallback ---
    {
        auto r = ParseSearchQueryToFts5("sfx");
        CHECK(r.ok, "sfx query parses OK");
        CHECK(has(r.nameContains, "sfx"), "sfx stays in nameContains");
    }

    // --- 20. Newly covered full-pinyin token expands to initials ---
    {
        auto r = ParseSearchQueryToFts5("guaiwu");
        CHECK(r.ok, "guaiwu query parses OK");
        CHECK(contains(r.ftsMatch, "guaiwu*"), "guaiwu ftsMatch keeps full pinyin");
        CHECK(contains(r.ftsMatch, "gw*"), "guaiwu ftsMatch expands to initials gw*");
        CHECK(r.nameContains.empty(), "guaiwu does NOT go to trigram nameContains");
    }

    // --- 21. Single-syllable pinyin tokens keep trigram fallback ---
    {
        auto r = ParseSearchQueryToFts5("men");
        CHECK(r.ok, "men query parses OK");
        CHECK(contains(r.ftsMatch, "men*"), "men ftsMatch keeps full token");
        CHECK(!contains(r.ftsMatch, "m*"), "men does NOT expand to single-letter initials");
        CHECK(has(r.nameContains, "men"), "men stays in nameContains");
    }

    // --- 22. Single CJK chars are FTS-only, not trigram terms ---
    {
        CHECK(InjectCJKSpaces(u8"嘈杂序列") == u8"嘈 杂 序 列 ",
              "CJK query is split into single-character FTS tokens");
        auto r = ParseSearchQueryToFts5(u8"嘈杂序列");
        CHECK(r.ok, "CJK sequence query parses OK");
        CHECK(r.nameContains.empty(),
              "CJK single-character tokens do NOT enter trigram nameContains");
    }

    // --- 23. Positive + multi-syllable pinyin + NOT keeps valid FTS5 syntax ---
    {
        auto r = ParseSearchQueryToFts5("fire daxiao -bad");
        CHECK(r.ok, "positive+pinyin+negative query parses OK");
        CHECK(contains(r.ftsMatch, "fire*"), "mixed NOT query keeps fire*");
        CHECK(contains(r.ftsMatch, "daxiao*"), "mixed NOT query keeps daxiao*");
        CHECK(contains(r.ftsMatch, "dx*"), "mixed NOT query expands daxiao to dx*");
        CHECK(contains(r.ftsMatch, "NOT bad*"), "mixed NOT query emits NOT bad*");
        CHECK(!contains(r.ftsMatch, "AND (NOT"), "mixed NOT query avoids invalid AND (NOT ...)");
        CHECK(r.nameContains.empty(), "mixed NOT query skips trigram collection");
    }

    std::printf("\n=== Summary: %d pass, %d fail ===\n", passCount, failCount);
    return failCount == 0 ? 0 : 1;
}
