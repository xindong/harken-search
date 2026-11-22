#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "audio_drag_card_wav_fixture.h"

HWND g_hwnd = nullptr;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), count);
    result.pop_back();
    return result;
}

struct DragCardOrigin {
    std::vector<float> peaks;
    int peakChannels = 1;
    std::wstring displayName;
};
namespace HarkenSearch::AudioDragCard {
struct CardSpec {
    std::vector<float> envelope;
    std::wstring fileName;
    int itemCount = 0;
};
}
static std::unordered_map<std::string, std::vector<float>> cachedPeaks;
static std::vector<std::string> lookups;
static bool TryGetCachedDragPeaks(const std::string& file, std::vector<float>& peaks, int& channels) {
    lookups.push_back(file);
    const auto it = cachedPeaks.find(file);
    if (it == cachedPeaks.end()) return false;
    peaks = it->second;
    channels = 1;
    return true;
}
static std::vector<float> BuildDragCardEnvelope(const std::vector<float>& peaks, int, int) { return peaks; }
static std::wstring DisplayNameForDragPath(const std::string& path) { return Utf8ToWide(path); }
static std::vector<bool> bitmapValidity;
static std::vector<std::wstring> dragDiagnostics;
static void AppendDiagLogF(const wchar_t* format, ...) {
    wchar_t message[256]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);
    dragDiagnostics.emplace_back(message);
}
static void RecordBitmapValidity(HBITMAP bitmap) {
    BITMAP info{};
    bitmapValidity.push_back(GetObjectW(bitmap, sizeof(info), &info) != 0);
}

#include "product_fragments.h"

static int failures = 0;
static void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

static int TestCoordinates() {
    g_hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 423, 237, 400, 300,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hwnd) return 2;
    ImGui::CreateContext();
    POINT origin{};
    ClientToScreen(g_hwnd, &origin);
    ImGui::GetMainViewport()->Pos = {static_cast<float>(origin.x), static_cast<float>(origin.y)};
    ImGui::GetIO().ConfigFlags = ImGuiConfigFlags_ViewportsEnable;
    RECT mainRect = WaveformScreenRect(static_cast<float>(origin.x + 30),
                                       static_cast<float>(origin.y + 50), 120, 80);
    Check(mainRect.left == origin.x + 30 && mainRect.top == origin.y + 50 &&
          mainRect.right == origin.x + 150 && mainRect.bottom == origin.y + 130,
          "multi-viewport host coordinates remain desktop coordinates");
    RECT detached = WaveformScreenRect(1300, 510, 240, 60);
    Check(detached.left == 1300 && detached.top == 510 && detached.right == 1540 && detached.bottom == 570,
          "detached viewport coordinates do not use host position");
    RECT negative = WaveformScreenRect(-700, -120, 160, 60);
    Check(negative.left == -700 && negative.top == -120 && negative.right == -540 && negative.bottom == -60,
          "negative monitor coordinates stay negative");
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(400, 300);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_RendererHasViewports;
    unsigned char* pixels = nullptr;
    int fontWidth = 0, fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fontWidth, &fontHeight);
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Platform_CreateWindow = [](ImGuiViewport* v) {
        v->PlatformHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(v->ID));
        v->PlatformUserData = v->PlatformHandle;
    };
    platform.Platform_DestroyWindow = [](ImGuiViewport* v) { v->PlatformHandle = nullptr; v->PlatformUserData = nullptr; };
    platform.Platform_ShowWindow = [](ImGuiViewport*) {};
    platform.Platform_SetWindowPos = [](ImGuiViewport* v, ImVec2 p) { v->Pos = p; };
    platform.Platform_GetWindowPos = [](ImGuiViewport* v) { return v->Pos; };
    platform.Platform_SetWindowSize = [](ImGuiViewport* v, ImVec2 size) { v->Size = size; };
    platform.Platform_GetWindowSize = [](ImGuiViewport* v) { return v->Size; };
    platform.Platform_SetWindowFocus = [](ImGuiViewport*) {};
    platform.Platform_GetWindowFocus = [](ImGuiViewport*) { return false; };
    platform.Platform_GetWindowMinimized = [](ImGuiViewport*) { return false; };
    platform.Platform_SetWindowTitle = [](ImGuiViewport*, const char*) {};
    ImGuiPlatformMonitor monitor;
    monitor.MainPos = monitor.WorkPos = ImVec2(-1920, -1080);
    monitor.MainSize = monitor.WorkSize = ImVec2(5760, 3240);
    monitor.DpiScale = 1.0f;
    platform.Monitors.push_back(monitor);
    ImGui::GetMainViewport()->PlatformHandle = g_hwnd;
    ImGui::GetMainViewport()->Size = io.DisplaySize;
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(origin.x + 30), static_cast<float>(origin.y + 40)));
        ImGui::SetNextWindowSize(ImVec2(180, 100));
        ImGui::Begin("Host waveform", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        const ImVec2 hostCursor = ImGui::GetCursorScreenPos();
        const RECT hostResult = WaveformScreenRect(hostCursor.x, hostCursor.y, 100, 50);
        if (frame == 2) {
            Check(hostCursor.x >= origin.x + 30 && hostCursor.y >= origin.y + 40 &&
                  hostResult.left == static_cast<LONG>(hostCursor.x) && hostResult.top == static_cast<LONG>(hostCursor.y),
                  "real ImGui host cursor is already in desktop coordinates");
        }
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(1300, 510));
        ImGui::SetNextWindowSize(ImVec2(180, 100));
        ImGui::Begin("Detached waveform", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        const ImVec2 detachedCursor = ImGui::GetCursorScreenPos();
        const RECT detachedResult = WaveformScreenRect(detachedCursor.x, detachedCursor.y, 100, 50);
        if (frame == 2) {
            Check(ImGui::GetWindowViewport()->ID != ImGui::GetMainViewport()->ID &&
                  detachedCursor.x >= 1300 && detachedResult.left == static_cast<LONG>(detachedCursor.x) &&
                  detachedResult.top == static_cast<LONG>(detachedCursor.y),
                  "real ImGui detached viewport cursor remains in desktop coordinates");
        }
        ImGui::End();
        ImGui::Render();
        ImGui::UpdatePlatformWindows();
    }
    ImGui::DestroyPlatformWindows();
    io.ConfigFlags = 0;
    ImGui::GetMainViewport()->Pos = {0, 0};
    RECT single = WaveformScreenRect(30, 50, 120, 80);
    Check(single.left == origin.x + 30 && single.top == origin.y + 50 &&
          single.right == origin.x + 150 && single.bottom == origin.y + 130,
          "single viewport coordinates convert from host client to desktop");
    ImGui::DestroyContext();
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    return failures ? 1 : 0;
}

