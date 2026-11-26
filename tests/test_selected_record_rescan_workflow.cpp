#include "src/SelectedRecordRescanWorkflow.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace aura::record_rescan;

namespace {

int g_failed = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failed;
    }
}

struct FakeOperationPort final : SelectedRecordRescanOperationPort {
    SelectedRecordRescanFileFacts fileFacts{true, true, 4096};
    SelectedRecordRescanMetadata metadata;
    SelectedRecordRescanAudioFacts audioFacts{48000, 2, 96000, 2.0f};
    bool metadataResult = true;
    bool decoderFailure = false;
    bool audioResult = true;
    bool updateResult = true;
    std::int64_t now = 7777;
    int clearCalls = 0;
    SelectedRecordRescanUpdate update;
    std::vector<std::string> trace;

    SelectedRecordRescanFileFacts observeFile(const std::string&) override {
        trace.push_back("file");
        return fileFacts;
    }
    bool readMetadata(const std::string&, SelectedRecordRescanMetadata& out) override {
        trace.push_back("metadata");
        out = metadata;
        return metadataResult;
    }
    bool wasDecoderInitFailure(const std::string&) override {
        trace.push_back("waveform");
        return decoderFailure;
    }
    bool probeAudio(const std::string&, SelectedRecordRescanAudioFacts& out) override {
        trace.push_back("audio");
        out = audioFacts;
        return audioResult;
    }
    std::int64_t lastWriteTimeUnixSeconds(const std::string&) override {
        trace.push_back("mtime");
        return 1234;
    }
    std::int64_t nowUnixSeconds() override {
        trace.push_back("now");
        return now;
    }
    bool updateRecord(const SelectedRecordRescanItem&,
                      const SelectedRecordRescanUpdate& value) override {
        trace.push_back("update");
        update = value;
        return updateResult;
    }
    void clearWaveformCache(const std::string&) override {
        trace.push_back("clear");
        ++clearCalls;
    }
};

SelectedRecordRescanItem Item() {
    SelectedRecordRescanItem item;
    item.sourceIndex = 3;
    item.filePath = "C:/sounds/hit.wav";
    item.title = "old-title";
    item.artist = "old-artist";
    item.album = "old-album";
    item.description = "old-description";
    item.category = "old-category";
    item.keywords = "old-keywords";
    item.extendedMetadata["library"] = "old-library";
    item.sampleRate = 44100;
    item.channels = 1;
    item.totalFrames = 44100;
    item.durationSeconds = 1.0f;
    return item;
}

void TestMissingFileStopsWithoutMutation() {
    FakeOperationPort port;
    port.fileFacts.regular = false;
    const auto result = RescanSelectedRecord(Item(), port);
    Check(result == SelectedRecordRescanItemResult::NotUpdated,
          "missing file is an ordinary not-updated result");
    Check(port.trace == std::vector<std::string>{"file"},
          "missing file stops before metadata and persistence");
}

void TestMetadataPrecedenceAndUpdateOrder() {
    FakeOperationPort port;
    port.metadata.title = "new-title";
    port.metadata.artist = "";
    port.metadata.genre = "genre-fallback";
    port.metadata.comment = "comment-fallback";
    port.metadata.extendedMetadata = {{"library", "new-library"},
                                     {"designer", "designer"}};

    const auto result = RescanSelectedRecord(Item(), port);
    Check(result == SelectedRecordRescanItemResult::Updated,
          "successful persistence reports updated");
    Check(port.trace == std::vector<std::string>{
              "file", "metadata", "waveform", "audio", "mtime", "now", "update"},
          "record observations and update preserve exact order");
    Check(port.update.title == "new-title" &&
              port.update.artist == "old-artist",
          "only non-empty basic metadata overwrites existing values");
    Check(port.update.category == "genre-fallback" &&
              port.update.keywords == "comment-fallback",
          "genre and comment remain secondary fallbacks");
    Check(port.update.extendedMetadata.at("library") == "new-library" &&
              port.update.extendedMetadata.at("designer") == "designer",
          "extended metadata merges over the copied record");
    Check(port.update.fileSize == 4096 && port.update.fileMtime == 1234 &&
              port.update.lastIndexed == 7777,
          "file observations and index time reach persistence");
    Check(port.update.sampleRate == 48000 && port.update.channels == 2 &&
              port.update.totalFrames == 96000 &&
              port.update.durationSeconds == 2.0f,
          "audio probe outputs reach persistence");
}

void TestWaveformRecoveryConditions() {
    auto missingAudio = Item();
    missingAudio.sampleRate = 0;
    FakeOperationPort missingPort;
    RescanSelectedRecord(missingAudio, missingPort);
    Check(missingPort.clearCalls == 1,
          "successful audio recovery clears cache for missing old facts");

    FakeOperationPort decoderPort;
    decoderPort.decoderFailure = true;
    RescanSelectedRecord(Item(), decoderPort);
    Check(decoderPort.clearCalls == 1,
          "successful decoder recovery clears stale failed waveform");

    FakeOperationPort probeFailure;
    probeFailure.decoderFailure = true;
    probeFailure.audioResult = false;
    RescanSelectedRecord(Item(), probeFailure);
    Check(probeFailure.clearCalls == 0,
          "failed audio probe never clears stale waveform");

    FakeOperationPort updateFailure;
    updateFailure.decoderFailure = true;
    updateFailure.updateResult = false;
    Check(RescanSelectedRecord(Item(), updateFailure) ==
              SelectedRecordRescanItemResult::NotUpdated &&
              updateFailure.clearCalls == 0,
          "failed record update is not counted and never clears waveform");
}

