#include "MetadataReader.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

int checks = 0;
int failures = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct Fixture {
    fs::path directory;

    Fixture() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 128; ++attempt) {
            const auto candidate = fs::temp_directory_path() /
                ("harken_vorbis_" + std::to_string(tick) + "_" + std::to_string(attempt));
            if (fs::create_directory(candidate)) {
                directory = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create a unique Vorbis fixture directory");
    }

    ~Fixture() {
        std::error_code error;
        fs::remove_all(directory, error);
    }

    fs::path Write(const std::string& name, const std::string& bytes) const {
        const fs::path path = directory / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        return path;
    }
};

void AppendLe32(std::string& bytes, uint32_t value) {
    bytes.push_back(static_cast<char>(value & 0xFF));
    bytes.push_back(static_cast<char>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<char>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<char>((value >> 24) & 0xFF));
}

std::string VorbisCommentPayload(const std::vector<std::string>& entries) {
    std::string payload;
    const std::string vendor = "Harken Search test";
    AppendLe32(payload, static_cast<uint32_t>(vendor.size()));
    payload += vendor;
    AppendLe32(payload, static_cast<uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        AppendLe32(payload, static_cast<uint32_t>(entry.size()));
        payload += entry;
    }
    return payload;
}

std::string FlacBlock(int type, bool last, const std::string& payload) {
    std::string bytes;
    bytes.push_back(static_cast<char>((last ? 0x80 : 0x00) | (type & 0x7F)));
    const uint32_t size = static_cast<uint32_t>(payload.size());
    bytes.push_back(static_cast<char>((size >> 16) & 0xFF));
    bytes.push_back(static_cast<char>((size >> 8) & 0xFF));
    bytes.push_back(static_cast<char>(size & 0xFF));
    bytes += payload;
    return bytes;
}

std::string BuildFlac(const std::vector<std::string>& entries,
                      bool withCommentBlock,
                      bool withLargePictureBlock) {
    std::string bytes = "fLaC";
    bytes += FlacBlock(0, false, std::string(34, '\0'));  // STREAMINFO
    if (withLargePictureBlock) {
        // A block the parser must skip by length instead of loading it.
        bytes += FlacBlock(6, false, std::string(4096, 'P'));
    }
    if (withCommentBlock) {
        bytes += FlacBlock(4, true, VorbisCommentPayload(entries));
    } else {
        bytes += FlacBlock(1, true, std::string(8, '\0'));  // PADDING as last block
    }
    bytes += std::string(64, '\0');  // stand-in audio frames
    return bytes;
}

// One Ogg page. `continued` marks that the first packet started on an earlier page.
std::string OggPage(bool continued, bool lastPage, const std::vector<std::string>& packets) {
    std::string lacing;
    std::string body;
    for (const auto& packet : packets) {
        size_t remaining = packet.size();
        while (remaining >= 255) {
            lacing.push_back(static_cast<char>(255));
            remaining -= 255;
        }
        lacing.push_back(static_cast<char>(remaining));
        body += packet;
    }
    std::string page = "OggS";
    page.push_back(0);                                   // version
    page.push_back(static_cast<char>(continued ? 0x01 : (lastPage ? 0x04 : 0x00)));
    page.append(8, '\0');                                // granule position
    page.append(4, '\0');                                // serial
    page.append(4, '\0');                                // sequence
    page.append(4, '\0');                                // checksum
    page.push_back(static_cast<char>(lacing.size()));
    page += lacing;
    page += body;
    return page;
}

