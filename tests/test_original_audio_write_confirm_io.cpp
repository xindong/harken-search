#include "src/OriginalAudioWriteConfirmGate.h"
#include "MetadataWriter.h"
#include "MetadataReader.h"
#include "Localization.h"
#include "platform/filesystem_probe_path_test_hooks.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

AppLanguage g_currentLanguage = AppLanguage::Chinese;
const char* GetLocalizedString(LStr id) {
    (void)id;
    return "";
}

namespace {

int g_failures = 0;
int g_writeCalls = 0;
int g_readCalls = 0;
bool g_forceReparseDecision = false;
std::filesystem::path g_forceReparseTarget;

#ifndef HARKEN_TEST_APPROVED_ROOT_UTF8
#error HARKEN_TEST_APPROVED_ROOT_UTF8 must name the isolated test fixture root
#endif
const std::filesystem::path kApprovedRoot =
    std::filesystem::u8path(HARKEN_TEST_APPROVED_ROOT_UTF8);

void Check(bool condition, const char* message) {
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

enum class AttributeQuery {
    Present,
    Missing,
    Failed,
};

AttributeQuery QueryPathAttributes(const std::filesystem::path& target, DWORD* attributes) {
    if (g_forceReparseDecision && !g_forceReparseTarget.empty() && target == g_forceReparseTarget) {
        *attributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
        return AttributeQuery::Present;
    }
    *attributes = GetFileAttributesW(target.c_str());
    if (*attributes != INVALID_FILE_ATTRIBUTES) {
        return AttributeQuery::Present;
    }
    const DWORD lastError = GetLastError();
    if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
        return AttributeQuery::Missing;
    }
    return AttributeQuery::Failed;
}

bool HasReparseAttribute(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::filesystem::path NormalizeAbsolute(const std::filesystem::path& target, std::error_code& error) {
    const std::filesystem::path absolutePath = std::filesystem::absolute(target, error);
    if (error) {
        return {};
    }
    return absolutePath.lexically_normal();
}

bool IsStrictChildPath(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const std::filesystem::path relative = child.lexically_relative(parent);
    if (relative.empty() || relative == std::filesystem::path(".")) {
        return false;
    }
    for (const auto& component : relative) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

bool WalkChainHasNoReparse(const std::filesystem::path& start, bool targetMustExist) {
    std::filesystem::path cursor = start;
    bool inspectingTarget = true;
    while (true) {
        DWORD attributes = 0;
        const AttributeQuery query = QueryPathAttributes(cursor, &attributes);
        if (query == AttributeQuery::Failed) {
            return false;
        }
        if (query == AttributeQuery::Missing) {
            if (!(inspectingTarget && !targetMustExist)) {
                return false;
            }
        } else if (HasReparseAttribute(attributes)) {
            return false;
        }
        const std::filesystem::path parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) {
            break;
        }
        cursor = parent;
        inspectingTarget = false;
    }
    return true;
}

struct PathInspection {
    bool ok = false;
    std::filesystem::path originalAbsolute;
    std::filesystem::path resolved;
};

PathInspection InspectPath(const std::filesystem::path& original,
                           bool targetMustExist,
                           bool mustBeStrictChild) {
    PathInspection inspection;
    std::error_code error;
    inspection.originalAbsolute = NormalizeAbsolute(original, error);
    if (error || inspection.originalAbsolute.empty()) {
        return inspection;
    }

    std::error_code existsError;
    const bool currentlyExists =
        std::filesystem::exists(inspection.originalAbsolute, existsError) && !existsError;
    if (existsError) {
        return inspection;
    }
    if (currentlyExists) {
        inspection.resolved = std::filesystem::weakly_canonical(inspection.originalAbsolute, error);
        if (error) {
            return inspection;
        }
    } else {
        inspection.resolved = inspection.originalAbsolute;
    }

    const std::filesystem::path approvedAbsolute = NormalizeAbsolute(kApprovedRoot, error);
    if (error) {
        return inspection;
    }
    const std::filesystem::path approvedResolved = std::filesystem::weakly_canonical(approvedAbsolute, error);
    if (error) {
        return inspection;
    }

    if (mustBeStrictChild) {
        if (!IsStrictChildPath(inspection.originalAbsolute, approvedResolved) ||
            !IsStrictChildPath(inspection.resolved, approvedResolved)) {
            return inspection;
        }
    } else if (inspection.resolved != approvedResolved &&
               !IsStrictChildPath(inspection.resolved, approvedResolved)) {
        return inspection;
    }

    if (!WalkChainHasNoReparse(inspection.originalAbsolute, targetMustExist)) {
        return inspection;
    }
    if (inspection.resolved != inspection.originalAbsolute &&
        !WalkChainHasNoReparse(inspection.resolved, targetMustExist)) {
        return inspection;
    }
    if (targetMustExist) {
        DWORD attributes = 0;
        if (QueryPathAttributes(inspection.resolved, &attributes) != AttributeQuery::Present) {
            return inspection;
        }
    }
    inspection.ok = true;
    return inspection;
}

bool VerifyApprovedRootReadOnly() {
    std::error_code error;
    if (!std::filesystem::exists(kApprovedRoot, error) || error) {
        return false;
    }
    const PathInspection inspection = InspectPath(kApprovedRoot, true, false);
    return inspection.ok;
}

bool VerifyUniqueFixture(const PathInspection& expected) {
    if (!expected.ok) {
        return false;
    }
    const PathInspection current = InspectPath(expected.originalAbsolute, true, true);
    return current.ok && current.resolved == expected.resolved;
}

bool CleanupUniqueFixtureDir(const std::filesystem::path& uniqueDir, bool* refused) {
    *refused = false;
    const PathInspection inspection = InspectPath(uniqueDir, true, true);
    if (!inspection.ok) {
        *refused = true;
        return false;
    }
    std::error_code error;
    std::filesystem::remove_all(inspection.resolved, error);
    return !error;
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
}

void WriteAllBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

bool GuardedWriteAllBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    const PathInspection inspection = InspectPath(path, false, true);
    if (!inspection.ok) {
        return false;
    }
    WriteAllBytes(inspection.resolved, bytes);
    const PathInspection afterWrite = InspectPath(path, true, true);
    return afterWrite.ok;
}

bool GuardedReadAllBytes(const std::filesystem::path& path, std::vector<uint8_t>* bytes) {
    const PathInspection inspection = InspectPath(path, true, true);
    if (!inspection.ok) {
        return false;
    }
    *bytes = ReadAllBytes(inspection.resolved);
    return true;
}

std::vector<uint8_t> MakePcmWav() {
    const uint32_t sampleRate = 8000;
    const uint16_t channels = 1;
    const uint16_t bitsPerSample = 16;
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    const uint32_t byteRate = sampleRate * blockAlign;
    const uint32_t dataBytes = 32;
    const uint32_t riffSize = 36 + dataBytes;
    std::vector<uint8_t> wav(44 + dataBytes, 0);
    std::memcpy(wav.data() + 0, "RIFF", 4);
    std::memcpy(wav.data() + 4, &riffSize, 4);
    std::memcpy(wav.data() + 8, "WAVE", 4);
    std::memcpy(wav.data() + 12, "fmt ", 4);
    const uint32_t fmtSize = 16;
    std::memcpy(wav.data() + 16, &fmtSize, 4);
    const uint16_t audioFormat = 1;
    std::memcpy(wav.data() + 20, &audioFormat, 2);
    std::memcpy(wav.data() + 22, &channels, 2);
    std::memcpy(wav.data() + 24, &sampleRate, 4);
    std::memcpy(wav.data() + 28, &byteRate, 4);
    std::memcpy(wav.data() + 32, &blockAlign, 2);
    std::memcpy(wav.data() + 34, &bitsPerSample, 2);
    std::memcpy(wav.data() + 36, "data", 4);
    std::memcpy(wav.data() + 40, &dataBytes, 4);
    return wav;
}

std::vector<uint8_t> MakeJpegBytes() {
    std::vector<uint8_t> jpeg = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
        0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xFE};
    const uint16_t commentLength = 108;
    jpeg.push_back(static_cast<uint8_t>((commentLength >> 8) & 0xFF));
    jpeg.push_back(static_cast<uint8_t>(commentLength & 0xFF));
    jpeg.insert(jpeg.end(), static_cast<size_t>(commentLength - 2), 0x41);
    jpeg.push_back(0xFF);
    jpeg.push_back(0xD9);
    return jpeg;
}

