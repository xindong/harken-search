#include "src/Database.h"
#include "src/DevToolsModule.h"
#include "src/MetadataReader.h"
#include "src/platform/filesystem.h"
#include "src/platform/image_codec.h"
#include "src/platform/system.h"

#include "imgui.h"

#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>
#endif

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace TestCoverStubs {

static int g_frameCount = 0;
static double g_timeSeconds = 0.0;
static GLuint g_nextTextureId = 1;
static std::mutex g_albumArtMutex;
static std::condition_variable g_albumArtCv;
static std::string g_slowAlbumArtPath;
static std::thread::id g_albumArtThread;
static bool g_releaseAlbumArt = false;

void PauseAlbumArt(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_albumArtMutex);
    g_slowAlbumArtPath = path;
    g_albumArtThread = {};
    g_releaseAlbumArt = false;
}

std::thread::id WaitForAlbumArtReader() {
    std::unique_lock<std::mutex> lock(g_albumArtMutex);
    g_albumArtCv.wait_for(lock, std::chrono::seconds(2), [] {
        return g_albumArtThread != std::thread::id{};
    });
    return g_albumArtThread;
}

void ResumeAlbumArt() {
    std::lock_guard<std::mutex> lock(g_albumArtMutex);
    g_releaseAlbumArt = true;
    g_albumArtCv.notify_all();
}

static void RecordAlbumArtRead(const std::string& path) {
    std::unique_lock<std::mutex> lock(g_albumArtMutex);
    if (path != g_slowAlbumArtPath) return;
    g_albumArtThread = std::this_thread::get_id();
    g_albumArtCv.notify_all();
    g_albumArtCv.wait_for(lock, std::chrono::milliseconds(650), [] { return g_releaseAlbumArt; });
}

void AdvanceFrame(int frames, double dtSeconds) {
    if (frames < 1) frames = 1;
    g_frameCount += frames;
    g_timeSeconds += dtSeconds * static_cast<double>(frames);
}

} // namespace TestCoverStubs

namespace ImGui {

ImGuiIO& GetIO() {
    alignas(ImGuiIO) static unsigned char storage[sizeof(ImGuiIO)] = {};
    return *reinterpret_cast<ImGuiIO*>(storage);
}

int GetFrameCount() {
    return TestCoverStubs::g_frameCount;
}

double GetTime() {
    return TestCoverStubs::g_timeSeconds;
}

} // namespace ImGui

#ifdef _WIN32
extern "C" {

void APIENTRY glGenTextures(GLsizei n, GLuint* textures) {
    if (!textures) return;
    for (GLsizei i = 0; i < n; ++i) {
        textures[i] = TestCoverStubs::g_nextTextureId++;
    }
}

void APIENTRY glBindTexture(GLenum, GLuint) {
}

void APIENTRY glTexParameteri(GLenum, GLenum, GLint) {
}

void APIENTRY glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*) {
}

void APIENTRY glDeleteTextures(GLsizei, const GLuint*) {
}

unsigned char* stbi_load_from_memory(const unsigned char*, int, int*, int*, int*, int) {
    return nullptr;
}

void stbi_image_free(void*) {
}

} // extern "C"
#endif

namespace Platform {

bool IsSystemDarkMode() { return false; }
std::string GetMachineId() { return "test-machine"; }
void OpenInFileManager(const std::string&) {}
void OpenUrl(const std::string&) {}
bool LaunchProcess(const std::string&, const std::string&) { return false; }

void InitThreadCom() {}
void UninitThreadCom() {}
void SetThreadBackgroundPriority() {}
std::string OpenFileDialog(void*, const std::string&, const std::string&) { return {}; }
std::string SaveFileDialog(void*, const std::string&, const std::string&) { return {}; }
std::string BrowseFolderDialog(void*, const std::string&, const std::string&) { return {}; }
std::string GetAppDataDir() { return {}; }
std::string GetExeDir() { return {}; }

} // namespace Platform

namespace Platform::ImageCodec {

DecodedImage DecodeImageFromMemory(const uint8_t* data, size_t dataSize) {
    DecodedImage out;
    if (!data || dataSize == 0) return out;
    out.width = 2;
    out.height = 2;
    out.rgba.assign({
        255, 0, 0, 255,
        0, 255, 0, 255,
        0, 0, 255, 255,
        255, 255, 0, 255,
    });
    return out;
}

std::vector<uint8_t> EncodeRgbaToJpeg(const uint8_t*, int, int, float) {
    return {0xff, 0xd8, 0xff, 0xd9};
}

} // namespace Platform::ImageCodec

namespace Platform::FileSystem {

#ifdef _WIN32
bool AtomicFileReplace(const std::string&, const std::string&) { return false; }
#endif

} // namespace Platform::FileSystem

namespace ExternalLibraryCover {

void setDbPath(const std::string&) {
}

bool tryGetCoverForFolder(const std::string&, std::string&) {
    return false;
}

} // namespace ExternalLibraryCover

void DevTools_TickFrame(bool) {}
void DevTools_LogFrameSpike(float) {}
void DevTools_LogCoverUploadMs(double) {}
void DevTools_LogCoverPrimingMs(double) {}
void DevTools_SetEnabled(bool) {}
bool DevTools_IsEnabled() { return false; }
void DevTools_LogFrameFps(float) {}
void DevTools_LogMemoryMb(double) {}
void DevTools_LogWebViewRpcMs(double) {}
void DevTools_LogWebViewFrameOverheadMs(double) {}
void DevTools_BeginFramePhase(const char*) {}
void DevTools_EndFramePhase() {}
void DevTools_ResetFramePhases() {}

Database::Database() = default;
Database::~Database() = default;

bool Database::open(const std::string& dbPath, bool) {
    m_mainDbPath = dbPath;
    return false;
}

void Database::close() {
}

AudioRecord Database::getFileByPath(const std::string&) {
    return AudioRecord{};
}

bool Database::saveCoverThumb(const std::string&, const std::vector<uint8_t>&, const std::string&) {
    return true;
}

bool Database::getCoverThumb(const std::string&, std::vector<uint8_t>&, std::string&) {
    return false;
}

bool Database::hasCoverThumb(const std::string&) {
    return false;
}

bool MetadataReader::readAlbumArt(const std::string& path, AlbumArt&) {
    TestCoverStubs::RecordAlbumArtRead(path);
    return false;
}
