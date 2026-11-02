from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_app_state_uses_solid_waveform_as_the_missing_setting_default():
    source = (ROOT / "src" / "AppState.h").read_text(encoding="utf-8")
    assert "int waveformStyle = DefaultMainWaveformStyle();" in source


def test_reset_default_sizes_restores_solid_main_waveform_style():
    source = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
    reset_start = source.index("if (aura::ui::ActionButton(L(Set_ResetDefaultSize)")
    reset_end = source.index("if (HoveredForHelp())", reset_start)
    reset_block = source[reset_start:reset_end]
    assert "state.settings.waveformStyle = DefaultMainWaveformStyle();" in reset_block
