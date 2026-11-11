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


def test_visible_start_button_must_show_the_actual_remaining_count():
    import ast
    tree = ast.parse(SCRIPT.read_text(encoding="utf-8"))
    functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "assert_remaining_label"]
    assert functions, "the QA must verify the visible button label"
    namespace = {}
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(SCRIPT), "exec"), namespace)
    assert_remaining_label = namespace["assert_remaining_label"]
    bounds = [120, 268, 979, 303]
    def observation(label):
        return {'checks': [{'bounds': bounds, 'label': label}]}
    assert_remaining_label(observation('开始批量特征分析 (1 个文件)'), bounds, 1)
    import pytest
    with pytest.raises(AssertionError):
        assert_remaining_label(observation('开始批量特征分析 (3 个文件)'), bounds, 1)
