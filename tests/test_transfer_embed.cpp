#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/EmbedScript.h"
#include "../src/HarkenMeta.h"
#include "../src/MetadataReader.h"
#include "../src/MetadataWriter.h"

static int failed = 0;
static int passed = 0;

static void CHECK(bool condition, const char* desc) {
    if (condition) {
        ++passed;
        std::printf("  PASS: %s\n", desc);
    } else {
        ++failed;
        std::printf("  FAIL: %s\n", desc);
    }
}

static void WriteBareWav(const std::string& path, uint32_t sampleRate, uint32_t frames) {
    auto le16 = [](std::vector<uint8_t>& out, uint16_t value) {
        out.push_back((uint8_t)(value & 0xFF));
        out.push_back((uint8_t)((value >> 8) & 0xFF));
    };
    auto le32 = [](std::vector<uint8_t>& out, uint32_t value) {
        out.push_back((uint8_t)(value & 0xFF));
        out.push_back((uint8_t)((value >> 8) & 0xFF));
        out.push_back((uint8_t)((value >> 16) & 0xFF));
        out.push_back((uint8_t)((value >> 24) & 0xFF));
    };

    const uint32_t dataBytes = frames * 2;
    std::vector<uint8_t> wav;
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    le32(wav, 36 + dataBytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    le32(wav, 16);
    le16(wav, 1);
    le16(wav, 1);
    le32(wav, sampleRate);
    le32(wav, sampleRate * 2);
    le16(wav, 2);
    le16(wav, 16);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    le32(wav, dataBytes);
    wav.insert(wav.end(), dataBytes, 0);

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(wav.data()), (std::streamsize)wav.size());
}

static void WriteWavWithId3(const std::string& path) {
    WriteBareWav(path, 48000, 480);
    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> wav((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    input.close();
    const std::vector<uint8_t> id3 = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0};
    wav.insert(wav.end(), {'I', 'D', '3', ' '});
    const uint32_t id3Size = static_cast<uint32_t>(id3.size());
    wav.push_back(static_cast<uint8_t>(id3Size & 0xFF));
    wav.push_back(static_cast<uint8_t>((id3Size >> 8) & 0xFF));
    wav.push_back(static_cast<uint8_t>((id3Size >> 16) & 0xFF));
    wav.push_back(static_cast<uint8_t>((id3Size >> 24) & 0xFF));
    wav.insert(wav.end(), id3.begin(), id3.end());
    const uint32_t riffSize = static_cast<uint32_t>(wav.size() - 8);
    wav[4] = static_cast<uint8_t>(riffSize & 0xFF);
    wav[5] = static_cast<uint8_t>((riffSize >> 8) & 0xFF);
    wav[6] = static_cast<uint8_t>((riffSize >> 16) & 0xFF);
    wav[7] = static_cast<uint8_t>((riffSize >> 24) & 0xFF);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(wav.data()),
                 static_cast<std::streamsize>(wav.size()));
}

static void WriteMinimalFlac(const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    const char header[] = {'f', 'L', 'a', 'C'};
    out.write(header, 4);
    out.put(static_cast<char>(0x80));  // last metadata block, type 0 (STREAMINFO)
    out.put(0); out.put(0); out.put(34);
    std::string streamInfo(34, '\0');
    out.write(streamInfo.data(), static_cast<std::streamsize>(streamInfo.size()));
}

