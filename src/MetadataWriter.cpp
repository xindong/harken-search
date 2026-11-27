#include "MetadataWriter.h"
#include "Localization.h"
#include "HarkenMeta.h"
#include "MetadataReader.h"
#include "platform/filesystem.h"
#include "platform/text_encoding.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <set>
#include <string_view>

namespace fs = std::filesystem;

// Convert UTF-8 to wide string for Windows file operations
static std::wstring Utf8ToWide(const std::string& str) {
    return Platform::TextEncoding::Utf8ToWidePermissive(str);
}

static bool ReadBinaryFile(const std::string& path,
                           std::vector<uint8_t>& data) {
    Platform::FileSystem::StreamReader reader;
    if (!reader.Open(path, true) ||
        !reader.Seek(0, Platform::FileSystem::StreamSeekOrigin::End)) {
        return false;
    }
    const int64_t size = reader.Tell();
    if (size < 0 || !reader.Seek(0, Platform::FileSystem::StreamSeekOrigin::Begin)) {
        return false;
    }
    data.resize(static_cast<size_t>(size));
    return data.empty() || reader.Read(data.data(), data.size()) == data.size();
}

static bool WriteBinaryFile(
    const std::string& path,
    std::initializer_list<std::string_view> chunks) {
    Platform::FileSystem::StreamWriter writer;
    if (!writer.Open(path, false, true)) return false;
    for (const auto chunk : chunks) {
        if (!writer.Write(chunk)) return false;
    }
    return writer.Flush();
}

static bool WriteBinaryFile(const std::string& path,
                            const std::vector<uint8_t>& data) {
    if (data.empty()) return WriteBinaryFile(path, {});
    return WriteBinaryFile(path, {
        std::string_view(reinterpret_cast<const char*>(data.data()), data.size())});
}

static void writeBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((v >> 24) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back(v & 0xFF);
}

