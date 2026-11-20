#include "src/DawBridgeInstaller.h"
#include "src/ReaperBridgeClient.h"
#include "src/ReaperBridgeProtocol.h"
#include "src/ReaperBridgeStatus.h"
#include "src/platform/pe_inspection.h"
#include "reaper_extension/reaper_plugin.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct MediaTrack { int id = 1; };
struct MediaItem {
    double position = 0.0;
    double length = 1.0;
    MediaTrack* track = nullptr;
};

static int failures = 0;
static void check(bool condition, const char* message) {
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
}

static std::string sha(const fs::path& path) {
    return HarkenSearch::DawBridge::ProbeBridgeBinary(path.u8string()).sha256;
}

static void TestProtocolAndPlacementModel() {
    using namespace HarkenSearch::ReaperBridge;
    std::vector<std::string> paths = {
        u8"E:\\素材 库\\第一条 | hit.wav",
        u8"E:\\素材 库\\第二条 # tail.wav",
    };
    std::string message;
    std::string error;
    check(BuildInsertBatchRequest(paths, message, error),
          "v3 batch request accepts Chinese, spaces, and pipe characters");
    std::vector<std::string> parsed;
    check(ParseInsertBatchRequest(message, parsed, error) && parsed == paths,
          "length-prefixed batch request round-trips exact UTF-8 paths");

    const auto placement = EvaluatePlacement(30.0, {{28.0, 31.0}});
    check(placement.valid && placement.correction == 2.0 &&
              placement.actualStart >= 30.0 && placement.nextCursor == 33.0,
          "placement model corrects an item that REAPER inserted before 30s");
    check(!EvaluatePlacement(30.0, {}).valid,
          "placement model rejects success without a discoverable item");
    check(!EvaluatePlacement(30.0, {{30.0, 30.0}}).valid,
          "placement model rejects a zero-duration inserted item");

    const auto hello = ParseHelloResponse(BuildHelloResponse(62001));
    check(hello.code == HandshakeCode::Ready &&
              hello.protocolVersion == kProtocolVersion &&
              hello.manifest == kBridgeManifest,
          "handshake binds version, protocol, architecture, and manifest");
}

static void TestStateModel() {
    using namespace HarkenSearch::DawBridge;
    ReaperBridgeProbeFacts facts;
    check(EvaluateReaperBridgeState(facts) == InstallState::Unconfigured,
          "empty REAPER path is explicitly unconfigured");
    facts.configured = true;
    check(EvaluateReaperBridgeState(facts) == InstallState::ReaperPathMissing,
          "missing configured root is explicit");
    facts.configuredPathExists = true;
    check(EvaluateReaperBridgeState(facts) == InstallState::ReaperExecutableMissing,
          "configured root without reaper.exe is explicit");
    facts.hostExecutableExists = true;
    facts.targetResolved = true;
    facts.bundledFilePresent = true;
    facts.hostRunning = true;
    check(EvaluateReaperBridgeState(facts) == InstallState::WaitingForHostExit,
          "missing bridge while REAPER runs becomes wait-for-exit");
    facts.filePresentAtTarget = true;
    facts.binaryIntegrityOk = true;
    facts.trustedSource = true;
    facts.installedVersion = HarkenSearch::ReaperBridge::kBridgeVersion;
    facts.expectedVersion = HarkenSearch::ReaperBridge::kBridgeVersion;
    facts.installedProtocolVersion = HarkenSearch::ReaperBridge::kProtocolVersion;
    facts.installedArchitecture = HarkenSearch::ReaperBridge::CurrentArchitecture();
    facts.hostArchitecture = HarkenSearch::ReaperBridge::CurrentArchitecture();
    facts.handshakeCode = HarkenSearch::ReaperBridge::HandshakeCode::Ready;
    check(EvaluateReaperBridgeState(facts) == InstallState::HandshakeReady,
          "only a compatible live handshake is send-ready");
}

static MediaTrack g_track;
static std::vector<std::unique_ptr<MediaItem>> g_items;
static double g_cursor = 30.0;
static void (*g_timer)() = nullptr;

