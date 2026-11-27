#include "MetadataReader.h"
#include "platform/filesystem.h"
#include "platform/metadata_legacy_tag_content.h"
#include "platform/text_encoding.h"
#include "Id3CommentText.h"
#include <wavpack.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <memory>
#include <string_view>

namespace fs = std::filesystem;

// Safety caps – prevent OOM when parsing corrupted files.
// Any single tag/chunk/block that claims to be bigger than this is rejected.

static constexpr size_t kMaxTagDataBytes   = 32 * 1024 * 1024;  // 32 MB  (ID3 tags, iXML, XMP)
static constexpr size_t kMaxChunkDataBytes = 64 * 1024 * 1024;  // 64 MB  (BWF bext, RIFF chunks)
static constexpr size_t kMaxCoverDataBytes = 32 * 1024 * 1024;  // 32 MB  (cover art payload)
static constexpr int    kMaxChunkIter      = 10000;              // max chunk iterations per file
static constexpr int kMaxWavPackTextTagBytes = 1024 * 1024;

// Private compatibility adapter: the parser keeps its existing stream-shaped
// control flow while file ownership stays in the opaque filesystem PAL.
class MetadataFileReader {
public:
    MetadataFileReader() = default;
    MetadataFileReader(const MetadataFileReader&) = delete;
    MetadataFileReader& operator=(const MetadataFileReader&) = delete;
    MetadataFileReader(MetadataFileReader&&) noexcept = default;
    MetadataFileReader& operator=(MetadataFileReader&&) noexcept = default;

    bool Open(const std::string& filePath) { return stream_.Open(filePath, true); }
    bool is_open() const { return stream_.IsOpen(); }
    void read(char* destination, std::streamsize bytes) {
        lastRead_ = static_cast<std::streamsize>(stream_.Read(destination, static_cast<size_t>(bytes)));
    }
    std::streamsize gcount() const { return lastRead_; }
    void seekg(std::streamoff offset, std::ios_base::seekdir origin) {
        stream_.Clear();
        const auto palOrigin = origin == std::ios_base::beg
            ? Platform::FileSystem::StreamSeekOrigin::Begin
            : (origin == std::ios_base::cur
                ? Platform::FileSystem::StreamSeekOrigin::Current
                : Platform::FileSystem::StreamSeekOrigin::End);
        stream_.Seek(static_cast<int64_t>(offset), palOrigin);
    }
    void seekg(std::streamoff offset) { seekg(offset, std::ios_base::beg); }
    std::streampos tellg() const { return std::streampos(stream_.Tell()); }
    bool good() const { return stream_.Good(); }
    void clear() { stream_.Clear(); }

private:
    Platform::FileSystem::StreamReader stream_;
    std::streamsize lastRead_ = 0;
};

static MetadataFileReader openFile(const std::string& filePath) {
    MetadataFileReader file;
    file.Open(filePath);
    return file;
}

using WavPackContextHandle = std::unique_ptr<WavpackContext, decltype(&WavpackCloseFile)>;

static WavPackContextHandle OpenWavPackTags(const std::string& filePath) {
    WavPackContextHandle context(
        WavpackOpenFileInput(filePath.c_str(), nullptr, OPEN_TAGS | OPEN_FILE_UTF8, 0),
        WavpackCloseFile);
    if (context && !(WavpackGetMode(context.get()) & MODE_APETAG)) context.reset();
    return context;
}

static std::string ReadWavPackTextTag(WavpackContext* context,
                                     std::initializer_list<const char*> aliases) {
    for (const auto* name : aliases) {
        const int size = WavpackGetTagItem(context, name, nullptr, 0);
        if (size <= 0 || size > kMaxWavPackTextTagBytes) continue;
        std::string value(static_cast<size_t>(size) + 1, '\0');
        if (WavpackGetTagItem(context, name, value.data(), size + 1) != size) continue;
        value.resize(static_cast<size_t>(size));

        std::string joined;
        for (size_t offset = 0; offset < value.size();) {
            const size_t separator = value.find('\0', offset);
            const size_t end = separator == std::string::npos ? value.size() : separator;
            const size_t first = value.find_first_not_of(" \t\r\n", offset);
            if (first < end) {
                const size_t last = value.find_last_not_of(" \t\r\n", end - 1);
                if (!joined.empty()) joined += "; ";
                joined.append(value, first, last - first + 1);
            }
            offset = end + 1;
        }
        if (!joined.empty()) return joined;
    }
    return {};
}

static bool ReadWavPackMetadata(const std::string& filePath, AudioMetadata& metadata) {
    const auto context = OpenWavPackTags(filePath);
    if (!context) return false;

    AudioMetadata tags;
    bool found = false;
    auto text = [&](std::string& field, std::initializer_list<const char*> names) {
        field = ReadWavPackTextTag(context.get(), names);
        found |= !field.empty();
    };
    auto number = [&](int& field, std::initializer_list<const char*> names, int max) {
        const auto value = ReadWavPackTextTag(context.get(), names);
        int parsed = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec == std::errc{} && parsed > 0 && parsed <= max) {
            field = parsed;
            found = true;
        }
    };
    auto gain = [&](float& field, const char* name) {
        const auto value = ReadWavPackTextTag(context.get(), {name});
        char* end = nullptr;
        const float parsed = std::strtof(value.c_str(), &end);
        if (end != value.c_str() && std::isfinite(parsed)) {
            field = parsed;
            found = true;
        }
    };
    text(tags.title, {"Title"});
    text(tags.artist, {"Artist"});
    text(tags.album, {"Album"});
    text(tags.albumArtist, {"Album Artist", "AlbumArtist"});
    text(tags.genre, {"Genre"});
    text(tags.comment, {"Comment"});
    text(tags.description, {"Description", "Comment"});
    text(tags.keywords, {"Keywords", "Tags"});
    text(tags.category, {"Category", "Subject"});
    text(tags.subcategory, {"Subcategory"});
    text(tags.library, {"Library", "Source"});
    text(tags.designer, {"Designer", "Engineer"});
    text(tags.manufacturer, {"Manufacturer"});
    text(tags.notes, {"Notes"});
    text(tags.url, {"URL", "Contact"});
    text(tags.copyright, {"Copyright"});
    text(tags.software, {"Software", "Tool Name"});
    text(tags.instruments, {"Instruments", "Instrument"});
    number(tags.year, {"Year", "Date"}, 9999);
    number(tags.track, {"Track", "TrackNumber"}, 65535);
    number(tags.disc, {"Disc", "DiscNumber"}, 65535);
    gain(tags.replayGainTrack, "REPLAYGAIN_TRACK_GAIN");
    gain(tags.replayGainAlbum, "REPLAYGAIN_ALBUM_GAIN");
    if (!found) return false;
    metadata = std::move(tags);
    return true;
}

static bool ReadWavPackAlbumArt(const std::string& filePath, AlbumArt& art) {
    const auto context = OpenWavPackTags(filePath);
    if (!context) return false;

    for (const char* name : {"Cover Art (Front)", "Cover Art (Back)"}) {
        const int size = WavpackGetBinaryTagItem(context.get(), name, nullptr, 0);
        if (size <= 0 || static_cast<size_t>(size) > kMaxCoverDataBytes) continue;
        std::vector<uint8_t> payload(static_cast<size_t>(size));
        if (WavpackGetBinaryTagItem(context.get(), name, reinterpret_cast<char*>(payload.data()), size) != size)
            continue;
        const auto separator = std::find(payload.begin(), payload.end(), uint8_t{0});
        if (separator == payload.begin() || separator == payload.end()) continue;
        const auto image = separator + 1;
        const size_t length = static_cast<size_t>(payload.end() - image);
        const auto* bytes = payload.data() + (image - payload.begin());
        AlbumArt decoded;
        if (length >= 33 && std::memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0 &&
            std::memcmp(bytes + 12, "IHDR", 4) == 0) {
            decoded.mimeType = "image/png";
        } else if (length >= 4 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) {
            decoded.mimeType = "image/jpeg";
        } else {
            continue;
        }
        decoded.data.assign(image, payload.end());
        art = std::move(decoded);
        return true;
    }
    return false;
}

std::string MetadataReader::trimString(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r\0");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\n\r\0");
    return str.substr(start, end - start + 1);
}

std::string MetadataReader::readFixedString(const char* data, size_t maxLen) {
    std::string result;
    for (size_t i = 0; i < maxLen && data[i] != '\0'; i++) {
        result += data[i];
    }
    return trimString(result);
}

