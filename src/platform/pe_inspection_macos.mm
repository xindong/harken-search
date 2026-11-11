#ifdef __APPLE__

#include "pe_inspection.h"

#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach-o/nlist.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <array>
#include <algorithm>
#include <map>
#include <string_view>

namespace Platform::PeInspection {
namespace {

// Inspect bytes only. Never map an image into the process or run its initializers.
class MachFile {
public:
    explicit MachFile(const std::string& path) {
        if (path.empty() || path.find('\0') != std::string::npos) {
            status = Status::InvalidArgument;
            return;
        }
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd_ < 0) { status = Status::OpenFailed; error = errno; return; }
        struct stat info {};
        if (::fstat(fd_, &info) != 0) { status = Status::QueryFailed; error = errno; return; }
        if (!S_ISREG(info.st_mode) || info.st_size < static_cast<off_t>(sizeof(header))) {
            status = Status::InvalidFormat;
            return;
        }
        size_ = static_cast<uint64_t>(info.st_size);
        if (!SelectIntelSlice()) return;
        if (!Read(0, &header, sizeof(header))) return;
        if (isUniversal_ && (header.cputype != CPU_TYPE_X86_64 ||
                             header.cpusubtype != advertisedSubtype_)) {
            status = Status::InvalidFormat;
            return;
        }
        if (header.magic != MH_MAGIC_64) { status = Status::InvalidFormat; return; }
        // This first product path is verified for the native Intel candidate.
        // Other architectures/formats cannot count as an installable component.
        if (header.cputype != CPU_TYPE_X86_64) { status = Status::Unsupported; return; }
        if (header.filetype != MH_DYLIB && header.filetype != MH_BUNDLE &&
            header.filetype != MH_EXECUTE && header.filetype != MH_OBJECT) {
            status = Status::InvalidFormat;
            return;
        }
        if (header.ncmds > 16384 || !Within(sizeof(header), header.sizeofcmds)) {
            status = Status::InvalidFormat;
            return;
        }
        if (header.sizeofcmds > 16 * 1024 * 1024) { status = Status::Unsupported; return; }
        const uint64_t end = sizeof(header) + static_cast<uint64_t>(header.sizeofcmds);
        uint64_t offset = sizeof(header);
        for (uint32_t i = 0; i < header.ncmds; ++i) {
            load_command command {};
            if (offset > end || end - offset < sizeof(command) ||
                !Read(offset, &command, sizeof(command))) { status = Status::InvalidFormat; return; }
            if (command.cmdsize < sizeof(command) || command.cmdsize % 8 != 0 ||
                command.cmdsize > end - offset) { status = Status::InvalidFormat; return; }
            if (command.cmd == LC_SYMTAB) {
                if (hasSymbols || command.cmdsize < sizeof(symbols) ||
                    !Read(offset, &symbols, sizeof(symbols))) { status = Status::InvalidFormat; return; }
                hasSymbols = true;
                if (!Within(symbols.symoff, static_cast<uint64_t>(symbols.nsyms) * sizeof(nlist_64)) ||
                    !Within(symbols.stroff, symbols.strsize) ||
                    symbols.symoff < end || symbols.stroff < end) {
                    status = Status::InvalidFormat;
                    return;
                }
                if (symbols.nsyms > 1000000 || symbols.strsize > 64 * 1024 * 1024) {
                    status = Status::Unsupported;
                    return;
                }
            } else if (command.cmd == LC_SEGMENT_64) {
                segment_command_64 segment {};
                if (command.cmdsize < sizeof(segment) || !Read(offset, &segment, sizeof(segment)) ||
                    static_cast<uint64_t>(segment.nsects) * sizeof(section_64) >
                        command.cmdsize - sizeof(segment) || !Within(segment.fileoff, segment.filesize)) {
                    status = Status::InvalidFormat;
                    return;
                }
                sectionCount += segment.nsects;
                if (sectionCount > 4096) { status = Status::Unsupported; return; }
                for (uint32_t index = 0; index < segment.nsects; ++index) {
                    section_64 section {};
                    if (!Read(offset + sizeof(segment) + static_cast<uint64_t>(index) * sizeof(section),
                              &section, sizeof(section))) return;
                    if (std::strncmp(section.sectname, "__harken_meta", sizeof(section.sectname)) != 0)
                        continue;
                    if (hasMetadata || std::strncmp(section.segname, "__TEXT", sizeof(section.segname)) != 0 ||
                        std::strncmp(segment.segname, "__TEXT", sizeof(segment.segname)) != 0 ||
                        (section.flags & SECTION_TYPE) != S_REGULAR || section.size == 0 || section.size > 4096 ||
                        !Within(section.offset, section.size) || section.offset < segment.fileoff ||
                        section.offset - segment.fileoff > segment.filesize ||
                        section.size > segment.filesize - (section.offset - segment.fileoff)) {
                        status = Status::InvalidFormat;
                        return;
                    }
                    hasMetadata = true;
                    metadataOffset = section.offset;
                    metadataSize = section.size;
                }
            }
            offset += command.cmdsize;
        }
        if (offset != end) { status = Status::InvalidFormat; return; }
    }
    ~MachFile() { if (fd_ >= 0) ::close(fd_); }
    MachFile(const MachFile&) = delete;
    MachFile& operator=(const MachFile&) = delete;