static int TestWaveform() {
    const std::vector<std::string> files{"first.wav", "second.wav"};
    cachedPeaks = {{"second.wav", {0.8f}}};
    lookups.clear();
    auto card = MakeCardForTest(files, nullptr);
    Check(card.fileName == L"first.wav" && card.envelope.empty(),
          "missing first waveform does not use second waveform");
    Check(lookups == std::vector<std::string>{"first.wav"},
          "multi-select only looks up first file waveform");
    cachedPeaks["first.wav"] = {0.2f};
    lookups.clear();
    card = MakeCardForTest(files, nullptr);
    Check(card.fileName == L"first.wav" && card.envelope == std::vector<float>{0.2f} && card.itemCount == 2,
          "first cached waveform keeps the first name and count");
    DragCardOrigin origin;
    origin.displayName = L"original.wav";
    origin.peaks = {0.6f};
    lookups.clear();
    card = MakeCardForTest(files, &origin);
    Check(card.fileName == L"original.wav" && card.envelope == std::vector<float>{0.6f} && lookups.empty(),
          "explicit region waveform and source name remain paired");
    return failures ? 1 : 0;
}

static HGLOBAL MakeGlobal(const char* value) {
    const size_t size = std::strlen(value) + 1;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return nullptr;
    void* ptr = GlobalLock(memory);
    if (!ptr) { GlobalFree(memory); return nullptr; }
    std::memcpy(ptr, value, size);
    GlobalUnlock(memory);
    return memory;
}

class ReleaseProbe : public IUnknown {
public:
    ReleaseProbe(HGLOBAL handle, int* count) : handle_(handle), count_(count) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (id != IID_IUnknown) { *out = nullptr; return E_NOINTERFACE; }
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (!refs) { GlobalFree(handle_); ++*count_; delete this; }
        return refs;
    }
private:
    ULONG refs_ = 1;
    HGLOBAL handle_;
    int* count_;
};

