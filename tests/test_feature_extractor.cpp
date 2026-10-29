#include "FeatureExtractor.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
int g_failures = 0;
void Check(bool condition, const char* message) {
    if (condition) std::cout << "PASS: " << message << '\n';
    else { std::cerr << "FAIL: " << message << '\n'; ++g_failures; }
}

void WritePcmWav(const fs::path& path, const std::vector<int16_t>& samples) {
    constexpr uint32_t sampleRate = 22050;
    constexpr uint16_t channels = 1, bitsPerSample = 16;
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t riffSize = 36 + dataBytes, fmtSize = 16;
    const uint16_t audioFormat = 1;
    const uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4); out.write(reinterpret_cast<const char*>(&riffSize), 4);
    out.write("WAVE", 4); out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmtSize), 4);
    out.write(reinterpret_cast<const char*>(&audioFormat), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    out.write(reinterpret_cast<const char*>(&sampleRate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    out.write("data", 4); out.write(reinterpret_cast<const char*>(&dataBytes), 4);
    if (!samples.empty()) out.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
}

std::vector<int16_t> MakeToneSamples() {
    std::vector<int16_t> samples(4096);
    constexpr double twoPi = 6.28318530717958647692;
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(
            std::sin(twoPi * 440.0 * i / 22050.0) * 12000.0);
    return samples;
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

void TestMissingFileReportsDecodeFailure(const fs::path& root) {
    const auto result = FeatureExtractor::extractFromFileChecked((root / "missing.wav").u8string());
    Check(!result.ok && result.error == FeatureExtractionError::DecodeFailed,
          "missing audio reports DecodeFailed");
}
void TestUnsupportedFormatHasItsOwnFailureReason(const fs::path& root) {
    const auto result = FeatureExtractor::extractFromFileChecked(
        (root / "unsupported.m4a").u8string());
    Check(!result.ok && result.error == FeatureExtractionError::UnsupportedFormat,
          "unsupported audio format reports UnsupportedFormat");
}
void TestPreCancelledRequestReportsCancellation(const fs::path& root) {
    const auto result = FeatureExtractor::extractFromFileChecked(
        (root / "missing.wav").u8string(), [] { return true; });
    Check(!result.ok && result.error == FeatureExtractionError::Cancelled,
          "pre-cancelled extraction reports Cancelled before decoding");
}
void TestValidPcmProducesFiniteFeatures(const fs::path& root) {
    const auto samples = MakeToneSamples();
    const fs::path path = root / "tone.wav"; WritePcmWav(path, samples);
    const auto result = FeatureExtractor::extractFromFileChecked(path.u8string());
    Check(result.ok && result.error == FeatureExtractionError::None, "valid PCM extraction succeeds");
    Check(result.features.isValid(), "valid PCM extraction produces finite features");
}

void TestUnicodePathUsesProductionUtf8Conversion(const fs::path& root) {
    const fs::path unicodeDir =
        root / fs::u8path(u8"\u97f3\u9891\u7d20\u6750");
    const fs::path path =
        unicodeDir / fs::u8path(u8"\u6d4b\u8bd5\u97f3.wav");
    std::error_code ec;
    fs::create_directories(unicodeDir, ec);
    WritePcmWav(path, MakeToneSamples());

    const auto result =
        FeatureExtractor::extractFromFileChecked(path.u8string());
    Check(result.ok && result.error == FeatureExtractionError::None,
          "UTF-8 audio path decodes through the production path converter");
    Check(result.features.isValid(),
          "UTF-8 audio path produces finite features");
}
void TestEmptyPcmReportsNoSamples(const fs::path& root) {
    const fs::path path = root / "empty.wav"; WritePcmWav(path, {});
    const auto result = FeatureExtractor::extractFromFileChecked(path.u8string());
    Check(!result.ok && result.error == FeatureExtractionError::NoSamples,
          "empty PCM reports NoSamples");
}

void TestBatchExtractionUsesIndependentCancellationSignal() {
    const fs::path repoRoot = fs::path(__FILE__).parent_path().parent_path();
    const std::string appState = ReadTextFile(repoRoot / "src" / "AppState.h");
    const std::string mainSource = ReadTextFile(repoRoot / "src" / "main.cpp");
    const std::string switchRuntime =
        ReadTextFile(repoRoot / "src" / "DatabaseSwitchWorkerRuntime.cpp");
    const std::string switchOperation =
        ReadTextFile(repoRoot / "src" / "DatabaseSwitchBlockingOperation.cpp");
    const std::string switchAdapter =
        ReadTextFile(repoRoot / "src" / "DatabaseSwitchBlockingOperationAdapter.cpp");
    const std::string windowsMain =
        ReadTextFile(repoRoot / "src" / "WindowsMain.cpp");
    const std::string windowsHeader =
        ReadTextFile(repoRoot / "src" / "WindowsMain.h");
    const std::string locale =
        ReadTextFile(repoRoot / "locale" / "harken.ui.strings.json");
    const size_t surfaceStart = mainSource.find(
        "void StartBatchFeatureExtraction(AppState& state)");
    const size_t surfaceEnd = mainSource.find(
        "void RenderUI(AppState& state)", surfaceStart);
    const std::string helper =
        surfaceStart == std::string::npos || surfaceEnd == std::string::npos
            ? std::string()
            : mainSource.substr(surfaceStart, surfaceEnd - surfaceStart);
    // The migrated index panel follows the shared batch entry, before RenderUI.
    const std::string& surface = helper;

    Check(appState.find("batchFeatureCancelRequested") != std::string::npos,
          "AppState has a cancellation signal separate from busy state");
    Check(appState.find("std::thread batchFeatureThread") != std::string::npos,
          "AppState owns the batch controller thread");
    const size_t cancelReset = surface.find(
        "batchFeatureCancelRequested.store(false)");
    const size_t busyStart = surface.find("batchFeatureExtracting.store(true)");
    Check(cancelReset != std::string::npos && busyStart != std::string::npos &&
              cancelReset < busyStart,
          "batch start resets cancellation before marking the controller busy");
    Check(surface.find("batchFeatureCancelRequested.store(true)") != std::string::npos,
          "cancel action requests cancellation without clearing busy state");
    Check(surface.find("batchFeatureThread = std::thread([&state]") != std::string::npos &&
              surface.find("}).detach()") == std::string::npos,
          "batch controller is owned as a joinable thread");
    Check(surface.find("batchFeatureThread.joinable()") != std::string::npos,
          "completed batch controller is joined before restart");
    Check(surface.find("ImGui::BeginDisabled(state.dbSwitchInProgress)") !=
              std::string::npos,
          "batch restart is disabled while a database switch owns teardown");
    Check(surface.find("while (state.batchFeatureExtracting.load())") == std::string::npos &&
              surface.find("while (!state.batchFeatureCancelRequested.load())") != std::string::npos,
          "workers stop from the independent cancellation signal");
    Check(surface.find("return state.batchFeatureCancelRequested.load()") != std::string::npos,
          "decoder cancellation callback reads the independent signal");
    Check(surface.find("if (!state.batchFeatureCancelRequested.load())") != std::string::npos,
          "similarity cache reload is skipped after cancellation");

    const size_t join = surface.find("for (auto& w : workers) w.join();");
    const size_t busyClear = surface.find("batchFeatureExtracting.store(false)");
    Check(join != std::string::npos && busyClear != std::string::npos &&
              busyClear > join,
          "busy state clears only after every worker joins");
    Check(surface.find("batchFeatureExtracting.store(false)", busyClear + 1) ==
              std::string::npos,
          "cancel path does not clear busy state before workers join");

    Check(windowsHeader.find("void WaitForDatabaseSwitchThread();") !=
              std::string::npos,
          "WindowsMain exposes the database-switch join barrier");
    Check(mainSource.find("void WaitForDatabaseSwitchThread()") !=
              std::string::npos,
          "main.cpp owns the database-switch join barrier implementation");

    const size_t shutdownCancel = windowsMain.find(
        "state.batchFeatureCancelRequested.store(true)");
    const size_t shutdownSwitchWait = windowsMain.find(
        "WaitForDatabaseSwitchThread();", shutdownCancel);
    const size_t shutdownJoin = windowsMain.find(
        "state.batchFeatureThread.join();", shutdownCancel);
    const size_t databaseClose = windowsMain.find("state.database.close();");
    Check(shutdownCancel != std::string::npos &&
              shutdownSwitchWait != std::string::npos &&
              shutdownJoin != std::string::npos &&
              databaseClose != std::string::npos &&
              shutdownCancel < shutdownSwitchWait &&
              shutdownSwitchWait < shutdownJoin &&
              shutdownJoin < databaseClose,
          "shutdown cancels, waits for DB switch, then joins batch before DB close");

    size_t dbJoinCount = 0;
    for (size_t pos = switchRuntime.find("impl_->worker.join();");
         pos != std::string::npos;
         pos = switchRuntime.find("impl_->worker.join();", pos + 1)) {
        ++dbJoinCount;
    }
    Check(dbJoinCount == 1,
          "database-switch thread has exactly one join owner");

    const size_t switchStart = mainSource.find(
        "static bool SwitchDatabase(AppState& state,");
    const size_t switchEnd = mainSource.find(
        "bool BeginSwitchDatabase", switchStart);
    const std::string switchSource =
        switchStart == std::string::npos || switchEnd == std::string::npos
            ? std::string()
            : mainSource.substr(switchStart, switchEnd - switchStart);
    const size_t switchCancel = switchSource.find(
        "state.batchFeatureCancelRequested.store(true)");
    const size_t dbWorkerStart = switchSource.find(
        "g_databaseSwitchWorkerRuntime.Start(");
    Check(switchCancel != std::string::npos &&
              dbWorkerStart != std::string::npos && switchCancel < dbWorkerStart,
          "database switch requests batch cancellation before starting teardown");

    const size_t switchBatchJoin = switchAdapter.find(
        "batchFeatureThread_.join();");
    const size_t switchJoinStep = switchOperation.find(
        "environment.JoinBatchFeatures();");
    const size_t switchDatabaseClose = switchOperation.find(
        "environment.CloseDatabase();");
    Check(switchBatchJoin != std::string::npos &&
              switchJoinStep != std::string::npos &&
              switchDatabaseClose != std::string::npos &&
              switchJoinStep < switchDatabaseClose,
          "database switch worker joins batch extraction before closing the database");

    Check(appState.find("batchFeatureFailedCount") != std::string::npos,
          "batch state publishes failed-file count separately from attempted progress");
    Check(appState.find("batchFeatureUnsupportedCount") != std::string::npos &&
              appState.find("batchFeatureDecodeFailedCount") != std::string::npos &&
              appState.find("batchFeatureNoSamplesCount") != std::string::npos &&
              appState.find("batchFeatureWriteFailedCount") != std::string::npos,
          "batch state keeps failure reasons separately");
    Check(surface.find("if (!state.database.saveAudioFeaturesByPath(path, vec))") !=
              std::string::npos,
          "batch treats a failed database write as a failed file");
    Check(surface.find("batchFeatureCompletionNotice.store(!paths.empty())") ==
              std::string::npos,
          "batch completion notice is not based only on a non-empty input list");
    Check(surface.find("Main_IndexPartialFailFmt") != std::string::npos,
          "similarity index panel has a partial-failure status");
    Check(windowsMain.find("batchFeatureFailedCount.load()") != std::string::npos,
          "isolated similarity-index status reports failed-file count");
    Check(windowsMain.find("unsupported_count") != std::string::npos &&
              windowsMain.find("decode_failed_count") != std::string::npos &&
              windowsMain.find("no_samples_count") != std::string::npos &&
              windowsMain.find("write_failed_count") != std::string::npos,
          "isolated similarity-index status reports failure reasons");
    Check(surface.find("Main_IndexFailureUnsupportedFmt") != std::string::npos &&
              surface.find("Main_IndexFailureDecodeFmt") != std::string::npos &&
              surface.find("Main_IndexFailureNoSamplesFmt") != std::string::npos &&
              surface.find("Main_IndexFailureWriteFmt") != std::string::npos,
          "similarity index panel renders failure reasons");
    Check(locale.find("Main_IndexFailureUnsupportedFmt") != std::string::npos &&
              locale.find("Main_IndexFailureDecodeFmt") != std::string::npos &&
              locale.find("Main_IndexFailureNoSamplesFmt") != std::string::npos &&
              locale.find("Main_IndexFailureWriteFmt") != std::string::npos,
          "failure reason labels are localized");
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "aris_feature_extractor_test";
    std::error_code ec; fs::remove_all(root, ec); fs::create_directories(root, ec);
    TestMissingFileReportsDecodeFailure(root);
    TestUnsupportedFormatHasItsOwnFailureReason(root);
    TestPreCancelledRequestReportsCancellation(root);
    TestValidPcmProducesFiniteFeatures(root);
    TestEmptyPcmReportsNoSamples(root);
    TestUnicodePathUsesProductionUtf8Conversion(root);
    TestBatchExtractionUsesIndependentCancellationSignal();
    fs::remove_all(root, ec);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
