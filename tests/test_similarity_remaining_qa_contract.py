from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "qa_similarity_remaining.py"


def test_scan_fixture_dismisses_the_native_result_window_before_analysis():
    source = SCRIPT.read_text(encoding="utf-8")
    scan_setup = source.split("if existing_records == 0:", 1)[1].split(
        "command(cmd='similar_index', action='open'", 1
    )[0]

    assert "scan_acknowledge_complete" not in scan_setup
    assert "command(cmd='open_scan_dialog')" in scan_setup
    assert "info.qaScanDone" in scan_setup
    assert "info_page_pointer" in scan_setup
    assert "scan_status" in scan_setup and "total" in scan_setup
