#include "AsyncWorker.h"
#include "AppState.h"
#include "BackfillQueue.h"
#include "BackfillWorkerPool.h"
#include "Database.h"
#include "DiagnosticsLogger.h"
#include "PathUtils.h"
#include "WaveformCachePolicy.h"
#include "WaveformExtractor.h"
#include "sqlite3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static fs::path makeTempRoot(const std::string& name) {
    fs::path root = fs::temp_directory_path() / ("aura_waveform_scan_contract_" + name);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

static void removeDbArtifacts(const fs::path& dbPath) {
    std::error_code ec;
    fs::remove(dbPath, ec);
    fs::remove(dbPath.string() + "-wal", ec);
    fs::remove(dbPath.string() + "-shm", ec);

    const std::string stem = dbPath.stem().u8string();
    const fs::path parent = dbPath.parent_path();
    fs::remove(parent / fs::u8path(stem + "_blobs.db"), ec);
    fs::remove(parent / fs::u8path(stem + "_blobs.db-wal"), ec);
    fs::remove(parent / fs::u8path(stem + "_blobs.db-shm"), ec);
    fs::remove(parent / fs::u8path(stem + "_waveforms.db"), ec);
    fs::remove(parent / fs::u8path(stem + "_waveforms.db-wal"), ec);
    fs::remove(parent / fs::u8path(stem + "_waveforms.db-shm"), ec);
}

static uint64_t fileSizeIfExists(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return 0;
    ec.clear();
    const auto bytes = fs::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(bytes);
}

static void waitDiagDrain() {
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
}

static bool writeLegacyMainWaveformBlobForTest(const fs::path& dbPath) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.u8string().c_str(), &raw) != SQLITE_OK || !raw) {
        if (raw) sqlite3_close(raw);
        return false;
    }
    char* err = nullptr;
    const int rc = sqlite3_exec(raw,
                                "UPDATE audio_files SET waveform_data = zeroblob(512);",
                                nullptr,
                                nullptr,
                                &err);
    if (err) sqlite3_free(err);
    sqlite3_close(raw);
    return rc == SQLITE_OK;
}

static void setScanWriteLockRetryBudgetForTest(const char* value) {
#ifdef _WIN32
    _putenv_s("AURA_SCAN_WRITE_LOCK_RETRY_MS", value ? value : "");
#else
    if (value && *value) {
        setenv("AURA_SCAN_WRITE_LOCK_RETRY_MS", value, 1);
    } else {
        unsetenv("AURA_SCAN_WRITE_LOCK_RETRY_MS");
    }
#endif
}

static void setWpk2DetailWriterForTest(const char* value) {
#ifdef _WIN32
    _putenv_s("AURA_WPK2_DETAIL_WRITER", value ? value : "");
#else
    if (value && *value) {
        setenv("AURA_WPK2_DETAIL_WRITER", value, 1);
    } else {
        unsetenv("AURA_WPK2_DETAIL_WRITER");
    }
#endif
}

static void writeSilentMonoWav(const fs::path& path, int sampleRate = 8000, int frameCount = 128) {
    const int channels = 1;
    const int bitsPerSample = 16;
    const int blockAlign = channels * (bitsPerSample / 8);
    const int byteRate = sampleRate * blockAlign;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(frameCount * blockAlign);
    const std::uint32_t riffSize = 36u + dataSize;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);

    const std::uint32_t fmtChunkSize = 16;
    const std::uint16_t audioFormat = 1;
    const std::uint16_t numChannels = static_cast<std::uint16_t>(channels);
    const std::uint16_t bits = static_cast<std::uint16_t>(bitsPerSample);
    const std::uint16_t align = static_cast<std::uint16_t>(blockAlign);
    out.write(reinterpret_cast<const char*>(&fmtChunkSize), sizeof(fmtChunkSize));
    out.write(reinterpret_cast<const char*>(&audioFormat), sizeof(audioFormat));
    out.write(reinterpret_cast<const char*>(&numChannels), sizeof(numChannels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&align), sizeof(align));
    out.write(reinterpret_cast<const char*>(&bits), sizeof(bits));

    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    std::vector<std::int16_t> pcm(static_cast<size_t>(frameCount * channels), 0);
    out.write(reinterpret_cast<const char*>(pcm.data()),
               static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
}

static void writeMonoImpulseWav(const fs::path& path,
                                int sampleRate,
                                int frameCount,
                                int impulseFrame,
                                std::int16_t amplitude = 28000) {
    const int channels = 1;
    const int bitsPerSample = 16;
    const int blockAlign = channels * (bitsPerSample / 8);
    const int byteRate = sampleRate * blockAlign;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(frameCount * blockAlign);
    const std::uint32_t riffSize = 36u + dataSize;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);

    const std::uint32_t fmtChunkSize = 16;
    const std::uint16_t audioFormat = 1;
    const std::uint16_t numChannels = static_cast<std::uint16_t>(channels);
    const std::uint16_t bits = static_cast<std::uint16_t>(bitsPerSample);
    const std::uint16_t align = static_cast<std::uint16_t>(blockAlign);
    out.write(reinterpret_cast<const char*>(&fmtChunkSize), sizeof(fmtChunkSize));
    out.write(reinterpret_cast<const char*>(&audioFormat), sizeof(audioFormat));
    out.write(reinterpret_cast<const char*>(&numChannels), sizeof(numChannels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&align), sizeof(align));
    out.write(reinterpret_cast<const char*>(&bits), sizeof(bits));

    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    std::vector<std::int16_t> pcm(static_cast<size_t>(frameCount * channels), 0);
    if (impulseFrame >= 0 && impulseFrame < frameCount) {
        pcm[(size_t)impulseFrame] = amplitude;
    }
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
}

static void writeSilentStereoWav(const fs::path& path, int sampleRate = 8000, int frameCount = 128) {
    const int channels = 2;
    const int bitsPerSample = 16;
    const int blockAlign = channels * (bitsPerSample / 8);
    const int byteRate = sampleRate * blockAlign;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(frameCount * blockAlign);
    const std::uint32_t riffSize = 36u + dataSize;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);

    const std::uint32_t fmtChunkSize = 16;
    const std::uint16_t audioFormat = 1;
    const std::uint16_t numChannels = static_cast<std::uint16_t>(channels);
    const std::uint16_t bits = static_cast<std::uint16_t>(bitsPerSample);
    const std::uint16_t align = static_cast<std::uint16_t>(blockAlign);
    out.write(reinterpret_cast<const char*>(&fmtChunkSize), sizeof(fmtChunkSize));
    out.write(reinterpret_cast<const char*>(&audioFormat), sizeof(audioFormat));
    out.write(reinterpret_cast<const char*>(&numChannels), sizeof(numChannels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&align), sizeof(align));
    out.write(reinterpret_cast<const char*>(&bits), sizeof(bits));

    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    std::vector<std::int16_t> pcm(static_cast<size_t>(frameCount * channels), 0);
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
}

static void writeBextTaggedWav(const fs::path& path,
                               const std::string& description,
                               int sampleRate = 8000,
                               int frameCount = 128) {
    const int channels = 1;
    const int bitsPerSample = 16;
    const int blockAlign = channels * (bitsPerSample / 8);
    const int byteRate = sampleRate * blockAlign;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(frameCount * blockAlign);
    const std::uint32_t bextSize = 602;
    const std::uint32_t riffSize = 4u + (8u + 16u) + (8u + dataSize) + (8u + bextSize);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);

    const std::uint32_t fmtChunkSize = 16;
    const std::uint16_t audioFormat = 1;
    const std::uint16_t numChannels = static_cast<std::uint16_t>(channels);
    const std::uint16_t bits = static_cast<std::uint16_t>(bitsPerSample);
    const std::uint16_t align = static_cast<std::uint16_t>(blockAlign);
    out.write(reinterpret_cast<const char*>(&fmtChunkSize), sizeof(fmtChunkSize));
    out.write(reinterpret_cast<const char*>(&audioFormat), sizeof(audioFormat));
    out.write(reinterpret_cast<const char*>(&numChannels), sizeof(numChannels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&align), sizeof(align));
    out.write(reinterpret_cast<const char*>(&bits), sizeof(bits));

    std::vector<char> bext(bextSize, 0);
    std::copy_n(description.data(), std::min<size_t>(description.size(), 255), bext.data());
    out.write("bext", 4);
    out.write(reinterpret_cast<const char*>(&bextSize), sizeof(bextSize));
    out.write(bext.data(), static_cast<std::streamsize>(bext.size()));

    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    std::vector<std::int16_t> pcm(static_cast<size_t>(frameCount * channels), 0);
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(pcm.size() * sizeof(std::int16_t)));
}

static void writeTinyCoverImage(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const char bytes[] = "not-a-real-jpeg-but-large-enough-for-thumb-cache";
    out.write(bytes, sizeof(bytes));
}

static bool check(bool condition, const char* label, int& failures) {
    std::cout << (condition ? "PASS" : "FAIL") << ": " << label << "\n";
    if (!condition) {
        ++failures;
    }
    return condition;
}

static bool approxEqual(float a, float b, float tolerance = 0.0001f) {
    return std::fabs(a - b) <= tolerance;
}

static int testWpk2PeakEnvelopeCodecRoundTripsStereo() {
    int failures = 0;
    const std::vector<float> peaks = {0.25f, 0.50f, 0.75f, 1.0f};
    const std::vector<float> mins = {-0.10f, -0.20f, -0.30f, -0.40f};
    std::vector<std::uint8_t> blob;

    check(WaveformExtractor::EncodePeakEnvelopeInt16(peaks, mins, 2, 2, blob),
          "WPK2 stereo encode succeeds", failures);
    check(!blob.empty(), "WPK2 stereo encode writes a blob", failures);
    check(blob.size() == 24, "WPK2 stereo 2-bucket payload is 16 bytes plus 8-byte header", failures);
    check(WaveformExtractor::IsPeakEnvelopeInt16Blob(blob),
          "WPK2 stereo blob is detected by header", failures);

    std::vector<float> decodedPeaks;
    std::vector<float> decodedMins;
    int decodedChannels = 0;
    int decodedBuckets = 0;
    check(WaveformExtractor::DecodePeakEnvelopeInt16(blob, decodedPeaks, decodedMins, decodedChannels, decodedBuckets),
          "WPK2 stereo decode succeeds", failures);
    check(decodedChannels == 2, "WPK2 stereo decode preserves channel count", failures);
    check(decodedBuckets == 2, "WPK2 stereo decode preserves bucket count", failures);
    check(decodedPeaks.size() == peaks.size(), "WPK2 stereo decode restores max shape", failures);
    check(decodedMins.size() == mins.size(), "WPK2 stereo decode restores min shape", failures);
    for (size_t i = 0; i < peaks.size() && i < decodedPeaks.size(); ++i) {
        check(approxEqual(decodedPeaks[i], peaks[i]), "WPK2 stereo max roundtrip stays close", failures);
        check(approxEqual(decodedMins[i], mins[i]), "WPK2 stereo min roundtrip stays close", failures);
    }
    return failures;
}

static int testWpk2PeakEnvelopeCodecRejectsMalformedAndLegacyCollision() {
    int failures = 0;

    std::vector<std::uint8_t> malformed = {0x00, 'W', 'P', 'K', '2'};
    check(!WaveformExtractor::IsPeakEnvelopeInt16Blob(malformed),
          "short WPK2-like blob is not detected as valid WPK2", failures);

    std::vector<float> decodedPeaks;
    std::vector<float> decodedMins;
    int decodedChannels = 0;
    int decodedBuckets = 0;
    check(!WaveformExtractor::DecodePeakEnvelopeInt16(malformed, decodedPeaks, decodedMins, decodedChannels, decodedBuckets),
          "malformed WPK2 blob is rejected", failures);

    std::vector<std::uint8_t> legacyFloatCollision(16, 0);
    legacyFloatCollision[0] = 'W';
    legacyFloatCollision[1] = 'P';
    legacyFloatCollision[2] = 'K';
    legacyFloatCollision[3] = '2';
    check(!WaveformExtractor::IsPeakEnvelopeInt16Blob(legacyFloatCollision),
          "legacy float blob with old 4-byte magic pattern is not detected as WPK2", failures);
    return failures;
}

static int testWpk2PeakEnvelopeCodecPreservesQuietTail() {
    int failures = 0;
    const std::vector<float> peaks = {0.0f, 0.00001f, 0.001f};
    const std::vector<float> mins = {0.0f, -0.00001f, -0.001f};
    std::vector<std::uint8_t> blob;

    check(WaveformExtractor::EncodePeakEnvelopeInt16(peaks, mins, 1, 3, blob),
          "WPK2 quiet encode succeeds", failures);

    std::vector<float> decodedPeaks;
    std::vector<float> decodedMins;
    int decodedChannels = 0;
    int decodedBuckets = 0;
    check(WaveformExtractor::DecodePeakEnvelopeInt16(blob, decodedPeaks, decodedMins, decodedChannels, decodedBuckets),
          "WPK2 quiet decode succeeds", failures);
    check(decodedPeaks.size() == peaks.size(), "WPK2 quiet decode restores max shape", failures);
    check(decodedMins.size() == mins.size(), "WPK2 quiet decode restores min shape", failures);
    if (decodedPeaks.size() >= 3 && decodedMins.size() >= 3) {
        check(decodedPeaks[0] == 0.0f, "WPK2 quiet exact zero stays zero", failures);
        check(decodedMins[0] == 0.0f, "WPK2 quiet exact negative zero stays zero", failures);
        check(decodedPeaks[1] > 0.0f, "WPK2 quiet positive non-zero survives quantization", failures);
        check(decodedMins[1] < 0.0f, "WPK2 quiet negative non-zero survives quantization", failures);
        check(approxEqual(decodedPeaks[2], peaks[2]), "WPK2 quiet audible positive value stays close", failures);
        check(approxEqual(decodedMins[2], mins[2]), "WPK2 quiet audible negative value stays close", failures);
    }
    return failures;
}

static bool waitForPrecompute(Database& db, int minProcessed) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (db.precomputeQueueDepth() == 0 && db.precomputeProcessedCount() >= minProcessed) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return db.precomputeQueueDepth() == 0 && db.precomputeProcessedCount() >= minProcessed;
}

static int querySingleInt(const std::string& dbPath, const char* sql, int fallback = -1) {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        return fallback;
    }
    sqlite3_stmt* stmt = nullptr;
    int value = fallback;
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            value = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(raw);
    return value;
}

static int querySingleInt(sqlite3* raw, const char* sql, int fallback = -1) {
    if (!raw) return fallback;
    sqlite3_stmt* stmt = nullptr;
    int value = fallback;
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            value = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return value;
}

static int countExternalWaveformRowsByPath(sqlite3* raw, const std::string& filePath) {
    if (!raw) return -1;
    sqlite3_stmt* stmt = nullptr;
    int value = -1;
    const char* sql = "SELECT COUNT(*) FROM waveforms_db.external_waveforms WHERE file_path = ?";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            value = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return value;
}

static int64_t fileTimeToUnixSecondsForTest(const fs::file_time_type& ftime) {
    using namespace std::chrono;
    auto sctp = time_point_cast<seconds>(ftime - fs::file_time_type::clock::now() + system_clock::now());
    return static_cast<int64_t>(sctp.time_since_epoch().count());
}

