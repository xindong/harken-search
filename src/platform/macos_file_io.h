#pragma once

// Private POSIX helpers for the macOS data backends. Walk directories through
// handles so a symbolic link cannot redirect a write outside the chosen tree.
#include <filesystem>
#include <string>

namespace Platform::MacFileIO {
class Descriptor {
public:
    Descriptor() = default;
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor();
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept;
    Descriptor& operator=(Descriptor&& other) noexcept;
    int get() const { return value_; }
    explicit operator bool() const { return value_ >= 0; }
    bool close();
private:
    int value_ = -1;
};

bool ValidPath(const std::string& path);
Descriptor OpenDirectory(const std::filesystem::path& path, bool create = false);
Descriptor OpenParent(const std::string& path, std::string& name);
bool WritableDirectory(int fd);
bool RegularFile(int fd);
bool RegularOrMissing(int directory, const std::string& name);
bool FlushFile(int fd);
bool WriteAll(int fd, const char* bytes, size_t count);
}
