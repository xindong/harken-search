// SoundManager - 元数据嵌入引擎模块
// EmbedScript.cpp - 元数据嵌入构建逻辑
// Export-side metadata embedding helpers for delivery workflows.

#include "EmbedScript.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <ctime>

// Helpers

std::string EmbedEngine::getField(const std::unordered_map<std::string, std::string>& metadata,
                                   const std::string& field, const std::string& fallback) {
    auto it = metadata.find(field);
    if (it != metadata.end() && !it->second.empty()) return it->second;
    return fallback;
}

bool EmbedEngine::validateISRC(const std::string& isrc, std::string& cleanedISRC) {
    if (isrc.empty() || isrc.length() < 12 || isrc.length() > 15) return false;
    
    cleanedISRC.clear();
    for (char c : isrc) {
        if (c != '-') cleanedISRC += c;
    }
    if (cleanedISRC.length() != 12) return false;
    
    // Validate pattern: 2 alpha + 3 alnum + 7 digit
    if (!std::isalpha((unsigned char)cleanedISRC[0]) ||
        !std::isalpha((unsigned char)cleanedISRC[1])) return false;
    for (int i = 2; i < 5; i++) {
        if (!std::isalnum((unsigned char)cleanedISRC[i])) return false;
    }
    for (int i = 5; i < 12; i++) {
        if (!std::isdigit((unsigned char)cleanedISRC[i])) return false;
    }
    return true;
}

std::string EmbedEngine::buildCopyrightString(
    const std::string& year, const std::string& manufacturer,
    const std::string& library, const std::string& url) {
    
    std::string yearStr = year;
    if (yearStr.empty()) {
        time_t now = time(nullptr);
        struct tm t;
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        yearStr = std::to_string(t.tm_year + 1900);
    }
    
    std::string result = yearStr;
    if (!manufacturer.empty()) result += " " + manufacturer;
    if (!library.empty()) result += " (" + library + ")";
    if (!url.empty()) result += " " + url;
    
    // Remove double spaces
    std::string cleaned;
    bool lastSpace = false;
    for (char c : result) {
        if (c == ' ') {
            if (!lastSpace) cleaned += c;
            lastSpace = true;
        } else {
            cleaned += c;
            lastSpace = false;
        }
    }
    return cleaned;
}

static std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string EmbedEngine::serializeIXML(const IXMLData& ix) {
    auto tag = [](const std::string& name, const std::string& value, const char* indent = "  ") {
        if (value.empty()) return std::string();
        return std::string(indent) + "<" + name + ">" + xmlEscape(value) + "</" + name + ">\n";
    };

    std::string xml;
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<BWFXML>\n";
    xml += "  <IXML_VERSION>1.5</IXML_VERSION>\n";
    xml += tag("PROJECT", ix.project);
    xml += tag("SCENE", ix.scene);
    xml += tag("TAKE", ix.take);
    xml += tag("TAPE", ix.tape);
    xml += tag("NOTE", ix.note);
    xml += tag("CIRCLED", ix.circled);

    if (!ix.speed.note.empty() || !ix.speed.masterSpeed.empty() ||
        !ix.speed.currentSpeed.empty() || !ix.speed.timeCodeRate.empty() ||
        !ix.speed.timeCodeFlag.empty()) {
        xml += "  <SPEED>\n";
        xml += tag("NOTE", ix.speed.note, "    ");
        xml += tag("MASTER_SPEED", ix.speed.masterSpeed, "    ");
        xml += tag("CURRENT_SPEED", ix.speed.currentSpeed, "    ");
        xml += tag("TIMECODE_RATE", ix.speed.timeCodeRate, "    ");
        xml += tag("TIMECODE_FLAG", ix.speed.timeCodeFlag, "    ");
        xml += "  </SPEED>\n";
    }

    xml += "  <TRACK_LIST>\n";
    xml += "    <TRACK_COUNT>" + std::to_string((int)ix.trackList.size()) + "</TRACK_COUNT>\n";
    for (const auto& track : ix.trackList) {
        xml += "    <TRACK>\n";
        xml += "      <CHANNEL_INDEX>" + std::to_string(track.channelIndex) + "</CHANNEL_INDEX>\n";
        xml += "      <NAME>" + xmlEscape(track.name) + "</NAME>\n";
        xml += "    </TRACK>\n";
    }
    xml += "  </TRACK_LIST>\n";

    if (!ix.userFields.empty() || ix.HasHarkenSection()) {
        xml += "  <USER>\n";
        for (const auto& kv : ix.userFields) {
            if (kv.first.empty() || kv.second.empty()) continue;
            xml += "    <" + kv.first + ">" + xmlEscape(kv.second) + "</" + kv.first + ">\n";
        }
        if (ix.HasHarkenSection()) {
            xml += "    <HARKEN>\n";
            xml += tag("CATEGORY", ix.harkenCategory, "      ");
            xml += tag("FUNCTION", ix.harkenFunction, "      ");
            xml += "    </HARKEN>\n";
        }
        xml += "  </USER>\n";
    }

    xml += "</BWFXML>\n";
    return xml;
}