const std::vector<uint8_t> kJpegBytes = MakeJpegBytes();

class FixtureDir {
public:
    explicit FixtureDir(std::filesystem::path uniqueDir)
        : uniqueDir_(std::move(uniqueDir)), active_(false) {
        if (!VerifyApprovedRootReadOnly()) {
            std::printf("FAIL: approved fixture root is missing or unsafe\n");
            return;
        }
        const PathInspection beforeCreate = InspectPath(uniqueDir_, false, true);
        if (!beforeCreate.ok) {
            std::printf("FAIL: unique fixture dir rejected before create\n");
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(beforeCreate.originalAbsolute, error);
        if (error) {
            std::printf("FAIL: cannot create unique fixture dir\n");
            return;
        }
        expected_ = InspectPath(uniqueDir_, true, true);
        if (!expected_.ok) {
            std::printf("FAIL: unique fixture dir rejected after create\n");
            bool refused = false;
            CleanupUniqueFixtureDir(uniqueDir_, &refused);
            return;
        }
        active_ = true;
    }

    bool CleanupNow() {
        if (cleaned_) {
            return true;
        }
        if (cleanupAttempted_) {
            return false;
        }
        cleanupAttempted_ = true;
        if (!active_) {
            cleaned_ = true;
            return true;
        }
        if (!VerifyUniqueFixture(expected_)) {
            std::printf("FAIL: refusing cleanup, identity not proven\n");
            ++g_failures;
            active_ = false;
            return false;
        }
        std::error_code error;
        std::filesystem::remove_all(expected_.resolved, error);
        if (error) {
            std::printf("FAIL: unique fixture cleanup failed\n");
            ++g_failures;
            active_ = false;
            return false;
        }
        cleaned_ = true;
        active_ = false;
        return true;
    }

    ~FixtureDir() {
        if (cleaned_ || cleanupAttempted_ || !active_) {
            return;
        }
        CleanupNow();
        if (cleaned_ || !active_) {
            return;
        }
        if (!VerifyUniqueFixture(expected_)) {
            std::printf("FAIL: refusing cleanup, identity not proven\n");
            ++g_failures;
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(expected_.resolved, error);
        if (error) {
            std::printf("FAIL: unique fixture cleanup failed\n");
            ++g_failures;
        }
    }

    bool active() const { return active_; }
    const std::filesystem::path& path() const { return expected_.resolved; }

    bool Reverify() const { return VerifyUniqueFixture(expected_); }

private:
    std::filesystem::path uniqueDir_;
    PathInspection expected_;
    bool active_;
    bool cleaned_ = false;
    bool cleanupAttempted_ = false;
};

std::string PathUtf8(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes > 0 ? bytes - 1 : 0), char{});
    if (bytes > 1) {
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
    }
    return utf8;
}

