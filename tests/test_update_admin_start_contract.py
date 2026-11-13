from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_product_starts_without_automatic_elevation() -> None:
    manifest = (ROOT / "resources/HarkenSearch.manifest").read_text(encoding="utf-8")
    assert 'requestedExecutionLevel level="asInvoker"' in manifest
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "/MANIFESTUAC:NO" in cmake


def test_update_product_does_not_add_a_second_elevation_handoff_path() -> None:
    composition = (ROOT / "src/update/HarkenSearchUpdateComposition.cpp").read_text(
        encoding="utf-8"
    )
    windows_main = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    system_header = (ROOT / "src/platform/system.h").read_text(encoding="utf-8")
    assert "LaunchElevatedProcess" not in composition
    assert "--aris-update-elevated-restart" not in windows_main
    assert "LaunchElevatedProcess" not in system_header
