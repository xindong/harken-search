#include "MetadataReader.h"
#include "Id3CommentText.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

struct TempRoot {
    fs::path path;
    bool owned = false;

    TempRoot(std::initializer_list<fs::path> candidates = {}) {
        for (const fs::path& candidate : candidates) {
            if (TryOwn(candidate)) return;
        }

        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 128; ++attempt) {
            const auto nonce = std::random_device{}();
            const fs::path candidate = fs::temp_directory_path() /
                ("aris_metadata_content_" + std::to_string(tick) + "_" +
                 std::to_string(nonce) + "_" + std::to_string(attempt));
            if (TryOwn(candidate)) return;
        }
        throw std::runtime_error("could not create a unique metadata fixture root");
    }

    ~TempRoot() {
        if (owned) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }

private:
    bool TryOwn(const fs::path& candidate) {
        std::error_code ec;
        const bool created = fs::create_directories(candidate, ec);
        if (!created) return false;
        path = candidate;
        owned = true;
        return true;
    }
};

void AppendAscii(std::vector<uint8_t>& out, const char* text, size_t count) {
    out.insert(out.end(), text, text + count);
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

void AppendSyncSafe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 21) & 0x7F));
    out.push_back(static_cast<uint8_t>((value >> 14) & 0x7F));
    out.push_back(static_cast<uint8_t>((value >> 7) & 0x7F));
    out.push_back(static_cast<uint8_t>(value & 0x7F));
}

void AppendFrame(std::vector<uint8_t>& tag,
                 const char* id,
                 uint8_t encoding,
                 const std::vector<uint8_t>& text) {
    AppendAscii(tag, id, 4);
    AppendBe32(tag, static_cast<uint32_t>(text.size() + 1));
    tag.push_back(0);
    tag.push_back(0);
    tag.push_back(encoding);
    tag.insert(tag.end(), text.begin(), text.end());
}

std::vector<uint8_t> Utf16Bytes(bool bigEndian,
                                const std::vector<uint16_t>& units,
                                bool withBom = true) {
    std::vector<uint8_t> out;
    if (withBom) {
        out.insert(out.end(), bigEndian ? std::initializer_list<uint8_t>{0xFE, 0xFF}
                                       : std::initializer_list<uint8_t>{0xFF, 0xFE});
    }
    for (uint16_t unit : units) {
        if (bigEndian) {
            out.push_back(static_cast<uint8_t>(unit >> 8));
            out.push_back(static_cast<uint8_t>(unit & 0xFF));
        } else {
            out.push_back(static_cast<uint8_t>(unit & 0xFF));
            out.push_back(static_cast<uint8_t>(unit >> 8));
        }
    }
    return out;
}

std::vector<uint8_t> TxxxBytes(const char* description, const char* value) {
    std::vector<uint8_t> out(description, description + std::strlen(description));
    out.push_back(0);
    out.insert(out.end(), value, value + std::strlen(value));
    return out;
}

std::vector<uint8_t> Utf16TxxxBytes(const char* description, const char* value) {
    std::vector<uint16_t> units;
    for (const char* p = description; *p; ++p) units.push_back(static_cast<uint8_t>(*p));
    units.push_back(0);
    for (const char* p = value; *p; ++p) units.push_back(static_cast<uint8_t>(*p));
    return Utf16Bytes(false, units);
}

bool WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

bool WriteId3v23(const fs::path& path, std::vector<uint8_t> tag) {
    tag.push_back(0);
    std::vector<uint8_t> file;
    AppendAscii(file, "ID3", 3);
    file.push_back(3);
    file.push_back(0);
    file.push_back(0);
    AppendSyncSafe32(file, static_cast<uint32_t>(tag.size()));
    file.insert(file.end(), tag.begin(), tag.end());
    return WriteBytes(path, file);
}

bool WriteId3v22(const fs::path& path, const std::vector<uint8_t>& tag) {
    std::vector<uint8_t> file;
    AppendAscii(file, "ID3", 3);
    file.push_back(2);
    file.push_back(0);
    file.push_back(0);
    AppendSyncSafe32(file, static_cast<uint32_t>(tag.size()));
    file.insert(file.end(), tag.begin(), tag.end());
    return WriteBytes(path, file);
}

