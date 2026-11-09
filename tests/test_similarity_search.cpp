#include "SimilaritySearch.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) {
        std::cout << "PASS: " << message << '\n';
    } else {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

AudioFeatureVector MakeFeatures(float value) {
    AudioFeatureVector features;
    for (int i = 0; i < AudioFeatureVector::DIMS; ++i)
        features.features[i] = value + static_cast<float>(i) * 0.001f;
    return features;
}

SimilaritySearch::SimilarityFeatureRecord MakeRecord(int64_t fileId,
                                                      float value) {
    SimilaritySearch::SimilarityFeatureRecord record;
    record.fileId = fileId;
    record.filePath = "C:/similar/" + std::to_string(fileId) + ".wav";
    record.features = MakeFeatures(value);
    return record;
}

void TestTopKAndStableRankingContract() {
    SimilaritySearch search;
    search.updateCacheEntry(30, "C:/similar/30.wav", MakeFeatures(3.0f));
    search.updateCacheEntry(10, "C:/similar/10.wav", MakeFeatures(1.0f));
    search.updateCacheEntry(20, "C:/similar/20.wav", MakeFeatures(1.0f));
    search.updateCacheEntry(25, "C:/similar/25.wav", MakeFeatures(1.0f));
    search.recomputeStats();

    const auto query = MakeFeatures(1.0f);
    Check(search.findSimilar(query, 10, 0).empty(),
          "topK zero returns no similarity results");
    Check(search.findSimilar(query, 10, -4).empty(),
          "negative topK returns no similarity results");

    const auto results = search.findSimilar(query, 10, 3);
    Check(results.size() == 3,
          "topK three returns the three non-excluded candidates");
    if (results.size() == 3) {
        Check(results[0].fileId == 20 && results[1].fileId == 25 &&
                  results[2].fileId == 30,
              "equal distances use ascending fileId as a stable tie-break");
    }

    bool scoresAreFiniteAndBounded = true;
    for (const auto& result : results) {
        scoresAreFiniteAndBounded =
            scoresAreFiniteAndBounded && std::isfinite(result.distance) &&
            std::isfinite(result.similarity) && result.similarity >= 0.0f &&
            result.similarity <= 1.0f;
    }
    Check(scoresAreFiniteAndBounded,
          "distances are finite and similarities stay within zero and one");
}

void TestInvalidFeaturesAreRejected() {
    SimilaritySearch search;
    std::vector<SimilaritySearch::SimilarityFeatureRecord> entries;
    entries.push_back(MakeRecord(1, 1.0f));
    auto invalid = MakeRecord(2, 2.0f);
    invalid.features.features[7] = std::numeric_limits<float>::infinity();
    entries.push_back(std::move(invalid));
    entries.push_back(MakeRecord(3, 3.0f));

    search.replaceFeatureCache(std::move(entries));
    Check(search.isCacheLoaded() && search.cacheSize() == 2,
          "cache replacement marks loaded and filters non-finite vectors");

    // Preserve valid stats, then exercise incremental cache mutation so two
    // finite vectors whose squared distances overflow are retained in cache.
    // With three candidates and topK two, failing to skip invalid distances
    // would leak one infinite-distance result into the output.
    search.removeCacheEntry(1);
    search.removeCacheEntry(3);

    AudioFeatureVector overflowingCandidate;
    for (float& value : overflowingCandidate.features)
        value = std::numeric_limits<float>::max();
    search.updateCacheEntry(4, "C:/similar/4.wav", overflowingCandidate);
    search.updateCacheEntry(5, "C:/similar/5.wav", overflowingCandidate);
    search.updateCacheEntry(1, "C:/similar/1.wav", MakeFeatures(1.0f));

    Check(search.cacheSize() == 3,
          "overflowing finite candidates are retained for search validation");
    const auto validResults = search.findSimilar(MakeFeatures(1.0f), -1, 2);
    Check(validResults.size() == 1 && validResults.front().fileId == 1,
          "invalid distances are skipped when candidate count exceeds topK");
    if (!validResults.empty()) {
        Check(std::isfinite(validResults.front().distance) &&
                  std::isfinite(validResults.front().similarity),
              "retained result remains finite after invalid candidates");
    }

    auto invalidQuery = MakeFeatures(1.0f);
    invalidQuery.features[11] = std::numeric_limits<float>::quiet_NaN();
    Check(search.findSimilar(invalidQuery, -1, 2).empty(),
          "non-finite query vectors return no results");
}

void TestPathIdentityExcludesReferenceAliasesAndDeduplicatesTopK() {
    SimilaritySearch search;
    std::vector<SimilaritySearch::SimilarityFeatureRecord> entries;

    auto reference = MakeRecord(10, 0.0f);
#ifdef _WIN32
    reference.filePath = "C:/Library/Impact.wav";
#else
    reference.filePath = "/Library/Impact.wav";
#endif
    entries.push_back(std::move(reference));
    auto referenceAlias = MakeRecord(11, 0.0f);
#ifdef _WIN32
    referenceAlias.filePath = "\\\\?\\c:\\library\\impact.wav";
#else
    referenceAlias.filePath = "/Library/temp/../Impact.wav";
#endif
    entries.push_back(std::move(referenceAlias));

    auto door = MakeRecord(20, 0.1f);
#ifdef _WIN32
    door.filePath = "C:/SFX/Door.wav";
#else
    door.filePath = "/SFX/Door.wav";
#endif
    entries.push_back(std::move(door));
    auto doorAlias = MakeRecord(21, 0.1f);
#ifdef _WIN32
    doorAlias.filePath = "c:\\sfx\\temp\\..\\door.wav";
#else
    doorAlias.filePath = "/SFX/temp/../Door.wav";
#endif
    entries.push_back(std::move(doorAlias));

    auto otherDirectorySameName = MakeRecord(30, 0.2f);
#ifdef _WIN32
    otherDirectorySameName.filePath = "D:/Other/Door.wav";
#else
    otherDirectorySameName.filePath = "/Other/Door.wav";
#endif
    entries.push_back(std::move(otherDirectorySameName));
    auto ambience = MakeRecord(40, 0.3f);
#ifdef _WIN32
    ambience.filePath = "E:/Ambience/Wind.wav";
#else
    ambience.filePath = "/Ambience/Wind.wav";
#endif
    entries.push_back(std::move(ambience));

    search.replaceFeatureCache(std::move(entries));
    // Exclude through the reference alias ID. The public ID-only overload must
    // still exclude the complete reference path identity and its aliases.
    const auto results = search.findSimilar(MakeFeatures(0.0f), 11, 3);

    Check(results.size() == 3,
          "path aliases do not consume unique top-K result slots");
    if (results.size() == 3) {
        Check(results[0].fileId == 20 && results[1].fileId == 30 &&
                  results[2].fileId == 40,
              "reference aliases are excluded, candidate aliases collapse, "
              "and different-directory names remain distinct");
    }
}

#ifdef _WIN32
void TestUnicodeCaseAliasesFollowWindowsPathIdentity() {
    SimilaritySearch search;
    std::vector<SimilaritySearch::SimilarityFeatureRecord> entries;

    auto reference = MakeRecord(50, 0.0f);
    reference.filePath = u8"C:/音效/Élan.wav";
    entries.push_back(std::move(reference));
    auto referenceAlias = MakeRecord(51, 0.0f);
    referenceAlias.filePath = u8"c:\\音效\\élan.wav";
    entries.push_back(std::move(referenceAlias));
    auto unique = MakeRecord(60, 0.2f);
    unique.filePath = u8"D:/音效/Unique.wav";
    entries.push_back(std::move(unique));

    search.replaceFeatureCache(std::move(entries));
    const auto results = search.findSimilar(MakeFeatures(0.0f), 51, 1);
    Check(results.size() == 1 && results.front().fileId == 60,
          "Windows Unicode case aliases are excluded as one path identity");
}
#endif

void TestPathAliasesDoNotSkewNormalizationStatistics() {
    const auto reference = MakeRecord(70, 0.0f);
    const auto near = MakeRecord(80, 0.1f);
    const auto far = MakeRecord(90, 0.2f);

    SimilaritySearch uniquePaths;
    uniquePaths.replaceFeatureCache({reference, near, far});
    const auto uniqueResults =
        uniquePaths.findSimilar(MakeFeatures(0.0f), reference.fileId, 2);

    auto alias = MakeRecord(71, 100.0f);
    alias.filePath = reference.filePath;
    SimilaritySearch aliasedPaths;
    aliasedPaths.replaceFeatureCache({reference, alias, near, far});
    const auto aliasedResults =
        aliasedPaths.findSimilar(MakeFeatures(0.0f), reference.fileId, 2);

    bool scoresMatch = uniqueResults.size() == aliasedResults.size();
    if (scoresMatch) {
        for (size_t index = 0; index < uniqueResults.size(); ++index) {
            scoresMatch = scoresMatch &&
                uniqueResults[index].fileId == aliasedResults[index].fileId &&
                std::abs(uniqueResults[index].similarity -
                         aliasedResults[index].similarity) < 1e-5f;
        }
    }
    Check(scoresMatch,
          "path aliases do not skew normalization statistics when features differ");
}

} // namespace

int main() {
    TestTopKAndStableRankingContract();
    TestInvalidFeaturesAreRejected();
    TestPathIdentityExcludesReferenceAliasesAndDeduplicatesTopK();
    TestPathAliasesDoNotSkewNormalizationStatistics();
#ifdef _WIN32
    TestUnicodeCaseAliasesFollowWindowsPathIdentity();
#endif
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
