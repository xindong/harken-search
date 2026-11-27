// SoundManager - 元数据字段体系模块
// MetadataFields.cpp - 字段注册、模板定义、验证实现
// Shared metadata field definitions for BWF/iXML/UCS-friendly workflows.

#include "MetadataFields.h"
#include <algorithm>
#include <cctype>
#include <regex>

// Static Data

static bool s_fieldInitialized = false;
static std::vector<FieldDef> s_allFields;
static std::unordered_map<std::string, int> s_fieldIndex;           // name -> index
static std::unordered_map<std::string, int> s_fieldLengthTable;     // name -> maxLength
static std::vector<DatabaseTemplateDef> s_templates;

// Field Registration

void MetadataFieldManager::registerField(const char* name, const char* display, const char* display_zh,
                                          FieldType type, int maxLen, bool indexed, bool filterable,
                                          const char* group, const char* desc) {
    FieldDef f;
    f.name = name;
    f.displayName = display;
    f.displayName_zh = display_zh;
    f.type = type;
    f.maxLength = maxLen;
    f.indexed = indexed;
    f.filterable = filterable;
    f.group = group;
    f.description = desc;
    
    s_fieldIndex[f.name] = (int)s_allFields.size();
    s_fieldLengthTable[f.name] = maxLen;
    s_allFields.push_back(std::move(f));
}

// Initialize

