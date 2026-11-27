#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Platform::FileSystem::ProtectedConfig {

enum class State { Missing, Safe, Unverifiable };

struct Access {
    std::uintmax_t mode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t group = 0;
    std::string acl;
    std::string label;
};

State Inspect(const std::filesystem::path& path, Access& access, std::string* bytes = nullptr);
bool SameAccess(const Access& source, const Access& copy);
bool WritePending(const std::filesystem::path& path, std::string_view content,
                  const Access* original, bool& created);
bool Publish(const std::filesystem::path& pending, const std::filesystem::path& target, bool replace);

}