using aura::metadata::OriginalAudioWriteConfirmRequest;

OriginalAudioWriteConfirmRequest MakeRequest(bool suppressConfirmDialogs, bool confirmed, bool cancelled) {
    OriginalAudioWriteConfirmRequest request;
    request.suppressConfirmDialogs = suppressConfirmDialogs;
    request.confirmed = confirmed;
    request.cancelled = cancelled;
    return request;
}

bool GuardedWriteAlbumArt(const std::filesystem::path& path, bool targetMustExist, bool* writerReturned) {
    if (writerReturned != nullptr) {
        *writerReturned = false;
    }
    const PathInspection inspection = InspectPath(path, targetMustExist, true);
    if (!inspection.ok) {
        return false;
    }
    ++g_writeCalls;
    const bool ok = MetadataWriter::writeAlbumArt(PathUtf8(inspection.resolved), kJpegBytes, "image/jpeg");
    if (writerReturned != nullptr) {
        *writerReturned = ok;
    }
    return true;
}

bool GuardedReadAlbumArt(const std::filesystem::path& path, AlbumArt* art, bool* readerReturned) {
    if (readerReturned != nullptr) {
        *readerReturned = false;
    }
    const PathInspection inspection = InspectPath(path, true, true);
    if (!inspection.ok) {
        return false;
    }
    ++g_readCalls;
    const bool ok = MetadataReader::readAlbumArt(PathUtf8(inspection.resolved), *art);
    if (readerReturned != nullptr) {
        *readerReturned = ok;
    }
    return true;
}

