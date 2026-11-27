#include "MetadataReader.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using namespace std::string_view_literals;

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
    fs::path path;

    Fixture() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 128; ++attempt) {
            const auto candidate = fs::temp_directory_path() /
                ("harken_mp3_id3_" + std::to_string(tick) + "_" + std::to_string(attempt));
            if (fs::create_directory(candidate)) {
                directory = candidate;
                path = directory / "synthetic.mp3";
                return;
            }
        }
        throw std::runtime_error("could not create a unique MP3 fixture directory");
    }

    ~Fixture() {
        std::error_code error;
        fs::remove(path, error);
        fs::remove(directory, error);
    }
};

void AppendSize(std::string& bytes, size_t size, int width, int bits) {
    for (int shift = (width - 1) * bits; shift >= 0; shift -= bits) {
        bytes.push_back(static_cast<char>((size >> shift) & ((1u << bits) - 1)));
    }
}

void AppendFrame(std::string& frames, int version, std::string_view id,
                 uint8_t encoding, std::string_view body) {
    frames.append(id);
    AppendSize(frames, body.size() + 1, version == 2 ? 3 : 4, version == 4 ? 7 : 8);
    if (version != 2) frames.append(2, '\0');
    frames.push_back(static_cast<char>(encoding));
    frames.append(body);
}

std::string Utf16(std::u16string_view text, bool bigEndian, bool bom) {
    std::string bytes;
    if (bom) bytes.append(bigEndian ? "\xFE\xFF" : "\xFF\xFE");
    for (char16_t unit : text) {
        const auto high = static_cast<char>(unit >> 8);
        const auto low = static_cast<char>(unit & 0xFF);
        bytes.push_back(bigEndian ? high : low);
        bytes.push_back(bigEndian ? low : high);
    }
    return bytes;
}

std::string UserText(std::string_view name, std::string_view value, size_t separatorWidth) {
    std::string body(name);
    body.append(separatorWidth, '\0');
    body.append(value);
    return body;
}

std::string Id3v1Trailer() {
    std::string trailer(128, '\0');
    trailer.replace(0, 3, "TAG");
    trailer.replace(3, 9, "Old title");
    trailer.replace(33, 10, "Old artist");
    trailer.replace(63, 9, "Old album");
    trailer.replace(93, 4, "1980");
    trailer.replace(97, 11, "Old comment");
    return trailer;
}

