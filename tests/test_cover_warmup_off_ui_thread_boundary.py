from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASYNC_WORKER = ROOT / "src/AsyncWorker.cpp"
DATABASE = ROOT / "src/Database.cpp"
MAIN = ROOT / "src/main.cpp"


def _between(text: str, start: str, end: str) -> str:
    begin = text.index(start)
    finish = text.index(end, begin)
    return text[begin:finish]


def test_search_worker_start_does_not_warm_cover_indexes() -> None:
    body = ASYNC_WORKER.read_text(encoding="utf-8")
    start = _between(
        body,
        "void SearchWorker::start(const std::string& dbPath)",
        "void SearchWorker::stop()",
    )
    assert "warmupCoverIndexes" not in start

    worker = body[body.index("void SearchWorker::workerThread()"):]
    head = worker[: worker.index("struct LegacyCommentCacheEntry")]
    assert "warmupCoverIndexes" in head


def test_file_player_mount_does_not_warm_cover_indexes_on_ui_thread() -> None:
    body = MAIN.read_text(encoding="utf-8")
    block = _between(
        body,
        "if (fullPreheat) {",
        "if (state.settings.autoReconcileOnStartup)",
    )
    assert "preheat()" in block
    assert "warmupCoverIndexes" not in block


def test_cover_warmup_sql_matches_partial_indexes() -> None:
    body = DATABASE.read_text(encoding="utf-8")
    warmup = _between(
        body,
        "void Database::warmupCoverIndexes()",
        "bool Database::openReadOnly",
    )
    assert warmup.count("<> ''") == 2
    assert "folder_cover_key > ''" not in warmup
    assert "embedded_cover_key > ''" not in warmup
    assert "WHERE folder_cover_key <> ''" in body
    assert "WHERE embedded_cover_key <> ''" in body