std::string BuildOgg(const std::vector<std::string>& entries, bool splitCommentAcrossPages) {
    const std::string comment = std::string("\x03vorbis", 7) + VorbisCommentPayload(entries);
    std::string bytes;
    bytes += OggPage(false, false, {std::string("\x01vorbis", 7) + std::string(23, '\0')});
    if (!splitCommentAcrossPages) {
        bytes += OggPage(false, true, {comment});
        return bytes;
    }
    // Split so the comment packet spans two pages: the first page ends with a
    // 255 lacing value, the second page declares a continued packet.
    const size_t splitAt = 255;
    const std::string head = comment.substr(0, splitAt);
    const std::string tail = comment.substr(splitAt);
    std::string first = "OggS";
    first.push_back(0);
    first.push_back(0);
    first.append(8, '\0');
    first.append(4, '\0');
    first.append(4, '\0');
    first.append(4, '\0');
    first.push_back(1);
    first.push_back(static_cast<char>(255));
    first += head;
    bytes += first;

    std::string lacing;
    std::string body;
    size_t remaining = tail.size();
    while (remaining >= 255) {
        lacing.push_back(static_cast<char>(255));
        remaining -= 255;
    }
    lacing.push_back(static_cast<char>(remaining));
    body += tail;
    std::string second = "OggS";
    second.push_back(0);
    second.push_back(0x01);
    second.append(8, '\0');
    second.append(4, '\0');
    second.append(4, '\0');
    second.append(4, '\0');
    second.push_back(static_cast<char>(lacing.size()));
    second += lacing;
    second += body;
    bytes += second;
    return bytes;
}

}  // namespace