// Build Embed Data

void EmbedEngine::buildEmbedData(
    const std::unordered_map<std::string, std::string>& metadata,
    const std::string& fileType,
    const EmbedConfig& config,
    const EmbedExtraParams& extraParams,
    BEXTData& outBEXT,
    IXMLData& outIXML,
    SteinbergData& outSteinberg,
    ID3EmbedData& outID3,
    LISTInfoData& outLIST) {

// BEXT
    if (config.embedBEXT && (fileType == "WAVE" || fileType == "FLAC")) {
        std::string bwDesc = getField(metadata, "BWDescription", getField(metadata, "Description"));
        outBEXT.description = bwDesc;
        outBEXT.date = getField(metadata, "BWDate");
        outBEXT.time = getField(metadata, "BWTime");
        outBEXT.originator = getField(metadata, "BWOriginator");
        outBEXT.originatorRef = getField(metadata, "BWOriginatorRef");
    }

// iXML
    if (config.embedIXML) {
        outIXML.project = getField(metadata, "ixmlProject");
        outIXML.take = getField(metadata, "Take");
        outIXML.tape = getField(metadata, "Tape");
        outIXML.scene = getField(metadata, "Scene");
        outIXML.note = getField(metadata, "ixmlNote");
        
        outIXML.speed.note = getField(metadata, "ixmlSpeedNote");
        outIXML.speed.masterSpeed = getField(metadata, "ixmlMasterSpeed");
        outIXML.speed.currentSpeed = getField(metadata, "ixmlCurrentSpeed");
        outIXML.speed.timeCodeRate = getField(metadata, "ixmlTimeCodeRate");
        outIXML.speed.timeCodeFlag = getField(metadata, "ixmlTimeCodeFlag");
        
        // User fields
        if (config.embedIXMLUser) {
            for (const auto& fieldName : config.ixmlUserFields) {
                std::string val = getField(metadata, fieldName);
                if (!val.empty()) {
                    outIXML.userFields[fieldName] = val;
                }
            }
            outIXML.userFields["Embedder"] = "Harken Search";
        }
        outIXML.harkenCategory = getField(metadata, "HarkenCategory");
        outIXML.harkenFunction = getField(metadata, "HarkenFunction");
    }

// Steinberg MediaBay
    if (config.embedSteinberg) {
        outSteinberg.mediaCompany = getField(metadata, "Publisher");
        outSteinberg.mediaArtist = getField(metadata, "Artist");
        outSteinberg.audioSoundEditor = getField(metadata, "Designer");
        outSteinberg.mediaRecordingLocation = getField(metadata, "Location");
        outSteinberg.mediaRecordingMethod = getField(metadata, "Microphone");
        outSteinberg.mediaTrackNumber = getField(metadata, "Track");
        outSteinberg.mediaLibraryManufacturer = getField(metadata, "Manufacturer");
        outSteinberg.mediaAlbum = getField(metadata, "CDTitle");
        outSteinberg.mediaCategoryPost = getField(metadata, "Category");
        outSteinberg.musicalCharacter = getField(metadata, "Mood");
        outSteinberg.audioSoundMixer = getField(metadata, "Recordist");
        outSteinberg.mediaLibrary = getField(metadata, "Library");
        outSteinberg.mediaComment = getField(metadata, "Description",
                                              getField(metadata, "BWDescription"));
        outSteinberg.musicalCategory = getField(metadata, "SubCategory");
        outSteinberg.musicalInstrument = getField(metadata, "Keywords");
        outSteinberg.smfSongName = getField(metadata, "TrackTitle",
                                             getField(metadata, "FXName"));
    }

// ID3 Tags
    if (config.embedID3 && (fileType == "MP3 " || fileType == "AIFF" || fileType == "WAVE")) {
        outID3.comment = getField(metadata, "Description");
        outID3.description = getField(metadata, "Description");
        outID3.subtitle = getField(metadata, "Notes");
        outID3.composer = getField(metadata, "Composer");
        outID3.publisher = getField(metadata, "Publisher");
        outID3.contentGroup = getField(metadata, "Publisher");
        outID3.title = getField(metadata, "TrackTitle");
        outID3.contentType = getField(metadata, "Category");
        outID3.album = getField(metadata, "CDTitle");
        outID3.leadArtist = getField(metadata, "Artist");
        outID3.bpm = getField(metadata, "BPM");
        outID3.lyrics = getField(metadata, "Lyrics");
        outID3.band = getField(metadata, "Library");
        outID3.trackNum = getField(metadata, "Track");
        outID3.origAlbum = getField(metadata, "Source");
        outID3.conductor = getField(metadata, "Conductor");
        outID3.initialKey = getField(metadata, "Key");
        outID3.origArtist = getField(metadata, "Performer");
        outID3.fileOwner = getField(metadata, "Library");
        
        // Year
        std::string embedYear = getField(metadata, "TrackYear");
        if (embedYear.empty()) {
            std::string releaseDate = getField(metadata, "ReleaseDate");
            if (releaseDate.length() > 4) {
                embedYear = releaseDate.substr(0, 4);
            }
        }
        if (embedYear.empty()) embedYear = std::to_string(config.masterYear);
        try { outID3.year = std::stoi(embedYear); } catch (...) { outID3.year = config.masterYear; }
        
        // Copyright
        std::string copyright = getField(metadata, "Copyright");
        if (copyright.empty()) {
            copyright = buildCopyrightString(embedYear,
                getField(metadata, "Manufacturer"),
                getField(metadata, "Library"),
                getField(metadata, "URL"));
        }
        outID3.copyright = copyright;
        
        // ISRC
        std::string isrc = getField(metadata, "ISRC");
        if (!isrc.empty()) {
            std::string cleaned;
            if (validateISRC(isrc, cleaned)) {
                outID3.isrc = cleaned;
            }
        }
    }

// LIST/INFO
    if (config.embedLISTInfo && fileType == "WAVE") {
        outLIST.IARL = getField(metadata, "BWOriginator");
        outLIST.ICMT = getField(metadata, "Description");
        outLIST.IGNR = getField(metadata, "Category");
        outLIST.IART = getField(metadata, "Artist",
                                 getField(metadata, "Designer",
                                          getField(metadata, "Composer",
                                                   getField(metadata, "Manufacturer"))));
        outLIST.IPRD = getField(metadata, "CDTitle", getField(metadata, "Library"));
        outLIST.INAM = getField(metadata, "TrackTitle", getField(metadata, "FXName"));
        outLIST.ISFT = "Harken Search";
        outLIST.ICRD = getField(metadata, "ReleaseDate");
        
        std::string embedYear = getField(metadata, "TrackYear");
        if (embedYear.empty()) {
            std::string releaseDate = getField(metadata, "ReleaseDate");
            if (releaseDate.length() > 4) embedYear = releaseDate.substr(0, 4);
        }
        if (embedYear.empty()) embedYear = std::to_string(config.masterYear);
        
        outLIST.ICOP = buildCopyrightString(embedYear,
            getField(metadata, "Manufacturer"),
            getField(metadata, "Library"),
            getField(metadata, "URL"));
    }
}