int main() {
    std::printf("=== TransferEmbed Tests ===\n");
    const std::string wav = "test_transfer_embed_tmp.wav";

    std::printf("\n--- TimeReference ---\n");
    WriteBareWav(wav, 48000, 96000);
    {
        MetadataFieldPatch patch;
        patch.bwTimeReference = 240000;
        patch.hasTimeReference = true;
        std::string error;
        bool wrote = MetadataWriter::writeMetadataFields(wav, patch, error);
        CHECK(wrote, ("writeMetadataFields(TimeReference) ok: " + error).c_str());

        AudioMetadata metadata;
        MetadataReader::readMetadata(wav, metadata);
        CHECK(metadata.bwTimeStamp == 240000, "bext TimeReference round-trips == 240000");
    }

    std::printf("\n--- iXML chunk ---\n");
    WriteBareWav(wav, 48000, 96000);
    {
        IXMLData ix;
        ix.project = "DragProj";
        ix.trackList.push_back({1, "M"});
        MetadataFieldPatch patch;
        patch.ixmlChunk = EmbedEngine::serializeIXML(ix);
        std::string error;
        bool wrote = MetadataWriter::writeMetadataFields(wav, patch, error);
        CHECK(wrote, ("writeMetadataFields(iXML) ok: " + error).c_str());

        std::ifstream in(wav, std::ios::binary);
        std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(blob.find("iXML") != std::string::npos, "WAV contains iXML chunk id");
        CHECK(blob.find("<PROJECT>DragProj</PROJECT>") != std::string::npos, "iXML payload embedded");
    }

    std::printf("\n--- HARKEN section ---\n");
    WriteBareWav(wav, 48000, 96000);
    {
        IXMLData ix;
        ix.project = "KeepMe";
        MetadataFieldPatch patch;
        patch.ixmlChunk = EmbedEngine::serializeIXML(ix);
        std::string error;
        CHECK(MetadataWriter::writeMetadataFields(wav, patch, error), "seed iXML ok");

        const std::string json =
            R"json({"category":"Weapons","function":"Impact","source":"ai"})json";
        CHECK(MetadataWriter::writeHarkenMetadata(wav, json, error),
              ("writeHarkenMetadata ok: " + error).c_str());

        std::string xml;
        CHECK(MetadataReader::readIXMLChunk(wav, xml, error), "read iXML chunk ok");
        CHECK(xml.find("<PROJECT>KeepMe</PROJECT>") != std::string::npos,
              "existing iXML PROJECT preserved");
        CHECK(xml.find("<HARKEN>") != std::string::npos, "HARKEN section inserted");
        CHECK(xml.find("<CATEGORY>Weapons</CATEGORY>") != std::string::npos, "HARKEN category written");
        CHECK(xml.find("<FUNCTION>Impact</FUNCTION>") != std::string::npos, "HARKEN function written");

        std::ifstream in(wav, std::ios::binary);
        std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        size_t chunkCount = 0;
        for (size_t pos = blob.find("iXML"); pos != std::string::npos;
             pos = blob.find("iXML", pos + 4)) {
            ++chunkCount;
        }
        CHECK(chunkCount == 1, "exactly one iXML chunk after HARKEN merge");
    }

    std::printf("\n--- HARKEN empty write ---\n");
    WriteBareWav(wav, 48000, 96000);
    {
        std::string error;
        CHECK(MetadataWriter::writeHarkenMetadata(wav, "", error), "empty HARKEN is a no-op");
        std::ifstream in(wav, std::ios::binary);
        std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(blob.find("iXML") == std::string::npos, "no iXML chunk created for empty values");
    }

    std::printf("\n--- FLAC HARKEN block ---\n");
    const std::string flac = "test_transfer_embed_tmp.flac";
    WriteMinimalFlac(flac);
    {
        std::string error;
        CHECK(MetadataWriter::writeHarkenMetadata(
                  flac, R"json({"category":"Ambience","function":"Rain"})json", error),
              ("writeHarkenMetadata(FLAC) ok: " + error).c_str());
        std::ifstream in(flac, std::ios::binary);
        std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(blob.find("HARK") != std::string::npos, "FLAC HARK application block written");
        CHECK(blob.find("<CATEGORY>Ambience</CATEGORY>") != std::string::npos,
              "FLAC HARKEN payload written");
        CHECK(blob.find("fLaC") == 0, "FLAC marker preserved");
    }

    std::printf("\n--- Cover removal result semantics ---\n");
    const std::string coveredWav = "test_transfer_embed_cover.wav";
    WriteWavWithId3(coveredWav);
    CHECK(MetadataWriter::removeAlbumArt(coveredWav),
          "WAV ID3 removal reports Removed");

    const std::string bareWav = "test_transfer_embed_bare.wav";
    WriteBareWav(bareWav, 48000, 480);
    CHECK(!MetadataWriter::removeAlbumArt(bareWav),
          "valid WAV without ID3 reports NoCover");

    const std::string bareMp3 = "test_transfer_embed_bare.mp3";
    {
        std::ofstream output(bareMp3, std::ios::binary);
        output << "audio-without-id3";
    }
    CHECK(!MetadataWriter::removeAlbumArt(bareMp3),
          "valid readable MP3 without ID3 reports NoCover");

    const std::string unsupportedOgg = "test_transfer_embed_unsupported.ogg";
    {
        std::ofstream output(unsupportedOgg, std::ios::binary);
        output << "OggS";
    }
    CHECK(!MetadataWriter::removeAlbumArt(unsupportedOgg),
          "unsupported Ogg removal reports Failed");
    CHECK(!MetadataWriter::removeAlbumArt("missing-cover-source.wav"),
          "missing source reports Failed");

    std::remove(wav.c_str());
    std::remove(coveredWav.c_str());
    std::remove(bareWav.c_str());
    std::remove(bareMp3.c_str());
    std::remove(unsupportedOgg.c_str());
    std::remove(flac.c_str());
    std::printf("\n=== TransferEmbed: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