    bool Read(uint64_t offset, void* data, size_t count) {
        if (!Within(offset, count)) { status = Status::InvalidFormat; return false; }
        auto* output = static_cast<char*>(data);
        while (count > 0) {
            const ssize_t read = ::pread(fd_, output, count, static_cast<off_t>(baseOffset_ + offset));
            if (read < 0 && errno == EINTR) continue;
            if (read < 0) { status = Status::QueryFailed; error = errno; return false; }
            if (read == 0) { status = Status::InvalidFormat; return false; }
            output += read; offset += read; count -= static_cast<size_t>(read);
        }
        return true;
    }

    Status status = Status::Success;
    unsigned long error = 0;
    mach_header_64 header {};
    symtab_command symbols {};
    bool hasSymbols = false;
    uint64_t sectionCount = 0;
    bool hasMetadata = false;
    uint64_t metadataOffset = 0;
    uint64_t metadataSize = 0;
private:
    bool SelectIntelSlice() {
        unsigned char prefix[8] {};
        if (!Read(0, prefix, sizeof(prefix))) return false;
        auto integer = [](const unsigned char* bytes, size_t width, bool little) {
            uint64_t value = 0;
            for (size_t i = 0; i < width; ++i)
                value = (value << 8) | bytes[little ? width - 1 - i : i];
            return value;
        };
        const auto magic = integer(prefix, 4, false);
        if (magic != FAT_MAGIC && magic != FAT_CIGAM &&
            magic != FAT_MAGIC_64 && magic != FAT_CIGAM_64) return true;
        const bool little = magic == FAT_CIGAM || magic == FAT_CIGAM_64;
        const bool wide = magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
        const uint64_t count = integer(prefix + 4, 4, little);
        const uint64_t stride = wide ? 32 : 20;
        auto invalid = [&]() { status = Status::InvalidFormat; return false; };
        if (count == 0 || count > 64 || !Within(8, count * stride)) return invalid();
        const uint64_t tableEnd = 8 + count * stride;
        struct Range { uint64_t start, size; };
        std::vector<Range> ranges;
        uint64_t selectedOffset = 0, selectedSize = 0;
        for (uint64_t index = 0; index < count; ++index) {
            unsigned char entry[32] {};
            if (!Read(8 + index * stride, entry, static_cast<size_t>(stride))) return false;
            const auto cpu = integer(entry, 4, little);
            const auto subtype = integer(entry + 4, 4, little);
            const auto offset = integer(entry + 8, wide ? 8 : 4, little);
            const auto size = integer(entry + (wide ? 16 : 12), wide ? 8 : 4, little);
            const auto alignment = integer(entry + (wide ? 24 : 16), 4, little);
            if (offset < tableEnd || size == 0 || !Within(offset, size) || alignment >= 64 ||
                (offset & ((uint64_t(1) << alignment) - 1)) != 0 ||
                (wide && integer(entry + 28, 4, little) != 0)) return invalid();
            for (const auto& range : ranges)
                if (offset < range.start + range.size && range.start < offset + size) return invalid();
            ranges.push_back({offset, size});
            if (cpu == CPU_TYPE_X86_64) {
                // Multiple Intel subtypes require an explicit selection policy, not guessing.
                if (selectedSize != 0 || size < sizeof(header)) return invalid();
                selectedOffset = offset;
                selectedSize = size;
                advertisedSubtype_ = static_cast<cpu_subtype_t>(subtype);
            }
        }
        if (selectedSize == 0) { status = Status::Unsupported; return false; }
        baseOffset_ = selectedOffset;
        size_ = selectedSize;
        isUniversal_ = true;
        return true;
    }
    bool Within(uint64_t offset, uint64_t count) const {
        return offset <= size_ && count <= size_ - offset;
    }
    int fd_ = -1;
    uint64_t size_ = 0;
    uint64_t baseOffset_ = 0;
    bool isUniversal_ = false;
    cpu_subtype_t advertisedSubtype_ = 0;
};

StringResult MetadataValue(const std::string& path, const std::string& key) {
    if (key.empty() || key.find('\0') != std::string::npos)
        return {Status::InvalidArgument, {}, 0};
    MachFile file(path);
    if (file.status != Status::Success) return {file.status, {}, file.error};
    if (!file.hasMetadata) return {Status::ValueUnavailable, {}, 0};
    std::vector<char> bytes(static_cast<size_t>(file.metadataSize));
    if (!file.Read(file.metadataOffset, bytes.data(), bytes.size())) return {file.status, {}, file.error};
    const auto terminator = std::find(bytes.begin(), bytes.end(), '\0');
    if (terminator == bytes.end() || std::any_of(terminator, bytes.end(), [](char c) { return c != 0; }))
        return {Status::InvalidFormat, {}, 0};
    const std::string_view text(bytes.data(), static_cast<size_t>(terminator - bytes.begin()));
    constexpr std::string_view prefix = "HARKEN-REAPER-METADATA-1\n";
    if (text.substr(0, prefix.size()) != prefix) return {Status::InvalidFormat, {}, 0};
    const std::array<std::string_view, 6> allowed = {"ProductName", "FileDescription", "FileVersion",
        "BridgeProtocolVersion", "BridgeArchitecture", "BridgeManifest"};
    std::map<std::string, std::string> fields;
    size_t start = prefix.size();
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string_view::npos) return {Status::InvalidFormat, {}, 0};
        const auto line = text.substr(start, end - start);
        const auto separator = line.find('=');
        if (separator == std::string_view::npos) return {Status::InvalidFormat, {}, 0};
        const auto name = line.substr(0, separator), value = line.substr(separator + 1);
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end() || value.empty() ||
            value.size() > 512 || std::any_of(value.begin(), value.end(), [](unsigned char c) {
                return c < 32 || c > 126;
            }) || !fields.emplace(std::string(name), std::string(value)).second)
            return {Status::InvalidFormat, {}, 0};
        start = end + 1;
    }
    if (fields.size() != allowed.size()) return {Status::InvalidFormat, {}, 0};
    const auto found = fields.find(key);
    return found == fields.end() ? StringResult{Status::ValueUnavailable, {}, 0}
                                : StringResult{Status::Success, found->second, 0};
}
}