// Default Config

EmbedConfig EmbedEngine::getDefaultConfig() {
    return EmbedConfig(); // Use struct defaults
}

// Default Field Mappings

std::vector<EmbedFieldMapping> EmbedEngine::getDefaultMappings() {
    std::vector<EmbedFieldMapping> m;
    
    // BEXT mappings
    m.push_back({"Description",     "BWDescription",     EmbedTarget::BEXT});
    m.push_back({"BWDate",          "BWDate",            EmbedTarget::BEXT});
    m.push_back({"BWTime",          "BWTime",            EmbedTarget::BEXT});
    m.push_back({"BWOriginator",    "BWOriginator",      EmbedTarget::BEXT});
    
    // iXML mappings
    m.push_back({"ixmlProject",     "PROJECT",           EmbedTarget::IXML});
    m.push_back({"Scene",           "SCENE",             EmbedTarget::IXML});
    m.push_back({"Take",            "TAKE",              EmbedTarget::IXML});
    m.push_back({"Tape",            "TAPE",              EmbedTarget::IXML});
    m.push_back({"ixmlNote",        "NOTE",              EmbedTarget::IXML});
    
    // Steinberg mappings
    m.push_back({"Publisher",       "MediaCompany",      EmbedTarget::Steinberg});
    m.push_back({"Artist",          "MediaArtist",       EmbedTarget::Steinberg});
    m.push_back({"Designer",        "AudioSoundEditor",  EmbedTarget::Steinberg});
    m.push_back({"Location",        "MediaRecordingLocation", EmbedTarget::Steinberg});
    m.push_back({"Microphone",      "MediaRecordingMethod",   EmbedTarget::Steinberg});
    m.push_back({"Manufacturer",    "MediaLibraryManufacturerName", EmbedTarget::Steinberg});
    m.push_back({"CDTitle",         "MediaAlbum",        EmbedTarget::Steinberg});
    m.push_back({"Category",        "MediaCategoryPost", EmbedTarget::Steinberg});
    m.push_back({"Mood",            "MusicalCharacter",  EmbedTarget::Steinberg});
    m.push_back({"Library",         "MediaLibrary",      EmbedTarget::Steinberg});
    m.push_back({"Description",     "MediaComment",      EmbedTarget::Steinberg});
    m.push_back({"SubCategory",     "MusicalCategory",   EmbedTarget::Steinberg});
    m.push_back({"Keywords",        "MusicalInstrument", EmbedTarget::Steinberg});
    m.push_back({"TrackTitle",      "SmfSongName",       EmbedTarget::Steinberg});
    
    // ID3 mappings
    m.push_back({"Description",     "ID3_COMMENT",       EmbedTarget::ID3});
    m.push_back({"Notes",           "ID3_SUBTITLE",      EmbedTarget::ID3});
    m.push_back({"Composer",        "ID3_COMPOSER",      EmbedTarget::ID3});
    m.push_back({"Publisher",       "ID3_PUBLISHER",     EmbedTarget::ID3});
    m.push_back({"TrackTitle",      "ID3_TITLE",         EmbedTarget::ID3});
    m.push_back({"Category",        "ID3_CONTENTTYPE",   EmbedTarget::ID3});
    m.push_back({"CDTitle",         "ID3_ALBUM",         EmbedTarget::ID3});
    m.push_back({"Artist",          "ID3_LEADARTIST",    EmbedTarget::ID3});
    m.push_back({"BPM",             "ID3_BPM",           EmbedTarget::ID3});
    m.push_back({"Library",         "ID3_BAND",          EmbedTarget::ID3});
    m.push_back({"Lyrics",          "ID3_UNSYNCEDLYRICS",EmbedTarget::ID3});
    m.push_back({"Key",             "ID3_INITIALKEY",    EmbedTarget::ID3});
    
    // LIST/INFO mappings
    m.push_back({"Description",     "ICMT",              EmbedTarget::LIST_INFO});
    m.push_back({"Category",        "IGNR",              EmbedTarget::LIST_INFO});
    m.push_back({"Artist",          "IART",              EmbedTarget::LIST_INFO});
    m.push_back({"CDTitle",         "IPRD",              EmbedTarget::LIST_INFO});
    m.push_back({"TrackTitle",      "INAM",              EmbedTarget::LIST_INFO});
    
    return m;
}
