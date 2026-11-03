#include "../MediaFoundationAudioDecoder.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace HarkenSearch::MediaFoundationAudio {
namespace {

// Keep the shared decoder interface name for compatibility. On macOS its
// M4A implementation uses the system Audio Toolbox, never Media Foundation.
struct AudioFile {
    ExtAudioFileRef ref = nullptr;
    ~AudioFile() {
        if (ref) ExtAudioFileDispose(ref);
    }
    AudioFile(const AudioFile&) = delete;
    AudioFile& operator=(const AudioFile&) = delete;
    AudioFile() = default;
};

void SetError(std::string* error, const std::string& value) {
    if (error) *error = value;
}

std::string StatusError(const char* operation, OSStatus status) {
    return std::string("audio_toolbox_") + operation + ":" + std::to_string(status);
}

bool OpenM4a(const std::string& path,
             AudioFile& file,
             AudioStreamBasicDescription& format,
             SInt64& lengthFrames,
             std::string* error) {
    format = {};
    lengthFrames = 0;
    if (!CanDecodeExtension(path)) {
        SetError(error, "media_foundation_extension_not_supported");
        return false;
    }
    if (LowerExtension(path) != ".m4a") {
        SetError(error, "media_foundation_unavailable");
        return false;
    }
    if (path.empty() || path.find('\0') != std::string::npos) {
        SetError(error, "audio_toolbox_invalid_path");
        return false;
    }

    CFStringRef name = CFStringCreateWithCString(
        kCFAllocatorDefault, path.c_str(), kCFStringEncodingUTF8);
    if (!name) {
        SetError(error, "audio_toolbox_invalid_utf8_path");
        return false;
    }
    CFURLRef url = CFURLCreateWithFileSystemPath(
        kCFAllocatorDefault, name, kCFURLPOSIXPathStyle, false);
    CFRelease(name);
    if (!url) {
        SetError(error, "audio_toolbox_invalid_file_url");
        return false;
    }
    const OSStatus openStatus = ExtAudioFileOpenURL(url, &file.ref);
    CFRelease(url);
    if (openStatus != noErr || !file.ref) {
        SetError(error, StatusError("open_failed", openStatus));
        return false;
    }

    UInt32 formatSize = sizeof(format);
    const OSStatus formatStatus = ExtAudioFileGetProperty(
        file.ref, kExtAudioFileProperty_FileDataFormat, &formatSize, &format);
    if (formatStatus != noErr || formatSize != sizeof(format)) {
        SetError(error, StatusError("format_failed", formatStatus));
        return false;
    }
    if (!std::isfinite(format.mSampleRate) || format.mSampleRate <= 0 ||
        format.mSampleRate > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
        format.mChannelsPerFrame == 0 || format.mChannelsPerFrame > 32) {
        SetError(error, "audio_toolbox_invalid_audio_format");
        return false;
    }

    UInt32 lengthSize = sizeof(lengthFrames);
    if (ExtAudioFileGetProperty(file.ref, kExtAudioFileProperty_FileLengthFrames,
                                &lengthSize, &lengthFrames) != noErr ||
        lengthSize != sizeof(lengthFrames)) {
        lengthFrames = 0;
    }
    return true;
}

} // namespace

bool Probe(const std::string& path,
           uint32_t& sampleRate,
           uint32_t& channels,
           uint64_t& totalFrames,
           float& durationSeconds,
           std::string* error) {
    sampleRate = 0;
    channels = 0;
    totalFrames = 0;
    durationSeconds = 0.0f;
    if (error) error->clear();

    AudioFile file;
    AudioStreamBasicDescription format = {};
    SInt64 lengthFrames = 0;
    if (!OpenM4a(path, file, format, lengthFrames, error)) return false;
    if (lengthFrames <= 0) {
        SetError(error, "audio_toolbox_missing_duration");
        return false;
    }
    sampleRate = static_cast<uint32_t>(std::llround(format.mSampleRate));
    channels = format.mChannelsPerFrame;
    totalFrames = static_cast<uint64_t>(lengthFrames);
    durationSeconds = static_cast<float>(
        static_cast<double>(totalFrames) / format.mSampleRate);
    return true;
}

bool Decode(const std::string& path,
            DecodedAudioBuffer& out,
            std::string* error,
            const std::atomic<bool>* cancelFlag) {
    out = {};
    if (error) error->clear();
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
        SetError(error, "cancelled");
        return false;
    }

    AudioFile file;
    AudioStreamBasicDescription format = {};
    SInt64 lengthFrames = 0;
    if (!OpenM4a(path, file, format, lengthFrames, error)) return false;

    AudioStreamBasicDescription client = {};
    client.mSampleRate = format.mSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                          kAudioFormatFlagsNativeEndian;
    client.mBitsPerChannel = 32;
    client.mChannelsPerFrame = format.mChannelsPerFrame;
    client.mFramesPerPacket = 1;
    client.mBytesPerFrame = client.mChannelsPerFrame * sizeof(float);
    client.mBytesPerPacket = client.mBytesPerFrame;
    const OSStatus clientStatus = ExtAudioFileSetProperty(
        file.ref, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client);
    if (clientStatus != noErr) {
        SetError(error, StatusError("client_format_failed", clientStatus));
        return false;
    }

    constexpr UInt32 kChunkFrames = 8192;
    DecodedAudioBuffer decoded;
    decoded.sampleRate = static_cast<uint32_t>(std::llround(format.mSampleRate));
    decoded.channels = format.mChannelsPerFrame;
    try {
        std::vector<float> chunk(static_cast<size_t>(kChunkFrames) * decoded.channels);
        if (lengthFrames > 0 &&
            static_cast<uint64_t>(lengthFrames) <=
                decoded.interleaved.max_size() / decoded.channels) {
            const size_t reserveSamples =
                static_cast<size_t>(lengthFrames) * decoded.channels;
            decoded.interleaved.reserve(reserveSamples);
        }
        for (;;) {
            if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                SetError(error, "cancelled");
                return false;
            }
            AudioBufferList buffers = {};
            buffers.mNumberBuffers = 1;
            buffers.mBuffers[0].mNumberChannels = decoded.channels;
            buffers.mBuffers[0].mDataByteSize =
                kChunkFrames * client.mBytesPerFrame;
            buffers.mBuffers[0].mData = chunk.data();
            UInt32 readFrames = kChunkFrames;
            const OSStatus readStatus = ExtAudioFileRead(file.ref, &readFrames, &buffers);
            if (readStatus != noErr) {
                SetError(error, StatusError("read_failed", readStatus));
                return false;
            }
            if (readFrames == 0) break;
            const size_t readSamples = static_cast<size_t>(readFrames) * decoded.channels;
            if (readSamples > decoded.interleaved.max_size() - decoded.interleaved.size()) {
                SetError(error, "audio_toolbox_audio_too_large");
                return false;
            }
            decoded.interleaved.insert(decoded.interleaved.end(),
                                       chunk.data(), chunk.data() + readSamples);
        }
    } catch (const std::bad_alloc&) {
        SetError(error, "audio_toolbox_out_of_memory");
        return false;
    }
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
        SetError(error, "cancelled");
        return false;
    }
    if (decoded.interleaved.empty()) {
        SetError(error, "audio_toolbox_decode_no_frames");
        return false;
    }
    decoded.totalFrames = decoded.interleaved.size() / decoded.channels;
    decoded.durationSeconds = static_cast<float>(
        static_cast<double>(decoded.totalFrames) / format.mSampleRate);
    out = std::move(decoded);
    return true;
}

} // namespace HarkenSearch::MediaFoundationAudio