void MetadataFieldManager::initialize() {
    if (s_fieldInitialized) return;
    s_allFields.reserve(80);

// Basic / Common Fields
    registerField("Description",    "Description",      "描述",     FieldType::String, 1000, true,  false, "Basic", "Primary text description");
    registerField("Source",         "Source",            "来源",     FieldType::String, 64,   false, false, "Basic", "Original source");
    registerField("Category",      "Category",          "分类",     FieldType::String, 64,   true,  true,  "Basic", "Primary category");
    registerField("SubCategory",   "Sub Category",      "子分类",   FieldType::String, 64,   true,  true,  "Basic", "Sub-category");
    registerField("HarkenCategory","Harken Category",   "Harken 分类", FieldType::String, 256, false, false, "Harken", "AI-assigned Harken category stored in harken_meta");
    registerField("HarkenFunction","Harken Function",   "Harken 功能", FieldType::String, 256, false, false, "Harken", "AI-assigned Harken function stored in harken_meta");
    registerField("CatID",         "Category ID",       "分类ID",   FieldType::String, 12,   false, false, "Basic", "UCS Category ID code");
    registerField("CategoryFull",  "Category Full",     "完整分类", FieldType::String, 256,  false, false, "Basic", "Full category path");
    registerField("FXName",        "FX Name",           "效果名",   FieldType::String, 128,  true,  false, "Basic", "Sound effect name");
    registerField("Notes",         "Notes",             "备注",     FieldType::String, 255,  false, false, "Basic", "Additional notes");
    registerField("Keywords",      "Keywords",          "关键词",   FieldType::String, 1000, true,  false, "Basic", "Search keywords/tags");
    registerField("UserComments",  "User Comments",     "用户备注", FieldType::String, 255,  true,  false, "Basic", "User comments");
    registerField("URL",           "URL",               "网址",     FieldType::String, 512,  false, false, "Basic", "Reference URL");
    registerField("UserCategory",  "User Category",     "用户分类", FieldType::String, 256,  false, false, "Basic", "User-defined category");
    registerField("VendorCategory","Vendor Category",   "厂商分类", FieldType::String, 256,  false, false, "Basic", "Vendor-defined category");
    registerField("OpenTier",      "Open Tier",         "开放层级", FieldType::String, 256,  false, false, "Basic", "Open tier classification");

// Library / Album Fields
    registerField("Library",       "Library",           "库",       FieldType::String, 64,   true,  true,  "Library", "Sound library name");
    registerField("Volume",        "Volume",            "卷",       FieldType::String, 100,  false, true,  "Library", "Volume/disc number");
    registerField("CDTitle",       "CD Title",          "专辑名",   FieldType::String, 128,  true,  true,  "Library", "Album/CD title");
    registerField("CDDescription", "CD Description",    "专辑描述", FieldType::String, 512,  false, false, "Library", "Album/CD description");
    registerField("Manufacturer",  "Manufacturer",      "制造商",   FieldType::String, 64,   false, true,  "Library", "Library manufacturer");
    registerField("Track",         "Track",             "曲目号",   FieldType::String, 31,   false, false, "Library", "Track number");

// Music Fields
    registerField("TrackTitle",    "Track Title",       "曲名",     FieldType::String, 128,  true,  false, "Music", "Track/song title");
    registerField("Composer",      "Composer",          "作曲",     FieldType::String, 512,  true,  false, "Music", "Composer name(s)");
    registerField("Artist",        "Artist",            "艺术家",   FieldType::String, 128,  true,  true,  "Music", "Performing artist");
    registerField("Arranger",      "Arranger",          "编曲",     FieldType::String, 128,  false, false, "Music", "Arranger name");
    registerField("Conductor",     "Conductor",         "指挥",     FieldType::String, 128,  false, false, "Music", "Conductor name");
    registerField("Performer",     "Performer",         "演奏者",   FieldType::String, 128,  false, false, "Music", "Performer name");
    registerField("Publisher",     "Publisher",         "出版商",   FieldType::String, 512,  true,  true,  "Music", "Music publisher");
    registerField("BPM",           "BPM",               "节拍",     FieldType::String, 31,   false, false, "Music", "Beats per minute");
    registerField("Key",           "Key",               "调性",     FieldType::String, 15,   false, false, "Music", "Musical key");
    registerField("Tempo",         "Tempo",             "速度",     FieldType::String, 62,   false, false, "Music", "Tempo description");
    registerField("Mood",          "Mood",              "情绪",     FieldType::String, 255,  true,  true,  "Music", "Mood/atmosphere");
    registerField("Usage",         "Usage",             "用途",     FieldType::String, 128,  false, false, "Music", "Intended usage");
    registerField("FeaturedInstrument", "Instrument",   "乐器",     FieldType::String, 128,  false, true,  "Music", "Featured instrument");
    registerField("Version",       "Version",           "版本",     FieldType::String, 62,   false, true,  "Music", "Track version");
    registerField("Era",           "Era",               "年代",     FieldType::String, 128,  false, false, "Music", "Musical era/period");
    registerField("TrackYear",     "Year",              "年份",     FieldType::String, 10,   false, false, "Music", "Release year");
    registerField("ISRC",          "ISRC",              "ISRC",     FieldType::String, 16,   false, false, "Music", "International Standard Recording Code");
    registerField("ISWC",          "ISWC",              "ISWC",     FieldType::String, 11,   false, false, "Music", "International Standard Musical Work Code");
    registerField("Lyrics",        "Lyrics",            "歌词",     FieldType::Blob,   0,    false, false, "Music", "Song lyrics");

// Sound Design / Post Fields
    registerField("Show",          "Show",              "节目",     FieldType::String, 62,   true,  true,  "SFX", "Show/project name");
    registerField("ShowCategory",  "Show Category",     "节目分类", FieldType::String, 64,   false, false, "SFX", "Show-specific category");
    registerField("ShowSubCategory","Show SubCategory", "节目子分类",FieldType::String, 64,   false, false, "SFX", "Show-specific subcategory");
    registerField("Designer",      "Designer",          "设计师",   FieldType::String, 62,   true,  true,  "SFX", "Sound designer");
    registerField("Location",      "Location",          "位置",     FieldType::String, 128,  false, false, "SFX", "Recording location");
    registerField("Microphone",    "Microphone",        "麦克风",   FieldType::String, 62,   false, false, "SFX", "Microphone used");
    registerField("MicPerspective","Mic Perspective",   "拾音距离", FieldType::String, 128,  false, false, "SFX", "Microphone perspective/distance");
    registerField("RecType",       "Rec Type",          "录制类型", FieldType::String, 62,   false, false, "SFX", "Recording type");
    registerField("RecMedium",     "Rec Medium",        "录制介质", FieldType::String, 62,   false, false, "SFX", "Recording medium");
    registerField("ShortID",       "Short ID",          "短ID",     FieldType::String, 31,   true,  false, "SFX", "Short identifier code");
    registerField("LongID",        "Long ID",           "长ID",     FieldType::String, 128,  false, false, "SFX", "Long identifier code");

// Production / Dialog Fields
    registerField("Scene",         "Scene",             "场景",     FieldType::String, 31,   true,  true,  "Production", "Scene number/name");
    registerField("Take",          "Take",              "条",       FieldType::String, 31,   false, false, "Production", "Take number");
    registerField("Tape",          "Tape",              "磁带",     FieldType::String, 31,   false, true,  "Production", "Tape/reel identifier");

// GPS / Location Fields
    registerField("ShootDate",     "Shoot Date",        "拍摄日期", FieldType::DateTime, 0,  false, false, "Location", "Shoot/recording date");
    registerField("GPSLat",        "GPS Latitude",      "GPS纬度",  FieldType::Real, 0,      false, false, "Location", "GPS latitude");
    registerField("GPSLon",        "GPS Longitude",     "GPS经度",  FieldType::Real, 0,      false, false, "Location", "GPS longitude");
    registerField("GPSAlt",        "GPS Altitude",      "GPS海拔",  FieldType::Real, 0,      false, false, "Location", "GPS altitude");

// iXML Fields
    registerField("ixmlProject",        "iXML Project",        "iXML项目",    FieldType::String, 64,  false, false, "iXML");
    registerField("ixmlNote",           "iXML Note",           "iXML备注",    FieldType::String, 64,  false, false, "iXML");
    registerField("ixmlFileUID",        "iXML File UID",       "iXML文件UID", FieldType::String, 31,  false, false, "iXML");
    registerField("ixmlOriginalFilename","iXML Orig Filename", "iXML原文件名",FieldType::String, 64,  false, false, "iXML");
    registerField("ixmlParentFilename", "iXML Parent File",    "iXML父文件",  FieldType::String, 64,  false, false, "iXML");
    registerField("ixmlParentUID",      "iXML Parent UID",     "iXML父UID",   FieldType::String, 64,  false, false, "iXML");
    registerField("ixmlCurrentSpeed",   "iXML Speed",          "iXML速度",    FieldType::String, 31,  false, false, "iXML");
    registerField("ixmlMasterSpeed",    "iXML Master Speed",   "iXML主速度",  FieldType::String, 31,  false, false, "iXML");
    registerField("ixmlSpeedNote",      "iXML Speed Note",     "iXML速度注",  FieldType::String, 31,  false, false, "iXML");
    registerField("ixmlTimeCodeFlag",   "iXML TC Flag",        "iXML时码标",  FieldType::String, 31,  false, false, "iXML");
    registerField("ixmlTimeCodeRate",   "iXML TC Rate",        "iXML时码率",  FieldType::String, 31,  false, false, "iXML");
    registerField("ixmlTrackLayout",    "iXML Track Layout",   "iXML声道布局",FieldType::String, 256, false, false, "iXML");

// Broadcast WAV (BEXT)
    registerField("BWDescription",     "BW Description",      "BW描述",       FieldType::String, 256, false, false, "BEXT");
    registerField("BWOriginator",      "BW Originator",       "BW来源",       FieldType::String, 32,  false, false, "BEXT");
    registerField("BWOriginatorRef",   "BW Originator Ref",   "BW来源引用",   FieldType::String, 32,  false, false, "BEXT");
    registerField("BWDate",            "BW Date",             "BW日期",       FieldType::String, 10,  false, false, "BEXT");
    registerField("BWTime",            "BW Time",             "BW时间",       FieldType::String, 8,   false, false, "BEXT");
    registerField("BWSMPTEStamp",      "BW SMPTE Stamp",      "BW SMPTE时间", FieldType::String, 31,  false, false, "BEXT");

// Internal / System Fields
    registerField("_TrackColour",  "Track Colour",     "轨道颜色", FieldType::String, 128,  false, false, "System", "Track color indicator");
    registerField("_FlatCategory", "Flat Category",    "扁平分类", FieldType::String, 512,  false, false, "System", "Flattened category for indexing");
    registerField("Rating",        "Rating",           "评分",     FieldType::Integer, 0,    false, false, "System", "User rating 0-5");

    // Build templates
    buildTemplates();
    s_fieldInitialized = true;
}

