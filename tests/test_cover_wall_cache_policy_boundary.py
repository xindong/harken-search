import os
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory

import pytest


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src/main.cpp"
HEADER = ROOT / "src/CoverWallCachePolicy.h"
SOURCE = ROOT / "src/CoverWallCachePolicy.cpp"


def test_cover_wall_cache_policy_has_independent_owner():
    assert HEADER.exists()
    assert SOURCE.exists()
    source = SOURCE.read_text(encoding="utf-8")
    for contract in (
        "CanReuseGlobalCoverWallCache",
        "StoreGlobalCoverWallCache",
        "LastCoverWallPageCursor",
        "ReconstructGlobalCoverWallPageState",
    ):
        assert contract in source
    assert '"main.cpp"' not in source
    assert '"WindowsMain.h"' not in source
    assert '"platform/' not in source


def test_main_consumes_cover_wall_cache_policy():
    main = MAIN.read_text(encoding="utf-8")
    assert '#include "CoverWallCachePolicy.h"' in main
    for old_owner in (
        "static bool CanReuseGlobalCoverWallCache(",
        "static void StoreGlobalCoverWallCache(",
        "static std::string LastCoverWallPageCursor(",
        "static void ReconstructGlobalCoverWallPageState(",
    ):
        assert old_owner not in main
    for symbol in (
        "CanReuseGlobalCoverWallCache",
        "StoreGlobalCoverWallCache",
        "LastCoverWallPageCursor",
        "ReconstructGlobalCoverWallPageState",
    ):
        assert f"using aura::cover_wall::{symbol};" in main


def test_return_from_facets_restarts_incomplete_global_pagination():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("// Global mode reads deduplicated covers from the database, then discovers more.")
    end = main.index("bool needStartLoad = false;", start)
    global_entry = main[start:end]
    assert "if (wasUsingFacetCoverWall && !canReuseGlobalCache) {" in global_entry
    assert "cw.prepareGlobalReloadAfterFacet(mainDbPath);" in global_entry
    assert global_entry.index("cw.prepareGlobalReloadAfterFacet(mainDbPath);") < global_entry.index("if (canReuseGlobalCache)")


def test_stale_preload_does_not_clear_live_pagination():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("CoverWallPageLoadResult preloaded = cw.preloadFuture.get();")
    end = main.index("// 不预热整页", start)
    retired, _, publish = main[start:end].partition("if (!stalePreload) {")
    assert "cw.preloadGeneration != cw.generation" in retired
    assert "cw.preloadInFlight = false;" in retired
    assert "cw.globalPageNextCursor = preloaded.nextCursor;" in publish
    assert "} else {" not in publish, "A stale preload must not clear the live page or cursor"


def test_stale_entries_and_folder_results_do_not_invalidate_the_current_generation():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("CoverWallPageLoadResult page = cw.entriesFuture.get();")
    end = main.index("// Keep display snapshots independent from pending cursors.", start)
    completions = main[start:end]
    assert "RequestDatabaseInvalidation" not in completions, (
        "An obsolete result must not invalidate a newer first page"
    )
    assert "if (!staleFolders && !staleFoldersView)" in completions


def test_old_entries_view_cannot_reconcile_the_current_page_or_claim_its_append():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("CoverWallPageLoadResult page = cw.entriesFuture.get();")
    end = main.index("cw.foldersFuture.get();", start)
    completion = main[start:end]
    assert "const bool currentEntriesView = !staleEntries && !staleEntriesView;" in completion
    assert "if (currentEntriesView && !requestMatchesCurrentPageState)" in completion
    assert "else if (currentEntriesView)" in completion
    assert "if (!staleEntries && cw.entriesRequestedAppend)" in completion
    assert "cw.entriesRequestedCursor == cw.globalPageNextCursor" in completion


def test_first_page_restart_prevents_a_competing_preload():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("if (needStartLoad && !cw.entriesInFlight && !cw.foldersInFlight)")
    end = main.index("CoverWallPageLoadResult page = cw.entriesFuture.get();", start)
    assert "cw.preloadDbPath = mainDbPath;" in main[start:end], (
        "The first-page restart must claim the same DB identity as the preload"
    )


def test_current_preload_can_replace_retained_items_after_content_invalidation():
    main = MAIN.read_text(encoding="utf-8")
    start = main.index("if (canReuseGlobalCache)", main.index("cw.prepareGlobalReloadAfterFacet(mainDbPath);"))
    end = main.index("bool needStartLoad = false;", start)
    ready, _, adoption = main[start:end].partition("if (preloadReadyForReuse) {")
    assert "cw.preloadGeneration == cw.generation" in ready
    assert "cw.preloadDbPath == mainDbPath" in ready
    assert "!cw.loaded &&" in ready, "Old displayed items must not block the current first page"
    assert "cw.items = cw.preloadFullList;" in adoption


