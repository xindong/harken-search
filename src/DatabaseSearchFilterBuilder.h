#pragma once

#include <array>
#include <cstddef>

struct AppState;
struct SearchFilter;

namespace aura::search {

inline constexpr std::array<const char*, 8> kAudioFormatFilters = {
    "", "WAV", "MP3", "FLAC", "OGG", "AIFF", "M4A", "WV"
};

constexpr const char* AudioFormatFilterAt(int index) {
    return index > 0 && static_cast<std::size_t>(index) < kAudioFormatFilters.size()
        ? kAudioFormatFilters[static_cast<std::size_t>(index)]
        : kAudioFormatFilters.front();
}

SearchFilter BuildCurrentDbFilter(const AppState& state);

}  // namespace aura::search
