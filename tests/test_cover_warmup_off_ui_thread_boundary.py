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
    assert "warmupCoverIndexes([this]() { return m_running.load(); })" in head


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
        "void Database::warmupCoverIndexes",
        "bool Database::openReadOnly",
    )
    assert warmup.count("<> ''") == 2
    assert "folder_cover_key > ''" not in warmup
    assert "embedded_cover_key > ''" not in warmup
    assert warmup.count("if (!keepGoing()) return;") == 2

    # Compare against the index definitions themselves; the warmup query text
    # also contains these predicates, so a whole-file search would pass even
    # after the partial indexes were removed.
    embedded_index = _between(
        body,
        "CREATE INDEX IF NOT EXISTS idx_files_cover_dedup_emb",
        ";",
    )
    folder_index = _between(
        body,
        "CREATE INDEX IF NOT EXISTS idx_files_cover_dedup_fold",
        ";",
    )
    assert "WHERE embedded_cover_key <> ''" in embedded_index
    assert "WHERE folder_cover_key <> ''" in folder_index
