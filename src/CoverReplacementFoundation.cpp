#include "CoverReplacementFoundation.h"

#include "Database.h"

namespace aura::cover_replacement {
namespace {

constexpr int kMinFacetCountToPersist = 500;

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool StartsWithAsciiCaseInsensitive(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        char lhs = value[i];
        char rhs = prefix[i];
        if (lhs >= 'A' && lhs <= 'Z') {
            lhs = static_cast<char>(lhs - 'A' + 'a');
        }
        if (rhs >= 'A' && rhs <= 'Z') {
            rhs = static_cast<char>(rhs - 'A' + 'a');
        }
        if (lhs != rhs) {
            return false;
        }
    }
    return true;
}

std::string StripLongPathPrefix(std::string_view rawPath) {
    if (StartsWithAsciiCaseInsensitive(rawPath, R"(\\?\UNC\)")) {
        return std::string(R"(\\)") + std::string(rawPath.substr(8));
    }
    if (StartsWithAsciiCaseInsensitive(rawPath, R"(\\?\)")) {
        return std::string(rawPath.substr(4));
    }
    return std::string(rawPath);
}

std::string NormalizePathLikeSegment(std::string_view rawPath) {
    if (rawPath.empty()) {
        return std::string();
    }

    std::string normalized = StripLongPathPrefix(rawPath);
    if (normalized.empty()) {
        return std::string();
    }

    for (char& ch : normalized) {
        if (ch == '/') {
            ch = '\\';
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }

    while (normalized.size() > 3 &&
           (normalized.back() == '\\' || normalized.back() == '/')) {
        normalized.pop_back();
    }

    return normalized;
}

} // namespace

bool IsPreliminaryCoverIdentity(std::string_view identityKey) {
    return StartsWith(identityKey, "?|") && identityKey.size() > 2;
}

bool IsLegacyCoverIdentityKey(std::string_view identityKey) {
    return StartsWith(identityKey, "F|") &&
           !StartsWith(identityKey, "F|H|") &&
           !StartsWith(identityKey, "F|PATH|");
}

CoverIdentityState ClassifyCoverIdentityState(std::string_view identityKey) {
    if (IsPreliminaryCoverIdentity(identityKey)) {
        return CoverIdentityState::Preliminary;
    }
    const std::string normalized = NormalizeIdentityKeyForComparison(identityKey);
    if (normalized.empty()) {
        return CoverIdentityState::Unknown;
    }
    if (StartsWith(normalized, "F|PATH|") &&
        IsLegacyCoverIdentityKey(identityKey)) {
        return CoverIdentityState::LegacyKey;
    }
    if (StartsWith(normalized, "E|") ||
        StartsWith(normalized, "F|H|") ||
        StartsWith(normalized, "F|PATH|")) {
        return CoverIdentityState::Canonical;
    }
    return CoverIdentityState::Unknown;
}

std::string NormalizeIdentityKeyForComparison(std::string_view identityKey) {
    if (identityKey.empty()) {
        return std::string();
    }

    if (IsPreliminaryCoverIdentity(identityKey)) {
        return std::string(identityKey);
    }

    if (StartsWith(identityKey, "E|")) {
        return std::string(identityKey);
    }

    if (StartsWith(identityKey, "F|H|")) {
        std::string_view rest = identityKey.substr(4);
        if (rest.empty()) {
            return std::string();
        }
        const size_t pipePos = rest.find('|');
        if (pipePos == 0 || pipePos == std::string_view::npos) {
            return std::string();
        }
        return std::string(identityKey);
    }

    if (StartsWith(identityKey, "F|PATH|")) {
        std::string_view rest = identityKey.substr(7);
        const size_t suffixPos = rest.find('|');
        const std::string_view pathPart =
            suffixPos == std::string_view::npos ? rest : rest.substr(0, suffixPos);
        const std::string_view suffix =
            suffixPos == std::string_view::npos ? std::string_view{} : rest.substr(suffixPos);
        const std::string normalizedPath = NormalizePathLikeSegment(pathPart);
        if (normalizedPath.empty()) {
            return std::string();
        }
        return std::string("F|PATH|") + normalizedPath + std::string(suffix);
    }

    if (IsLegacyCoverIdentityKey(identityKey)) {
        const std::string normalizedPath =
            NormalizePathLikeSegment(identityKey.substr(2));
        if (normalizedPath.empty()) {
            return std::string();
        }
        return std::string("F|PATH|") + normalizedPath;
    }

    return std::string();
}

bool HasPersistentFacetFilterSemantics(const SearchFilter& filter) {
    return !filter.keyword.empty() ||
           !filter.category.empty() ||
           !filter.folderPath.empty() ||
           filter.minDuration > 0.0f ||
           filter.maxDuration > 0.0f ||
           filter.minSampleRate > 0 ||
           filter.maxSampleRate > 0 ||
           filter.minBpm > 0.0f ||
           filter.maxBpm > 0.0f ||
           !filter.artist.empty() ||
           !filter.artists.empty() ||
           !filter.album.empty() ||
           !filter.format.empty() ||
           filter.channels > 0 ||
           filter.minChannels > 0 ||
           filter.maxChannels > 0 ||
           filter.channelCategory != SearchFilter::ChannelCategory::Any ||
           filter.coverFilter != 0 ||
           !filter.specificCoverKey.empty() ||
           !filter.musicalKey.empty() ||
           filter.unplayedOnly ||
           filter.playedOnlyAny ||
           filter.highlyRatedOnly ||
           filter.addedAfter > 0 ||
           filter.playedAfter > 0 ||
           !filter.excludeExtensions.empty() ||
           !filter.includeGenres.empty() ||
           !filter.excludeGenres.empty() ||
           !filter.includeInstruments.empty() ||
           !filter.excludeInstruments.empty() ||
           filter.synonyms != nullptr ||
           filter.includeOnlyKnownExts ||
           filter.losslessWavPackOnly ||
           filter.exactMode;
}

bool ShouldPersistFacetEntriesForFilter(const SearchFilter& filter, int entryCount) {
    if (entryCount <= 0) {
        return false;
    }
    if (HasPersistentFacetFilterSemantics(filter)) {
        return true;
    }
    return entryCount >= kMinFacetCountToPersist;
}

bool IsFilteredRefreshCountStateImpossible(int loadedCount,
                                           int64_t totalCount,
                                           bool hasActiveDbFilter) {
    return loadedCount > 0 &&
           totalCount == 0 &&
           hasActiveDbFilter;
}

bool IsFilteredFacetWarmStartCountStateImpossible(int64_t totalCount,
                                                  bool hasActiveDbFilter,
                                                  bool facetsComputed,
                                                  bool hasAnyFacetPayload) {
    return totalCount == 0 &&
           hasActiveDbFilter &&
           facetsComputed &&
           hasAnyFacetPayload;
}

}
