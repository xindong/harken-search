import re
import sqlite3
import unittest
from pathlib import Path


DATABASE_SOURCE = Path(__file__).resolve().parents[1] / "src" / "Database.cpp"
SOURCE = DATABASE_SOURCE.read_text(encoding="utf-8")
SUGGESTION_SCHEMA = re.search(
    r"CREATE VIRTUAL TABLE IF NOT EXISTS (?:temp\.)?audio_files_(?:suggestion_)?vocab "
    r"USING fts5vocab\([^\n]+\);",
    SOURCE,
).group(0)
SUGGESTION_METHOD = SOURCE.split("Database::getSearchSuggestions(", 1)[1]
SQL_LITERAL = re.search(r"const char\* sql = ([^;]+);", SUGGESTION_METHOD).group(1)
SUGGESTION_SQL = "".join(re.findall(r'"([^"\n]*)"', SQL_LITERAL))


class SearchSuggestionCandidateTests(unittest.TestCase):
    def setUp(self):
        self.create_database("UTF-8")

    def create_database(self, encoding):
        self.database = sqlite3.connect(":memory:")
        self.addCleanup(self.database.close)
        self.database.execute(f"PRAGMA encoding = '{encoding}'")
        self.database.executescript(
            """
            CREATE VIRTUAL TABLE audio_files_fts USING fts5(
                file_name, file_path, folder_path, title, artist, album,
                description, keywords, category, search_data
            );
            """
        )
        self.database.execute(SUGGESTION_SCHEMA)

    def add_terms(self, *documents):
        self.database.executemany(
            "INSERT INTO audio_files_fts(file_name) VALUES (?)", ((name,) for name in documents)
        )

    def candidates(self, prefix, limit=8, max_bytes=254):
        upper = prefix[:-1] + chr(ord(prefix[-1]) + 1)
        bindings = (prefix, upper, limit, max_bytes, "|wav|mp3|wave|")
        return [row[0] for row in self.database.execute(SUGGESTION_SQL, bindings)]

    def test_filter_serials_before_limiting_candidates(self):
        noisy = [str(serial) for serial in range(1000, 1012)] * 3
        compact_names = [f"{serial}wav" for serial in range(1000, 1012)] * 3
        self.add_terms(*noisy, *compact_names, "10step", "10impact")
        self.assertEqual(self.candidates("10"), ["10step", "10impact"])
        self.assertEqual(self.candidates("100"), [])

    def test_filter_standalone_extension_before_limiting_candidates(self):
        self.add_terms("wav", "wav", "water", "waveform")
        self.assertEqual(self.candidates("wa", limit=1), ["water"])

    def test_suggest_source_words_not_compact_aliases_or_path_tokens(self):
        self.database.execute(
            "INSERT INTO audio_files_fts"
            "(file_name, file_path, folder_path, description, search_data) VALUES (?, ?, ?, ?, ?)",
            (
                "Water_Splash.wav",
                "waterfolder/waterpath/Water_Splash.wav",
                "waterfolder/waterpath",
                "water splashes",
                "watersplashwav",
            ),
        )
        self.assertCountEqual(self.candidates("water"), ["water"])
        self.assertCountEqual(self.candidates("splash"), ["splash", "splashes"])

    def test_deduplicate_across_columns_without_merging_distinct_models(self):
        self.database.executemany(
            "INSERT INTO audio_files_fts(file_name, title, keywords, search_data) "
            "VALUES (?, ?, ?, ?)",
            [
                ("SM57_mic.wav", "SM57", "microphone sm57", "sm57micwav"),
                ("sm57_mic_alt.wav", "sm57", "SM57", "sm57micaltwav"),
                ("SM57A_mic.wav", "SM57A", "SM57A", "sm57amicwav"),
            ],
        )
        self.assertEqual(self.candidates("sm57"), ["sm57", "sm57a"])
        self.assertEqual(self.candidates("microphone"), ["microphone"])

    def test_rank_by_document_frequency_then_length_without_duplicates(self):
        self.add_terms("sm57 sm57 sm58 sm7b", "sm57", "sm58")
        self.add_terms(*(f"smmodel{index}" for index in range(12)))
        candidates = self.candidates("sm")
        self.assertEqual(len(candidates), 8)
        self.assertEqual(candidates[:3], ["sm57", "sm58", "sm7b"])
        self.assertEqual(len(set(candidates)), 8)

    def test_uncompletable_terms_do_not_use_the_limit(self):
        self.add_terms("impact" + "x" * 80, "impact" + "x" * 80, "impacts")
        self.assertEqual(self.candidates("im", limit=1, max_bytes=8), ["impacts"])

    def test_overlong_source_word_does_not_take_a_candidate_slot(self):
        long_term = "over" + "audiosignal" * 11
        self.add_terms(long_term, long_term, "overhead")
        self.assertEqual(self.candidates("over", limit=1), ["overhead"])

    def test_count_characters_in_utf16_database(self):
        self.create_database("UTF-16le")
        term = "音" * 32
        self.add_terms(term, term + "响")
        self.assertEqual(self.candidates("音", max_bytes=249), [term])


if __name__ == "__main__":
    unittest.main()
