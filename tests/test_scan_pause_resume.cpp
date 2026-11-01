#include "Database.h"
#include "ScanInterruption.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void check(bool condition, const char* label, int& failures) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << label << '\n';
    if (!condition) ++failures;
}

void writeSilentMonoWav(const fs::path& path) {
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bitsPerSample = 16;
    constexpr std::uint16_t blockAlign = channels * bitsPerSample / 8;
    constexpr std::uint32_t byteRate = sampleRate * blockAlign;
    constexpr std::uint32_t frameCount = 128;
    constexpr std::uint32_t dataSize = frameCount * blockAlign;
    constexpr std::uint32_t riffSize = 36 + dataSize;
    constexpr std::uint32_t fmtSize = 16;
    constexpr std::uint16_t pcmFormat = 1;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVEfmt ", 8);
    out.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
    out.write(reinterpret_cast<const char*>(&pcmFormat), sizeof(pcmFormat));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    out.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    const std::vector<std::int16_t> silence(frameCount, 0);
    out.write(reinterpret_cast<const char*>(silence.data()),
              static_cast<std::streamsize>(silence.size() * sizeof(std::int16_t)));
}

fs::path makeFixture(const char* name) {
    const fs::path root = fs::temp_directory_path() /
        (std::string("aris_scan_pause_resume_") + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "audio", ec);
    for (int i = 1; i <= 3; ++i) {
        writeSilentMonoWav(root / "audio" /
            (std::string("stall_0") + std::to_string(i) + ".wav"));
    }
    return root;
}

ScanStats interruptAfterFirstCommit(
    Database& db,
    const fs::path& audioRoot,
    HarkenSearch::ScanStopDisposition requested) {
    std::atomic<bool> cancel{false};
    std::atomic<HarkenSearch::ScanStopDisposition> disposition{
        HarkenSearch::ScanStopDisposition::None
    };
    db.beginScanRollbackSession();
    return db.scanDirectoryEx(
        audioRoot.u8string(),
        true,
        false,
        false,
        [&](int current, int, const std::string&, bool dataCommitted) {
            if (dataCommitted && current > 0) {
                disposition.store(requested, std::memory_order_release);
                cancel.store(true, std::memory_order_release);
            }
        },
        &cancel,
        1,
        false,
        false,
        nullptr,
        {},
        0,
        0,
        &disposition);
}

void testPausePersistsAcrossRestart(int& failures) {
    const fs::path root = makeFixture("keep");
    const fs::path dbPath = root / "library.db";
    Database db;
    check(db.open(dbPath.u8string()), "open pause database", failures);

    const ScanStats paused = interruptAfterFirstCommit(
        db, root / "audio", HarkenSearch::ScanStopDisposition::PauseKeepCommitted);
    check(paused.paused && !paused.cancelled,
          "pause is reported separately from cancel", failures);
    const int64_t retained = db.getFileCount();
    check(retained > 0 && retained < 3,
          "pause retains only the committed prefix", failures);
    db.close();

    check(db.open(dbPath.u8string()), "reopen pause database", failures);
    const ScanStats resumed = db.scanDirectoryEx(
        (root / "audio").u8string(), true, false, false,
        nullptr, nullptr, 1, false, false);
    check(db.getFileCount() == 3,
          "cross-restart rescan completes the folder", failures);
    check(resumed.added == 3 - retained,
          "resume inserts only unfinished files", failures);
    db.close();

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testCancelStillRollsBack(int& failures) {
    const fs::path root = makeFixture("rollback");
    Database db;
    check(db.open((root / "library.db").u8string()),
          "open rollback database", failures);

    const ScanStats cancelled = interruptAfterFirstCommit(
        db, root / "audio", HarkenSearch::ScanStopDisposition::CancelRollback);
    check(cancelled.cancelled && !cancelled.paused,
          "rollback cancel is reported separately from pause", failures);
    check(db.getFileCount() == 0,
          "rollback cancel removes the committed prefix", failures);
    db.close();

    std::error_code ec;
    fs::remove_all(root, ec);
}

void testPostProcessingNeverInflatesFileCounts(int& failures) {
    const fs::path root = makeFixture("progress_counts");
    Database db;
    check(db.open((root / "library.db").u8string()), "open progress database", failures);
    int maxCurrent = 0, maxTotal = 0, lastCurrent = -1, lastTotal = -1;
    int finalizingCalls = 0;
    bool finalizingCountsSeen = false;
    const auto stats = db.scanDirectoryEx(
        (root / "audio").u8string(), true, false, false,
        [&](int current, int total, const std::string&, bool) {
            maxCurrent = std::max(maxCurrent, current);
            maxTotal = std::max(maxTotal, total);
            lastCurrent = current; lastTotal = total;
            if (finalizingCalls > 0) finalizingCountsSeen = current == 3 && total == 3;
        }, nullptr, 1, false, false, nullptr, {}, 0, 0, nullptr,
        [&]() { ++finalizingCalls; });
    check(stats.total == 3 && db.getFileCount() == 3, "progress fixture imports three real files", failures);
    check(maxCurrent == 3 && maxTotal == 3 && lastCurrent == 3 && lastTotal == 3,
          "post-processing is not counted as extra audio files", failures);
    check(finalizingCalls == 1 && finalizingCountsSeen,
          "synchronous finalizing notification precedes real-count tail reports", failures);
    db.close();
    // Retain this small fixture; the QA temp root owns its lifecycle.
}

} // namespace

int main() {
    int failures = 0;
    testPausePersistsAcrossRestart(failures);
    testCancelStillRollsBack(failures);
    testPostProcessingNeverInflatesFileCounts(failures);
    if (failures == 0) {
        std::cout << "All scan pause and resume tests passed.\n";
        return 0;
    }
    std::cerr << failures << " scan pause and resume test(s) failed.\n";
    return 1;
}
