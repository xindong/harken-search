#ifdef __APPLE__

#include "pe_inspection.h"

#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

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
        if (!Read(0, &header, sizeof(header))) return;
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
            const ssize_t read = ::pread(fd_, output, count, static_cast<off_t>(offset));
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
private:
    bool Within(uint64_t offset, uint64_t count) const {
        return offset <= size_ && count <= size_ - offset;
    }
    int fd_ = -1;
    uint64_t size_ = 0;
};
}

StringResult ReadArchitecture(const std::string& pathUtf8) {
    try {
        MachFile file(pathUtf8);
        return {file.status, file.status == Status::Success ? "x64" : "", file.error};
    } catch (...) {
        return {Status::QueryFailed, {}, 0};
    }
}

StringResult ReadFileVersion(const std::string& /*pathUtf8*/) {
    StringResult result;
    result.status = Status::Unsupported;
    return result;
}

StringResult ReadVersionString(const std::string& /*pathUtf8*/,
                               const std::string& /*keyUtf8*/) {
    StringResult result;
    result.status = Status::Unsupported;
    return result;
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