static bool insertExternalWaveformDetailRow(sqlite3* raw,
                                            const fs::path& filePath,
                                            const std::vector<float>& detail,
                                            const std::vector<float>& minDetail,
                                            int channels) {
    if (!raw || detail.empty()) return false;
    std::error_code ec;
    const int64_t fileSize = static_cast<int64_t>(fs::file_size(filePath, ec));
    if (ec) return false;
    const int64_t fileMtime = fileTimeToUnixSecondsForTest(fs::last_write_time(filePath, ec));
    if (ec) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"(
        INSERT OR REPLACE INTO waveforms_db.external_waveforms
            (file_path, file_size, file_mtime, waveform_detail, waveform_min_detail,
             detail_num_channels, updated_at, cache_key)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    const std::string pathUtf8 = filePath.u8string();
    const std::string cacheKey = std::to_string(fileSize) + ":" + std::to_string(fileMtime);
    sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, fileSize);
    sqlite3_bind_int64(stmt, 3, fileMtime);
    sqlite3_bind_blob(stmt, 4, detail.data(), (int)(detail.size() * sizeof(float)), SQLITE_TRANSIENT);
    if (minDetail.empty()) {
        sqlite3_bind_null(stmt, 5);
    } else {
        sqlite3_bind_blob(stmt, 5, minDetail.data(), (int)(minDetail.size() * sizeof(float)), SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, 6, channels);
    sqlite3_bind_int64(stmt, 7, 1);
    sqlite3_bind_text(stmt, 8, cacheKey.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static bool insertWpk2WaveformDetailRow(sqlite3* raw,
                                        int64_t fileId,
                                        const std::vector<std::uint8_t>& blob,
                                        int channels) {
    if (!raw || fileId <= 0 || blob.empty() || channels <= 0) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = R"(
        INSERT INTO waveforms_db.waveforms
            (file_id, waveform_detail, waveform_min_detail, detail_num_channels,
             status, error_code, retry_count, updated_at)
        VALUES (?, ?, NULL, ?, 'done', '', 0, 1)
        ON CONFLICT(file_id) DO UPDATE SET
            waveform_detail = excluded.waveform_detail,
            waveform_min_detail = NULL,
            detail_num_channels = excluded.detail_num_channels,
            status = 'done',
            error_code = '',
            retry_count = 0,
            updated_at = excluded.updated_at
    )";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, fileId);
    sqlite3_bind_blob(stmt, 2, blob.data(), (int)blob.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, channels);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static bool execSqlite(const std::string& dbPath, const char* sql) {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        return false;
    }
    char* err = nullptr;
    const bool ok = sqlite3_exec(raw, sql, nullptr, nullptr, &err) == SQLITE_OK;
    sqlite3_free(err);
    sqlite3_close(raw);
    return ok;
}

static bool execSqlite(sqlite3* raw, const char* sql) {
    if (!raw) return false;
    char* err = nullptr;
    const bool ok = sqlite3_exec(raw, sql, nullptr, nullptr, &err) == SQLITE_OK;
    if (!ok && err) {
        std::cerr << "sqlite exec failed: " << err << "\n";
    }
    sqlite3_free(err);
    return ok;
}

static int waveformSidecarRmsBytes(Database& db, const fs::path& mainDbPath) {
    const std::string waveformsPath = db.getWaveformsDbPathPublic(mainDbPath.u8string());
    return querySingleInt(
        waveformsPath,
        "SELECT COALESCE(length(rms_data), 0) FROM waveforms LIMIT 1",
        -1);
}

static int testScanPrecomputeWritesCompleteWaveformCache() {
    int failures = 0;
    const fs::path root = makeTempRoot("pending_only");
    const fs::path audioPath = root / "pending_only.wav";
    const fs::path dbPath = root / "pending_only.db";
    writeSilentMonoWav(audioPath, 48000, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS * 2);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const int processedBefore = db.precomputeProcessedCount();
    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, true);
    check(stats.added == 1, "scan inserts one audio file", failures);
    check(waitForPrecompute(db, processedBefore + 1), "precompute worker drains scan job", failures);

    WaveformCacheState cacheState;
    const bool hasCacheState = db.getWaveformCacheStateByPath(audioPath.u8string(), cacheState);
    check(hasCacheState, "scan writes waveform cache state row", failures);
    check(hasCacheState && cacheState.status == "done", "complete import marks waveform row done", failures);
    check(!db.getWaveformByPath(audioPath.u8string()).empty(), "complete import writes mini waveform blob", failures);
    Database::MiniWaveformBundle miniBundle;
    check(db.getMiniWaveformBundleByPath(audioPath.u8string(), miniBundle, /*targetFrames=*/1024),
          "complete import scan mini bundle is readable", failures);
    check(!miniBundle.peaks.empty(),
          "complete import scan writes decoded mini max peaks", failures);
    check(miniBundle.minPeaks.size() == miniBundle.peaks.size(),
          "complete import scan writes decoded parallel minPeaks", failures);
    const auto detailPeaks = db.getWaveformDetailByPath(audioPath.u8string());
    check(!detailPeaks.empty(), "complete import writes main detail blob", failures);
    check(detailPeaks.size() == (size_t)WaveformCachePolicy::kShortMainDetailPeaks,
          "complete import writes policy-sized main detail blob", failures);
    check(!db.getWaveformMinDetailByPath(audioPath.u8string()).empty(), "complete import writes main min detail blob", failures);
    check(db.getWaveformDetailChannelsByPath(audioPath.u8string()) == 1, "complete import writes main detail channels", failures);
    const std::string waveformsPath = db.getWaveformsDbPathPublic(dbPath.u8string());
    check(querySingleInt(waveformsPath, "SELECT length(waveform_detail) FROM waveforms LIMIT 1", 0) ==
              (WaveformCachePolicy::kShortMainDetailPeaks * 4 + 8),
          "complete import stores persistent main detail as WPK2", failures);
    check(querySingleInt(waveformsPath, "SELECT waveform_min_detail IS NULL FROM waveforms LIMIT 1", 0) == 1,
          "complete import stores min envelope inside WPK2 detail blob", failures);
    check(waveformSidecarRmsBytes(db, dbPath) == 0,
          "complete import leaves rms_data NULL for default waveform cache", failures);
    check(db.countFilesNeedingDetailCache() == 0, "complete import leaves no missing detail waveform task", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testCompleteImportSkipsPersistentDetailForOverFiveMinutes() {
    int failures = 0;
    const fs::path root = makeTempRoot("long_file_mini_only");
    const fs::path audioPath = root / "long_file_mini_only.wav";
    const fs::path dbPath = root / "long_file_mini_only.db";
    writeSilentMonoWav(audioPath, 8000, 8000 * 301);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const int processedBefore = db.precomputeProcessedCount();
    const ScanStats stats = db.scanDirectoryEx(root.u8string(),
                                               true,
                                               false,
                                               false,
                                               nullptr,
                                               nullptr,
                                               1,
                                               /*completeImport=*/true);
    check(stats.added == 1, "long-file import inserts one audio file", failures);
    check(stats.waveformLongDetailSkipped >= 1,
          "long-file import reports skipped persistent main detail", failures);
    check(waitForPrecompute(db, processedBefore + 1),
          "long-file precompute worker drains mini-only job", failures);

    WaveformCacheState cacheState;
    const bool hasCacheState = db.getWaveformCacheStateByPath(audioPath.u8string(), cacheState);
    check(hasCacheState, "long-file import writes waveform cache state row", failures);
    check(hasCacheState && cacheState.status == "done",
          "long-file import marks mini-only waveform row done", failures);
    check(!db.getWaveformByPath(audioPath.u8string()).empty(),
          "long-file import still writes persistent mini waveform", failures);
    Database::MiniWaveformBundle longMiniBundle;
    check(db.getMiniWaveformBundleByPath(audioPath.u8string(), longMiniBundle, /*targetFrames=*/1024),
          "long-file import scan mini bundle is readable", failures);
    check(!longMiniBundle.peaks.empty(),
          "long-file import scan writes decoded mini max peaks", failures);
    check(longMiniBundle.minPeaks.size() == longMiniBundle.peaks.size(),
          "long-file import scan writes decoded parallel minPeaks", failures);
    const int longMiniChannels = std::max(1, longMiniBundle.miniNumChannels);
    const int longMiniPeakCount = static_cast<int>(longMiniBundle.peaks.size()) / longMiniChannels;
    check(longMiniPeakCount > 128,
          "long-file import scan adaptive sentinel exceeds old fixed 128 peaks", failures);
    check(longMiniPeakCount >= 512,
          "long-file import scan raised cap produces at least 512 decoded peaks", failures);
    check(db.getWaveformDetailByPath(audioPath.u8string()).empty(),
          "long-file import does not persist main detail", failures);
    check(db.getWaveformMinDetailByPath(audioPath.u8string()).empty(),
          "long-file import does not persist min detail", failures);
    check(db.countFilesNeedingDetailCache() == 0,
          "long-file import is not counted as missing persistent detail", failures);
    check(db.getWaveformDetailRebuildTasks(10, 0, true).empty(),
          "long-file import is not returned by incremental detail rebuild", failures);
    check(db.countFilesNeedingAnyWaveformCache() == 0,
          "long-file import does not keep any waveform cache debt after mini exists", failures);

    const std::string waveformsPath = db.getWaveformsDbPathPublic(dbPath.u8string());
    check(querySingleInt(waveformsPath,
                         "SELECT COALESCE(length(waveform_detail), 0) FROM waveforms LIMIT 1",
                         -1) == 0,
          "long-file sidecar detail blob remains NULL", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testScanStatsReportIngestSummary() {
    int failures = 0;
    const fs::path root = makeTempRoot("ingest_summary");
    const fs::path audioPath = root / "ingest_summary.wav";
    const fs::path dbPath = root / "ingest_summary.db";
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    auto& diag = DiagnosticsLogger::instance();
    diag.setEnabled(true);
    diag.setMaxEntries(512);
    diag.clear();

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "scan inserts one file for ingest summary", failures);
    check(stats.ingested == 1, "scan stats reports this-run ingested count", failures);
    check(stats.failedFiles == 0, "successful scan reports no failed files", failures);
    check(stats.ingestDurationMs >= 0, "scan stats reports ingest duration", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testCompleteWaveformExtractionReturnsAllScanArtifacts() {
    int failures = 0;
    const fs::path root = makeTempRoot("complete_extract");
    const fs::path audioPath = root / "complete_extract.wav";
    writeSilentMonoWav(audioPath, 16000, 4096);

    WaveformExtractResult mini;
    WaveformExtractResult detail;
    const bool miniExtracted = WaveformExtractor::ExtractMini(audioPath.u8string(), 100, mini);
    const bool detailExtracted = WaveformExtractor::ExtractDetail(
        audioPath.u8string(),
        AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS,
        true,
        detail);

    check(miniExtracted, "mini extraction succeeds", failures);
    check(detailExtracted, "detail extraction succeeds", failures);
    check(!mini.peaks.empty(), "mini extraction returns peaks", failures);
    check(!detail.peaks.empty(), "detail extraction returns peaks", failures);
    check(!detail.rms.empty(), "detail extraction returns RMS", failures);
    check(detail.sampleRate == 16000, "detail extraction returns sample rate", failures);
    check(detail.channels == 1, "detail extraction returns channel count", failures);
    check(detail.decodedFrames == 4096, "detail extraction records decoded frame count", failures);
    check(detail.peaks.size() <= (size_t)AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS,
          "detail extraction respects the requested detail peak budget", failures);
    check(detail.rms.size() == detail.peaks.size(),
          "detail extraction returns RMS aligned with detail peaks", failures);
    check(detail.hasTrueRms, "detail extraction returns true RMS", failures);

    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testWaveformExtractionUnicodePathRejectsEmbeddedNul() {
    int failures = 0;
    const fs::path root = makeTempRoot("unicode_path_boundary");
    const fs::path audioPath = root / fs::u8path(u8"波形 😀.wav");
    writeSilentMonoWav(audioPath, 16000, 4096);

    const std::string validPath = audioPath.u8string();
    WaveformExtractResult mini;
    WaveformExtractResult detail;
    check(WaveformExtractor::ExtractMini(validPath, 100, mini),
          "Unicode path mini extraction succeeds", failures);
    check(WaveformExtractor::ExtractDetail(
              validPath, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS, true, detail),
          "Unicode path detail extraction succeeds", failures);

    std::string embeddedNulPath = validPath;
    embeddedNulPath.append("\0.wav", 5);
    WaveformExtractResult nulMini;
    nulMini.peaks = {1.0f};
    nulMini.minPeaks = {-1.0f};
    nulMini.rms = {0.5f};
    nulMini.channels = 2;
    nulMini.samplesPerPeak = 64;
    nulMini.decodedFrames = 128;
    nulMini.sampleRate = 48000;
    nulMini.ok = true;
    nulMini.errorCode = "stale";
    nulMini.trueRmsDbfs = -12.0f;
    nulMini.hasTrueRms = true;
    HarkenSearch::MediaFoundationAudio::DecodedAudioBuffer nulDecoded;
    nulDecoded.sampleRate = 48000;
    nulDecoded.channels = 2;
    nulDecoded.totalFrames = 128;
    nulDecoded.durationSeconds = 1.0f;
    nulDecoded.interleaved = {0.5f, -0.5f};
    WaveformExtractResult nulDetail = nulMini;

    check(!WaveformExtractor::ExtractMiniReturningDecoded(
              embeddedNulPath, 100, nulMini, false, nullptr, nulDecoded),
          "embedded NUL mini path does not open valid prefix", failures);
    check(nulMini.peaks.empty() && nulMini.minPeaks.empty() && nulMini.rms.empty(),
          "embedded NUL mini clears stale waveform vectors", failures);
    check(!nulMini.ok && nulMini.channels == 0 && nulMini.samplesPerPeak == 0 &&
              nulMini.decodedFrames == 0 && nulMini.sampleRate == 0 &&
              !nulMini.hasTrueRms && nulMini.trueRmsDbfs == -100.0f,
          "embedded NUL mini clears stale waveform metadata", failures);
    check(nulMini.errorCode == "decoder_init_failed",
          "embedded NUL mini reports decoder_init_failed", failures);
    check(nulDecoded.interleaved.empty() && nulDecoded.sampleRate == 0 &&
              nulDecoded.channels == 0 && nulDecoded.totalFrames == 0 &&
              nulDecoded.durationSeconds == 0.0f,
          "embedded NUL mini clears stale decoded audio", failures);
    check(!WaveformExtractor::ExtractDetail(
              embeddedNulPath, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS, true, nulDetail),
          "embedded NUL detail path does not open valid prefix", failures);
    check(nulDetail.peaks.empty() && nulDetail.minPeaks.empty() && nulDetail.rms.empty(),
          "embedded NUL detail clears stale waveform vectors", failures);
    check(!nulDetail.ok && nulDetail.channels == 0 && nulDetail.samplesPerPeak == 0 &&
              nulDetail.decodedFrames == 0 && nulDetail.sampleRate == 0 &&
              !nulDetail.hasTrueRms && nulDetail.trueRmsDbfs == -100.0f,
          "embedded NUL detail clears stale waveform metadata", failures);
    check(nulDetail.errorCode == "decoder_init_failed",
          "embedded NUL detail reports decoder_init_failed", failures);

    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFullImportRescanKeepsMissingDetailEligible() {
    int failures = 0;
    const fs::path root = makeTempRoot("rescan_missing_detail");
    const fs::path audioPath = root / "rescan_missing_detail.wav";
    const fs::path dbPath = root / "rescan_missing_detail.db";
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats initialStats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(initialStats.added == 1, "initial lightweight scan inserts one audio file", failures);
    check(db.countFilesNeedingDetailCache() == 1, "lightweight scan leaves one missing detail waveform task", failures);

    const int processedBefore = db.precomputeProcessedCount();
    const ScanStats fullImportRescan = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, true);
    check(fullImportRescan.total == 1, "complete import rescan still discovers the existing audio file", failures);
    check(fullImportRescan.added == 0 && fullImportRescan.updated == 0,
          "complete import rescan can finish with only skipped existing rows", failures);
    check(waitForPrecompute(db, processedBefore + 1), "complete import rescan queues missing waveform work", failures);
    check(db.countFilesNeedingDetailCache() == 0,
          "complete import rescan fills missing detail waveform cache", failures);
    check(db.getWaveformDetailRebuildTasks(8, 0, true).empty(),
          "complete import rescan does not leave post-scan rebuild tasks", failures);
    check(!db.getWaveformByPath(audioPath.u8string()).empty(), "complete import rescan writes mini waveform", failures);
    check(!db.getWaveformDetailByPath(audioPath.u8string()).empty(), "complete import rescan writes detail waveform", failures);
    check(!db.getWaveformMinDetailByPath(audioPath.u8string()).empty(), "complete import rescan writes min detail waveform", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFullImportRescanRepairsDoneRowWithMissingMiniBlob() {
    int failures = 0;
    const fs::path root = makeTempRoot("rescan_missing_mini_done");
    const fs::path audioPath = root / "rescan_missing_mini_done.wav";
    const fs::path dbPath = root / "rescan_missing_mini_done.db";
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats initialStats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(initialStats.added == 1, "initial lightweight scan inserts fixture file", failures);

    SearchFilter filter;
    const auto rows = db.searchFiles(filter, 4, 0);
    check(rows.size() == 1, "fixture row can be queried for cache key", failures);
    if (rows.empty()) {
        db.close();
        removeDbArtifacts(dbPath);
        fs::remove_all(root);
        return failures;
    }

    const std::string cacheKey = WaveformExtractor::BuildCacheKey(
        rows[0].filePath, rows[0].fileSize, rows[0].fileMtime);
    check(db.saveWaveformDetailByPath(audioPath.u8string(),
                                      {0.1f, 0.2f, 0.3f, 0.4f},
                                      {-0.1f, -0.2f, -0.3f, -0.4f}),
          "fixture detail blob saved", failures);
    check(db.upsertWaveformCacheStateByPath(audioPath.u8string(), "done", "", 0, cacheKey),
          "fixture malformed done state saved", failures);
    check(db.getWaveformByPath(audioPath.u8string()).empty(),
          "fixture starts with missing mini waveform blob", failures);
    check(db.hasWaveformDetailByPath(audioPath.u8string()),
          "fixture starts with existing detail waveform blob", failures);

    const int processedBefore = db.precomputeProcessedCount();
    const ScanStats fullImportRescan = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, true);
    check(fullImportRescan.total == 1, "complete import rescan discovers existing fixture", failures);
    check(waitForPrecompute(db, processedBefore + 1),
          "complete import rescan queues malformed done row for mini repair", failures);
    check(!db.getWaveformByPath(audioPath.u8string()).empty(),
          "complete import rescan repairs missing mini waveform blob", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testCompleteImportUsesBackgroundFolderCoverDiscovery() {
    int failures = 0;
    const fs::path root = makeTempRoot("cover_indexing");
    const fs::path nested = root / "Pack" / "Sub";
    const fs::path audioPath = nested / "whoosh.wav";
    const fs::path dbPath = root / "cover_indexing.db";
    std::error_code ec;
    fs::create_directories(nested, ec);
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, true);
    check(stats.added == 1, "complete import cover indexing scan inserts one nested audio file", failures);

    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(!rec.folderCoverKey.empty(), "complete import writes folder cover key using background discovery", failures);
    check(!rec.folderCoverSrcPath.empty(), "complete import writes folder cover source path", failures);

    const auto wall = db.getGlobalCoverWallEntries();
    check(!wall.empty(), "complete import finalized folder cover appears in global cover wall entries", failures);

    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root, ec);
    return failures;
}

static int testCompleteImportWritesFolderCoverThumb() {
    int failures = 0;
    const fs::path root = makeTempRoot("cover_indexing_complete");
    const fs::path nested = root / "Pack" / "Sub";
    const fs::path audioPath = nested / "whoosh.wav";
    const fs::path coverPath = root / "Pack" / "cover.jpg";
    const fs::path dbPath = root / "cover_indexing_complete.db";
    std::error_code ec;
    fs::create_directories(nested, ec);
    writeSilentMonoWav(audioPath);
    writeTinyCoverImage(coverPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, true);
    check(stats.added == 1, "complete import inserts one audio file with folder cover", failures);

    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(!rec.folderCoverKey.empty(), "complete import writes folder cover key", failures);

    std::vector<uint8_t> thumbData;
    std::string thumbMime;
    check(db.getCoverThumb(rec.folderCoverKey, thumbData, thumbMime) && !thumbData.empty(),
          "complete import writes folder cover thumbnail blob", failures);

    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root, ec);
    return failures;
}

static int testFastImportDefersCoverAndWaveformCaches() {
    int failures = 0;
    const fs::path root = makeTempRoot("fast_import_deferred_cache");
    const fs::path nested = root / "Pack" / "Sub";
    const fs::path audioPath = nested / "whoosh.wav";
    const fs::path coverPath = root / "Pack" / "cover.jpg";
    const fs::path dbPath = root / "fast_import_deferred_cache.db";
    std::error_code ec;
    fs::create_directories(nested, ec);
    writeSilentMonoWav(audioPath);
    writeTinyCoverImage(coverPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "fast import inserts one audio file", failures);

    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(rec.folderCoverKey.empty(), "fast import leaves folder cover key for later rebuild", failures);
    check(rec.folderCoverSrcPath.empty(), "fast import leaves folder cover source for later rebuild", failures);
    check(db.getWaveformDetailByPath(audioPath.u8string()).empty(),
          "fast import does not write main detail waveform cache", failures);
    check(db.countFilesNeedingDetailCache() == 1,
          "fast import leaves file eligible for later waveform rebuild", failures);

    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root, ec);
    return failures;
}

static int testWaveformPurgeApis() {
    int failures = 0;
    const fs::path root = makeTempRoot("purge");
    const fs::path audioPath = root / "purge.wav";
    const fs::path uncachedPath = root / "purge_uncached.wav";
    const fs::path dbPath = root / "purge.db";
    writeSilentMonoWav(audioPath);
    writeSilentMonoWav(uncachedPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 2, "two-file fixture scanned before purge checks", failures);
    check(db.saveWaveformByPath(audioPath.u8string(), {0.1f, 0.3f, 0.2f, 0.4f}, -18.0f, 1),
          "save mini waveform succeeds", failures);
    check(db.saveWaveformDetailByPath(audioPath.u8string(),
                                      {0.6f, 0.2f, 0.7f, 0.1f, 0.5f, 0.4f},
                                      {-0.6f, -0.2f, -0.7f, -0.1f, -0.5f, -0.4f}),
          "save detail waveform succeeds", failures);
    check(!db.getWaveformByPath(audioPath.u8string()).empty(), "mini waveform exists before purge", failures);
    check(!db.getWaveformDetailByPath(audioPath.u8string()).empty(), "detail waveform exists before purge", failures);
    check(db.saveRmsDataByPath(audioPath.u8string(), {0.2f, 0.3f, 0.4f}),
          "save main RMS proof before mini-only purge", failures);
    const int miniMissingBefore = db.countFilesNeedingWaveformCache();
    const int detailMissingBefore = db.countFilesNeedingDetailCache();
    check(miniMissingBefore == 1, "partial fixture starts with one missing mini cache", failures);
    check(detailMissingBefore == 1, "uncached fixture keeps detail debt explicit", failures);

    const int coldMiniPurged = db.purgeAllMiniWaveform();
    check(coldMiniPurged == 1, "mini-only purge reports only the cached row", failures);
    check(miniMissingBefore + coldMiniPurged == stats.added,
          "partial-cache accounting conserves every fixture row", failures);
    check(db.countFilesNeedingWaveformCache() == stats.added,
          "mini-only purge makes every fixture row cold", failures);
    check(!db.getWaveformDetailByPath(audioPath.u8string()).empty(),
          "mini-only purge preserves main detail", failures);
    check(!db.getWaveformMinDetailByPath(audioPath.u8string()).empty(),
          "mini-only purge preserves main min detail", failures);
    check(!db.getRmsDataByPath(audioPath.u8string()).empty(),
          "mini-only purge preserves main RMS", failures);
    check(db.countFilesNeedingDetailCache() == detailMissingBefore,
          "mini-only purge preserves detail eligibility", failures);
    check(db.saveWaveformByPath(audioPath.u8string(), {0.1f, 0.3f, 0.2f, 0.4f}, -18.0f, 1),
          "restore mini waveform for legacy purge checks", failures);

    check(db.clearWaveformCacheByPath(audioPath.u8string()),
          "single-file waveform cache clear succeeds", failures);
    check(db.getWaveformByPath(audioPath.u8string()).empty(),
          "single-file waveform cache clear removes mini waveform", failures);
    check(db.getWaveformDetailByPath(audioPath.u8string()).empty(),
          "single-file waveform cache clear removes detail waveform", failures);
    WaveformCacheState clearedState;
    check(db.getWaveformCacheStateByPath(audioPath.u8string(), clearedState) &&
              clearedState.status == "pending",
          "single-file waveform cache clear marks row pending", failures);

    check(db.saveWaveformByPath(audioPath.u8string(), {0.1f, 0.3f, 0.2f, 0.4f}, -18.0f, 1),
          "save mini waveform succeeds after single-file clear", failures);
    check(db.saveWaveformDetailByPath(audioPath.u8string(),
                                      {0.6f, 0.2f, 0.7f, 0.1f, 0.5f, 0.4f},
                                      {-0.6f, -0.2f, -0.7f, -0.1f, -0.5f, -0.4f}),
          "save detail waveform succeeds after single-file clear", failures);

    const int detailPurged = db.purgeAllWaveformDetail();
    check(detailPurged >= 1, "purgeAllWaveformDetail reports affected rows", failures);
    check(db.getWaveformDetailByPath(audioPath.u8string()).empty(), "detail waveform removed by purgeAllWaveformDetail", failures);
    check(!db.getWaveformByPath(audioPath.u8string()).empty(), "detail purge keeps mini waveform", failures);

    const int miniPurged = db.purgeAllMiniWaveform();
    check(miniPurged >= 1, "purgeAllMiniWaveform reports affected rows", failures);
    check(db.getWaveformByPath(audioPath.u8string()).empty(), "mini waveform removed by purgeAllMiniWaveform", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testCompleteDetailCacheDoesNotRequireRms() {
    int failures = 0;
    const fs::path root = makeTempRoot("detail_no_rms_mono");
    const fs::path audioPath = root / "detail_no_rms_mono.wav";
    const fs::path dbPath = root / "detail_no_rms_mono.db";
    writeSilentMonoWav(audioPath, 8000, 128);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "mono no-RMS fixture scanned", failures);

    std::vector<float> peaks(128, 0.25f);
    std::vector<float> mins(peaks.size(), -0.25f);
    check(db.saveWaveformDetailByPath(audioPath.u8string(), peaks, mins, 1),
          "mono complete detail cache saved without RMS", failures);
    check(waveformSidecarRmsBytes(db, dbPath) == 0,
          "mono complete detail cache leaves rms_data NULL", failures);
    check(db.countFilesNeedingDetailCache() == 0,
          "mono complete detail cache without RMS is not counted as missing", failures);
    check(db.getWaveformDetailRebuildTasks(10, 0, true).empty(),
          "complete max/min/channel detail cache does not require rms_data", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testStereoDetailCacheDoesNotRequireRms() {
    int failures = 0;
    const fs::path root = makeTempRoot("detail_no_rms_stereo");
    const fs::path audioPath = root / "detail_no_rms_stereo.wav";
    const fs::path dbPath = root / "detail_no_rms_stereo.db";
    writeSilentStereoWav(audioPath, 8000, 128);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "stereo no-RMS fixture scanned", failures);

    std::vector<float> peaks(128 * 2, 0.35f);
    std::vector<float> mins(peaks.size(), -0.35f);
    check(db.saveWaveformDetailByPath(audioPath.u8string(), peaks, mins, 2),
          "stereo complete detail cache saved without RMS", failures);
    check(db.getWaveformDetailChannelsByPath(audioPath.u8string()) == 2,
          "stereo complete detail cache stores channel count", failures);
    check(waveformSidecarRmsBytes(db, dbPath) == 0,
          "stereo complete detail cache leaves rms_data NULL", failures);
    check(db.countFilesNeedingDetailCache() == 0,
          "stereo complete detail cache without RMS is not counted as missing", failures);
    check(db.getWaveformDetailRebuildTasks(10, 0, true).empty(),
          "stereo detail cache without RMS is valid when channels=2", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testWpk2DetailCacheReadsAndDoesNotRebuild() {
    int failures = 0;
    const fs::path root = makeTempRoot("wpk2_detail_complete");
    const fs::path audioPath = root / "wpk2_detail_complete.wav";
    const fs::path dbPath = root / "wpk2_detail_complete.db";
    writeSilentStereoWav(audioPath, 48000, 48000);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "WPK2 fixture scanned", failures);
    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(rec.id > 0, "WPK2 fixture row has file id", failures);

    const int channels = 2;
    const int buckets = 2000;
    std::vector<float> peaks((size_t)channels * (size_t)buckets, 0.0f);
    std::vector<float> mins(peaks.size(), 0.0f);
    for (int frame = 0; frame < buckets; ++frame) {
        peaks[(size_t)frame * 2] = 0.25f;
        peaks[(size_t)frame * 2 + 1] = 0.50f;
        mins[(size_t)frame * 2] = -0.20f;
        mins[(size_t)frame * 2 + 1] = -0.45f;
    }
    std::vector<std::uint8_t> blob;
    check(WaveformExtractor::EncodePeakEnvelopeInt16(peaks, mins, channels, buckets, blob),
          "WPK2 fixture detail blob encodes", failures);
    check(blob.size() == 16008, "WPK2 stereo 2000-bucket blob is about 16KB plus header", failures);
    check(insertWpk2WaveformDetailRow(db.db(), rec.id, blob, channels),
          "WPK2-only sidecar detail row inserted", failures);

    const auto storedPeaks = db.getWaveformDetailByPath(audioPath.u8string());
    const auto storedMins = db.getWaveformMinDetailByPath(audioPath.u8string());
    check(storedPeaks.size() == peaks.size(), "WPK2 DB detail read restores max vector shape", failures);
    check(storedMins.size() == mins.size(), "WPK2 DB detail read restores min vector shape from waveform_detail", failures);
    if (storedPeaks.size() == peaks.size() && storedMins.size() == mins.size()) {
        check(approxEqual(storedPeaks[0], peaks[0]), "WPK2 DB max left channel stays close", failures);
        check(approxEqual(storedPeaks[1], peaks[1]), "WPK2 DB max right channel stays close", failures);
        check(approxEqual(storedMins[0], mins[0]), "WPK2 DB min left channel stays close", failures);
        check(approxEqual(storedMins[1], mins[1]), "WPK2 DB min right channel stays close", failures);
    }
    check(db.getWaveformDetailChannelsByPath(audioPath.u8string()) == channels,
          "WPK2 DB detail channel count remains readable", failures);
    check(db.hasWaveformDetailByPath(audioPath.u8string()),
          "WPK2-only detail row reports present", failures);
    check(db.countFilesNeedingDetailCache() == 0,
          "WPK2-only detail row is not counted as missing detail", failures);
    check(db.getFilesNeedingDetailCache(10).empty(),
          "WPK2-only detail row is not returned by fill-missing query", failures);
    check(db.getWaveformDetailRebuildTasks(10, 0, true).empty(),
          "WPK2-only detail row is not returned by rebuild cursor", failures);

    const int purged = db.purgeLowResWaveformDetail(10000);
    check(purged == 0, "WPK2-only detail row is exempt from legacy low-res purge", failures);
    check(db.hasWaveformDetailByPath(audioPath.u8string()),
          "WPK2-only detail row remains present after low-res purge", failures);

    db.close();

    Database reopened;
    check(reopened.open(dbPath.u8string()), "WPK2-only detail row survives DB reopen", failures);
    if (reopened.isOpen()) {
        check(reopened.hasWaveformDetailByPath(audioPath.u8string()),
              "WPK2-only detail row reports present after DB reopen", failures);
        check(reopened.countFilesNeedingDetailCache() == 0,
              "WPK2-only detail row is not counted as missing after DB reopen", failures);
        check(reopened.getWaveformDetailRebuildTasks(10, 0, true).empty(),
              "WPK2-only detail row is not returned by rebuild cursor after DB reopen", failures);
        reopened.close();
    }

    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testMalformedWpk2ChannelMetadataReentersRebuild() {
    int failures = 0;
    const fs::path root = makeTempRoot("wpk2_channel_mismatch");
    const fs::path audioPath = root / "wpk2_channel_mismatch.wav";
    const fs::path dbPath = root / "wpk2_channel_mismatch.db";
    writeSilentStereoWav(audioPath, 48000, 48000);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "WPK2 channel-mismatch fixture scanned", failures);
    const AudioRecord rec = db.getFileByPath(audioPath.u8string());
    check(rec.id > 0, "WPK2 channel-mismatch fixture row has file id", failures);

    const int blobChannels = 2;
    const int buckets = 2000;
    std::vector<float> peaks((size_t)blobChannels * (size_t)buckets, 0.25f);
    std::vector<float> mins(peaks.size(), -0.25f);
    std::vector<std::uint8_t> blob;
    check(WaveformExtractor::EncodePeakEnvelopeInt16(peaks, mins, blobChannels, buckets, blob),
          "WPK2 channel-mismatch fixture detail blob encodes", failures);
    check(insertWpk2WaveformDetailRow(db.db(), rec.id, blob, 1),
          "WPK2 channel-mismatch row inserted with mismatched DB metadata", failures);

    check(db.countFilesNeedingDetailCache() == 1,
          "malformed WPK2 channel metadata is counted as missing detail", failures);
    check(!db.getFilesNeedingDetailCache(10).empty(),
          "malformed WPK2 channel metadata returns to fill-missing query", failures);
    check(!db.getWaveformDetailRebuildTasks(10, 0, true).empty(),
          "malformed WPK2 channel metadata returns to rebuild cursor", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testWpk2DetailWriterDefaultsOnAndCanBeDisabled() {
    int failures = 0;
    setWpk2DetailWriterForTest(nullptr);

    const fs::path root = makeTempRoot("wpk2_detail_writer");
    const fs::path audioPath = root / "wpk2_detail_writer.wav";
    const fs::path dbPath = root / "wpk2_detail_writer.db";
    writeSilentStereoWav(audioPath, 48000, 48000);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        setWpk2DetailWriterForTest(nullptr);
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "WPK2 writer fixture scanned", failures);

    const int channels = 2;
    const int buckets = 2000;
    std::vector<float> peaks((size_t)channels * (size_t)buckets, 0.0f);
    std::vector<float> mins(peaks.size(), 0.0f);
    for (int frame = 0; frame < buckets; ++frame) {
        peaks[(size_t)frame * 2] = 0.25f;
        peaks[(size_t)frame * 2 + 1] = 0.50f;
        mins[(size_t)frame * 2] = -0.20f;
        mins[(size_t)frame * 2 + 1] = -0.45f;
    }

    check(db.saveWaveformDetailByPath(audioPath.u8string(), peaks, mins, channels),
          "WPK2 writer default-on detail save succeeds", failures);
    check(querySingleInt(db.db(), "SELECT length(waveform_detail) FROM waveforms_db.waveforms LIMIT 1", 0) == 16008,
          "SEMANTIC: WPK2 writer defaults to compact detail blob for new cache rows", failures);
    check(querySingleInt(db.db(), "SELECT waveform_min_detail IS NULL FROM waveforms_db.waveforms LIMIT 1", 0) == 1,
          "WPK2 writer default-on stores min envelope inside waveform_detail", failures);

    const auto storedPeaks = db.getWaveformDetailByPath(audioPath.u8string());
    const auto storedMins = db.getWaveformMinDetailByPath(audioPath.u8string());
    check(storedPeaks.size() == peaks.size(), "WPK2 writer default-on remains readable as max detail", failures);
    check(storedMins.size() == mins.size(), "WPK2 writer default-on remains readable as min detail", failures);
    if (storedPeaks.size() == peaks.size() && storedMins.size() == mins.size()) {
        check(approxEqual(storedPeaks[0], peaks[0]), "WPK2 writer default-on max left channel stays close", failures);
        check(approxEqual(storedPeaks[1], peaks[1]), "WPK2 writer default-on max right channel stays close", failures);
        check(approxEqual(storedMins[0], mins[0]), "WPK2 writer default-on min left channel stays close", failures);
        check(approxEqual(storedMins[1], mins[1]), "WPK2 writer default-on min right channel stays close", failures);
    }
    check(db.countFilesNeedingDetailCache() == 0,
          "WPK2 writer default-on does not re-enter fill-missing queue", failures);
    check(db.getWaveformDetailRebuildTasks(10, 0, true).empty(),
          "WPK2 writer default-on does not re-enter rebuild cursor", failures);

    setWpk2DetailWriterForTest("0");
    check(db.saveWaveformDetailByPath(audioPath.u8string(), peaks, mins, channels),
          "WPK2 writer opt-out legacy detail save succeeds", failures);
    setWpk2DetailWriterForTest(nullptr);
    check(querySingleInt(db.db(), "SELECT length(waveform_detail) FROM waveforms_db.waveforms LIMIT 1", 0) == (int)(peaks.size() * sizeof(float)),
          "WPK2 writer opt-out stores legacy float detail", failures);
    check(querySingleInt(db.db(), "SELECT waveform_min_detail IS NOT NULL FROM waveforms_db.waveforms LIMIT 1", 0) == 1,
          "WPK2 writer opt-out keeps legacy min detail sidecar", failures);

    db.close();
    setWpk2DetailWriterForTest(nullptr);
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testIncompleteDetailCacheStillRebuildsWithoutRms() {
    int failures = 0;
    setWpk2DetailWriterForTest("0");
    const fs::path root = makeTempRoot("detail_incomplete_no_rms");
    const fs::path audioPath = root / "detail_incomplete_no_rms.wav";
    const fs::path dbPath = root / "detail_incomplete_no_rms.db";
    writeSilentMonoWav(audioPath, 8000, 128);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        setWpk2DetailWriterForTest(nullptr);
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "incomplete no-RMS fixture scanned", failures);

    std::vector<float> peaks(128, 0.45f);
    check(db.saveWaveformDetailByPath(audioPath.u8string(), peaks, {}, 1),
          "fixture detail without min cache saved", failures);
    check(db.getWaveformDetailRebuildTasks(10, 0, true).empty(),
          "missing waveform_min_detail does not make fill-missing rebuild run", failures);

    std::vector<float> mins(peaks.size(), -0.45f);
    check(db.saveWaveformDetailByPath(audioPath.u8string(), peaks, mins, 1),
          "fixture complete detail cache saved before channel corruption", failures);
    const std::string waveformsPath = db.getWaveformsDbPathPublic(dbPath.u8string());
    check(execSqlite(waveformsPath, "UPDATE waveforms SET detail_num_channels = 0"),
          "fixture channel metadata corrupted to zero", failures);
    check(db.getWaveformDetailRebuildTasks(10, 0, true).empty(),
          "missing detail_num_channels does not make fill-missing rebuild run", failures);

    db.close();
    setWpk2DetailWriterForTest(nullptr);
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testMiniWaveformDerivesFromDetailWhenMiniCacheMissing() {
    int failures = 0;
    const fs::path root = makeTempRoot("mini_from_detail");
    const fs::path audioPath = root / "mini_from_detail.wav";
    const fs::path dbPath = root / "mini_from_detail.db";
    writeSilentMonoWav(audioPath, 8000, 512);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    auto& diag = DiagnosticsLogger::instance();
    diag.setEnabled(true);
    diag.setMaxEntries(512);
    diag.clear();

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "mini-from-detail fixture scanned", failures);
    check(db.getWaveformByPath(audioPath.u8string()).empty(),
          "mini-from-detail fixture starts without mini cache", failures);

    std::vector<float> peaks(AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS, 0.0f);
    std::vector<float> mins(peaks.size(), 0.0f);
    for (size_t i = 0; i < peaks.size(); ++i) {
        const float v = (i % 31 == 0) ? 0.8f : 0.2f;
        peaks[i] = v;
        mins[i] = -v;
    }
    check(db.saveWaveformDetailByPath(audioPath.u8string(), peaks, mins, 1),
          "mini-from-detail fixture stores complete detail cache", failures);

    Database::MiniWaveformBundle bundle;
    check(db.getMiniWaveformBundleByPath(audioPath.u8string(), bundle),
          "missing mini cache derives mini waveform from detail cache", failures);
    check(bundle.peaks.size() == (size_t)AppState::WAVEFORM_MINI_PEAKS,
          "detail-derived mini has default mini peak count", failures);
    check(bundle.rms.size() == bundle.peaks.size(),
          "detail-derived mini has display RMS fallback", failures);
    check(bundle.miniNumChannels == 1,
          "detail-derived mini preserves mono channel count", failures);
    check(db.getWaveformByPath(audioPath.u8string()).empty(),
          "detail-derived mini does not persist a new mini BLOB by default", failures);

    waitDiagDrain();
    const std::string miniDiag = diag.getRecentJsonLines(32);
    check(miniDiag.find("\"category\":\"MiniWave\"") != std::string::npos,
          "detail-derived mini emits MiniWave diagnostics", failures);
    check(miniDiag.find("DetailDerived") != std::string::npos,
          "detail-derived mini diagnostics record DetailDerived path", failures);
    diag.setEnabled(false);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testMiniWaveformDerivesFromMaxDetailWhenMinMissing() {
    int failures = 0;
    const fs::path root = makeTempRoot("mini_from_max_detail_only");
    const fs::path audioPath = root / "mini_from_max_detail_only.wav";
    const fs::path dbPath = root / "mini_from_max_detail_only.db";
    writeSilentMonoWav(audioPath, 8000, 512);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "max-only detail fixture scanned", failures);

    std::vector<float> peaks(AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS, 0.1f);
    peaks[17] = 0.85f;
    check(db.saveWaveformDetailByPath(audioPath.u8string(), peaks, {}, 1),
          "max-only detail cache saved", failures);
    check(db.getWaveformDetailRebuildTasks(10, 0, true).empty(),
          "max-only detail cache is enough for fill-missing waveform rebuild", failures);

    Database::MiniWaveformBundle bundle;
    check(db.getMiniWaveformBundleByPath(audioPath.u8string(), bundle),
          "missing mini cache derives display mini from max detail even when min detail is absent", failures);
    check(bundle.peaks.size() == (size_t)AppState::WAVEFORM_MINI_PEAKS,
          "max-only detail-derived mini has default mini peak count", failures);
    bool sawPeak = false;
    for (float peak : bundle.peaks) {
        if (peak > 0.8f) {
            sawPeak = true;
            break;
        }
    }
    check(sawPeak, "max-only detail-derived mini preserves positive peak", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testLongWavMiniPreservesNonCenterTransient() {
    int failures = 0;
    const fs::path root = makeTempRoot("mini_long_wav_transient");
    const fs::path audioPath = root / "mini_long_wav_transient.wav";
    const int sampleRate = 8000;
    const int frameCount = sampleRate * 32;
    const int targetPeaks = AppState::WAVEFORM_MINI_PEAKS;
    const int segmentFrames = frameCount / targetPeaks;
    const int impulseFrame = segmentFrames * 10 + 10;
    writeMonoImpulseWav(audioPath, sampleRate, frameCount, impulseFrame);

    WaveformExtractResult mini;
    check(WaveformExtractor::ExtractMini(audioPath.u8string(), targetPeaks, mini, false),
          "long wav mini extraction succeeds", failures);
    check(mini.peaks.size() == (size_t)targetPeaks,
          "long wav mini extraction returns requested peak count", failures);
    bool sawImpulse = false;
    for (float peak : mini.peaks) {
        if (peak > 0.5f) {
            sawImpulse = true;
            break;
        }
    }
    check(sawImpulse, "long wav mini preserves transient outside bucket center window", failures);

    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testWaveformRebuildTaskCursorAndBatchSave() {
    int failures = 0;
    const fs::path root = makeTempRoot("rebuild_cursor");
    const fs::path dbPath = root / "rebuild_cursor.db";
    const fs::path a = root / "a.wav";
    const fs::path b = root / "b.wav";
    const fs::path c = root / "c.wav";
    writeSilentMonoWav(a);
    writeSilentMonoWav(b);
    writeSilentMonoWav(c);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 3, "fixture files scanned before rebuild cursor checks", failures);

    const auto firstPage = db.getWaveformDetailRebuildTasks(2, 0, true);
    check(firstPage.size() == 2, "first rebuild page returns requested missing rows", failures);
    check(firstPage.size() == 2 && firstPage[0].fileId < firstPage[1].fileId,
          "first rebuild page is ordered by ascending file id", failures);

    std::vector<Database::WaveformDetailBatchEntry> batch;
    for (const auto& task : firstPage) {
        Database::WaveformDetailBatchEntry entry;
        entry.fileId = task.fileId;
        entry.waveform.assign(128, 0.1f);
        entry.minWaveform.assign(entry.waveform.size(), -0.1f);
        entry.rms.assign(entry.waveform.size(), 0.05f);
        entry.detailChannels = 1;
        batch.push_back(std::move(entry));
    }
    check(db.saveWaveformDetailBatch(batch), "batch waveform detail save succeeds", failures);

    const auto secondPage = db.getWaveformDetailRebuildTasks(
        2,
        firstPage.empty() ? 0 : firstPage.back().fileId,
        true);
    check(secondPage.size() == 1, "cursor advances to remaining missing row without repeats", failures);
    check(secondPage.size() == 1 &&
              std::all_of(firstPage.begin(), firstPage.end(), [&](const Database::WaveformRebuildTask& task) {
                  return task.fileId != secondPage.front().fileId;
              }),
          "second rebuild page does not repeat claimed rows", failures);

    std::vector<Database::WaveformDetailBatchEntry> finalBatch;
    for (const auto& task : secondPage) {
        Database::WaveformDetailBatchEntry entry;
        entry.fileId = task.fileId;
        entry.waveform.assign(128, 0.2f);
        entry.minWaveform.assign(entry.waveform.size(), -0.2f);
        entry.rms.assign(entry.waveform.size(), 0.08f);
        entry.detailChannels = 1;
        finalBatch.push_back(std::move(entry));
    }
    check(db.saveWaveformDetailBatch(finalBatch), "final waveform detail batch save succeeds", failures);
    check(db.getWaveformDetailRebuildTasks(10, 0, true).empty(),
          "incremental rebuild queue is empty after all detail blobs are written", failures);
    check(db.getWaveformDetailRebuildTasks(10, 0, false).size() == 3,
          "full rebuild task query streams all files regardless of existing detail", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testIncrementalWaveformRebuildPersistsPendingDetail() {
    int failures = 0;
    const fs::path root = makeTempRoot("incremental_rebuild");
    const fs::path audioPath = root / "incremental_rebuild.wav";
    const fs::path dbPath = root / "incremental_rebuild.db";
    writeSilentMonoWav(audioPath, 44100, 4096);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "scan inserts one file before detail rebuild batch", failures);
    check(db.countFilesNeedingDetailCache() == 1, "lightweight scan leaves one pending detail waveform task", failures);
    check(db.getWaveformDetailByPath(audioPath.u8string()).empty(), "detail waveform is absent before rebuild batch", failures);

    const auto tasks = db.getWaveformDetailRebuildTasks(8, 0, true);
    check(tasks.size() == 1, "missing detail query returns the scanned file", failures);

    std::vector<Database::WaveformDetailBatchEntry> batch;
    for (const auto& task : tasks) {
        WaveformExtractResult detail;
        const bool extracted = WaveformExtractor::ExtractDetail(
            task.filePath,
            AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS,
            true,
            detail);
        check(extracted && !detail.peaks.empty(), "detail waveform extraction succeeds for rebuild task", failures);
        if (extracted && !detail.peaks.empty()) {
            Database::WaveformDetailBatchEntry entry;
            entry.fileId = task.fileId;
            entry.waveform = std::move(detail.peaks);
            entry.minWaveform = std::move(detail.minPeaks);
            entry.rms = std::move(detail.rms);
            entry.detailChannels = detail.channels;
            batch.push_back(std::move(entry));
        }
    }

    check(db.saveWaveformDetailBatch(batch), "detail waveform rebuild batch persists successfully", failures);
    check(!db.getWaveformDetailByPath(audioPath.u8string()).empty(), "detail waveform exists after rebuild batch", failures);
    check(!db.getWaveformMinDetailByPath(audioPath.u8string()).empty(), "min detail waveform exists after rebuild batch", failures);
    check(db.getWaveformDetailChannelsByPath(audioPath.u8string()) == 1, "detail waveform channels exist after rebuild batch", failures);
    check(waveformSidecarRmsBytes(db, dbPath) == 0,
          "detail rebuild batch leaves rms_data NULL for default waveform cache", failures);
    check(db.countFilesNeedingDetailCache() == 0, "no pending detail waveform tasks remain after rebuild batch", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testWaveformRebuildCountsMissingMiniAndDetailOnce() {
    int failures = 0;
    const fs::path root = makeTempRoot("rebuild_union_count");
    const fs::path audioPath = root / "rebuild_union_count.wav";
    const fs::path dbPath = root / "rebuild_union_count.db";
    writeSilentMonoWav(audioPath, 44100, 4096);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "scan inserts one file before union rebuild count", failures);
    check(db.countFilesNeedingWaveformCache() == 1,
          "fixture is missing mini waveform cache", failures);
    check(db.countFilesNeedingDetailCache() == 1,
          "fixture is missing detail waveform cache", failures);
    check(db.countFilesNeedingAnyWaveformCache() == 1,
          "incremental rebuild counts file missing mini+detail once", failures);

    const std::vector<float> detail(4096, 0.2f);
    const std::vector<float> minDetail(detail.size(), -0.2f);
    check(db.saveWaveformDetailByPath(audioPath.u8string(), detail, minDetail, 1),
          "fixture detail cache save succeeds for union count", failures);
    check(db.countFilesNeedingDetailCache() == 0,
          "fixture no longer needs detail cache after detail save", failures);
    check(db.countFilesNeedingWaveformCache() == 1,
          "fixture still needs mini cache after detail-only save", failures);
    check(db.countFilesNeedingAnyWaveformCache() == 1,
          "incremental rebuild still counts mini-only missing file once", failures);

    const std::vector<float> mini(128, 0.2f);
    check(db.saveWaveformByPath(audioPath.u8string(), mini, -200.0f, 1),
          "fixture mini cache save succeeds for union count", failures);
    check(db.countFilesNeedingAnyWaveformCache() == 0,
          "incremental rebuild count is zero after mini and detail caches exist", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFillMissingWaveformSkipsFailedMiniDebt() {
    int failures = 0;
    const fs::path root = makeTempRoot("fill_missing_skips_failed_mini");
    const fs::path audioPath = root / "fill_missing_skips_failed_mini.wav";
    const fs::path dbPath = root / "fill_missing_skips_failed_mini.db";
    writeSilentMonoWav(audioPath, 44100, 4096);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    check(db.setWaveformCacheMode(WaveformCachePolicy::Mode::MiniOnly),
          "failed-mini fixture uses mini-only fill-missing mode", failures);
    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "failed-mini fixture scanned", failures);

    const auto rows = db.searchFiles(SearchFilter{}, 4, 0);
    check(rows.size() == 1, "failed-mini fixture row can be queried", failures);
    if (rows.empty()) {
        db.close();
        removeDbArtifacts(dbPath);
        fs::remove_all(root);
        return failures;
    }

    const std::string cacheKey = WaveformExtractor::BuildCacheKey(
        rows[0].filePath, rows[0].fileSize, rows[0].fileMtime);
    check(db.upsertWaveformCacheStateByPath(audioPath.u8string(),
                                            "failed",
                                            "mini_extract_failed",
                                            1,
                                            cacheKey),
          "BDD: Given mini waveform extraction has already failed permanently", failures);
    check(db.getWaveformByPath(audioPath.u8string()).empty(),
          "BDD: Given the failed row still has no mini waveform blob", failures);

    check(db.countFilesNeedingWaveformCache() == 0,
          "BDD: Then fill-missing mini count excludes failed debt instead of looping forever", failures);
    check(db.getFilesNeedingWaveformCache(10).empty(),
          "BDD: Then fill-missing mini queue excludes failed debt instead of requeueing it", failures);
    check(db.countFilesNeedingAnyWaveformCache() == 0,
          "BDD: Then mini-only any-cache debt also excludes failed debt", failures);

    check(db.clearWaveformCache(),
          "BDD: When user chooses full rebuild/clear cache, failed debt can be retried deliberately", failures);
    check(db.countFilesNeedingWaveformCache() == 1,
          "BDD: Then full rebuild clear returns the row to retryable missing-mini debt", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testWaveformRebuildIgnoresLegacyMainMiniWhenSidecarMissing() {
    int failures = 0;
    const fs::path root = makeTempRoot("rebuild_legacy_main_mini");
    const fs::path audioPath = root / "rebuild_legacy_main_mini.wav";
    const fs::path dbPath = root / "rebuild_legacy_main_mini.db";
    writeSilentMonoWav(audioPath, 44100, 4096);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    check(db.setWaveformCacheMode(WaveformCachePolicy::Mode::MiniOnly),
          "legacy-main-mini fixture uses Standard mini-only policy", failures);
    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "legacy-main-mini fixture scanned", failures);
    check(writeLegacyMainWaveformBlobForTest(dbPath),
          "legacy-main-mini fixture writes old main-table mini blob", failures);
    check(db.getWaveformByPath(audioPath.u8string()).empty(),
          "legacy-main-mini fixture sidecar mini remains missing", failures);
    check(db.countFilesNeedingWaveformCache() == 1,
          "fill-missing mini counts sidecar-missing file even if legacy main mini exists", failures);
    const auto ids = db.getFilesNeedingWaveformCache(10);
    check(ids.size() == 1,
          "fill-missing mini queues sidecar-missing file even if legacy main mini exists", failures);
    check(db.countFilesNeedingAnyWaveformCache() == 1,
          "standard policy any-waveform debt follows sidecar mini debt", failures);

    check(db.saveWaveformByPath(audioPath.u8string(), std::vector<float>(128, 0.2f), -200.0f, 1),
          "legacy-main-mini fixture sidecar mini save succeeds", failures);
    check(db.countFilesNeedingAnyWaveformCache() == 0,
          "standard policy any-waveform debt clears after sidecar mini exists", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testWaveformRebuildExactTotalsIgnoreFolderIndexCache() {
    int failures = 0;
    const fs::path root = makeTempRoot("rebuild_exact_totals");
    const fs::path shortPath = root / "short.wav";
    const fs::path longPath = root / "long.wav";
    const fs::path dbPath = root / "rebuild_exact_totals.db";
    writeSilentMonoWav(shortPath, 44100, 4096);
    writeSilentMonoWav(longPath, 44100, 4096);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 2, "exact total fixture scans two files", failures);
    auto execRaw = [&](const char* sql) {
        return sqlite3_exec(db.db(), sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    };
    check(execRaw("UPDATE audio_files SET duration = 360.0 WHERE file_name = 'long.wav'"),
          "exact total fixture marks one long file", failures);
    check(execRaw("DELETE FROM folder_index"),
          "exact total fixture clears folder index", failures);
    check(execRaw("INSERT INTO folder_index (folder_path, file_count, last_updated) VALUES ('stale', 1, 0)"),
          "exact total fixture creates stale folder index", failures);

    check(db.countAllFilesForWaveformRebuild() == 2,
          "full waveform rebuild total uses exact audio_files count", failures);
    check(db.countFilesEligibleForPersistentDetailCache() == 1,
          "full waveform rebuild detail total excludes >5 minute files", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testWaveformCacheVersionMigrationKeepsRmsData() {
    int failures = 0;
    setWpk2DetailWriterForTest("0");
    const fs::path root = makeTempRoot("migration_keeps_rms");
    const fs::path audioPath = root / "migration_keeps_rms.wav";
    const fs::path dbPath = root / "migration_keeps_rms.db";
    writeSilentMonoWav(audioPath);

    {
        Database db;
        if (!db.open(dbPath.u8string())) {
            std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
            setWpk2DetailWriterForTest(nullptr);
            fs::remove_all(root);
            return 1;
        }
        ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
        check(stats.added == 1, "migration fixture scanned", failures);
        const std::vector<float> detail(128, 0.25f);
        const std::vector<float> minDetail(128, -0.25f);
        check(db.saveWaveformDetailByPath(audioPath.u8string(), detail, minDetail, 1),
              "migration fixture detail cache saved", failures);
        check(db.saveRmsDataByPath(audioPath.u8string(), {0.1f, 0.2f, 0.3f, 0.4f}),
              "migration fixture rms cache saved", failures);
        check(waveformSidecarRmsBytes(db, dbPath) == 16, "rms exists before forced version migration", failures);
        check(!db.getWaveformDetailByPath(audioPath.u8string()).empty(),
              "detail waveform exists before forced version migration", failures);
        check(!db.getWaveformMinDetailByPath(audioPath.u8string()).empty(),
              "min detail waveform exists before forced version migration", failures);
        check(db.getWaveformDetailChannelsByPath(audioPath.u8string()) == 1,
              "detail channel metadata exists before forced version migration", failures);
        check(db.countFilesNeedingDetailCache() == 0,
              "fixture detail cache is complete before forced version migration", failures);
        db.close();
    }
    setWpk2DetailWriterForTest(nullptr);

    const std::string sidecar = Database().getWaveformsDbPathPublic(dbPath.u8string());
    check(execSqlite(sidecar,
                     "INSERT INTO meta(key, value) VALUES('cache_version', '6') "
                     "ON CONFLICT(key) DO UPDATE SET value = excluded.value;"),
          "forced waveform cache version downgrade succeeds", failures);

    {
        Database db;
        check(db.open(dbPath.u8string()), "reopen triggers waveform cache migration", failures);
        check(waveformSidecarRmsBytes(db, dbPath) == 16,
              "v6 to current detail migration preserves rms_data", failures);
        check(db.getWaveformDetailByPath(audioPath.u8string()).empty(),
              "v6 to current detail migration removes stale detail waveform", failures);
        check(db.getWaveformMinDetailByPath(audioPath.u8string()).empty(),
              "v6 to current detail migration removes stale min detail waveform", failures);
        check(db.getWaveformDetailChannelsByPath(audioPath.u8string()) == 0,
              "v6 to current detail migration clears detail channel metadata", failures);
        check(db.countFilesNeedingDetailCache() == 1,
              "v6 to current detail migration makes file eligible for automatic detail rebuild", failures);
        check(querySingleInt(sidecar,
                             "SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'cache_version'",
                             0) == 8,
              "v6 to current detail migration persists waveform cache version", failures);
        db.close();
    }

    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testRemoveMissingDeletesWaveformRows() {
    int failures = 0;
    const fs::path root = makeTempRoot("remove_missing");
    const fs::path audioPath = root / "remove_missing.wav";
    const fs::path dbPath = root / "remove_missing.db";
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 1, "fixture file scanned before removeMissing checks", failures);
    check(db.saveWaveformByPath(audioPath.u8string(), {0.2f, 0.4f, 0.1f, 0.3f}, -20.0f, 1),
          "save mini waveform succeeds before removeMissing", failures);
    check(db.saveWaveformDetailByPath(audioPath.u8string(),
                                      {0.6f, 0.1f, 0.7f, 0.2f},
                                      {-0.6f, -0.1f, -0.7f, -0.2f}),
          "save detail waveform succeeds before removeMissing", failures);
    const std::string coverKey = "E|remove_missing_orphan_cover";
    const std::vector<std::uint8_t> coverThumb(64, 0x7f);
    check(db.assignEmbeddedCoverByPaths({audioPath.u8string()}, coverKey, 1),
          "assign embedded cover key before removeMissing", failures);
    db.upsertCoverRegistry(coverKey, audioPath.u8string(), true, coverKey);
    check(db.saveCoverThumb(coverKey, coverThumb, "image/jpeg"),
          "save cover thumbnail before removeMissing", failures);
    check(db.countCoverRegistryEntries() == 1,
          "cover registry row exists before removeMissing", failures);
    check(db.hasCoverThumb(coverKey),
          "cover thumbnail exists before removeMissing", failures);
    check(db.getGlobalCoverWallEntries().size() == 1,
          "cover wall sees fixture cover before removeMissing", failures);

    std::error_code ec;
    fs::remove(audioPath, ec);
    check(!fs::exists(audioPath), "fixture file deleted from disk", failures);

    stats = db.scanDirectoryEx(root.u8string(), true, false, true, nullptr, nullptr, 1, false);
    check(stats.removed == 1, "removeMissing scan reports one removed row", failures);

    SearchFilter filter;
    check(db.searchFilesCount(filter) == 0, "audio row removed from database", failures);

    WaveformCacheState cacheState;
    check(!db.getWaveformCacheStateByPath(audioPath.u8string(), cacheState),
          "waveform cache row removed together with audio row", failures);
    check(db.countCoverRegistryEntries() == 0,
          "orphan cover registry row removed together with audio row", failures);
    check(!db.hasCoverThumb(coverKey),
          "orphan cover thumbnail removed together with audio row", failures);
    check(db.getGlobalCoverWallEntries().empty(),
          "cover wall has no stale covers after removeMissing", failures);

    db.close();
    removeDbArtifacts(dbPath);
    fs::remove_all(root, ec);
    return failures;
}

static int testCoverWallIgnoresStaleOrphanRegistryRows() {
    int failures = 0;
    const fs::path root = makeTempRoot("orphan_cover_wall");
    const fs::path dbPath = root / "orphan_cover_wall.db";

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const std::string coverKey = "E|stale_orphan_cover";
    db.upsertCoverRegistry(coverKey, "C:/missing.wav", true, coverKey);
    check(db.countCoverRegistryEntries() == 1,
          "fixture stale cover registry row exists without audio rows", failures);
    check(db.getGlobalCoverWallEntries().empty(),
          "cover wall ignores stale registry entries without live audio rows", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testRemoveFolderSubtreeFromDbKeepsDiskFilesByDefault() {
    int failures = 0;
    const fs::path root = makeTempRoot("remove_folder_db_only");
    const fs::path target = root / "Target";
    const fs::path child = target / "Child";
    const fs::path sibling = root / "TargetSibling";
    const fs::path dbPath = root / "remove_folder_db_only.db";
    fs::create_directories(child);
    fs::create_directories(sibling);
    const fs::path targetFile = target / "a.wav";
    const fs::path childFile = child / "b.wav";
    const fs::path siblingFile = sibling / "c.wav";
    writeSilentMonoWav(targetFile);
    writeSilentMonoWav(childFile);
    writeSilentMonoWav(siblingFile);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    db.mergeLibraryRoots({root.u8string()});
    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 3, "folder removal fixture scanned", failures);
    check(db.getFileCount() == 3, "folder removal fixture starts with three counted rows", failures);

    const AudioRecord targetRec = db.getFileByPath(targetFile.u8string());
    const AudioRecord childRec = db.getFileByPath(childFile.u8string());
    const int64_t playlistId = db.createPlaylist("remove-folder-fixture");
    check(targetRec.id > 0 && childRec.id > 0 && playlistId > 0,
          "folder removal fixture ids are valid", failures);
    check(db.addToPlaylist(playlistId, targetRec.id), "target file added to playlist fixture", failures);
    check(db.addToPlaylist(playlistId, childRec.id), "child file added to playlist fixture", failures);
    check(db.markBackfillPending(targetRec.id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)),
          "target pending waveform fixture inserted", failures);
    check(db.markBackfillPending(childRec.id, static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb)),
          "child pending cover fixture inserted", failures);
    check(db.saveWaveformByPath(targetFile.u8string(), {0.1f, 0.2f, 0.3f}, -18.0f, 1),
          "target waveform fixture inserted", failures);
    check(db.saveWaveformDetailByPath(childFile.u8string(), {0.1f, 0.2f}, {0.1f}, 1),
          "child detail waveform fixture inserted", failures);
    check(execSqlite(db.db(),
                     "INSERT OR REPLACE INTO waveforms_db.waveforms(file_id, status) "
                     "VALUES(987654321, 'done')"),
          "stale orphan waveform fixture inserted", failures);
    check(execSqlite(db.db(),
                     "INSERT OR REPLACE INTO waveforms_db.external_waveforms(file_path, file_size, file_mtime) "
                     "VALUES('C:/stale-folder-cache.wav', 1, 1)"),
          "stale external waveform fixture inserted", failures);
    check(db.getPlaylistFileCount(playlistId) == 2,
          "playlist fixture starts with two file refs", failures);
    check(db.countBackfillPending() == 2,
          "backfill fixture starts with two pending rows", failures);

    const std::string targetCanon = HarkenSearch::CanonicalFolderPath(target.u8string());
    check(db.getFolderFileCount(targetCanon) == 2, "target subtree starts with two db rows", failures);
    const int removed = db.removeFolderSubtreeFromDb(target.u8string(), false);
    check(removed == 2, "db-only folder subtree removal reports two rows", failures);
    check(db.getFileCount() == 1, "db-only folder subtree removal invalidates total file count immediately", failures);
    check(!db.fileExists(targetFile.u8string()), "target root file db row removed", failures);
    check(!db.fileExists(childFile.u8string()), "target child file db row removed", failures);
    check(db.fileExists(siblingFile.u8string()), "same-prefix sibling folder db row is preserved", failures);
    check(fs::exists(targetFile), "db-only removal keeps target disk file", failures);
    check(fs::exists(childFile), "db-only removal keeps child disk file", failures);
    check(fs::exists(siblingFile), "db-only removal keeps sibling disk file", failures);
    check(db.getPlaylistFileCount(playlistId) == 0,
          "folder subtree removal clears playlist refs for removed files", failures);
    check(db.countBackfillPending() == 0,
          "folder subtree removal clears durable backfill rows for removed files", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM waveforms_db.waveforms w "
                         "LEFT JOIN main.audio_files af ON af.id = w.file_id "
                         "WHERE af.id IS NULL",
                         -1) == 0,
          "folder subtree removal prunes orphan waveform sidecar rows", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM waveforms_db.external_waveforms ew "
                         "LEFT JOIN main.audio_files af ON af.file_path = ew.file_path "
                         "WHERE af.id IS NULL",
                         -1) == 0,
          "folder subtree removal prunes orphan external waveform rows", failures);

    check(!db.saveWaveformDetailByPath(targetFile.u8string(),
                                       {0.9f, 0.8f, 0.7f, 0.6f},
                                       {-0.9f, -0.8f, -0.7f, -0.6f},
                                       1),
          "late precompute detail cannot external-cache a library-managed path after db-only removal", failures);
    check(countExternalWaveformRowsByPath(db.db(), targetFile.u8string()) == 0,
          "late precompute detail leaves no external cache row for managed target", failures);

    const std::vector<float> staleDetail = {0.9f, 0.8f, 0.7f, 0.6f};
    const std::vector<float> staleMinDetail = {-0.9f, -0.8f, -0.7f, -0.6f};
    check(insertExternalWaveformDetailRow(db.db(), targetFile, staleDetail, staleMinDetail, 1),
          "fixture can emulate a stale external detail row left by an older build", failures);
    check(countExternalWaveformRowsByPath(db.db(), targetFile.u8string()) == 1,
          "fixture stale external detail row exists for managed target", failures);
    check(db.getWaveformDetailByPath(targetFile.u8string()).empty(),
          "library-managed path ignores stale external detail while db row is absent", failures);
    check(db.getWaveformMinDetailByPath(targetFile.u8string()).empty(),
          "library-managed path ignores stale external min-detail while db row is absent", failures);
    check(db.getWaveformDetailChannelsByPath(targetFile.u8string()) == 0,
          "library-managed path ignores stale external channel count while db row is absent", failures);
    check(!db.hasWaveformDetailByPath(targetFile.u8string()),
          "library-managed path reports no detail while only stale external cache exists", failures);

    const ScanStats rescanStats = db.scanDirectoryEx(target.u8string(), true, false, false,
                                                     nullptr, nullptr, 1, false);
    check(rescanStats.added == 2,
          "rescanning unchanged disk files re-adds target subtree rows", failures);
    check(db.fileExists(targetFile.u8string()),
          "rescanned target root file db row exists", failures);
    check(db.fileExists(childFile.u8string()),
          "rescanned target child file db row exists", failures);
    check(db.getWaveformDetailByPath(targetFile.u8string()).empty(),
          "rescanned db row ignores stale external detail waveform", failures);
    check(db.getWaveformMinDetailByPath(targetFile.u8string()).empty(),
          "rescanned db row ignores stale external min-detail waveform", failures);
    check(db.getWaveformDetailChannelsByPath(targetFile.u8string()) == 0,
          "rescanned db row ignores stale external detail channel count", failures);
    check(!db.hasWaveformDetailByPath(targetFile.u8string()),
          "rescanned db row is still missing library-owned detail cache", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testRemoveDriveRootSubtreeCleansHiddenIndexedRows() {
    int failures = 0;
    const fs::path root = makeTempRoot("remove_drive_root_subtree");
    const fs::path dbPath = root / "remove_drive_root_subtree.db";

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    AudioRecord visible;
    visible.filePath = "H:\\Sound effect\\Visible\\visible.wav";
    visible.fileName = "visible.wav";
    visible.extension = ".wav";
    visible.format = "WAV";
    visible.fileSize = 10;
    visible.sampleRate = 48000;
    visible.channels = 1;
    visible.totalFrames = 48000;
    visible.durationSeconds = 1.0f;
    visible.embeddedCoverKey = "E|visible_cover";
    check(db.addFile(visible), "drive-root fixture inserts visible row", failures);

    AudioRecord hidden;
    hidden.filePath = "\\\\?\\H:\\Sound effect\\Hidden\\hidden.wav";
    hidden.fileName = "hidden.wav";
    hidden.extension = ".wav";
    hidden.format = "WAV";
    hidden.fileSize = 20;
    hidden.sampleRate = 48000;
    hidden.channels = 1;
    hidden.totalFrames = 96000;
    hidden.durationSeconds = 2.0f;
    hidden.folderCoverKey = "F|hidden_cover";
    check(db.addFile(hidden), "drive-root fixture inserts long-prefix hidden row", failures);

    AudioRecord sibling;
    sibling.filePath = "I:\\Sound effect\\Sibling\\sibling.wav";
    sibling.fileName = "sibling.wav";
    sibling.extension = ".wav";
    sibling.format = "WAV";
    sibling.fileSize = 30;
    sibling.sampleRate = 48000;
    sibling.channels = 1;
    sibling.totalFrames = 144000;
    sibling.durationSeconds = 3.0f;
    check(db.addFile(sibling), "drive-root fixture inserts other-drive sibling row", failures);

    const AudioRecord visibleRec = db.getFileByPath(visible.filePath);
    const AudioRecord hiddenRec = db.getFileByPath(hidden.filePath);
    check(visibleRec.id > 0 && hiddenRec.id > 0, "drive-root fixture ids are valid", failures);
    check(db.saveWaveformByPath(visible.filePath, {0.1f, 0.2f}, -18.0f, 1),
          "drive-root fixture visible waveform inserted", failures);
    check(db.saveWaveformDetailByPath(hidden.filePath, {0.3f, 0.4f}, {-0.3f, -0.4f}, 1),
          "drive-root fixture hidden detail inserted", failures);
    db.upsertCoverRegistry("E|visible_cover", visible.filePath, true, "E|visible_cover");
    db.upsertCoverRegistry("F|hidden_cover", hidden.filePath, false, "F|hidden_cover");
    check(db.saveCoverThumb("E|visible_cover", std::vector<uint8_t>(32, 0x11), "image/png"),
          "drive-root fixture visible cover thumb inserted", failures);
    check(db.saveCoverThumb("F|hidden_cover", std::vector<uint8_t>(32, 0x22), "image/png"),
          "drive-root fixture hidden cover thumb inserted", failures);
    check(execSqlite(db.db(),
                     "INSERT OR REPLACE INTO folder_index(folder_path, file_count, last_updated) "
                     "VALUES('h:\\sound effect\\visible', 1, 1)"),
          "drive-root fixture can emulate a stale partial folder_index", failures);
    check(execSqlite(db.db(),
                     "INSERT OR REPLACE INTO folder_cover_facet_scope_state(scope_path, scope_depth, row_count, updated_at) "
                     "VALUES('h:', 1, 2, 1)"),
          "drive-root fixture stale folder cover scope inserted", failures);
    check(execSqlite(db.db(),
                     "INSERT OR REPLACE INTO folder_cover_facet_index(scope_path, scope_depth, cover_key, rep_path, is_embedded, hit_count, updated_at) "
                     "VALUES('h:', 1, 'E|visible_cover', 'H:\\Sound effect\\Visible\\visible.wav', 1, 1, 1)"),
          "drive-root fixture stale cover facet inserted", failures);
    check(execSqlite(db.db(),
                     "INSERT OR REPLACE INTO folder_path_facet_index(scope_path, scope_depth, folder_path, hit_count, updated_at) "
                     "VALUES('h:', 1, 'h:\\sound effect\\hidden', 1, 1)"),
          "drive-root fixture stale path facet inserted", failures);

    check(querySingleInt(db.db(), "SELECT COUNT(*) FROM audio_files", -1) == 3,
          "drive-root fixture starts with three rows", failures);
    check(querySingleInt(db.db(),
                         "SELECT COALESCE(SUM(file_count), 0) FROM folder_index",
                         -1) == 1,
          "drive-root fixture starts with intentionally stale folder_index", failures);

    const int removed = db.removeFolderSubtreeFromDb("H:", false);
    check(removed == 2, "drive-root subtree removal reports both H rows", failures);
    check(!db.fileExists(visible.filePath), "drive-root removal deletes normal H row", failures);
    check(!db.fileExists(hidden.filePath), "drive-root removal deletes long-prefix H row", failures);
    check(db.fileExists(sibling.filePath), "drive-root removal preserves other-drive row", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM audio_files WHERE folder_path = 'h:' OR (folder_path >= 'h:\\' AND folder_path < 'h:\\' || char(255))",
                         -1) == 0,
          "drive-root removal leaves no H audio rows", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM waveforms_db.waveforms w "
                         "LEFT JOIN main.audio_files af ON af.id = w.file_id "
                         "WHERE af.id IS NULL",
                         -1) == 0,
          "drive-root removal prunes waveform rows for removed files", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM cover_cache_registry",
                         -1) == 0,
          "drive-root removal prunes cover registry rows for removed files", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM blobs_db.cover_thumbs",
                         -1) == 0,
          "drive-root removal prunes cover thumb blobs for removed files", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM folder_cover_facet_index WHERE scope_path = 'h:'",
                         -1) == 0,
          "drive-root removal invalidates stale cover facet scope", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM folder_path_facet_index WHERE scope_path = 'h:'",
                         -1) == 0,
          "drive-root removal invalidates stale path facet scope", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM folder_cover_facet_scope_state WHERE scope_path = 'h:'",
                         -1) == 0,
          "drive-root removal invalidates stale cover facet scope state", failures);
    check(querySingleInt(db.db(),
                         "SELECT COALESCE(SUM(file_count), 0) FROM folder_index",
                         -1) == 1,
          "drive-root removal rebuilds folder_index to surviving sibling only", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFolderHierarchyFromIndexRepairsStaleIndex() {
    int failures = 0;
    const fs::path root = makeTempRoot("folder_index_repair");
    const fs::path dbPath = root / "folder_index_repair.db";

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    AudioRecord a;
    a.filePath = "H:\\Sound effect\\A\\a.wav";
    a.fileName = "a.wav";
    a.extension = ".wav";
    a.format = "WAV";
    a.fileSize = 10;
    a.sampleRate = 48000;
    a.channels = 1;
    a.totalFrames = 48000;
    a.durationSeconds = 1.0f;
    check(db.addFile(a), "folder-index repair fixture inserts first row", failures);

    AudioRecord b = a;
    b.filePath = "H:\\Sound effect\\B\\b.wav";
    b.fileName = "b.wav";
    check(db.addFile(b), "folder-index repair fixture inserts second row", failures);

    check(db.rebuildFolderIndex(), "folder-index repair fixture builds baseline index", failures);
    check(execSqlite(db.db(),
                     "DELETE FROM folder_index WHERE folder_path = 'h:\\sound effect\\b'"),
          "folder-index repair fixture can corrupt one folder_index row", failures);
    check(querySingleInt(db.db(),
                         "SELECT COALESCE(SUM(file_count), 0) FROM folder_index",
                         -1) == 1,
          "folder-index repair fixture starts with stale index sum", failures);

    const FolderInfo tree = db.getFolderHierarchyFromIndex();
    check(!tree.subfolders.empty(), "folder-index repair returns a tree", failures);
    check(querySingleInt(db.db(),
                         "SELECT COALESCE(SUM(file_count), 0) FROM folder_index",
                         -1) == 2,
          "folder-index repair rebuilds stale index before returning tree", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testMultiFolderScanProgressAggregatesLocalCallbacks() {
    int failures = 0;

    HarkenSearch::MultiFolderScanProgressAggregator progress(2);
    auto firstStart = progress.beginFolder(0);
    if (firstStart.current != 0 || firstStart.total != 1) {
        std::cerr << "FAIL: first folder starts at 0/1 while local total is unknown"
                  << " (got " << firstStart.current << "/" << firstStart.total << ")\n";
        ++failures;
    }

    auto firstMid = progress.updateCurrentFolder(397, 500);
    if (firstMid.current != 397 || firstMid.total != 500) {
        std::cerr << "FAIL: first folder progress should mirror local progress"
                  << " (got " << firstMid.current << "/" << firstMid.total << ")\n";
        ++failures;
    }

    progress.finishFolder(0, 500);
    auto secondStart = progress.beginFolder(1);
    if (secondStart.current != 500 || secondStart.total != 501) {
        std::cerr << "FAIL: second folder start must not reset global progress to zero\n";
        ++failures;
    }

    auto secondMid = progress.updateCurrentFolder(20, 200);
    if (secondMid.current != 520 || secondMid.total != 700) {
        std::cerr << "FAIL: second folder local progress should be offset by completed folders\n";
        ++failures;
    }

    auto complete = progress.complete(700);
    if (complete.current != 700 || complete.total != 700) {
        std::cerr << "FAIL: aggregate completion should report combined total\n";
        ++failures;
    }

    return failures;
}

static int testRemoveFolderSubtreeMatchesLegacyCaseFolderPaths() {
    int failures = 0;
    const fs::path root = makeTempRoot("remove_folder_legacy_case");
    const fs::path dbPath = root / "remove_folder_legacy_case.db";

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    auto& diag = DiagnosticsLogger::instance();
    diag.setEnabled(true);
    diag.setMaxEntries(512);
    diag.clear();

    AudioRecord target;
    target.filePath = "H:\\Sound effect\\MixedCase\\BoomLibrary\\hit.wav";
    target.fileName = "hit.wav";
    target.extension = ".wav";
    target.format = "WAV";
    target.fileSize = 10;
    target.sampleRate = 48000;
    target.channels = 1;
    target.totalFrames = 48000;
    target.durationSeconds = 1.0f;
    check(db.addFile(target), "legacy-case fixture inserts target row", failures);

    AudioRecord child = target;
    child.filePath = "H:\\Sound effect\\MixedCase\\BoomLibrary\\Child\\child.wav";
    child.fileName = "child.wav";
    child.fileSize = 20;
    child.totalFrames = 96000;
    check(db.addFile(child), "legacy-case fixture inserts child row", failures);

    AudioRecord sibling = target;
    sibling.filePath = "H:\\Sound effect\\MixedCase\\BoomLibrarySibling\\keep.wav";
    sibling.fileName = "keep.wav";
    sibling.fileSize = 30;
    sibling.totalFrames = 144000;
    check(db.addFile(sibling), "legacy-case fixture inserts sibling row", failures);

    check(execSqlite(db.db(),
                     "UPDATE audio_files "
                     "SET folder_path = 'H:\\Sound effect\\MixedCase\\BoomLibrary' "
                     "WHERE file_name = 'hit.wav'"),
          "legacy-case fixture rewrites target folder_path to display case", failures);
    check(execSqlite(db.db(),
                     "UPDATE audio_files "
                     "SET folder_path = 'H:\\Sound effect\\MixedCase\\BoomLibrary\\Child' "
                     "WHERE file_name = 'child.wav'"),
          "legacy-case fixture rewrites child folder_path to display case", failures);
    check(execSqlite(db.db(),
                     "UPDATE audio_files "
                     "SET folder_path = 'H:\\Sound effect\\MixedCase\\BoomLibrarySibling' "
                     "WHERE file_name = 'keep.wav'"),
          "legacy-case fixture rewrites sibling folder_path to display case", failures);
    check(execSqlite(db.db(),
                     "INSERT OR REPLACE INTO folder_index(folder_path, file_count, last_updated) "
                     "VALUES('H:\\Sound effect\\MixedCase\\BoomLibrary', 2, 1)"),
          "legacy-case fixture inserts stale display-case folder index", failures);

    const int removed = db.removeFolderSubtreeFromDb("h:\\Sound effect\\MixedCase\\BoomLibrary", false);
    check(removed == 2, "legacy-case subtree removal reports target and child rows", failures);
    check(!db.fileExists(target.filePath), "legacy-case removal deletes target row", failures);
    check(!db.fileExists(child.filePath), "legacy-case removal deletes child row", failures);
    check(db.fileExists(sibling.filePath), "legacy-case removal preserves same-prefix sibling", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM folder_index WHERE folder_path LIKE 'H:\\Sound effect\\MixedCase\\BoomLibrary%'",
                         -1) == 1,
          "legacy-case removal rebuilds folder index with only sibling remaining", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testRemoveFolderAncestorSubtreeMatchesLegacyCaseDescendants() {
    int failures = 0;
    const fs::path root = makeTempRoot("remove_folder_legacy_case_ancestor");
    const fs::path dbPath = root / "remove_folder_legacy_case_ancestor.db";

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    AudioRecord first;
    first.filePath = "H:\\Sound effect\\MixedCase\\BoomLibrary\\hit.wav";
    first.fileName = "hit.wav";
    first.extension = ".wav";
    first.format = "WAV";
    first.fileSize = 10;
    first.sampleRate = 48000;
    first.channels = 1;
    first.totalFrames = 48000;
    first.durationSeconds = 1.0f;
    check(db.addFile(first), "legacy-ancestor fixture inserts first descendant row", failures);

    AudioRecord second = first;
    second.filePath = "H:\\Sound effect\\MixedCase\\SoundMorph音效合辑\\child.wav";
    second.fileName = "child.wav";
    second.fileSize = 20;
    second.totalFrames = 96000;
    second.durationSeconds = 2.0f;
    check(db.addFile(second), "legacy-ancestor fixture inserts second descendant row", failures);

    AudioRecord sibling = first;
    sibling.filePath = "H:\\Sound effect\\KeepCase\\keep.wav";
    sibling.fileName = "keep.wav";
    sibling.fileSize = 30;
    sibling.totalFrames = 144000;
    sibling.durationSeconds = 3.0f;
    check(db.addFile(sibling), "legacy-ancestor fixture inserts sibling row", failures);

    check(execSqlite(db.db(),
                     "UPDATE audio_files "
                     "SET folder_path = 'H:\\Sound effect\\MixedCase\\BoomLibrary' "
                     "WHERE file_name = 'hit.wav'"),
          "legacy-ancestor fixture rewrites first descendant folder_path to display case", failures);
    check(execSqlite(db.db(),
                     "UPDATE audio_files "
                     "SET folder_path = 'H:\\Sound effect\\MixedCase\\SoundMorph音效合辑' "
                     "WHERE file_name = 'child.wav'"),
          "legacy-ancestor fixture rewrites second descendant folder_path to display case", failures);
    check(execSqlite(db.db(),
                     "UPDATE audio_files "
                     "SET folder_path = 'H:\\Sound effect\\KeepCase' "
                     "WHERE file_name = 'keep.wav'"),
          "legacy-ancestor fixture rewrites sibling folder_path to display case", failures);
    check(execSqlite(db.db(),
                     "INSERT OR REPLACE INTO folder_index(folder_path, file_count, last_updated) "
                     "VALUES('H:\\Sound effect\\MixedCase\\BoomLibrary', 1, 1)"),
          "legacy-ancestor fixture inserts first stale folder index row", failures);
    check(execSqlite(db.db(),
                     "INSERT OR REPLACE INTO folder_index(folder_path, file_count, last_updated) "
                     "VALUES('H:\\Sound effect\\MixedCase\\SoundMorph音效合辑', 1, 1)"),
          "legacy-ancestor fixture inserts second stale folder index row", failures);

    const int removed = db.removeFolderSubtreeFromDb("h:\\sound effect\\mixedcase", false);
    check(removed == 2, "legacy-ancestor subtree removal reports both descendant rows", failures);
    check(!db.fileExists(first.filePath), "legacy-ancestor removal deletes first descendant row", failures);
    check(!db.fileExists(second.filePath), "legacy-ancestor removal deletes second descendant row", failures);
    check(db.fileExists(sibling.filePath), "legacy-ancestor removal preserves sibling branch", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM audio_files "
                         "WHERE folder_path LIKE 'H:\\Sound effect\\MixedCase%'",
                         -1) == 0,
          "legacy-ancestor removal leaves no mixed-case subtree rows behind", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testRemoveFolderSubtreeSkipsMissingLegacyBackfillTable() {
    int failures = 0;
    const fs::path root = makeTempRoot("remove_folder_missing_backfill_pending");
    const fs::path dbPath = root / "remove_folder_missing_backfill_pending.db";

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    AudioRecord target;
    target.filePath = "H:\\Sound effect\\综合\\SoundMorph音效合辑\\hit.wav";
    target.fileName = "hit.wav";
    target.extension = ".wav";
    target.format = "WAV";
    target.fileSize = 10;
    target.sampleRate = 48000;
    target.channels = 1;
    target.totalFrames = 48000;
    target.durationSeconds = 1.0f;
    check(db.addFile(target), "missing-backfill fixture inserts target row", failures);

    AudioRecord sibling = target;
    sibling.filePath = "H:\\Sound effect\\KeepCase\\keep.wav";
    sibling.fileName = "keep.wav";
    sibling.fileSize = 20;
    sibling.totalFrames = 96000;
    sibling.durationSeconds = 2.0f;
    check(db.addFile(sibling), "missing-backfill fixture inserts sibling row", failures);

    check(execSqlite(db.db(), "DROP TABLE backfill_pending;"),
          "missing-backfill fixture removes optional legacy backfill_pending table", failures);

    const int removed = db.removeFolderSubtreeFromDb("h:\\sound effect\\综合", false);
    check(removed == 1, "missing-backfill subtree removal still reports descendant row", failures);
    check(!db.fileExists(target.filePath), "missing-backfill removal deletes subtree row", failures);
    check(db.fileExists(sibling.filePath), "missing-backfill removal preserves sibling branch", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM audio_files "
                         "WHERE folder_path LIKE 'H:\\Sound effect\\综合%'",
                         -1) == 0,
          "missing-backfill removal leaves no target subtree rows behind", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testRemoveFolderSubtreeLeavesPackageCompactable() {
    int failures = 0;
    const fs::path root = makeTempRoot("remove_folder_compact_package");
    const fs::path dbPath = root / "remove_folder_compact_package.db";

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    AudioRecord target;
    target.filePath = "H:\\Sound effect\\综合\\SoundMorph音效合辑\\hit.wav";
    target.folderPath = "H:\\Sound effect\\综合\\SoundMorph音效合辑";
    target.fileName = "hit.wav";
    target.extension = ".wav";
    target.format = "WAV";
    target.fileSize = 10;
    target.sampleRate = 48000;
    target.channels = 1;
    target.totalFrames = 48000;
    target.durationSeconds = 1.0f;
    check(db.addFile(target), "compact-remove fixture inserts target row", failures);

    AudioRecord sibling = target;
    sibling.filePath = "I:\\Sound effect\\KeepCase\\keep.wav";
    sibling.folderPath = "I:\\Sound effect\\KeepCase";
    sibling.fileName = "keep.wav";
    sibling.fileSize = 20;
    sibling.totalFrames = 96000;
    sibling.durationSeconds = 2.0f;
    check(db.addFile(sibling), "compact-remove fixture inserts sibling row", failures);

    const std::vector<float> largeMini(4096, 0.10f);
    const std::vector<float> largeDetail(65536, 0.30f);
    const std::vector<float> largeMinDetail(65536, -0.30f);
    check(db.saveWaveformByPath(target.filePath, largeMini, -18.0f, 1),
          "compact-remove fixture target mini inserted", failures);
    check(db.saveWaveformDetailByPath(target.filePath, largeDetail, largeMinDetail, 1),
          "compact-remove fixture target detail inserted", failures);
    check(db.saveWaveformByPath(sibling.filePath, {0.5f, 0.6f}, -18.0f, 1),
          "compact-remove fixture sibling mini inserted", failures);

    std::string compactError;
    check(db.compactCachePackage(compactError),
          "compact-remove fixture compacts before measuring package size", failures);
    const uint64_t packageBytesBeforeRemove = Database::GetDatabasePackageSize(dbPath.u8string());
    check(packageBytesBeforeRemove > 0,
          "compact-remove fixture package has physical size before subtree removal", failures);

    const int removed = db.removeFolderSubtreeFromDb("h:\\sound effect\\综合", false);
    check(removed == 1, "compact-remove subtree removal reports target row", failures);
    check(!db.fileExists(target.filePath), "compact-remove removal deletes subtree row", failures);
    check(db.fileExists(sibling.filePath), "compact-remove removal preserves sibling row", failures);

    std::string shrinkError;
    check(db.compactCachePackage(shrinkError),
          "compact-remove subtree removal leaves database package explicitly compactable", failures);
    const uint64_t packageBytesAfterCompact = Database::GetDatabasePackageSize(dbPath.u8string());
    check(packageBytesAfterCompact < packageBytesBeforeRemove,
          "compact-remove explicit compact shrinks database package after subtree removal", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testRemoveFolderSubtreeRecoversMalformedFtsIndexes() {
    int failures = 0;
    const fs::path root = makeTempRoot("remove_folder_repair_malformed_fts");
    const fs::path dbPath = root / "remove_folder_repair_malformed_fts.db";

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    AudioRecord target;
    target.filePath = "H:\\Sound effect\\综合\\SoundMorph音效合辑\\hit.wav";
    target.folderPath = "H:\\Sound effect\\综合\\SoundMorph音效合辑";
    target.fileName = "hit.wav";
    target.extension = ".wav";
    target.format = "WAV";
    target.fileSize = 10;
    target.sampleRate = 48000;
    target.channels = 1;
    target.totalFrames = 48000;
    target.durationSeconds = 1.0f;
    check(db.addFile(target), "malformed-fts fixture inserts target row", failures);

    AudioRecord sibling = target;
    sibling.filePath = "I:\\Sound effect\\KeepCase\\keep.wav";
    sibling.folderPath = "I:\\Sound effect\\KeepCase";
    sibling.fileName = "keep.wav";
    sibling.fileSize = 20;
    sibling.totalFrames = 96000;
    sibling.durationSeconds = 2.0f;
    check(db.addFile(sibling), "malformed-fts fixture inserts sibling row", failures);

    check(execSqlite(db.db(), "DELETE FROM audio_files_fts_data;"),
          "malformed-fts fixture corrupts FTS shadow data", failures);

    const int removed = db.removeFolderSubtreeFromDb("h:\\sound effect\\综合", false);
    check(removed == 1, "malformed-fts subtree removal retries after repairing FTS indexes", failures);
    check(!db.fileExists(target.filePath), "malformed-fts removal deletes subtree row", failures);
    check(db.fileExists(sibling.filePath), "malformed-fts removal preserves sibling row", failures);
    check(querySingleInt(db.db(),
                         "SELECT COUNT(*) FROM audio_files "
                         "WHERE folder_path LIKE 'H:\\Sound effect\\综合%'",
                         -1) == 0,
          "malformed-fts removal leaves no target subtree rows behind", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testClearWaveformCacheByFolderUsesCanonicalFolderPath() {
    int failures = 0;
    const fs::path root = makeTempRoot("clear_waveform_folder_canon");
    const fs::path dbPath = root / "clear_waveform_folder_canon.db";

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    AudioRecord target;
    target.filePath = "\\\\?\\H:\\Sound effect\\Folder\\target.wav";
    target.fileName = "target.wav";
    target.extension = ".wav";
    target.format = "WAV";
    target.fileSize = 10;
    target.sampleRate = 48000;
    target.channels = 1;
    target.totalFrames = 48000;
    target.durationSeconds = 1.0f;
    check(db.addFile(target), "clear-folder fixture inserts long-prefix target row", failures);

    AudioRecord sibling = target;
    sibling.filePath = "I:\\Sound effect\\Folder\\sibling.wav";
    sibling.fileName = "sibling.wav";
    check(db.addFile(sibling), "clear-folder fixture inserts sibling row", failures);

    const std::vector<float> largeMini(4096, 0.10f);
    const std::vector<float> largeDetail(65536, 0.30f);
    const std::vector<float> largeMinDetail(65536, -0.30f);
    check(db.saveWaveformByPath(target.filePath, largeMini, -18.0f, 1),
          "clear-folder fixture target mini inserted", failures);
    check(db.saveWaveformDetailByPath(target.filePath, largeDetail, largeMinDetail, 1),
          "clear-folder fixture target detail inserted", failures);
    check(db.saveWaveformByPath(sibling.filePath, {0.5f, 0.6f}, -18.0f, 1),
          "clear-folder fixture sibling mini inserted", failures);
    check(db.upsertWaveformCacheStateByPath(target.filePath, "done", "", 0, "target-cache-key"),
          "clear-folder fixture target cache state inserted", failures);

    const fs::path waveformsDbPath = fs::u8path(db.getWaveformsDbPathPublic(dbPath.u8string()));
    std::string compactError;
    check(db.compactCachePackage(compactError), "clear-folder fixture compacts before measuring size", failures);
    const uint64_t waveformsBytesBeforeClear = fileSizeIfExists(waveformsDbPath);
    check(waveformsBytesBeforeClear > 0, "clear-folder fixture waveforms sidecar has physical size", failures);

    const int cleared = db.clearWaveformCacheByFolder("H:");
    check(cleared > 0, "clear waveform cache by drive root affects canonical H row", failures);
    check(db.fileExists(target.filePath),
          "clear waveform cache by drive root keeps target database row", failures);
    check(db.getWaveformByPath(target.filePath).empty(),
          "clear waveform cache by drive root clears target mini", failures);
    check(db.getWaveformDetailByPath(target.filePath).empty(),
          "clear waveform cache by drive root clears target detail", failures);
    check(db.getWaveformMinDetailByPath(target.filePath).empty(),
          "clear waveform cache by drive root clears target min detail", failures);
    WaveformCacheState clearedState;
    const bool hasClearedState = db.getWaveformCacheStateByPath(target.filePath, clearedState);
    check(hasClearedState, "clear waveform cache by drive root keeps target cache state row", failures);
    check(hasClearedState && clearedState.status == "pending",
          "clear waveform cache by drive root marks target cache pending", failures);
    check(hasClearedState && clearedState.cacheKey.empty(),
          "clear waveform cache by drive root clears target cache key", failures);
    check(!db.getWaveformByPath(sibling.filePath).empty(),
          "clear waveform cache by drive root preserves sibling mini", failures);
    check(db.fileExists(sibling.filePath),
          "clear waveform cache by drive root keeps sibling database row", failures);
    const uint64_t waveformsBytesAfterClear = fileSizeIfExists(waveformsDbPath);
    check(waveformsBytesAfterClear < waveformsBytesBeforeClear,
          "clear waveform cache by drive root physically shrinks waveforms sidecar", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testExternalWaveformCacheStillWorksOutsideLibraryRoots() {
    int failures = 0;
    const fs::path root = makeTempRoot("external_waveform_outside_roots");
    const fs::path libraryRoot = root / "Library";
    const fs::path externalRoot = root / "External";
    const fs::path dbPath = root / "external_waveform_outside_roots.db";
    fs::create_directories(libraryRoot);
    fs::create_directories(externalRoot);
    const fs::path externalFile = externalRoot / "preview.wav";
    writeSilentMonoWav(externalFile);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const std::vector<float> detail = {0.1f, 0.2f, 0.3f, 0.4f};
    const std::vector<float> minDetail = {-0.1f, -0.2f, -0.3f, -0.4f};
    db.mergeLibraryRoots({libraryRoot.u8string()});
    check(db.saveWaveformDetailByPath(externalFile.u8string(), detail, minDetail, 1),
          "external file outside library roots can still use external waveform cache", failures);
    check(countExternalWaveformRowsByPath(db.db(), externalFile.u8string()) == 1,
          "external waveform cache row is stored for outside-library file", failures);
    const auto storedDetail = db.getWaveformDetailByPath(externalFile.u8string());
    const auto storedMinDetail = db.getWaveformMinDetailByPath(externalFile.u8string());
    check(storedDetail.size() == detail.size(),
          "outside-library external detail waveform remains readable", failures);
    check(storedMinDetail.size() == minDetail.size(),
          "outside-library external min-detail waveform remains readable", failures);
    if (storedDetail.size() == detail.size() && storedMinDetail.size() == minDetail.size()) {
        check(approxEqual(storedDetail[0], detail[0]),
              "outside-library external detail waveform preserves values", failures);
        check(approxEqual(storedMinDetail[0], minDetail[0]),
              "outside-library external min-detail waveform preserves values", failures);
    }
    check(db.getWaveformDetailChannelsByPath(externalFile.u8string()) == 1,
          "outside-library external detail channel count remains readable", failures);
    check(db.hasWaveformDetailByPath(externalFile.u8string()),
          "outside-library external detail reports present", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testRemoveFolderSubtreeFromDbCanDeleteDiskFiles() {
    int failures = 0;
    const fs::path root = makeTempRoot("remove_folder_delete_disk");
    const fs::path target = root / "Target";
    const fs::path sibling = root / "TargetSibling";
    const fs::path dbPath = root / "remove_folder_delete_disk.db";
    fs::create_directories(target);
    fs::create_directories(sibling);
    const fs::path targetFile = target / "a.wav";
    const fs::path siblingFile = sibling / "c.wav";
    writeSilentMonoWav(targetFile);
    writeSilentMonoWav(siblingFile);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 2, "delete-files fixture scanned", failures);

    const int removed = db.removeFolderSubtreeFromDb(target.u8string(), true);
    check(removed == 1, "delete-files folder subtree removal reports one row", failures);
    check(!db.fileExists(targetFile.u8string()), "delete-files target db row removed", failures);
    check(db.fileExists(siblingFile.u8string()), "delete-files sibling db row is preserved", failures);
    check(!fs::exists(targetFile), "checked delete removes target disk file", failures);
    check(fs::exists(siblingFile), "checked delete preserves sibling disk file", failures);
    check(fs::exists(target), "checked delete does not remove the folder itself", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testRemoveSingleFileInvalidatesCountAndFolderIndex() {
    int failures = 0;
    const fs::path root = makeTempRoot("remove_single_file_count");
    const fs::path target = root / "Target";
    const fs::path sibling = root / "Sibling";
    const fs::path dbPath = root / "remove_single_file_count.db";
    fs::create_directories(target);
    fs::create_directories(sibling);
    const fs::path targetFile = target / "a.wav";
    const fs::path siblingFile = sibling / "b.wav";
    writeSilentMonoWav(targetFile);
    writeSilentMonoWav(siblingFile);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.added == 2, "single-file removal fixture scanned", failures);
    check(db.getFileCount() == 2, "single-file removal fixture starts with two counted rows", failures);
    const std::string targetCanon = HarkenSearch::CanonicalFolderPath(target.u8string());
    check(db.getFolderFileCount(targetCanon) == 1, "single-file removal target folder starts with one row", failures);

    check(db.removeFileByPath(targetFile.u8string()), "single-file db removal succeeds", failures);
    check(db.getFileCount() == 1, "single-file db removal invalidates total file count immediately", failures);
    check(db.getFolderFileCount(targetCanon) == 0, "single-file db removal invalidates folder index immediately", failures);
    check(!db.fileExists(targetFile.u8string()), "single-file target db row removed", failures);
    check(db.fileExists(siblingFile.u8string()), "single-file sibling db row preserved", failures);
    check(fs::exists(targetFile), "single-file db removal keeps disk file", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testScanWriteInfrastructureFailureCountsDiscoveredFiles() {
    int failures = 0;
    const fs::path root = makeTempRoot("write_failure_counts");
    const fs::path dbPath = root / "write_failure_counts.db";
    for (int i = 0; i < 3; ++i) {
        writeSilentMonoWav(root / fs::u8path("forced_fail_" + std::to_string(i) + ".wav"));
    }

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    check(execSqlite(db.db(), "DROP INDEX IF EXISTS idx_files_path;"),
          "fixture removes scan writer lookup index", failures);

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.total == 3, "forced write failure discovers all fixture files", failures);
    check(stats.added == 0, "forced write failure does not report rolled-back files as added", failures);
    check(stats.errors == stats.total,
          "forced write failure accounts every discovered file as failed", failures);
    check(stats.failedFiles == stats.total,
          "forced write failure exposes a file-level failure count", failures);
    check(db.getFileCount() == 0, "forced write failure leaves database empty", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testScanBatchRetriesWhenDatabaseIsTemporarilyLocked() {
    int failures = 0;
    const fs::path root = makeTempRoot("write_lock_retry");
    const fs::path dbPath = root / "write_lock_retry.db";
    for (int i = 0; i < 3; ++i) {
        writeSilentMonoWav(root / fs::u8path("lock_retry_" + std::to_string(i) + ".wav"));
    }

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }
    check(execSqlite(db.db(), "PRAGMA busy_timeout = 0;"),
          "fixture disables scan busy timeout so retry budget owns lock recovery", failures);

    sqlite3* blocker = nullptr;
    if (sqlite3_open_v2(dbPath.u8string().c_str(), &blocker, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (blocker) sqlite3_close(blocker);
        db.close();
        removeDbArtifacts(dbPath);
        fs::remove_all(root);
        return failures + 1;
    }
    sqlite3_busy_timeout(blocker, 100);
    check(execSqlite(blocker, "BEGIN IMMEDIATE TRANSACTION;"),
          "fixture holds a temporary external write lock", failures);

    std::thread releaseLock([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        execSqlite(blocker, "COMMIT;");
        sqlite3_close(blocker);
        blocker = nullptr;
    });

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    releaseLock.join();

    check(stats.total == 3, "temporary-lock scan discovers all fixture files", failures);
    check(stats.added == 3, "temporary-lock scan retries and inserts all files", failures);
    check(stats.errors == 0, "temporary-lock scan does not count retried batch as errors", failures);
    check(stats.failedFiles == 0, "temporary-lock retry reports no failed files", failures);
    check(db.getFileCount() == 3, "temporary-lock scan leaves all rows queryable", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testScanBatchFailsFastWhenDatabaseLockPersists() {
    int failures = 0;
    const fs::path root = makeTempRoot("write_lock_fast_fail");
    const fs::path dbPath = root / "write_lock_fast_fail.db";
    for (int i = 0; i < 3; ++i) {
        writeSilentMonoWav(root / fs::u8path("lock_fast_fail_" + std::to_string(i) + ".wav"));
    }

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }
    check(execSqlite(db.db(), "PRAGMA busy_timeout = 0;"),
          "fixture disables scan busy timeout so scan retry budget is measurable", failures);

    sqlite3* blocker = nullptr;
    if (sqlite3_open_v2(dbPath.u8string().c_str(), &blocker, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (blocker) sqlite3_close(blocker);
        db.close();
        removeDbArtifacts(dbPath);
        fs::remove_all(root);
        return failures + 1;
    }
    sqlite3_busy_timeout(blocker, 100);
    check(execSqlite(blocker, "BEGIN IMMEDIATE TRANSACTION;"),
          "fixture holds a persistent external write lock", failures);

    setScanWriteLockRetryBudgetForTest("1000");
    const auto start = std::chrono::steady_clock::now();
    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    setScanWriteLockRetryBudgetForTest(nullptr);

    execSqlite(blocker, "COMMIT;");
    sqlite3_close(blocker);
    blocker = nullptr;

    check(elapsedMs < 1500,
          "persistent-lock scan fails within the interactive stall budget", failures);
    check(stats.total == 3, "persistent-lock scan discovers all fixture files", failures);
    check(stats.added == 0, "persistent-lock scan does not report rolled-back files as added", failures);
    check(stats.errors == stats.total,
          "persistent-lock scan accounts every discovered file as failed", failures);
    check(stats.failedFiles == stats.total,
          "persistent-lock scan exposes every unprocessed file as failed", failures);
    check(db.getFileCount() == 0, "persistent-lock scan leaves database empty", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testSlowScanWorkerDoesNotDropCompletedResultAfterStallWarning() {
    int failures = 0;
    const fs::path root = makeTempRoot("slow_worker_result");
    const fs::path dbPath = root / "slow_worker_result.db";
    const fs::path audioPath = root / "stall_slow_worker.wav";
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats stats = db.scanDirectoryEx(root.u8string(), true, false, false, nullptr, nullptr, 1, false);
    check(stats.total == 1, "slow worker fixture discovers one audio file", failures);
    check(stats.added == 1, "slow worker result is consumed after stall warning", failures);
    check(stats.errors == 0, "slow worker does not become a false scan error", failures);
    check(db.fileExists(audioPath.u8string()), "slow worker fixture row is stored in database", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFastScanQueuesWaveformBackfillWhenRequested() {
    int failures = 0;
    const fs::path root = makeTempRoot("fast_scan_backfill_queue");
    const fs::path dbPath = root / "fast_scan_backfill_queue.db";
    writeSilentMonoWav(root / "scan_backfill_a.wav");
    writeSilentMonoWav(root / "scan_backfill_b.wav");

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), 7);

    const ScanStats stats = db.scanDirectoryEx(root.u8string(),
                                               true,
                                               false,
                                               false,
                                               nullptr,
                                               nullptr,
                                               1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/true,
                                               &queue,
                                               dbPath.u8string(),
                                               7);
    check(stats.added == 2, "fast scan inserts both fixture files", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 2,
          "fast scan marks waveform backfill pending in the scan transaction", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb)) == 2,
          "fast scan marks cover backfill pending in the scan transaction", failures);
    const auto snapshot = queue.snapshot();
    check(snapshot.byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 2,
          "fast scan enqueues two waveform backfill jobs after batch commit", failures);
    check(snapshot.scanStaleRejected == 0,
          "matching scan db snapshot is accepted by backfill queue", failures);
    aura::BackfillJob firstJob{};
    check(queue.tryDequeue(firstJob), "scan backfill queue has a first job", failures);
    check(firstJob.kind == aura::BackfillJobKind::WaveformL1Overview,
          "waveform backfill is prioritized ahead of cover backfill after scan", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFastScanWaveformBackfillPrioritizesShortFiles() {
    int failures = 0;
    const fs::path root = makeTempRoot("fast_scan_backfill_priority");
    const fs::path dbPath = root / "fast_scan_backfill_priority.db";
    const fs::path shortPath = root / "scan_backfill_short.wav";
    const fs::path longPath = root / "scan_backfill_long.wav";
    writeSilentMonoWav(shortPath, 48000, 48000);
    writeSilentMonoWav(longPath, 48000, 48000 * 24);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), 29);

    const ScanStats stats = db.scanDirectoryEx(root.u8string(),
                                               true,
                                               false,
                                               false,
                                               nullptr,
                                               nullptr,
                                               1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/true,
                                               &queue,
                                               dbPath.u8string(),
                                               29);
    check(stats.added == 2, "priority fixture inserts both files", failures);
    aura::BackfillJob firstJob{};
    check(queue.tryDequeue(firstJob), "priority fixture has a first queued job", failures);
    check(firstJob.kind == aura::BackfillJobKind::WaveformL1Overview,
          "short waveform backfill outranks idle cover backfill", failures);
    const AudioRecord firstRecord = db.getFileById(firstJob.fileId);
    check(firstRecord.fileName == shortPath.filename().u8string(),
          "short waveform file is dequeued before long waveform file", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFastScanPendingPreservesExistingWaveformBlobs() {
    int failures = 0;
    const fs::path root = makeTempRoot("fast_scan_pending_preserves_blobs");
    const fs::path dbPath = root / "fast_scan_pending_preserves_blobs.db";
    const fs::path audioPath = root / "preserve_existing_blobs.wav";
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    const ScanStats initialStats = db.scanDirectoryEx(root.u8string(),
                                                      true,
                                                      false,
                                                      false,
                                                      nullptr,
                                                      nullptr,
                                                      1,
                                                      /*completeImport=*/false);
    check(initialStats.added == 1, "preserve-blob fixture is initially scanned", failures);

    SearchFilter filter;
    const auto rows = db.searchFiles(filter, 4, 0);
    check(rows.size() == 1, "preserve-blob fixture row can be queried", failures);
    if (rows.empty()) {
        db.close();
        removeDbArtifacts(dbPath);
        fs::remove_all(root);
        return failures;
    }

    const std::string currentCacheKey = WaveformExtractor::BuildCacheKey(
        rows[0].filePath, rows[0].fileSize, rows[0].fileMtime);
    check(db.saveWaveformByPath(audioPath.u8string(),
                                std::vector<float>(AppState::WAVEFORM_MINI_PEAKS, 0.2f),
                                -18.0f,
                                1),
          "preserve-blob fixture mini waveform saved", failures);
    check(db.saveWaveformDetailByPath(audioPath.u8string(),
                                      std::vector<float>(128, 0.4f),
                                      std::vector<float>(128, -0.4f),
                                      1),
          "preserve-blob fixture detail waveform saved", failures);
    check(db.upsertWaveformCacheStateByPath(audioPath.u8string(), "done", "", 0, "stale-cache-key"),
          "preserve-blob fixture starts with stale done cache key", failures);
    check(!db.getWaveformByPath(audioPath.u8string()).empty(),
          "preserve-blob fixture has mini waveform before rescan", failures);
    check(db.hasWaveformDetailByPath(audioPath.u8string()),
          "preserve-blob fixture has detail waveform before rescan", failures);

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), 23);

    const ScanStats rescanStats = db.scanDirectoryEx(root.u8string(),
                                                     true,
                                                     false,
                                                     false,
                                                     nullptr,
                                                     nullptr,
                                                     1,
                                                     /*completeImport=*/false,
                                                     /*enqueueExtraction=*/true,
                                                     &queue,
                                                     dbPath.u8string(),
                                                     23);
    check(rescanStats.added == 0 && rescanStats.updated == 0 && rescanStats.skipped == 1,
          "preserve-blob rescan skips unchanged fixture", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 1,
          "preserve-blob rescan marks one waveform backfill row", failures);

    WaveformCacheState state;
    check(db.getWaveformCacheStateByPath(audioPath.u8string(), state),
          "preserve-blob rescan keeps waveform state row", failures);
    check(state.status == "pending", "preserve-blob rescan marks state pending", failures);
    check(state.cacheKey == currentCacheKey, "preserve-blob rescan updates cache key", failures);
    check(!db.getWaveformByPath(audioPath.u8string()).empty(),
          "preserve-blob rescan keeps existing mini waveform blob", failures);
    check(db.hasWaveformDetailByPath(audioPath.u8string()),
          "preserve-blob rescan keeps existing detail waveform blob", failures);
    check(!db.getWaveformMinDetailByPath(audioPath.u8string()).empty(),
          "preserve-blob rescan keeps existing min-detail waveform blob", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFastScanReusesMiniCacheWithoutMainDetailInHighSpeedMode() {
    int failures = 0;
    const fs::path root = makeTempRoot("fast_scan_reuses_mini_without_detail");
    const fs::path dbPath = root / "fast_scan_reuses_mini_without_detail.db";
    const fs::path audioPath = root / "already_mini_cached.wav";
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }
    check(db.setWaveformCacheMode(WaveformCachePolicy::Mode::MiniAndMain),
          "BDD: Given a High-speed library policy is active", failures);

    aura::BackfillQueue firstQueue;
    firstQueue.onDbOpened(dbPath.u8string(), 41);
    const ScanStats initialStats = db.scanDirectoryEx(root.u8string(),
                                                      true,
                                                      false,
                                                      false,
                                                      nullptr,
                                                      nullptr,
                                                      1,
                                                      /*completeImport=*/false,
                                                      /*enqueueExtraction=*/true,
                                                      &firstQueue,
                                                      dbPath.u8string(),
                                                      41);
    check(initialStats.added == 1,
          "BDD: When the first fast scan imports one audio file, it creates the row", failures);

    const auto rows = db.searchFiles(SearchFilter{}, 4, 0);
    check(rows.size() == 1, "fixture row can be queried for cache key", failures);
    if (rows.empty()) {
        firstQueue.shutdown();
        db.close();
        removeDbArtifacts(dbPath);
        fs::remove_all(root);
        return failures;
    }

    const std::string cacheKey = WaveformExtractor::BuildCacheKey(
        rows[0].filePath, rows[0].fileSize, rows[0].fileMtime);
    check(db.saveWaveformByPath(audioPath.u8string(),
                                std::vector<float>(AppState::WAVEFORM_MINI_PEAKS, 0.25f),
                                -18.0f,
                                1),
          "BDD: Given the scan backfill has written a usable mini waveform", failures);
    check(db.upsertWaveformCacheStateByPath(audioPath.u8string(), "done", "", 0, cacheKey),
          "BDD: Given the scan backfill has marked the mini cache done", failures);
    db.clearBackfillPending(rows[0].id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview));
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 0,
          "BDD: Given no waveform pending rows remain after the first mini backfill", failures);
    check(db.getWaveformDetailByPath(audioPath.u8string()).empty(),
          "BDD: Given fast scan did not persist main detail", failures);

    firstQueue.shutdown();
    aura::BackfillQueue secondQueue;
    secondQueue.onDbOpened(dbPath.u8string(), 42);
    const ScanStats rescanStats = db.scanDirectoryEx(root.u8string(),
                                                     true,
                                                     false,
                                                     false,
                                                     nullptr,
                                                     nullptr,
                                                     1,
                                                     /*completeImport=*/false,
                                                     /*enqueueExtraction=*/true,
                                                     &secondQueue,
                                                     dbPath.u8string(),
                                                     42);
    check(rescanStats.total == 1 && rescanStats.skipped == 1,
          "BDD: When the same folder is scanned again, unchanged audio is skipped", failures);
    check(rescanStats.waveformCacheReused == 1,
          "BDD: Then fast scan reuses mini cache even when High-speed main detail is absent", failures);
    check(rescanStats.waveformPendingCreated == 0,
          "BDD: Then fast scan does not recreate waveform pending work for an already cached mini", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 0,
          "BDD: Then the DB remains free of repeated waveform pending rows", failures);
    check(secondQueue.snapshot().byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 0,
          "BDD: Then the scan queue receives no duplicate waveform job", failures);
    check(db.countFilesNeedingWaveformCache() == 0,
          "BDD: Then the fill-missing mini command has no work after fast scan mini cache is done", failures);
    check(db.countFilesNeedingDetailCache() == 1,
          "BDD: Then missing main detail remains a separate full-preheat concern", failures);
    check(db.countFilesNeedingAnyWaveformCache() == 1,
          "BDD: Then any-cache debt still reports main-detail debt for explicit full maintenance", failures);

    secondQueue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFastScanRescanRequeuesExistingPendingMiniCacheDebt() {
    int failures = 0;
    const fs::path root = makeTempRoot("fast_scan_requeues_pending_mini_debt");
    const fs::path dbPath = root / "fast_scan_requeues_pending_mini_debt.db";
    const fs::path audioPath = root / "already_imported_missing_mini.wav";
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    aura::BackfillQueue firstQueue;
    firstQueue.onDbOpened(dbPath.u8string(), 51);
    const ScanStats initialStats = db.scanDirectoryEx(root.u8string(),
                                                      true,
                                                      false,
                                                      false,
                                                      nullptr,
                                                      nullptr,
                                                      1,
                                                      /*completeImport=*/false,
                                                      /*enqueueExtraction=*/true,
                                                      &firstQueue,
                                                      dbPath.u8string(),
                                                      51);
    check(initialStats.added == 1,
          "BDD: Given fast scan has already imported the audio row", failures);

    const auto rows = db.searchFiles(SearchFilter{}, 4, 0);
    check(rows.size() == 1, "pending-debt fixture row can be queried", failures);
    if (rows.empty()) {
        firstQueue.shutdown();
        db.close();
        removeDbArtifacts(dbPath);
        fs::remove_all(root);
        return failures;
    }

    const std::string cacheKey = WaveformExtractor::BuildCacheKey(
        rows[0].filePath, rows[0].fileSize, rows[0].fileMtime);
    check(db.upsertWaveformCacheStateByPath(audioPath.u8string(), "pending", "", 0, cacheKey),
          "BDD: Given the previous mini waveform job is still marked pending", failures);
    check(db.getWaveformByPath(audioPath.u8string()).empty(),
          "BDD: Given no usable mini waveform has been written yet", failures);
    db.clearBackfillPending(rows[0].id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview));
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 0,
          "BDD: Given the durable waveform queue row was lost before restart", failures);

    firstQueue.shutdown();
    aura::BackfillQueue secondQueue;
    secondQueue.onDbOpened(dbPath.u8string(), 52);
    const ScanStats rescanStats = db.scanDirectoryEx(root.u8string(),
                                                     true,
                                                     false,
                                                     false,
                                                     nullptr,
                                                     nullptr,
                                                     1,
                                                     /*completeImport=*/false,
                                                     /*enqueueExtraction=*/true,
                                                     &secondQueue,
                                                     dbPath.u8string(),
                                                     52);
    check(rescanStats.total == 1 && rescanStats.skipped == 1,
          "BDD: When the same unchanged folder is scanned again, the file row is skipped", failures);
    check(rescanStats.waveformPendingCreated == 1,
          "BDD: Then rescan records one repaired mini waveform backfill job", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 1,
          "BDD: Then rescan restores the durable waveform backfill row", failures);
    check(secondQueue.snapshot().byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 1,
          "BDD: Then rescan re-enqueues the missing mini waveform job", failures);
    check(db.countFilesNeedingWaveformCache() == 1,
          "BDD: Then fill-missing mini still reports the file as needing mini cache until worker writes it", failures);

    secondQueue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFastScanRescanIgnoresLegacyMainMiniWhenSidecarMissing() {
    int failures = 0;
    const fs::path root = makeTempRoot("fast_scan_legacy_main_mini_sidecar_missing");
    const fs::path dbPath = root / "fast_scan_legacy_main_mini_sidecar_missing.db";
    const fs::path audioPath = root / "legacy_main_only_mini.wav";
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }
    check(db.setWaveformCacheMode(WaveformCachePolicy::Mode::MiniOnly),
          "BDD: Given Standard mini cache policy is active", failures);

    const ScanStats initialStats = db.scanDirectoryEx(root.u8string(),
                                                      true,
                                                      false,
                                                      false,
                                                      nullptr,
                                                      nullptr,
                                                      1,
                                                      /*completeImport=*/false);
    check(initialStats.added == 1,
          "BDD: Given the audio row was imported by an earlier lightweight scan", failures);

    const auto rows = db.searchFiles(SearchFilter{}, 4, 0);
    check(rows.size() == 1, "legacy-main scan fixture row can be queried", failures);
    if (rows.empty()) {
        db.close();
        removeDbArtifacts(dbPath);
        fs::remove_all(root);
        return failures;
    }

    const std::string cacheKey = WaveformExtractor::BuildCacheKey(
        rows[0].filePath, rows[0].fileSize, rows[0].fileMtime);
    check(writeLegacyMainWaveformBlobForTest(dbPath),
          "BDD: Given only a legacy main-table mini waveform blob exists", failures);
    check(db.getWaveformByPath(audioPath.u8string()).empty(),
          "BDD: Given the sidecar mini waveform is still missing", failures);
    check(db.upsertWaveformCacheStateByPath(audioPath.u8string(), "done", "", 0, cacheKey),
          "BDD: Given the stale cache state incorrectly says done", failures);
    db.clearBackfillPending(rows[0].id, static_cast<int>(aura::BackfillJobKind::WaveformL1Overview));
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 0,
          "BDD: Given no durable waveform repair row exists before rescan", failures);

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), 62);
    const ScanStats rescanStats = db.scanDirectoryEx(root.u8string(),
                                                     true,
                                                     false,
                                                     false,
                                                     nullptr,
                                                     nullptr,
                                                     1,
                                                     /*completeImport=*/false,
                                                     /*enqueueExtraction=*/true,
                                                     &queue,
                                                     dbPath.u8string(),
                                                     62);
    check(rescanStats.total == 1 && rescanStats.skipped == 1,
          "BDD: When the unchanged folder is scanned again, the file row is skipped", failures);
    check(rescanStats.waveformPendingCreated == 1,
          "BDD: Then fast rescan treats sidecar-missing mini as cache debt", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 1,
          "BDD: Then fast rescan restores durable waveform pending work", failures);
    check(queue.snapshot().byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 1,
          "BDD: Then fast rescan enqueues waveform backfill despite legacy main mini", failures);
    check(db.countFilesNeedingWaveformCache() == 1,
          "BDD: Then fill-missing mini agrees that sidecar mini is still missing", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFastScanMarksDurableWaveformBackfillWhenQueueSnapshotUnavailable() {
    int failures = 0;
    const fs::path root = makeTempRoot("fast_scan_waveform_queue_snapshot_unavailable");
    const fs::path dbPath = root / "fast_scan_waveform_queue_snapshot_unavailable.db";
    const fs::path audioPath = root / "snapshot_unavailable.wav";
    writeSilentMonoWav(audioPath);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    aura::BackfillQueue unopenedQueue;
    const ScanStats stats = db.scanDirectoryEx(root.u8string(),
                                               true,
                                               false,
                                               false,
                                               nullptr,
                                               nullptr,
                                               1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/true,
                                               &unopenedQueue,
                                               /*backfillDbPathSnapshot=*/"",
                                               /*backfillDbGenerationSnapshot=*/0);
    check(stats.added == 1,
          "BDD: Given fast scan imports a row while live queue snapshot is unavailable", failures);
    check(stats.waveformPendingCreated == 1,
          "BDD: Then scan still records durable mini waveform cache debt", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 1,
          "BDD: Then durable waveform pending survives even without live queue enqueue", failures);
    check(unopenedQueue.snapshot().byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 0,
          "BDD: Then unavailable queue does not receive an unsafe stale enqueue", failures);

    unopenedQueue.onDbOpened(dbPath.u8string(), 63);
    unopenedQueue.rebuildFromDatabase(&db, 8);
    check(unopenedQueue.snapshot().byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 1,
          "BDD: When the queue opens later, it can recover the durable mini waveform debt", failures);

    unopenedQueue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testCompleteImportStillRequiresMainDetailForHighSpeedMode() {
    int failures = 0;
    const fs::path root = makeTempRoot("complete_import_requires_detail");
    const fs::path dbPath = root / "complete_import_requires_detail.db";
    const fs::path audioPath = root / "mini_only_before_complete_import.wav";
    writeSilentMonoWav(audioPath, 16000, 4096);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }
    check(db.setWaveformCacheMode(WaveformCachePolicy::Mode::MiniAndMain),
          "BDD: Given a High-speed library is active for complete import", failures);

    const ScanStats initialStats = db.scanDirectoryEx(root.u8string(),
                                                      true,
                                                      false,
                                                      false,
                                                      nullptr,
                                                      nullptr,
                                                      1,
                                                      /*completeImport=*/false);
    check(initialStats.added == 1,
          "BDD: Given a lightweight scan has inserted one audio file", failures);
    const auto rows = db.searchFiles(SearchFilter{}, 4, 0);
    check(rows.size() == 1, "complete-import fixture row can be queried", failures);
    if (rows.empty()) {
        db.close();
        removeDbArtifacts(dbPath);
        fs::remove_all(root);
        return failures;
    }

    const std::string cacheKey = WaveformExtractor::BuildCacheKey(
        rows[0].filePath, rows[0].fileSize, rows[0].fileMtime);
    check(db.saveWaveformByPath(audioPath.u8string(),
                                std::vector<float>(AppState::WAVEFORM_MINI_PEAKS, 0.20f),
                                -18.0f,
                                1),
          "BDD: Given only mini waveform cache exists", failures);
    check(db.upsertWaveformCacheStateByPath(audioPath.u8string(), "done", "", 0, cacheKey),
          "BDD: Given the mini-only cache row is marked done", failures);
    check(db.getWaveformDetailByPath(audioPath.u8string()).empty(),
          "BDD: Given main detail is absent before complete import", failures);

    const int processedBefore = db.precomputeProcessedCount();
    const ScanStats completeStats = db.scanDirectoryEx(root.u8string(),
                                                       true,
                                                       false,
                                                       false,
                                                       nullptr,
                                                       nullptr,
                                                       1,
                                                       /*completeImport=*/true);
    check(completeStats.total == 1 && completeStats.skipped == 1,
          "BDD: When complete import rescans the unchanged file, it still inspects cache completeness", failures);
    check(waitForPrecompute(db, processedBefore + 1),
          "BDD: Then complete import queues missing main-detail work", failures);
    check(!db.getWaveformDetailByPath(audioPath.u8string()).empty(),
          "BDD: Then complete import fills main waveform detail", failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFastScanDefersMetadataToBackfillWorker() {
    int failures = 0;
    constexpr int64_t kDbGeneration = 31;
    constexpr int64_t kForegroundSessionId = 9301;
    const fs::path root = makeTempRoot("fast_scan_defers_metadata");
    const fs::path dbPath = root / "fast_scan_defers_metadata.db";
    const fs::path audioPath = root / "tagged_bext.wav";
    const std::string description = "Deferred BWF description";
    writeBextTaggedWav(audioPath, description);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), kDbGeneration);

    const ScanStats stats = db.scanDirectoryEx(root.u8string(),
                                               true,
                                               false,
                                               false,
                                               nullptr,
                                               nullptr,
                                               1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/false,
                                               &queue,
                                               dbPath.u8string(),
                                               kDbGeneration,
                                               kForegroundSessionId);
    check(stats.added == 1, "foreground fast scan inserts fixture", failures);

    AudioRecord row = db.getFileByPath(audioPath.u8string());
    check(row.id > 0, "foreground fast scan fixture row exists", failures);
    check(row.description == description, "foreground fast scan applies BWF description inline", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::MetadataRefresh)) == 0,
          "foreground fast scan does not leave metadata backfill pending", failures);
    check(queue.snapshot().byKind[static_cast<int>(aura::BackfillJobKind::MetadataRefresh)] == 0,
          "foreground fast scan does not enqueue metadata backfill job", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testFastScanDoesNotQueueWaveformBackfillWhenDisabled() {
    int failures = 0;
    const fs::path root = makeTempRoot("fast_scan_backfill_disabled");
    const fs::path dbPath = root / "fast_scan_backfill_disabled.db";
    writeSilentMonoWav(root / "scan_backfill_disabled.wav");

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), 11);

    const ScanStats stats = db.scanDirectoryEx(root.u8string(),
                                               true,
                                               false,
                                               false,
                                               nullptr,
                                               nullptr,
                                               1,
                                               /*completeImport=*/false,
                                               /*enqueueExtraction=*/false,
                                               &queue,
                                               dbPath.u8string(),
                                               11);
    check(stats.added == 1, "disabled backfill scan still inserts fixture file", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)) == 0,
          "disabled backfill scan does not mark durable waveform pending rows", failures);
    check(db.countBackfillPendingKind(static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb)) == 1,
          "disabled waveform scan still marks durable cover pending rows", failures);
    const auto snapshot = queue.snapshot();
    check(snapshot.byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 0,
          "disabled backfill scan does not enqueue waveform jobs", failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testScanForegroundCompletionLedgerCounts() {
    int failures = 0;
    const fs::path root = makeTempRoot("scan_foreground_completion_ledger");
    const fs::path dbPath = root / "scan_foreground_completion_ledger.db";

    Database db;
    check(db.open(dbPath.u8string(), true), "open foreground completion ledger DB", failures);

    AudioRecord a;
    a.filePath = (root / "a.wav").u8string();
    a.fileName = "a.wav";
    a.folderPath = root.u8string();
    a.extension = ".wav";
    a.fileSize = 44100;
    a.fileMtime = 100;
    check(db.addFile(a), "insert first scan row", failures);

    AudioRecord b = a;
    b.filePath = (root / "b.wav").u8string();
    b.fileName = "b.wav";
    b.fileMtime = 101;
    check(db.addFile(b), "insert second scan row", failures);

    const AudioRecord recA = db.getFileByPath(a.filePath);
    const AudioRecord recB = db.getFileByPath(b.filePath);
    const int64_t sessionId = 7001;
    const int64_t secondSessionId = 7002;
    check(db.resetScanForegroundCompletionSession(sessionId), "reset scan foreground session", failures);
    check(db.resetScanForegroundCompletionSession(secondSessionId), "reset second scan foreground session", failures);
    check(db.seedScanForegroundCompletionRows(sessionId, {recA.id, recB.id}, true, true, true),
          "seed two foreground completion rows", failures);
    check(db.seedScanForegroundCompletionRows(secondSessionId, {recA.id}, true, true, true),
          "seed second session with shared foreground file row", failures);

    ScanForegroundCompletionCounts counts = db.getScanForegroundCompletionCounts(sessionId);
    check(counts.total == 2, "ledger total starts at 2", failures);
    check(counts.miniUnresolved == 2, "mini unresolved starts at 2", failures);
    check(counts.coverUnresolved == 2, "cover unresolved starts at 2", failures);
    check(counts.metadataUnresolved == 2, "metadata unresolved starts at 2", failures);

    check(db.markScanForegroundMiniTerminal(sessionId, recA.id, ScanForegroundTerminalState::Cached, ""),
          "mark first mini cached", failures);
    check(db.markScanForegroundCoverTerminal(sessionId, recA.id, ScanForegroundTerminalState::Resolved, ""),
          "mark first cover resolved", failures);
    counts = db.getScanForegroundCompletionCounts(sessionId);
    check(counts.pendingFiles == 2 && counts.metadataUnresolved == 2,
          "foreground pending-files gate does not clear until hidden metadata is terminal",
          failures);
    check(db.markScanForegroundMetadataTerminal(sessionId, recA.id, ScanForegroundTerminalState::Resolved, ""),
          "mark first metadata resolved", failures);
    counts = db.getScanForegroundCompletionCounts(sessionId);
    check(counts.pendingFiles == 1,
          "one file is ready only after mini cover and metadata are terminal",
          failures);
    ScanForegroundCompletionCounts secondCounts = db.getScanForegroundCompletionCounts(secondSessionId);
    check(secondCounts.miniUnresolved == 1 &&
              secondCounts.coverUnresolved == 1 &&
              secondCounts.metadataUnresolved == 1,
          "session-scoped mark leaves shared file unresolved in the second session",
          failures);

    check(db.markScanForegroundMiniTerminal(sessionId, recB.id, ScanForegroundTerminalState::Failed, "decode_failed"),
          "mark second mini failed terminal", failures);
    check(db.markScanForegroundCoverTerminal(sessionId, recB.id, ScanForegroundTerminalState::NoSource, "no_cover"),
          "mark second cover no-source terminal", failures);
    check(db.markScanForegroundMetadataTerminal(sessionId, recB.id, ScanForegroundTerminalState::NoSource, "metadata_no_source"),
          "mark second metadata no-source terminal", failures);

    counts = db.getScanForegroundCompletionCounts(sessionId);
    check(counts.miniUnresolved == 0, "mini unresolved reaches zero", failures);
    check(counts.coverUnresolved == 0, "cover unresolved reaches zero", failures);
    check(counts.metadataUnresolved == 0, "metadata unresolved reaches zero", failures);
    check(counts.miniTerminal == 2, "mini terminal count reaches total", failures);
    check(counts.coverTerminal == 2, "cover terminal count reaches total", failures);
    check(counts.metadataTerminal == 2, "metadata terminal count reaches total", failures);
    check(counts.failureTerminal == 3, "failure/no-source terminal count is visible", failures);

    check(db.seedScanForegroundCompletionRows(sessionId, {recA.id, recB.id}, true, true, true),
          "reseeding foreground rows preserves terminal ledger states", failures);
    counts = db.getScanForegroundCompletionCounts(sessionId);
    check(counts.miniUnresolved == 0 &&
              counts.coverUnresolved == 0 &&
              counts.metadataUnresolved == 0,
          "reseeding already terminal rows does not reset unresolved counts",
          failures);
    check(counts.failureTerminal == 3,
          "reseeding already terminal rows does not erase failure/no-source classifications",
          failures);

    check(db.markScanForegroundMiniTerminal(sessionId, recA.id, ScanForegroundTerminalState::Failed, "late_decode_failed"),
          "repeated mini terminal mark is accepted as idempotent", failures);
    check(db.markScanForegroundCoverTerminal(sessionId, recA.id, ScanForegroundTerminalState::Failed, "late_cover_failed"),
          "repeated cover terminal mark is accepted as idempotent", failures);
    check(db.markScanForegroundMetadataTerminal(sessionId, recA.id, ScanForegroundTerminalState::Failed, "late_metadata_failed"),
          "repeated metadata terminal mark is accepted as idempotent", failures);
    counts = db.getScanForegroundCompletionCounts(sessionId);
    check(counts.failureTerminal == 3,
          "repeated terminal marks do not overwrite existing successful terminal states",
          failures);

    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testCancelledScanRollsBackCommittedRowsAndQueueDebt() {
    int failures = 0;
    const fs::path root = makeTempRoot("cancelled_scan_rollback");
    const fs::path dbPath = root / "cancelled_scan_rollback.db";
    writeSilentMonoWav(root / "stall_one.wav", 8000, 12000);
    writeSilentMonoWav(root / "stall_two.wav", 8000, 12000);
    writeSilentMonoWav(root / "stall_three.wav", 8000, 12000);

    Database db;
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open db: " << db.getLastError() << "\n";
        fs::remove_all(root);
        return 1;
    }

    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), 17);

    std::atomic<bool> cancelFlag{false};
    ScanStats stats;
    std::thread worker([&]() {
        stats = db.scanDirectoryEx(root.u8string(),
                                   true,
                                   false,
                                   false,
                                   nullptr,
                                   &cancelFlag,
                                   1,
                                   /*completeImport=*/false,
                                   /*enqueueExtraction=*/true,
                                   &queue,
                                   dbPath.u8string(),
                                   17);
    });

    bool sawPartialCommit = false;
    for (int attempt = 0; attempt < 80; ++attempt) {
        if (querySingleInt(dbPath.u8string(), "SELECT COUNT(*) FROM audio_files", -1) > 0) {
            sawPartialCommit = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    check(sawPartialCommit,
          "cancelled scan test observes a committed partial row before triggering rollback",
          failures);

    cancelFlag.store(true, std::memory_order_release);
    worker.join();

    check(stats.added == 0 && stats.updated == 0 && stats.removed == 0 && stats.ingested == 0,
          "cancelled scan reports zero committed row counts after rollback",
          failures);
    check(querySingleInt(db.db(), "SELECT COUNT(*) FROM audio_files", -1) == 0,
          "cancelled scan rollback removes partially imported audio rows",
          failures);
    check(querySingleInt(db.db(), "SELECT COUNT(*) FROM waveforms_db.waveforms", -1) == 0,
          "cancelled scan rollback removes partially imported waveform rows",
          failures);
    check(querySingleInt(db.db(), "SELECT COUNT(*) FROM backfill_pending", -1) == 0,
          "cancelled scan rollback removes durable backfill debt",
          failures);

    const auto snapshot = queue.snapshot();
    check(snapshot.byKind[static_cast<int>(aura::BackfillJobKind::CoverResolveAndThumb)] == 0 &&
              snapshot.byKind[static_cast<int>(aura::BackfillJobKind::WaveformL1Overview)] == 0 &&
              snapshot.byKind[static_cast<int>(aura::BackfillJobKind::MetadataRefresh)] == 0,
          "cancelled scan rollback clears queued runtime backfill jobs",
          failures);

    queue.shutdown();
    db.close();
    removeDbArtifacts(dbPath);
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

static int testUnchangedFullScanWithConcurrentCacheWrite(bool cached) {
    int failures = 0;
    const fs::path root = makeTempRoot(cached ? "rescan_cached_concurrent_write"
                                            : "rescan_missing_concurrent_write");
    const fs::path audioRoot = root / "audio";
    fs::create_directories(audioRoot);
    for (int index = 0; index < 51; ++index) {
        writeSilentMonoWav(audioRoot / ("voice-" + std::to_string(index) + ".wav"));
    }
    Database db;
    const fs::path dbPath = root / "library.db";
    if (!db.open(dbPath.u8string())) {
        std::cerr << "FAIL: open rescan fixture: " << db.getLastError() << '\n';
        return 1;
    }
    const auto initial = db.scanDirectoryEx(audioRoot.u8string(), true, false, false,
                                            nullptr, nullptr, 1, false, false);
    check(initial.added == 51, "seed previously scanned audio files", failures);
    const auto records = db.searchFiles(SearchFilter{}, 100, 0);
    for (const auto& record : records) {
        if (cached) {
            check(db.saveWaveformByPath(record.filePath,
                  std::vector<float>(AppState::WAVEFORM_MINI_PEAKS, 0.2f), -18.0f, 1),
                  "seed cached mini", failures);
        }
        check(db.upsertWaveformCacheStateByPath(record.filePath, cached ? "done" : "pending",
              "", 0, WaveformExtractor::BuildCacheKey(record.filePath, record.fileSize, record.fileMtime)),
              "seed existing waveform state", failures);
    }
    Database writer;
    if (!writer.open(dbPath.u8string())) {
        std::cerr << "FAIL: open concurrent writer: " << writer.getLastError() << '\n';
        return failures + 1;
    }
    aura::BackfillQueue queue;
    queue.onDbOpened(dbPath.u8string(), 31);
    constexpr int64_t session = 57;
    bool wrote = false;
    const auto stats = db.scanDirectoryEx(audioRoot.u8string(), true, false, false,
        [&](int current, int, const std::string&, bool committed) {
            if (current != 50 || committed || wrote) return;
            // A real second connection commits between scan reads and the next
            // completion write, just like the visible waveform worker.
            const int rc = sqlite3_exec(writer.db(),
                "UPDATE waveforms_db.waveforms SET updated_at = updated_at + 1;"
                "INSERT OR REPLACE INTO app_meta(key, value) VALUES('concurrent_scan_test', '1');",
                nullptr, nullptr, nullptr);
            check(rc == SQLITE_OK, "concurrent cache writer commits", failures);
            wrote = rc == SQLITE_OK;
        }, nullptr, 1, false, true, &queue, dbPath.u8string(), 31, session);
    check(wrote, "rescan exercised concurrent cache update", failures);
    for (const auto& error : stats.errorMessages) std::cerr << error << '\n';
    check(stats.errors == 0, "unchanged rescan accepts completion writes after concurrent update", failures);
    const auto counts = db.getScanForegroundCompletionCounts(session);
    check(counts.total == 51, "all unchanged files enter the completion ledger", failures);
    check(cached ? counts.miniTerminal == 51 : counts.miniUnresolved == 51,
          "cache reuse or missing mini work is recorded for every file", failures);
    queue.shutdown();
    writer.close();
    db.close();
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cout << "=== Scan/Waveform Cache Contract Tests ===\n";
    if (argc == 2 && std::string(argv[1]) == "--concurrent-rescan") {
        return testUnchangedFullScanWithConcurrentCacheWrite(false) +
               testUnchangedFullScanWithConcurrentCacheWrite(true) == 0 ? 0 : 1;
    }

    int failures = 0;
    std::cout << "-- testWpk2PeakEnvelopeCodecRoundTripsStereo\n";
    failures += testWpk2PeakEnvelopeCodecRoundTripsStereo();
    std::cout << "-- testWpk2PeakEnvelopeCodecRejectsMalformedAndLegacyCollision\n";
    failures += testWpk2PeakEnvelopeCodecRejectsMalformedAndLegacyCollision();
    std::cout << "-- testWpk2PeakEnvelopeCodecPreservesQuietTail\n";
    failures += testWpk2PeakEnvelopeCodecPreservesQuietTail();
    std::cout << "-- testScanPrecomputeWritesCompleteWaveformCache\n";
    failures += testScanPrecomputeWritesCompleteWaveformCache();
    std::cout << "-- testCompleteImportSkipsPersistentDetailForOverFiveMinutes\n";
    failures += testCompleteImportSkipsPersistentDetailForOverFiveMinutes();
    std::cout << "-- testScanStatsReportIngestSummary\n";
    failures += testScanStatsReportIngestSummary();
    std::cout << "-- testMultiFolderScanProgressAggregatesLocalCallbacks\n";
    failures += testMultiFolderScanProgressAggregatesLocalCallbacks();
    std::cout << "-- testCompleteWaveformExtractionReturnsAllScanArtifacts\n";
    failures += testCompleteWaveformExtractionReturnsAllScanArtifacts();
    std::cout << "-- testWaveformExtractionUnicodePathRejectsEmbeddedNul\n";
    failures += testWaveformExtractionUnicodePathRejectsEmbeddedNul();
    std::cout << "-- testFullImportRescanKeepsMissingDetailEligible\n";
    failures += testFullImportRescanKeepsMissingDetailEligible();
    std::cout << "-- testFullImportRescanRepairsDoneRowWithMissingMiniBlob\n";
    failures += testFullImportRescanRepairsDoneRowWithMissingMiniBlob();
    std::cout << "-- testCompleteImportUsesBackgroundFolderCoverDiscovery\n";
    failures += testCompleteImportUsesBackgroundFolderCoverDiscovery();
    std::cout << "-- testCompleteImportWritesFolderCoverThumb\n";
    failures += testCompleteImportWritesFolderCoverThumb();
    std::cout << "-- testFastImportDefersCoverAndWaveformCaches\n";
    failures += testFastImportDefersCoverAndWaveformCaches();
    std::cout << "-- testWaveformPurgeApis\n";
    failures += testWaveformPurgeApis();
    std::cout << "-- testCompleteDetailCacheDoesNotRequireRms\n";
    failures += testCompleteDetailCacheDoesNotRequireRms();
    std::cout << "-- testStereoDetailCacheDoesNotRequireRms\n";
    failures += testStereoDetailCacheDoesNotRequireRms();
    std::cout << "-- testWpk2DetailCacheReadsAndDoesNotRebuild\n";
    failures += testWpk2DetailCacheReadsAndDoesNotRebuild();
    std::cout << "-- testMalformedWpk2ChannelMetadataReentersRebuild\n";
    failures += testMalformedWpk2ChannelMetadataReentersRebuild();
    std::cout << "-- testWpk2DetailWriterDefaultsOnAndCanBeDisabled\n";
    failures += testWpk2DetailWriterDefaultsOnAndCanBeDisabled();
    std::cout << "-- testIncompleteDetailCacheStillRebuildsWithoutRms\n";
    failures += testIncompleteDetailCacheStillRebuildsWithoutRms();
    std::cout << "-- testMiniWaveformDerivesFromDetailWhenMiniCacheMissing\n";
    failures += testMiniWaveformDerivesFromDetailWhenMiniCacheMissing();
    std::cout << "-- testMiniWaveformDerivesFromMaxDetailWhenMinMissing\n";
    failures += testMiniWaveformDerivesFromMaxDetailWhenMinMissing();
    std::cout << "-- testLongWavMiniPreservesNonCenterTransient\n";
    failures += testLongWavMiniPreservesNonCenterTransient();
    std::cout << "-- testWaveformRebuildTaskCursorAndBatchSave\n";
    failures += testWaveformRebuildTaskCursorAndBatchSave();
    std::cout << "-- testIncrementalWaveformRebuildPersistsPendingDetail\n";
    failures += testIncrementalWaveformRebuildPersistsPendingDetail();
    std::cout << "-- testWaveformRebuildCountsMissingMiniAndDetailOnce\n";
    failures += testWaveformRebuildCountsMissingMiniAndDetailOnce();
    std::cout << "-- testFillMissingWaveformSkipsFailedMiniDebt\n";
    failures += testFillMissingWaveformSkipsFailedMiniDebt();
    std::cout << "-- testWaveformRebuildIgnoresLegacyMainMiniWhenSidecarMissing\n";
    failures += testWaveformRebuildIgnoresLegacyMainMiniWhenSidecarMissing();
    std::cout << "-- testWaveformRebuildExactTotalsIgnoreFolderIndexCache\n";
    failures += testWaveformRebuildExactTotalsIgnoreFolderIndexCache();
    std::cout << "-- testWaveformCacheVersionMigrationKeepsRmsData\n";
    failures += testWaveformCacheVersionMigrationKeepsRmsData();
    std::cout << "-- testRemoveMissingDeletesWaveformRows\n";
    failures += testRemoveMissingDeletesWaveformRows();
    std::cout << "-- testRemoveFolderSubtreeFromDbKeepsDiskFilesByDefault\n";
    failures += testRemoveFolderSubtreeFromDbKeepsDiskFilesByDefault();
    std::cout << "-- testRemoveDriveRootSubtreeCleansHiddenIndexedRows\n";
    failures += testRemoveDriveRootSubtreeCleansHiddenIndexedRows();
    std::cout << "-- testRemoveFolderSubtreeMatchesLegacyCaseFolderPaths\n";
    failures += testRemoveFolderSubtreeMatchesLegacyCaseFolderPaths();
    std::cout << "-- testRemoveFolderAncestorSubtreeMatchesLegacyCaseDescendants\n";
    failures += testRemoveFolderAncestorSubtreeMatchesLegacyCaseDescendants();
    std::cout << "-- testRemoveFolderSubtreeSkipsMissingLegacyBackfillTable\n";
    failures += testRemoveFolderSubtreeSkipsMissingLegacyBackfillTable();
    std::cout << "-- testRemoveFolderSubtreeLeavesPackageCompactable\n";
    failures += testRemoveFolderSubtreeLeavesPackageCompactable();
    std::cout << "-- testRemoveFolderSubtreeRecoversMalformedFtsIndexes\n";
    failures += testRemoveFolderSubtreeRecoversMalformedFtsIndexes();
    std::cout << "-- testFolderHierarchyFromIndexRepairsStaleIndex\n";
    failures += testFolderHierarchyFromIndexRepairsStaleIndex();
    std::cout << "-- testClearWaveformCacheByFolderUsesCanonicalFolderPath\n";
    failures += testClearWaveformCacheByFolderUsesCanonicalFolderPath();
    std::cout << "-- testExternalWaveformCacheStillWorksOutsideLibraryRoots\n";
    failures += testExternalWaveformCacheStillWorksOutsideLibraryRoots();
    std::cout << "-- testRemoveFolderSubtreeFromDbCanDeleteDiskFiles\n";
    failures += testRemoveFolderSubtreeFromDbCanDeleteDiskFiles();
    std::cout << "-- testRemoveSingleFileInvalidatesCountAndFolderIndex\n";
    failures += testRemoveSingleFileInvalidatesCountAndFolderIndex();
    std::cout << "-- testScanWriteInfrastructureFailureCountsDiscoveredFiles\n";
    failures += testScanWriteInfrastructureFailureCountsDiscoveredFiles();
    std::cout << "-- testScanBatchRetriesWhenDatabaseIsTemporarilyLocked\n";
    failures += testScanBatchRetriesWhenDatabaseIsTemporarilyLocked();
    std::cout << "-- testScanBatchFailsFastWhenDatabaseLockPersists\n";
    failures += testScanBatchFailsFastWhenDatabaseLockPersists();
    std::cout << "-- testSlowScanWorkerDoesNotDropCompletedResultAfterStallWarning\n";
    failures += testSlowScanWorkerDoesNotDropCompletedResultAfterStallWarning();
    std::cout << "-- testFastScanQueuesWaveformBackfillWhenRequested\n";
    failures += testFastScanQueuesWaveformBackfillWhenRequested();
    std::cout << "-- testFastScanWaveformBackfillPrioritizesShortFiles\n";
    failures += testFastScanWaveformBackfillPrioritizesShortFiles();
    std::cout << "-- testFastScanPendingPreservesExistingWaveformBlobs\n";
    failures += testFastScanPendingPreservesExistingWaveformBlobs();
    std::cout << "-- testFastScanReusesMiniCacheWithoutMainDetailInHighSpeedMode\n";
    failures += testFastScanReusesMiniCacheWithoutMainDetailInHighSpeedMode();
    std::cout << "-- testFastScanRescanRequeuesExistingPendingMiniCacheDebt\n";
    failures += testFastScanRescanRequeuesExistingPendingMiniCacheDebt();
    std::cout << "-- testFastScanRescanIgnoresLegacyMainMiniWhenSidecarMissing\n";
    failures += testFastScanRescanIgnoresLegacyMainMiniWhenSidecarMissing();
    std::cout << "-- testFastScanMarksDurableWaveformBackfillWhenQueueSnapshotUnavailable\n";
    failures += testFastScanMarksDurableWaveformBackfillWhenQueueSnapshotUnavailable();
    std::cout << "-- testCompleteImportStillRequiresMainDetailForHighSpeedMode\n";
    failures += testCompleteImportStillRequiresMainDetailForHighSpeedMode();
    std::cout << "-- testFastScanDefersMetadataToBackfillWorker\n";
    failures += testFastScanDefersMetadataToBackfillWorker();
    std::cout << "-- testFastScanDoesNotQueueWaveformBackfillWhenDisabled\n";
    failures += testFastScanDoesNotQueueWaveformBackfillWhenDisabled();
    std::cout << "-- testScanForegroundCompletionLedgerCounts\n";
    failures += testScanForegroundCompletionLedgerCounts();
    std::cout << "-- testUnchangedFullScanWithConcurrentCacheWrite\n";
    failures += testUnchangedFullScanWithConcurrentCacheWrite(false);
    failures += testUnchangedFullScanWithConcurrentCacheWrite(true);
    std::cout << "-- testCancelledScanRollsBackCommittedRowsAndQueueDebt\n";
    failures += testCancelledScanRollsBackCommittedRowsAndQueueDebt();
    std::cout << "-- testCoverWallIgnoresStaleOrphanRegistryRows\n";
    failures += testCoverWallIgnoresStaleOrphanRegistryRows();

    if (failures != 0) {
        std::cerr << "FAIL: " << failures << " assertion(s)\n";
        return 1;
    }

    std::cout << "PASS\n";
    return 0;
}
