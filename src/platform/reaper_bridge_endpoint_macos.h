#pragma once

#ifdef __APPLE__

#include <string>
#include <string_view>

namespace HarkenSearch::ReaperBridge {

inline constexpr char kMacQaSocketVariable[] = "HARKEN_REAPER_BRIDGE_TEST_SOCKET";
inline constexpr char kMacDefaultSocketName[] = "harken-reaper.sock";

std::string MacPrivateSocketPath(std::string_view rawPath);
std::string MacSocketPathInPrivateDirectory(std::string_view rawDirectory);
std::string MacDefaultSocketPath();
std::string MacSelectedSocketPath();

} // namespace HarkenSearch::ReaperBridge

#endif // __APPLE__
