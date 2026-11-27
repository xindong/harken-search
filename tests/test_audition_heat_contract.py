import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


class AuditionHeatContract(unittest.TestCase):
    def test_legacy_transfers_do_not_record_counts(self):
        for path in ("src/main.cpp", "src/WindowsMain.cpp", "src/AppState.h",
                     "src/WwiseTransferPanel.cpp", "src/Database.h", "src/Database.cpp"):
            with self.subTest(path=path):
                self.assertIsNone(re.search(
                    r"\b(?:queueUseCount|incrementUseCount|CountDragOutUse|CountWwiseTransferUseOnce)\b",
                    read(path)))
        self.assertIsNone(re.search(r"\buse_count\b", read("src/Database.cpp")))
        for path in ("src/main.cpp", "src/WwiseTransferPanel.cpp"):
            with self.subTest(path=path):
                self.assertIsNone(re.search(r"\b(?:incrementPlayCount|RecordSuccessfulPlayStats)\(", read(path)))
        windows = read("src/WindowsMain.cpp")
        self.assertNotIn("incrementPlayCount(", windows)
        self.assertEqual(windows.count("RecordSuccessfulPlayStats("), 1)

    def test_new_label_is_in_the_standard_dictionary(self):
        entries = json.loads(read("locale/harken.ui.strings.json"))["entries"]
        labels = {entry["id"]: entry for entry in entries}
        self.assertFalse("Col_UseCount" in labels, "old usage-count label is still present")
        label = labels["Col_AuditionHeat"]
        self.assertEqual((label["zh"], label["en"], label["ja"]),
                         ("试听热度", "Audition Heat", "試聴頻度"))

    def test_scrollable_table_keeps_its_existing_columns_and_order(self):
        main = read("src/main.cpp")
        setup = main.split("ImGui::TableSetupColumn(L(Col_FileName),", 1)[1]
        setup = setup.split("ImGui::TableSetupScrollFreeze(", 1)[0]
        columns = re.findall(r"ImGui::TableSetupColumn\(L\((Col_\w+)\)", setup)
        self.assertEqual(columns, [
            "Col_Waveform", "Col_Duration", "Col_Format", "Col_Size", "Col_SampleRate",
            "Col_Description", "Col_Keywords", "Col_Channels", "Col_Category", "Col_Artist",
            "Col_Album", "Col_BPM", "Col_MusicalKey", "Col_Mood", "Col_DateAdded",
            "Col_FilePath", "Col_Cover", "Col_Rating",
        ])
        sort_columns = read("src/AppState.h").split("enum class SortColumn {", 1)[1].split("};", 1)[0]
        self.assertEqual(re.findall(r"^\s*(\w+)\s*,?\s*(?://.*)?$", sort_columns, re.M), [
            "FileName", "History", "Duration", "Format", "Size", "SampleRate", "Description",
            "Keywords", "Channels", "Category", "Artist", "Album", "DateAdded", "Loudness",
            "RMS", "TruePeak", "Cover", "BPM", "MusicalKey", "Mood", "Rating", "Similarity",
            "FilePath",
        ])

    def test_audition_heat_reserves_space_at_the_visible_right_edge(self):
        main = read("src/main.cpp")
        self.assertIn("const bool showAuditionHeat = !browserFileTable;", main)
        self.assertIn(
            "const float resultsTableOuterWidth = showAuditionHeat ? auditionHeatRail.tableWidth : 0.0f;",
            main,
        )
        self.assertIn("ImVec2(resultsTableOuterWidth, 0.0f), resultsTableInnerWidth", main)
        self.assertIn("const float heatLeftX = resultsTable->OuterRect.Max.x;", main)
        self.assertIn("const float heatRightX = auditionHeatRail.rightX;", main)
        self.assertIn("auditionHeatDrawList->AddCircleFilled(", main)
        self.assertIn("drawRowChrome(rowVisualTopLeft, rowVisualHeight, isSelected, rowHovered, record.playCount);", main)


if __name__ == "__main__":
    unittest.main()