// Build Templates

void MetadataFieldManager::buildTemplates() {
    // --- Music Template ---
    {
        DatabaseTemplateDef t;
        t.type = DatabaseTemplate::Music;
        t.name = "Music";
        t.description = "Music production and licensing";
        t.fieldNames = {"Description","Source","Category","SubCategory","Notes","Show","Library",
                        "Keywords","Manufacturer","Composer","Arranger","Artist","Conductor",
                        "Publisher","Performer","Mood","Usage","ShortID","LongID","BPM","ISRC","ISWC",
                        "Tempo","TrackYear","Key","TrackTitle","CDTitle","CDDescription",
                        "FeaturedInstrument","Version","UserComments","URL","Lyrics"};
        t.indexedFields = {"Description","Category","SubCategory","Library","CDTitle",
                           "Composer","Artist","TrackTitle","Keywords","UserComments"};
        t.filterBrowser = {"Library","Category","CDTitle"};
        t.rdbFields = {"Artist","Arranger","BPM","Category","CDTitle","Composer","Conductor",
                        "FeaturedInstrument","Library","Manufacturer","Mood","Performer",
                        "Publisher","Show","SubCategory","TrackYear","Usage","Volume","Key"};
        s_templates.push_back(std::move(t));
    }

    // --- SoundEffects Template ---
    {
        DatabaseTemplateDef t;
        t.type = DatabaseTemplate::SoundEffects;
        t.name = "Sound Effects";
        t.description = "Sound effects and field recordings";
        t.fieldNames = {"Description","Source","Category","SubCategory","CatID","CategoryFull",
                        "ShowCategory","ShowSubCategory","FXName","Notes","Show","Library",
                        "RecType","ShortID","LongID","RecMedium","Keywords","Location",
                        "Microphone","MicPerspective","Manufacturer","Designer","Scene","Take","Tape",
                        "TrackYear","Era","UserComments","UserCategory","VendorCategory","OpenTier",
                        "CDTitle","ShootDate","GPSLat","GPSLon","GPSAlt","URL"};
        t.indexedFields = {"Description","Category","SubCategory","Show","ShortID","Library",
                           "Notes","Keywords","Designer","FXName","UserComments","CDTitle"};
        t.filterBrowser = {"Library","Category","SubCategory"};
        t.rdbFields = {"Category","SubCategory","Show","Library","Manufacturer","Designer",
                        "CDTitle","Microphone","MicPerspective","ShortID","Scene","Take",
                        "ChannelLayout","Location","CategoryFull","CatID","ShowCategory",
                        "ShowSubCategory","UserCategory","VendorCategory","OpenTier"};
        s_templates.push_back(std::move(t));
    }

    // --- Dialog Template ---
    {
        DatabaseTemplateDef t;
        t.type = DatabaseTemplate::Dialog;
        t.name = "Dialog";
        t.description = "Dialog and production audio";
        t.fieldNames = {"Description","Source","Category","SubCategory","Notes","Show","Library",
                        "RecType","ShortID","LongID","RecMedium","Keywords","Location",
                        "Microphone","Manufacturer","Designer","Scene","Take","Tape","UserComments","URL"};
        t.indexedFields = {"Description","Category","SubCategory","Show","ShortID","Library",
                           "Notes","Keywords","UserComments"};
        t.filterBrowser = {"Library","Tape","Scene"};
        t.rdbFields = {"Category","SubCategory","Show","Library","Manufacturer","Designer",
                        "Scene","Tape","Location"};
        s_templates.push_back(std::move(t));
    }

    // --- Music and FX Template ---
    {
        DatabaseTemplateDef t;
        t.type = DatabaseTemplate::MusicAndFX;
        t.name = "Music and FX";
        t.description = "Combined music and sound effects";
        t.fieldNames = {"Description","Source","Category","SubCategory","CatID","CategoryFull",
                        "ShowCategory","ShowSubCategory","FXName","Notes","Show","Library",
                        "RecType","ShortID","LongID","RecMedium","Keywords","Location",
                        "Microphone","MicPerspective","Manufacturer","Composer","Arranger",
                        "Artist","Conductor","Publisher","Performer","Designer","Scene","Take","Tape",
                        "Mood","Usage","BPM","ISRC","ISWC","Tempo","TrackYear","Key",
                        "TrackTitle","CDTitle","CDDescription","FeaturedInstrument","Version",
                        "Era","UserComments","UserCategory","VendorCategory","OpenTier",
                        "ShootDate","GPSLat","GPSLon","GPSAlt","URL","Lyrics"};
        t.indexedFields = {"Description","Category","SubCategory","Library","Show","CDTitle",
                           "Composer","Artist","Designer","TrackTitle","Keywords","ShortID",
                           "FXName","UserComments"};
        t.filterBrowser = {"Library","Category","CDTitle"};
        t.rdbFields = {"Artist","Arranger","BPM","Category","CDTitle","Composer","Designer",
                        "Conductor","FeaturedInstrument","Library","Manufacturer","Mood",
                        "Performer","Publisher","Show","SubCategory","TrackYear","Usage","Volume","Key",
                        "Microphone","MicPerspective","ShortID","Scene","Take",
                        "Location","CategoryFull","CatID","ShowCategory","ShowSubCategory",
                        "UserCategory","VendorCategory","OpenTier"};
        s_templates.push_back(std::move(t));
    }

    // --- Universal Template ---
    {
        DatabaseTemplateDef t;
        t.type = DatabaseTemplate::Universal;
        t.name = "Universal";
        t.description = "All fields available";
        // Include everything
        for (const auto& f : s_allFields) {
            if (f.group != "System" && f.group != "iXML" && f.group != "BEXT") {
                t.fieldNames.push_back(f.name);
            }
        }
        t.indexedFields = {"Description","Category","SubCategory","Library","CDTitle",
                           "Composer","Artist","TrackTitle","Keywords","Mood","UserComments",
                           "Usage","FeaturedInstrument","Tempo","TrackYear","Version","FXName",
                           "Designer","Show"};
        t.filterBrowser = {"Library","Category","CDTitle","Volume"};
        t.rdbFields = {"Artist","Arranger","BPM","CDTitle","Composer","Conductor","Library",
                        "Manufacturer","Mood","Performer","Publisher","Show","SubCategory",
                        "TrackYear","Volume"};
        s_templates.push_back(std::move(t));
    }
}