AudioMetadata ReadFixture(const Fixture& fixture, int version, const std::string& frames,
                          const char* message, std::string_view trailer = {}) {
    std::string bytes("ID3");
    bytes.push_back(static_cast<char>(version));
    bytes.append(2, '\0');
    AppendSize(bytes, frames.size(), 4, 7);
    bytes += frames;
    bytes.append(trailer);
    std::ofstream file(fixture.path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) throw std::runtime_error("could not write MP3 fixture");

    AudioMetadata metadata;
    Check(MetadataReader::readMetadata(fixture.path.u8string(), metadata), message);
    std::ifstream input(fixture.path, std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Check(after == bytes, "metadata reader leaves the MP3 bytes unchanged");
    return metadata;
}

void CheckTag(const AudioMetadata& metadata, const std::string& name,
              const std::string& value, const char* message) {
    const auto tag = metadata.customTextTags.find(name);
    Check(tag != metadata.customTextTags.end() && tag->second == value, message);
}

void CheckId3v23(const Fixture& fixture) {
    std::string frames;
    AppendFrame(frames, 3, "TXXX", 1, UserText(
        Utf16(u"DeScRiPtIoN", false, true), Utf16(u"\u96E8 \u00E9cho", true, true), 2));
    AppendFrame(frames, 3, "TXXX", 1, UserText(
        Utf16(u"Keywords", true, true), Utf16(u"Wind; \u96E8\0"sv, true, false), 2));
    AppendFrame(frames, 3, "TXXX", 1, UserText(
        Utf16(u"replaygain_track_gain", false, true), Utf16(u"-6.25 dB", false, false), 2));
    AppendFrame(frames, 3, "TXXX", 1, UserText(
        Utf16(u"ReplayGain_Album_Gain", false, true), Utf16(u"-4.5 dB", false, true), 2));
    AppendFrame(frames, 3, "TXXX", 1, UserText(
        Utf16(u"Instuments", true, true), Utf16(u"  Piano  ", true, false), 2));
    AppendFrame(frames, 3, "TXXX", 1, UserText(
        Utf16(u"  MiXeD\u0100 \u97F3  ", false, true),
        Utf16(u"\tCaf\u00E9 \u96E8  ", false, false), 2));
    AppendFrame(frames, 3, "COMM", 0, "engnote\0A distinct comment"sv);
    AppendFrame(frames, 3, "TYER", 0, "1998");
    AppendFrame(frames, 3, "TCOM", 0, " A composer \0"sv);
    AppendFrame(frames, 3, "TBPM", 0, "124\0"sv);
    AppendFrame(frames, 3, "TKEY", 0, "F#");

    const auto metadata = ReadFixture(fixture, 3, frames, "reads ID3v2.3 fixture");
    Check(metadata.description == "\xE9\x9B\xA8 \xC3\xA9" "cho",
          "UTF-16 DESCRIPTION uses the separate value BOM without emitting it");
    Check(metadata.keywords == "Wind; \xE9\x9B\xA8",
          "UTF-16 KEYWORDS inherits big-endian byte order with no second BOM");
    Check(metadata.replayGainTrack == -6.25f && metadata.replayGainAlbum == -4.5f,
          "UTF-16 ReplayGain values are decoded and parsed");
    Check(metadata.instruments == "Piano", "legacy vendor instrument alias still works");
    Check(metadata.comment == "A distinct comment", "COMM remains distinct from DESCRIPTION");
    Check(metadata.year == 1998, "TYER still populates the year");
    Check(metadata.composer == "A composer" && metadata.bpm == "124" && metadata.musicalKey == "F#",
          "TCOM, TBPM and TKEY populate the common MP3 string fields without terminators");
    CheckTag(metadata, "  MiXeD\xC4\x80 \xE9\x9F\xB3  ", "\tCaf\xC3\xA9 \xE9\x9B\xA8  ",
             "unknown UTF-16LE tag preserves whitespace, case, and text across an aligned separator");
    CheckTag(metadata, "Instuments", "  Piano  ",
             "custom tag map preserves the original vendor alias and untrimmed value");
    CheckTag(metadata, "DeScRiPtIoN", "\xE9\x9B\xA8 \xC3\xA9" "cho",
             "the recognized DESCRIPTION alias is also retained under its original name");

    std::unordered_map<std::string, std::string> columns;
    PopulateExtendedMeta(columns, metadata);
    const std::unordered_map<std::string, std::string> expected{
        {"track_year", "1998"}, {"composer", "A composer"}, {"bpm", "124"},
        {"musical_key", "F#"}, {"comment", "A distinct comment"}
    };
    Check(columns == expected, "extended metadata exports only recognized column keys");
}

void CheckId3v24(const Fixture& fixture) {
    std::string frames;
    auto noSeparator = Utf16(u"Bad\u0100", false, true);
    noSeparator.push_back('\0');
    AppendFrame(frames, 4, "TXXX", 1, noSeparator);
    AppendFrame(frames, 4, "TXXX", 2, UserText(
        Utf16(u"Edge\u0100", true, false), Utf16(u" \u96E8 ", true, false), 2));
    AppendFrame(frames, 4, "TXXX", 3, UserText("  UTF8 \xCE\xA9  ", std::string(140, 'x'), 1));
    AppendFrame(frames, 4, "TXXX", 0, UserText("Caf\xE9", "  Ol\xE9  \0"sv, 1));
    AppendFrame(frames, 4, "TXXX", 4, UserText("Invalid encoding", "ignored", 1));
    AppendFrame(frames, 4, "TIT2", 3, "Title follows custom frames");
    AppendFrame(frames, 4, "TDRC", 3, "2025-06-13");

    const auto metadata = ReadFixture(fixture, 4, frames, "reads ID3v2.4 fixture");
    Check(metadata.title == "Title follows custom frames", "custom frames keep the next frame aligned");
    Check(metadata.year == 2025, "TDRC preserves year extraction from a recording date");
    Check(metadata.customTextTags.size() == 3,
          "malformed separator and unknown encoding do not produce custom tags");
    CheckTag(metadata, "Edge\xC4\x80", " \xE9\x9B\xA8 ",
             "UTF-16BE without BOM splits only at an aligned two-byte separator");
    CheckTag(metadata, "  UTF8 \xCE\xA9  ", std::string(140, 'x'),
             "UTF-8 custom names and values survive a syncsafe size larger than 127 bytes");
    CheckTag(metadata, "Caf\xC3\xA9", "  Ol\xC3\xA9  ",
             "Latin-1 custom text is decoded without a trailing terminator");
}

void CheckId3v22(const Fixture& fixture) {
    std::string frames;
    AppendFrame(frames, 2, "TT2", 0, "Version 2.2");
    AppendFrame(frames, 2, "TXX", 1, UserText(
        Utf16(u"KEYWORDS", false, true), Utf16(u"\u97F3", false, false), 2));
    AppendFrame(frames, 2, "COM", 0, "eng\0V2.2 comment"sv);
    AppendFrame(frames, 2, "TYE", 0, "2002");
    AppendFrame(frames, 2, "TCM", 0, "Legacy composer");
    AppendFrame(frames, 2, "TBP", 0, "98");
    AppendFrame(frames, 2, "TKE", 0, "Am");

    const auto metadata = ReadFixture(fixture, 2, frames, "reads ID3v2.2 fixture");
    Check(metadata.title == "Version 2.2" && metadata.year == 2002,
          "v2.2 title and year use three-byte frame headers and sizes");
    Check(metadata.keywords == "\xE9\x9F\xB3", "v2.2 TXX decodes UTF-16 KEYWORDS");
    Check(metadata.description.empty() && metadata.comment == "V2.2 comment",
          "v2.2 COM populates comment separately");
    Check(metadata.composer == "Legacy composer" && metadata.bpm == "98" && metadata.musicalKey == "Am",
          "TCM, TBP and the short final TKE frame use the v2.2 equivalents");
    CheckTag(metadata, "KEYWORDS", "\xE9\x9F\xB3", "v2.2 TXX also preserves original custom text");

    frames.clear();
    AppendFrame(frames, 2, "COM", 0, "eng\0V2.2 comment"sv);
    const auto mixed = ReadFixture(fixture, 2, frames, "reads mixed ID3v2.2 / ID3v1 MP3",
                                   Id3v1Trailer());
    Check(mixed.title == "Old title" && mixed.artist == "Old artist" && mixed.album == "Old album",
          "ID3v2.2 retains ID3v1 fallback fields");
    Check(mixed.comment == "V2.2 comment", "ID3v2.2 comment takes precedence over the fallback");
}

void CheckStandaloneTags(const Fixture& fixture) {
    std::string frames;
    AppendFrame(frames, 3, "TXXX", 1, UserText(Utf16(u"Custom", false, true),
                                              Utf16(u"\u97F3", false, false), 2));
    const auto customOnly = ReadFixture(fixture, 3, frames, "an MP3 with only custom text has metadata");
    CheckTag(customOnly, "Custom", "\xE9\x9F\xB3", "custom-only tag is retained");
    frames.clear();
    AppendFrame(frames, 3, "TYER", 0, "1987");
    const auto yearOnly = ReadFixture(fixture, 3, frames, "an MP3 with only a year has metadata");
    std::unordered_map<std::string, std::string> columns;
    PopulateExtendedMeta(columns, yearOnly);
    Check(columns == std::unordered_map<std::string, std::string>{{"track_year", "1987"}},
          "empty fields are omitted from extended metadata");

    const auto mixed = ReadFixture(fixture, 3, frames, "reads sparse ID3v2.3 with an ID3v1 trailer",
                                   Id3v1Trailer());
    Check(mixed.year == 1987 && mixed.title == "Old title" && mixed.comment == "Old comment",
          "sparse ID3v2 keeps existing fallback fields without losing the v2 year");
}

void CheckCustomTagReadOutcome(const Fixture& fixture) {
    const auto writeFixture = [&fixture](std::string_view bytes) {
        std::ofstream file(fixture.path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) throw std::runtime_error("could not write MP3 tag-status fixture");
    };
    const auto expectEmpty = [&fixture](bool trusted, const char* message) {
        AudioMetadata metadata;
        bool customTagsRead = !trusted;
        const bool foundMetadata = MetadataReader::readMetadata(fixture.path.u8string(), metadata, &customTagsRead);
        Check(!foundMetadata && customTagsRead == trusted && metadata.customTextTags.empty(), message);
    };
    std::string audio("\xff\xfb\x90\x64", 4);
    audio.resize(417);
    writeFixture(audio);
    expectEmpty(true, "readable MP3 without an ID3 header permits clearing deleted tags");

    const std::string emptyTag("ID3\x03\0\0\0\0\0\0", 10);
    writeFixture(emptyTag + audio);
    expectEmpty(true, "valid empty ID3 tag permits clearing deleted tags");

    auto truncatedTag = emptyTag;
    truncatedTag.back() = 32;
    writeFixture(truncatedTag);
    expectEmpty(false, "truncated ID3 tag must not erase saved tags");

    fs::remove(fixture.path);
    expectEmpty(false, "unreadable MP3 must not erase saved tags");
}

}

int main() {
    const Fixture fixture;
    CheckId3v23(fixture);
    CheckId3v24(fixture);
    CheckId3v22(fixture);
    CheckStandaloneTags(fixture);
    CheckCustomTagReadOutcome(fixture);
    std::cout << checks - failures << '/' << checks << " MP3 ID3 checks passed\n";
    return failures == 0 ? 0 : 1;
}