int main() {
    Fixture fixture;

    // --- FLAC -------------------------------------------------------------
    const std::vector<std::string> entries = {
        "TITLE=" "Flac Room Tone",
        "ARTIST=" "First Artist",
        "ARTIST=" "Second Artist",
        "ALBUM=" "Field Recordings",
        "COMPOSER=" "ElderScore",
        "BPM=181",
        "INITIALKEY=Dorian",
        "DATE=2034-05-01",
        "DESCRIPTION=" "RoomToneDescription",
        "KEYWORDS=FabricRipple",
        "VENDOR LABEL=" "Zephyr Only",
        "NOTE=" "trailing nul",
    };
    const fs::path flac = fixture.Write("synthetic.flac", BuildFlac(entries, true, true));

    AudioMetadata flacMetadata;
    bool flacCustomRead = false;
    const bool flacOk = MetadataReader::readMetadata(flac.u8string(), flacMetadata, &flacCustomRead);
    Check(flacOk, "FLAC metadata is reported as found");
    Check(flacCustomRead, "FLAC reports that the Vorbis comment block was parsed");
    Check(flacMetadata.customTextTagsRead, "FLAC mirrors the custom-tag flag on the metadata object");
    Check(flacMetadata.title == "Flac Room Tone", "FLAC TITLE maps to title");
    Check(flacMetadata.artist == "First Artist; Second Artist", "FLAC multi-value ARTIST is joined");
    Check(flacMetadata.album == "Field Recordings", "FLAC ALBUM maps to album");
    Check(flacMetadata.composer == "ElderScore", "FLAC COMPOSER maps to composer");
    Check(flacMetadata.bpm == "181", "FLAC BPM maps to bpm");
    Check(flacMetadata.musicalKey == "Dorian", "FLAC INITIALKEY maps to musical key");
    Check(flacMetadata.year == 2034, "FLAC DATE maps to year");
    Check(flacMetadata.description == "RoomToneDescription", "FLAC DESCRIPTION maps to description");
    Check(flacMetadata.keywords == "FabricRipple", "FLAC KEYWORDS maps to keywords");
    Check(flacMetadata.customTextTags.count("VENDOR LABEL") == 1 &&
              flacMetadata.customTextTags["VENDOR LABEL"] == "Zephyr Only",
          "FLAC keeps unknown vendor keys with their original name");
    Check(flacMetadata.customTextTags.count("NOTE") == 1 &&
              flacMetadata.customTextTags["NOTE"] == "trailing nul",
          "FLAC values keep their text without trailing terminators");
    Check(flacMetadata.customTextTags.count("TITLE") == 0,
          "FLAC does not duplicate mapped keys into custom tags");

    const fs::path flacWithoutComment =
        fixture.Write("no_comment.flac", BuildFlac(entries, false, false));
    AudioMetadata noComment;
    bool noCommentCustomRead = true;
    const bool noCommentFound = MetadataReader::readMetadata(
        flacWithoutComment.u8string(), noComment, &noCommentCustomRead);
    Check(!noCommentFound && !noCommentCustomRead,
          "FLAC without a Vorbis comment block reports no tags and no custom-tag read");

    // --- OGG (single page) ------------------------------------------------
    const fs::path ogg = fixture.Write("synthetic.ogg", BuildOgg(entries, false));
    AudioMetadata oggMetadata;
    bool oggCustomRead = false;
    const bool oggOk = MetadataReader::readMetadata(ogg.u8string(), oggMetadata, &oggCustomRead);
    Check(oggOk, "OGG metadata is reported as found");
    Check(oggCustomRead, "OGG reports that the comment packet was parsed");
    Check(oggMetadata.composer == "ElderScore", "OGG COMPOSER maps to composer");
    Check(oggMetadata.bpm == "181", "OGG BPM maps to bpm");
    Check(oggMetadata.musicalKey == "Dorian", "OGG INITIALKEY maps to musical key");
    Check(oggMetadata.customTextTags.count("VENDOR LABEL") == 1,
          "OGG keeps unknown vendor keys");

    // --- OGG (comment packet spanning two pages) --------------------------
    const fs::path splitOgg = fixture.Write("split_comment.ogg", BuildOgg(entries, true));
    AudioMetadata splitMetadata;
    bool splitCustomRead = false;
    const bool splitOk = MetadataReader::readMetadata(splitOgg.u8string(), splitMetadata, &splitCustomRead);
    Check(splitOk, "OGG with a comment packet spanning two pages is still readable");
    Check(splitCustomRead, "OGG spanning packet still reports a complete custom-tag read");
    Check(splitMetadata.composer == "ElderScore", "OGG spanning packet keeps composer");
    Check(splitMetadata.title == "Flac Room Tone", "OGG spanning packet keeps title");

    // --- Vorbis files that carry only music tags -------------------------
    const std::vector<std::string> musicOnlyEntries = {
        "COMPOSER=" "OnlyScore",
        "BPM=199",
        "INITIALKEY=C#m",
        "VENDOR ONLY=" "SoloValue",
    };
    const fs::path musicOnlyFlac =
        fixture.Write("music_only.flac", BuildFlac(musicOnlyEntries, true, false));
    AudioMetadata musicOnlyMetadata;
    bool musicOnlyCustomRead = false;
    Check(MetadataReader::readMetadata(musicOnlyFlac.u8string(), musicOnlyMetadata, &musicOnlyCustomRead),
          "FLAC with only music tags still counts as readable");
    Check(musicOnlyCustomRead, "music-only FLAC still reports a complete comment read");
    Check(musicOnlyMetadata.composer == "OnlyScore" && musicOnlyMetadata.bpm == "199" &&
              musicOnlyMetadata.musicalKey == "C#m",
          "music-only FLAC keeps composer, BPM and key");
    Check(musicOnlyMetadata.customTextTags.count("VENDOR ONLY") == 1,
          "music-only FLAC keeps its custom tag");

    const fs::path musicOnlyOgg =
        fixture.Write("music_only.ogg", BuildOgg(musicOnlyEntries, false));
    AudioMetadata musicOnlyOggMetadata;
    bool musicOnlyOggRead = false;
    Check(MetadataReader::readMetadata(musicOnlyOgg.u8string(), musicOnlyOggMetadata, &musicOnlyOggRead),
          "OGG with only music tags still counts as readable");
    Check(musicOnlyOggMetadata.musicalKey == "C#m" && musicOnlyOggMetadata.bpm == "199",
          "music-only OGG keeps key and BPM");
    Check(musicOnlyOggMetadata.customTextTags.count("VENDOR ONLY") == 1,
          "music-only OGG keeps its custom tag");

    // --- unknown container -------------------------------------------------
    const fs::path unknown = fixture.Write("broken.ogg", "not-an-ogg-container");
    AudioMetadata unknownMetadata;
    Check(!MetadataReader::readMetadata(unknown.u8string(), unknownMetadata),
          "a file that is not FLAC or OGG reads nothing");

    std::cout << "checks=" << checks << " failures=" << failures << std::endl;
    return failures == 0 ? 0 : 1;
}