class RejectDataObject : public FileDataObject {
public:
    RejectDataObject() : FileDataObject({"C:/one.wav"}) {}
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
};

static int TestData() {
    const HRESULT init = OleInitialize(nullptr);
    if (FAILED(init)) return 2;
    auto* object = new FileDataObject({"C:/one.wav", "C:/two.wav"});
    FORMATETC drop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM output{};
    HRESULT hr = object->GetData(&drop, &output);
    wchar_t firstPath[MAX_PATH]{};
    wchar_t secondPath[MAX_PATH]{};
    const bool pathsMatch = SUCCEEDED(hr) &&
        DragQueryFileW(static_cast<HDROP>(output.hGlobal), 0, firstPath, MAX_PATH) &&
        DragQueryFileW(static_cast<HDROP>(output.hGlobal), 1, secondPath, MAX_PATH) &&
        std::wstring(firstPath) == L"C:\\one.wav" && std::wstring(secondPath) == L"C:\\two.wav";
    Check(SUCCEEDED(hr) && DragQueryFileW(static_cast<HDROP>(output.hGlobal), 0xFFFFFFFF, nullptr, 0) == 2 &&
          pathsMatch && object->QueryGetData(&drop) == S_OK,
          "CF_HDROP still provides both external files");
    if (SUCCEEDED(hr)) ReleaseStgMedium(&output);

    FORMATETC extra{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"HarkenDragReview")),
                    nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM input{};
    input.tymed = TYMED_HGLOBAL;
    input.hGlobal = MakeGlobal("copy before release");
    hr = object->SetData(&extra, &input, FALSE);
    std::printf("SetData(FALSE) hr=0x%08lx\n", static_cast<unsigned long>(hr));
    Check(hr == S_OK, "SetData(FALSE) accepts and copies private data");
    ReleaseStgMedium(&input);
    output = {};
    hr = object->GetData(&extra, &output);
    bool copied = false;
    if (SUCCEEDED(hr)) {
        const char* value = static_cast<const char*>(GlobalLock(output.hGlobal));
        copied = value && std::strcmp(value, "copy before release") == 0;
        if (value) GlobalUnlock(output.hGlobal);
        ReleaseStgMedium(&output);
    }
    Check(copied && object->QueryGetData(&extra) == S_OK,
          "GetData returns private data after caller releases its copy");

    FORMATETC streamFormat{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"HarkenDragReviewStream")),
                           nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM};
    IStream* stream = nullptr;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    Check(SUCCEEDED(hr) && stream, "create stream for drag context");
    if (stream) {
        const char payload[] = "stream data";
        ULONG written = 0;
        stream->Write(payload, sizeof(payload), &written);
        LARGE_INTEGER start{};
        stream->Seek(start, STREAM_SEEK_SET, nullptr);
        STGMEDIUM streamMedium{};
        streamMedium.tymed = TYMED_ISTREAM;
        streamMedium.pstm = stream;
        hr = object->SetData(&streamFormat, &streamMedium, FALSE);
        Check(hr == S_OK, "SetData(FALSE) retains an IStream reference");
        stream->Release();
        output = {};
        hr = object->GetData(&streamFormat, &output);
        char read[sizeof(payload)]{};
        ULONG count = 0;
        if (SUCCEEDED(hr)) output.pstm->Read(read, sizeof(read), &count);
        Check(SUCCEEDED(hr) && count == sizeof(payload) && std::strcmp(read, payload) == 0,
              "GetData returns valid stream after caller releases its reference");
        if (SUCCEEDED(hr)) ReleaseStgMedium(&output);
    }

    int releases = 0;
    STGMEDIUM owned{};
    owned.tymed = TYMED_HGLOBAL;
    owned.hGlobal = MakeGlobal("owned");
    owned.pUnkForRelease = new ReleaseProbe(owned.hGlobal, &releases);
    hr = object->SetData(&extra, &owned, TRUE);
    Check(hr == S_OK && releases == 0, "SetData(TRUE) transfers release ownership");
    if (FAILED(hr)) ReleaseStgMedium(&owned);

    IDragSourceHelper* helper = nullptr;
    hr = CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper));
    Check(SUCCEEDED(hr) && helper, "system drag helper available");
    if (helper) {
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = 1;
        bitmapInfo.bmiHeader.biHeight = -1;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (pixels) *static_cast<uint32_t*>(pixels) = 0;
        SHDRAGIMAGE image{};
        image.sizeDragImage = {1, 1};
        image.hbmpDragImage = bitmap;
        image.crColorKey = CLR_NONE;
        hr = helper->InitializeFromBitmap(&image, object);
        std::printf("InitializeFromBitmap hr=0x%08lx\n", static_cast<unsigned long>(hr));
        BITMAP bitmapState{};
        const bool bitmapExists = GetObjectW(bitmap, sizeof(bitmapState), &bitmapState) != 0;
        std::printf("bitmap exists after initialization=%d\n", bitmapExists);
        Check(SUCCEEDED(hr), "system helper initializes transparent image");
        if (FAILED(hr) && bitmapExists) DeleteObject(bitmap);
        FORMATETC imageFormat{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DragImageBits")),
                              nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        output = {};
        hr = object->GetData(&imageFormat, &output);
        std::printf("GetData(DragImageBits) hr=0x%08lx\n", static_cast<unsigned long>(hr));
        Check(SUCCEEDED(hr), "system helper image can be read back");
        if (SUCCEEDED(hr)) ReleaseStgMedium(&output);
        auto* rejected = new RejectDataObject();
        pixels = nullptr;
        HBITMAP rejectedBitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
        image.hbmpDragImage = rejectedBitmap;
        const HRESULT rejectedHr = helper->InitializeFromBitmap(&image, rejected);
        const bool rejectedBitmapExists = GetObjectW(rejectedBitmap, sizeof(bitmapState), &bitmapState) != 0;
        std::printf("rejected bitmap init hr=0x%08lx bitmap still exists=%d\n",
                    static_cast<unsigned long>(rejectedHr), rejectedBitmapExists);
        Check(FAILED(rejectedHr), "helper reports rejected SetData");
        if (rejectedBitmapExists) DeleteObject(rejectedBitmap);
        rejected->Release();
        helper->Release();
    }
    object->Release();
    Check(releases == 1, "transferred medium released exactly once");
    bitmapValidity.clear();
    dragDiagnostics.clear();
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto* next = new FileDataObject({"C:/one.wav"});
        SuppressDefaultDragImage(next);
        FORMATETC imageFormat{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DragImageBits")),
                              nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        FORMATETC contextFormat{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DragContext")),
                                nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM};
        STGMEDIUM image{};
        STGMEDIUM context{};
        const HRESULT imageHr = next->GetData(&imageFormat, &image);
        const HRESULT contextHr = next->GetData(&contextFormat, &context);
        Check(SUCCEEDED(imageHr) && SUCCEEDED(contextHr),
              attempt == 0 ? "first product helper initializes image and context"
                           : "second product helper initializes fresh image and context");
        if (SUCCEEDED(imageHr)) ReleaseStgMedium(&image);
        if (SUCCEEDED(contextHr)) ReleaseStgMedium(&context);
        IEnumFORMATETC* enumeration = nullptr;
        FORMATETC firstFormat{};
        ULONG fetched = 0;
        const HRESULT enumHr = next->EnumFormatEtc(DATADIR_GET, &enumeration);
        const HRESULT nextHr = enumeration ? enumeration->Next(1, &firstFormat, &fetched) : E_FAIL;
        Check(SUCCEEDED(enumHr) && nextHr == S_OK && fetched == 1 && firstFormat.cfFormat == CF_HDROP &&
              next->QueryGetData(&drop) == S_OK, "file format remains first and available after helper");
        if (enumeration) enumeration->Release();
        next->Release();
    }
    Check(bitmapValidity.size() == 2 && bitmapValidity[0] && bitmapValidity[1],
          "each product helper call supplies a live bitmap");
    Check(dragDiagnostics.empty(), "successful helper does not report failure");
    auto* failure = new RejectDataObject();
    SuppressDefaultDragImage(failure);
    Check(dragDiagnostics.size() == 1 && dragDiagnostics[0].find(L"0x80004001") != std::wstring::npos,
          "product reports failed helper initialization and HRESULT");
    output = {};
    hr = failure->GetData(&drop, &output);
    Check(SUCCEEDED(hr) && DragQueryFileW(static_cast<HDROP>(output.hGlobal), 0xFFFFFFFF, nullptr, 0) == 1,
          "failed helper leaves file handoff available");
    if (SUCCEEDED(hr)) ReleaseStgMedium(&output);
    failure->Release();
    OleUninitialize();
    return failures ? 1 : 0;
}

