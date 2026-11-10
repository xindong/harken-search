from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")


def test_query_status_contract_is_registered() -> None:
    assert "TestScanQueryStatusContract" in CMAKE


def test_redundant_query_labels_are_not_rendered() -> None:
    # These two user-visible labels used to appear together while switching libraries.
    # Animation and completion are exercised against the actual QA executable.
    assert "L(Search_Querying)" not in MAIN
    assert "L(Main_QueryingProgress)" not in MAIN
