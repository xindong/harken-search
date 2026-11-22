from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src/main.cpp"
HEADER = ROOT / "src/DatabaseSearchFilterBuilder.h"
SOURCE = ROOT / "src/DatabaseSearchFilterBuilder.cpp"


def test_database_filter_builder_has_independent_owner():
    assert HEADER.exists()
    assert SOURCE.exists()
    source = SOURCE.read_text(encoding="utf-8")
    for contract in (
        "SearchFilter BuildCurrentDbFilter(const AppState& state)",
        "CompileSearchBufferKeywordForFilter",
        "NormalizeFolderPathForDb",
        "ApplyFileTypePolicyToFilter",
        "activeUserCollectionIdx",
        "genrePillState",
        "instrumentPillState",
    ):
        assert contract in source
    assert '"main.cpp"' not in source
    assert '"WindowsMain.h"' not in source
    assert '"platform/' not in source


def test_main_only_consumes_database_filter_builder():
    main = MAIN.read_text(encoding="utf-8")
    assert '#include "DatabaseSearchFilterBuilder.h"' in main
    assert "static SearchFilter BuildCurrentDbFilter(" not in main
    assert "using aura::search::BuildCurrentDbFilter;" in main


def test_wv_format_filter_uses_one_order_for_sidebar_query_and_qa():
    header = HEADER.read_text(encoding="utf-8")
    builder = SOURCE.read_text(encoding="utf-8")
    main = MAIN.read_text(encoding="utf-8")
    windows = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    assert '"M4A", "WV"' in header
    assert "AudioFormatFilterAt(state.formatFilterIndex)" in builder
    assert "aura::search::kAudioFormatFilters" in main
    assert "AudioFormatFilterAt(state.formatFilterIndex)" in main
    assert "aura::search::AudioFormatFilterAt(index)" in windows
    assert "aura::search::kAudioFormatFilters" in windows
