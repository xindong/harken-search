from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN_CPP = ROOT / "src" / "main.cpp"
FUNCTION_START = "static void DrawLibraryTaskToasts("


def read(path):
    assert path.is_file(), f"missing: {path}"
    return path.read_text(encoding="utf-8")


def toast_block(text):
    begin = text.index(FUNCTION_START)
    end = text.index("\nstatic ", begin + len(FUNCTION_START))
    return text[begin:end]


def test_failed_row_close_is_submitted_before_full_row_button():
    """失败行的 × 先提交，避免整行按钮先把 hover 拿走。"""
    block = toast_block(read(MAIN_CPP))
    close_index = block.index('ImGui::InvisibleButton("##close"')
    row_index = block.index('ImGui::InvisibleButton("##row"')
    assert close_index < row_index, (
        "##close must be submitted before ##row: ImGui locks HoveredId on the "
        "first hovered item, so a later 16x16 close fully covered by the row "
        "button can never receive hover or clicks"
    )


def test_failed_row_button_leaves_the_close_rect_alone():
    """失败行的整行按钮必须缩短 18px，两块矩形不能重叠。"""
    block = toast_block(read(MAIN_CPP))
    assert 'ImGui::InvisibleButton("##row", ImVec2(failed ? rowWidth - 18.0f : rowWidth, 20.0f))' in block, (
        "the row button must stop before the 16px dismiss button on failed rows; "
        "overlapping rects make the hover and the tooltip flip between the two items"
    )


def test_close_stays_inside_failed_branch():
    block = toast_block(read(MAIN_CPP))
    failed_branch = block.index("if (failed) {")
    close_index = block.index('ImGui::InvisibleButton("##close"')
    row_index = block.index('ImGui::InvisibleButton("##row"')
    assert failed_branch < close_index < row_index


def test_close_dismisses_every_row_kind():
    block = toast_block(read(MAIN_CPP))
    for call in (
        "state.scanTaskNotice.Dismiss()",
        "state.manualSimilarityNotice.Dismiss()",
        "state.folderRemovalNotice.Dismiss()",
        "state.updateTaskDismissed = true",
        "state.wwiseTaskDismissed = true",
    ):
        assert call in block, f"missing dismiss target: {call}"
