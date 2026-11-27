#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

// Metadata Field Patch — specifies which fields to write back to file

struct MetadataFieldPatch {
    // Basic tags (empty = don't touch)
    std::string title;
    std::string artist;
    std::string description;
    std::string keywords;
    std::string category;
    std::string album;
    std::string comment;
    std::string genre;

    // BWF extension fields (WAV only)
    std::string bwOriginator;       // 32 chars max
    std::string bwOriginatorRef;    // 32 chars max
    std::string bwDate;             // YYYY-MM-DD
    std::string bwTime;             // HH:MM:SS
    std::string bwUMID;             // 64 hex chars
    uint64_t bwTimeReference = 0;    // BWF TimeReference sample position
    bool hasTimeReference = false;

    // Pre-serialized iXML chunk payload (UTF-8 XML). Empty means do not write.
    std::string ixmlChunk;
    bool hasIXMLChunk() const { return !ixmlChunk.empty(); }

    bool hasBasicFields() const {
        return !title.empty() || !artist.empty() || !description.empty() ||
               !keywords.empty() || !category.empty() || !album.empty() ||
               !comment.empty() || !genre.empty();
    }
    bool hasBWFFields() const {
        return !bwOriginator.empty() || !bwOriginatorRef.empty() ||
               !bwDate.empty() || !bwTime.empty() || !bwUMID.empty();
    }
};

// @stage7-contract
// API contract: write operations accept UTF-8 paths, preserve the original
// file on failure, and report success only after the documented atomic write
// completes. Batch methods return the number of successful files and summarize
// failures without retaining caller-owned strings or patch storage.
// Metadata writing functionality for audio files
// Supports: album art, general field writeback, BWF extension fields

class MetadataWriter {
public:
// Album Art
    static bool writeAlbumArt(const std::string& audioFilePath,
                              const std::vector<uint8_t>& imageData,
                              const std::string& mimeType);
    static bool writeAlbumArtFromFile(const std::string& audioFilePath,
                                      const std::string& imageFilePath);
    static bool removeAlbumArt(const std::string& audioFilePath);
    static int writeAlbumArtBatch(const std::vector<std::string>& audioPaths,
                                  const std::string& imageFilePath);
    static int removeAlbumArtBatch(const std::vector<std::string>& audioPaths);
    static bool loadImageFile(const std::string& imagePath,
                              std::vector<uint8_t>& imageData,
                              std::string& mimeType);

// General Field Writeback (F-013)
    // Write metadata fields to audio file. Uses temp file + atomic swap.
    // Supported: MP3 (ID3v2), WAV (LIST/INFO + BEXT), FLAC (Vorbis Comment)
    // Returns true on success, errorMessage filled on failure.
    static bool writeMetadataFields(const std::string& audioFilePath,
                                    const MetadataFieldPatch& patch,
                                    std::string& errorMessage);

    // Write the Harken-owned iXML USER/HARKEN section without touching the
    // standard BEXT/iXML/ID3 fields. WAV keeps one merged iXML chunk; FLAC
    // stores the same XML payload in a vendor APPLICATION block.
    static bool writeHarkenMetadata(const std::string& audioFilePath,
                                    const std::string& harkenMetaJson,
                                    std::string& errorMessage);

    // Batch field writeback. Returns number of files successfully written.
    static int writeMetadataFieldsBatch(const std::vector<std::string>& audioPaths,
                                        const MetadataFieldPatch& patch,
                                        std::string& errorSummary);

private:
    // Format-specific album art writers
    static bool writeID3v2AlbumArt(const std::string& filePath,
                                   const std::vector<uint8_t>& imageData,
                                   const std::string& mimeType);
    static bool writeWAVID3AlbumArt(const std::string& filePath,
                                    const std::vector<uint8_t>& imageData,
                                    const std::string& mimeType);
    static bool writeOGGAlbumArt(const std::string& filePath,
                                 const std::vector<uint8_t>& imageData,
                                 const std::string& mimeType);
    static bool writeFLACAlbumArt(const std::string& filePath,
                                  const std::vector<uint8_t>& imageData,
                                  const std::string& mimeType);
    static bool writeM4AAlbumArt(const std::string& filePath,
                                 const std::vector<uint8_t>& imageData,
                                 const std::string& mimeType);

    // Format-specific field writers
    static bool writeID3v2Fields(const std::string& filePath,
                                 const MetadataFieldPatch& patch,
                                 std::string& errorMessage);
    static bool writeWAVFields(const std::string& filePath,
                               const MetadataFieldPatch& patch,
                               std::string& errorMessage);
    static bool writeFLACFields(const std::string& filePath,
                                const MetadataFieldPatch& patch,
                                std::string& errorMessage);

    // Helpers
    static std::string getExtension(const std::string& filePath);
    static bool atomicSwap(const std::string& originalPath, const std::string& tempPath);
};