static int MockInsertMedia(const char* path, int) {
    if (std::string(path).find("fail") != std::string::npos) return 0;
    auto item = std::make_unique<MediaItem>();
    item->position = g_cursor - 2.0; // reproduce the reported pre-cursor placement
    item->length = std::string(path).find("second") != std::string::npos ? 2.0 : 1.5;
    item->track = &g_track;
    g_items.push_back(std::move(item));
    return 1;
}
static double MockGetCursorPosition() { return g_cursor; }
static void MockSetEditCurPos(double value, bool, bool) { g_cursor = value; }
static int MockCountMediaItems(ReaProject*) { return static_cast<int>(g_items.size()); }
static MediaItem* MockGetMediaItem(ReaProject*, int index) {
    return index >= 0 && index < static_cast<int>(g_items.size())
        ? g_items[static_cast<std::size_t>(index)].get() : nullptr;
}
static double MockGetMediaItemInfoValue(MediaItem* item, const char* key) {
    if (std::strcmp(key, "D_POSITION") == 0) return item->position;
    if (std::strcmp(key, "D_LENGTH") == 0) return item->length;
    return 0.0;
}
static bool MockSetMediaItemInfoValue(MediaItem* item, const char* key, double value) {
    if (std::strcmp(key, "D_POSITION") != 0) return false;
    item->position = value;
    return true;
}
static MediaTrack* MockGetMediaItemTrack(MediaItem* item) { return item->track; }
static bool MockDeleteTrackMediaItem(MediaTrack*, MediaItem* item) {
    const auto found = std::find_if(g_items.begin(), g_items.end(),
        [&](const auto& value) { return value.get() == item; });
    if (found == g_items.end()) return false;
    g_items.erase(found);
    return true;
}
static int MockCountTracks(ReaProject*) { return 1; }
static MediaTrack* MockGetTrack(ReaProject*, int index) { return index == 0 ? &g_track : nullptr; }
static void MockDeleteTrack(MediaTrack*) {}
static void MockVoid() {}
static void MockUndoEnd(const char*, int) {}
static void MockConsole(const char*) {}

static void* MockGetFunc(const char* name) {
#define MATCH_API(api, function) if (std::strcmp(name, api) == 0) return reinterpret_cast<void*>(function)
    MATCH_API("InsertMedia", MockInsertMedia);
    MATCH_API("GetCursorPosition", MockGetCursorPosition);
    MATCH_API("SetEditCurPos", MockSetEditCurPos);
    MATCH_API("CountMediaItems", MockCountMediaItems);
    MATCH_API("GetMediaItem", MockGetMediaItem);
    MATCH_API("GetMediaItemInfo_Value", MockGetMediaItemInfoValue);
    MATCH_API("SetMediaItemInfo_Value", MockSetMediaItemInfoValue);
    MATCH_API("GetMediaItemTrack", MockGetMediaItemTrack);
    MATCH_API("DeleteTrackMediaItem", MockDeleteTrackMediaItem);
    MATCH_API("CountTracks", MockCountTracks);
    MATCH_API("GetTrack", MockGetTrack);
    MATCH_API("DeleteTrack", MockDeleteTrack);
    MATCH_API("UpdateArrange", MockVoid);
    MATCH_API("Undo_BeginBlock", MockVoid);
    MATCH_API("Undo_EndBlock", MockUndoEnd);
    MATCH_API("ShowConsoleMsg", MockConsole);
#undef MATCH_API
    return nullptr;
}

static int MockRegister(const char* name, void* value) {
    if (std::strcmp(name, "command_id") == 0) return 62001;
    if (std::strcmp(name, "timer") == 0) g_timer = reinterpret_cast<void (*)()>(value);
    return 1;
}

static std::wstring UniquePipeName() {
    return L"\\\\.\\pipe\\HarkenSearchReaperBridgeV3Test_" +
           std::to_wstring(GetCurrentProcessId());
}

static fs::path TempRoot(const char* suffix) {
    return fs::temp_directory_path() /
        (std::string("aris_reaper_v3_") + suffix + "_" +
         std::to_string(GetCurrentProcessId()));
}

static bool PumpUntil(std::future<HarkenSearch::ReaperBridge::BatchSendResult>& future) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (g_timer) g_timer();
        if (future.wait_for(std::chrono::milliseconds(5)) == std::future_status::ready) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