uint32_t MetadataReader::readLE32(const uint8_t* data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

uint32_t MetadataReader::readBE32(const uint8_t* data) {
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

uint32_t MetadataReader::readSyncSafeInt(const uint8_t* data) {
    return ((data[0] & 0x7F) << 21) | ((data[1] & 0x7F) << 14) |
           ((data[2] & 0x7F) << 7) | (data[3] & 0x7F);
}

bool MetadataReader::readMetadata(const std::string& filePath, AudioMetadata& metadata,
                                  bool* customTagsRead) {
    bool localCustomTags = false;
    bool* customTagsOut = customTagsRead ? customTagsRead : &localCustomTags;
    *customTagsOut = false;
    metadata.customTextTagsRead = false;
    try {
    // Determine format from extension
    std::string ext;
    std::wstring wpath = Platform::TextEncoding::Utf8ToWidePermissive(filePath);
    if (!filePath.empty() && wpath.empty()) return false;
#ifdef _WIN32
    fs::path fsPath(wpath);
    std::wstring wext = fsPath.extension().wstring();
    ext = Platform::TextEncoding::WideToUtf8Permissive(wext);
#else
    ext = fs::path(filePath).extension().string();
#endif
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    bool found = false;
    if (ext == ".mp3") {
        found = readID3v2(filePath, metadata, true, customTagsOut);
        if (!found) found = readID3v1(filePath, metadata);
    } else if (ext == ".wav" || ext == ".wave") {
        found = readBWF(filePath, metadata);
    } else if (ext == ".ogg" || ext == ".flac") {
        found = readVorbisComment(filePath, metadata, customTagsOut);
    } else if (ext == ".m4a" || ext == ".aac" || ext == ".mp4" || ext == ".m4p") {
        found = readM4A(filePath, metadata);
    } else if (ext == ".aiff" || ext == ".aif") {
        found = readID3v2(filePath, metadata);
    } else if (ext == ".wv") {
        found = ReadWavPackMetadata(filePath, metadata);
    }
    // Report whether the custom-tag container of this format was parsed fully,
    // not whether the format happens to be MP3.
    metadata.customTextTagsRead = *customTagsOut;
    return found;
    } catch (...) {
// Tolerate any exception (including SEH with /EHa) from corrupted files
    }
    return false;
}

bool MetadataReader::readID3v1(const std::string& filePath, AudioMetadata& metadata) {
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    // ID3v1 tag is at the last 128 bytes
    file.seekg(-128, std::ios::end);
    if (!file.good()) return false;

    char tag[128];
    file.read(tag, 128);
    if (file.gcount() != 128) return false;

    // Check for TAG header
    if (tag[0] != 'T' || tag[1] != 'A' || tag[2] != 'G') {
        return false;
    }

    // Read fields
    metadata.title = readFixedString(tag + 3, 30);
    metadata.artist = readFixedString(tag + 33, 30);
    metadata.album = readFixedString(tag + 63, 30);

    // Year (4 chars)
    std::string yearStr = readFixedString(tag + 93, 4);
    if (!yearStr.empty()) {
        try { metadata.year = std::stoi(yearStr); } catch (...) {}
    }

    // Comment (30 chars, or 28 + track number in ID3v1.1)
    if (tag[125] == 0 && tag[126] != 0) {
        // ID3v1.1 - track number present
        metadata.comment = readFixedString(tag + 97, 28);
        metadata.track = static_cast<uint8_t>(tag[126]);
    } else {
        metadata.comment = readFixedString(tag + 97, 30);
    }

    // Genre (1 byte index)
    static const char* genres[] = {
        "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
        "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
        "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
        "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient",
        "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical",
        "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
        "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative",
        "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
        "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
        "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap",
        "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
        "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
        "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
        "Hard Rock"
    };
    uint8_t genreIndex = static_cast<uint8_t>(tag[127]);
    if (genreIndex < sizeof(genres) / sizeof(genres[0])) {
        metadata.genre = genres[genreIndex];
    }

    return metadata.hasData();
}

using Platform::Metadata::LegacyTagContent::Utf16ByteOrder;

static std::string DecodeId3Text(uint8_t encoding, std::string_view bytes, Utf16ByteOrder& order) {
    if (encoding == 3) return std::string(bytes);
    if (encoding == 0) return Platform::Metadata::LegacyTagContent::DecodeId3v2Encoding0(bytes);
    if (encoding != 1 && encoding != 2) return {};

    if (bytes.substr(0, 2) == "\xFF\xFE") {
        order = Utf16ByteOrder::LittleEndian;
        bytes.remove_prefix(2);
    } else if (bytes.substr(0, 2) == "\xFE\xFF") {
        order = Utf16ByteOrder::BigEndian;
        bytes.remove_prefix(2);
    }
    return Platform::Metadata::LegacyTagContent::DecodeUtf16(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), order);
}

static size_t FindId3TextSeparator(std::string_view bytes, size_t width) {
    if (width == 1) return bytes.find('\0');
    for (size_t offset = 0; offset + 1 < bytes.size(); offset += 2) {
        if (bytes[offset] == '\0' && bytes[offset + 1] == '\0') return offset;
    }
    return std::string_view::npos;
}

static void StripId3TextTerminators(std::string& text) {
    while (!text.empty() && text.back() == '\0') text.pop_back();
}

bool MetadataReader::readID3v2(const std::string& filePath, AudioMetadata& metadata,
                               bool useV1Fallback, bool* customTagsRead) {
    if (customTagsRead) *customTagsRead = false;
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    // Read ID3v2 header (10 bytes)
    uint8_t header[10];
    file.read(reinterpret_cast<char*>(header), 10);
    if (file.gcount() != 10) return false;

    // Check for ID3 header
    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        if (customTagsRead) *customTagsRead = true;
        return false;
    }

    uint8_t majorVersion = header[3];
    // uint8_t minorVersion = header[4];
    // uint8_t flags = header[5];
    uint32_t tagSize = readSyncSafeInt(header + 6);

    if (majorVersion < 2 || majorVersion > 4) {
        return false;
    }
    bool customTagsComplete = (header[5] & 0xC0) == 0 &&
        ((header[6] | header[7] | header[8] | header[9]) & 0x80) == 0;
    if (tagSize == 0) {
        if (customTagsRead) *customTagsRead = customTagsComplete;
        return false;
    }

// Guard against corrupted/unreasonable tag sizes.
    if (tagSize > kMaxTagDataBytes) return false;

    // Read tag data
    std::vector<uint8_t> tagData(tagSize);
    file.read(reinterpret_cast<char*>(tagData.data()), tagSize);
    if (static_cast<uint32_t>(file.gcount()) != tagSize) return false;
    if (majorVersion == 2 && useV1Fallback) readID3v1(filePath, metadata);

    // Parse frames
    size_t pos = 0;
    const size_t idLength = majorVersion >= 3 ? 4 : 3;
    const size_t headerLength = majorVersion >= 3 ? 10 : 6;
    while (pos + headerLength < tagSize) {
        const auto* frame = tagData.data() + pos;
        if (frame[0] == 0) break;
        const std::string_view id(reinterpret_cast<const char*>(frame), idLength);
        const auto* sizeBytes = frame + idLength;

        uint32_t frameSize;
        if (majorVersion >= 4) {
            frameSize = readSyncSafeInt(sizeBytes);
        } else if (majorVersion == 3) {
            frameSize = readBE32(sizeBytes);
        } else {
            frameSize = (sizeBytes[0] << 16) | (sizeBytes[1] << 8) | sizeBytes[2];
        }
        pos += headerLength;
        if (frameSize == 0 || frameSize > tagSize - pos) {
            customTagsComplete = false;
            break;
        }

        const uint8_t encoding = tagData[pos];
        const std::string_view body(
            reinterpret_cast<const char*>(tagData.data() + pos + 1), frameSize - 1);
        pos += frameSize;
        if (id == "COMM" || id == "COM") {
            metadata.comment = trimString(Id3CommentText::Decode(encoding, body));
            continue;
        }

        auto order = encoding == 2 ? Utf16ByteOrder::BigEndian : Utf16ByteOrder::LittleEndian;
        if (id == "TXXX" || id == "TXX") {
            if (encoding > 3) {
                customTagsComplete = false;
                continue;
            }
            const size_t width = encoding == 1 || encoding == 2 ? 2 : 1;
            const size_t separator = FindId3TextSeparator(body, width);
            if (separator == std::string_view::npos) {
                customTagsComplete = false;
                continue;
            }

            auto description = DecodeId3Text(encoding, body.substr(0, separator), order);
            auto value = DecodeId3Text(encoding, body.substr(separator + width), order);
            StripId3TextTerminators(value);

            auto alias = trimString(description);
            for (char& letter : alias) {
                if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 'a' + 'A');
            }
            if (alias == "DESCRIPTION") {
                metadata.description = trimString(value);
            } else if (alias == "KEYWORDS") {
                metadata.keywords = trimString(value);
            } else if (alias == "REPLAYGAIN_TRACK_GAIN") {
                try { metadata.replayGainTrack = std::stof(value); } catch (...) {}
            } else if (alias == "REPLAYGAIN_ALBUM_GAIN") {
                try { metadata.replayGainAlbum = std::stof(value); } catch (...) {}
            } else if (alias == "INSTRUMENTS" || alias == "INSTUMENTS" ||
                       alias == "INSTRUMENT" || alias == "INSTUMENT") {
                if (metadata.instruments.empty()) metadata.instruments = trimString(value);
            }
            metadata.customTextTags.insert_or_assign(std::move(description), std::move(value));
            continue;
        }

        auto content = DecodeId3Text(encoding, body, order);
        std::string* commonField = nullptr;
        if (id == "TCOM" || id == "TCM") commonField = &metadata.composer;
        else if (id == "TBPM" || id == "TBP") commonField = &metadata.bpm;
        else if (id == "TKEY" || id == "TKE") commonField = &metadata.musicalKey;
        if (commonField) {
            StripId3TextTerminators(content);
            *commonField = trimString(content);
            continue;
        }
        content = trimString(content);

        // Map frame ID to metadata field
        if (id == "TIT2" || id == "TT2") {
            metadata.title = content;
        } else if (id == "TPE1" || id == "TP1") {
            metadata.artist = content;
        } else if (id == "TALB" || id == "TAL") {
            metadata.album = content;
        } else if (id == "TYER" || id == "TYE" || id == "TDRC") {
            try { metadata.year = std::stoi(content.substr(0, 4)); } catch (...) {}
        } else if (id == "TRCK" || id == "TRK") {
            try { metadata.track = std::stoi(content); } catch (...) {}
        } else if (id == "TPE2" || id == "TP2") {
            metadata.albumArtist = content;
        } else if (id == "TPOS" || id == "TPA") {
            try { metadata.disc = std::stoi(content); } catch (...) {}
        } else if (id == "TCON" || id == "TCO") {
            metadata.genre = content;
        }
    }

    if (customTagsRead) {
        *customTagsRead = customTagsComplete &&
            std::all_of(tagData.begin() + pos, tagData.end(), [](uint8_t byte) { return byte == 0; });
    }
    if (metadata.hasData()) return true;
    if (!useV1Fallback) return false;
    const bool foundExtended = metadata.year > 0 || !metadata.composer.empty() ||
        !metadata.bpm.empty() || !metadata.musicalKey.empty() || !metadata.customTextTags.empty();
    if (foundExtended && majorVersion >= 3) {
        const int id3v2Year = metadata.year;
        readID3v1(filePath, metadata);
        if (id3v2Year > 0) metadata.year = id3v2Year;
    }
    return foundExtended;
}