void ExpectBlockedWrite(const char* invokeMessage,
                        const std::filesystem::path& audioPath,
                        const std::vector<uint8_t>& originalBytes,
                        OriginalAudioWriteConfirmRequest request) {
    if (!InspectPath(audioPath, true, true).ok) {
        Check(false, "blocked-write path failed re-verification");
        return;
    }
    g_writeCalls = 0;
    const bool invoked = TryInvokeOriginalAudioWrite(request, [&]() {
        GuardedWriteAlbumArt(audioPath, true, nullptr);
    });
    Check(!invoked, invokeMessage);
    Check(g_writeCalls == 0, invokeMessage);
    std::vector<uint8_t> afterBytes;
    Check(GuardedReadAllBytes(audioPath, &afterBytes) && afterBytes == originalBytes, invokeMessage);
    AlbumArt art;
    bool readable = false;
    Check(GuardedReadAlbumArt(audioPath, &art, &readable), invokeMessage);
    Check(!readable || art.data != kJpegBytes, invokeMessage);
}

void RunCancelMatrix(const char* formatName,
                     const std::filesystem::path& audioPath,
                     const std::vector<uint8_t>& originalBytes) {
    if (std::string(formatName) == "wav") {
        ExpectBlockedWrite("unconfirmed wav does not invoke writer", audioPath, originalBytes, MakeRequest(false, false, false));
        ExpectBlockedWrite("cancelled wav does not invoke writer", audioPath, originalBytes, MakeRequest(false, false, true));
        ExpectBlockedWrite("cancelled+confirmed wav does not invoke writer", audioPath, originalBytes, MakeRequest(false, true, true));
        ExpectBlockedWrite("cancelled+suppress wav does not invoke writer", audioPath, originalBytes, MakeRequest(true, false, true));
        ExpectBlockedWrite("cancelled+confirmed+suppress wav does not invoke writer", audioPath, originalBytes, MakeRequest(true, true, true));
        return;
    }
    ExpectBlockedWrite("unconfirmed mp3 does not invoke writer", audioPath, originalBytes, MakeRequest(false, false, false));
    ExpectBlockedWrite("cancelled mp3 does not invoke writer", audioPath, originalBytes, MakeRequest(false, false, true));
    ExpectBlockedWrite("cancelled+confirmed mp3 does not invoke writer", audioPath, originalBytes, MakeRequest(false, true, true));
    ExpectBlockedWrite("cancelled+suppress mp3 does not invoke writer", audioPath, originalBytes, MakeRequest(true, false, true));
    ExpectBlockedWrite("cancelled+confirmed+suppress mp3 does not invoke writer", audioPath, originalBytes, MakeRequest(true, true, true));
}

}

