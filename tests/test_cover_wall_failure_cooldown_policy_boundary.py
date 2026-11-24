from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "src/main.cpp"
HEADER = ROOT / "src/CoverWallFailureCooldownPolicy.h"
SOURCE = ROOT / "src/CoverWallFailureCooldownPolicy.cpp"


def test_failure_cooldown_policy_has_independent_owner():
    assert HEADER.exists()
    assert SOURCE.exists()
    source = SOURCE.read_text(encoding="utf-8")
    for contract in (
        "FailureCoolingDown",
        "RecordFailure",
        "RecordSuccess",
        "ShouldRecordConfirmedFailure",
        "ReadyToWorkCandidateSatisfied",
    ):
        assert contract in source
    assert '"main.cpp"' not in source
    assert '"WindowsMain.h"' not in source
    assert '"platform/' not in source


def test_main_consumes_failure_cooldown_policy():
    main = MAIN.read_text(encoding="utf-8")
    assert '#include "CoverWallFailureCooldownPolicy.h"' in main
    for old_owner in (
        "static bool CoverWallFailureCoolingDown(",
        "static void CoverWallRecordFailure(",
        "static void CoverWallRecordSuccess(",
        "static bool CoverWallShouldRecordConfirmedFailure(",
        "static bool CoverWallReadyToWorkCandidateSatisfied(",
    ):
        assert old_owner not in main


def test_stale_decoder_cannot_publish_texture_or_mark_failed():
    source = (ROOT / "src/CoverModule.cpp").read_text(encoding="utf-8")
    worker = source.split("static void AsyncLoaderThread()", 1)[1].split(
        "static bool StartAsyncLoader()", 1
    )[0]
    after_decode = worker.split("NormalizeCoverKeyByVisualIdentity(folderCoverImagePathUtf8)", 1)[1]
    generation_check = (
        r"if \(task\.generation != g_asyncGeneration\.load\(std::memory_order_relaxed\)\)"
        r"\s*(?:\{\s*)?continue;"
    )
    match = re.search(
        r"std::lock_guard<std::mutex> \w+\(g_asyncMutex\);\s*" + generation_check,
        after_decode,
    )
    assert match, "Old results must leave before updating the current generation under its lock"
    for effect in ("g_asyncPendingKeys.erase(", "PushUpload(", "g_asyncFailedKeys["):
        assert after_decode.index(effect) > match.end(), effect
    before_decode = worker.split("std::vector<uint8_t> pixels;", 1)[0]
    assert re.search(generation_check, before_decode)
    assert "g_asyncPendingKeys.erase(" not in before_decode


def test_viewport_invalidation_releases_pending_keys_for_fresh_requests():
    source = (ROOT / "src/CoverModule.cpp").read_text(encoding="utf-8")
    invalidate = source.split("void InvalidateViewportCoverTasks()", 1)[1].split(
        "uint64_t ComputeBatchSignatureXXH3", 1
    )[0]
    assert "g_asyncPendingKeys.clear();" in invalidate
    assert invalidate.index("g_asyncMutex") < invalidate.index("g_asyncGeneration.fetch_add")