bool MetadataReader::readBWF(const std::string& filePath, AudioMetadata& metadata) {
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    // Read RIFF header
    char riffHeader[12];
    file.read(riffHeader, 12);
    if (file.gcount() != 12) return false;

    // Check RIFF/WAVE header
    if (memcmp(riffHeader, "RIFF", 4) != 0 || memcmp(riffHeader + 8, "WAVE", 4) != 0) {
        return false;
    }

    // Get file size for bounds checking
    file.seekg(0, std::ios::end);
    std::streamsize bwfFileSize = file.tellg();
    file.seekg(12);  // back to after RIFF header

    // Search for bext chunk (with iteration cap to avoid infinite loop on corrupted files)
    int chunkIter = 0;
    while (file.good() && chunkIter++ < kMaxChunkIter) {
        char chunkId[4];
        uint8_t chunkSizeData[4];

        auto curPos = file.tellg();
        if (curPos < 0 || curPos >= bwfFileSize) break; // past EOF

        file.read(chunkId, 4);
        file.read(reinterpret_cast<char*>(chunkSizeData), 4);

        if (file.gcount() != 4) break;

        uint32_t chunkSize = readLE32(chunkSizeData);

        // Reject obviously corrupt chunk sizes
        if (chunkSize > kMaxChunkDataBytes || ((std::streamsize)curPos + 8 + (std::streamsize)chunkSize) > bwfFileSize) {
            break; // corrupted file – stop parsing
        }

        if (memcmp(chunkId, "bext", 4) == 0) {
            // BWF bext chunk found (extended BWDescription, BWOriginator, etc.)
            if (chunkSize >= 602) {
                std::vector<char> bextData(chunkSize);
                file.read(bextData.data(), chunkSize);

                // bext structure (EBU Tech 3285):
                // 0-255: Description (256 chars) -> BWDescription
                // 256-287: Originator (32 chars) -> BWOriginator
                // 288-319: OriginatorReference (32 chars) -> BWOriginatorRef
                // 320-329: OriginationDate (10 chars, YYYY-MM-DD)
                // 330-337: OriginationTime (8 chars, HH:MM:SS)
                // 338-345: TimeReferenceLow (4 bytes) + TimeReferenceHigh (4 bytes) -> BWTimeStamp
                // 346-347: Version (2 bytes)
                // 348-411: UMID (64 bytes)
                // 412-601: Reserved (190 bytes)
                // 602+: CodingHistory (variable)

                metadata.bwDescription = readFixedString(bextData.data(), 256);
                metadata.originator = readFixedString(bextData.data() + 256, 32);
                metadata.originatorRef = readFixedString(bextData.data() + 288, 32);
                metadata.originationDate = readFixedString(bextData.data() + 320, 10);
                metadata.originationTime = readFixedString(bextData.data() + 330, 8);
                
                // Read TimeReference (64-bit sample count)
                if (chunkSize >= 346) {
                    uint32_t timeLow = readLE32(reinterpret_cast<uint8_t*>(bextData.data() + 338));
                    uint32_t timeHigh = readLE32(reinterpret_cast<uint8_t*>(bextData.data() + 342));
                    metadata.bwTimeStamp = ((uint64_t)timeHigh << 32) | timeLow;
                }
                
                // Read CodingHistory if present
                if (chunkSize > 602) {
                    metadata.codingHistory = readFixedString(bextData.data() + 602, chunkSize - 602);
                }
                
                // Don't return - continue reading other chunks
            }
            // Pad byte if odd size
            if (chunkSize & 1) {
                file.seekg(1, std::ios::cur);
            }
        } else if (memcmp(chunkId, "LIST", 4) == 0) {
            // Check for INFO list
            char listType[4];
            file.read(listType, 4);

            if (memcmp(listType, "INFO", 4) == 0) {
                // Read INFO chunks
                uint32_t remaining = chunkSize - 4;
                while (remaining > 8) {
                    char infoId[4];
                    uint8_t infoSizeData[4];

                    file.read(infoId, 4);
                    file.read(reinterpret_cast<char*>(infoSizeData), 4);
                    remaining -= 8;

                    uint32_t infoSize = readLE32(infoSizeData);
                    if (infoSize > remaining) break;

                    std::string value(infoSize, '\0');
                    file.read(&value[0], infoSize);
                    value = trimString(value);
                    remaining -= infoSize;

                    // Pad byte if odd size
                    if (infoSize & 1) {
                        char pad;
                        file.read(&pad, 1);
                        remaining--;
                    }

                    if (memcmp(infoId, "INAM", 4) == 0) metadata.title = value;
                    else if (memcmp(infoId, "IART", 4) == 0) metadata.artist = value;
                    else if (memcmp(infoId, "IPRD", 4) == 0) metadata.album = value;
                    else if (memcmp(infoId, "ICMT", 4) == 0) metadata.comment = value;
                    else if (memcmp(infoId, "IGNR", 4) == 0) metadata.genre = value;
                    else if (memcmp(infoId, "ICRD", 4) == 0) {
                        try { metadata.year = std::stoi(value.substr(0, 4)); } catch (...) {}
                    }
                    // Extended INFO tags used by audio-library tools
                    else if (memcmp(infoId, "IKEY", 4) == 0) metadata.keywords = value;      // Keywords
                    else if (memcmp(infoId, "ISBJ", 4) == 0) metadata.category = value;      // Subject/Category
                    else if (memcmp(infoId, "ISFT", 4) == 0) metadata.software = value;      // Software
                    else if (memcmp(infoId, "ICOP", 4) == 0) metadata.copyright = value;     // Copyright
                    else if (memcmp(infoId, "ISRC", 4) == 0) metadata.library = value;       // Source/Library
                    else if (memcmp(infoId, "IENG", 4) == 0) metadata.designer = value;      // Engineer/Designer
                    else if (memcmp(infoId, "ITCH", 4) == 0) metadata.manufacturer = value;  // Technician
                }
                if (metadata.hasData()) return true;
            } else {
                // Skip this LIST chunk
                file.seekg(chunkSize - 4, std::ios::cur);
            }
        } else if (memcmp(chunkId, "iXML", 4) == 0 || memcmp(chunkId, "IXML", 4) == 0) {
            // iXML chunk - professional audio metadata (used by tools such as Pro Tools, etc.)
            if (chunkSize > kMaxTagDataBytes) { file.seekg(chunkSize, std::ios::cur); if (chunkSize & 1) file.seekg(1, std::ios::cur); continue; }
            std::vector<char> xmlData(chunkSize);
            file.read(xmlData.data(), chunkSize);
            std::string xmlStr(xmlData.data(), chunkSize);
            parseIXML(xmlStr, metadata);
            // Pad byte if odd size
            if (chunkSize & 1) {
                file.seekg(1, std::ios::cur);
            }
        } else if (memcmp(chunkId, "_PMX", 4) == 0 || memcmp(chunkId, "XMP ", 4) == 0) {
            // XMP metadata chunk (Adobe format)
            if (chunkSize > kMaxTagDataBytes) { file.seekg(chunkSize, std::ios::cur); if (chunkSize & 1) file.seekg(1, std::ios::cur); continue; }
            std::vector<char> xmpData(chunkSize);
            file.read(xmpData.data(), chunkSize);
            std::string xmpStr(xmpData.data(), chunkSize);
            // Parse simple XMP tags
            auto extractXmp = [&xmpStr](const std::string& tag) -> std::string {
                std::string startTag = "<" + tag + ">";
                std::string endTag = "</" + tag + ">";
                size_t start = xmpStr.find(startTag);
                if (start == std::string::npos) return "";
                start += startTag.length();
                size_t end = xmpStr.find(endTag, start);
                if (end == std::string::npos) return "";
                return xmpStr.substr(start, end - start);
            };
            if (metadata.title.empty()) metadata.title = extractXmp("dc:title");
            if (metadata.description.empty()) metadata.description = extractXmp("dc:description");
            if (metadata.artist.empty()) metadata.artist = extractXmp("dc:creator");
            if (metadata.copyright.empty()) metadata.copyright = extractXmp("dc:rights");
            // Pad byte if odd size
            if (chunkSize & 1) {
                file.seekg(1, std::ios::cur);
            }
        } else {
            // Skip unknown chunk
            file.seekg(chunkSize, std::ios::cur);
            // Pad byte if odd size
            if (chunkSize & 1) {
                file.seekg(1, std::ios::cur);
            }
        }
    }

    return metadata.hasData();
}

// Parse iXML chunk (professional audio metadata format)
bool MetadataReader::parseIXML(const std::string& xmlData, AudioMetadata& metadata) {
    // Simple XML tag extraction (no full XML parser needed)
    auto extractTag = [&xmlData](const std::string& tag) -> std::string {
        // Case-insensitive search
        std::string xmlLower = xmlData;
        std::transform(xmlLower.begin(), xmlLower.end(), xmlLower.begin(), ::tolower);
        std::string tagLower = tag;
        std::transform(tagLower.begin(), tagLower.end(), tagLower.begin(), ::tolower);

        std::string startTag = "<" + tagLower + ">";
        std::string endTag = "</" + tagLower + ">";
        size_t start = xmlLower.find(startTag);
        if (start == std::string::npos) return "";
        start += startTag.length();
        size_t end = xmlLower.find(endTag, start);
        if (end == std::string::npos) return "";
        // Get actual content from original string (preserving case)
        return xmlData.substr(start, end - start);
    };

    // iXML standard fields
    if (metadata.project.empty()) metadata.project = extractTag("PROJECT");
    if (metadata.scene.empty()) metadata.scene = extractTag("SCENE");
    if (metadata.take.empty()) metadata.take = extractTag("TAKE");
    if (metadata.tape.empty()) metadata.tape = extractTag("TAPE");
    if (metadata.circled.empty()) metadata.circled = extractTag("CIRCLED");
    if (metadata.fileUID.empty()) metadata.fileUID = extractTag("FILE_UID");
    if (metadata.notes.empty()) metadata.notes = extractTag("NOTE");
    if (metadata.userBits.empty()) metadata.userBits = extractTag("UBITS");

    // Extended metadata fields that may appear in iXML
    if (metadata.title.empty()) metadata.title = extractTag("TITLE");
    if (metadata.description.empty()) metadata.description = extractTag("DESCRIPTION");
    if (metadata.artist.empty()) metadata.artist = extractTag("ARTIST");
    if (metadata.category.empty()) metadata.category = extractTag("CATEGORY");
    if (metadata.subcategory.empty()) metadata.subcategory = extractTag("SUBCATEGORY");
    if (metadata.keywords.empty()) metadata.keywords = extractTag("KEYWORDS");
    if (metadata.library.empty()) metadata.library = extractTag("LIBRARY");
    if (metadata.designer.empty()) metadata.designer = extractTag("DESIGNER");
    if (metadata.manufacturer.empty()) metadata.manufacturer = extractTag("MANUFACTURER");
    if (metadata.copyright.empty()) metadata.copyright = extractTag("COPYRIGHT");
    if (metadata.url.empty()) metadata.url = extractTag("URL");

    // BWF-compatible fields that may appear in iXML
    if (metadata.originator.empty()) metadata.originator = extractTag("ORIGINATOR");
    if (metadata.originatorRef.empty()) metadata.originatorRef = extractTag("ORIGINATOR_REFERENCE");
    if (metadata.codingHistory.empty()) metadata.codingHistory = extractTag("CODING_HISTORY");

    // Track list
    if (metadata.trackList.empty()) {
        // Look for TRACK_LIST > TRACK > NAME pattern
        size_t pos = 0;
        std::string tracks;
        while ((pos = xmlData.find("<TRACK>", pos)) != std::string::npos) {
            size_t nameStart = xmlData.find("<NAME>", pos);
            size_t trackEnd = xmlData.find("</TRACK>", pos);
            if (nameStart != std::string::npos && nameStart < trackEnd) {
                nameStart += 6;
                size_t nameEnd = xmlData.find("</NAME>", nameStart);
                if (nameEnd != std::string::npos && nameEnd < trackEnd) {
                    if (!tracks.empty()) tracks += ", ";
                    tracks += xmlData.substr(nameStart, nameEnd - nameStart);
                }
            }
            pos = trackEnd + 1;
        }
        metadata.trackList = tracks;
    }

    return metadata.hasData();
}

