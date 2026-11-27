#pragma once

#include <string_view>

namespace aris::update {

inline constexpr char kLegacyUpdateProduct[] = "aris";
inline constexpr char kUpdateProduct[] = "harken-search";
inline constexpr char kLegacyUpdateReleaseNotesFilename[] = "aris_release_notes.json";
inline constexpr char kUpdateReleaseNotesFilename[] = "harkensearch_release_notes.json";

inline constexpr bool IsSupportedUpdateProduct(std::string_view product) {
    return product == kUpdateProduct || product == kLegacyUpdateProduct;
}

}