StringResult ReadArchitecture(const std::string& pathUtf8) {
    try {
        MachFile file(pathUtf8);
        return {file.status, file.status == Status::Success ? "x64" : "", file.error};
    } catch (...) {
        return {Status::QueryFailed, {}, 0};
    }
}

StringResult ReadFileVersion(const std::string& pathUtf8) {
    return ReadVersionString(pathUtf8, "FileVersion");
}

StringResult ReadVersionString(const std::string& pathUtf8,
                               const std::string& keyUtf8) {
    try { return MetadataValue(pathUtf8, keyUtf8); }
    catch (...) { return {Status::QueryFailed, {}, 0}; }
}

ExportResult ProbeExport(const std::string& pathUtf8,
                         const std::string& exportName) {
    if (exportName.empty() || exportName.size() > 4096 || exportName.find('\0') != std::string::npos)
        return {Status::InvalidArgument, false, 0};
    try {
        MachFile file(pathUtf8);
        if (file.status != Status::Success) return {file.status, false, file.error};
        if (!file.hasSymbols) return {Status::ValueUnavailable, false, 0};
        std::vector<nlist_64> symbols(file.symbols.nsyms);
        std::vector<char> strings(file.symbols.strsize);
        if (!file.Read(file.symbols.symoff, symbols.data(), symbols.size() * sizeof(nlist_64)) ||
            !file.Read(file.symbols.stroff, strings.data(), strings.size()))
            return {file.status, false, file.error};
        const std::string name = "_" + exportName;
        for (const auto& symbol : symbols) {
            if ((symbol.n_type & N_STAB) || !(symbol.n_type & N_EXT) ||
                (symbol.n_type & N_PEXT) || (symbol.n_type & N_TYPE) != N_SECT) continue;
            const auto index = symbol.n_un.n_strx;
            if (symbol.n_sect == 0 || symbol.n_sect > file.sectionCount || index >= strings.size())
                return {Status::InvalidFormat, false, 0};
            if (!std::memchr(strings.data() + index, 0, strings.size() - index))
                return {Status::InvalidFormat, false, 0};
            if (name.size() < strings.size() - index && strings[index + name.size()] == 0 &&
                std::memcmp(strings.data() + index, name.data(), name.size()) == 0)
                return {Status::Success, true, 0};
        }
        return {Status::Success, false, 0};
    } catch (...) {
        return {Status::QueryFailed, false, 0};
    }
}

} // namespace Platform::PeInspection

#endif // __APPLE__