// Query Functions

const std::vector<FieldDef>& MetadataFieldManager::getAllFields() {
    return s_allFields;
}

const FieldDef* MetadataFieldManager::getField(const std::string& name) {
    auto it = s_fieldIndex.find(name);
    if (it != s_fieldIndex.end() && it->second < (int)s_allFields.size()) {
        return &s_allFields[it->second];
    }
    return nullptr;
}

std::vector<const FieldDef*> MetadataFieldManager::getFieldsByGroup(const std::string& group) {
    std::vector<const FieldDef*> result;
    for (const auto& f : s_allFields) {
        if (f.group == group) result.push_back(&f);
    }
    return result;
}

std::vector<std::string> MetadataFieldManager::getStringFieldNames() {
    std::vector<std::string> result;
    for (const auto& f : s_allFields) {
        if (f.type == FieldType::String && f.group != "System") {
            result.push_back(f.name);
        }
    }
    return result;
}

const DatabaseTemplateDef* MetadataFieldManager::getTemplate(DatabaseTemplate type) {
    for (const auto& t : s_templates) {
        if (t.type == type) return &t;
    }
    return nullptr;
}

std::vector<DatabaseTemplate> MetadataFieldManager::getAllTemplateTypes() {
    return {DatabaseTemplate::Music, DatabaseTemplate::SoundEffects,
            DatabaseTemplate::Dialog, DatabaseTemplate::MusicAndFX,
            DatabaseTemplate::Universal};
}