bool MetadataReader::readIXMLChunk(const std::string& filePath, std::string& xmlData,
                                   std::string& errorMessage, bool* chunkMissing) {
    xmlData.clear();
    errorMessage.clear();
    if (chunkMissing) *chunkMissing = false;
    std::ifstream file(fs::u8path(filePath), std::ios::binary);
    if (!file) { errorMessage = "Cannot open WAV file"; return false; }
    char header[12] = {};
    file.read(header, 12);
    if (file.gcount() != 12 || std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "WAVE", 4) != 0) {
        errorMessage = "Not a valid WAV file";
        return false;
    }
    int iterations = 0;
    while (file && iterations++ < kMaxChunkIter) {
        char chunkHeader[8] = {};
        file.read(chunkHeader, 8);
        if (file.gcount() != 8) break;
        const uint32_t chunkSize = readLE32(reinterpret_cast<const uint8_t*>(chunkHeader) + 4);
        if (std::memcmp(chunkHeader, "iXML", 4) == 0 || std::memcmp(chunkHeader, "IXML", 4) == 0) {
            if (chunkSize > kMaxTagDataBytes) {
                errorMessage = "iXML chunk too large";
                return false;
            }
            xmlData.assign(chunkSize, '\0');
            if (chunkSize > 0) file.read(xmlData.data(), static_cast<std::streamsize>(chunkSize));
            if (static_cast<uint32_t>(file.gcount()) != chunkSize) {
                xmlData.clear();
                errorMessage = "Truncated iXML chunk";
                return false;
            }
            return true;
        }
        file.seekg(static_cast<std::streamoff>(chunkSize + (chunkSize & 1)), std::ios::cur);
    }
    errorMessage = "No iXML chunk";
    if (chunkMissing) *chunkMissing = true;
    return false;
}

