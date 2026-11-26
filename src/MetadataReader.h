#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>

// Album art data
struct AlbumArt {
    std::vector<uint8_t> data;      // Raw image data (JPEG/PNG)
    std::string mimeType;            // "image/jpeg" or "image/png"
    int width = 0;
    int height = 0;

    bool hasData() const { return !data.empty(); }
};

// Audio metadata structure (extended metadata fields)
struct AudioMetadata {
    // Basic tags
    std::string title;
    std::string artist;
    std::string album;
    std::string description;
    std::string genre;
    std::string comment;
    int year = 0;
    int track = 0;
    std::string albumArtist;  // TPE2 / ALBUMARTIST / album artist atom
    int disc = 1;             // TPOS / DISCNUMBER / disc atom (1-based)
    std::string composer;
    std::string bpm;
    std::string musicalKey;
    std::unordered_map<std::string, std::string> customTextTags;

    // BWF/Broadcast specific (from bext chunk)
    std::string bwDescription;    // BWF Description (256 chars from bext)
    std::string originator;       // BWOriginator
    std::string originatorRef;    // BWOriginatorRef
    std::string originationDate;  // BWF date
    std::string originationTime;  // BWF time
    std::string codingHistory;    // Coding history
    std::string umid;             // Unique Material Identifier
    uint64_t bwTimeStamp = 0;     // BWTimeStamp (sample count)

    // Extended metadata fields
    std::string category;       // Primary category
    std::string subcategory;    // Sub-category
    std::string keywords;       // Searchable keywords/tags
    std::string library;        // Sound library name
    std::string designer;       // Sound designer
    std::string manufacturer;   // Equipment/source manufacturer
    std::string notes;          // Additional notes
    std::string url;            // Reference URL
    std::string copyright;      // Copyright info
    std::string software;       // Creation software

    // iXML / Production fields
    std::string project;        // Project name
    std::string scene;          // Scene identifier
    std::string take;           // Take number
    std::string tape;           // Tape/reel identifier
    std::string circled;        // Circled take marker
    std::string fileUID;        // Unique file identifier
    std::string userBits;       // Timecode user bits
    std::string trackList;      // Track names

    // Instrument/genre vendor tags (F-033: used to populate norm_instruments)
    std::string instruments;       // TXXX:INSTRUMENTS / Vorbis INSTRUMENTS

    // ReplayGain (dB values; 0.0 = not present/not set)
    float replayGainTrack = 0.0f;  // REPLAYGAIN_TRACK_GAIN
    float replayGainAlbum = 0.0f;  // REPLAYGAIN_ALBUM_GAIN

    // Album art
    AlbumArt albumArt;

    bool hasData() const {
        return !title.empty() || !artist.empty() || !album.empty() ||
               !description.empty() || !comment.empty() || !keywords.empty() ||
               !category.empty() || !library.empty() || !bwDescription.empty() ||
               !originator.empty() || !originatorRef.empty() || !codingHistory.empty();
    }
};

// Map AudioMetadata → DB-column-keyed map for extended metadata fields
// Called by scan pipeline and rescan to populate AudioRecord.meta
inline void PopulateExtendedMeta(std::unordered_map<std::string, std::string>& meta,
                                  const AudioMetadata& am) {
    auto s = [&](const char* col, const std::string& val) {
        if (!val.empty()) meta[col] = val;
    };
    s("subcategory", am.subcategory);  s("notes", am.notes);
    s("url", am.url);                 s("copyright", am.copyright);
    s("software", am.software);       s("genre", am.genre);
    s("comment", am.comment);         s("library", am.library);
    if (am.year > 0) meta["track_year"] = std::to_string(am.year);
    s("composer", am.composer);       s("bpm", am.bpm);
    s("musical_key", am.musicalKey);
    s("manufacturer", am.manufacturer); s("designer", am.designer);
    s("bw_description", am.bwDescription);
    s("bw_originator", am.originator); s("bw_originator_ref", am.originatorRef);
    s("bw_date", am.originationDate);  s("bw_time", am.originationTime);
    s("bw_umid", am.umid);            s("bw_coding_history", am.codingHistory);
    s("ixml_project", am.project);    s("scene", am.scene);
    s("take_num", am.take);           s("tape", am.tape);
    s("ixml_file_uid", am.fileUID);   s("ixml_track_layout", am.trackList);
}

// @stage7-contract
// API contract: read operations accept UTF-8 paths or owned input strings and
// write outputs only on success. They do not retain caller storage, do not
// mutate source files, and return false for unsupported, malformed, or
// unreadable input while leaving the output object in its caller-provided state.
class MetadataReader {
public:
    // Read metadata from audio file (auto-detects format)
    static bool readMetadata(const std::string& filePath, AudioMetadata& metadata);

    // Read album art only (for display)
    static bool readAlbumArt(const std::string& filePath, AlbumArt& art);

    // Format-specific readers
    static bool readID3v1(const std::string& filePath, AudioMetadata& metadata);
    static bool readID3v2(const std::string& filePath, AudioMetadata& metadata,
                          bool useV1Fallback = false);
    static bool readBWF(const std::string& filePath, AudioMetadata& metadata);
    static bool readVorbisComment(const std::string& filePath, AudioMetadata& metadata);
    static bool readM4A(const std::string& filePath, AudioMetadata& metadata);

    // Parse iXML chunk data (embedded in WAV files)
    static bool parseIXML(const std::string& xmlData, AudioMetadata& metadata);

private:
    // Helper functions
    static std::string trimString(const std::string& str);
    static std::string readFixedString(const char* data, size_t maxLen);
    static uint32_t readLE32(const uint8_t* data);
    static uint32_t readBE32(const uint8_t* data);
    static uint32_t readSyncSafeInt(const uint8_t* data);
    static bool readID3v2AlbumArt(const std::string& filePath, AlbumArt& art);
    static bool readFLACAlbumArt(const std::string& filePath, AlbumArt& art);
    static bool readM4AAlbumArt(const std::string& filePath, AlbumArt& art);
    static bool readOGGAlbumArt(const std::string& filePath, AlbumArt& art);
    static bool readWAVID3Chunk(const std::string& filePath, AlbumArt& art);
};