static void TestBuiltExtensionPlacementTransaction() {
#ifdef ARIS_REAPER_TEST_DLL_PATH
    const fs::path root = TempRoot("host");
    fs::remove_all(root);
    fs::create_directories(root / fs::u8path(u8"中文 空格"));
    const fs::path first = root / fs::u8path(u8"中文 空格/first (hit).wav");
    const fs::path second = root / fs::u8path(u8"中文 空格/second # tail.wav");
    const fs::path fail = root / fs::u8path(u8"中文 空格/fail.wav");
    std::ofstream(first, std::ios::binary).put('\0');
    std::ofstream(second, std::ios::binary).put('\0');
    std::ofstream(fail, std::ios::binary).put('\0');

    const std::wstring pipe = UniquePipeName();
    SetEnvironmentVariableW(L"ARIS_REAPER_BRIDGE_PIPE_NAME", pipe.c_str());
    HMODULE module = LoadLibraryW(fs::u8path(ARIS_REAPER_TEST_DLL_PATH).c_str());
    check(module != nullptr, "built versioned reaper_harken.dll loads");
    if (!module) return;
    const auto exportResult = Platform::PeInspection::ProbeExport(
        ARIS_REAPER_TEST_DLL_PATH, "ReaperPluginEntry");
    check(exportResult.status == Platform::PeInspection::Status::Success &&
              exportResult.present,
          "PE export PAL finds ReaperPluginEntry in the test extension");
    using Entry = int (*)(HINSTANCE, reaper_plugin_info_t*);
    auto entry = reinterpret_cast<Entry>(GetProcAddress(module, "ReaperPluginEntry"));
    reaper_plugin_info_t host = {};
    host.caller_version = 0x20E;
    host.Register = MockRegister;
    host.GetFunc = MockGetFunc;
    check(entry && entry(module, &host) == 1 && g_timer,
          "extension accepts a compatible mocked REAPER host");

    HarkenSearch::ReaperBridge::HandshakeResult handshake;
    for (int attempt = 0; attempt < 100; ++attempt) {
        handshake = HarkenSearch::ReaperBridge::ProbeHandshake(pipe, 100);
        if (handshake.code == HarkenSearch::ReaperBridge::HandshakeCode::Ready) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(handshake.code == HarkenSearch::ReaperBridge::HandshakeCode::Ready,
          "built extension completes the v3 handshake");

    g_cursor = 30.0;
    g_items.clear();
    auto send = std::async(std::launch::async, [&] {
        return HarkenSearch::ReaperBridge::SendFiles(
            {first.u8string(), second.u8string()}, {}, pipe, 3000);
    });
    check(PumpUntil(send), "batch send receives a bounded response");
    const auto sent = send.get();
    if (!(sent.ok && sent.sent == 2 && g_items.size() == 2)) {
        std::printf("TRACE batch ok=%d sent=%d total=%d items=%zu detail=%s\n",
                    sent.ok ? 1 : 0, sent.sent, sent.total, g_items.size(), sent.detail.c_str());
    }
    check(sent.ok && sent.sent == 2 && g_items.size() == 2,
          "single batch inserts both files exactly once");
    if (g_items.size() == 2) {
        check(g_items[0]->position >= 30.0,
              "first item start is not before the pre-send 30s edit cursor");
        check(g_items[1]->position >= g_items[0]->position + g_items[0]->length,
              "second item starts at or after the first item end");
        check(g_cursor >= g_items[1]->position + g_items[1]->length,
              "edit cursor advances only to the final inserted item end");
    }

    g_cursor = 30.0;
    g_items.clear();
    auto failed = std::async(std::launch::async, [&] {
        return HarkenSearch::ReaperBridge::SendFiles(
            {first.u8string(), fail.u8string()}, {}, pipe, 3000);
    });
    check(PumpUntil(failed), "failed batch receives a bounded response");
    const auto failedResult = failed.get();
    check(!failedResult.ok && g_items.empty() && g_cursor == 30.0,
          "mid-batch failure removes inserted items and restores the edit cursor");

    entry(module, nullptr);
    FreeLibrary(module);
    SetEnvironmentVariableW(L"ARIS_REAPER_BRIDGE_PIPE_NAME", nullptr);
    fs::remove_all(root);
#else
    check(false, "ARIS_REAPER_TEST_DLL_PATH is provided");
#endif
}

static void TestDoctorTransaction() {
#ifdef ARIS_REAPER_TEST_DLL_PATH
    using namespace HarkenSearch::DawBridge;
    const fs::path source = fs::u8path(ARIS_REAPER_TEST_DLL_PATH);
    const auto sourceInfo = ProbeBridgeBinary(source.u8string());
    check(sourceInfo.exists && sourceInfo.trustedSource &&
              sourceInfo.version == HarkenSearch::ReaperBridge::kBridgeVersion &&
              sourceInfo.protocolVersion == HarkenSearch::ReaperBridge::kProtocolVersion &&
              sourceInfo.manifest == HarkenSearch::ReaperBridge::kBridgeManifest &&
              !sourceInfo.sha256.empty(),
          "built bridge exposes trusted version/protocol/manifest/SHA-256 metadata");

    const fs::path root = TempRoot("doctor");
    fs::remove_all(root);
    fs::create_directories(root / "UserPlugins");
    std::ofstream(root / "reaper.ini") << "[reaper]\n";
    wchar_t testExe[32768] = {};
    GetModuleFileNameW(nullptr, testExe, static_cast<DWORD>(std::size(testExe)));
    fs::copy_file(testExe, root / "reaper.exe", fs::copy_options::overwrite_existing);

    ReaperBridgeProbeOptions options;
    options.configuredReaperRootPath = root.u8string();
    options.bundledDllPathOverride = source.u8string();
    options.hostRunningOverride = false;
    auto status = ProbeReaperBridge(options);
    check((status.state == InstallState::NotInstalled ||
           status.state == InstallState::WrongLocation) &&
              status.targetResolution == ReaperBridgeTargetResolution::ConfiguredPortableInstall,
          "Doctor resolves a configured portable REAPER root and reports any alternate install");

    const auto installed = InstallReaperBridge(options, false);
    const fs::path target = root / "UserPlugins" / "reaper_harken.dll";
    check(installed.ok && installed.changed && sha(target) == sourceInfo.sha256,
          "Doctor installs and verifies the packaged bridge");

    std::ofstream(target, std::ios::binary | std::ios::app).put('\0');
    const std::string corruptHash = sha(target);
    options.hostRunningOverride = true;
    const auto blocked = InstallReaperBridge(options, false);
    check(!blocked.ok && blocked.failureCode == "reaper-close-required" &&
              sha(target) == corruptHash,
          "running REAPER blocks replacement without changing the target");

    options.hostRunningOverride = false;
    const auto updated = InstallReaperBridge(options, false);
    check(updated.ok && updated.changed && !updated.backupPath.empty() &&
              fs::exists(fs::u8path(updated.backupPath)) && sha(target) == sourceInfo.sha256,
          "old/corrupt known bridge is backed up and atomically updated");

    std::ofstream(target, std::ios::binary | std::ios::app).put('\1');
    const std::string rollbackSourceHash = sha(target);
    const auto rolledBack = InstallReaperBridge(
        options, false,
        [](ReaperBridgeInstallCheckpoint checkpoint) {
            return checkpoint != ReaperBridgeInstallCheckpoint::ReplacementCommitted;
        });
    check(!rolledBack.ok && rolledBack.rollbackPerformed && rolledBack.rollbackVerified &&
              sha(target) == rollbackSourceHash,
          "verification interruption rolls back to the byte-exact previous bridge");

    std::atomic<int> hostQueries{0};
    options.hostRunningQueryOverride = [&] {
        return hostQueries.fetch_add(1) < 2;
    };
    ReaperBridgeDoctorRequest waitRequest;
    waitRequest.action = ReaperBridgeDoctorAction::Update;
    waitRequest.probeOptions = options;
    waitRequest.waitForHostExit = true;
    waitRequest.hostExitWaitTimeoutMs = 1000;
    waitRequest.recheckIntervalMs = 25;
    const auto continued = RunReaperBridgeDoctor(waitRequest);
    check(continued.ok && continued.changed && continued.binaryVerified &&
              sha(target) == sourceInfo.sha256 &&
              std::find(continued.steps.begin(), continued.steps.end(),
                        "waiting_for_reaper_exit") != continued.steps.end() &&
              std::find(continued.steps.begin(), continued.steps.end(),
                        "reaper_exit_detected") != continued.steps.end(),
          "Doctor waits for REAPER exit and automatically continues the update");
    options.hostRunningQueryOverride = {};

    const std::string summary = BuildReaperBridgeDiagnosticSummary(ProbeReaperBridge(options), true);
    check(summary.find("plugin_sha256=") != std::string::npos &&
              summary.find("expected_protocol=3") != std::string::npos,
          "redacted diagnostic summary exposes hash and protocol evidence");
    fs::remove_all(root);
#else
    check(false, "ARIS_REAPER_TEST_DLL_PATH is provided for Doctor tests");
#endif
}

static std::string WideArgToUtf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

static int RunRealDoctorUpdate(int argc, wchar_t** argv, bool installMissingOnly) {
#ifdef ARIS_REAPER_TEST_DLL_PATH
    if (argc != 4) {
        std::fprintf(stderr, "usage: --doctor-update|--doctor-install-missing <reaper-root> <bundled-dll>\n");
        return 64;
    }
    HarkenSearch::DawBridge::ReaperBridgeDoctorRequest request;
    request.action = HarkenSearch::DawBridge::ReaperBridgeDoctorAction::Update;
    request.installMissingOnly = installMissingOnly;
    request.probeOptions.configuredReaperRootPath = WideArgToUtf8(argv[2]);
    request.probeOptions.bundledDllPathOverride = WideArgToUtf8(argv[3]);
    const auto report = HarkenSearch::DawBridge::RunReaperBridgeDoctor(request);
    std::printf("doctor_ok=%d changed=%d binary_verified=%d failure=%s installed=%s sha256=%s\n",
                report.ok ? 1 : 0,
                report.changed ? 1 : 0,
                report.binaryVerified ? 1 : 0,
                report.failureCode.c_str(),
                report.install.installedPath.c_str(),
                report.after.installedSha256.c_str());
    return report.ok && report.binaryVerified ? 0 : 2;
#else
    (void)argc;
    (void)argv;
    (void)installMissingOnly;
    return 3;
#endif
}

static int RunRealSend(int argc, wchar_t** argv) {
    const bool customPipe = argc > 1 && std::wcscmp(argv[1], L"--send-pipe") == 0;
    const int firstPathArgument = customPipe ? 3 : 2;
    if (argc <= firstPathArgument) {
        std::fprintf(stderr,
                     "usage: --send <file> [file ...] | "
                     "--send-pipe <pipe-name> <file> [file ...]\n");
        return 64;
    }
    std::wstring pipeName = HarkenSearch::ReaperBridge::kDefaultPipeName;
    if (customPipe) {
        pipeName = argv[2];
    }
    std::vector<std::string> paths;
    for (int index = firstPathArgument; index < argc; ++index) {
        paths.emplace_back(WideArgToUtf8(argv[index]));
    }
    const auto result = HarkenSearch::ReaperBridge::SendFiles(paths, {}, pipeName, 5000);
    std::printf("send_ok=%d sent=%d total=%d detail=%s\n",
                result.ok ? 1 : 0, result.sent, result.total, result.detail.c_str());
    return result.ok && result.sent == result.total ? 0 : 2;
}

int RunDawIntegrationSettingsChecks(const fs::path& root);

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wcscmp(argv[1], L"--doctor-update") == 0) {
        return RunRealDoctorUpdate(argc, argv, false);
    }
    if (argc > 1 && std::wcscmp(argv[1], L"--doctor-install-missing") == 0) {
        return RunRealDoctorUpdate(argc, argv, true);
    }
    if (argc > 1 && (std::wcscmp(argv[1], L"--send") == 0 ||
                     std::wcscmp(argv[1], L"--send-pipe") == 0)) {
        return RunRealSend(argc, argv);
    }
    TestProtocolAndPlacementModel();
    TestStateModel();
    TestBuiltExtensionPlacementTransaction();
    TestDoctorTransaction();
    const auto settingsRoot = TempRoot("settings");
    if (fs::create_directory(settingsRoot)) {
        failures += RunDawIntegrationSettingsChecks(settingsRoot);
        for (const auto* fixture : {"staged.dll", "existing.dll", "empty.dll"}) {
            fs::remove(settingsRoot / fixture);
        }
        fs::remove(settingsRoot);
    } else {
        std::fprintf(stderr, "DAW integration scratch directory already exists\n");
        ++failures;
    }
    std::printf("\nREAPER bridge contract failures: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