bool MetadataReader::readVorbisComment(const std::string& filePath, AudioMetadata& metadata) {
    // Basic Vorbis comment reading for OGG/FLAC
    // This is a simplified implementation
namespace {

using VorbisCommentEntries = std::vector<std::pair<std::string, std::string>>;

std::string TrimVorbisText(std::string value) {
    while (!value.empty() && (value.back() == '\0' || value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// Payload of a Vorbis comment header: vendor string, then a length delimited
// "KEY=value" list. The same layout is used by FLAC METADATA_BLOCK type 4 and
// by the Ogg comment packet (after its 7-byte "\x03vorbis" signature).
bool ParseVorbisCommentPayload(const uint8_t* data, size_t size, VorbisCommentEntries& entries) {
    size_t pos = 0;
    auto readLe32 = [&](uint32_t& out) -> bool {
        if (pos + 4 > size) return false;
        out = static_cast<uint32_t>(data[pos]) |
              (static_cast<uint32_t>(data[pos + 1]) << 8) |
              (static_cast<uint32_t>(data[pos + 2]) << 16) |
              (static_cast<uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return true;
    };

    uint32_t vendorLength = 0;
    if (!readLe32(vendorLength) || vendorLength > size - pos) return false;
    pos += vendorLength;

    uint32_t count = 0;
    if (!readLe32(count) || count > 1000000u) return false;
    entries.reserve(entries.size() + count);
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t length = 0;
        if (!readLe32(length) || length > size - pos) return false;
        const std::string_view raw(reinterpret_cast<const char*>(data + pos), length);
        pos += length;
        const size_t separator = raw.find('=');
        if (separator == std::string_view::npos || separator == 0) continue;
        std::string key = TrimVorbisText(std::string(raw.substr(0, separator)));
        std::string value = TrimVorbisText(std::string(raw.substr(separator + 1)));
        if (key.empty() || value.empty()) continue;
        entries.emplace_back(std::move(key), std::move(value));
    }
    return true;
}

void AppendVorbisValue(std::string& field, const std::string& value) {
    if (field.empty()) {
        field = value;
    } else if (field != value) {
        field += "; ";
        field += value;
    }
}

// Map one Vorbis comment entry. Known keys go to the shared AudioMetadata
// fields used by the result columns and the main search; everything else is
// kept verbatim in customTextTags so unknown vendor keys stay searchable.
void ApplyVorbisCommentEntry(const std::string& key, const std::string& value, AudioMetadata& metadata) {
    std::string alias = key;
    for (char& letter : alias) {
        if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 'a' + 'A');
    }

    if (alias == "TITLE") { AppendVorbisValue(metadata.title, value); return; }
    if (alias == "ARTIST") { AppendVorbisValue(metadata.artist, value); return; }
    if (alias == "ALBUM") { AppendVorbisValue(metadata.album, value); return; }
    if (alias == "ALBUMARTIST" || alias == "ALBUM ARTIST") {
        AppendVorbisValue(metadata.albumArtist, value);
        return;
    }
    if (alias == "DESCRIPTION") { AppendVorbisValue(metadata.description, value); return; }
    if (alias == "COMMENT") { AppendVorbisValue(metadata.comment, value); return; }
    if (alias == "GENRE") { AppendVorbisValue(metadata.genre, value); return; }
    if (alias == "KEYWORDS" || alias == "KEYWORD") {
        AppendVorbisValue(metadata.keywords, value);
        return;
    }
    if (alias == "TAGS") {
        if (metadata.keywords.empty()) metadata.keywords = value;
        return;
    }
    if (alias == "DATE" || alias == "YEAR") {
        try { metadata.year = std::stoi(value.substr(0, 4)); } catch (...) {}
        return;
    }
    if (alias == "TRACKNUMBER") {
        try { metadata.track = std::stoi(value); } catch (...) {}
        return;
    }
    if (alias == "DISCNUMBER") {
        try { metadata.disc = std::stoi(value); } catch (...) {}
        return;
    }
    if (alias == "CATEGORY" || alias == "SUBJECT") { AppendVorbisValue(metadata.category, value); return; }
    if (alias == "SUBCATEGORY") { AppendVorbisValue(metadata.subcategory, value); return; }
    if (alias == "LIBRARY" || alias == "SOURCE") { AppendVorbisValue(metadata.library, value); return; }
    if (alias == "DESIGNER" || alias == "ENGINEER") { AppendVorbisValue(metadata.designer, value); return; }
    if (alias == "INSTRUMENTS" || alias == "INSTRUMENT") {
        AppendVorbisValue(metadata.instruments, value);
        return;
    }
    if (alias == "COPYRIGHT") { AppendVorbisValue(metadata.copyright, value); return; }
    if (alias == "URL" || alias == "CONTACT") { AppendVorbisValue(metadata.url, value); return; }
    if (alias == "SOFTWARE") { AppendVorbisValue(metadata.software, value); return; }
    if (alias == "REPLAYGAIN_TRACK_GAIN") {
        try { metadata.replayGainTrack = std::stof(value); } catch (...) {}
        return;
    }
    if (alias == "REPLAYGAIN_ALBUM_GAIN") {
        try { metadata.replayGainAlbum = std::stof(value); } catch (...) {}
        return;
    }
    if (alias == "COMPOSER") { AppendVorbisValue(metadata.composer, value); return; }
    if (alias == "BPM") { AppendVorbisValue(metadata.bpm, value); return; }
    if (alias == "INITIALKEY" || alias == "KEY" || alias == "MUSICALKEY" || alias == "MUSICAL_KEY") {
        AppendVorbisValue(metadata.musicalKey, value);
        return;
    }
    if (alias == "METADATA_BLOCK_PICTURE" || alias == "COVERART" || alias == "COVERARTMIME" ||
        alias == "ENCODER" || alias == "ENCODED_BY") {
        return;
    }
    if (metadata.customTextTags.find(key) == metadata.customTextTags.end()) {
        metadata.customTextTags.emplace(key, value);
    }
}

// Vorbis files routinely carry only music tags. Those fields must count as a
// successful read, otherwise the scan and import paths drop the whole comment
// block just because no title or artist is present.
bool HasVorbisMusicOnlyData(const AudioMetadata& metadata) {
    return !metadata.composer.empty() || !metadata.bpm.empty() ||
           !metadata.musicalKey.empty() || !metadata.customTextTags.empty() ||
           metadata.year > 0;
}

// FLAC: walk the metadata block chain and read the VORBIS_COMMENT block.
// Blocks we do not need (SEEKTABLE, PICTURE, ...) are skipped by length so a
// large embedded picture never gets pulled into memory here.
bool ReadFlacVorbisComment(const std::string& filePath, AudioMetadata& metadata, bool& customTagsRead) {
    customTagsRead = false;
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    char magic[4];
    file.read(magic, 4);
    if (file.gcount() != 4 || memcmp(magic, "fLaC", 4) != 0) return false;

    VorbisCommentEntries entries;
    bool commentParsed = false;
    bool complete = true;
    for (int block = 0; block < 256; ++block) {
        uint8_t header[4];
        file.read(reinterpret_cast<char*>(header), 4);
        if (file.gcount() != 4) {
            complete = false;
            break;
        }
        const bool lastBlock = (header[0] & 0x80) != 0;
        const int type = header[0] & 0x7F;
        const uint32_t length = (static_cast<uint32_t>(header[1]) << 16) |
                                (static_cast<uint32_t>(header[2]) << 8) |
                                static_cast<uint32_t>(header[3]);
        if (type == 4) {
            if (length > kMaxTagDataBytes) {
                complete = false;
                break;
            }
            std::vector<uint8_t> payload(length);
            file.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(length));
            if (static_cast<uint32_t>(file.gcount()) != length) {
                complete = false;
                break;
            }
            commentParsed = ParseVorbisCommentPayload(payload.data(), payload.size(), entries);
            if (!commentParsed) complete = false;
        } else {
            file.seekg(static_cast<std::streamoff>(length), std::ios_base::cur);
            if (!file.good()) {
                complete = false;
                break;
            }
        }
        if (lastBlock) break;
    }

    for (const auto& entry : entries) ApplyVorbisCommentEntry(entry.first, entry.second, metadata);
    customTagsRead = commentParsed && complete;
    return metadata.hasData() || HasVorbisMusicOnlyData(metadata);
}

// Ogg: assemble packets from page lacing values and read the comment packet
// ("\x03vorbis"). Continuation pages append to the packet that spans them.
bool ReadOggVorbisComment(const std::string& filePath, AudioMetadata& metadata, bool& customTagsRead) {
    customTagsRead = false;
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    VorbisCommentEntries entries;
    std::vector<uint8_t> packet;
    bool commentParsed = false;
    bool complete = true;
    bool done = false;
    size_t scannedBytes = 0;

    for (int page = 0; page < 512 && !done; ++page) {
        uint8_t header[27];
        file.read(reinterpret_cast<char*>(header), 27);
        if (file.gcount() != 27) break;
        if (memcmp(header, "OggS", 4) != 0) {
            complete = false;
            break;
        }
        const uint8_t headerType = header[5];
        const uint8_t segmentCount = header[26];
        std::vector<uint8_t> lacing(segmentCount);
        if (segmentCount > 0) {
            file.read(reinterpret_cast<char*>(lacing.data()), static_cast<std::streamsize>(segmentCount));
            if (file.gcount() != static_cast<std::streamsize>(segmentCount)) {
                complete = false;
                break;
            }
        }
        size_t payloadSize = 0;
        bool packetEndsOnPage = false;
        for (uint8_t value : lacing) {
            payloadSize += value;
            if (value < 255) packetEndsOnPage = true;
        }
        if (payloadSize > kMaxChunkDataBytes) {
            complete = false;
            break;
        }
        std::vector<uint8_t> body(payloadSize);
        if (payloadSize > 0) {
            file.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(payloadSize));
            if (file.gcount() != static_cast<std::streamsize>(payloadSize)) {
                complete = false;
                break;
            }
        }
        scannedBytes += payloadSize;
        if ((headerType & 0x01) == 0) packet.clear();

        size_t offset = 0;
        for (uint8_t value : lacing) {
            packet.insert(packet.end(), body.begin() + static_cast<std::ptrdiff_t>(offset),
                          body.begin() + static_cast<std::ptrdiff_t>(offset + value));
            offset += value;
            if (value == 255) continue;

            const bool isCommentPacket = packet.size() >= 7 && packet[0] == 0x03 &&
                memcmp(packet.data() + 1, "vorbis", 6) == 0;
            if (isCommentPacket) {
                commentParsed = ParseVorbisCommentPayload(packet.data() + 7, packet.size() - 7, entries);
                if (!commentParsed) complete = false;
                done = true;
            }
            packet.clear();
            if (done) break;
        }

        if (!done && !packetEndsOnPage && packet.size() > kMaxTagDataBytes) {
            complete = false;
            break;
        }
        if (scannedBytes > kMaxChunkDataBytes) {
            complete = false;
            break;
        }
    }

    for (const auto& entry : entries) ApplyVorbisCommentEntry(entry.first, entry.second, metadata);
    customTagsRead = commentParsed && complete;
    return metadata.hasData() || HasVorbisMusicOnlyData(metadata);
}

}  // namespace

bool MetadataReader::readVorbisComment(const std::string& filePath, AudioMetadata& metadata,
                                       bool* customTagsRead) {
    if (customTagsRead) *customTagsRead = false;
    MetadataFileReader probe = openFile(filePath);
    if (!probe.is_open()) return false;

    char magic[4] = {};
    probe.read(magic, 4);
    if (probe.gcount() != 4) return false;

    bool parsed = false;
    bool hasData = false;
    if (memcmp(magic, "fLaC", 4) == 0) {
        hasData = ReadFlacVorbisComment(filePath, metadata, parsed);
    } else if (memcmp(magic, "OggS", 4) == 0) {
        hasData = ReadOggVorbisComment(filePath, metadata, parsed);
    } else {
        return false;
    }
    metadata.customTextTagsRead = parsed;
    if (customTagsRead) *customTagsRead = parsed;
    return hasData;
}
// M4A/AAC metadata reading (iTunes-style atoms)
bool MetadataReader::readM4A(const std::string& filePath, AudioMetadata& metadata) {
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    // Read entire file into memory (M4A metadata can be anywhere)
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    if (fileSize > 50 * 1024 * 1024) fileSize = 50 * 1024 * 1024; // Limit to 50MB
    file.seekg(0);

    std::vector<uint8_t> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    size_t bytesRead = file.gcount();

    // Find "ilst" atom (iTunes metadata container)
    auto findAtom = [&data, bytesRead](const char* atomName, size_t startPos = 0) -> std::pair<size_t, size_t> {
        for (size_t i = startPos; i + 8 < bytesRead; i++) {
            if (memcmp(&data[i + 4], atomName, 4) == 0) {
                uint32_t size = (data[i] << 24) | (data[i + 1] << 16) | (data[i + 2] << 8) | data[i + 3];
                if (size < 8 || i + size > bytesRead) continue;
                return {i, size};
            }
        }
        return {0, 0};
    };

    // Read text from "data" atom
    auto readDataAtom = [&data, bytesRead](size_t pos, size_t size) -> std::string {
        // Look for "data" atom inside
        for (size_t i = pos + 8; i + 16 < pos + size && i < bytesRead; i++) {
            if (memcmp(&data[i + 4], "data", 4) == 0) {
                uint32_t dataSize = (data[i] << 24) | (data[i + 1] << 16) | (data[i + 2] << 8) | data[i + 3];
                if (dataSize < 16 || i + dataSize > bytesRead) return "";
                // Skip: size(4) + "data"(4) + type(4) + locale(4) = 16 bytes before text
                size_t textStart = i + 16;
                size_t textLen = dataSize - 16;
                return std::string(reinterpret_cast<char*>(&data[textStart]), textLen);
            }
        }
        return "";
    };

    // Find moov/udta/meta/ilst atoms (iTunes metadata)
    auto [moovPos, moovSize] = findAtom("moov");
    if (moovPos == 0) return false;

    auto [udtaPos, udtaSize] = findAtom("udta", moovPos);
    if (udtaPos == 0) udtaPos = moovPos;

    auto [metaPos, metaSize] = findAtom("meta", udtaPos);
    if (metaPos == 0) metaPos = udtaPos;

    auto [ilstPos, ilstSize] = findAtom("ilst", metaPos);
    if (ilstPos == 0) return false;

    // Parse ilst atoms
    size_t pos = ilstPos + 8;
    size_t ilstEnd = ilstPos + ilstSize;
    while (pos + 8 < ilstEnd && pos < bytesRead) {
        uint32_t atomSize = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
        if (atomSize < 8 || pos + atomSize > bytesRead) break;

        char atomType[5] = {0};
        memcpy(atomType, &data[pos + 4], 4);
        std::string value = readDataAtom(pos, atomSize);

        // Map iTunes atoms to metadata (use memcmp to avoid escape sequence issues)
        // iTunes uses © (0xA9) prefix for standard atoms
        if (atomType[0] == '\xA9') {
            if (memcmp(atomType + 1, "nam", 3) == 0) metadata.title = value;
            else if (memcmp(atomType + 1, "ART", 3) == 0) metadata.artist = value;
            else if (memcmp(atomType + 1, "alb", 3) == 0) metadata.album = value;
            else if (memcmp(atomType + 1, "cmt", 3) == 0) metadata.comment = value;
            else if (memcmp(atomType + 1, "gen", 3) == 0) metadata.genre = value;
            else if (memcmp(atomType + 1, "wrt", 3) == 0) metadata.designer = value;  // Composer
            else if (memcmp(atomType + 1, "day", 3) == 0) {
                try { metadata.year = std::stoi(value.substr(0, 4)); } catch (...) {}
            }
        }
        else if (memcmp(atomType, "aART", 4) == 0) metadata.albumArtist = value;
        else if (memcmp(atomType, "desc", 4) == 0) metadata.description = value;
        else if (memcmp(atomType, "keyw", 4) == 0) metadata.keywords = value;
        else if (memcmp(atomType, "catg", 4) == 0) metadata.category = value;
        else if (memcmp(atomType, "cprt", 4) == 0) metadata.copyright = value;
        else if (memcmp(atomType, "disk", 4) == 0) {
            // disk atom: binary data atom, payload is 6 bytes:
            // [2 bytes padding][2 bytes disc number BE][2 bytes total discs BE]
            for (size_t i = pos + 8; i + 16 < pos + atomSize && i < bytesRead; i++) {
                if (memcmp(&data[i + 4], "data", 4) == 0) {
                    uint32_t dataSize = (data[i] << 24) | (data[i + 1] << 16) | (data[i + 2] << 8) | data[i + 3];
                    if (dataSize >= 20 && i + dataSize <= bytesRead) {
                        // Skip 16-byte header, then 2-byte padding, then 2-byte disc number
                        size_t discOff = i + 16 + 2;
                        int discNum = (data[discOff] << 8) | data[discOff + 1];
                        if (discNum > 0) metadata.disc = discNum;
                    }
                    break;
                }
            }
        }

        pos += atomSize;
    }

    return metadata.hasData();
}

bool MetadataReader::readAlbumArt(const std::string& filePath, AlbumArt& art) {
    bool found = false;
    try {
        // Determine format from extension
        std::string ext;
        std::wstring wpath = Platform::TextEncoding::Utf8ToWidePermissive(filePath);
        if (!filePath.empty() && wpath.empty()) return false;
#ifdef _WIN32
        fs::path fsPath(wpath);
        std::wstring wext = fsPath.extension().wstring();
        ext = Platform::TextEncoding::WideToUtf8Permissive(wext);
#else
        ext = fs::path(filePath).extension().string();
#endif
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".mp3") {
            found = readID3v2AlbumArt(filePath, art);
        } else if (ext == ".wav" || ext == ".wave") {
            if (readWAVID3Chunk(filePath, art)) found = true;
            else found = readID3v2AlbumArt(filePath, art);
        } else if (ext == ".flac") {
            found = readFLACAlbumArt(filePath, art);
        } else if (ext == ".m4a" || ext == ".aac" || ext == ".mp4") {
            found = readM4AAlbumArt(filePath, art);
        } else if (ext == ".ogg") {
            found = readOGGAlbumArt(filePath, art);
        } else if (ext == ".aiff" || ext == ".aif") {
            found = readID3v2AlbumArt(filePath, art);
        } else if (ext == ".wv") {
            found = ReadWavPackAlbumArt(filePath, art);
        }
    } catch (...) {

    }

    if (found && art.hasData()) return true;

    // If embedded art is absent, probe sidecar images derived from the audio stem.
    // Suffix priority is stem, thumb, art, cover, then hyphen variants.
    try {
#ifdef _WIN32
        std::wstring wpath = Platform::TextEncoding::Utf8ToWidePermissive(filePath);
        fs::path p(wpath);
#else
        fs::path p(filePath);
#endif
        fs::path stemPath = p.stem();
        fs::path parentDir = p.parent_path();

        if (!parentDir.empty() && !stemPath.empty()) {
            static const char* kSidecarSuffixes[] = { "", "_thumb", "_art", "_cover", "-thumb", "-art" };
            for (const char* suffix : kSidecarSuffixes) {
                for (const char* imgExt : { ".jpg", ".png", ".jpeg" }) {
                    fs::path fileName = stemPath;
                    fileName += suffix;
                    fileName += imgExt;
                    fs::path imgPath = parentDir / fileName;
                    std::string probeError;
                    if (Platform::FileSystem::ProbePath(
                            imgPath.u8string(), &probeError) == Platform::FileSystem::PathStatus::Present) {
                        MetadataFileReader f;
                        if (f.Open(imgPath.u8string()) && f.is_open()) {
                            f.seekg(0, std::ios::end);
                            const std::streamsize fsize =
                                static_cast<std::streamsize>(f.tellg());
                            if (fsize <= 0 ||
                                static_cast<size_t>(fsize) > kMaxCoverDataBytes) {
                                continue;
                            }
                            f.seekg(0, std::ios::beg);
                            if (!f.good()) continue;
                            art.data.resize(static_cast<size_t>(fsize));
                            f.read(reinterpret_cast<char*>(art.data.data()), fsize);
                            if (f.gcount() != fsize) {
                                art.data.clear();
                                continue;
                            }
                            art.mimeType = (std::strcmp(imgExt, ".png") == 0) ? "image/png" : "image/jpeg";
                            return true; // 成功伪装成内嵌封面返回！
                        }
                    }
                }
            }
        }
    } catch (...) {}

    return false;
}

