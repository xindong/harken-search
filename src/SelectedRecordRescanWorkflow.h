#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aura::record_rescan {

// @stage7-contract
// Private synchronous workflow values. All strings and maps are owned; ports
// are borrowed for a call and retained nowhere.
struct SelectedRecordRescanItem {
    std::size_t sourceIndex = 0;
    std::string filePath;
    std::string title;
    std::string artist;
    std::string album;
    std::string description;
    std::string category;
    std::string keywords;
    std::unordered_map<std::string, std::string> extendedMetadata;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint64_t totalFrames = 0;
    float durationSeconds = 0.0f;
};

struct SelectedRecordRescanMetadata {
    std::string title;
    std::string artist;
    std::string album;
    std::string description;
    std::string category;
    std::string genre;
    std::string keywords;
    std::string comment;
    std::unordered_map<std::string, std::string> extendedMetadata;
    std::optional<std::unordered_map<std::string, std::string>> id3TextTags;
};

struct SelectedRecordRescanFileFacts {
    bool regular = false;
    bool sizeAvailable = false;
    std::uint64_t size = 0;
};

struct SelectedRecordRescanAudioFacts {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint64_t totalFrames = 0;
    float durationSeconds = 0.0f;
};

struct SelectedRecordRescanUpdate {
    std::string title;
    std::string artist;
    std::string album;
    std::string description;
    std::string category;
    std::string keywords;
    std::unordered_map<std::string, std::string> extendedMetadata;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint64_t totalFrames = 0;
    float durationSeconds = 0.0f;
    std::uint64_t fileSize = 0;
    std::int64_t fileMtime = 0;
    std::int64_t lastIndexed = 0;
    std::optional<std::unordered_map<std::string, std::string>> rereadId3TextTags;
};

enum class SelectedRecordRescanItemResult {
    NotUpdated,
    Updated,
    Failed,
};

struct SelectedRecordRescanBatchResult {
    int updated = 0;
    int failed = 0;
};

class SelectedRecordRescanOperationPort {
public:
    virtual ~SelectedRecordRescanOperationPort() = default;

    virtual SelectedRecordRescanFileFacts observeFile(
        const std::string& filePath) = 0;
    virtual bool readMetadata(
        const std::string& filePath,
        SelectedRecordRescanMetadata& metadata) = 0;
    virtual bool wasDecoderInitFailure(const std::string& filePath) = 0;
    virtual bool probeAudio(
        const std::string& filePath,
        SelectedRecordRescanAudioFacts& facts) = 0;
    virtual std::int64_t lastWriteTimeUnixSeconds(
        const std::string& filePath) = 0;
    virtual std::int64_t nowUnixSeconds() = 0;
    virtual bool updateRecord(
        const SelectedRecordRescanItem& item,
        const SelectedRecordRescanUpdate& update) = 0;
    virtual void clearWaveformCache(const std::string& filePath) = 0;
};

class SelectedRecordRescanBatchPort {
public:
    virtual ~SelectedRecordRescanBatchPort() = default;

    virtual bool beginTransaction() = 0;
    virtual SelectedRecordRescanItemResult processProtected(
        const SelectedRecordRescanItem& item) = 0;
    virtual bool commitTransaction() = 0;
};

SelectedRecordRescanItemResult RescanSelectedRecord(
    const SelectedRecordRescanItem& item,
    SelectedRecordRescanOperationPort& port);

SelectedRecordRescanBatchResult RescanSelectedRecords(
    const std::vector<SelectedRecordRescanItem>& items,
    SelectedRecordRescanBatchPort& port);

}  // namespace aura::record_rescan