std::string MetadataFieldManager::getTemplateName(DatabaseTemplate type) {
    auto* t = getTemplate(type);
    return t ? t->name : "Unknown";
}

const std::unordered_map<std::string, int>& MetadataFieldManager::getFieldLengthTable() {
    return s_fieldLengthTable;
}

// Validators

ISRCValidation MetadataFieldManager::validateISRC(const std::string& isrc) {
    ISRCValidation result;
    if (isrc.empty()) {
        result.error = "ISRC is empty";
        return result;
    }
    if (isrc.length() < 12 || isrc.length() > 15) {
        result.error = "ISRC must be 12 characters (or 15 with hyphens)";
        return result;
    }
    
    // Remove hyphens
    std::string cleaned;
    for (char c : isrc) {
        if (c != '-') cleaned += c;
    }
    
    if (cleaned.length() != 12) {
        result.error = "ISRC must be exactly 12 characters after removing hyphens";
        return result;
    }
    
    // Pattern: CC-XXX-YY-NNNNN -> CCXXXYYNNNNN
    // CC = country (alpha), XXX = registrant (alphanumeric), YY = year (digits), NNNNN = designation (digits)
    // Simplified: 2 alpha + 3 alphanumeric + 7 digits
    bool valid = true;
    if (!std::isalpha((unsigned char)cleaned[0]) || !std::isalpha((unsigned char)cleaned[1])) valid = false;
    for (int i = 2; i < 5; i++) {
        if (!std::isalnum((unsigned char)cleaned[i])) valid = false;
    }
    for (int i = 5; i < 12; i++) {
        if (!std::isdigit((unsigned char)cleaned[i])) valid = false;
    }
    
    if (!valid) {
        result.error = "Invalid ISRC format (expected: CC-XXX-YY-NNNNN)";
        return result;
    }
    
    result.valid = true;
    result.cleaned = cleaned;
    return result;
}

bool MetadataFieldManager::validateBPM(const std::string& bpm) {
    if (bpm.empty()) return true; // empty is valid
    try {
        float val = std::stof(bpm);
        return val > 0 && val < 999;
    } catch (...) {
        return false;
    }
}

bool MetadataFieldManager::validateYear(const std::string& year) {
    if (year.empty()) return true;
    if (year.length() != 4) return false;
    for (char c : year) {
        if (!std::isdigit((unsigned char)c)) return false;
    }
    int y = std::stoi(year);
    return y >= 1900 && y <= 2099;
}

bool MetadataFieldManager::isInitialized() {
    return s_fieldInitialized;
}

int MetadataFieldManager::getFieldCount() {
    return (int)s_allFields.size();
}
