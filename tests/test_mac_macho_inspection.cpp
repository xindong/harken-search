#include "platform/pe_inspection.h"
#include "ReaperBridgeVersion.h"
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach-o/nlist.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
namespace inspect = Platform::PeInspection;
static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}
static void save(const fs::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), bytes.size());
    check(out.good(), "damaged fixture bytes were written");
}
template<class T> static T readAt(const std::vector<char>& bytes, size_t offset) {
    check(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "fixture structure is in bounds");
    T value {}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
template<class T> static void writeAt(std::vector<char>& bytes, size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
// Synthetic container, actual Intel image bytes; the other slice is not a runnable host.
static void integer(std::vector<char>& out, size_t at, uint64_t value, size_t width, bool little) {
    for (size_t i = 0; i < width; ++i)
        out[at + i] = static_cast<char>(value >> (8 * (little ? i : width - 1 - i)));
}
static std::vector<char> universal(const std::vector<char>& image, bool wide, bool little) {
    const size_t second = (4096 + image.size() + 4095) & ~size_t(4095);
    std::vector<char> out(second + 32);
    integer(out, 0, wide ? FAT_MAGIC_64 : FAT_MAGIC, 4, little);
    integer(out, 4, 2, 4, little);
    const size_t stride = wide ? 32 : 20;
    const auto h = readAt<mach_header_64>(image, 0);
    for (size_t i = 0; i < 2; ++i) {
        const size_t at = 8 + i * stride;
        integer(out, at, i ? CPU_TYPE_ARM64 : h.cputype, 4, little);
        integer(out, at + 4, i ? 0 : h.cpusubtype, 4, little);
        integer(out, at + 8, i ? second : 4096, wide ? 8 : 4, little);
        integer(out, at + (wide ? 16 : 12), i ? 32 : image.size(), wide ? 8 : 4, little);
        integer(out, at + (wide ? 24 : 16), 12, 4, little);
    }
    std::copy(image.begin(), image.end(), out.begin() + 4096);
    return out;
}
int main(int argc, char** argv) {
    try {
        check(argc == 4 && fs::path(argv[1]).is_absolute(), "explicit QA data and two real libraries supplied");
        const fs::path root(argv[1]);
        fs::create_directories(root);
        const auto marker = root / "unexpected-load.txt";
        check(!fs::exists(marker), "test initializer has not run");
        check(setenv("HARKEN_INSPECTION_LOAD_MARKER", marker.c_str(), 1) == 0,
              "test initializer marker is configured");
        for (int i = 2; i < 4; ++i) {
            auto arch = inspect::ReadArchitecture(argv[i]);
            check(arch.status == inspect::Status::Success && arch.value == "x64",
                  "actual Intel Mach-O architecture is read");
            auto entry = inspect::ProbeExport(argv[i], "ReaperPluginEntry");
            check(entry.status == inspect::Status::Success && entry.present,
                  "actual REAPER entry export is found without loading");
            auto absent = inspect::ProbeExport(argv[i], "NoSuchHarkenExport");
            check(absent.status == inspect::Status::Success && !absent.present,
                  "missing export is distinguished from invalid binary");
        }
        check(!fs::exists(marker), "inspection never executed the library initializer");
        for (const char* name : {"HiddenInspectionSymbol", "fopen"}) {
            const auto result = inspect::ProbeExport(argv[2], name);
            check(result.status == inspect::Status::Success && !result.present,
                  "hidden definitions and imported symbols do not count as exports");
        }
        check(inspect::ReadArchitecture("").status == inspect::Status::InvalidArgument,
              "empty binary path is rejected");
        check(inspect::ReadArchitecture(std::string("bad\0path", 8)).status == inspect::Status::InvalidArgument,
              "embedded NUL path is rejected");
        check(inspect::ReadArchitecture((root / "missing").string()).status == inspect::Status::OpenFailed,
              "missing file reports open failure");
        std::ifstream in(argv[2], std::ios::binary);
        std::vector<char> bytes{std::istreambuf_iterator<char>(in), {}};
        const auto damaged = root / "damaged.dylib";
        save(damaged, {'n','o','t','-','m','a','c','h','o'});
        check(inspect::ReadArchitecture(damaged.string()).status == inspect::Status::InvalidFormat,
              "plain text cannot pass architecture inspection");
        auto truncated = bytes; truncated.resize(sizeof(mach_header_64) - 1); save(damaged, truncated);
        check(inspect::ProbeExport(damaged.string(), "ReaperPluginEntry").status == inspect::Status::InvalidFormat,
              "truncated header is rejected");
        auto badCommands = bytes;
        auto header = readAt<mach_header_64>(bytes, 0);
        header.sizeofcmds = 0xffffffffU; writeAt(badCommands, 0, header); save(damaged, badCommands);
        check(inspect::ReadArchitecture(damaged.string()).status == inspect::Status::InvalidFormat,
              "out of bounds command table is rejected");
        auto badSize = bytes;
        auto command = readAt<load_command>(bytes, sizeof(mach_header_64));
        command.cmdsize = 0; writeAt(badSize, sizeof(mach_header_64), command); save(damaged, badSize);
        check(inspect::ProbeExport(damaged.string(), "ReaperPluginEntry").status == inspect::Status::InvalidFormat,
              "zero length command cannot stall inspection");
        auto unsupported = bytes;
        header = readAt<mach_header_64>(bytes, 0); header.cputype = CPU_TYPE_ARM64;
        writeAt(unsupported, 0, header); save(damaged, unsupported);
        check(inspect::ReadArchitecture(damaged.string()).status == inspect::Status::Unsupported,
              "untested architecture cannot count as supported native host");
        header = readAt<mach_header_64>(bytes, 0);
        size_t offset = sizeof(header), symOffset = 0;
        symtab_command sym {};
        for (uint32_t i = 0; i < header.ncmds; ++i) {
            command = readAt<load_command>(bytes, offset);
            if (command.cmd == LC_SYMTAB) { symOffset = offset; sym = readAt<symtab_command>(bytes, offset); break; }
            offset += command.cmdsize;
        }
        check(symOffset && sym.nsyms && sym.strsize, "actual fixture has a symbol and string table");
        auto badSymbols = bytes;
        auto corrupt = sym; corrupt.symoff = 0xffffffffU;
        writeAt(badSymbols, symOffset, corrupt); save(damaged, badSymbols);
        check(inspect::ProbeExport(damaged.string(), "ReaperPluginEntry").status == inspect::Status::InvalidFormat,
              "out of bounds symbol table is rejected");
        auto badStrings = bytes;
        std::fill(badStrings.begin() + sym.stroff, badStrings.begin() + sym.stroff + sym.strsize, 'X');
        save(damaged, badStrings);
        check(inspect::ProbeExport(damaged.string(), "ReaperPluginEntry").status == inspect::Status::InvalidFormat,
              "unterminated symbol strings are rejected");
        check(inspect::ReadFileVersion(argv[2]).status == inspect::Status::ValueUnavailable,
              "unmarked test library does not acquire a fabricated version");
        auto version = inspect::ReadFileVersion(argv[3]);
        check(version.status == inspect::Status::Success && version.value == ARIS_REAPER_BRIDGE_VERSION_STRING,
              "actual extension exposes the existing bridge version");
        auto product = inspect::ReadVersionString(argv[3], "ProductName");
        check(product.status == inspect::Status::Success && product.value == "HarkenSearch",
              "actual extension exposes product identity");
        check(inspect::ReadVersionString(argv[3], "AbsentMetadataKey").status == inspect::Status::ValueUnavailable,
              "missing metadata key remains distinct from a valid value");
        auto protocol = inspect::ReadVersionString(argv[3], "BridgeProtocolVersion");
        check(protocol.status == inspect::Status::Success && protocol.value ==
              std::to_string(ARIS_REAPER_BRIDGE_PROTOCOL_VERSION), "metadata retains the existing protocol version");
        std::ifstream componentInput(argv[3], std::ios::binary);
        const std::vector<char> component{std::istreambuf_iterator<char>(componentInput), {}};
        for (bool wide : {false, true}) for (bool little : {false, true}) {
            for (const auto* image : {static_cast<const std::vector<char>*>(&bytes), &component}) {
                auto fat = universal(*image, wide, little); save(damaged, fat);
                auto a = inspect::ReadArchitecture(damaged.string());
                check(a.status == inspect::Status::Success && a.value == "x64",
                      "universal container selects its actual Intel slice in either byte order");
                auto e = inspect::ProbeExport(damaged.string(), "ReaperPluginEntry");
                check(e.status == inspect::Status::Success && e.present,
                      "universal exports use slice-relative offsets");
                auto v = inspect::ReadFileVersion(damaged.string());
                check(image == &bytes ? v.status == inspect::Status::ValueUnavailable :
                      v.status == inspect::Status::Success && v.value == ARIS_REAPER_BRIDGE_VERSION_STRING,
                      "universal metadata stays in the selected slice");
            }
            auto reordered = universal(component, wide, little);
            const size_t recordSize = wide ? 32 : 20;
            for (size_t j = 0; j < recordSize; ++j) std::swap(reordered[8 + j], reordered[8 + recordSize + j]);
            save(damaged, reordered);
            check(inspect::ProbeExport(damaged.string(), "ReaperPluginEntry").present,
                  "Intel slice need not be first in the container");
            const auto valid = universal(component, wide, little);
            const size_t stride = wide ? 32 : 20;
            auto reject = [&](std::vector<char> bad, const char* why) {
                save(damaged, bad);
                check(inspect::ReadArchitecture(damaged.string()).status == inspect::Status::InvalidFormat, why);
            };
            auto bad = valid; integer(bad, 4, 0, 4, little); reject(bad, "empty architecture table rejected");
            bad = valid; integer(bad, 4, 65, 4, little); reject(bad, "excessive architecture count rejected");
            bad = valid; bad.resize(8 + stride - 1); reject(bad, "truncated architecture table rejected");
            bad = valid; integer(bad, 16, 8, wide ? 8 : 4, little); reject(bad, "slice cannot overlap container table");
            bad = valid; integer(bad, 16, ~uint64_t(0), wide ? 8 : 4, little); reject(bad, "overflowing slice offset rejected");
            bad = valid; integer(bad, 8 + (wide ? 24 : 16), 64, 4, little); reject(bad, "invalid alignment shift rejected");
            bad = valid; integer(bad, 16, 4097, wide ? 8 : 4, little); reject(bad, "misaligned slice rejected");
            bad = valid; integer(bad, 8 + stride + 8, 4096, wide ? 8 : 4, little); reject(bad, "overlapping slices rejected");
            bad = valid; integer(bad, 8 + stride, CPU_TYPE_X86_64, 4, little); reject(bad, "ambiguous Intel slices rejected");
            bad = valid; bad[4096 + 4] ^= 1; reject(bad, "advertised and actual CPU must agree");
            bad = valid; integer(bad, 12, 0x1234, 4, little); reject(bad, "advertised and actual CPU subtype must agree");
            bad = valid; integer(bad, 8 + (wide ? 16 : 12), sizeof(mach_header_64), wide ? 8 : 4, little);
            reject(bad, "load commands cannot escape selected slice");
            // Bytes past the selected slice exist in the outer container but are not readable as its data.
            bad = universal(bytes, wide, little);
            auto escaped = sym; escaped.stroff = static_cast<uint32_t>(bytes.size()); escaped.strsize = 1;
            writeAt(bad, 4096 + symOffset, escaped);
            reject(bad, "symbol strings cannot escape into container padding");
            bad = valid; integer(bad, 8, CPU_TYPE_ARM64, 4, little); save(damaged, bad);
            check(inspect::ReadArchitecture(damaged.string()).status == inspect::Status::Unsupported,
                  "ARM-only container does not claim native support");
            if (wide) { bad = valid; integer(bad, 36, 1, 4, little); reject(bad, "fat64 reserved field must be zero"); }
        }
        auto system = inspect::ReadArchitecture("/System/Applications/TextEdit.app/Contents/MacOS/TextEdit");
        check(system.status == inspect::Status::Success && system.value == "x64",
              "actual system universal executable exposes its Intel slice without launch");
        const auto componentHeader = readAt<mach_header_64>(component, 0);
        offset = sizeof(componentHeader);
        size_t metadataRecord = 0;
        section_64 metadata {};
        for (uint32_t i = 0; i < componentHeader.ncmds; ++i) {
            const auto load = readAt<load_command>(component, offset);
            if (load.cmd == LC_SEGMENT_64) {
                const auto segment = readAt<segment_command_64>(component, offset);
                for (uint32_t j = 0; j < segment.nsects; ++j) {
                    const size_t position = offset + sizeof(segment) + j * sizeof(section_64);
                    const auto section = readAt<section_64>(component, position);
                    if (std::strncmp(section.sectname, "__harken_meta", sizeof(section.sectname)) == 0) {
                        metadata = section; metadataRecord = position;
                    }
                }
            }
            offset += load.cmdsize;
        }
        check(metadataRecord && metadata.size && metadata.offset + metadata.size <= component.size(),
              "actual extension has a bounded embedded metadata section");
        auto badMetadata = component;
        auto outside = metadata; outside.offset = 0xffffffffU;
        writeAt(badMetadata, metadataRecord, outside); save(damaged, badMetadata);
        check(inspect::ReadFileVersion(damaged.string()).status == inspect::Status::InvalidFormat,
              "out of bounds metadata cannot produce a version");
        badMetadata = component; badMetadata[metadata.offset] ^= 1; save(damaged, badMetadata);
        check(inspect::ReadFileVersion(damaged.string()).status == inspect::Status::InvalidFormat,
              "unknown metadata frame is rejected");
        badMetadata = component;
        const std::string oldKey = "FileVersion=", duplicateKey = "ProductName=";
        auto duplicate = std::search(badMetadata.begin() + metadata.offset,
            badMetadata.begin() + metadata.offset + metadata.size, oldKey.begin(), oldKey.end());
        check(duplicate != badMetadata.begin() + metadata.offset + metadata.size,
              "version field found in actual metadata");
        std::copy(duplicateKey.begin(), duplicateKey.end(), duplicate); save(damaged, badMetadata);
        check(inspect::ReadVersionString(damaged.string(), "ProductName").status == inspect::Status::InvalidFormat,
              "duplicate metadata identity cannot produce a trusted value");
        badMetadata = component; badMetadata[metadata.offset + metadata.size - 1] = 'X'; save(damaged, badMetadata);
        check(inspect::ReadFileVersion(damaged.string()).status == inspect::Status::InvalidFormat,
              "unterminated metadata or nonzero tail is rejected");
        check(!fs::exists(marker), "all invalid input checks remained non-executing");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