int main() {
    bool refusedApprovedCleanup = false;
    Check(!CleanupUniqueFixtureDir(kApprovedRoot, &refusedApprovedCleanup) && refusedApprovedCleanup,
          "approved root cleanup is refused");

    const std::filesystem::path siblingPrefix =
        kApprovedRoot.parent_path() / (kApprovedRoot.filename().wstring() + L"-sibling");
    const PathInspection siblingInspection = InspectPath(siblingPrefix / "probe.wav", false, true);
    Check(!siblingInspection.ok, "sibling prefix path is rejected without create");
    Check(!std::filesystem::exists(siblingPrefix), "sibling prefix path is rejected without create");

    const std::filesystem::path dotdotEscape = kApprovedRoot / ".." / "outside-batch4-r2";
    const PathInspection escapeInspection = InspectPath(dotdotEscape, false, true);
    Check(!escapeInspection.ok, "dotdot escape is rejected without create");

    g_forceReparseDecision = true;
    g_forceReparseTarget = kApprovedRoot;
    const PathInspection forcedReparse = InspectPath(kApprovedRoot / "forced-reparse-child", false, true);
    Check(!forcedReparse.ok, "forced ancestor reparse is a guard decision reject");
    g_forceReparseDecision = false;
    g_forceReparseTarget.clear();

    const DWORD processId = GetCurrentProcessId();
    const DWORD ticks = GetTickCount();
    const std::filesystem::path unique =
        kApprovedRoot / ("batch4-io-r2-" + std::to_string(processId) + "-" + std::to_string(ticks));
    FixtureDir fixture(unique);
    Check(fixture.active(), "unique fixture dir is inside approved root");
    if (!fixture.active()) {
        return 1;
    }
    Check(fixture.Reverify(), "unique fixture dir still matches after create");

    const std::vector<uint8_t> wavBytes = MakePcmWav();
    const std::filesystem::path wavPath = fixture.path() / "confirm.wav";
    Check(GuardedWriteAllBytes(wavPath, wavBytes), "wav fixture written inside unique dir");
    RunCancelMatrix("wav", wavPath, wavBytes);

    bool wavWriteOk = false;
    bool wavWriteAttempted = false;
    g_writeCalls = 0;
    const bool wavConfirmed = TryInvokeOriginalAudioWrite(MakeRequest(false, true, false), [&]() {
        wavWriteAttempted = GuardedWriteAlbumArt(wavPath, true, &wavWriteOk);
    });
    Check(wavConfirmed, "confirmed wav invokes writer callback");
    Check(wavWriteAttempted, "confirmed wav writer not skipped");
    Check(g_writeCalls == 1, "confirmed wav calls writeAlbumArt once");
    Check(wavWriteOk, "confirmed wav writeAlbumArt returned true");
    AlbumArt wavArt;
    g_readCalls = 0;
    bool wavReadable = false;
    Check(GuardedReadAlbumArt(wavPath, &wavArt, &wavReadable), "confirmed wav reader not skipped");
    Check(wavReadable, "wav album art readable");
    Check(wavArt.data == kJpegBytes, "wav JPEG bytes round-trip");

    const std::filesystem::path mp3Path = fixture.path() / "tag-only.mp3";
    const std::vector<uint8_t> mp3Bytes = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    Check(GuardedWriteAllBytes(mp3Path, mp3Bytes), "mp3 fixture written inside unique dir");
    RunCancelMatrix("mp3", mp3Path, mp3Bytes);

    bool mp3WriteOk = false;
    bool mp3WriteAttempted = false;
    g_writeCalls = 0;
    const bool mp3Confirmed = TryInvokeOriginalAudioWrite(MakeRequest(false, true, false), [&]() {
        mp3WriteAttempted = GuardedWriteAlbumArt(mp3Path, true, &mp3WriteOk);
    });
    Check(mp3Confirmed, "confirmed mp3 invokes writer callback");
    Check(mp3WriteAttempted, "confirmed mp3 writer not skipped");
    Check(g_writeCalls == 1, "confirmed mp3 calls writeAlbumArt once");
    Check(mp3WriteOk, "confirmed mp3 writeAlbumArt returned true");
    AlbumArt mp3Art;
    g_readCalls = 0;
    bool mp3Readable = false;
    Check(GuardedReadAlbumArt(mp3Path, &mp3Art, &mp3Readable), "confirmed mp3 reader not skipped");
    Check(mp3Readable, "mp3 album art readable");
    Check(mp3Art.data == kJpegBytes, "mp3 JPEG APIC round-trip; not MPEG playback");

    const std::filesystem::path missingPath = fixture.path() / "missing.wav";
    Check(InspectPath(missingPath, false, true).ok, "missing path stays inside unique dir");
    Check(!std::filesystem::exists(missingPath), "missing path write does not create the file first");
    bool missingWriterReturned = false;
    Check(GuardedWriteAlbumArt(missingPath, false, &missingWriterReturned), "missing path writer not skipped");
    Check(!missingWriterReturned, "missing path write returns false");
    Check(!std::filesystem::exists(missingPath), "missing path remains absent");

    const std::filesystem::path fakeWavPath = fixture.path() / "fake.wav";
    const std::vector<uint8_t> fakeBytes = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    Check(GuardedWriteAllBytes(fakeWavPath, fakeBytes), "fake wav fixture written");
    bool fakeWriterReturned = false;
    Check(GuardedWriteAlbumArt(fakeWavPath, true, &fakeWriterReturned), "fake wav writer not skipped");
    Check(!fakeWriterReturned, "non-WAVE .wav write returns false");
    std::vector<uint8_t> fakeAfter;
    Check(GuardedReadAllBytes(fakeWavPath, &fakeAfter) && fakeAfter == fakeBytes, "fake wav bytes unchanged");

    const std::filesystem::path localPath = fixture.path() / "local.wav";
    const std::filesystem::path networkPath = fixture.path() / "network.wav";
    Check(GuardedWriteAllBytes(localPath, wavBytes), "local wav fixture written");
    Check(GuardedWriteAllBytes(networkPath, wavBytes), "network wav fixture written");
    const std::string localUtf8 = PathUtf8(localPath);
    const std::string networkUtf8 = PathUtf8(networkPath);
    Platform::FileSystem::Testing::NetworkPathForceScript forceScript;
    forceScript.prefix = networkUtf8;
    Platform::FileSystem::Testing::SetNetworkPathForceScript(&forceScript);
    std::vector<std::string> succeeded;
    bool localWriterReturned = false;
    bool networkWriterReturned = false;
    const bool localAttempted = GuardedWriteAlbumArt(localPath, true, &localWriterReturned);
    const bool networkAttempted = GuardedWriteAlbumArt(networkPath, true, &networkWriterReturned);
    const bool localOk = localAttempted && localWriterReturned;
    const bool networkOk = networkAttempted && networkWriterReturned;
    Check(localAttempted, "local wav writer not skipped");
    Check(networkAttempted, "network wav writer not skipped");
    if (localOk) {
        succeeded.push_back(localUtf8);
    }
    if (networkOk) {
        succeeded.push_back(networkUtf8);
    }
    Platform::FileSystem::Testing::SetNetworkPathForceScript(nullptr);
    Check(localOk, "local wav write succeeded");
    Check(!networkOk, "forced network path write returned false");
    Check(succeeded.size() == 1 && succeeded[0] == localUtf8,
          "succeeded set contains only the local path");
    std::vector<uint8_t> networkAfter;
    Check(GuardedReadAllBytes(networkPath, &networkAfter) && networkAfter == wavBytes,
          "network-forced wav bytes unchanged");
    Check(forceScript.calls >= 1, "network hook observed the forced path");

    bool sidecarFound = false;
    if (fixture.Reverify()) {
        for (const auto& entry : std::filesystem::directory_iterator(fixture.path())) {
            if (entry.path().extension() == ".jpg") {
                sidecarFound = true;
            }
        }
    } else {
        Check(false, "fixture re-verification failed before sidecar scan");
    }
    Check(!sidecarFound, "fixture dir has no stem.jpg sidecar");

    const std::filesystem::path rejectPath = fixture.path() / "reject-guard.wav";
    Check(GuardedWriteAllBytes(rejectPath, wavBytes), "reject-guard fixture written");
    g_forceReparseDecision = true;
    g_forceReparseTarget = rejectPath;
    g_writeCalls = 0;
    g_readCalls = 0;
    Check(!GuardedWriteAlbumArt(rejectPath, true, nullptr), "forced reparse skips writer");
    Check(g_writeCalls == 0, "forced reparse does not call writer");
    AlbumArt skippedArt;
    Check(!GuardedReadAlbumArt(rejectPath, &skippedArt, nullptr), "forced reparse skips reader");
    Check(g_readCalls == 0, "forced reparse does not call reader");
    g_forceReparseDecision = false;
    g_forceReparseTarget.clear();

    Check(fixture.CleanupNow(), "unique fixture cleaned before summary");

    std::printf("\n=== Original Audio Write Confirm IO: %d failure(s) ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

