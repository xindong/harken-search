// AudioDragCard 渲染核对：把拖出小卡按实际绘制代码离屏渲染成 PNG。
// 传入真实 WAV 时使用该音频的真实峰值；单拖、多拖、缩卡中段各出一张，
// 并检查画布尺寸与透明角。该工具只验证卡片绘制，不代替真实拖放与文件交接。

#include "AudioDragCard.h"
#include "audio_drag_card_wav_fixture.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace Gdiplus;

int EncoderClsidForMime(const wchar_t* mime, CLSID* out) {
    UINT count = 0;
    UINT size = 0;
    GetImageEncodersSize(&count, &size);
    if (size == 0) return -1;
    std::vector<unsigned char> buffer(size);
    auto* encoders = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    GetImageEncoders(count, size, encoders);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, mime) == 0) {
            *out = encoders[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool WritePng(const std::vector<unsigned char>& pixels, int width, int height, const std::wstring& path) {
    Bitmap bitmap(width, height, width * 4, PixelFormat32bppPARGB,
                  const_cast<BYTE*>(pixels.data()));
    CLSID encoder = {};
    if (EncoderClsidForMime(L"image/png", &encoder) < 0) return false;
    return bitmap.Save(path.c_str(), &encoder, nullptr) == Ok;
}

bool CheckFrame(const char* label, const std::vector<unsigned char>& pixels, int width, int height,
                int expectedWidth, int expectedHeight) {
    if (width != expectedWidth || height != expectedHeight) {
        std::printf("FAIL %s: size %dx%d, expected %dx%d\n", label, width, height, expectedWidth, expectedHeight);
        return false;
    }
    const size_t corner = 0;
    const size_t center = (static_cast<size_t>(height / 2) * static_cast<size_t>(width) +
                           static_cast<size_t>(width / 2)) * 4;
    const bool cornerTransparent = pixels[corner + 3] == 0;
    const bool centerOpaque = pixels[center + 3] == 255;
    if (!cornerTransparent || !centerOpaque) {
        std::printf("FAIL %s: cornerAlpha=%u centerAlpha=%u\n", label,
                    static_cast<unsigned>(pixels[corner + 3]),
                    static_cast<unsigned>(pixels[center + 3]));
        return false;
    }
    std::printf("PASS %s: %dx%d cornerAlpha=0 centerAlpha=255\n", label, width, height);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) {
        std::printf("FAIL: GdiplusStartup\n");
        return 1;
    }

    std::filesystem::path outputDir = argc > 1 ? argv[1] : std::filesystem::current_path();
    std::filesystem::create_directories(outputDir);

    HarkenSearch::AudioDragCard::CardSpec spec;
    spec.fileName = L"(no audio)";
    std::vector<unsigned char> wavBytes;
    audio_drag_card_test::WavInfo wav;
    bool hasRealWaveform = false;
    if (argc > 2) {
        const std::filesystem::path wavPath = argv[2];
        if (audio_drag_card_test::ParseWav(wavPath, wavBytes, wav)) {
            spec.envelope = audio_drag_card_test::BuildEnvelopeFromWav(wav, 76);
            spec.fileName = wavPath.filename().wstring();
            hasRealWaveform = true;
            std::printf("WAV %s: %d ch, %d bit, %zu frames, %zu bars\n",
                        wavPath.filename().string().c_str(), wav.channels, wav.bitsPerSample,
                        wav.sampleFrames, spec.envelope.size());
        } else {
            std::printf("WAV unreadable: %s\n", wavPath.string().c_str());
        }
    }
    if (!hasRealWaveform) std::printf("NOTE: no real WAV supplied, card renders an empty waveform lane\n");

    bool ok = true;

    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;

    ok &= HarkenSearch::AudioDragCard::RenderToPixels(spec, 1.0f, 1.0f, 255, 0, pixels, width, height);
    ok &= CheckFrame("single-follow", pixels, width, height, 168, 59);
    ok &= WritePng(pixels, width, height, (outputDir / L"card-single-follow.png").wstring());

    HarkenSearch::AudioDragCard::CardSpec multi = spec;
    multi.itemCount = 3;
    ok &= HarkenSearch::AudioDragCard::RenderToPixels(multi, 1.0f, 1.0f, 255, 255, pixels, width, height);
    ok &= CheckFrame("multi-follow", pixels, width, height, 176, 65);
    ok &= WritePng(pixels, width, height, (outputDir / L"card-multi-follow.png").wstring());

    ok &= HarkenSearch::AudioDragCard::RenderToPixels(spec, 2.6f, 2.1f, 96, 0, pixels, width, height);
    ok &= CheckFrame("single-morph", pixels, width, height, static_cast<int>(std::lround(168.0f * 2.6f)),
                     static_cast<int>(std::lround(59.0f * 2.1f)));
    ok &= WritePng(pixels, width, height, (outputDir / L"card-single-morph.png").wstring());

    GdiplusShutdown(token);
    std::printf(ok ? "ALL PASS\n" : "HAS FAILURES\n");
    return ok ? 0 : 1;
}
