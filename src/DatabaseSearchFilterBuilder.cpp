#include "DatabaseSearchFilterBuilder.h"

#include "AppState.h"
#include "AsyncWorker.h"
#include "DatabasePathNormalizationPolicy.h"
#include "SearchFilterFileTypeAdapter.h"
#include "SearchKeywordCompiler.h"

namespace aura::search {

SearchFilter BuildCurrentDbFilter(const AppState& state) {
    SearchFilter filter;
    filter.keyword = CompileSearchBufferKeywordForFilter(state.searchBuffer);
    filter.folderPath = state.folderFilterActive
        ? aura::database::NormalizeFolderPathForDb(state.selectedFolderPath)
        : "";
    if (!state.artistFilterAlternates.empty()) {
        filter.artists = state.artistFilterAlternates;
    } else {
        filter.artist = state.artistFilterBuffer;
    }
    filter.album = state.albumFilterBuffer;
    filter.category = state.categoryFilterBuffer;

    const char* formats[] = {"", "WAV", "MP3", "FLAC", "OGG", "AIFF", "M4A"};
    if (state.formatFilterIndex > 0 && state.formatFilterIndex < 7) {
        filter.format = formats[state.formatFilterIndex];
    }

    using ChannelCategory = SearchFilter::ChannelCategory;
    static constexpr ChannelCategory kChannelCategoryMap[] = {
        ChannelCategory::Any,
        ChannelCategory::Mono,
        ChannelCategory::Stereo,
        ChannelCategory::Quad,
        ChannelCategory::Surround_5_1,
        ChannelCategory::Surround_7_1,
        ChannelCategory::MultiOther,
    };
    const int channelIndex = state.channelFilterIndex;
    if (channelIndex >= 0 && channelIndex < 7) {
        filter.channelCategory = kChannelCategoryMap[channelIndex];
    }

    filter.minDuration = state.minDurationFilter;
    filter.maxDuration = state.maxDurationFilter;
    filter.minBpm = state.minBpmFilter;
    filter.maxBpm = state.maxBpmFilter;
    filter.musicalKey = state.keyFilterBuffer;

    const std::uint32_t sampleRates[] = {0, 44100, 48000, 96000, 192000};
    if (state.sampleRateFilterIndex > 0 && state.sampleRateFilterIndex < 5) {
        filter.minSampleRate = sampleRates[state.sampleRateFilterIndex];
        filter.maxSampleRate = sampleRates[state.sampleRateFilterIndex];
    }

    filter.coverFilter = state.coverFilterIndex;
    if (state.coverFilterActive) {
        filter.specificCoverKey = state.coverFilterPath;
    }
    filter.exactMode = state.searchExactMode;

    for (const auto& [tag, selection] : state.genrePillState) {
        if (selection == 1) filter.includeGenres.push_back(tag);
        else if (selection == 2) filter.excludeGenres.push_back(tag);
    }
    for (const auto& [tag, selection] : state.instrumentPillState) {
        if (selection == 1) filter.includeInstruments.push_back(tag);
        else if (selection == 2) filter.excludeInstruments.push_back(tag);
    }

    if (state.activeUserCollectionIdx >= 0 &&
        state.activeUserCollectionIdx < static_cast<int>(state.userCollections.size())) {
        const auto& collection = state.userCollections[state.activeUserCollectionIdx];
        for (const auto& rule : collection.rules) {
            if (rule.value[0] == '\0') continue;
            using Field = AppState::CRuleField;
            using Condition = AppState::CRuleCond;
            const auto condition = static_cast<Condition>(rule.condition);
            const auto field = static_cast<Field>(rule.field);
            switch (field) {
                case Field::Artist:
                    if (condition == Condition::Contains || condition == Condition::Equals) {
                        filter.artist = rule.value;
                    }
                    break;
                case Field::Genre:
                    if (condition == Condition::Contains || condition == Condition::Equals) {
                        filter.includeGenres.push_back(rule.value);
                    } else if (condition == Condition::NotContains) {
                        filter.excludeGenres.push_back(rule.value);
                    }
                    break;
                case Field::Instruments:
                    if (condition == Condition::Contains || condition == Condition::Equals) {
                        filter.includeInstruments.push_back(rule.value);
                    } else if (condition == Condition::NotContains) {
                        filter.excludeInstruments.push_back(rule.value);
                    }
                    break;
                case Field::Key:
                    if (filter.musicalKey.empty()) filter.musicalKey = rule.value;
                    break;
                case Field::Description:
                default:
                    if (!filter.keyword.empty()) filter.keyword += " ";
                    filter.keyword += rule.value;
                    break;
            }
        }
    }

    ApplyFileTypePolicyToFilter(state, filter);
    return filter;
}

}  // namespace aura::search
