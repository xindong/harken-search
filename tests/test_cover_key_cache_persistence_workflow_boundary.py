from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src/main.cpp"
WINDOWS_MAIN = ROOT / "src/WindowsMain.cpp"
HEADER = ROOT / "src/CoverKeyCachePersistenceWorkflow.h"
SOURCE = ROOT / "src/CoverKeyCachePersistenceWorkflow.cpp"
DATABASE = ROOT / "src/Database.cpp"
CMODEL = ROOT / "CMakeLists.txt"
BEHAVIOR = ROOT / "cmake/harken_search_behavior_tests.cmake"


def function_body(source: str, signature: str) -> str:
    assert signature in source, signature
    brace = source.index("{", source.index(signature))
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"unterminated function: {signature}")


def test_cover_key_persistence_owns_database_sync_boundary():
    main = MAIN.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    cmake = CMODEL.read_text(encoding="utf-8")
    behavior = BEHAVIOR.read_text(encoding="utf-8")

    assert "PumpCoverKeyCachePersistence" not in main
    assert "PumpPersistence" in header
    assert "PumpPersistence(" in source
    assert "CoverKeyCachePersistenceWorkflow.cpp" in cmake
    assert "TestCoverKeyCachePersistenceWorkflowBoundary" in behavior


def test_cover_key_persistence_has_no_root_or_ui_dependency():
    source = SOURCE.read_text(encoding="utf-8")
    assert '"main.cpp"' not in source
    assert '"WindowsMain.h"' not in source
    assert "ImGui::" not in source


def test_frame_pump_only_signals_cover_key_writeback():
    source = SOURCE.read_text(encoding="utf-8")
    pump = function_body(source, "PumpPersistence(")
    assert "updateCoverKeyCacheBatch(" not in pump, "frame pump writes to SQLite"
    for blocking_call in ("sqlite3_", ".open(", ".join(", ".wait(", "DrainCoverKeyCacheUpdates("):
        assert blocking_call not in pump, blocking_call

    main = MAIN.read_text(encoding="utf-8")
    assert "!state.dbSwitchInProgress" in main.split("aura::cover_cache::PumpPersistence(", 1)[0][-450:]


def test_cover_key_writer_drains_bounded_batches_and_reuses_database_update():
    source = SOURCE.read_text(encoding="utf-8")
    assert "std::thread" in source, "no background writer owns the SQLite connection"
    assert "DrainCoverKeyCacheUpdates(updates, kWriteBatchSize)" in source
    assert "updateCoverKeyCacheBatch(" in source
    assert "openForCoverKeyCachePersistence(" in source

    database = DATABASE.read_text(encoding="utf-8")
    opener = function_body(database, "Database::openForCoverKeyCachePersistence(")
    assert "SQLITE_OPEN_READWRITE" in opener
    for invalid_opener in ("SQLITE_OPEN_CREATE", "createTables(", "attachSidecarDatabases("):
        assert invalid_opener not in opener, invalid_opener


def test_database_switch_retires_writer_before_any_database_close_or_replacement():
    main = MAIN.read_text(encoding="utf-8")
    switch = main.split("g_databaseSwitchWorkerRuntime.Start(", 1)[1]
    worker = switch.split("RunBlockingDatabaseSwitchOperation(", 1)[0]
    assert "aura::cover_cache::StopPersistence();" in worker


def test_normal_exit_drains_after_producers_and_primary_writers_stop():
    windows_main = WINDOWS_MAIN.read_text(encoding="utf-8")
    shutdown = windows_main.split('logShutdownStage("CoverTaskThreadJoinEnd");', 1)[1]
    stages = (
        "CoverModule::ShutdownCoverWallPrecache();",
        "state.database.close();",
        "aura::cover_cache::FlushPersistence();",
        "CoverModule::ClearAllCoverCaches();",
    )
    assert all(stage in shutdown for stage in stages)
    offsets = [shutdown.index(stage) for stage in stages]
    assert offsets == sorted(offsets)