static void writeBE64(std::vector<uint8_t>& out, uint64_t v) {
    out.push_back((v >> 56) & 0xFF);
    out.push_back((v >> 48) & 0xFF);
    out.push_back((v >> 40) & 0xFF);
    out.push_back((v >> 32) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back(v & 0xFF);
}

static uint32_t readBE32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint64_t readBE64(const uint8_t* p) {
    return (uint64_t)p[0] << 56 | (uint64_t)p[1] << 48 | (uint64_t)p[2] << 40 | (uint64_t)p[3] << 32 |
           (uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 | (uint64_t)p[6] << 8 | (uint64_t)p[7];
}

std::string MetadataWriter::getExtension(const std::string& filePath) {
    std::string ext;
#ifdef _WIN32
    std::wstring wpath = Utf8ToWide(filePath);
    fs::path fsPath(wpath);
    std::wstring wext = fsPath.extension().wstring();
    ext = Platform::TextEncoding::WideToUtf8Permissive(wext);
#else
    ext = fs::path(filePath).extension().string();
#endif
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

bool MetadataWriter::loadImageFile(const std::string& imagePath,
                                   std::vector<uint8_t>& imageData,
                                   std::string& mimeType) {
    Platform::FileSystem::StreamReader file;
    if (!file.Open(imagePath, true)) return false;
    
    // Read entire file
    if (!file.Seek(0, Platform::FileSystem::StreamSeekOrigin::End)) return false;
    const int64_t fileSizeValue = file.Tell();
    if (fileSizeValue < 0) return false;
    const size_t fileSize = static_cast<size_t>(fileSizeValue);
    if (!file.Seek(0, Platform::FileSystem::StreamSeekOrigin::Begin)) return false;
    
    imageData.resize(fileSize);
    if (file.Read(imageData.data(), fileSize) != fileSize) return false;
    
    // Detect MIME type from header
    if (imageData.size() >= 3 && imageData[0] == 0xFF && imageData[1] == 0xD8 && imageData[2] == 0xFF) {
        mimeType = "image/jpeg";
    } else if (imageData.size() >= 8 && imageData[0] == 0x89 && imageData[1] == 'P' &&
               imageData[2] == 'N' && imageData[3] == 'G') {
        mimeType = "image/png";
    } else {
        // Unknown format, assume JPEG
        mimeType = "image/jpeg";
    }
    
    return true;
}

bool MetadataWriter::writeAlbumArt(const std::string& audioFilePath,
                                   const std::vector<uint8_t>& imageData,
                                   const std::string& mimeType) {
    if (Platform::FileSystem::IsNetworkPath(audioFilePath)) {
        return false; // L(Write_BlockedNetworkLibrary)
    }
     std::string ext = getExtension(audioFilePath);
    
    if (ext == ".mp3") {
        return writeID3v2AlbumArt(audioFilePath, imageData, mimeType);
    } else if (ext == ".wav" || ext == ".wave") {
        return writeWAVID3AlbumArt(audioFilePath, imageData, mimeType);
    } else if (ext == ".ogg" || ext == ".opus" || ext == ".oga") {
        return writeOGGAlbumArt(audioFilePath, imageData, mimeType);
    } else if (ext == ".flac") {
        return writeFLACAlbumArt(audioFilePath, imageData, mimeType);
    } else if (ext == ".m4a" || ext == ".aac" || ext == ".mp4") {
        return writeM4AAlbumArt(audioFilePath, imageData, mimeType);
    }
    
    return false;
}

bool MetadataWriter::writeAlbumArtFromFile(const std::string& audioFilePath,
                                           const std::string& imageFilePath) {
    std::vector<uint8_t> imageData;
    std::string mimeType;
    
    if (!loadImageFile(imageFilePath, imageData, mimeType)) {
        return false;
    }
    
    return writeAlbumArt(audioFilePath, imageData, mimeType);
}

// Forward declarations for MP3 ID3 helpers (defined below)
static uint32_t readUint32BE(const uint8_t* p);
static uint32_t readSyncSafeInt(const uint8_t* p);
static bool isFrameIdValid4(const uint8_t* id);
static void appendApicFrame(std::vector<uint8_t>& outFrames,
                            int id3MajorVersion,
                            const std::vector<uint8_t>& imageData,
                            const std::string& mimeType);

bool MetadataWriter::removeAlbumArt(const std::string& audioFilePath) {
    if (Platform::FileSystem::IsNetworkPath(audioFilePath)) {
        return false; // L(Write_BlockedNetworkLibrary)
    }
     std::string ext = getExtension(audioFilePath);
    if (ext == ".wav" || ext == ".wave") {
        // Remove WAV ID3 chunk by rewriting RIFF
        std::vector<uint8_t> fileData;
        if (!ReadBinaryFile(audioFilePath, fileData)) return false;
        const size_t fileSize = fileData.size();

        if (fileSize < 12 || memcmp(fileData.data(), "RIFF", 4) != 0 || memcmp(fileData.data() + 8, "WAVE", 4) != 0) return false;

        std::vector<uint8_t> out;
        out.reserve(fileSize);
        out.insert(out.end(), fileData.begin(), fileData.begin() + 12);
        bool removedId3 = false;
        // placeholder RIFF size
        out[4] = out[5] = out[6] = out[7] = 0;

        size_t pos = 12;
        while (pos + 8 <= fileSize) {
            const uint8_t* id = &fileData[pos];
            uint32_t sz = (uint32_t)fileData[pos + 4] | ((uint32_t)fileData[pos + 5] << 8) |
                          ((uint32_t)fileData[pos + 6] << 16) | ((uint32_t)fileData[pos + 7] << 24);
            pos += 8;
            if (pos + sz > fileSize) break;
            bool isId3 = (memcmp(id, "ID3 ", 4) == 0 || memcmp(id, "id3 ", 4) == 0);
            removedId3 = removedId3 || isId3;
            if (!isId3) {
                out.insert(out.end(), id, id + 4);
                out.push_back((uint8_t)(sz & 0xFF));
                out.push_back((uint8_t)((sz >> 8) & 0xFF));
                out.push_back((uint8_t)((sz >> 16) & 0xFF));
                out.push_back((uint8_t)((sz >> 24) & 0xFF));
                out.insert(out.end(), fileData.begin() + (ptrdiff_t)pos, fileData.begin() + (ptrdiff_t)(pos + sz));
                if (sz & 1) {
                    if (pos + sz < fileSize) out.push_back(fileData[pos + sz]); else out.push_back(0);
                }
            }
            pos += sz + (sz & 1);
        }

        if (!removedId3) return false;

        uint32_t riffSize = (uint32_t)(out.size() - 8);
        out[4] = (uint8_t)(riffSize & 0xFF);
        out[5] = (uint8_t)((riffSize >> 8) & 0xFF);
        out[6] = (uint8_t)((riffSize >> 16) & 0xFF);
        out[7] = (uint8_t)((riffSize >> 24) & 0xFF);

        std::string tempPath = audioFilePath + ".tmp";
        if (!WriteBinaryFile(tempPath, out)) return false;
        if (!atomicSwap(audioFilePath, audioFilePath + ".tmp")) return false;
        return true;
    }
    if (ext == ".flac") {
        // Remove FLAC picture blocks
        std::vector<uint8_t> fileData;
        if (!ReadBinaryFile(audioFilePath, fileData)) return false;
        const size_t fileSize = fileData.size();

        if (fileSize < 4 || memcmp(fileData.data(), "fLaC", 4) != 0) return false;

        struct Block { uint8_t type; std::vector<uint8_t> data; };
        std::vector<Block> blocks;
        bool removedPicture = false;
        size_t pos = 4;
        bool last = false;
        while (!last && pos + 4 <= fileSize) {
            uint8_t h0 = fileData[pos];
            last = (h0 & 0x80) != 0;
            uint8_t type = (uint8_t)(h0 & 0x7F);
            uint32_t len = ((uint32_t)fileData[pos + 1] << 16) | ((uint32_t)fileData[pos + 2] << 8) | (uint32_t)fileData[pos + 3];
            pos += 4;
            if (pos + len > fileSize) return false;
            if (type != 6) {
                Block b;
                b.type = type;
                b.data.assign(fileData.begin() + (ptrdiff_t)pos, fileData.begin() + (ptrdiff_t)(pos + len));
                blocks.push_back(std::move(b));
            } else {
                removedPicture = true;
            }
            pos += len;
        }
        size_t audioStart = pos;
        if (!removedPicture) return false;

        // Rebuild file
        std::vector<uint8_t> out;
        out.reserve(fileSize);
        out.insert(out.end(), {'f','L','a','C'});
        for (size_t i = 0; i < blocks.size(); ++i) {
            bool isLast = (i == blocks.size() - 1);
            uint8_t h0 = (uint8_t)((isLast ? 0x80 : 0x00) | (blocks[i].type & 0x7F));
            out.push_back(h0);
            uint32_t len = (uint32_t)blocks[i].data.size();
            out.push_back((len >> 16) & 0xFF);
            out.push_back((len >> 8) & 0xFF);
            out.push_back(len & 0xFF);
            out.insert(out.end(), blocks[i].data.begin(), blocks[i].data.end());
        }
        out.insert(out.end(), fileData.begin() + (ptrdiff_t)audioStart, fileData.end());

        std::string tempPath = audioFilePath + ".tmp";
        if (!WriteBinaryFile(tempPath, out)) return false;
        if (!atomicSwap(audioFilePath, audioFilePath + ".tmp")) return false;
        return true;
    }
    if (ext == ".m4a" || ext == ".aac" || ext == ".mp4") {
        // Remove covr by rewriting moov (reuse MP4 machinery with empty covr append).
        std::vector<uint8_t> fileData;
        if (!ReadBinaryFile(audioFilePath, fileData)) return false;
        const size_t fileSize = fileData.size();

        auto typeStr = [&](uint32_t t) -> std::string {
            std::string s(4, 0);
            s[0] = (char)((t >> 24) & 0xFF);
            s[1] = (char)((t >> 16) & 0xFF);
            s[2] = (char)((t >> 8) & 0xFF);
            s[3] = (char)(t & 0xFF);
            return s;
        };
        struct Atom { size_t start=0; uint64_t size=0; size_t header=0; uint32_t type=0; };
        auto readAtom = [&](size_t off, Atom& a) -> bool {
            if (off + 8 > fileSize) return false;
            uint32_t sz32 = readBE32(&fileData[off]);
            uint32_t tp = readBE32(&fileData[off + 4]);
            a.start = off; a.type = tp;
            if (sz32 == 0) { a.size = (uint64_t)(fileSize - off); a.header = 8; return a.size >= 8; }
            if (sz32 == 1) { if (off + 16 > fileSize) return false; uint64_t sz64 = readBE64(&fileData[off + 8]); a.size = sz64; a.header = 16; return sz64 >= 16 && off + sz64 <= fileSize; }
            a.size = sz32; a.header = 8; return sz32 >= 8 && off + (size_t)sz32 <= fileSize;
        };
        auto makeAtom = [&](const char* t4, const std::vector<uint8_t>& payload) -> std::vector<uint8_t> {
            std::vector<uint8_t> out;
            uint64_t total = 8ull + (uint64_t)payload.size();
            writeBE32(out, (uint32_t)total);
            out.push_back((uint8_t)t4[0]); out.push_back((uint8_t)t4[1]); out.push_back((uint8_t)t4[2]); out.push_back((uint8_t)t4[3]);
            out.insert(out.end(), payload.begin(), payload.end());
            return out;
        };

        auto removeCovrFromIlstPayload = [&](const std::vector<uint8_t>& ilstPayload) -> std::vector<uint8_t> {
            std::vector<uint8_t> out;
            size_t p = 0;
            while (p + 8 <= ilstPayload.size()) {
                uint32_t sz32 = readBE32(&ilstPayload[p]);
                uint32_t tp = readBE32(&ilstPayload[p + 4]);
                uint64_t sz = sz32;
                size_t hdr = 8;
                if (sz32 == 1 && p + 16 <= ilstPayload.size()) { sz = readBE64(&ilstPayload[p + 8]); hdr = 16; }
                else if (sz32 == 0) sz = (uint64_t)(ilstPayload.size() - p);
                if (sz < hdr || p + (size_t)sz > ilstPayload.size()) break;
                bool isCovr = (tp == readBE32((const uint8_t*)"covr"));
                if (!isCovr) out.insert(out.end(), ilstPayload.begin() + (ptrdiff_t)p, ilstPayload.begin() + (ptrdiff_t)(p + (size_t)sz));
                p += (size_t)sz;
            }
            return out;
        };

        auto findTopLevel = [&](const char* t4, Atom& outAtom) -> bool {
            uint32_t tt = readBE32((const uint8_t*)t4);
            size_t p = 0;
            while (p + 8 <= fileSize) {
                Atom a;
                if (!readAtom(p, a)) break;
                if (a.type == tt) { outAtom = a; return true; }
                p += (size_t)a.size;
            }
            return false;
        };

        Atom moov, mdat;
        if (!findTopLevel("moov", moov)) return false;
        findTopLevel("mdat", mdat);

        std::vector<uint8_t> moovPayload(fileData.begin() + (ptrdiff_t)(moov.start + moov.header),
                                         fileData.begin() + (ptrdiff_t)(moov.start + (size_t)moov.size));

        auto scanChild = [&](const std::vector<uint8_t>& parentPayload, const char* t4, Atom& child) -> bool {
            uint32_t tt = readBE32((const uint8_t*)t4);
            size_t p = 0;
            while (p + 8 <= parentPayload.size()) {
                uint32_t sz32 = readBE32(&parentPayload[p]);
                uint32_t tp = readBE32(&parentPayload[p + 4]);
                uint64_t sz = sz32;
                size_t hdr = 8;
                if (sz32 == 1 && p + 16 <= parentPayload.size()) { sz = readBE64(&parentPayload[p + 8]); hdr = 16; }
                else if (sz32 == 0) sz = (uint64_t)(parentPayload.size() - p);
                if (sz < hdr || p + (size_t)sz > parentPayload.size()) break;
                if (tp == tt) { child.start = p; child.size = sz; child.header = hdr; child.type = tp; return true; }
                p += (size_t)sz;
            }
            return false;
        };

        Atom udta;
        if (!scanChild(moovPayload, "udta", udta)) return true; // no tags -> nothing to remove
        size_t udtaPos = (size_t)udta.start;
        std::vector<uint8_t> udtaPayload(moovPayload.begin() + (ptrdiff_t)(udtaPos + udta.header),
                                         moovPayload.begin() + (ptrdiff_t)(udtaPos + (size_t)udta.size));
        Atom meta;
        if (!scanChild(udtaPayload, "meta", meta)) return true;
        size_t metaPos = (size_t)meta.start;
        std::vector<uint8_t> metaPayload(udtaPayload.begin() + (ptrdiff_t)(metaPos + meta.header),
                                         udtaPayload.begin() + (ptrdiff_t)(metaPos + (size_t)meta.size));
        if (metaPayload.size() < 4) return false;
        std::vector<uint8_t> metaChildren(metaPayload.begin() + 4, metaPayload.end());
        Atom ilst;
        if (!scanChild(metaChildren, "ilst", ilst)) return true;
        size_t ilstPos = (size_t)ilst.start;
        size_t ilstSize = (size_t)ilst.size;
        std::vector<uint8_t> ilstPayload(metaChildren.begin() + (ptrdiff_t)(ilstPos + ilst.header),
                                         metaChildren.begin() + (ptrdiff_t)(ilstPos + ilstSize));
        std::vector<uint8_t> ilstPayloadNoCovr = removeCovrFromIlstPayload(ilstPayload);
        std::vector<uint8_t> newIlstAtom = makeAtom("ilst", ilstPayloadNoCovr);

        // rebuild metaChildren
        std::vector<uint8_t> newMetaChildren;
        newMetaChildren.insert(newMetaChildren.end(), metaChildren.begin(), metaChildren.begin() + (ptrdiff_t)ilstPos);
        newMetaChildren.insert(newMetaChildren.end(), newIlstAtom.begin(), newIlstAtom.end());
        newMetaChildren.insert(newMetaChildren.end(), metaChildren.begin() + (ptrdiff_t)(ilstPos + ilstSize), metaChildren.end());
        std::vector<uint8_t> newMetaPayload;
        writeBE32(newMetaPayload, 0);
        newMetaPayload.insert(newMetaPayload.end(), newMetaChildren.begin(), newMetaChildren.end());
        std::vector<uint8_t> newMetaAtom = makeAtom("meta", newMetaPayload);

        // rebuild udta
        std::vector<uint8_t> newUdtaPayload;
        newUdtaPayload.insert(newUdtaPayload.end(), udtaPayload.begin(), udtaPayload.begin() + (ptrdiff_t)metaPos);
        newUdtaPayload.insert(newUdtaPayload.end(), newMetaAtom.begin(), newMetaAtom.end());
        newUdtaPayload.insert(newUdtaPayload.end(), udtaPayload.begin() + (ptrdiff_t)(metaPos + (size_t)meta.size), udtaPayload.end());
        std::vector<uint8_t> newUdtaAtom = makeAtom("udta", newUdtaPayload);

        // rebuild moov payload
        std::vector<uint8_t> newMoovPayload;
        newMoovPayload.insert(newMoovPayload.end(), moovPayload.begin(), moovPayload.begin() + (ptrdiff_t)udtaPos);
        newMoovPayload.insert(newMoovPayload.end(), newUdtaAtom.begin(), newUdtaAtom.end());
        newMoovPayload.insert(newMoovPayload.end(), moovPayload.begin() + (ptrdiff_t)(udtaPos + (size_t)udta.size), moovPayload.end());
        std::vector<uint8_t> newMoovAtom = makeAtom("moov", newMoovPayload);
        int64_t delta = (int64_t)newMoovAtom.size() - (int64_t)moov.size;

        // Adjust offsets if needed (same logic as writer; simplified recursion on newMoovAtom)
        if (mdat.size > 0 && moov.start < mdat.start && delta != 0) {
            const size_t moovHdr = 8;
            std::function<void(size_t,size_t)> walk = [&](size_t start, size_t end) {
                size_t p = start;
                while (p + 8 <= end && p + 8 <= newMoovAtom.size()) {
                    uint32_t sz32 = readBE32(&newMoovAtom[p]);
                    uint32_t tp = readBE32(&newMoovAtom[p + 4]);
                    uint64_t sz = sz32;
                    size_t hdr = 8;
                    if (sz32 == 1 && p + 16 <= end && p + 16 <= newMoovAtom.size()) { sz = readBE64(&newMoovAtom[p + 8]); hdr = 16; }
                    else if (sz32 == 0) sz = (uint64_t)(end - p);
                    if (sz < hdr || p + (size_t)sz > end) break;

                    if (tp == readBE32((const uint8_t*)"stco") && p + hdr + 8 <= newMoovAtom.size()) {
                        size_t q = p + hdr;
                        if (q + 8 <= p + (size_t)sz) {
                            uint32_t count = readBE32(&newMoovAtom[q + 4]);
                            size_t off = q + 8;
                            for (uint32_t i = 0; i < count; ++i) {
                                if (off + 4 > p + (size_t)sz) break;
                                uint32_t v = readBE32(&newMoovAtom[off]);
                                int64_t nv = (int64_t)v + delta;
                                if (nv < 0) nv = v;
                                newMoovAtom[off]     = (uint8_t)((nv >> 24) & 0xFF);
                                newMoovAtom[off + 1] = (uint8_t)((nv >> 16) & 0xFF);
                                newMoovAtom[off + 2] = (uint8_t)((nv >> 8) & 0xFF);
                                newMoovAtom[off + 3] = (uint8_t)(nv & 0xFF);
                                off += 4;
                            }
                        }
                    } else if (tp == readBE32((const uint8_t*)"co64") && p + hdr + 8 <= newMoovAtom.size()) {
                        size_t q = p + hdr;
                        if (q + 8 <= p + (size_t)sz) {
                            uint32_t count = readBE32(&newMoovAtom[q + 4]);
                            size_t off = q + 8;
                            for (uint32_t i = 0; i < count; ++i) {
                                if (off + 8 > p + (size_t)sz) break;
                                uint64_t v = readBE64(&newMoovAtom[off]);
                                int64_t nv = (int64_t)v + delta;
                                if (nv < 0) nv = (int64_t)v;
                                uint64_t uv = (uint64_t)nv;
                                newMoovAtom[off]     = (uint8_t)((uv >> 56) & 0xFF);
                                newMoovAtom[off + 1] = (uint8_t)((uv >> 48) & 0xFF);
                                newMoovAtom[off + 2] = (uint8_t)((uv >> 40) & 0xFF);
                                newMoovAtom[off + 3] = (uint8_t)((uv >> 32) & 0xFF);
                                newMoovAtom[off + 4] = (uint8_t)((uv >> 24) & 0xFF);
                                newMoovAtom[off + 5] = (uint8_t)((uv >> 16) & 0xFF);
                                newMoovAtom[off + 6] = (uint8_t)((uv >> 8) & 0xFF);
                                newMoovAtom[off + 7] = (uint8_t)(uv & 0xFF);
                                off += 8;
                            }
                        }
                    }

                    std::string ts = typeStr(tp);
                    bool container = (ts == "moov" || ts == "trak" || ts == "mdia" || ts == "minf" || ts == "stbl" ||
                                      ts == "udta" || ts == "meta" || ts == "ilst");
                    if (container) {
                        size_t childStart = p + hdr;
                        if (ts == "meta") childStart += 4;
                        if (childStart < p + (size_t)sz) walk(childStart, p + (size_t)sz);
                    }
                    p += (size_t)sz;
                }
            };
            walk(moovHdr, newMoovAtom.size());
        }

        std::vector<uint8_t> out;
        out.reserve(fileSize);
        out.insert(out.end(), fileData.begin(), fileData.begin() + (ptrdiff_t)moov.start);
        out.insert(out.end(), newMoovAtom.begin(), newMoovAtom.end());
        out.insert(out.end(), fileData.begin() + (ptrdiff_t)(moov.start + (size_t)moov.size), fileData.end());

        std::string tempPath = audioFilePath + ".tmp";
        if (!WriteBinaryFile(tempPath, out)) return false;
        if (!atomicSwap(audioFilePath, audioFilePath + ".tmp")) return false;
        return true;
    }

    if (ext != ".mp3") return false;

    // Read existing file
    std::vector<uint8_t> fileData;
    if (!ReadBinaryFile(audioFilePath, fileData)) return false;
    const size_t fileSize = fileData.size();

    // If no ID3 tag, nothing to remove
    if (fileSize < 10 || fileData[0] != 'I' || fileData[1] != 'D' || fileData[2] != '3') {
        return false;
    }

    int id3Major = (int)fileData[3];
    if (id3Major != 3 && id3Major != 4) id3Major = 3;
    uint8_t id3Flags = fileData[5];

    uint32_t tagSize = readSyncSafeInt(&fileData[6]);
    size_t tagEnd = 10 + (size_t)tagSize;
    if (tagEnd > fileSize) tagEnd = fileSize;
    size_t audioDataStart = tagEnd;

    std::vector<uint8_t> keptFrames;
    size_t pos = 10;
    if (id3Flags & 0x40) {
        if (pos + 4 <= tagEnd) {
            uint32_t extSize = (id3Major == 4) ? readSyncSafeInt(&fileData[pos]) : readUint32BE(&fileData[pos]);
            size_t skip = 4 + (size_t)extSize;
            if (pos + skip <= tagEnd) pos += skip;
        }
    }

    while (pos + 10 <= tagEnd) {
        const uint8_t* id = &fileData[pos];
        if (id[0] == 0 && id[1] == 0 && id[2] == 0 && id[3] == 0) break;
        if (!isFrameIdValid4(id)) break;
        uint32_t fsize = (id3Major == 4) ? readSyncSafeInt(&fileData[pos + 4]) : readUint32BE(&fileData[pos + 4]);
        if (fsize == 0) break;
        size_t frameTotal = 10 + (size_t)fsize;
        if (pos + frameTotal > tagEnd) break;

        bool isApic = (id[0] == 'A' && id[1] == 'P' && id[2] == 'I' && id[3] == 'C');
        if (!isApic) {
            keptFrames.insert(keptFrames.end(), fileData.begin() + (ptrdiff_t)pos, fileData.begin() + (ptrdiff_t)(pos + frameTotal));
        }
        pos += frameTotal;
    }

    // If there are no frames left, remove tag entirely by writing only audio payload.
    if (keptFrames.empty()) {
        std::string tempPath = audioFilePath + ".tmp";
        const std::string_view audioPayload(
            reinterpret_cast<const char*>(fileData.data() + audioDataStart),
            fileSize - audioDataStart);
        if (!WriteBinaryFile(tempPath, {audioPayload})) return false;
        if (!atomicSwap(audioFilePath, audioFilePath + ".tmp")) return false;
        return true;
    }

    // Otherwise, rebuild tag without APIC
    std::vector<uint8_t> newTag;
    newTag.push_back('I');
    newTag.push_back('D');
    newTag.push_back('3');
    newTag.push_back((uint8_t)id3Major);
    newTag.push_back(0);
    uint8_t outFlags = (uint8_t)(id3Flags & 0x40);
    newTag.push_back(outFlags);
    size_t sizePos = newTag.size();
    newTag.push_back(0); newTag.push_back(0); newTag.push_back(0); newTag.push_back(0);
    newTag.insert(newTag.end(), keptFrames.begin(), keptFrames.end());
    uint32_t tagContentSize = (uint32_t)(newTag.size() - 10);
    newTag[sizePos] = (tagContentSize >> 21) & 0x7F;
    newTag[sizePos + 1] = (tagContentSize >> 14) & 0x7F;
    newTag[sizePos + 2] = (tagContentSize >> 7) & 0x7F;
    newTag[sizePos + 3] = tagContentSize & 0x7F;

    // Write new file (tag + original audio payload)
    std::string tempPath = audioFilePath + ".tmp";
    const std::string_view tagBytes(
        reinterpret_cast<const char*>(newTag.data()), newTag.size());
    const std::string_view audioPayload(
        reinterpret_cast<const char*>(fileData.data() + audioDataStart),
        fileSize - audioDataStart);
    if (!WriteBinaryFile(tempPath, {tagBytes, audioPayload})) return false;

    if (!atomicSwap(audioFilePath, audioFilePath + ".tmp")) return false;
    return true;
}

int MetadataWriter::writeAlbumArtBatch(const std::vector<std::string>& audioPaths,
                                       const std::string& imageFilePath) {
    std::vector<uint8_t> imageData;
    std::string mimeType;
    
    if (!loadImageFile(imageFilePath, imageData, mimeType)) {
        return 0;
    }
    
    int successCount = 0;
    for (const auto& path : audioPaths) {
        if (writeAlbumArt(path, imageData, mimeType)) {
            successCount++;
        }
    }
    return successCount;
}

int MetadataWriter::removeAlbumArtBatch(const std::vector<std::string>& audioPaths) {
    int successCount = 0;
    for (const auto& path : audioPaths) {
        if (removeAlbumArt(path)) {
            successCount++;
        }
    }
    return successCount;
}

// ID3v2 Album Art Writer (for MP3 files)

static void writeUint32BE(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back((value >> 24) & 0xFF);
    data.push_back((value >> 16) & 0xFF);
    data.push_back((value >> 8) & 0xFF);
    data.push_back(value & 0xFF);
}

static void writeSyncSafeInt(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back((value >> 21) & 0x7F);
    data.push_back((value >> 14) & 0x7F);
    data.push_back((value >> 7) & 0x7F);
    data.push_back(value & 0x7F);
}

static uint32_t readUint32BE(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint32_t readSyncSafeInt(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | ((uint32_t)(p[3] & 0x7F));
}

static bool isFrameIdValid4(const uint8_t* id) {
    // ASCII A-Z0-9 only (rough check)
    for (int i = 0; i < 4; ++i) {
        unsigned char c = id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

static void appendApicFrame(std::vector<uint8_t>& outFrames,
                            int id3MajorVersion,
                            const std::vector<uint8_t>& imageData,
                            const std::string& mimeType) {
    // APIC frame header: "APIC" + size + flags
    outFrames.push_back('A');
    outFrames.push_back('P');
    outFrames.push_back('I');
    outFrames.push_back('C');

    // Build frame content first
    std::vector<uint8_t> content;
    content.reserve(1 + mimeType.size() + 1 + 1 + 1 + imageData.size());
    content.push_back(0);  // Encoding: ISO-8859-1
    for (char c : mimeType) content.push_back((uint8_t)c);
    content.push_back(0);  // Null terminator
    content.push_back(3);  // Picture type: Front cover
    content.push_back(0);  // Description (empty, null-terminated)
    content.insert(content.end(), imageData.begin(), imageData.end());

    uint32_t frameSize = (uint32_t)content.size();
    if (id3MajorVersion == 4) {
        writeSyncSafeInt(outFrames, frameSize);
    } else {
        writeUint32BE(outFrames, frameSize);
    }
    // Flags
    outFrames.push_back(0);
    outFrames.push_back(0);
    // Content
    outFrames.insert(outFrames.end(), content.begin(), content.end());
}

bool MetadataWriter::writeID3v2AlbumArt(const std::string& filePath,
                                        const std::vector<uint8_t>& imageData,
                                        const std::string& mimeType) {
    // Read existing file
    std::vector<uint8_t> fileData;
    if (!ReadBinaryFile(filePath, fileData)) return false;
    const size_t fileSize = fileData.size();
    
    // Parse existing ID3v2 tag and keep all frames except APIC (preserve metadata)
    size_t audioDataStart = 0;
    int id3Major = 3; // default write v2.3 if no tag
    uint8_t id3Flags = 0;
    std::vector<uint8_t> keptFrames;

    if (fileSize >= 10 && fileData[0] == 'I' && fileData[1] == 'D' && fileData[2] == '3') {
        id3Major = (int)fileData[3];
        if (id3Major != 3 && id3Major != 4) id3Major = 3;
        id3Flags = fileData[5];

        uint32_t tagSize = readSyncSafeInt(&fileData[6]);
        size_t tagEnd = 10 + (size_t)tagSize;
        if (tagEnd > fileSize) tagEnd = fileSize;
        audioDataStart = tagEnd;

        size_t pos = 10;
        // Extended header
        if (id3Flags & 0x40) {
            if (pos + 4 <= tagEnd) {
                uint32_t extSize = (id3Major == 4) ? readSyncSafeInt(&fileData[pos]) : readUint32BE(&fileData[pos]);
                size_t skip = 4 + (size_t)extSize;
                if (pos + skip <= tagEnd) pos += skip;
            }
        }

        // Frames
        while (pos + 10 <= tagEnd) {
            const uint8_t* id = &fileData[pos];
            // Padding or invalid -> stop
            if (id[0] == 0 && id[1] == 0 && id[2] == 0 && id[3] == 0) break;
            if (!isFrameIdValid4(id)) break;

            uint32_t fsize = (id3Major == 4) ? readSyncSafeInt(&fileData[pos + 4]) : readUint32BE(&fileData[pos + 4]);
            if (fsize == 0) break;
            size_t frameTotal = 10 + (size_t)fsize;
            if (pos + frameTotal > tagEnd) break;

            bool isApic = (id[0] == 'A' && id[1] == 'P' && id[2] == 'I' && id[3] == 'C');
            if (!isApic) {
                keptFrames.insert(keptFrames.end(), fileData.begin() + (ptrdiff_t)pos, fileData.begin() + (ptrdiff_t)(pos + frameTotal));
            }
            pos += frameTotal;
        }
    }

    // Build new ID3 tag (keep previous frames + new APIC)
    std::vector<uint8_t> newTag;
    
    // ID3 header (v2.3 or v2.4) (will be updated later with final size)
    newTag.push_back('I');
    newTag.push_back('D');
    newTag.push_back('3');
    newTag.push_back((uint8_t)id3Major);
    newTag.push_back(0);  // Revision
    // Preserve only extended header flag; clear unsync/experimental/footer for safety
    uint8_t outFlags = (uint8_t)(id3Flags & 0x40);
    newTag.push_back(outFlags);
    size_t sizePos = newTag.size();
    newTag.push_back(0); newTag.push_back(0); newTag.push_back(0); newTag.push_back(0);  // Size placeholder

    // Existing frames (without APIC)
    if (!keptFrames.empty()) {
        newTag.insert(newTag.end(), keptFrames.begin(), keptFrames.end());
    }

    // New APIC frame (append)
    appendApicFrame(newTag, id3Major, imageData, mimeType);
    
    // Update tag size in header (syncsafe integer, excludes header)
    uint32_t tagContentSize = (uint32_t)(newTag.size() - 10);
    newTag[sizePos] = (tagContentSize >> 21) & 0x7F;
    newTag[sizePos + 1] = (tagContentSize >> 14) & 0x7F;
    newTag[sizePos + 2] = (tagContentSize >> 7) & 0x7F;
    newTag[sizePos + 3] = tagContentSize & 0x7F;
    
    // Write new file
    const std::string_view tagBytes(
        reinterpret_cast<const char*>(newTag.data()), newTag.size());
    const std::string_view audioPayload(
        reinterpret_cast<const char*>(fileData.data() + audioDataStart),
        fileSize - audioDataStart);
    if (!WriteBinaryFile(filePath + ".tmp", {tagBytes, audioPayload})) return false;
    
    if (!atomicSwap(filePath, filePath + ".tmp")) return false;
    
    return true;
}

bool MetadataWriter::writeFLACAlbumArt(const std::string& filePath,
                                       const std::vector<uint8_t>& imageData,
                                       const std::string& mimeType) {
    // Rewrite FLAC metadata blocks: keep all blocks except PICTURE, append new PICTURE
    std::vector<uint8_t> fileData;
    if (!ReadBinaryFile(filePath, fileData)) return false;
    const size_t fileSize = fileData.size();

    if (fileSize < 4 || memcmp(fileData.data(), "fLaC", 4) != 0) return false;

    struct Block { uint8_t type; std::vector<uint8_t> data; };
    std::vector<Block> blocks;
    size_t pos = 4;
    bool last = false;
    while (!last && pos + 4 <= fileSize) {
        uint8_t h0 = fileData[pos];
        last = (h0 & 0x80) != 0;
        uint8_t type = (uint8_t)(h0 & 0x7F);
        uint32_t len = ((uint32_t)fileData[pos + 1] << 16) | ((uint32_t)fileData[pos + 2] << 8) | (uint32_t)fileData[pos + 3];
        pos += 4;
        if (pos + len > fileSize) return false;
        if (type != 6) {
            Block b;
            b.type = type;
            b.data.assign(fileData.begin() + (ptrdiff_t)pos, fileData.begin() + (ptrdiff_t)(pos + len));
            blocks.push_back(std::move(b));
        }
        pos += len;
    }
    size_t audioStart = pos;

    // Build PICTURE block payload
    std::vector<uint8_t> pic;
    pic.reserve(64 + mimeType.size() + imageData.size());
    writeBE32(pic, 3); // picture type: front cover
    writeBE32(pic, (uint32_t)mimeType.size());
    pic.insert(pic.end(), mimeType.begin(), mimeType.end());
    writeBE32(pic, 0); // description length
    // width/height/depth/colors (best-effort fill; keep 0 if unknown)
    uint32_t width = 0, height = 0, depth = 0, colors = 0;
    // PNG
    if (imageData.size() >= 24 && imageData[0] == 0x89 && imageData[1] == 'P' && imageData[2] == 'N' && imageData[3] == 'G') {
        width = readBE32(&imageData[16]);
        height = readBE32(&imageData[20]);
        uint8_t bitDepth = imageData[24];
        uint8_t colorType = imageData[25];
        uint32_t channels = (colorType == 2 ? 3 : (colorType == 6 ? 4 : (colorType == 0 ? 1 : 0)));
        if (channels > 0) depth = (uint32_t)bitDepth * channels;
    } else {
        // JPEG: scan for SOF0/SOF2
        size_t jp = 0;
        if (imageData.size() >= 4 && imageData[0] == 0xFF && imageData[1] == 0xD8) {
            jp = 2;
            while (jp + 4 < imageData.size()) {
                if (imageData[jp] != 0xFF) { jp++; continue; }
                uint8_t marker = imageData[jp + 1];
                if (marker == 0xD9 || marker == 0xDA) break;
                uint16_t segLen = (uint16_t)imageData[jp + 2] << 8 | (uint16_t)imageData[jp + 3];
                if (segLen < 2) break;
                if (marker == 0xC0 || marker == 0xC2) {
                    if (jp + 2 + segLen <= imageData.size() && segLen >= 7) {
                        height = (uint32_t)imageData[jp + 5] << 8 | (uint32_t)imageData[jp + 6];
                        width = (uint32_t)imageData[jp + 7] << 8 | (uint32_t)imageData[jp + 8];
                        uint8_t comps = imageData[jp + 9];
                        depth = 8u * (uint32_t)comps;
                    }
                    break;
                }
                jp += 2 + segLen;
            }
        }
    }
    writeBE32(pic, width);
    writeBE32(pic, height);
    writeBE32(pic, depth);
    writeBE32(pic, colors);
    writeBE32(pic, (uint32_t)imageData.size());
    pic.insert(pic.end(), imageData.begin(), imageData.end());

    Block picBlock;
    picBlock.type = 6;
    picBlock.data = std::move(pic);
    blocks.push_back(std::move(picBlock));

    // Rebuild file
    std::vector<uint8_t> out;
    out.reserve(fileSize + imageData.size() + 128);
    out.insert(out.end(), {'f','L','a','C'});
    for (size_t i = 0; i < blocks.size(); ++i) {
        bool isLast = (i == blocks.size() - 1);
        uint8_t h0 = (uint8_t)((isLast ? 0x80 : 0x00) | (blocks[i].type & 0x7F));
        out.push_back(h0);
        uint32_t len = (uint32_t)blocks[i].data.size();
        out.push_back((len >> 16) & 0xFF);
        out.push_back((len >> 8) & 0xFF);
        out.push_back(len & 0xFF);
        out.insert(out.end(), blocks[i].data.begin(), blocks[i].data.end());
    }
    out.insert(out.end(), fileData.begin() + (ptrdiff_t)audioStart, fileData.end());

    if (!WriteBinaryFile(filePath + ".tmp", out)) return false;
    if (!atomicSwap(filePath, filePath + ".tmp")) return false;
    return true;
}

bool MetadataWriter::writeM4AAlbumArt(const std::string& filePath,
                                      const std::vector<uint8_t>& imageData,
                                      const std::string& mimeType) {
    // MP4/M4A: write 'covr' atom under udta/meta/ilst; adjust stco/co64 if moov size changes before mdat.
    std::vector<uint8_t> fileData;
    if (!ReadBinaryFile(filePath, fileData)) return false;
    const size_t fileSize = fileData.size();

    auto readType = [&](size_t at) -> uint32_t {
        if (at + 4 > fileSize) return 0;
        return readBE32(&fileData[at]);
    };
    auto typeStr = [&](uint32_t t) -> std::string {
        std::string s(4, 0);
        s[0] = (char)((t >> 24) & 0xFF);
        s[1] = (char)((t >> 16) & 0xFF);
        s[2] = (char)((t >> 8) & 0xFF);
        s[3] = (char)(t & 0xFF);
        return s;
    };
    struct Atom { size_t start=0; uint64_t size=0; size_t header=0; uint32_t type=0; };
    auto readAtom = [&](size_t off, Atom& a) -> bool {
        if (off + 8 > fileSize) return false;
        uint32_t sz32 = readBE32(&fileData[off]);
        uint32_t tp = readBE32(&fileData[off + 4]);
        a.start = off; a.type = tp;
        if (sz32 == 0) {
            a.size = (uint64_t)(fileSize - off);
            a.header = 8;
            return a.size >= 8;
        }
        if (sz32 == 1) {
            if (off + 16 > fileSize) return false;
            uint64_t sz64 = readBE64(&fileData[off + 8]);
            a.size = sz64;
            a.header = 16;
            return sz64 >= 16 && off + sz64 <= fileSize;
        }
        a.size = sz32;
        a.header = 8;
        return sz32 >= 8 && off + (size_t)sz32 <= fileSize;
    };

    auto makeAtom = [&](const char* t4, const std::vector<uint8_t>& payload) -> std::vector<uint8_t> {
        std::vector<uint8_t> out;
        uint64_t total = 8ull + (uint64_t)payload.size();
        if (total < 0xFFFFFFFFull) {
            writeBE32(out, (uint32_t)total);
            out.push_back((uint8_t)t4[0]); out.push_back((uint8_t)t4[1]); out.push_back((uint8_t)t4[2]); out.push_back((uint8_t)t4[3]);
        } else {
            writeBE32(out, 1);
            out.push_back((uint8_t)t4[0]); out.push_back((uint8_t)t4[1]); out.push_back((uint8_t)t4[2]); out.push_back((uint8_t)t4[3]);
            writeBE64(out, total + 8); // header becomes 16
        }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    };

    auto makeFullBoxPayload = [&](uint32_t versionFlags, const std::vector<uint8_t>& rest) -> std::vector<uint8_t> {
        std::vector<uint8_t> p;
        writeBE32(p, versionFlags);
        p.insert(p.end(), rest.begin(), rest.end());
        return p;
    };

    auto makeHdlrAtom = [&]() -> std::vector<uint8_t> {
        std::vector<uint8_t> p;
        writeBE32(p, 0);          // version/flags
        writeBE32(p, 0);          // pre_defined
        // handler_type 'mdir'
        p.push_back('m'); p.push_back('d'); p.push_back('i'); p.push_back('r');
        // reserved[3]
        writeBE32(p, 0); writeBE32(p, 0); writeBE32(p, 0);
        const char* name = "SoundManager";
        p.insert(p.end(), name, name + std::strlen(name));
        p.push_back(0);
        return makeAtom("hdlr", p);
    };

    auto makeCovrAtom = [&]() -> std::vector<uint8_t> {
        // data atom
        uint32_t dataType = 13; // JPEG
        if (mimeType.find("png") != std::string::npos) dataType = 14;
        std::vector<uint8_t> dataPayload;
        writeBE32(dataPayload, 0);          // version/flags
        writeBE32(dataPayload, dataType);   // type
        writeBE32(dataPayload, 0);          // locale
        dataPayload.insert(dataPayload.end(), imageData.begin(), imageData.end());
        std::vector<uint8_t> dataAtom = makeAtom("data", dataPayload);
        std::vector<uint8_t> covrPayload;
        covrPayload.insert(covrPayload.end(), dataAtom.begin(), dataAtom.end());
        return makeAtom("covr", covrPayload);
    };

    auto removeCovrFromIlstPayload = [&](const std::vector<uint8_t>& ilstPayload) -> std::vector<uint8_t> {
        std::vector<uint8_t> out;
        size_t p = 0;
        while (p + 8 <= ilstPayload.size()) {
            uint32_t sz32 = readBE32(&ilstPayload[p]);
            uint32_t tp = readBE32(&ilstPayload[p + 4]);
            uint64_t sz = sz32;
            size_t hdr = 8;
            if (sz32 == 1 && p + 16 <= ilstPayload.size()) {
                sz = readBE64(&ilstPayload[p + 8]);
                hdr = 16;
            } else if (sz32 == 0) {
                sz = (uint64_t)(ilstPayload.size() - p);
            }
            if (sz < hdr || p + (size_t)sz > ilstPayload.size()) break;
            bool isCovr = (tp == readBE32((const uint8_t*)"covr"));
            if (!isCovr) {
                out.insert(out.end(), ilstPayload.begin() + (ptrdiff_t)p, ilstPayload.begin() + (ptrdiff_t)(p + (size_t)sz));
            }
            p += (size_t)sz;
        }
        return out;
    };

    auto findTopLevel = [&](const char* t4, Atom& outAtom) -> bool {
        uint32_t tt = readBE32((const uint8_t*)t4);
        size_t p = 0;
        while (p + 8 <= fileSize) {
            Atom a;
            if (!readAtom(p, a)) break;
            if (a.type == tt) { outAtom = a; return true; }
            p += (size_t)a.size;
        }
        return false;
    };

    Atom moov, mdat;
    if (!findTopLevel("moov", moov)) return false;
    findTopLevel("mdat", mdat); // best-effort

    // Extract moov payload
    std::vector<uint8_t> moovPayload(fileData.begin() + (ptrdiff_t)(moov.start + moov.header),
                                     fileData.begin() + (ptrdiff_t)(moov.start + (size_t)moov.size));

    auto scanChild = [&](const std::vector<uint8_t>& parentPayload, const char* t4, Atom& child, size_t parentStartAbs, size_t parentHeaderAbs, bool metaSpecial = false) -> bool {
        uint32_t tt = readBE32((const uint8_t*)t4);
        size_t p = 0;
        while (p + 8 <= parentPayload.size()) {
            uint32_t sz32 = readBE32(&parentPayload[p]);
            uint32_t tp = readBE32(&parentPayload[p + 4]);
            uint64_t sz = sz32;
            size_t hdr = 8;
            if (sz32 == 1 && p + 16 <= parentPayload.size()) {
                sz = readBE64(&parentPayload[p + 8]);
                hdr = 16;
            } else if (sz32 == 0) {
                sz = (uint64_t)(parentPayload.size() - p);
            }
            if (sz < hdr || p + (size_t)sz > parentPayload.size()) break;
            if (tp == tt) {
                child.start = parentStartAbs + parentHeaderAbs + p;
                child.size = sz;
                child.header = hdr;
                child.type = tp;
                return true;
            }
            p += (size_t)sz;
        }
        return false;
    };

    // Find or create udta/meta/ilst inside moov
    Atom udta;
    bool hasUdta = scanChild(moovPayload, "udta", udta, moov.start, moov.header);
    std::vector<uint8_t> udtaPayload;
    size_t udtaRelPos = 0;
    if (hasUdta) {
        udtaRelPos = (size_t)(udta.start - (moov.start + moov.header));
        udtaPayload.assign(moovPayload.begin() + (ptrdiff_t)(udtaRelPos + udta.header),
                           moovPayload.begin() + (ptrdiff_t)(udtaRelPos + (size_t)udta.size));
    } else {
        udtaPayload.clear();
    }

    Atom meta;
    bool hasMeta = false;
    if (hasUdta) {
        hasMeta = scanChild(udtaPayload, "meta", meta, udta.start, udta.header);
    }

    std::vector<uint8_t> metaPayload; // full meta payload (including version/flags + children)
    size_t metaRelPos = 0;
    if (hasUdta && hasMeta) {
        metaRelPos = (size_t)(meta.start - (udta.start + udta.header));
        metaPayload.assign(udtaPayload.begin() + (ptrdiff_t)(metaRelPos + meta.header),
                           udtaPayload.begin() + (ptrdiff_t)(metaRelPos + (size_t)meta.size));
    } else {
        // create empty meta payload (version/flags + hdlr + ilst)
        std::vector<uint8_t> children;
        std::vector<uint8_t> hdlr = makeHdlrAtom();
        std::vector<uint8_t> ilst = makeAtom("ilst", std::vector<uint8_t>{});
        children.insert(children.end(), hdlr.begin(), hdlr.end());
        children.insert(children.end(), ilst.begin(), ilst.end());
        metaPayload = makeFullBoxPayload(0, children);
    }

    // meta children start after version/flags (4 bytes)
    if (metaPayload.size() < 4) return false;
    std::vector<uint8_t> metaChildren(metaPayload.begin() + 4, metaPayload.end());

    // Ensure hdlr exists; if not, prepend one
    Atom hdlr;
    bool hasHdlr = scanChild(metaChildren, "hdlr", hdlr, 0, 0);
    if (!hasHdlr) {
        std::vector<uint8_t> h = makeHdlrAtom();
        std::vector<uint8_t> tmp;
        tmp.insert(tmp.end(), h.begin(), h.end());
        tmp.insert(tmp.end(), metaChildren.begin(), metaChildren.end());
        metaChildren.swap(tmp);
    }

    // Find ilst
    Atom ilst;
    bool hasIlst = scanChild(metaChildren, "ilst", ilst, 0, 0);
    std::vector<uint8_t> ilstPayload;
    size_t ilstPos = 0;
    size_t ilstSize = 0;
    if (hasIlst) {
        ilstPos = (size_t)(ilst.start); // relative within metaChildren
        ilstSize = (size_t)ilst.size;
        size_t payloadStart = ilstPos + ilst.header;
        ilstPayload.assign(metaChildren.begin() + (ptrdiff_t)payloadStart,
                           metaChildren.begin() + (ptrdiff_t)(ilstPos + ilstSize));
    } else {
        ilstPayload.clear();
        ilstPos = metaChildren.size();
        ilstSize = 0;
    }

    // Replace covr: remove existing, append new covr
    std::vector<uint8_t> ilstPayloadNoCovr = removeCovrFromIlstPayload(ilstPayload);
    std::vector<uint8_t> covrAtom = makeCovrAtom();
    ilstPayloadNoCovr.insert(ilstPayloadNoCovr.end(), covrAtom.begin(), covrAtom.end());
    std::vector<uint8_t> newIlstAtom = makeAtom("ilst", ilstPayloadNoCovr);

    // Rebuild metaChildren with replaced/added ilst
    std::vector<uint8_t> newMetaChildren;
    if (hasIlst) {
        newMetaChildren.insert(newMetaChildren.end(), metaChildren.begin(), metaChildren.begin() + (ptrdiff_t)ilstPos);
        newMetaChildren.insert(newMetaChildren.end(), newIlstAtom.begin(), newIlstAtom.end());
        newMetaChildren.insert(newMetaChildren.end(), metaChildren.begin() + (ptrdiff_t)(ilstPos + ilstSize), metaChildren.end());
    } else {
        newMetaChildren = metaChildren;
        newMetaChildren.insert(newMetaChildren.end(), newIlstAtom.begin(), newIlstAtom.end());
    }

    // Build new meta atom
    std::vector<uint8_t> newMetaPayload = makeFullBoxPayload(0, newMetaChildren);
    std::vector<uint8_t> newMetaAtom = makeAtom("meta", newMetaPayload);

    // Build new udta atom
    std::vector<uint8_t> newUdtaChildren;
    if (hasUdta && hasMeta) {
        // replace existing meta inside udtaPayload
        size_t metaPos = metaRelPos;
        size_t metaSize = (size_t)meta.size;
        newUdtaChildren.insert(newUdtaChildren.end(), udtaPayload.begin(), udtaPayload.begin() + (ptrdiff_t)metaPos);
        newUdtaChildren.insert(newUdtaChildren.end(), newMetaAtom.begin(), newMetaAtom.end());
        newUdtaChildren.insert(newUdtaChildren.end(), udtaPayload.begin() + (ptrdiff_t)(metaPos + metaSize), udtaPayload.end());
    } else if (hasUdta && !hasMeta) {
        newUdtaChildren = udtaPayload;
        newUdtaChildren.insert(newUdtaChildren.end(), newMetaAtom.begin(), newMetaAtom.end());
    } else {
        newUdtaChildren = newMetaAtom;
    }
    std::vector<uint8_t> newUdtaAtom = makeAtom("udta", newUdtaChildren);

    // Build new moov payload
    std::vector<uint8_t> newMoovPayload;
    if (hasUdta) {
        newMoovPayload.insert(newMoovPayload.end(), moovPayload.begin(), moovPayload.begin() + (ptrdiff_t)udtaRelPos);
        newMoovPayload.insert(newMoovPayload.end(), newUdtaAtom.begin(), newUdtaAtom.end());
        newMoovPayload.insert(newMoovPayload.end(), moovPayload.begin() + (ptrdiff_t)(udtaRelPos + (size_t)udta.size), moovPayload.end());
    } else {
        newMoovPayload = moovPayload;
        newMoovPayload.insert(newMoovPayload.end(), newUdtaAtom.begin(), newUdtaAtom.end());
    }

    std::vector<uint8_t> newMoovAtom = makeAtom("moov", newMoovPayload);
    int64_t delta = (int64_t)newMoovAtom.size() - (int64_t)moov.size;

    // If moov is before mdat, adjust stco/co64 offsets by delta (mdat shifted)
    auto adjustOffsets = [&](std::vector<uint8_t>& moovAtomBytes, int64_t d) {
        if (d == 0) return;
        // Parse within moov atom payload; adjust stco/co64 full boxes.
        const size_t moovHdr = 8;
        if (moovAtomBytes.size() < moovHdr) return;
        std::function<void(size_t,size_t)> walk = [&](size_t start, size_t end) {
            size_t p = start;
            while (p + 8 <= end && p + 8 <= moovAtomBytes.size()) {
                uint32_t sz32 = readBE32(&moovAtomBytes[p]);
                uint32_t tp = readBE32(&moovAtomBytes[p + 4]);
                uint64_t sz = sz32;
                size_t hdr = 8;
                if (sz32 == 1 && p + 16 <= end && p + 16 <= moovAtomBytes.size()) {
                    sz = readBE64(&moovAtomBytes[p + 8]);
                    hdr = 16;
                } else if (sz32 == 0) {
                    sz = (uint64_t)(end - p);
                }
                if (sz < hdr || p + (size_t)sz > end) break;

                // stco/co64
                if (tp == readBE32((const uint8_t*)"stco") && p + hdr + 8 <= moovAtomBytes.size()) {
                    size_t q = p + hdr;
                    // version/flags (4) + entry_count(4)
                    if (q + 8 <= p + (size_t)sz) {
                        uint32_t count = readBE32(&moovAtomBytes[q + 4]);
                        size_t off = q + 8;
                        for (uint32_t i = 0; i < count; ++i) {
                            if (off + 4 > p + (size_t)sz) break;
                            uint32_t v = readBE32(&moovAtomBytes[off]);
                            int64_t nv = (int64_t)v + d;
                            if (nv < 0) nv = v;
                            moovAtomBytes[off]     = (uint8_t)((nv >> 24) & 0xFF);
                            moovAtomBytes[off + 1] = (uint8_t)((nv >> 16) & 0xFF);
                            moovAtomBytes[off + 2] = (uint8_t)((nv >> 8) & 0xFF);
                            moovAtomBytes[off + 3] = (uint8_t)(nv & 0xFF);
                            off += 4;
                        }
                    }
                } else if (tp == readBE32((const uint8_t*)"co64") && p + hdr + 8 <= moovAtomBytes.size()) {
                    size_t q = p + hdr;
                    if (q + 8 <= p + (size_t)sz) {
                        uint32_t count = readBE32(&moovAtomBytes[q + 4]);
                        size_t off = q + 8;
                        for (uint32_t i = 0; i < count; ++i) {
                            if (off + 8 > p + (size_t)sz) break;
                            uint64_t v = readBE64(&moovAtomBytes[off]);
                            int64_t nv = (int64_t)v + d;
                            if (nv < 0) nv = (int64_t)v;
                            uint64_t uv = (uint64_t)nv;
                            moovAtomBytes[off]     = (uint8_t)((uv >> 56) & 0xFF);
                            moovAtomBytes[off + 1] = (uint8_t)((uv >> 48) & 0xFF);
                            moovAtomBytes[off + 2] = (uint8_t)((uv >> 40) & 0xFF);
                            moovAtomBytes[off + 3] = (uint8_t)((uv >> 32) & 0xFF);
                            moovAtomBytes[off + 4] = (uint8_t)((uv >> 24) & 0xFF);
                            moovAtomBytes[off + 5] = (uint8_t)((uv >> 16) & 0xFF);
                            moovAtomBytes[off + 6] = (uint8_t)((uv >> 8) & 0xFF);
                            moovAtomBytes[off + 7] = (uint8_t)(uv & 0xFF);
                            off += 8;
                        }
                    }
                }

                // Recurse into known containers
                std::string ts = typeStr(tp);
                bool container = (ts == "moov" || ts == "trak" || ts == "mdia" || ts == "minf" || ts == "stbl" ||
                                  ts == "udta" || ts == "meta" || ts == "ilst");
                if (container) {
                    size_t childStart = p + hdr;
                    if (ts == "meta") childStart += 4; // version/flags
                    if (childStart < p + (size_t)sz) {
                        walk(childStart, p + (size_t)sz);
                    }
                }

                p += (size_t)sz;
            }
        };
        walk(moovHdr, moovAtomBytes.size());
    };

    if (mdat.size > 0 && moov.start < mdat.start) {
        adjustOffsets(newMoovAtom, delta);
    }

    // Rebuild entire file with replaced moov atom
    std::vector<uint8_t> out;
    out.reserve(fileSize + (size_t)std::max<int64_t>(0, delta));
    out.insert(out.end(), fileData.begin(), fileData.begin() + (ptrdiff_t)moov.start);
    out.insert(out.end(), newMoovAtom.begin(), newMoovAtom.end());
    out.insert(out.end(), fileData.begin() + (ptrdiff_t)(moov.start + (size_t)moov.size), fileData.end());

    if (!WriteBinaryFile(filePath + ".tmp", out)) return false;
    if (!atomicSwap(filePath, filePath + ".tmp")) return false;
    return true;
}

// WAV ID3 chunk writer

bool MetadataWriter::writeWAVID3AlbumArt(const std::string& filePath,
                                        const std::vector<uint8_t>& imageData,
                                        const std::string& mimeType) {
    std::vector<uint8_t> fileData;
    if (!ReadBinaryFile(filePath, fileData)) return false;
    const size_t fileSize = fileData.size();

    if (fileSize < 12 || memcmp(fileData.data(), "RIFF", 4) != 0 || memcmp(fileData.data() + 8, "WAVE", 4) != 0) return false;

    // Build ID3v2.3 tag with APIC only (safe default)
    std::vector<uint8_t> tag;
    tag.push_back('I'); tag.push_back('D'); tag.push_back('3');
    tag.push_back(3); // v2.3
    tag.push_back(0);
    tag.push_back(0);
    size_t sizePos = tag.size();
    tag.push_back(0); tag.push_back(0); tag.push_back(0); tag.push_back(0);
    
    appendApicFrame(tag, 3, imageData, mimeType);
    
    uint32_t tagContentSize = (uint32_t)(tag.size() - 10);
    tag[sizePos]     = (tagContentSize >> 21) & 0x7F;
    tag[sizePos + 1] = (tagContentSize >> 14) & 0x7F;
    tag[sizePos + 2] = (tagContentSize >> 7) & 0x7F;
    tag[sizePos + 3] = tagContentSize & 0x7F;

    // Rewrite the RIFF container while preserving chunk padding.
    std::vector<uint8_t> out;
    out.reserve(fileSize + tag.size() + 16);
    out.insert(out.end(), fileData.begin(), fileData.begin() + 12);
    out[4] = out[5] = out[6] = out[7] = 0; // RIFF size placeholder

    size_t pos = 12;
    while (pos + 8 <= fileSize) {
        const uint8_t* id = &fileData[pos];
        uint32_t sz = (uint32_t)fileData[pos + 4] | ((uint32_t)fileData[pos + 5] << 8) |
                      ((uint32_t)fileData[pos + 6] << 16) | ((uint32_t)fileData[pos + 7] << 24);
        pos += 8;
        if (pos + sz > fileSize) break;
        
        bool isId3 = (memcmp(id, "ID3 ", 4) == 0 || memcmp(id, "id3 ", 4) == 0);
        if (!isId3) {
            out.insert(out.end(), id, id + 4);
            out.push_back((uint8_t)(sz & 0xFF));
            out.push_back((uint8_t)((sz >> 8) & 0xFF));
            out.push_back((uint8_t)((sz >> 16) & 0xFF));
            out.push_back((uint8_t)((sz >> 24) & 0xFF));
            out.insert(out.end(), fileData.begin() + (ptrdiff_t)pos, fileData.begin() + (ptrdiff_t)(pos + sz));
            
            // RIFF chunks are padded to an even byte boundary.
            if (sz % 2 != 0) {
                out.push_back(0);
            }
        }
        // Advance over the source chunk and its optional padding byte.
        pos += sz + (sz % 2);
    }

    // Append ID3 chunk
    out.push_back('I'); out.push_back('D'); out.push_back('3'); out.push_back(' ');
    uint32_t id3ChunkSize = (uint32_t)tag.size();
    out.push_back((uint8_t)(id3ChunkSize & 0xFF));
    out.push_back((uint8_t)((id3ChunkSize >> 8) & 0xFF));
    out.push_back((uint8_t)((id3ChunkSize >> 16) & 0xFF));
    out.push_back((uint8_t)((id3ChunkSize >> 24) & 0xFF));
    out.insert(out.end(), tag.begin(), tag.end());
    
    // Pad the appended ID3 chunk to an even byte boundary.
    if (id3ChunkSize % 2 != 0) out.push_back(0); 

    uint32_t riffSize = (uint32_t)(out.size() - 8);
    out[4] = (uint8_t)(riffSize & 0xFF);
    out[5] = (uint8_t)((riffSize >> 8) & 0xFF);
    out[6] = (uint8_t)((riffSize >> 16) & 0xFF);
    out[7] = (uint8_t)((riffSize >> 24) & 0xFF);

    if (!WriteBinaryFile(filePath + ".tmp", out)) return false;

    if (!atomicSwap(filePath, filePath + ".tmp")) return false;
    return true;
}

// OGG/OPUS: not yet implemented (writer)

bool MetadataWriter::writeOGGAlbumArt(const std::string& filePath,
                                     const std::vector<uint8_t>& imageData,
                                     const std::string& mimeType) {
    (void)filePath; (void)imageData; (void)mimeType;
    // Ogg and Opus album-art writes are unsupported; leave the source unchanged.
    return false;
}

// Atomic file swap helper

bool MetadataWriter::atomicSwap(const std::string& originalPath, const std::string& tempPath) {
    return Platform::FileSystem::AtomicFileReplace(originalPath, tempPath);
}

// General Field Writeback — public dispatcher (F-013)

bool MetadataWriter::writeMetadataFields(const std::string& audioFilePath,
                                          const MetadataFieldPatch& patch,
                                          std::string& errorMessage) {
    if (Platform::FileSystem::IsNetworkPath(audioFilePath)) {
        errorMessage = L(Write_BlockedNetworkLibrary);
        return false;
    }
     if (!patch.hasBasicFields() && !patch.hasBWFFields() &&
        !patch.hasTimeReference && !patch.hasIXMLChunk()) {
        errorMessage = "No fields to write";
        return false;
    }
    std::string ext = getExtension(audioFilePath);
    if (ext == ".mp3") {
        return writeID3v2Fields(audioFilePath, patch, errorMessage);
    } else if (ext == ".wav") {
        return writeWAVFields(audioFilePath, patch, errorMessage);
    } else if (ext == ".flac") {
        return writeFLACFields(audioFilePath, patch, errorMessage);
    } else {
        errorMessage = "Unsupported format for field writeback: " + ext;
        return false;
    }
}

static bool WriteHarkenFlacBlock(const std::string& filePath, const std::string& xml,
                                 std::string& errorMessage) {
    std::vector<uint8_t> fileData;
    if (!ReadBinaryFile(filePath, fileData)) { errorMessage = "Cannot open FLAC file"; return false; }
    if (fileData.size() < 42 || std::memcmp(fileData.data(), "fLaC", 4) != 0) {
        errorMessage = "Not a valid FLAC file";
        return false;
    }

    struct Block { uint8_t type; std::vector<uint8_t> data; };
    std::vector<Block> blocks;
    size_t pos = 4;
    bool sawLast = false;
    while (pos + 4 <= fileData.size()) {
        const uint8_t header = fileData[pos];
        const uint8_t type = header & 0x7F;
        const uint32_t length = (static_cast<uint32_t>(fileData[pos + 1]) << 16) |
                                (static_cast<uint32_t>(fileData[pos + 2]) << 8) |
                                static_cast<uint32_t>(fileData[pos + 3]);
        if (pos + 4 + length > fileData.size()) { errorMessage = "Truncated FLAC metadata"; return false; }
        const bool isLast = (header & 0x80) != 0;
        const uint8_t* body = fileData.data() + pos + 4;
        const bool isHarkenApp = type == 2 && length >= 4 && std::memcmp(body, "HARK", 4) == 0;
        if (!isHarkenApp) {
            blocks.push_back(Block{type, std::vector<uint8_t>(body, body + length)});
        }
        pos += 4 + length;
        if (isLast) { sawLast = true; break; }
    }
    if (!sawLast) { errorMessage = "FLAC metadata is missing its final block"; return false; }

    std::vector<uint8_t> payload = {'H', 'A', 'R', 'K'};
    payload.insert(payload.end(), xml.begin(), xml.end());
    blocks.push_back(Block{2, std::move(payload)});

    std::vector<uint8_t> out;
    out.reserve(fileData.size() + xml.size() + 16);
    out.insert(out.end(), {'f', 'L', 'a', 'C'});
    for (size_t i = 0; i < blocks.size(); ++i) {
        const bool last = i + 1 == blocks.size();
        const uint32_t length = static_cast<uint32_t>(blocks[i].data.size());
        out.push_back(static_cast<uint8_t>((last ? 0x80 : 0x00) | (blocks[i].type & 0x7F)));
        out.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(length & 0xFF));
        out.insert(out.end(), blocks[i].data.begin(), blocks[i].data.end());
    }
    out.insert(out.end(), fileData.begin() + static_cast<std::ptrdiff_t>(pos), fileData.end());

    const std::string tempPath = filePath + ".tmp";
    if (!WriteBinaryFile(tempPath, out)) { errorMessage = "Cannot create temp file"; return false; }
    if (!Platform::FileSystem::AtomicFileReplace(filePath, tempPath)) {
        errorMessage = "Failed to replace original file";
        return false;
    }
    return true;
}

bool MetadataWriter::writeHarkenMetadata(const std::string& audioFilePath,
                                         const std::string& harkenMetaJson,
                                         std::string& errorMessage) {
    errorMessage.clear();
    const HarkenMeta::Values values = HarkenMeta::Parse(harkenMetaJson);
    const std::string ext = getExtension(audioFilePath);
    if (ext == ".wav") {
        std::string existing;
        std::string readError;
        const bool hasIxml = MetadataReader::readIXMLChunk(audioFilePath, existing, readError);
        const bool existingHasHarken =
            HarkenMeta::FindCaseInsensitive(existing, "<HARKEN") != std::string::npos;
        if (values.empty() && !existingHasHarken) return true;
        const std::string merged = HarkenMeta::MergeIntoIXML(hasIxml ? existing : std::string(), values);
        if (merged.empty()) return true;
        MetadataFieldPatch patch;
        patch.ixmlChunk = merged;
        return writeMetadataFields(audioFilePath, patch, errorMessage);
    }
    if (ext == ".flac") {
        if (values.empty()) return true;
        return WriteHarkenFlacBlock(audioFilePath, HarkenMeta::BuildIXML(values), errorMessage);
    }
    // MP3/AIFF/OGG and other formats do not carry Harken-owned metadata.
    return true;
}

int MetadataWriter::writeMetadataFieldsBatch(const std::vector<std::string>& audioPaths,
                                              const MetadataFieldPatch& patch,
                                              std::string& errorSummary) {
    int success = 0;
    int fail = 0;
    for (const auto& path : audioPaths) {
        std::string err;
        if (writeMetadataFields(path, patch, err)) {
            success++;
        } else {
            fail++;
            if (fail <= 3) { // Report first 3 errors
                if (!errorSummary.empty()) errorSummary += "; ";
                errorSummary += fs::u8path(path).filename().u8string() + ": " + err;
            }
        }
    }
    if (fail > 3) {
        errorSummary += " ... and " + std::to_string(fail - 3) + " more";
    }
    return success;
}

// ID3v2 Text Frame Helper

static void appendTextFrame(std::vector<uint8_t>& out, int id3Major,
                            const char* frameId, const std::string& text) {
    if (text.empty()) return;
    // Frame ID (4 bytes)
    for (int i = 0; i < 4; i++) out.push_back((uint8_t)frameId[i]);

    // Content: encoding byte (3 = UTF-8) + text bytes
    std::vector<uint8_t> content;
    content.push_back(3); // UTF-8 encoding
    content.insert(content.end(), text.begin(), text.end());

    uint32_t frameSize = (uint32_t)content.size();
    if (id3Major == 4) {
        writeSyncSafeInt(out, frameSize);
    } else {
        writeUint32BE(out, frameSize);
    }
    // Flags
    out.push_back(0);
    out.push_back(0);
    // Content
    out.insert(out.end(), content.begin(), content.end());
}

// Append TXXX (user-defined text) frame
static void appendTXXXFrame(std::vector<uint8_t>& out, int id3Major,
                             const std::string& desc, const std::string& value) {
    if (value.empty()) return;
    out.push_back('T'); out.push_back('X'); out.push_back('X'); out.push_back('X');

    std::vector<uint8_t> content;
    content.push_back(3); // UTF-8
    content.insert(content.end(), desc.begin(), desc.end());
    content.push_back(0); // null separator
    content.insert(content.end(), value.begin(), value.end());

    uint32_t frameSize = (uint32_t)content.size();
    if (id3Major == 4) {
        writeSyncSafeInt(out, frameSize);
    } else {
        writeUint32BE(out, frameSize);
    }
    out.push_back(0); out.push_back(0); // flags
    out.insert(out.end(), content.begin(), content.end());
}

// Append COMM (comment) frame
static void appendCOMMFrame(std::vector<uint8_t>& out, int id3Major,
                             const std::string& text) {
    if (text.empty()) return;
    out.push_back('C'); out.push_back('O'); out.push_back('M'); out.push_back('M');

    std::vector<uint8_t> content;
    content.push_back(3); // UTF-8
    content.push_back('e'); content.push_back('n'); content.push_back('g'); // language
    content.push_back(0); // short description (empty)
    content.insert(content.end(), text.begin(), text.end());

    uint32_t frameSize = (uint32_t)content.size();
    if (id3Major == 4) {
        writeSyncSafeInt(out, frameSize);
    } else {
        writeUint32BE(out, frameSize);
    }
    out.push_back(0); out.push_back(0);
    out.insert(out.end(), content.begin(), content.end());
}

// MP3: Write ID3v2 text fields (preserves existing frames)

bool MetadataWriter::writeID3v2Fields(const std::string& filePath,
                                       const MetadataFieldPatch& patch,
                                       std::string& errorMessage) {
    std::vector<uint8_t> fileData;
    if (!ReadBinaryFile(filePath, fileData)) { errorMessage = "Cannot open file"; return false; }
    const size_t fileSize = fileData.size();

    // Set of frame IDs we'll replace (only for frames we're writing)
    auto shouldReplace = [&](const uint8_t* id, const uint8_t* frameData, uint32_t frameDataSize) -> bool {
        if (memcmp(id, "TIT2", 4) == 0 && !patch.title.empty()) return true;
        if (memcmp(id, "TPE1", 4) == 0 && !patch.artist.empty()) return true;
        if (memcmp(id, "TALB", 4) == 0 && !patch.album.empty()) return true;
        if (memcmp(id, "TCON", 4) == 0 && !patch.genre.empty()) return true;
        if (memcmp(id, "COMM", 4) == 0 && !patch.description.empty()) return true;
        // TXXX: only replace if description matches our fields
        if (memcmp(id, "TXXX", 4) == 0 && frameDataSize > 1) {
            // Parse: encoding(1) + description(null-terminated)
            // Find null terminator after description
            size_t descStart = 1;
            size_t descEnd = descStart;
            while (descEnd < frameDataSize && frameData[descEnd] != 0) descEnd++;
            std::string desc((const char*)frameData + descStart, descEnd - descStart);
            if ((desc == "KEYWORDS" && !patch.keywords.empty()) ||
                (desc == "CATEGORY" && !patch.category.empty())) {
                return true;
            }
        }
        return false;
    };

    // Parse existing ID3v2
    size_t audioDataStart = 0;
    int id3Major = 3;
    uint8_t id3Flags = 0;
    std::vector<uint8_t> keptFrames;

    if (fileSize >= 10 && fileData[0] == 'I' && fileData[1] == 'D' && fileData[2] == '3') {
        id3Major = (int)fileData[3];
        if (id3Major != 3 && id3Major != 4) id3Major = 3;
        id3Flags = fileData[5];
        uint32_t tagSize = readSyncSafeInt(&fileData[6]);
        size_t tagEnd = 10 + (size_t)tagSize;
        if (tagEnd > fileSize) tagEnd = fileSize;
        audioDataStart = tagEnd;

        size_t pos = 10;
        if (id3Flags & 0x40) { // Extended header
            if (pos + 4 <= tagEnd) {
                uint32_t extSize = (id3Major == 4) ? readSyncSafeInt(&fileData[pos]) : readUint32BE(&fileData[pos]);
                pos += 4 + (size_t)extSize;
            }
        }
        while (pos + 10 <= tagEnd) {
            const uint8_t* id = &fileData[pos];
            if (id[0] == 0 && id[1] == 0 && id[2] == 0 && id[3] == 0) break;
            if (!isFrameIdValid4(id)) break;
            uint32_t fsize = (id3Major == 4) ? readSyncSafeInt(&fileData[pos + 4]) : readUint32BE(&fileData[pos + 4]);
            if (fsize == 0) break;
            size_t frameTotal = 10 + (size_t)fsize;
            if (pos + frameTotal > tagEnd) break;

            const uint8_t* frameContent = fileData.data() + pos + 10;
            if (!shouldReplace(id, frameContent, fsize)) {
                keptFrames.insert(keptFrames.end(), fileData.begin() + (ptrdiff_t)pos,
                                  fileData.begin() + (ptrdiff_t)(pos + frameTotal));
            }
            pos += frameTotal;
        }
    }

    // Build new tag
    std::vector<uint8_t> newTag;
    newTag.reserve(keptFrames.size() + 1024);
    newTag.push_back('I'); newTag.push_back('D'); newTag.push_back('3');
    newTag.push_back((uint8_t)id3Major);
    newTag.push_back(0); // revision
    newTag.push_back((uint8_t)(id3Flags & 0x40)); // preserve only ext header flag
    size_t sizePos = newTag.size();
    newTag.push_back(0); newTag.push_back(0); newTag.push_back(0); newTag.push_back(0);

    // Kept frames
    newTag.insert(newTag.end(), keptFrames.begin(), keptFrames.end());

    // New frames
    if (!patch.title.empty())       appendTextFrame(newTag, id3Major, "TIT2", patch.title);
    if (!patch.artist.empty())      appendTextFrame(newTag, id3Major, "TPE1", patch.artist);
    if (!patch.album.empty())       appendTextFrame(newTag, id3Major, "TALB", patch.album);
    if (!patch.genre.empty())       appendTextFrame(newTag, id3Major, "TCON", patch.genre);
    if (!patch.description.empty()) appendCOMMFrame(newTag, id3Major, patch.description);
    if (!patch.keywords.empty())    appendTXXXFrame(newTag, id3Major, "KEYWORDS", patch.keywords);
    if (!patch.category.empty())    appendTXXXFrame(newTag, id3Major, "CATEGORY", patch.category);

    // Update size
    uint32_t tagContentSize = (uint32_t)(newTag.size() - 10);
    newTag[sizePos]     = (tagContentSize >> 21) & 0x7F;
    newTag[sizePos + 1] = (tagContentSize >> 14) & 0x7F;
    newTag[sizePos + 2] = (tagContentSize >>  7) & 0x7F;
    newTag[sizePos + 3] =  tagContentSize        & 0x7F;

    // Write temp file
    std::string tempPath = filePath + ".tmp";
    const std::string_view tagBytes(
        reinterpret_cast<const char*>(newTag.data()), newTag.size());
    const std::string_view audioPayload(
        reinterpret_cast<const char*>(fileData.data() + audioDataStart),
        fileSize - audioDataStart);
    if (!WriteBinaryFile(tempPath, {tagBytes, audioPayload})) {
        errorMessage = "Cannot create temp file";
        return false;
    }

    if (!atomicSwap(filePath, tempPath)) {
        errorMessage = "Failed to replace original file";
        return false;
    }
    return true;
}

// WAV: Write LIST/INFO + BEXT chunks

static void writeLE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
}

static void writeInfoSubChunk(std::vector<uint8_t>& out, const char* id, const std::string& val) {
    if (val.empty()) return;
    out.push_back((uint8_t)id[0]); out.push_back((uint8_t)id[1]);
    out.push_back((uint8_t)id[2]); out.push_back((uint8_t)id[3]);
    uint32_t dataSize = (uint32_t)(val.size() + 1); // include null terminator
    writeLE32(out, dataSize);
    out.insert(out.end(), val.begin(), val.end());
    out.push_back(0); // null terminator
    if (dataSize & 1) out.push_back(0); // pad to even
}

bool MetadataWriter::writeWAVFields(const std::string& filePath,
                                     const MetadataFieldPatch& patch,
                                     std::string& errorMessage) {
    std::vector<uint8_t> fileData;
    if (!ReadBinaryFile(filePath, fileData)) { errorMessage = "Cannot open WAV file"; return false; }
    const size_t fileSize = fileData.size();
    if (fileSize < 44) { errorMessage = "File too small"; return false; }

    // Verify RIFF/WAVE header
    if (memcmp(fileData.data(), "RIFF", 4) != 0 || memcmp(fileData.data() + 8, "WAVE", 4) != 0) {
        errorMessage = "Not a valid WAV file";
        return false;
    }

    // Collect existing chunks, replacing LIST/INFO and bext
    std::vector<uint8_t> outFile;
    outFile.reserve(fileSize + 4096);
    outFile.insert(outFile.end(), fileData.begin(), fileData.begin() + 12); // RIFF + WAVE header

    size_t pos = 12;
    bool wroteLIST = false, wroteBEXT = false;
    std::vector<uint8_t> existingBextData; // preserve old bext for merging

    while (pos + 8 <= fileSize) {
        char chunkId[5] = {};
        memcpy(chunkId, fileData.data() + pos, 4);
        uint32_t chunkSize = *(uint32_t*)(fileData.data() + pos + 4); // LE
        size_t chunkTotal = 8 + chunkSize;
        if (chunkSize & 1) chunkTotal++; // pad byte

        bool skipChunk = false;

        // Replace LIST/INFO chunk
        if (memcmp(chunkId, "LIST", 4) == 0 && pos + 12 <= fileSize &&
            memcmp(fileData.data() + pos + 8, "INFO", 4) == 0 && patch.hasBasicFields()) {
            skipChunk = true;
        }
        // Replace bext chunk — but save old data for merging
        if (memcmp(chunkId, "bext", 4) == 0 && (patch.hasBWFFields() || patch.hasTimeReference)) {
            if (chunkSize > 0 && pos + 8 + chunkSize <= fileSize) {
                existingBextData.assign(fileData.begin() + pos + 8, fileData.begin() + pos + 8 + chunkSize);
            }
            skipChunk = true;
        }
        // Replace an existing iXML chunk instead of leaving it in place and
        // appending a second one below.
        if ((memcmp(chunkId, "iXML", 4) == 0 || memcmp(chunkId, "IXML", 4) == 0) && patch.hasIXMLChunk()) {
            skipChunk = true;
        }

        if (!skipChunk) {
            size_t end = pos + chunkTotal;
            if (end > fileSize) end = fileSize;
            outFile.insert(outFile.end(), fileData.begin() + pos, fileData.begin() + end);
        }
        pos += chunkTotal;
    }

    // Append new LIST/INFO chunk
    if (patch.hasBasicFields()) {
        std::vector<uint8_t> infoData;
        infoData.push_back('I'); infoData.push_back('N'); infoData.push_back('F'); infoData.push_back('O');
        if (!patch.title.empty())       writeInfoSubChunk(infoData, "INAM", patch.title);
        if (!patch.artist.empty())      writeInfoSubChunk(infoData, "IART", patch.artist);
        if (!patch.description.empty()) writeInfoSubChunk(infoData, "ICMT", patch.description);
        if (!patch.keywords.empty())    writeInfoSubChunk(infoData, "IKEY", patch.keywords);
        if (!patch.category.empty())    writeInfoSubChunk(infoData, "IGNR", patch.category);
        if (!patch.album.empty())       writeInfoSubChunk(infoData, "IPRD", patch.album);

        outFile.push_back('L'); outFile.push_back('I'); outFile.push_back('S'); outFile.push_back('T');
        writeLE32(outFile, (uint32_t)infoData.size());
        outFile.insert(outFile.end(), infoData.begin(), infoData.end());
        wroteLIST = true;
    }

    // Append new bext chunk (merge with existing data to preserve unpatched fields)
    if (patch.hasBWFFields() || patch.hasTimeReference) {
        // Start with existing bext data as base, or zeroed if no existing chunk
        size_t bextMinSize = 602;
        std::vector<uint8_t> bextData;
        if (!existingBextData.empty()) {
            bextData = existingBextData;
            if (bextData.size() < bextMinSize) bextData.resize(bextMinSize, 0);
        } else {
            bextData.assign(bextMinSize, 0);
            bextData[346] = 1; bextData[347] = 0; // Version 1 default
        }
        // Only overwrite fields that are present in the patch
        // Description: 256 bytes at offset 0
        if (!patch.description.empty()) {
            memset(bextData.data(), 0, 256);
            size_t len = std::min(patch.description.size(), (size_t)256);
            memcpy(bextData.data(), patch.description.data(), len);
        }
        // Originator: 32 bytes at offset 256
        if (!patch.bwOriginator.empty()) {
            memset(bextData.data() + 256, 0, 32);
            size_t len = std::min(patch.bwOriginator.size(), (size_t)32);
            memcpy(bextData.data() + 256, patch.bwOriginator.data(), len);
        }
        // OriginatorReference: 32 bytes at offset 288
        if (!patch.bwOriginatorRef.empty()) {
            memset(bextData.data() + 288, 0, 32);
            size_t len = std::min(patch.bwOriginatorRef.size(), (size_t)32);
            memcpy(bextData.data() + 288, patch.bwOriginatorRef.data(), len);
        }
        // OriginationDate: 10 bytes at offset 320
        if (!patch.bwDate.empty()) {
            memset(bextData.data() + 320, 0, 10);
            size_t len = std::min(patch.bwDate.size(), (size_t)10);
            memcpy(bextData.data() + 320, patch.bwDate.data(), len);
        }
        // OriginationTime: 8 bytes at offset 330
        if (!patch.bwTime.empty()) {
            memset(bextData.data() + 330, 0, 8);
            size_t len = std::min(patch.bwTime.size(), (size_t)8);
            memcpy(bextData.data() + 330, patch.bwTime.data(), len);
        }
        if (patch.hasTimeReference) {
            const uint32_t lo = (uint32_t)(patch.bwTimeReference & 0xFFFFFFFFull);
            const uint32_t hi = (uint32_t)(patch.bwTimeReference >> 32);
            bextData[338] = (uint8_t)(lo & 0xFF);
            bextData[339] = (uint8_t)((lo >> 8) & 0xFF);
            bextData[340] = (uint8_t)((lo >> 16) & 0xFF);
            bextData[341] = (uint8_t)((lo >> 24) & 0xFF);
            bextData[342] = (uint8_t)(hi & 0xFF);
            bextData[343] = (uint8_t)((hi >> 8) & 0xFF);
            bextData[344] = (uint8_t)((hi >> 16) & 0xFF);
            bextData[345] = (uint8_t)((hi >> 24) & 0xFF);
        }
        // Version (346) preserved from existing data
        // UMID: 64 bytes at offset 348
        if (!patch.bwUMID.empty()) {
            size_t len = std::min(patch.bwUMID.size(), (size_t)64);
            memcpy(bextData.data() + 348, patch.bwUMID.data(), len);
        }

        outFile.push_back('b'); outFile.push_back('e'); outFile.push_back('x'); outFile.push_back('t');
        writeLE32(outFile, (uint32_t)bextData.size());
        outFile.insert(outFile.end(), bextData.begin(), bextData.end());
        wroteBEXT = true;
    }

    if (patch.hasIXMLChunk()) {
        const std::string& xml = patch.ixmlChunk;
        outFile.push_back('i'); outFile.push_back('X'); outFile.push_back('M'); outFile.push_back('L');
        writeLE32(outFile, (uint32_t)xml.size());
        outFile.insert(outFile.end(), xml.begin(), xml.end());
        if (xml.size() & 1) outFile.push_back(0);
    }

    // Update RIFF size
    uint32_t riffSize = (uint32_t)(outFile.size() - 8);
    outFile[4] = riffSize & 0xFF;
    outFile[5] = (riffSize >> 8) & 0xFF;
    outFile[6] = (riffSize >> 16) & 0xFF;
    outFile[7] = (riffSize >> 24) & 0xFF;

    // Write temp and swap
    std::string tempPath = filePath + ".tmp";
    if (!WriteBinaryFile(tempPath, outFile)) {
        errorMessage = "Cannot create temp file";
        return false;
    }

    if (!atomicSwap(filePath, tempPath)) {
        errorMessage = "Failed to replace original file";
        return false;
    }
    return true;
}

// FLAC: Write Vorbis Comment block

bool MetadataWriter::writeFLACFields(const std::string& filePath,
                                      const MetadataFieldPatch& patch,
                                      std::string& errorMessage) {
    std::vector<uint8_t> fileData;
    if (!ReadBinaryFile(filePath, fileData)) { errorMessage = "Cannot open FLAC file"; return false; }
    const size_t fileSize = fileData.size();
    if (fileSize < 42) { errorMessage = "File too small for FLAC"; return false; }

    // Verify fLaC header
    if (memcmp(fileData.data(), "fLaC", 4) != 0) {
        errorMessage = "Not a valid FLAC file";
        return false;
    }

    // Patch fields: uppercase key → value (will override existing comments)
    std::unordered_map<std::string, std::string> patchFields;
    if (!patch.title.empty())       patchFields["TITLE"] = patch.title;
    if (!patch.artist.empty())      patchFields["ARTIST"] = patch.artist;
    if (!patch.album.empty())       patchFields["ALBUM"] = patch.album;
    if (!patch.description.empty()) patchFields["DESCRIPTION"] = patch.description;
    if (!patch.keywords.empty())    patchFields["KEYWORDS"] = patch.keywords;
    if (!patch.category.empty())    patchFields["GENRE"] = patch.category;
    if (!patch.comment.empty())     patchFields["COMMENT"] = patch.comment;
    if (patchFields.empty()) { errorMessage = "No fields to write"; return false; }

    // Helper: build Vorbis Comment block from merged comments list
    auto buildVCBlock = [](const std::string& vendor, const std::vector<std::string>& comments) {
        std::vector<uint8_t> vc;
        uint32_t vLen = (uint32_t)vendor.size();
        vc.push_back(vLen & 0xFF); vc.push_back((vLen >> 8) & 0xFF);
        vc.push_back((vLen >> 16) & 0xFF); vc.push_back((vLen >> 24) & 0xFF);
        vc.insert(vc.end(), vendor.begin(), vendor.end());
        uint32_t cnt = (uint32_t)comments.size();
        vc.push_back(cnt & 0xFF); vc.push_back((cnt >> 8) & 0xFF);
        vc.push_back((cnt >> 16) & 0xFF); vc.push_back((cnt >> 24) & 0xFF);
        for (const auto& c : comments) {
            uint32_t cLen = (uint32_t)c.size();
            vc.push_back(cLen & 0xFF); vc.push_back((cLen >> 8) & 0xFF);
            vc.push_back((cLen >> 16) & 0xFF); vc.push_back((cLen >> 24) & 0xFF);
            vc.insert(vc.end(), c.begin(), c.end());
        }
        return vc;
    };

    // Helper: parse existing Vorbis Comment block, merge with patch, return new block
    auto mergeVorbisComments = [&](const uint8_t* data, size_t len) -> std::vector<uint8_t> {
        std::string vendor = "HarkenSearch/2.0";
        std::vector<std::string> merged;
        std::set<std::string> seenKeys; // track which keys we've output (uppercase)

        if (len >= 4) {
            size_t off = 0;
            uint32_t vLen = data[off] | (data[off+1]<<8) | (data[off+2]<<16) | (data[off+3]<<24);
            off += 4;
            if (off + vLen <= len) {
                vendor = std::string((const char*)data + off, vLen);
                off += vLen;
            }
            if (off + 4 <= len) {
                uint32_t cnt = data[off] | (data[off+1]<<8) | (data[off+2]<<16) | (data[off+3]<<24);
                off += 4;
                for (uint32_t i = 0; i < cnt && off + 4 <= len; i++) {
                    uint32_t cLen = data[off] | (data[off+1]<<8) | (data[off+2]<<16) | (data[off+3]<<24);
                    off += 4;
                    if (off + cLen > len) break;
                    std::string comment((const char*)data + off, cLen);
                    off += cLen;
                    // Extract key (before '=')
                    auto eq = comment.find('=');
                    if (eq != std::string::npos) {
                        std::string key = comment.substr(0, eq);
                        // Uppercase for comparison
                        std::string keyUpper = key;
                        for (auto& c : keyUpper) c = (char)toupper((unsigned char)c);
                        if (patchFields.count(keyUpper)) {
                            // Replace with patch value
                            merged.push_back(keyUpper + "=" + patchFields[keyUpper]);
                            seenKeys.insert(keyUpper);
                        } else {
                            // Keep existing
                            merged.push_back(comment);
                        }
                    } else {
                        merged.push_back(comment);
                    }
                }
            }
        }
        // Add any patch fields not already present
        for (const auto& [key, val] : patchFields) {
            if (!seenKeys.count(key)) {
                merged.push_back(key + "=" + val);
            }
        }
        return buildVCBlock(vendor, merged);
    };

    // Rebuild: fLaC header + iterate metadata blocks, replace/insert vorbis_comment (type 4)
    std::vector<uint8_t> outFile;
    outFile.reserve(fileSize + 4096);
    outFile.insert(outFile.end(), fileData.begin(), fileData.begin() + 4); // fLaC

    size_t pos = 4;
    bool replacedVC = false;
    bool isLast = false;

    while (pos + 4 <= fileSize && !isLast) {
        uint8_t blockHeader = fileData[pos];
        isLast = (blockHeader & 0x80) != 0;
        uint8_t blockType = blockHeader & 0x7F;
        uint32_t blockLen = ((uint32_t)fileData[pos+1] << 16) | ((uint32_t)fileData[pos+2] << 8) | fileData[pos+3];
        size_t blockTotal = 4 + blockLen;

        if (blockType == 4) {
            // Merge patch with existing Vorbis Comments
            const uint8_t* existingData = (blockLen > 0 && pos + 4 + blockLen <= fileSize) ? fileData.data() + pos + 4 : nullptr;
            std::vector<uint8_t> vcBlock = mergeVorbisComments(existingData, existingData ? blockLen : 0);
            uint8_t hdr = 4; // type 4, not last (will fix later)
            outFile.push_back(hdr);
            uint32_t vcLen = (uint32_t)vcBlock.size();
            outFile.push_back((vcLen >> 16) & 0xFF);
            outFile.push_back((vcLen >> 8) & 0xFF);
            outFile.push_back(vcLen & 0xFF);
            outFile.insert(outFile.end(), vcBlock.begin(), vcBlock.end());
            replacedVC = true;
        } else {
            // Keep as-is (but clear isLast flag, we'll set it on the final block)
            outFile.push_back(blockType); // clear isLast bit
            outFile.push_back(fileData[pos+1]);
            outFile.push_back(fileData[pos+2]);
            outFile.push_back(fileData[pos+3]);
            if (blockLen > 0 && pos + blockTotal <= fileSize) {
                outFile.insert(outFile.end(), fileData.begin() + pos + 4,
                               fileData.begin() + pos + blockTotal);
            }
        }
        pos += blockTotal;
    }

    if (!replacedVC) {
        // Insert new Vorbis Comment block (no existing block to merge with)
        std::vector<uint8_t> vcBlock = mergeVorbisComments(nullptr, 0);
        uint8_t hdr = 4; // type 4
        outFile.push_back(hdr);
        uint32_t vcLen = (uint32_t)vcBlock.size();
        outFile.push_back((vcLen >> 16) & 0xFF);
        outFile.push_back((vcLen >> 8) & 0xFF);
        outFile.push_back(vcLen & 0xFF);
        outFile.insert(outFile.end(), vcBlock.begin(), vcBlock.end());
    }

    // Mark the last metadata block header's isLast bit
    // Walk from offset 4 to find the last block header
    {
        size_t scanPos = 4;
        size_t lastBlockHeaderPos = 4;
        while (scanPos + 4 <= outFile.size()) {
            uint32_t bl = ((uint32_t)outFile[scanPos+1] << 16) | ((uint32_t)outFile[scanPos+2] << 8) | outFile[scanPos+3];
            lastBlockHeaderPos = scanPos;
            scanPos += 4 + bl;
            if (scanPos >= pos) break; // past metadata into audio
        }
        outFile[lastBlockHeaderPos] |= 0x80; // set isLast
    }

    // Append audio frames
    if (pos < fileSize) {
        outFile.insert(outFile.end(), fileData.begin() + pos, fileData.end());
    }

    // Write temp and swap
    std::string tempPath = filePath + ".tmp";
    if (!WriteBinaryFile(tempPath, outFile)) {
        errorMessage = "Cannot create temp file";
        return false;
    }

    if (!atomicSwap(filePath, tempPath)) {
        errorMessage = "Failed to replace original file";
        return false;
    }
    return true;
}
