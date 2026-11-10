#include "Database.h"
#include "SimilarityQueryService.h"
#include "platform/harken_user_data_layout.h"
#include "sqlite3.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
int failures = 0;
void Check(bool ok, const char* text) {
    if (ok) std::cout << "PASS: " << text << '\n';
    else { std::cerr << "FAIL: " << text << '\n'; ++failures; }
}
std::string ReadTextFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}
void CheckDiagnosticsSourceContract() {
    const fs::path sourcePath =
        fs::path(__FILE__).parent_path().parent_path() /
        "src" / "SimilarityQueryService.cpp";
    const std::string source = ReadTextFile(sourcePath);

    Check(!source.empty(),
          "diagnostics contract can read SimilarityQueryService source");
    Check(source.find("#include \"DiagnosticsLogger.h\"") != std::string::npos &&
              source.find("DIAG_ACTION_FMT(") != std::string::npos,
          "diagnostics contract uses the existing diagnostics action macro");
    Check(source.find(
              "request_start gen=%llu ref=%lld preset=%d") !=
              std::string::npos,
          "diagnostics contract keeps the request_start event schema stable");
    Check(source.find(
              "request_done gen=%llu candidates=%d rows=%d "
              "cache_ms=%.2f query_ms=%.2f") != std::string::npos,
          "diagnostics contract keeps the request_done event schema stable");
    Check(source.find("request_failed gen=%llu ref=%lld") !=
                  std::string::npos &&
              source.find("candidates=%d") != std::string::npos &&
              source.find("cache_ms=%.2f") != std::string::npos &&
              source.find("query_ms=%.2f") != std::string::npos &&
              source.find("rows=%d") != std::string::npos &&
              source.find("error=%d") != std::string::npos,
          "diagnostics contract keeps request_failed identity, timing, row, "
          "candidate, and error fields stable");
}
AudioFeatureVector Feature(float value) {
    AudioFeatureVector out;
    for (float& component : out.features) component = value;
    out.features[AudioFeatureVector::PITCH_OFFSET + 2] = 0.5f;
    return out;
}
void WritePcmWav(const fs::path& path) {
    constexpr uint32_t sampleRate = 22050;
    constexpr uint16_t channels = 1;
    constexpr uint16_t bitsPerSample = 16;
    std::vector<int16_t> samples(4096);
    constexpr double twoPi = 6.28318530717958647692;
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<int16_t>(
            std::sin(twoPi * 440.0 * static_cast<double>(i) / sampleRate) *
            12000.0);
    }
    const uint32_t dataBytes =
        static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t riffSize = 36 + dataBytes;
    const uint32_t fmtSize = 16;
    const uint16_t audioFormat = 1;
    const uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmtSize), 4);
    out.write(reinterpret_cast<const char*>(&audioFormat), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    out.write(reinterpret_cast<const char*>(&sampleRate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), 4);
    out.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
}
void ExecuteSql(Database& db, const char* sql, const char* message) {
    char* error = nullptr;
    const int result = sqlite3_exec(db.db(), sql, nullptr, nullptr, &error);
    if (error) sqlite3_free(error);
    Check(result == SQLITE_OK, message);
}
void RemoveDatabaseFamily(const std::string& path) {
    (void)std::remove(path.c_str());
    (void)std::remove((path + "-wal").c_str());
    (void)std::remove((path + "-shm").c_str());
    std::string base = path;
    const size_t dot = base.rfind('.');
    if (dot != std::string::npos) base.resize(dot);
    for (const char* suffix : {"_blobs.db", "_waveforms.db"}) {
        const std::string sidecar = base + suffix;
        (void)std::remove(sidecar.c_str());
        (void)std::remove((sidecar + "-wal").c_str());
        (void)std::remove((sidecar + "-shm").c_str());
    }
}
AudioRecord AddRecord(Database& db, const std::string& path,
                      float duration, const char* format = "WAV") {
    AudioRecord record;
    record.filePath = path;
    record.fileName = fs::path(path).filename().u8string();
    record.displayName = record.fileName;
    record.extension = ".wav";
    record.format = format;
    record.durationSeconds = duration;
    record.channels = 1;
    record.sampleRate = 22050;
    record.bitDepth = 16;
    record.totalFrames = static_cast<uint64_t>(duration * record.sampleRate);
    record.fileSize = record.totalFrames * sizeof(int16_t);
    record.folderPath = fs::path(path).parent_path().u8string();
    Check(db.addFile(record), "fixture row is inserted");
    return db.getFileByPath(path);
}
HarkenSearch::SimilarityRequest RequestFor(const std::string& dbPath,
                                   const AudioRecord& reference,
                                   uint64_t generation, int topK = 50) {
    HarkenSearch::SimilarityRequest request;
    request.generation = generation;
    request.databasePath = dbPath;
    request.reference = {reference.id, reference.filePath, reference.fileName};
    request.topK = topK;
    return request;
}
void Run(const fs::path& root) {
    CheckDiagnosticsSourceContract();

    const std::string firstPath = (root / "first.db").u8string();
    const std::string secondPath = (root / "second.db").u8string();
    const std::string emptyPath = (root / "empty.db").u8string();
    const std::string invalidReferencePath =
        (root / "invalid-reference.db").u8string();
    const std::string invalidGlobalsPath =
        (root / "invalid-globals.db").u8string();
    const std::string corruptCachePath =
        (root / "corrupt-cache.db").u8string();
    const std::string sabotagedCachePath =
        (root / "sabotaged-cache.db").u8string();
    Database first;
    Check(first.open(firstPath, true), "first fixture database opens");
    int indexedCount = -1, totalCount = -1;
    Check(first.tryGetFeatureIndexCounts(indexedCount, totalCount) &&
              indexedCount == 0 && totalCount == 0, "empty index counts are 0/0");
    const AudioRecord a = AddRecord(first, "A.wav", 1.0f);
    const AudioRecord b = AddRecord(first, "B.wav", 1.2f);
    const AudioRecord c = AddRecord(first, "C.wav", 1.4f, "AIFF");
    Check(first.saveAudioFeaturesByPath(a.filePath, Feature(0.0f)), "reference feature saves");
    Check(first.tryGetFeatureIndexCounts(indexedCount, totalCount) &&
              indexedCount == 1 && totalCount == 3,
          "partial index uses actual audio rows, not cached folder totals");
    Check(first.saveAudioFeaturesByPath(b.filePath, Feature(0.1f)), "near feature saves");
    Check(first.saveAudioFeaturesByPath(c.filePath, Feature(2.0f)), "far feature saves");
    Check(first.tryGetFeatureIndexCounts(indexedCount, totalCount) &&
              indexedCount == 3 && totalCount == 3, "completed index counts are 3/3");
    Database unopened;
    indexedCount = 17;
    totalCount = 19;
    Check(!unopened.tryGetFeatureIndexCounts(indexedCount, totalCount) &&
              indexedCount == 17 && totalCount == 19,
          "failed index count does not publish false empty/completed state");

    SimilaritySearch cache;
    {
        HarkenSearch::SimilarityQueryService service(cache);
        const auto result = service.Execute(RequestFor(firstPath, a, 7, 2), [] { return false; });
        Check(result.generation == 7 && result.reference == HarkenSearch::SimilarityReference{a.id, a.filePath, a.fileName},
              "completion preserves request identity");
        Check(result.error == HarkenSearch::SimilarityWorkflowError::None && result.rows.size() == 2,
              "BDD: indexed candidates return lightweight window rows");
        Check(result.rows.size() == 2 && result.rows[0].fileId == b.id && result.rows[1].fileId == c.id,
              "BDD: display rows preserve acoustic order");
        Check(result.rows.size() == 2 && result.rows[0].filePath == b.filePath &&
                  result.rows[0].fileName == b.fileName &&
                  result.rows[0].durationSeconds == b.durationSeconds && result.rows[0].format == b.format,
              "display rows contain required file metadata");

        const AudioRecord d = AddRecord(first, "D.wav", 1.1f);
        Check(first.saveAudioFeaturesByPath(d.filePath, Feature(0.05f)), "new feature saves after first query");
        const auto refreshed = service.Execute(RequestFor(firstPath, a, 8, 3), [] { return false; });
        Check(refreshed.error == HarkenSearch::SimilarityWorkflowError::None && refreshed.rows.size() == 3 &&
                  refreshed.rows[0].fileId == d.id,
              "feature-count changes refresh ranking cache");

        const AudioRecord referenceAlias = AddRecord(first, "a.wav", 1.0f);
        const AudioRecord candidateAlias = AddRecord(first, "b.wav", 1.2f);
        const AudioRecord sameNameOtherDirectory =
            AddRecord(first, "Other/B.wav", 1.25f);
        Check(first.saveAudioFeaturesByPath(referenceAlias.filePath, Feature(0.0f)),
              "reference path alias feature saves");
        Check(first.saveAudioFeaturesByPath(candidateAlias.filePath, Feature(0.1f)),
              "candidate path alias feature saves");
        Check(first.saveAudioFeaturesByPath(
                  sameNameOtherDirectory.filePath, Feature(0.15f)),
              "different-directory same-name feature saves");
        const auto pathIdentityResult = service.Execute(
            RequestFor(firstPath, referenceAlias, 19, 10), [] { return false; });
        int rootCandidateCount = 0;
        bool containsExactReference = false;
        bool containsCaseVariantReference = false;
        bool containsOtherDirectorySameName = false;
        for (const auto& row : pathIdentityResult.rows) {
            containsExactReference |= row.filePath == referenceAlias.filePath;
            containsCaseVariantReference |= row.filePath == a.filePath;
            rootCandidateCount +=
                row.filePath == b.filePath || row.filePath == candidateAlias.filePath;
            containsOtherDirectorySameName |=
                row.filePath == sameNameOtherDirectory.filePath;
        }
#ifdef _WIN32
        const bool pathIdentityMatchesPlatform =
            !containsCaseVariantReference && rootCandidateCount == 1;
#else
        // POSIX paths keep case: these are distinct database identities even
        // when a particular volume happens to be case-insensitive.
        const bool pathIdentityMatchesPlatform =
            containsCaseVariantReference && rootCandidateCount == 2;
#endif
        Check(pathIdentityResult.error == HarkenSearch::SimilarityWorkflowError::None &&
                  !containsExactReference && pathIdentityMatchesPlatform &&
                  containsOtherDirectorySameName,
              "query service excludes the reference, applies platform path "
              "identity, and preserves same names in different directories");

        const AudioRecord missing = AddRecord(first, (root / "missing-reference.wav").u8string(), 0.5f);
        const auto unreadable = service.Execute(RequestFor(firstPath, missing, 9, 2), [] { return false; });
        Check(unreadable.error == HarkenSearch::SimilarityWorkflowError::ReferenceUnreadable && unreadable.rows.empty(),
              "missing reference DecodeFailed maps to ReferenceUnreadable");
        const auto cancelled = service.Execute(RequestFor(firstPath, a, 10, 2), [] { return true; });
        Check(cancelled.error == HarkenSearch::SimilarityWorkflowError::Cancelled && cancelled.rows.empty(),
              "pre-cancelled request publishes no rows");

        const fs::path missingParent = root / "must-not-be-created";
        const std::string missingDatabase =
            (missingParent / "unavailable.db").u8string();
        const auto missingPathResult = service.Execute(
            RequestFor(missingDatabase, a, 11, 2), [] { return false; });
        Check(missingPathResult.error ==
                  HarkenSearch::SimilarityWorkflowError::DatabaseUnavailable,
              "nonexistent database path reports DatabaseUnavailable");
        Check(!fs::exists(missingParent) && !fs::exists(missingDatabase),
              "query service never creates a directory or empty database");

        const auto unavailable = service.Execute(
            RequestFor(root.u8string(), a, 11, 2),
            [] { return false; });
        Check(unavailable.error == HarkenSearch::SimilarityWorkflowError::DatabaseUnavailable && unavailable.rows.empty(),
              "open failure maps to DatabaseUnavailable");

        Database second;
        Check(second.open(secondPath, true), "second fixture database opens");
        const AudioRecord x = AddRecord(second, "X.wav", 2.0f);
        const AudioRecord y = AddRecord(second, "Y.wav", 2.1f);
        Check(second.saveAudioFeaturesByPath(x.filePath, Feature(5.0f)), "second reference feature saves");
        Check(second.saveAudioFeaturesByPath(y.filePath, Feature(5.1f)), "second candidate feature saves");
        const auto switched = service.Execute(RequestFor(secondPath, x, 12), [] { return false; });
        Check(switched.error == HarkenSearch::SimilarityWorkflowError::None && switched.rows.size() == 1 &&
                  switched.rows[0].fileName == y.fileName,
              "database path switch replaces connection and cache");

        const fs::path validAudio = root / "recover-reference.wav";
        WritePcmWav(validAudio);
        Database invalidReference;
        Check(invalidReference.open(invalidReferencePath, true),
              "invalid-reference fixture database opens");
        const AudioRecord invalidRef =
            AddRecord(invalidReference, validAudio.u8string(), 0.2f);
        const AudioRecord invalidRefCandidate =
            AddRecord(invalidReference, "RecoveredCandidate.wav", 0.3f);
        AudioFeatureVector nonFiniteReference = Feature(0.0f);
        nonFiniteReference.features[0] =
            std::numeric_limits<float>::quiet_NaN();
        Check(invalidReference.saveAudioFeaturesByPath(
                  invalidRef.filePath, nonFiniteReference),
              "non-finite reference fixture feature saves");
        Check(invalidReference.saveAudioFeaturesByPath(
                  invalidRefCandidate.filePath, Feature(0.2f)),
              "recovery candidate feature saves");
        const auto recoveredReference = service.Execute(
            RequestFor(invalidReferencePath, invalidRef, 13, 2),
            [] { return false; });
        AudioFeatureVector persistedReference;
        Check(recoveredReference.error == HarkenSearch::SimilarityWorkflowError::None &&
                  recoveredReference.rows.size() == 1 &&
                  recoveredReference.rows[0].fileId == invalidRefCandidate.id,
              "non-finite stored reference is re-extracted before ranking");
        Check(invalidReference.getAudioFeaturesById(
                  invalidRef.id, persistedReference) &&
                  persistedReference.isValid(),
              "re-extracted finite reference replaces invalid storage");

        Database invalidGlobals;
        Check(invalidGlobals.open(invalidGlobalsPath, true),
              "invalid-global fixture database opens");
        const AudioRecord invalidGlobalRef =
            AddRecord(invalidGlobals, validAudio.u8string(), 0.2f);
        const AudioRecord invalidGlobalCandidate =
            AddRecord(invalidGlobals, "GlobalCandidate.wav", 0.3f);
        Check(invalidGlobals.saveAudioFeaturesByPath(
                  invalidGlobalCandidate.filePath, Feature(0.2f)),
              "invalid-global candidate feature saves");
        const float infinity = std::numeric_limits<float>::infinity();
        Check(invalidGlobals.saveLoudnessByPath(
                  invalidGlobalRef.filePath, infinity, -70.0f, -70.0f,
                  infinity),
              "non-finite loudness fixture saves");
        const auto invalidGlobalResult = service.Execute(
            RequestFor(invalidGlobalsPath, invalidGlobalRef, 14, 2),
            [] { return false; });
        AudioFeatureVector forbiddenPersist;
        Check(invalidGlobalResult.error ==
                  HarkenSearch::SimilarityWorkflowError::ReferenceUnreadable,
              "non-finite post-fill reference reports ReferenceUnreadable");
        Check(!invalidGlobals.getAudioFeaturesById(
                  invalidGlobalRef.id, forbiddenPersist),
              "non-finite post-fill reference is never persisted");

        Database corruptCache;
        Check(corruptCache.open(corruptCachePath, true),
              "corrupt-cache fixture database opens");
        const AudioRecord cacheRef =
            AddRecord(corruptCache, "CacheReference.wav", 1.0f);
        const AudioRecord cacheNear =
            AddRecord(corruptCache, "CacheNear.wav", 1.1f);
        const AudioRecord malformed =
            AddRecord(corruptCache, "Malformed.wav", 1.2f);
        const AudioRecord nanCandidate =
            AddRecord(corruptCache, "NanCandidate.wav", 1.3f);
        Check(corruptCache.saveAudioFeaturesByPath(
                  cacheRef.filePath, Feature(0.0f)),
              "cache reference feature saves");
        Check(corruptCache.saveAudioFeaturesByPath(
                  cacheNear.filePath, Feature(0.1f)),
              "cache near feature saves");
        Check(corruptCache.saveAudioFeaturesByPath(
                  malformed.filePath, Feature(0.2f)),
              "malformed placeholder feature saves");
        AudioFeatureVector nonFiniteCandidate = Feature(0.3f);
        nonFiniteCandidate.features[1] =
            std::numeric_limits<float>::infinity();
        Check(corruptCache.saveAudioFeaturesByPath(
                  nanCandidate.filePath, nonFiniteCandidate),
              "non-finite cache candidate saves");
        ExecuteSql(corruptCache,
                   "UPDATE audio_files SET audio_features=X'00', "
                   "features_version=1 WHERE file_path='Malformed.wav'",
                   "malformed feature blob is installed");
        int corruptFeatureCount = -1;
        std::vector<Database::AudioFeatureEntry> corruptEntries(1);
        corruptEntries[0].fileId = 999;
        Check(corruptCache.tryCountFilesWithFeatures(corruptFeatureCount) &&
                  corruptFeatureCount == 4 &&
                  !corruptCache.tryLoadAllAudioFeatures(corruptEntries) &&
                  corruptEntries.size() == 1 &&
                  corruptEntries[0].fileId == 999,
              "fixture exposes count/load failure without partial output");
        const std::vector<Database::AudioFeatureEntry> legacyEntries =
            corruptCache.loadAllAudioFeatures();
        bool legacyHasReference = false;
        bool legacyHasNear = false;
        bool legacyHasNanCandidate = false;
        for (const Database::AudioFeatureEntry& entry : legacyEntries) {
            legacyHasReference |= entry.fileId == cacheRef.id;
            legacyHasNear |= entry.fileId == cacheNear.id;
            legacyHasNanCandidate |= entry.fileId == nanCandidate.id;
        }
        Check(legacyEntries.size() == 3 && legacyHasReference &&
                  legacyHasNear && legacyHasNanCandidate,
              "legacy feature load skips only malformed blobs");
        const auto malformedLoad = service.Execute(
            RequestFor(corruptCachePath, cacheRef, 15, 3),
            [] { return false; });
        Check(malformedLoad.error == HarkenSearch::SimilarityWorkflowError::SearchFailed,
              "count/load mismatch fails without replacing shared cache");

        Check(corruptCache.saveAudioFeaturesByPath(
                  malformed.filePath, Feature(0.2f)),
              "malformed feature blob is repaired");
        const auto nonFiniteLoad = service.Execute(
            RequestFor(corruptCachePath, cacheRef, 16, 3),
            [] { return false; });
        Check(nonFiniteLoad.error == HarkenSearch::SimilarityWorkflowError::SearchFailed,
              "full cache refresh rejects non-finite entries");

        Check(corruptCache.saveAudioFeaturesByPath(
                  nanCandidate.filePath, Feature(0.3f)),
              "non-finite cache candidate is repaired");
        const auto repairedLoad = service.Execute(
            RequestFor(corruptCachePath, cacheRef, 17, 3),
            [] { return false; });
        Check(repairedLoad.error == HarkenSearch::SimilarityWorkflowError::None &&
                  repairedLoad.rows.size() == 3,
              "failed cache refresh does not suppress same-count retry");

        Database sabotagedCache;
        Check(sabotagedCache.open(sabotagedCachePath, true),
              "sabotaged-cache fixture database opens");
        const AudioRecord sabotageRef =
            AddRecord(sabotagedCache, "SabotageReference.wav", 1.0f);
        const AudioRecord sabotageCandidate =
            AddRecord(sabotagedCache, "SabotageCandidate.wav", 1.1f);
        Check(sabotagedCache.saveAudioFeaturesByPath(
                  sabotageRef.filePath, Feature(0.0f)),
              "sabotage reference feature saves");
        Check(sabotagedCache.saveAudioFeaturesByPath(
                  sabotageCandidate.filePath, Feature(0.1f)),
              "sabotage candidate feature saves");

        int cancellationChecks = 0;
        bool tableDroppedAfterReferenceRead = false;
        auto sabotageAfterReferenceRead = [&] {
            ++cancellationChecks;
            if (cancellationChecks == 4) {
                char* error = nullptr;
                const int result = sqlite3_exec(
                    sabotagedCache.db(), "DROP TABLE audio_files",
                    nullptr, nullptr, &error);
                if (error) sqlite3_free(error);
                tableDroppedAfterReferenceRead = result == SQLITE_OK;
            }
            return false;
        };
        const auto sabotagedResult = service.Execute(
            RequestFor(sabotagedCachePath, sabotageRef, 18, 2),
            sabotageAfterReferenceRead);
        Check(tableDroppedAfterReferenceRead,
              "fixture drops audio_files after reference feature read");
        Check(sabotagedResult.error ==
                  HarkenSearch::SimilarityWorkflowError::SearchFailed,
              "checked cache read failure maps to SearchFailed");

        int untouchedCount = 123;
        std::vector<Database::AudioFeatureEntry> untouchedEntries(1);
        untouchedEntries[0].fileId = 456;
        Check(!sabotagedCache.tryCountFilesWithFeatures(untouchedCount) &&
                  untouchedCount == 123,
              "checked feature count reports missing table without publishing");
        Check(!sabotagedCache.tryLoadAllAudioFeatures(untouchedEntries) &&
                  untouchedEntries.size() == 1 &&
                  untouchedEntries[0].fileId == 456,
              "checked feature load reports missing table without partial output");

        sabotagedCache.close();
        corruptCache.close();
        invalidGlobals.close();
        invalidReference.close();

        Database empty;
        Check(empty.open(emptyPath, true), "single-row fixture database opens");
        const AudioRecord only = AddRecord(empty, "Only.wav", 0.8f);
        Check(empty.saveAudioFeaturesByPath(only.filePath, Feature(1.0f)), "single reference feature saves");
        const auto noCandidates = service.Execute(RequestFor(emptyPath, only, 13), [] { return false; });
        Check(noCandidates.error == HarkenSearch::SimilarityWorkflowError::NoCandidates && noCandidates.rows.empty(),
              "single-row index reports NoCandidates");
        empty.close();
        second.close();
    }
    first.close();
    RemoveDatabaseFamily(firstPath);
    RemoveDatabaseFamily(secondPath);
    RemoveDatabaseFamily(emptyPath);
    RemoveDatabaseFamily(invalidReferencePath);
    RemoveDatabaseFamily(invalidGlobalsPath);
    RemoveDatabaseFamily(corruptCachePath);
    RemoveDatabaseFamily(sabotagedCachePath);
}
} // namespace
int main() {
    const std::string unique = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const fs::path temporaryBase = Platform::HarkenUserData::PreviewIsolationActive()
        ? fs::u8path(Platform::HarkenUserData::PreviewIsolationRootUtf8()) / "Testing"
        : fs::temp_directory_path();
    const fs::path root = temporaryBase / ("harken_similarity_query_service_" + unique);
    std::error_code ec;
    fs::create_directories(root, ec);
    Check(!ec, "temporary fixture directory is created");
    Run(root);
    fs::remove_all(root, ec);
    if (failures == 0) std::cout << "PASS: similarity query service integration contract\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
