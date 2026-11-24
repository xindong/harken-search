from pathlib import Path
import json


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
APP_STATE = (ROOT / "src" / "AppState.h").read_text(encoding="utf-8")
DIALOG_STATE = (ROOT / "src" / "SettingsDialogState.h").read_text(encoding="utf-8")
SETTINGS_CATALOG = (ROOT / "src" / "SettingsCatalog.h").read_text(encoding="utf-8")
SETTINGS_FIELDS = (ROOT / "src" / "SettingsFields.inl").read_text(encoding="utf-8")
INDEXER = (ROOT / "src" / "SimilarityIndexer.h").read_text(encoding="utf-8")
INDEXER_IMPL = (ROOT / "src" / "SimilarityIndexer.cpp").read_text(encoding="utf-8")
LOCALIZED = {
    entry["id"]: entry
    for entry in json.loads(
        (ROOT / "locale" / "harken.ui.strings.json").read_text(encoding="utf-8")
    )["entries"]
}


def test_default_matches_engine_default_and_is_persisted():
    assert "inline constexpr int kDefaultSimilarityConcurrency = 4;" in INDEXER
    assert "int similarityAnalysisConcurrency = 4;" in APP_STATE
    assert 'kv["similarityAnalysisConcurrency"]' in MAIN
    assert 'items.push_back({"similarityAnalysisConcurrency"' in MAIN
    assert "kMaxSimilarityConcurrency" in MAIN
    assert "PREF(similarityAnalysisConcurrency)" in DIALOG_STATE


def test_setting_row_is_searchable_and_clamped():
    assert 'u8"similarConcurrency",Page::Library' in SETTINGS_CATALOG
    assert "LStr::Set_SimilarityConcurrency" in SETTINGS_CATALOG
    assert 'if(id=="similarConcurrency")' in SETTINGS_FIELDS
    assert "&draft.similarityAnalysisConcurrency,1,16" in SETTINGS_FIELDS


def test_similarity_run_uses_persisted_concurrency():
    assert "const int similarityConcurrency = std::clamp(" in MAIN
    assert "state.settings.similarityAnalysisConcurrency, 1, HarkenSearch::kMaxSimilarityConcurrency" in MAIN
    assert "[&state, databasePath, modelDirectory, recheckAll, similarityConcurrency]" in MAIN
    assert "SimilarityIndexScope::PendingOnly, similarityConcurrency)" in MAIN


def test_labels_are_localized():
    label = LOCALIZED["Set_SimilarityConcurrency"]
    assert label["zh"] == "相似分析并发数"
    assert label["en"] == "Similarity analysis concurrency"
    assert label["ja"] == "類似解析の同時実行数"
    hint = LOCALIZED["SetUX_SimilarityConcurrencyHint"]
    assert "下次分析生效" in hint["zh"]
    assert "next run" in hint["en"]
    assert "次回" in hint["ja"]


def test_parallel_results_use_a_worker_sized_ring():
    assert "slots(pool.size())" in INDEXER_IMPL
    assert "nextTask - consumed < slots.size()" in INDEXER_IMPL
    assert "slots[index % slots.size()]" in INDEXER_IMPL