bool WriteId3v1(const fs::path& path, const std::vector<uint8_t>& title) {
    std::vector<uint8_t> bytes(128, 0);
    bytes[0] = 'T';
    bytes[1] = 'A';
    bytes[2] = 'G';
    for (size_t i = 0; i < title.size() && i < 30; ++i) bytes[3 + i] = title[i];
    return WriteBytes(path, bytes);
}

std::string Bytes(std::initializer_list<uint8_t> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.begin()), bytes.size());
}

} // namespace

int main() {
    TempRoot temp;
    const fs::path occupiedPath = temp.path / "occupied_candidate";
    const fs::path ownedPath = temp.path / "owned_candidate";
    Check(fs::create_directories(occupiedPath), "create occupied temp candidate");
    Check(WriteBytes(occupiedPath / "marker", {1}), "write occupied candidate marker");
    {
        TempRoot retrying({occupiedPath, ownedPath});
        Check(retrying.path == ownedPath,
              "temp fixture retries after an occupied candidate");
    }
    Check(fs::exists(occupiedPath / "marker"),
          "temp fixture never deletes an unowned collision");

    const fs::path id3v2Path = temp.path / "content_matrix.mp3";
    std::vector<uint8_t> tag;
    AppendFrame(tag, "TIT2", 0, {'C', 'P', ' ', 0x80, ' ', 0x93, 'Q', 0x94});
    AppendFrame(tag, "TPE1", 1,
                Utf16Bytes(false, {'L', 'E', ' ', 0xD83D, 0xDE00, '!', 0, 'X'}));
    AppendFrame(tag, "TALB", 1, Utf16Bytes(true, {'B', 'E', ' ', 0x03A9, 0}));
    auto odd = Utf16Bytes(false, {'O', 'd', 'd'}, false);
    odd.push_back(0xFF);
    AppendFrame(tag, "TCON", 1, odd);
    AppendFrame(tag, "COMM", 1, Utf16Bytes(false, {0xD83D, 0}));
    AppendFrame(tag, "TPE2", 0, {'P', 0, 'H'});
    Check(WriteId3v23(id3v2Path, tag), "write synthetic ID3v2 content fixture");

    AudioMetadata metadata;
    Check(MetadataReader::readMetadata(id3v2Path.u8string(), metadata),
          "real MetadataReader TU parses the synthetic ID3v2 fixture");
#ifdef _WIN32
    Check(metadata.title == Bytes({'C', 'P', ' ', 0xE2, 0x82, 0xAC, ' ',
                                   0xE2, 0x80, 0x9C, 'Q', 0xE2, 0x80, 0x9D}),
          "ID3v2 encoding 0 uses Windows-1252 euro and smart quotes");
    Check(metadata.artist == Bytes({'L', 'E', ' ', 0xF0, 0x9F, 0x98, 0x80, '!'}),
          "UTF-16LE combines a surrogate-pair emoji and trims at NUL");
    Check(metadata.comment.empty(),
          "COMM without a language and descriptor terminator has no comment text");
#else
    Check(metadata.title == Bytes({'C', 'P', ' ', 0xC2, 0x80, ' ', 0xC2,
                                   0x93, 'Q', 0xC2, 0x94}),
          "non-Windows encoding 0 preserves the ISO-8859-1 fallback mapping");
    Check(metadata.artist == Bytes({'L', 'E', ' ', 0xED, 0xA0, 0xBD,
                                    0xED, 0xB8, 0x80, '!'}),
          "non-Windows UTF-16 fallback preserves separate surrogate units");
    Check(metadata.comment.empty(),
          "COMM without a language and descriptor terminator has no comment text");
#endif
    Check(metadata.album == Bytes({'B', 'E', ' ', 0xCE, 0xA9}),
          "UTF-16BE BOM selects big-endian decoding and trims at NUL");
    Check(metadata.genre == "Odd",
          "odd UTF-16 byte length ignores the dangling byte");
    Check(metadata.albumArtist.size() == 3 && metadata.albumArtist[0] == 'P' &&
              metadata.albumArtist[1] == '\0' && metadata.albumArtist[2] == 'H',
          "encoding 0 preserves an embedded NUL and its suffix");

    const fs::path commentPath = temp.path / "comment_frame.mp3";
    std::vector<uint8_t> commentTag;
    std::vector<uint8_t> commentBody = {'e', 'n', 'g', 0xFF, 0xFE, 0, 0,
                                        0xFF, 0xFE, 'A', 0, 0xF3, 0x97, 0, 0};
    AppendFrame(commentTag, "COMM", 1, commentBody);
    Check(WriteId3v23(commentPath, commentTag), "write UTF-16 comment fixture");
    AudioMetadata commentMetadata;
    Check(MetadataReader::readMetadata(commentPath.u8string(), commentMetadata) &&
              commentMetadata.comment == "A\xE9\x9F\xB3",
          "COMM skips language and short description before decoding comment text");
    const std::string storedComment(
        reinterpret_cast<const char*>(commentBody.data()), commentBody.size());
    Check(Id3CommentText::RecoverStoredLegacy(storedComment) ==
              std::optional<std::string>("A\xE9\x9F\xB3"),
          "legacy database bytes recover the original comment for list display");
    const fs::path id3v22CommentPath = temp.path / "comment_v22.mp3";
    const std::vector<uint8_t> id3v22CommentTag = {
        'C', 'O', 'M', 0, 0, 9, 0, 'e', 'n', 'g', 0, '2', '.', '2', '!'
    };
    Check(WriteId3v22(id3v22CommentPath, id3v22CommentTag),
          "write mixed ID3v2.2 and ID3v1 fixture");
    std::vector<uint8_t> id3v1Trailer(128, 0);
    id3v1Trailer[0] = 'T';
    id3v1Trailer[1] = 'A';
    id3v1Trailer[2] = 'G';
    std::memcpy(id3v1Trailer.data() + 3, "Old Title", 9);
    std::memcpy(id3v1Trailer.data() + 33, "Old Artist", 10);
    std::memcpy(id3v1Trailer.data() + 63, "Old Album", 9);
    {
        std::ofstream append(id3v22CommentPath, std::ios::binary | std::ios::app);
        append.write(reinterpret_cast<const char*>(id3v1Trailer.data()),
                     static_cast<std::streamsize>(id3v1Trailer.size()));
        Check(append.good(), "append ID3v1 fallback fields");
    }
    AudioMetadata id3v22CommentMetadata;
    Check(MetadataReader::readMetadata(id3v22CommentPath.u8string(), id3v22CommentMetadata) &&
              id3v22CommentMetadata.title == "Old Title" &&
              id3v22CommentMetadata.artist == "Old Artist" &&
              id3v22CommentMetadata.album == "Old Album",
          "mixed ID3v2.2 and ID3v1 retains existing fallback fields");
    std::string truncatedStoredComment = storedComment;
    truncatedStoredComment.pop_back();
    truncatedStoredComment.back() = '6';
    Check(Id3CommentText::RecoverStoredLegacy(truncatedStoredComment) ==
              std::optional<std::string>("A\xE9\x9F\xB3" "6"),
          "legacy comment recovers a final ASCII digit whose UTF-16 high byte was lost");
    Check(!Id3CommentText::RecoverStoredLegacy("Normal UTF-8 description"),
          "normal database descriptions are not rewritten by legacy recovery");
    const fs::path englishCommentPath = temp.path / "english_comment.mp3";
    std::vector<uint8_t> englishTag;
    std::vector<uint8_t> englishBody = {'e', 'n', 'g'};
    const auto englishDescriptor = Utf16Bytes(false, {'n', 'o', 't', 'e', 0});
    const auto englishValue = Utf16Bytes(false, {'H', 'e', 'l', 'l', 'o', 0});
    englishBody.insert(englishBody.end(), englishDescriptor.begin(), englishDescriptor.end());
    englishBody.insert(englishBody.end(), englishValue.begin(), englishValue.end());
    AppendFrame(englishTag, "COMM", 1, englishBody);
    Check(WriteId3v23(englishCommentPath, englishTag), "write English comment fixture");
    AudioMetadata englishComment;
    Check(MetadataReader::readMetadata(englishCommentPath.u8string(), englishComment) &&
              englishComment.comment == "Hello",
          "COMM skips a nonempty short descriptor");

    const fs::path utf8CommentPath = temp.path / "utf8_comment.mp3";
    std::vector<uint8_t> utf8CommentTag;
    const std::string longComment = std::string(180, 'A') + "中文";
    std::vector<uint8_t> utf8CommentBody = {'z', 'h', 'o', 'n', 'o', 't', 'e', 0};
    utf8CommentBody.insert(utf8CommentBody.end(), longComment.begin(), longComment.end());
    AppendFrame(utf8CommentTag, "COMM", 3, utf8CommentBody);
    Check(WriteId3v23(utf8CommentPath, utf8CommentTag), "write long UTF-8 comment fixture");
    AudioMetadata utf8Comment;
    Check(MetadataReader::readMetadata(utf8CommentPath.u8string(), utf8Comment) &&
              utf8Comment.comment == longComment,
          "COMM preserves a long UTF-8 comment after its descriptor");

    const fs::path emptyCommentPath = temp.path / "empty_comment.mp3";
    std::vector<uint8_t> emptyCommentTag;
    AppendFrame(emptyCommentTag, "COMM", 1, {'e', 'n', 'g', 0xFF, 0xFE, 0, 0});
    Check(WriteId3v23(emptyCommentPath, emptyCommentTag), "write empty comment fixture");
    AudioMetadata emptyComment;
    Check(!MetadataReader::readMetadata(emptyCommentPath.u8string(), emptyComment) &&
              emptyComment.comment.empty(),
          "COMM keeps an empty comment empty");

    const fs::path utf8Path = temp.path / "encoding3_direct.mp3"; std::vector<uint8_t> utf8Tag;
    AppendFrame(utf8Tag, "TIT2", 3, {'U', 'T', 'F', '8'});
    Check(WriteId3v23(utf8Path, utf8Tag), "write encoding-3 direct fixture");
    AudioMetadata utf8Metadata;
    Check(MetadataReader::readMetadata(utf8Path.u8string(), utf8Metadata) &&
              utf8Metadata.title == "UTF8",
          "encoding-3 keeps direct UTF-8 bytes");

    const fs::path unknownPath = temp.path / "encoding_unknown.mp3";
    std::vector<uint8_t> unknownTag;
    AppendFrame(unknownTag, "TIT2", 4, {'i'});
    Check(WriteId3v23(unknownPath, unknownTag), "write unknown encoding fixture");
    AudioMetadata unknownMetadata;
    unknownMetadata.title = "stale";
    Check(!MetadataReader::readMetadata(unknownPath.u8string(), unknownMetadata) &&
              unknownMetadata.title.empty(),
          "unknown encoding maps empty content and returns false");

    const fs::path emptyUtf8Path = temp.path / "encoding3_empty.mp3";
    std::vector<uint8_t> emptyUtf8Tag;
    AppendFrame(emptyUtf8Tag, "TIT2", 3, {});
    Check(WriteId3v23(emptyUtf8Path, emptyUtf8Tag), "write empty encoding-3 fixture");
    AudioMetadata emptyUtf8Metadata;
    emptyUtf8Metadata.title = "stale";
    Check(!MetadataReader::readMetadata(emptyUtf8Path.u8string(), emptyUtf8Metadata) &&
              emptyUtf8Metadata.title.empty(),
          "empty encoding-3 clears title and returns false");

    const fs::path tyerPath = temp.path / "tyer_only.mp3";
    std::vector<uint8_t> tyerTag;
    AppendFrame(tyerTag, "TYER", 0, {'1', '9', '9', '8'});
    Check(WriteId3v23(tyerPath, tyerTag), "write TYER fixture");
    AudioMetadata tyer;
    Check(!MetadataReader::readMetadata(tyerPath.u8string(), tyer) &&
              tyer.year == 1998,
          "TYER maps decoded content but year alone does not satisfy hasData");

    const fs::path numericPath = temp.path / "numeric_frames.mp3";
    std::vector<uint8_t> numericTag;
    AppendFrame(numericTag, "TDRC", 1,
                Utf16Bytes(false, {'2', '0', '2', '4', '-', '0', '7'}));
    AppendFrame(numericTag, "TRCK", 0, {'7', '/', '1', '2'});
    AppendFrame(numericTag, "TPOS", 1, Utf16Bytes(false, {'2', '/', '3'}));
    Check(WriteId3v23(numericPath, numericTag), "write numeric frame fixture");
    AudioMetadata numeric;
    Check(!MetadataReader::readMetadata(numericPath.u8string(), numeric) &&
              numeric.year == 2024 && numeric.track == 7 && numeric.disc == 2,
          "TDRC TRCK and TPOS map decoded prefixes but do not satisfy hasData");
    AudioMetadata staleNumeric;
    staleNumeric.title = "stale";
    Check(MetadataReader::readMetadata(numericPath.u8string(), staleNumeric) &&
              staleNumeric.title == "stale" && staleNumeric.year == 2024 &&
              staleNumeric.track == 7 && staleNumeric.disc == 2,
          "pre-existing mapped hasData field controls the numeric-frame return");

    const fs::path latinTxxxPath = temp.path / "latin_txxx.mp3";
    std::vector<uint8_t> latinTxxxTag;
    AppendFrame(latinTxxxTag, "TXXX", 0,
                TxxxBytes("REPLAYGAIN_TRACK_GAIN", "-7.25 dB"));
    AppendFrame(latinTxxxTag, "TXXX", 0,
                TxxxBytes("REPLAYGAIN_ALBUM_GAIN", "-4.5 dB"));
    AppendFrame(latinTxxxTag, "TXXX", 0,
                TxxxBytes("INSTRUMENTS", "  prepared piano  "));
    Check(WriteId3v23(latinTxxxPath, latinTxxxTag), "write encoding-0 TXXX fixture");
    AudioMetadata latinTxxx;
    Check(!MetadataReader::readMetadata(latinTxxxPath.u8string(), latinTxxx) &&
              latinTxxx.replayGainTrack == -7.25f &&
              latinTxxx.replayGainAlbum == -4.5f &&
              latinTxxx.instruments == "prepared piano",
          "encoding-0 TXXX preserves its NUL separator but mapped fields do not satisfy hasData");

    const fs::path utf16TxxxPath = temp.path / "utf16_txxx.mp3";
    std::vector<uint8_t> utf16TxxxTag;
    AppendFrame(utf16TxxxTag, "TXXX", 1,
                Utf16TxxxBytes("REPLAYGAIN_TRACK_GAIN", "-1.0 dB"));
    AppendFrame(utf16TxxxTag, "TXXX", 1,
                Utf16TxxxBytes("INSTRUMENTS", "violin"));
    Check(WriteId3v23(utf16TxxxPath, utf16TxxxTag), "write UTF-16 TXXX fixture");
    AudioMetadata utf16Txxx;
    utf16Txxx.replayGainTrack = 3.0f;
    utf16Txxx.instruments = "stale instrument";
    Check(!MetadataReader::readMetadata(utf16TxxxPath.u8string(), utf16Txxx) &&
              utf16Txxx.replayGainTrack == 3.0f &&
              utf16Txxx.instruments == "stale instrument",
          "UTF-16 first-NUL truncation removes the TXXX separator and preserves public fields");

    const fs::path id3v1Path = temp.path / "id3v1_raw.mp3";
    const std::vector<uint8_t> rawTitle = {0x80, 0x93, 'V', 0x94};
    Check(WriteId3v1(id3v1Path, rawTitle), "write synthetic ID3v1 fixture");
    AudioMetadata id3v1;
    Check(MetadataReader::readMetadata(id3v1Path.u8string(), id3v1) &&
              id3v1.title == Bytes({0x80, 0x93, 'V', 0x94}),
          "ID3v1 preserves legacy bytes without applying ID3v2 CP1252 conversion");

    const fs::path emptyUtf16Path = temp.path / "empty_utf16.mp3";
    std::vector<uint8_t> emptyTag;
    AppendFrame(emptyTag, "TIT2", 1, {0xFF, 0xFE, 0x41});
    Check(WriteId3v23(emptyUtf16Path, emptyTag), "write malformed UTF-16 fixture");
    AudioMetadata emptyUtf16;
    emptyUtf16.title = "stale";
    Check(!MetadataReader::readMetadata(emptyUtf16Path.u8string(), emptyUtf16) &&
              emptyUtf16.title.empty(),
          "malformed UTF-16 mapped title clears that field and returns false");

    const fs::path badHeaderPath = temp.path / "bad_header.mp3";
    Check(WriteBytes(badHeaderPath, {1, 2, 3}), "write invalid ID3 header fixture");
    AudioMetadata badHeader;
    badHeader.title = "stale";
    Check(!MetadataReader::readMetadata(badHeaderPath.u8string(), badHeader) &&
              badHeader.title == "stale",
          "failure before frame mapping leaves caller metadata unchanged");

    std::cout << (failures == 0 ? "PASS" : "FAIL")
              << ": MetadataReader tag-content encoding characterization\n";
    return failures == 0 ? 0 : 1;
}