static void Put16(std::vector<unsigned char>& bytes, size_t at, uint16_t value) {
    bytes[at] = static_cast<unsigned char>(value);
    bytes[at + 1] = static_cast<unsigned char>(value >> 8);
}
static void Put32(std::vector<unsigned char>& bytes, size_t at, uint32_t value) {
    Put16(bytes, at, static_cast<uint16_t>(value));
    Put16(bytes, at + 2, static_cast<uint16_t>(value >> 16));
}
static std::vector<unsigned char> MakeWav(uint16_t channels, uint16_t bits, uint16_t align,
                                          const std::vector<unsigned char>& data, uint16_t tag = 1) {
    std::vector<unsigned char> bytes(44 + data.size() + (data.size() % 2), 0);
    std::memcpy(bytes.data(), "RIFF", 4);
    Put32(bytes, 4, static_cast<uint32_t>(bytes.size() - 8));
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
    Put32(bytes, 16, 16);
    Put16(bytes, 20, tag);
    Put16(bytes, 22, channels);
    Put32(bytes, 24, 48000);
    Put32(bytes, 28, 48000 * align);
    Put16(bytes, 32, align);
    Put16(bytes, 34, bits);
    std::memcpy(bytes.data() + 36, "data", 4);
    Put32(bytes, 40, static_cast<uint32_t>(data.size()));
    std::copy(data.begin(), data.end(), bytes.begin() + 44);
    return bytes;
}
static bool ParseBytes(const char* name, const std::vector<unsigned char>& data,
                       std::vector<unsigned char>& bytes, audio_drag_card_test::WavInfo& info) {
    std::ofstream stream(name, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    stream.close();
    return audio_drag_card_test::ParseWav(name, bytes, info);
}
static int TestWav() {
    std::vector<unsigned char> bytes;
    audio_drag_card_test::WavInfo info{};
    Check(!ParseBytes("short-align.wav", MakeWav(2, 16, 2, {0, 0, 0, 0}), bytes, info),
          "reject block alignment shorter than channel samples");
    info = {};
    Check(!ParseBytes("bad-bits.wav", MakeWav(1, 20, 3, {0, 0, 0}), bytes, info),
          "reject unsupported sample width");
    info = {};
    Check(!ParseBytes("zero-channels.wav", MakeWav(0, 16, 2, {0, 0}), bytes, info),
          "reject zero channels");
    info = {};
    Check(!ParseBytes("partial-frame.wav", MakeWav(2, 16, 4, {0, 0, 0, 0, 0}), bytes, info),
          "reject incomplete sample frame");
    info = {};
    auto huge = MakeWav(1, 16, 2, {0, 0});
    Put32(huge, 40, 0xFFFFFFFFu);
    Check(!ParseBytes("oversized.wav", huge, bytes, info), "reject chunk length beyond file");
    info = {};
    Check(ParseBytes("valid.wav", MakeWav(2, 16, 4, {0, 0, 0xff, 0x7f}), bytes, info) &&
          info.sampleFrames == 1 && audio_drag_card_test::ReadSample(info, 0, 1) > 0.99f,
          "accept and read valid stereo samples");
    Check(audio_drag_card_test::ReadSample(info, 1, 0) == 0.0f &&
          audio_drag_card_test::ReadSample(info, 0, 2) == 0.0f,
          "reject reads outside frame and channel bounds");
    info = {};
    Check(ParseBytes("valid-odd.wav", MakeWav(1, 8, 1, {0x80}), bytes, info) && info.sampleFrames == 1,
          "accept odd-sized 8-bit payload with padding");
    auto missingPad = MakeWav(1, 8, 1, {0x80});
    missingPad.pop_back();
    Check(!ParseBytes("missing-pad.wav", missingPad, bytes, info),
          "reject missing padding even when reusing WavInfo");
    info = {};
    Check(ParseBytes("float32.wav", MakeWav(1, 32, 4, {0, 0, 0, 0x3f}, 3), bytes, info) &&
          audio_drag_card_test::ReadSample(info, 0, 0) == 0.5f,
          "accept supported 32-bit floating point samples");
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string test = argv[1];
    if (test == "coordinates") return TestCoordinates();
    if (test == "waveform") return TestWaveform();
    if (test == "data") return TestData();
    if (test == "wav") return TestWav();
    return 2;
}
