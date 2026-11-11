#include "platform/pe_inspection.h"
#include "ReaperBridgeVersion.h"
#include <mach-o/loader.h>
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
