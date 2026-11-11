#include "platform/pe_inspection.h"
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
        check(inspect::ReadFileVersion(argv[2]).status == inspect::Status::Unsupported,
              "version metadata remains explicitly unimplemented");
        check(!fs::exists(marker), "all invalid input checks remained non-executing");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
