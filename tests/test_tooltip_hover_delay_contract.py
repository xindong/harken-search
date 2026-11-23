import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "TooltipHoverDelay.h"
DELAY_SECONDS = 1.0


def test_plain_text_tooltip_wrapper_waits_one_second_of_dwell():
    header = HEADER.read_text(encoding="utf-8")
    assert f"kTooltipHoverDelaySeconds = {DELAY_SECONDS}f" in header
    assert "ImGuiHoveredFlags_DelayNormal" in header
    assert "ImGuiHoveredFlags_AllowWhenDisabled" in header
    set_block = header.split("inline void SetTooltip(const char* fmt, ...) {", 1)[1]
    assert "TooltipItemHoverDwelled()" in set_block.split("}", 1)[0]
    begin_block = header.split("inline bool BeginTooltip() {", 1)[1]
    assert "TooltipItemHoverDwelled()" in begin_block.split("}", 1)[0]


def test_theme_style_uses_the_shared_delay_for_imgui_hover_flags():
    theme = (ROOT / "src" / "Theme.cpp").read_text(encoding="utf-8")
    assert "style.HoverDelayNormal = HarkenSearch::kTooltipHoverDelaySeconds;" in theme


def test_no_source_file_bypasses_the_shared_tooltip_entry_points():
    offenders = []
    for path in (ROOT / "src").rglob("*"):
        if path.suffix not in {".cpp", ".h", ".inl"}:
            continue
        if path.name == "TooltipHoverDelay.h":
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for entry_point in ("ImGui::SetTooltip(", "ImGui::BeginTooltip("):
            if entry_point in text:
                offenders.append(f"{path.relative_to(ROOT)}: {entry_point}")
    assert not offenders, offenders


def test_preview_class_tooltips_keep_the_immediate_entry_point():
    main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
    preview = main.split("// Hover preview and cover metadata.", 1)[1][:600]
    assert "HarkenSearch::BeginTooltipImmediate()" in preview
    assert "BeginTooltipImmediate" in HEADER.read_text(encoding="utf-8")


def test_dwell_timer_does_not_share_state_between_controls():
    header = HEADER.read_text(encoding="utf-8")
    dwell = header.split("inline bool TooltipItemHoverDwelled() {", 1)[1].split("}", 1)[0]
    assert "ImGuiHoveredFlags_NoSharedDelay" in dwell


def test_begin_tooltip_call_sites_only_close_what_they_opened():
    header = HEADER.read_text(encoding="utf-8")
    assert "inline bool BeginTooltipUngated() {\n    if (!TooltipItemHoverDwelled()) return false;" in header
    main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
    helper = main.split("static bool BeginSearchExplainabilityTooltip() {", 1)[1].split("\n}", 1)[0]
    assert "if (!HarkenSearch::BeginTooltipUngated())" in helper
    similar = (ROOT / "src" / "SimilarSoundsWindow.cpp").read_text(encoding="utf-8")
    assert "HarkenSearch::BeginTooltip()) {" in similar
    assert "\n                HarkenSearch::BeginTooltip();" not in similar


def test_dock_drop_hints_dwell_on_their_own_region():
    header = HEADER.read_text(encoding="utf-8")
    assert "inline void SetTooltipRegionUngated(const void* regionKey" in header
    main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
    dock_hint = main.split("auto drawDockPreview = [&](bool titleBarDragging) {", 1)[1][:400]
    assert "HarkenSearch::SetTooltipRegionUngated(&g_dockDropPlan" in dock_hint
    assert 'HarkenSearch::SetTooltipUngated("%s", L(Dock_UxTooSmall))' not in main
