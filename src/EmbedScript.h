#pragma once
// SoundManager - 元数据嵌入引擎模块
// EmbedScript.h - 音频文件元数据嵌入配置和执行
// Export-side metadata embedding helpers for delivery workflows.

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

// @stage7-contract
// API contract: embedding helpers consume owned/value metadata and return
// explicit format/write results. They do not retain callbacks or mutate UI
// state; file-format failure remains visible to the delivery workflow.

// Embed Target Formats

enum class EmbedTarget {
    BEXT,           // Broadcast WAV Extension chunk
    IXML,           // iXML chunk (WAV)
    Steinberg,      // Steinberg MediaBay attributes (iXML)
    ID3,            // ID3v2 tags (MP3/AIFF/WAV)
    LIST_INFO,      // LIST/INFO chunk (WAV)
    AXML,           // aXML chunk (ISRC in WAV)
    IXMLUser,       // iXML USER table (custom fields)
    VendorChunk     // Vendor-specific metadata chunk
};

// Embed Field Mapping

struct EmbedFieldMapping {
    std::string sourceField;    // 源元数据字段名 (如 "Description")
    std::string targetTag;      // 目标标签名 (如 "ID3_COMMENT")
    EmbedTarget target;         // 目标格式
    bool enabled = true;        // 是否启用
};

// BEXT Data Structure

struct BEXTData {
    std::string description;        // 256 chars max
    std::string originator;         // 32 chars max
    std::string originatorRef;      // 32 chars max
    std::string date;               // YYYY-MM-DD (10 chars)
    std::string time;               // HH:MM:SS (8 chars)
    uint64_t timeStamp = 0;         // Sample position
    std::string codingHistory;      // Unlimited
};

// iXML Data Structure

struct IXMLData {
    std::string project;
    std::string scene;
    std::string take;
    std::string tape;
    std::string note;
    std::string circled;
    
    // Speed section
    struct {
        std::string note;
        std::string masterSpeed;
        std::string currentSpeed;
        std::string timeCodeRate;
        std::string timeCodeFlag;
    } speed;
    
    // Track list
    struct TrackInfo {
        int channelIndex = 0;
        std::string name;
    };
    std::vector<TrackInfo> trackList;
    
    // User fields
    std::unordered_map<std::string, std::string> userFields;

    // Harken-owned section inside USER. Empty values are omitted.
    std::string harkenCategory;
    std::string harkenFunction;
    bool HasHarkenSection() const {
        return !harkenCategory.empty() || !harkenFunction.empty();
    }
};

// Steinberg MediaBay Mapping

struct SteinbergData {
    std::string mediaCompany;           // Publisher
    std::string mediaArtist;            // Artist
    std::string audioSoundEditor;       // Designer
    std::string mediaRecordingLocation; // Location
    std::string mediaRecordingMethod;   // Microphone
    std::string mediaTrackNumber;       // Track
    std::string mediaLibraryManufacturer; // Manufacturer
    std::string mediaAlbum;             // CDTitle
    std::string mediaCategoryPost;      // Category
    std::string musicalCharacter;       // Mood
    std::string audioSoundMixer;        // Recordist
    std::string mediaLibrary;           // Library
    std::string mediaComment;           // Description
    std::string musicalCategory;        // SubCategory
    std::string musicalInstrument;      // Keywords
    std::string smfSongName;            // TrackTitle or FXName
};

// ID3 Tag Data Structure