// Base64 decoding helper for OGG METADATA_BLOCK_PICTURE
static std::vector<uint8_t> base64Decode(const std::string& encoded) {
    static const std::string base64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::vector<uint8_t> decoded;
    if (encoded.empty()) return decoded;

    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[base64Chars[i]] = i;

    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return decoded;
}

bool MetadataReader::readFLACAlbumArt(const std::string& filePath, AlbumArt& art) {
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    // Check FLAC signature: "fLaC"
    char sig[4];
    file.read(sig, 4);
    if (file.gcount() != 4 || memcmp(sig, "fLaC", 4) != 0) {
        return false;
    }

    // Parse metadata blocks
    bool isLast = false;
    while (!isLast && file.good()) {
        uint8_t blockHeader[4];
        file.read(reinterpret_cast<char*>(blockHeader), 4);
        if (file.gcount() != 4) break;

        isLast = (blockHeader[0] & 0x80) != 0;
        uint8_t blockType = blockHeader[0] & 0x7F;
        uint32_t blockLength = (blockHeader[1] << 16) | (blockHeader[2] << 8) | blockHeader[3];

        // Block type 6 = PICTURE
        if (blockType == 6 && blockLength > 32 && blockLength <= kMaxCoverDataBytes) {
            std::vector<uint8_t> blockData(blockLength);
            file.read(reinterpret_cast<char*>(blockData.data()), blockLength);
            if (static_cast<uint32_t>(file.gcount()) != blockLength) break;

            size_t pos = 0;
            
            // Picture type (4 bytes, big-endian)
            uint32_t pictureType = (blockData[pos] << 24) | (blockData[pos+1] << 16) | 
                                   (blockData[pos+2] << 8) | blockData[pos+3];
            pos += 4;

            // MIME type length (4 bytes)
            uint32_t mimeLen = (blockData[pos] << 24) | (blockData[pos+1] << 16) |
                               (blockData[pos+2] << 8) | blockData[pos+3];
            pos += 4;

            if (pos + mimeLen > blockLength) break;
            std::string mimeType(reinterpret_cast<char*>(blockData.data() + pos), mimeLen);
            pos += mimeLen;

            // Description length (4 bytes)
            if (pos + 4 > blockLength) break;
            uint32_t descLen = (blockData[pos] << 24) | (blockData[pos+1] << 16) |
                               (blockData[pos+2] << 8) | blockData[pos+3];
            pos += 4;
            pos += descLen;  // Skip description

            // Width, height, depth, colors (16 bytes)
            if (pos + 16 > blockLength) break;
            art.width = (blockData[pos] << 24) | (blockData[pos+1] << 16) |
                        (blockData[pos+2] << 8) | blockData[pos+3];
            pos += 4;
            art.height = (blockData[pos] << 24) | (blockData[pos+1] << 16) |
                         (blockData[pos+2] << 8) | blockData[pos+3];
            pos += 4;
            pos += 8;  // Skip depth and colors

            // Image data length (4 bytes)
            if (pos + 4 > blockLength) break;
            uint32_t dataLen = (blockData[pos] << 24) | (blockData[pos+1] << 16) |
                               (blockData[pos+2] << 8) | blockData[pos+3];
            pos += 4;

            if (pos + dataLen > blockLength) break;

            // Extract image data
            art.data.assign(blockData.begin() + pos, blockData.begin() + pos + dataLen);
            art.mimeType = mimeType;

            // Prefer front cover (type 3) but accept any
            if (pictureType == 3 || art.data.empty()) {
                return true;
            }
        } else {
            // Skip this block
            file.seekg(blockLength, std::ios::cur);
        }
    }

    return !art.data.empty();
}

bool MetadataReader::readM4AAlbumArt(const std::string& filePath, AlbumArt& art) {
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    // M4A uses the ISO base media format; locate the
    // moov/udta/meta/ilst/covr/data atom chain.
    file.seekg(0, std::ios::end);
    std::streamsize fileSize = file.tellg();
    file.seekg(0);

    // Read entire file for simplicity (most M4A files have metadata near start)
    size_t maxSearch = std::min((std::streamsize)(16 * 1024 * 1024), fileSize); // Max 16MB search
    std::vector<uint8_t> buffer(maxSearch);
    file.read(reinterpret_cast<char*>(buffer.data()), maxSearch);
    size_t bytesRead = file.gcount();

    // Search for "covr" atom
    for (size_t i = 0; i + 8 < bytesRead; i++) {
        if (buffer[i] == 'c' && buffer[i+1] == 'o' && buffer[i+2] == 'v' && buffer[i+3] == 'r') {
            // Found covr, backtrack to read atom size
            if (i < 4) continue;
            size_t atomStart = i - 4;
            uint32_t atomSize = (buffer[atomStart] << 24) | (buffer[atomStart+1] << 16) |
                                (buffer[atomStart+2] << 8) | buffer[atomStart+3];

            if (atomSize < 16 || atomStart + atomSize > bytesRead) continue;

            // Look for "data" sub-atom
            size_t dataPos = i + 4;
            while (dataPos + 8 < atomStart + atomSize) {
                uint32_t subSize = (buffer[dataPos] << 24) | (buffer[dataPos+1] << 16) |
                                   (buffer[dataPos+2] << 8) | buffer[dataPos+3];
                if (subSize < 8 || dataPos + subSize > atomStart + atomSize) break;

                if (buffer[dataPos+4] == 'd' && buffer[dataPos+5] == 'a' &&
                    buffer[dataPos+6] == 't' && buffer[dataPos+7] == 'a') {
                    // Found data atom
                    // Skip: size(4) + "data"(4) + type(4) + locale(4) = 16 bytes header
                    size_t imageStart = dataPos + 16;
                    size_t imageSize = subSize - 16;

                    if (imageStart + imageSize <= bytesRead && imageSize > 0) {
                        art.data.assign(buffer.begin() + imageStart, buffer.begin() + imageStart + imageSize);
                        
                        // Detect MIME type from image header
                        if (imageSize >= 3 && buffer[imageStart] == 0xFF && buffer[imageStart+1] == 0xD8) {
                            art.mimeType = "image/jpeg";
                        } else if (imageSize >= 8 && buffer[imageStart] == 0x89 && buffer[imageStart+1] == 'P') {
                            art.mimeType = "image/png";
                        } else {
                            art.mimeType = "image/jpeg";  // Default
                        }
                        return true;
                    }
                }
                dataPos += subSize;
            }
        }
    }

    return false;
}

