#pragma once

#include "Localization.h"
#include "TransferEngine.h"

#include <cstddef>

namespace aura::ui {

inline const char* TransferOptionLabel(const std::pair<FolderNamingStrategy, std::string>& option) {
    switch (option.first) {
        case FolderNamingStrategy::SingleFolder: return L(TransferOption_SingleFolder);
        case FolderNamingStrategy::ByCategory: return L(TransferOption_ByCategory);
        case FolderNamingStrategy::ByCategorySubCategory: return L(TransferOption_ByCategorySubCategory);
        case FolderNamingStrategy::ByCategoryOptionalSubCategory: return L(TransferOption_ByCategoryOptionalSubCategory);
        case FolderNamingStrategy::ByCategorySubCategoryCDTitle: return L(TransferOption_ByCategorySubCategoryCDTitle);
        case FolderNamingStrategy::ByLibraryCategory: return L(TransferOption_ByLibraryCategory);
        case FolderNamingStrategy::ByLibraryCategorySubCategory: return L(TransferOption_ByLibraryCategorySubCategory);
        case FolderNamingStrategy::ByLibraryVolume: return L(TransferOption_ByLibraryVolume);
        case FolderNamingStrategy::ByLibraryVolumeCDTitle: return L(TransferOption_ByLibraryVolumeCDTitle);
        case FolderNamingStrategy::ByLibraryCDTitle: return L(TransferOption_ByLibraryCDTitle);
        case FolderNamingStrategy::ByShowCategory: return L(TransferOption_ByShowCategory);
        case FolderNamingStrategy::ByShowCategorySubCategory: return L(TransferOption_ByShowCategorySubCategory);
        case FolderNamingStrategy::ByShowCategoryOptionalSubCategory: return L(TransferOption_ByShowCategoryOptionalSubCategory);
        case FolderNamingStrategy::ByDesignerCategorySubCategory: return L(TransferOption_ByDesignerCategorySubCategory);
        case FolderNamingStrategy::ByComposer: return L(TransferOption_ByComposer);
        case FolderNamingStrategy::ByChannelLayout: return L(TransferOption_ByChannelLayout);
        case FolderNamingStrategy::MirrorSource: return L(TransferOption_MirrorSource);
        default: return option.second.c_str();
    }
}

inline const char* TransferOptionLabel(const std::pair<std::string, TransferFileType>& option) {
    switch (option.second) {
        case TransferFileType::WAV: return L(TransferOption_WAV);
        case TransferFileType::AIFF: return L(TransferOption_AIFF);
        case TransferFileType::FLAC: return L(TransferOption_FLAC);
        case TransferFileType::MP3: return L(TransferOption_MP3);
        case TransferFileType::SourceFormat: return L(TransferOption_SourceFormat);
        default: return option.first.c_str();
    }
}

inline const char* TransferOptionLabel(const std::pair<std::string, ExistingFileBehavior>& option) {
    switch (option.second) {
        case ExistingFileBehavior::AlwaysCreateNew: return L(TransferOption_AlwaysCreateNew);
        case ExistingFileBehavior::SkipExisting: return L(TransferOption_SkipExisting);
        case ExistingFileBehavior::UpdateMetadata: return L(TransferOption_UpdateMetadata);
        case ExistingFileBehavior::ReplaceExisting: return L(TransferOption_ReplaceExisting);
        default: return option.first.c_str();
    }
}

template <typename Option>
const char* TransferOptionPreview(const std::vector<Option>& options, int selected, const char* fallback) {
    if (selected < 0 || static_cast<std::size_t>(selected) >= options.size()) return fallback;
    return TransferOptionLabel(options[selected]);
}

}