struct ID3EmbedData {
    std::string comment;         // ID3_COMMENT
    std::string description;     // ID3_DESCRIPTION (non-standard)
    std::string subtitle;        // ID3_SUBTITLE
    std::string composer;        // ID3_COMPOSER
    std::string publisher;       // ID3_PUBLISHER
    std::string contentGroup;    // ID3_CONTENTGROUP
    std::string title;           // ID3_TITLE
    std::string contentType;     // ID3_CONTENTTYPE (genre)
    std::string album;           // ID3_ALBUM
    std::string leadArtist;      // ID3_LEADARTIST
    std::string band;            // ID3_BAND (album artist)
    int year = 0;                // ID3_YEAR
    std::string bpm;             // ID3_BPM
    std::string lyrics;          // ID3_UNSYNCEDLYRICS
    std::string trackNum;        // ID3_TRACKNUM
    std::string origAlbum;       // ID3_ORIGALBUM
    std::string conductor;       // ID3_CONDUCTOR
    std::string initialKey;      // ID3_INITIALKEY
    std::string origArtist;      // ID3_ORIGARTIST
    std::string fileOwner;       // ID3_FILEOWNER
    std::string copyright;       // ID3_COPYRIGHT
    std::string isrc;            // ID3FID_ISRC
    std::vector<uint8_t> picture; // Album art data
};

// LIST/INFO Chunk Data

struct LISTInfoData {
    std::string IARL;   // Archival location (BWOriginator)
    std::string ICMT;   // Comments (Description)
    std::string IGNR;   // Genre (Category)
    std::string IART;   // Artist
    std::string IPRD;   // Product (CDTitle)
    std::string INAM;   // Name (TrackTitle)
    std::string ISFT;   // Software identifier (ISFT)
    std::string ICRD;   // Creation date
    std::string ICOP;   // Copyright
};

// Embed Configuration

struct EmbedConfig {
    bool embedBEXT = true;
    bool embedIXML = true;
    bool embedSteinberg = true;
    bool embedID3 = true;
    bool embedLISTInfo = true;
    bool embedAXML = true;          // ISRC in aXML
    bool embedIXMLUser = true;
    bool embedPicture = true;       // Copy album art
    
    // iXML USER fields to include
    std::vector<std::string> ixmlUserFields = {
        "CatID", "Category", "SubCategory", "Description", "Notes",
        "Microphone", "MicPerspective", "Library", "Designer",
        "ShootDate", "CategoryFull", "RecType", "ShortID", "TrackYear",
        "Keywords", "Show", "Source", "Location", "FXName", "TrackTitle",
        "Artist", "LongID", "Volume", "Track", "Manufacturer",
        "RecMedium", "CDTitle", "Rating", "URL", "ReleaseDate", "OpenTier"
    };
    
    int masterYear = 2024;          // Default year for copyright
};

// Embed Extra Parameters (transfer context)

struct EmbedExtraParams {
    bool firstStem = true;      // Is this the first stem?
    int currentStem = 1;        // Which stem number
    int totalStems = 1;         // Total stems
};

// Metadata Embed Engine

class EmbedEngine {
public:
    // Build embed data from source metadata
    // metadata: field name -> value
    // fileType: "WAVE", "AIFF", "MP3 ", "FLAC"
    static void buildEmbedData(
        const std::unordered_map<std::string, std::string>& metadata,
        const std::string& fileType,
        const EmbedConfig& config,
        const EmbedExtraParams& extraParams,
        // Output
        BEXTData& outBEXT,
        IXMLData& outIXML,
        SteinbergData& outSteinberg,
        ID3EmbedData& outID3,
        LISTInfoData& outLIST
    );

    // Serialize IXMLData into a BWF/iXML UTF-8 XML payload.
    static std::string serializeIXML(const IXMLData& ixml);
    
    // Get default embed config
    static EmbedConfig getDefaultConfig();
    
    // Build copyright string
    static std::string buildCopyrightString(
        const std::string& year,
        const std::string& manufacturer,
        const std::string& library,
        const std::string& url
    );
    
    // Get the complete list of embed field mappings
    static std::vector<EmbedFieldMapping> getDefaultMappings();
    
    // Validate ISRC code
    static bool validateISRC(const std::string& isrc, std::string& cleanedISRC);
    
    // Get metadata value with fallback
    static std::string getField(const std::unordered_map<std::string, std::string>& metadata,
                                 const std::string& field, const std::string& fallback = "");
};