bool MetadataReader::readOGGAlbumArt(const std::string& filePath, AlbumArt& art) {
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    // Read file into memory (OGG files are usually not too large for metadata search)
    file.seekg(0, std::ios::end);
    std::streamsize fileSize = file.tellg();
    file.seekg(0);

    size_t maxSearch = std::min((std::streamsize)(8 * 1024 * 1024), fileSize);
    std::vector<char> buffer(maxSearch);
    file.read(buffer.data(), maxSearch);
    size_t bytesRead = file.gcount();

    // Search for METADATA_BLOCK_PICTURE in Vorbis comments
    // It's stored as base64 encoded data
    std::string searchKey = "METADATA_BLOCK_PICTURE=";
    for (size_t i = 0; i + searchKey.size() < bytesRead; i++) {
        if (memcmp(buffer.data() + i, searchKey.c_str(), searchKey.size()) == 0) {
            size_t valueStart = i + searchKey.size();
            
            // Find end of base64 data (next null or control char)
            size_t valueEnd = valueStart;
            while (valueEnd < bytesRead && buffer[valueEnd] > 31 && buffer[valueEnd] != 127) {
                valueEnd++;
            }

            std::string base64Data(buffer.data() + valueStart, valueEnd - valueStart);
            std::vector<uint8_t> decoded = base64Decode(base64Data);

            if (decoded.size() < 32) continue;

            // Parse PICTURE block (same format as FLAC)
            size_t pos = 0;
            
            // Picture type
            pos += 4;

            // MIME type length
            uint32_t mimeLen = (decoded[pos] << 24) | (decoded[pos+1] << 16) |
                               (decoded[pos+2] << 8) | decoded[pos+3];
            pos += 4;

            if (pos + mimeLen > decoded.size()) continue;
            std::string mimeType(reinterpret_cast<char*>(decoded.data() + pos), mimeLen);
            pos += mimeLen;

            // Description length
            if (pos + 4 > decoded.size()) continue;
            uint32_t descLen = (decoded[pos] << 24) | (decoded[pos+1] << 16) |
                               (decoded[pos+2] << 8) | decoded[pos+3];
            pos += 4;
            pos += descLen;

            // Skip width, height, depth, colors (16 bytes)
            if (pos + 16 > decoded.size()) continue;
            pos += 16;

            // Image data length
            if (pos + 4 > decoded.size()) continue;
            uint32_t dataLen = (decoded[pos] << 24) | (decoded[pos+1] << 16) |
                               (decoded[pos+2] << 8) | decoded[pos+3];
            pos += 4;

            if (pos + dataLen > decoded.size()) continue;

            art.data.assign(decoded.begin() + pos, decoded.begin() + pos + dataLen);
            art.mimeType = mimeType;
            return true;
        }
    }

    return false;
}

bool MetadataReader::readID3v2AlbumArt(const std::string& filePath, AlbumArt& art) {
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    // Read ID3v2 header (10 bytes)
    uint8_t header[10];
    file.read(reinterpret_cast<char*>(header), 10);
    if (file.gcount() != 10) return false;

    // Check for ID3 header at start
    bool foundAtStart = (header[0] == 'I' && header[1] == 'D' && header[2] == '3');

    // If not at start, search for ID3 tag at end of file (common for WAV files)
    if (!foundAtStart) {
        // Get file size
        file.seekg(0, std::ios::end);
        std::streamsize fileSize = file.tellg();

        // Search backwards from end for "ID3" marker
        // Typically within last 128KB of file
        std::streamsize searchSize = std::min(fileSize, (std::streamsize)(256 * 1024));
        std::streamsize searchStart = fileSize - searchSize;

        file.seekg(searchStart);
        std::vector<uint8_t> buffer(searchSize);
        file.read(reinterpret_cast<char*>(buffer.data()), searchSize);
        std::streamsize bytesRead = file.gcount();

        // Search for "ID3" in buffer
        bool found = false;
        for (std::streamsize i = 0; i < bytesRead - 10; i++) {
            if (buffer[i] == 'I' && buffer[i + 1] == 'D' && buffer[i + 2] == '3') {
                // Found ID3 tag, seek to it
                file.clear();
                file.seekg(searchStart + i);
                file.read(reinterpret_cast<char*>(header), 10);
                if (file.gcount() == 10) {
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            return false;
        }
    }

    // Verify ID3 header
    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        return false;
    }

    uint8_t majorVersion = header[3];
    uint32_t tagSize = readSyncSafeInt(header + 6);

    if (majorVersion < 2 || majorVersion > 4 || tagSize == 0) {
        return false;
    }

    if (tagSize > kMaxTagDataBytes) return false;

    // Read tag data
    std::vector<uint8_t> tagData(tagSize);
    file.read(reinterpret_cast<char*>(tagData.data()), tagSize);
    if (static_cast<uint32_t>(file.gcount()) != tagSize) return false;

    // Parse frames looking for APIC
    size_t pos = 0;
    while (pos + 10 < tagSize) {
        char frameId[5] = {0};
        if (majorVersion >= 3) {
            memcpy(frameId, tagData.data() + pos, 4);
            pos += 4;
        } else {
            memcpy(frameId, tagData.data() + pos, 3);
            pos += 3;
        }

        if (frameId[0] == 0) break;

        uint32_t frameSize;
        if (majorVersion >= 4) {
            frameSize = readSyncSafeInt(tagData.data() + pos);
        } else if (majorVersion == 3) {
            frameSize = readBE32(tagData.data() + pos);
        } else {
            frameSize = (tagData[pos] << 16) | (tagData[pos + 1] << 8) | tagData[pos + 2];
            pos += 3;
            pos += frameSize;
            continue;
        }
        pos += 4;

        if (majorVersion >= 3) {
            pos += 2;  // Skip flags
        }

        if (frameSize == 0 || pos + frameSize > tagSize) break;

        std::string id(frameId);
        if (id == "APIC" || id == "PIC") {
            // Found album art frame
            size_t frameStart = pos;
            uint8_t encoding = tagData[frameStart];
            size_t mimeStart = frameStart + 1;

            // Read MIME type (null-terminated)
            std::string mimeType;
            size_t i = mimeStart;
            while (i < tagSize && tagData[i] != 0) {
                mimeType += static_cast<char>(tagData[i]);
                i++;
            }
            i++;  // Skip null terminator

            // Skip picture type (1 byte)
            i++;

            // Skip description (null-terminated, depends on encoding)
            if (encoding == 0 || encoding == 3) {
                while (i < tagSize && tagData[i] != 0) i++;
                i++;
            } else {
                // UTF-16: skip until double null
                while (i + 1 < tagSize && !(tagData[i] == 0 && tagData[i + 1] == 0)) i += 2;
                i += 2;
            }

            // Find image data - don't trust frameSize, search for actual image boundaries
            size_t imageStart = i;
            size_t imageEnd = 0;
            
            // Check for JPEG (FF D8 FF)
            if (imageStart + 3 < tagSize && tagData[imageStart] == 0xFF && tagData[imageStart + 1] == 0xD8) {
                // Use the last JPEG EOI marker within this APIC frame.
                size_t searchEnd = std::min((size_t)tagSize, frameStart + frameSize);
                size_t lastEnd = 0;
                for (size_t j = imageStart + 2; j + 1 < searchEnd; j++) {
                    if (tagData[j] == 0xFF && tagData[j + 1] == 0xD9) {
                        lastEnd = j + 2; // 找到但不 break，继续往后找
                    }
                }
                if (lastEnd > imageStart) {
                    art.data.assign(tagData.begin() + imageStart, tagData.begin() + lastEnd);
                    art.mimeType = mimeType.empty() ? "image/jpeg" : mimeType;
                    return true;
                }
            }
            // Check for PNG (89 50 4E 47 ... IEND)
            else if (imageStart + 8 < tagSize && tagData[imageStart] == 0x89 && tagData[imageStart + 1] == 0x50 &&
                     tagData[imageStart + 2] == 0x4E && tagData[imageStart + 3] == 0x47) {
                // Find PNG IEND marker (00 00 00 00 49 45 4E 44 AE 42 60 82)
                for (size_t j = imageStart + 8; j + 7 < tagSize; j++) {
                    if (tagData[j] == 'I' && tagData[j + 1] == 'E' && tagData[j + 2] == 'N' && tagData[j + 3] == 'D') {
                        imageEnd = j + 8;  // IEND + CRC
                        break;
                    }
                }
                if (imageEnd > imageStart) {
                    art.data.assign(tagData.begin() + imageStart, tagData.begin() + imageEnd);
                    art.mimeType = mimeType.empty() ? "image/png" : mimeType;
                    return true;
                }
            }
            
            // Fallback: use frameSize if image markers not found
            size_t imageSize = frameSize - (i - frameStart);
            if (imageSize > 0 && i + imageSize <= tagSize) {
                art.data.assign(tagData.begin() + i, tagData.begin() + i + imageSize);
                art.mimeType = mimeType;
                return true;
            }
        }

        pos += frameSize;
    }

    return false;
}

// Local helpers (duplicated from MetadataReader private statics for use in file-scope functions)
static uint32_t localReadBE32(const uint8_t* d) {
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
}
static uint32_t localReadSyncSafe(const uint8_t* d) {
    return ((d[0] & 0x7F) << 21) | ((d[1] & 0x7F) << 14) | ((d[2] & 0x7F) << 7) | (d[3] & 0x7F);
}

// Helper: try to parse an ID3v2 tag blob and extract APIC frame cover art.
// Returns true if cover art was found.
static bool parseID3v2BlobForAPIC(const uint8_t* data, size_t dataSize, AlbumArt& art) {
    if (dataSize < 10) return false;
    // Verify ID3v2 header: "ID3"
    if (data[0] != 'I' || data[1] != 'D' || data[2] != '3') return false;

    uint8_t majorVersion = data[3];
    if (majorVersion < 2 || majorVersion > 4) return false;

    uint32_t tagSize = localReadSyncSafe(data + 6);
    if (tagSize == 0 || tagSize + 10 > dataSize) tagSize = (uint32_t)(dataSize - 10);

    size_t pos = 10; // skip header
    while (pos + 10 < 10 + tagSize) {
        char frameId[5] = {0};
        if (majorVersion >= 3) {
            memcpy(frameId, data + pos, 4);
            pos += 4;
        } else {
            memcpy(frameId, data + pos, 3);
            pos += 3;
        }
        if (frameId[0] == 0) break;

        uint32_t frameSize;
        if (majorVersion >= 4) {
            frameSize = localReadSyncSafe(data + pos);
        } else if (majorVersion == 3) {
            frameSize = localReadBE32(data + pos);
        } else {
            frameSize = (data[pos] << 16) | (data[pos + 1] << 8) | data[pos + 2];
            pos += 3 + frameSize;
            continue;
        }
        pos += 4;
        if (majorVersion >= 3) pos += 2; // skip flags

        if (frameSize == 0 || pos + frameSize > 10 + tagSize) break;

        std::string id(frameId);
        if (id == "APIC" || id == "PIC") {
            size_t frameStart = pos;
            // uint8_t encoding = data[frameStart];
            size_t mimeStart = frameStart + 1;

            // Read MIME type (null-terminated)
            std::string mimeType;
            size_t i = mimeStart;
            while (i < pos + frameSize && data[i] != 0) {
                mimeType += static_cast<char>(data[i]);
                i++;
            }
            i++; // skip null

            uint8_t pictureType = (i < pos + frameSize) ? data[i] : 0;
            i++; // skip picture type

            // Skip description
            uint8_t encoding = data[frameStart];
            if (encoding == 0 || encoding == 3) {
                while (i < pos + frameSize && data[i] != 0) i++;
                i++;
            } else {
                while (i + 1 < pos + frameSize && !(data[i] == 0 && data[i + 1] == 0)) i += 2;
                i += 2;
            }

            size_t imageDataSize = frameSize - (i - frameStart);
            if (imageDataSize > 100 && i + imageDataSize <= dataSize) {
                art.data.assign(data + i, data + i + imageDataSize);
                if (mimeType.empty()) {
                    // Detect from magic bytes
                    if (imageDataSize > 3 && data[i] == 0xFF && data[i+1] == 0xD8)
                        mimeType = "image/jpeg";
                    else if (imageDataSize > 4 && data[i] == 0x89 && data[i+1] == 0x50)
                        mimeType = "image/png";
                }
                art.mimeType = mimeType.empty() ? "image/jpeg" : mimeType;
                // Prefer front cover (type 3), but accept any
                if (pictureType == 3) return true;
                // Keep looking for front cover, but remember this one
                if (pictureType != 3) {
                    // Continue searching for a front cover type
                    AlbumArt backup = art;
                    pos += frameSize;
                    // Quick scan for another APIC frame with type 3
                    bool foundFront = false;
                    while (pos + 10 < 10 + tagSize) {
                        char fid2[5] = {0};
                        if (majorVersion >= 3) { memcpy(fid2, data + pos, 4); pos += 4; }
                        else { memcpy(fid2, data + pos, 3); pos += 3; }
                        if (fid2[0] == 0) break;
                        uint32_t fs2;
                        if (majorVersion >= 4) fs2 = localReadSyncSafe(data + pos);
                        else if (majorVersion == 3) fs2 = localReadBE32(data + pos);
                        else { fs2 = (data[pos]<<16)|(data[pos+1]<<8)|data[pos+2]; pos += 3 + fs2; continue; }
                        pos += 4;
                        if (majorVersion >= 3) pos += 2;
                        if (fs2 == 0 || pos + fs2 > 10 + tagSize) break;
                        std::string fid2s(fid2);
                        if ((fid2s == "APIC" || fid2s == "PIC") && fs2 > 10) {
                            size_t fs2Start = pos;
                            size_t mi = fs2Start + 1;
                            std::string mt2;
                            while (mi < pos + fs2 && data[mi] != 0) { mt2 += (char)data[mi]; mi++; }
                            mi++;
                            uint8_t pt2 = (mi < pos + fs2) ? data[mi] : 0;
                            mi++;
                            uint8_t enc2 = data[fs2Start];
                            if (enc2 == 0 || enc2 == 3) { while (mi < pos+fs2 && data[mi]!=0) mi++; mi++; }
                            else { while (mi+1 < pos+fs2 && !(data[mi]==0&&data[mi+1]==0)) mi+=2; mi+=2; }
                            size_t ids2 = fs2 - (mi - fs2Start);
                            if (ids2 > 100 && pt2 == 3 && mi + ids2 <= dataSize) {
                                art.data.assign(data + mi, data + mi + ids2);
                                art.mimeType = mt2.empty() ? "image/jpeg" : mt2;
                                foundFront = true;
                                break;
                            }
                        }
                        pos += fs2;
                    }
                    if (!foundFront) art = backup; // use the first APIC we found
                    return true;
                }
            }
        }
        pos += frameSize;
    }
    return false;
}

// Validate JPEG signatures before treating a raw blob as cover art. PCM can
// contain FF D8 FF by chance, so the byte after SOI must be a legal marker.
static bool isValidJpegMarkerAfterSOI(uint8_t marker) {
    // After SOI (FF D8), the next marker must be one of:
    if (marker >= 0xE0 && marker <= 0xEF) return true;  // APP0..APP15 (JFIF, EXIF, XMP, etc.)
    if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC8) return true;  // SOF0..SOF15 (except reserved C8)
    if (marker == 0xDB) return true;  // DQT (quantization table)
    if (marker == 0xC4) return true;  // DHT (Huffman table)
    if (marker == 0xDD) return true;  // DRI (restart interval)
    if (marker == 0xDA) return true;  // SOS (start of scan)
    if (marker == 0xFE) return true;  // COM (comment)
    return false;
}

