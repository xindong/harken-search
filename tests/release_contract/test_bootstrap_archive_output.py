"""内部静态库不得和发布程序落在同一输出目录。"""

from __future__ import annotations

import importlib.util
import re
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
CMAKE_PATH = ROOT / "CMakeLists.txt"
MCP_CMAKE_PATH = ROOT / "cmake" / "harken_external_ai_mcp.cmake"
BEHAVIOR_TESTS = ROOT / "cmake" / "harken_search_behavior_tests.cmake"
COMPLIANCE_PATH = ROOT / "tools" / "check_release_compliance.py"

_DEFAULT_ARCHIVE_DIRS = {
    "${CMAKE_BINARY_DIR}",
    "${CMAKE_BINARY_DIR}/$<CONFIG>",
    "${CMAKE_CURRENT_BINARY_DIR}",
    "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>",
}
_PROP_BLOCK = re.compile(
    r"set_target_properties\(\s*(?P<target>[A-Za-z0-9_]+)\s+PROPERTIES\s+(?P<body>.*?)\)",
    re.DOTALL,
)
_PROP_ENTRY = re.compile(r"(?P<key>[A-Za-z0-9_]+)\s+(?:\"(?P<quoted>[^\"]*)\"|(?P<bare>[^\s)]+))")
_BOOTSTRAP_LINK = re.compile(
    r"target_link_libraries\(\s*HarkenSearchBootstrap\s+PRIVATE\s+HarkenSearchBootstrapCore\b",
    re.DOTALL,
)


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _target_properties(cmake: str, target: str) -> dict[str, str]:
    properties: dict[str, str] = {}
    for match in _PROP_BLOCK.finditer(cmake):
        if match.group("target") != target:
            continue
        for entry in _PROP_ENTRY.finditer(match.group("body")):
            value = entry.group("quoted")
            if value is None:
                value = entry.group("bare")
            properties[entry.group("key")] = value
    return properties


def _load_compliance_module():
    spec = importlib.util.spec_from_file_location(
        "release_compliance_checker_bootstrap_archive",
        COMPLIANCE_PATH,
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_bootstrap_core_archive_is_isolated_from_shipped_runtime_directory() -> None:
    cmake = _read(CMAKE_PATH)
    assert "add_library(HarkenSearchBootstrapCore STATIC EXCLUDE_FROM_ALL" in cmake

    props = _target_properties(cmake, "HarkenSearchBootstrapCore")
    archive = props.get("ARCHIVE_OUTPUT_DIRECTORY", "")
    assert archive, "HarkenSearchBootstrapCore 必须设置 ARCHIVE_OUTPUT_DIRECTORY"
    assert archive not in _DEFAULT_ARCHIVE_DIRS
    assert archive.startswith("${CMAKE_BINARY_DIR}/")
    isolated_name = archive[len("${CMAKE_BINARY_DIR}/") :].strip("/")
    assert isolated_name
    assert "/" not in isolated_name
    assert isolated_name not in {".", "$<CONFIG>", "Debug", "Release"}
    assert "$<CONFIG>" not in isolated_name
    assert not archive.startswith("${CMAKE_BINARY_DIR}/Release")
    assert not archive.startswith("${CMAKE_BINARY_DIR}/$<CONFIG>")

    debug_archive = props.get("ARCHIVE_OUTPUT_DIRECTORY_DEBUG", "")
    release_archive = props.get("ARCHIVE_OUTPUT_DIRECTORY_RELEASE", "")
    if debug_archive or release_archive:
        assert debug_archive == f"{archive}/Debug"
        assert release_archive == f"{archive}/Release"

    for product in ("HarkenSearch", "HarkenSearchBootstrap"):
        product_props = _target_properties(cmake, product)
        runtime = product_props.get("RUNTIME_OUTPUT_DIRECTORY", "")
        if runtime:
            assert runtime != archive
            assert not runtime.startswith(f"{archive}/")


def test_bootstrap_core_link_relationships_remain_unchanged() -> None:
    cmake = _read(CMAKE_PATH)
    behavior = _read(BEHAVIOR_TESTS)
    assert "add_library(HarkenSearchBootstrapCore STATIC EXCLUDE_FROM_ALL" in cmake
    assert _BOOTSTRAP_LINK.search(cmake)
    assert "target_link_libraries(TestHarkenSearchBootstrapPlan PRIVATE HarkenSearchBootstrapCore)" in behavior
    assert "src/bootstrap/HarkenSearchBootstrapPlan.cpp" in cmake


@pytest.mark.parametrize(
    ("path", "target"),
    ((CMAKE_PATH, "harken_wavpack"), (MCP_CMAKE_PATH, "HarkenExternalAiLibrary")),
)
def test_internal_static_libraries_are_outside_release_payload_directory(
    path: Path, target: str
) -> None:
    cmake = _read(path)
    assert re.search(rf"add_library\(\s*{target}\s+STATIC\b", cmake)
    props = _target_properties(cmake, target)
    assert props.get("ARCHIVE_OUTPUT_DIRECTORY") == "${CMAKE_BINARY_DIR}/internal-archive"
    assert props.get("ARCHIVE_OUTPUT_DIRECTORY_DEBUG") == (
        "${CMAKE_BINARY_DIR}/internal-archive/Debug"
    )
    assert props.get("ARCHIVE_OUTPUT_DIRECTORY_RELEASE") == (
        "${CMAKE_BINARY_DIR}/internal-archive/Release"
    )


def test_release_compliance_still_forbids_static_libraries(tmp_path: Path) -> None:
    source = _read(COMPLIANCE_PATH)
    assert '"*.lib"' in source

    module = _load_compliance_module()
    assert "*.lib" in module.RELEASE_FORBIDDEN_GLOBS

    fixture_root = tmp_path / "synthetic-release-lib-ban"
    fixture_root.mkdir(parents=True, exist_ok=True)
    (fixture_root / "internal.lib").write_bytes(b"synthetic-static-library")
    nested = fixture_root / "nested"
    nested.mkdir(parents=True, exist_ok=True)
    (nested / "internal.lib").write_bytes(b"synthetic-nested-static-library")
    issues = module.validate_release_dir(fixture_root)
    assert any(
        issue == "forbidden release artifact present: internal.lib" for issue in issues
    )
    assert any(
        issue == "forbidden release artifact present: nested/internal.lib"
        for issue in issues
    )
