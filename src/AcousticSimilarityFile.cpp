#include "AcousticSimilarity.h"
#include "platform/audio_file.h"
#include <algorithm>
#include <limits>
namespace HarkenSearch {
AcousticAnalysisResult AnalyzeSimilarityFile(const std::string& path, const FeatureExtractionCancelCheck& cancel) {
    using namespace Platform::AudioFile;
    auto failure = [](FeatureExtractionError error) { AcousticAnalysisResult result; result.error = error; return result; };
    auto cancelled = [&] { return cancel && cancel(); };
    if (cancelled()) return failure(FeatureExtractionError::Cancelled);
    MonoFloatReader reader;
    if (reader.Open(path, LegacyWindowsPathPolicy::FeatureLegacyUtf8VerbatimPrefix, MonoSampleRate::Hz22050) != Status::Ok)
        return failure(FeatureExtractionError::DecodeFailed);
    std::vector<float> block(8192); size_t count = 0;
    for (;;) {
        if (cancelled()) return failure(FeatureExtractionError::Cancelled);
        const auto read = reader.Read(block.data(), block.size());
        if (read.status != Status::Ok && read.status != Status::EndOfFile)
            return failure(read.status == Status::Cancelled ? FeatureExtractionError::Cancelled : FeatureExtractionError::DecodeFailed);
        if (read.framesRead > block.size() || read.framesRead > std::numeric_limits<size_t>::max() - count)
            return failure(FeatureExtractionError::DecodeFailed);
        count += static_cast<size_t>(read.framesRead);
        if (read.status == Status::EndOfFile || read.framesRead == 0) break;
    }
    size_t position = count, cacheAt = 0; std::vector<float> cache;
    const auto readAt = [&](size_t at, size_t length, float* destination) {
        if (cancelled()) return false;
        size_t overlap = 0;
        if (at >= cacheAt && at < cacheAt + cache.size()) {
            overlap = std::min(length, cacheAt + cache.size() - at);
            std::copy_n(cache.data() + at - cacheAt, overlap, destination);
        }
        if (overlap < length) {
            const size_t next = at + overlap;
            if (position != next && reader.Seek(next) != Status::Ok) return false;
            position = next;
            size_t done = overlap;
            while (done < length) {
                if (cancelled()) return false;
                const auto read = reader.Read(destination + done, length - done);
                if ((read.status != Status::Ok && read.status != Status::EndOfFile) ||
                    read.framesRead == 0 || read.framesRead > length - done) return false;
                done += static_cast<size_t>(read.framesRead); position += static_cast<size_t>(read.framesRead);
            }
        }
        cache.assign(destination, destination + length); cacheAt = at;
        return true;
    };
    auto result = AnalyzeSimilarityStream(count, readAt, cancel);
    if (cancelled()) return failure(FeatureExtractionError::Cancelled);
    return result;
}
}
