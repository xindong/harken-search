#include "AudioEmbeddingService.h"
#include "platform/audio_file.h"
#include "platform/filesystem.h"
#include "platform/file_hash.h"
#include "platform/text_encoding.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#ifdef HARKEN_WITH_ONNX
#include <onnxruntime_cxx_api.h>
#endif
namespace HarkenSearch {
namespace {
constexpr size_t kSamples = 480000, kFft = 1024, kBands = 64, kFrames = 1001;
bool Cancelled(const FeatureExtractionCancelCheck& cancel) { return cancel && cancel(); }
bool Normalize(AudioEmbedding& value) {
    double norm = 0;
    for (float v : value) { if (!std::isfinite(v)) return false; norm += double(v) * v; }
    if (norm < 1e-16) return false;
    const float scale = float(1 / std::sqrt(norm));
    for (float& v : value) v *= scale;
    return true;
}
void Fft(std::array<std::complex<double>, kFft>& data) {
    for (size_t i = 1, j = 0; i < kFft; ++i) {
        size_t bit = kFft >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit; if (i < j) std::swap(data[i], data[j]);
    }
    for (size_t width = 2; width <= kFft; width *= 2) {
        const auto step = std::polar(1.0, -6.2831853071795864769 / width);
        for (size_t at = 0; at < kFft; at += width) {
            std::complex<double> phase(1, 0);
            for (size_t j = 0; j < width / 2; ++j) {
                const auto a = data[at + j], b = data[at + j + width / 2] * phase;
                data[at + j] = a + b; data[at + j + width / 2] = a - b; phase *= step;
            }
        }
    }
}
bool Pinned(const std::string& path, const char* expected) {
    auto hash = Platform::Crypto::ComputeSha256File(path);
    std::transform(hash.hexDigest.begin(), hash.hexDigest.end(), hash.hexDigest.begin(),
        [](unsigned char c) { return c >= 'A' && c <= 'F' ? char(c + 'a' - 'A') : char(c); });
    return hash.status == Platform::Crypto::FileHashStatus::Success && hash.hexDigest == expected;
}
}
struct AudioEmbeddingService::Impl {
    std::vector<double> filters, window;
#ifdef HARKEN_WITH_ONNX
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "HarkenSimilarity"};
    std::unique_ptr<Ort::Session> session;
#endif
    bool ready = false;
};
AudioEmbeddingService::AudioEmbeddingService() : impl_(std::make_unique<Impl>()) {}
AudioEmbeddingService::~AudioEmbeddingService() = default;
bool AudioEmbeddingService::Load(const std::string& directory, std::string& error) {
    impl_ = std::make_unique<Impl>(); error.clear();
#ifndef HARKEN_WITH_ONNX
    error = "model_runtime_unavailable"; return false;
#else
    const std::string model = directory + "/clap-audio.onnx";
    if (!Pinned(model, "50d03fb781f7bf520a5d424a3479871cfca4af7a31026892ac1e935d5303f677") ||
        !Pinned(directory + "/clap-mel-filters.f64", "256b8c586fb0f0ea44c6b2cafa44caa9bf288c1cfe1a05ff2527b73ae123d37a") ||
        !Pinned(directory + "/clap-window.f64", "0d9c0e8dfb7e477785eaaac6e2082e19a030bc113178a207cb4d1ab8d536a7a9")) {
        error = "model_missing_or_version_mismatch"; return false;
    }
    const auto filters = Platform::FileSystem::ReadFileBytes(directory + "/clap-mel-filters.f64", 513 * 64 * sizeof(double));
    const auto window = Platform::FileSystem::ReadFileBytes(directory + "/clap-window.f64", kFft * sizeof(double));
    if (filters.size() != 513 * 64 * sizeof(double) || window.size() != kFft * sizeof(double)) {
        error = "invalid_preprocessor"; return false;
    }
    impl_->filters.resize(513 * 64); impl_->window.resize(kFft);
    std::memcpy(impl_->filters.data(), filters.data(), filters.size());
    std::memcpy(impl_->window.data(), window.data(), window.size());
    try {
        Ort::SessionOptions options; options.SetIntraOpNumThreads(4); options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        const auto nativePath = Platform::TextEncoding::Utf8ToWideStrict(model);
#else
        const auto& nativePath = model;
#endif
        impl_->session = std::make_unique<Ort::Session>(impl_->environment, nativePath.c_str(), options);
        const auto shape = impl_->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        const auto outputShape = impl_->session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (shape != std::vector<int64_t>{1,1,1001,64} || outputShape != std::vector<int64_t>{1,512}) {
            impl_->session.reset(); error = "model_shape_mismatch"; return false;
        }
        impl_->ready = true; return true;
    } catch (const Ort::Exception& ex) { error = ex.what(); impl_->session.reset(); return false; }
#endif
}
bool AudioEmbeddingService::IsReady() const { return impl_->ready; }
bool AudioEmbeddingService::MakeMel(const float* pcm, size_t samples, std::vector<float>& mel,
                                    std::string& error, const FeatureExtractionCancelCheck& cancel) const {
    mel.clear(); error.clear();
    if (Cancelled(cancel)) { error = "cancelled"; return false; }
    if (!IsReady()) { error = "model_not_ready"; return false; }
    if (!pcm || samples == 0 || samples > kSamples) { error = "invalid_audio_window"; return false; }
    for (size_t i = 0; i < samples; ++i) if (!std::isfinite(pcm[i])) { error = "nonfinite_audio"; return false; }
    // Official repeatpad: whole repeats followed by zeros, not a partial extra repeat.
    std::vector<float> padded(kSamples, 0);
    const size_t repeats = kSamples / samples;
    for (size_t i = 0; i < repeats; ++i) std::copy_n(pcm, samples, padded.data() + i * samples);
    std::vector<float> output(kFrames * kBands);
    std::array<std::complex<double>, kFft> frame{};
    std::array<double, 513> power{};
    for (size_t t = 0; t < kFrames; ++t) {
        if (Cancelled(cancel)) { error = "cancelled"; return false; }
        for (size_t i = 0; i < kFft; ++i) {
            int64_t at = int64_t(t * 480 + i) - 512;
            if (at < 0) at = -at;
            if (at >= int64_t(kSamples)) at = 2 * int64_t(kSamples) - 2 - at;
            frame[i] = double(padded[size_t(at)]) * impl_->window[i];
        }
        Fft(frame);
        for (size_t i = 0; i < power.size(); ++i) {
            // The reference stores FFT output as complex64 before computing power.
            const double real = float(frame[i].real()), imag = float(frame[i].imag());
            power[i] = real * real + imag * imag;
        }
        for (size_t band = 0; band < kBands; ++band) {
            double sum = 0;
            for (size_t f = 0; f < power.size(); ++f) sum += power[f] * impl_->filters[f * kBands + band];
            output[t * kBands + band] = float(10 * std::log10(std::max(1e-10, sum)));
        }
    }
    mel.swap(output); return true;
}
bool AudioEmbeddingService::EncodeWindow(const float* pcm, size_t samples, AudioEmbedding& result,
                                         std::string& error, const FeatureExtractionCancelCheck& cancel) {
    result = {}; std::vector<float> mel;
    if (!MakeMel(pcm, samples, mel, error, cancel)) return false;
#ifdef HARKEN_WITH_ONNX
    try {
        const std::array<int64_t, 4> shape{1,1,1001,64};
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto input = Ort::Value::CreateTensor<float>(memory, mel.data(), mel.size(), shape.data(), shape.size());
        const char* inputs[] = {"mel"}; const char* outputs[] = {"embedding"};
        if (Cancelled(cancel)) { error = "cancelled"; return false; }
        auto output = impl_->session->Run(Ort::RunOptions{nullptr}, inputs, &input, 1, outputs, 1);
        if (Cancelled(cancel)) { error = "cancelled"; return false; }
        if (output.size() != 1 || !output[0].IsTensor() || output[0].GetTensorTypeAndShapeInfo().GetElementCount() != 512) {
            error = "invalid_model_output"; return false;
        }
        AudioEmbedding value{}; std::copy_n(output[0].GetTensorData<float>(), 512, value.begin());
        if (!Normalize(value)) { error = "invalid_model_output"; return false; }
        result = value; return true;
    } catch (const Ort::Exception& ex) { error = ex.what(); return false; }
#else
    return false;
#endif
}
AudioEmbeddingResult AudioEmbeddingService::EncodeFile(const std::string& path, float activeStart, float activeSeconds,
                                                       const FeatureExtractionCancelCheck& cancel) {
    AudioEmbeddingResult output;
    auto fail = [&](const char* message) { AudioEmbeddingResult result; result.error = message; return result; };
    if (!std::isfinite(activeStart) || !std::isfinite(activeSeconds) || activeStart < 0 || activeStart > 604800 || activeSeconds <= 0 || activeSeconds > 20480)
        return fail("invalid_or_oversized_audio");
    if (Cancelled(cancel)) return fail("cancelled");
    using namespace Platform::AudioFile;
    MonoFloatReader reader;
    auto open = [&] { return reader.Open(path, LegacyWindowsPathPolicy::FeatureLegacyUtf8VerbatimPrefix, MonoSampleRate::Hz48000) == Status::Ok; };
    if (!open()) return fail("audio_open_failed");
    const uint64_t start = uint64_t(std::llround(double(activeStart) * 48000));
    const uint64_t requestedEnd = start + uint64_t(std::llround(double(activeSeconds) * 48000));
    std::vector<float> pcm(kSamples), normalized(kSamples); float peak = 0;
    uint64_t sourceFrames = 0;
    // Measure the actual 48 kHz stream. Its final sample can differ slightly
    // from a time converted from the separate 22.05 kHz acoustic grid.
    for (;;) {
        if (Cancelled(cancel)) return fail("cancelled");
        const auto read = reader.Read(pcm.data(), pcm.size());
        if ((read.status != Status::Ok && read.status != Status::EndOfFile) || read.framesRead > pcm.size()) return fail("audio_read_failed");
        for (size_t i = 0; i < size_t(read.framesRead); ++i) {
            if (!std::isfinite(pcm[i])) return fail("nonfinite_audio");
            if (sourceFrames + i >= start && sourceFrames + i < requestedEnd) peak = std::max(peak,std::fabs(pcm[i]));
        }
        sourceFrames += read.framesRead;
        if (read.status == Status::EndOfFile || !read.framesRead) break;
    }
    if (start >= sourceFrames || requestedEnd <= start || requestedEnd > sourceFrames + 96) return fail("audio_bounds_mismatch");
    const uint64_t length = std::min(sourceFrames,requestedEnd) - start;
    if (peak < 1e-12f) return fail("silent_audio");
    if (!open()) return fail("audio_open_failed");
    auto readExact = [&](float* destination, size_t count) {
        size_t done = 0;
        while (done < count) {
            if (Cancelled(cancel)) return false;
            const auto read = reader.Read(destination + done, count - done);
            if ((read.status != Status::Ok && read.status != Status::EndOfFile) || !read.framesRead || read.framesRead > count - done) return false;
            done += size_t(read.framesRead);
        }
        return true;
    };
    // Replay sequentially, keeping overlap in a bounded rolling window. A seek
    // cannot reset resampler history or invent samples at the file boundary.
    for (uint64_t skipped = 0; skipped < start;) {
        const size_t count = size_t(std::min<uint64_t>(kSamples,start-skipped));
        if (!readExact(pcm.data(),count)) return fail(Cancelled(cancel)?"cancelled":"audio_read_failed");
        skipped += count;
    }
    size_t retained = 0; uint64_t covered = 0;
    for (uint64_t at = 0; covered < length; at += kSamples / 2) {
        const size_t count = size_t(std::min<uint64_t>(kSamples, length - at));
        if (!readExact(pcm.data()+retained,count-retained)) return fail(Cancelled(cancel)?"cancelled":"audio_read_failed");
        for (size_t i = 0; i < count; ++i) normalized[i] = pcm[i] * (0.95f / peak);
        AudioEmbeddingSegment segment;
        segment.startSeconds = float(double(start + at) / 48000);
        segment.endSeconds = float(double(start + at + count) / 48000);
        if (!EncodeWindow(normalized.data(), count, segment.embedding, output.error, cancel)) return output;
        const float weight = float(at + count - covered) / float(length);
        for (size_t i = 0; i < 512; ++i) output.pooled[i] += weight * segment.embedding[i];
        covered = at + count; output.segments.push_back(segment);
        if (covered < length) {
            retained = count - kSamples / 2;
            std::copy(pcm.begin()+kSamples/2,pcm.begin()+count,pcm.begin());
        }
    }
    if (!Normalize(output.pooled)) return fail("invalid_model_output");
    output.ok = true; return output;
}
float AudioEmbeddingCosine(const AudioEmbedding& a, const AudioEmbedding& b) {
    double dot = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return 0;
        dot += double(a[i]) * b[i]; aa += double(a[i]) * a[i]; bb += double(b[i]) * b[i];
    }
    return aa > 1e-16 && bb > 1e-16 ? float(std::clamp(dot / std::sqrt(aa * bb), -1.0, 1.0)) : 0;
}
bool IsValidAudioEmbedding(const AudioEmbedding& value) {
    double norm = 0;
    for (float v : value) { if (!std::isfinite(v)) return false; norm += double(v) * v; }
    return std::fabs(norm - 1.0) < 0.002;
}
} // namespace HarkenSearch