void TestMp3RereadFillsMissingTagsWithoutOverwritingLibraryEdits() {
    auto item = Item();
    item.filePath = "C:/sounds/tagged.MP3";
    item.artist.clear();
    item.extendedMetadata["composer"] = "library-composer";
    item.extendedMetadata["track_year"] = "";

    FakeOperationPort port;
    port.metadata.title = "embedded-title";
    port.metadata.artist = "embedded-artist";
    port.metadata.album = "embedded-album";
    port.metadata.description = "embedded-description";
    port.metadata.keywords = "embedded-keywords";
    port.metadata.extendedMetadata = {{"composer", "embedded-composer"},
                                      {"track_year", "2024"},
                                      {"musical_key", "A#m"}};

    Check(RescanSelectedRecord(item, port) == SelectedRecordRescanItemResult::Updated,
          "MP3 rescan updates the library record");
    Check(port.update.title == item.title && port.update.album == item.album &&
              port.update.description == item.description && port.update.keywords == item.keywords,
          "MP3 rescan keeps nonempty standard fields already in the library");
    Check(port.update.artist == "embedded-artist" &&
              port.update.extendedMetadata.at("composer") == "library-composer" &&
              port.update.extendedMetadata.at("track_year") == "2024" &&
              port.update.extendedMetadata.at("musical_key") == "A#m",
          "MP3 rescan fills missing music fields without replacing user values");
}

void TestMp3RereadDistinguishesEmptyFromUnavailableTags() {
    auto item = Item();
    item.filePath = "C:/sounds/tagless.mp3";
    FakeOperationPort readable;
    readable.metadata.id3TextTags.emplace();
    Check(RescanSelectedRecord(item, readable) == SelectedRecordRescanItemResult::Updated &&
              readable.update.rereadId3TextTags && readable.update.rereadId3TextTags->empty(),
          "MP3 reread publishes an empty tag set when tags were removed");

    FakeOperationPort unreadable;
    unreadable.metadataResult = false;
    Check(RescanSelectedRecord(item, unreadable) == SelectedRecordRescanItemResult::Updated &&
              !unreadable.update.rereadId3TextTags,
          "MP3 reread keeps saved tags when the tag read fails");
}

struct FakeBatchPort final : SelectedRecordRescanBatchPort {
    std::vector<SelectedRecordRescanItemResult> results;
    std::vector<std::string> trace;
    std::size_t next = 0;
    bool beginResult = true;
    bool commitResult = true;

    bool beginTransaction() override {
        trace.push_back("begin");
        return beginResult;
    }
    SelectedRecordRescanItemResult processProtected(
        const SelectedRecordRescanItem& item) override {
        trace.push_back("item:" + std::to_string(item.sourceIndex));
        return results.at(next++);
    }
    bool commitTransaction() override {
        trace.push_back("commit");
        return commitResult;
    }
};

void TestBatchLifecycleAndCounts() {
    FakeBatchPort emptyPort;
    const auto empty = RescanSelectedRecords({}, emptyPort);
    Check(empty.updated == 0 && empty.failed == 0 && emptyPort.trace.empty(),
          "empty batch performs no transaction or record call");

    auto first = Item();
    first.sourceIndex = 4;
    auto second = Item();
    second.sourceIndex = 5;
    auto third = Item();
    third.sourceIndex = 6;
    FakeBatchPort port;
    port.results = {SelectedRecordRescanItemResult::Updated,
                    SelectedRecordRescanItemResult::NotUpdated,
                    SelectedRecordRescanItemResult::Failed};
    const auto result = RescanSelectedRecords({first, second, third}, port);
    Check(result.updated == 1 && result.failed == 2,
          "batch counts both protected failures and zero-update outcomes");
    Check(port.trace == std::vector<std::string>{
              "begin", "item:4", "item:5", "item:6", "commit"},
          "successful batch preserves transaction and input order");

    FakeBatchPort beginFailure;
    beginFailure.beginResult = false;
    beginFailure.results = {SelectedRecordRescanItemResult::Updated};
    const auto notStarted = RescanSelectedRecords({first}, beginFailure);
    Check(notStarted.updated == 0 && notStarted.failed == 1 &&
              beginFailure.trace == std::vector<std::string>{"begin"},
          "begin failure aborts without processing and reports failure");

    FakeBatchPort commitFailure;
    commitFailure.commitResult = false;
    commitFailure.results = {SelectedRecordRescanItemResult::Updated};
    const auto notCommitted = RescanSelectedRecords({first}, commitFailure);
    Check(notCommitted.updated == 0 && notCommitted.failed == 1 &&
              commitFailure.trace == std::vector<std::string>{
                  "begin", "item:4", "commit"},
          "commit failure never reports uncommitted updates as successful");

    FakeBatchPort unchangedPort;
    unchangedPort.results = {SelectedRecordRescanItemResult::NotUpdated};
    const auto unchanged = RescanSelectedRecords({first}, unchangedPort);
    Check(unchanged.updated == 0 && unchanged.failed == 1,
          "zero-update batch reports a visible unsuccessful outcome");
}

}  // namespace

int main() {
    TestMissingFileStopsWithoutMutation();
    TestMetadataPrecedenceAndUpdateOrder();
    TestWaveformRecoveryConditions();
    TestMp3RereadFillsMissingTagsWithoutOverwritingLibraryEdits();
    TestMp3RereadDistinguishesEmptyFromUnavailableTags();
    TestBatchLifecycleAndCounts();
    if (g_failed != 0) {
        std::cerr << "FAIL: " << g_failed
                  << " selected record rescan checks failed\n";
        return 1;
    }
    std::cout << "PASS: selected record rescan workflow\n";
    return 0;
}