static bool bruteForceScanForImage(const uint8_t* data, size_t dataSize, AlbumArt& art) {
    for (size_t i = 0; i + 3 < dataSize; i++) {
        // JPEG: FF D8 FF xx — must validate xx is a real JPEG marker
        if (data[i] == 0xFF && data[i + 1] == 0xD8 && data[i + 2] == 0xFF) {
            // Reject if 4th byte is not a legitimate JPEG marker type
            if (i + 3 >= dataSize || !isValidJpegMarkerAfterSOI(data[i + 3])) continue;

            // Find the LAST FF D9 to get the full image (not just EXIF thumbnail)
            size_t lastEnd = 0;
            for (size_t j = i + 3; j + 1 < dataSize; j++) {
                if (data[j] == 0xFF && data[j + 1] == 0xD9) {
                    lastEnd = j + 2;
                }
            }
            if (lastEnd > 0) {
                size_t jpegSize = lastEnd - i;
                if (jpegSize > 1000) {
                    art.data.assign(data + i, data + lastEnd);
                    art.mimeType = "image/jpeg";
                    return true;
                }
            }
        }
        // PNG: 89 50 4E 47
        if (i + 4 <= dataSize &&
            data[i] == 0x89 && data[i + 1] == 0x50 && data[i + 2] == 0x4E && data[i + 3] == 0x47) {
            // Find PNG IEND marker
            for (size_t j = i + 8; j + 7 < dataSize; j++) {
                if (data[j] == 'I' && data[j + 1] == 'E' && data[j + 2] == 'N' && data[j + 3] == 'D') {
                    size_t pngSize = j - i + 8;
                    if (pngSize > 1000) {
                        art.data.assign(data + i, data + j + 8);
                        art.mimeType = "image/png";
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// Read album art from WAV chunks (ID3, DGDA - Boom Library, Avid, etc.)
bool MetadataReader::readWAVID3Chunk(const std::string& filePath, AlbumArt& art) {
    MetadataFileReader file = openFile(filePath);
    if (!file.is_open()) return false;

    char riffHeader[12];
    file.read(riffHeader, 12);
    if (file.gcount() != 12) return false;
    // Support both RIFF/WAVE and RF64/WAVE
    bool isRIFF = (memcmp(riffHeader, "RIFF", 4) == 0);
    bool isRF64 = (memcmp(riffHeader, "RF64", 4) == 0);
    if ((!isRIFF && !isRF64) || memcmp(riffHeader + 8, "WAVE", 4) != 0) return false;

    file.seekg(0, std::ios::end);
    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(12);

    size_t pos = 12;
    // For RF64, skip ds64 chunk if present
    if (isRF64 && pos + 8 < fileSize) {
        char cid[4];
        uint8_t sb[4];
        file.read(cid, 4);
        file.read(reinterpret_cast<char*>(sb), 4);
        if (file.gcount() == 4 && memcmp(cid, "ds64", 4) == 0) {
            uint32_t ds64Size = readLE32(sb);
            pos += 8 + ds64Size;
            if (ds64Size % 2 == 1) pos++;
            file.seekg(pos);
        } else {
            file.seekg(12); // reset
        }
    }

    while (pos + 8 < fileSize) {
        char chunkId[5] = {0};
        uint8_t sizeBytes[4];
        file.read(chunkId, 4);
        file.read(reinterpret_cast<char*>(sizeBytes), 4);
        if (file.gcount() != 4) break;

        uint32_t chunkSize = readLE32(sizeBytes);
        if (chunkSize > kMaxCoverDataBytes) break; // reject obviously corrupt chunk size

        // ID3 chunk (embedded ID3v2 tag in WAV)
        if ((memcmp(chunkId, "ID3 ", 4) == 0 || memcmp(chunkId, "id3 ", 4) == 0) && chunkSize > 10) {
            std::vector<uint8_t> data(chunkSize);
            file.read(reinterpret_cast<char*>(data.data()), chunkSize);
            if (static_cast<uint32_t>(file.gcount()) != chunkSize) break;

            // Strategy 1: Try proper ID3v2 APIC frame parsing (most reliable)
            if (parseID3v2BlobForAPIC(data.data(), chunkSize, art)) {
                                return true;
                            }

            // Strategy 2: Brute-force scan for JPEG/PNG markers (fallback)
            if (bruteForceScanForImage(data.data(), chunkSize, art)) {
                                return true;
            }
        }

        // DGDA chunk (Digidesign/Avid - Boom Library uses this)
        else if (memcmp(chunkId, "DGDA", 4) == 0 && chunkSize > 100) {
            std::vector<uint8_t> data(chunkSize);
            file.read(reinterpret_cast<char*>(data.data()), chunkSize);
            if (static_cast<uint32_t>(file.gcount()) != chunkSize) break;

            // Brute-force scan for JPEG/PNG in DGDA chunk
            if (bruteForceScanForImage(data.data(), chunkSize, art)) {
                            return true;
                        }
                    }

        // AXML chunk (some tools store XML with embedded base64 images)
        else if ((memcmp(chunkId, "axml", 4) == 0 || memcmp(chunkId, "AXML", 4) == 0) && chunkSize > 100) {
            // Skip: axml usually doesn't contain cover art, just metadata
            // (but we still need to advance past it)
        }

        pos += 8 + chunkSize;
        if (chunkSize % 2 == 1) pos++;
        file.seekg(pos);
    }
    return false;
}