def _production_source(relative_path):
    baseline = os.environ.get("HARKEN_COVER_WALL_BASELINE")
    if baseline:
        return subprocess.check_output(
            ["git", "show", f"{baseline}:{relative_path}"],
            cwd=ROOT, encoding="utf-8", timeout=10,
        )
    return (ROOT / relative_path).read_text(encoding="utf-8")


def _cpp_block(source, anchor):
    start = source.index(anchor)
    opening = source.index("{", start + len(anchor))
    tokens = re.compile(r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]""", re.S)
    depth = 0
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:token.end()]
    raise AssertionError(f"Unclosed production block: {anchor}")


def _build_async_completion_probe(folder):
    app = _production_source("src/AppState.h")
    cover = _production_source("src/CoverModule.h")
    database = _production_source("src/Database.h")
    adapter = _production_source("src/CoverWallRefreshStateAdapter.cpp")
    main = _production_source("src/main.cpp")
    fragments = {
        "cover_item": ";\n".join(
            _cpp_block(cover, anchor) for anchor in ("enum class CoverKind", "struct CoverItem")
        ) + ";",
        "cover_state": ";\n".join(_cpp_block(app, anchor) for anchor in (
            "struct CoverWallTextureMirrorEntry", "struct CoverWallFailedTextureEntry",
            "enum class CoverWallSource", "struct CoverWallPageLoadResult", "struct CoverWallState",
        )) + ";",
        "database_entry": _cpp_block(database, "struct CoverWallEntry") + ";",
        "app_invalidation": _cpp_block(app, "void invalidateCoverWallAfterDatabaseContentChange()"),
        "force_reload": _cpp_block(adapter, "void ForceCoverWallReload(AppState& state)"),
        "request_invalidation": _cpp_block(adapter, "void RequestDatabaseInvalidation(AppState& state)"),
        "completions": "\n".join(
            _cpp_block(main, f"if (cw.{name}InFlight && cw.{name}Future.valid()")
            for name in ("preload", "entries", "folders")
        ),
    }
    for name, content in fragments.items():
        (folder / f"{name}.inc").write_text(content + "\n", encoding="utf-8")
    probe = (ROOT / "tests" / "cover_wall_async_completion_probe.cpp").as_posix()
    (folder / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(CoverWallAsyncCompletionProbe LANGUAGES CXX)\n"
        "find_package(Threads REQUIRED)\n"
        f'add_executable(CoverWallAsyncCompletionProbe "{probe}")\n'
        "target_compile_features(CoverWallAsyncCompletionProbe PRIVATE cxx_std_17)\n"
        f'target_include_directories(CoverWallAsyncCompletionProbe PRIVATE "{folder.as_posix()}")\n'
        "target_link_libraries(CoverWallAsyncCompletionProbe PRIVATE Threads::Threads)\n"
        "if(MSVC)\n"
        "  target_compile_options(CoverWallAsyncCompletionProbe PRIVATE /utf-8)\n"
        "endif()\n",
        encoding="utf-8",
    )
    binary = folder / "bin"
    generator = ["-G", "Visual Studio 17 2022", "-A", "x64"] if os.name == "nt" else []
    for command in (
        ["cmake", "-S", str(folder), "-B", str(binary), *generator],
        ["cmake", "--build", str(binary), "--config", "Release"],
    ):
        result = subprocess.run(
            command, cwd=folder, text=True, errors="replace", capture_output=True, timeout=45,
        )
        assert result.returncode == 0, result.stdout + result.stderr
    name = "CoverWallAsyncCompletionProbe.exe" if os.name == "nt" else "CoverWallAsyncCompletionProbe"
    executable = binary / "Release" / name
    return executable if executable.is_file() else binary / name


@pytest.fixture(scope="module")
def async_completion_probe():
    """Compile just the production completion paths; HARKEN_COVER_WALL_BASELINE selects a Git ref."""
    with TemporaryDirectory(prefix="harken-cover-wall-") as folder:
        yield _build_async_completion_probe(Path(folder))


@pytest.mark.parametrize("request_kind", ["preload", "entries", "folders"])
def test_late_result_preserves_new_first_page(async_completion_probe, request_kind):
    result = subprocess.run(
        [str(async_completion_probe), request_kind], text=True, capture_output=True, timeout=10
    )
    assert result.returncode == 0, result.stdout + result.stderr
