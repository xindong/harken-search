// Cocoa owns the window and OpenGL context. Product state, rendering, settings,
// audio and worker lifetimes use the same services as the Windows entry.
// Carbon's historical FolderInfo collides with the shared database type.
// Confine that SDK spelling to this native translation unit.
#define FolderInfo MacFinderFolderInfo
#import <Cocoa/Cocoa.h>
#undef FolderInfo
#import <OpenGL/gl.h>
#include "imgui.h"
#include "imgui_impl_osx.h"
#include "imgui_impl_opengl3.h"
#include "../AppRuntime.h"
#include "../AppFrameServices.h"
#include "../AppMainServices.h"
#include "../UIRenderer.h"
#include "../UiFonts.h"
#include "../UiHostServices.h"
#include "../WindowsMain.h"
#include "../FileInfoPanel.h"
#include "../Theme.h"
#include "../ThemeSchedule.h"
#include "../ThemeInspector.h"
#include "../ModuleUI.h"
#include "../CoverModule.h"
#include "../DiagnosticsLogger.h"
#include "../UserActionLog.h"
#include "../MemoryBudget.h"
#include "../WorkerThreadPolicy.h"
#include "../Utils.h"
#include "harken_user_data_layout.h"
#include "theme_custom_file.h"
#include "macos_qa_bridge.h"
#include "filesystem.h"
#include "system.h"
#include <algorithm>
#include <fstream>
#include <memory>
#include <filesystem>
#include <cstring>
#include <cstdlib>

extern std::string g_imguiIniPathUtf8;
extern std::string g_appSettingsPathUtf8;
namespace {
NSWindow* mainWindow = nil;
bool qaSmoke = false;
bool qaControl = false;
int applicationExitCode = 0;
std::ofstream runtimeLog;
void RecordStage(const char* stage) {
    if (runtimeLog) {
        runtimeLog << "{\"event\":\"" << stage << "\",\"monotonic_ms\":"
                   << Platform::MonotonicMilliseconds() << "}\n";
        runtimeLog.flush();
    }
    DIAG_SYSTEM("MacRuntime", stage);
}
std::filesystem::path QaOutput() {
    return std::filesystem::path(Platform::HarkenUserData::PreviewIsolationRootUtf8()) / "runtime";
}
int SnapMacFontSizePx(int requested) {
    requested = std::clamp(requested, 10, 24);
    int closest = kFontSteps[0];
    for (int step : kFontSteps) {
        if (std::abs(step - requested) < std::abs(closest - requested)) closest = step;
    }
    return closest;
}
}
void* HarkenSearch::UiHost::MainWindowOwner() noexcept { return (__bridge void*)mainWindow; }

@interface HarkenOpenGLView : NSOpenGLView <NSDraggingDestination>
- (BOOL)acceptFolderURLs:(NSArray<NSURL*>*)urls;
- (BOOL)acceptDatabaseURL:(NSURL*)url;
- (BOOL)acceptAudioPaths:(NSArray<NSString*>*)paths;
@end
@implementation HarkenOpenGLView
- (BOOL)acceptsFirstResponder { return YES; }
- (NSArray<NSString*>*)validatedFolderPaths:(NSArray<NSURL*>*)urls {
    if (urls.count == 0 || urls.count > 20000) return nil;
    NSMutableArray<NSString*>* paths = [NSMutableArray arrayWithCapacity:urls.count];
    NSMutableSet<NSString*>* seen = [NSMutableSet setWithCapacity:urls.count];
    for (NSURL* url in urls) {
        if (![url isFileURL]) return nil;
        NSString* path = url.path.stringByStandardizingPath;
        BOOL directory = NO;
        if (path.length == 0 || ![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] ||
            !directory || !path.UTF8String) return nil;
        if (![seen containsObject:path]) {
            [seen addObject:path];
            [paths addObject:path];
        }
    }
    return paths;
}
- (NSString*)validatedDatabasePath:(NSArray<NSURL*>*)urls {
    if (urls.count != 1 || !urls.firstObject.isFileURL) return nil;
    NSString* path = urls.firstObject.path.stringByStandardizingPath;
    BOOL directory = NO;
    if (path.length == 0 || ![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] ||
        directory || !path.UTF8String ||
        ![[path.pathExtension lowercaseString] isEqualToString:@"db"]) return nil;
    return path;
}
- (NSArray<NSString*>*)validatedAudioPaths:(NSArray<NSURL*>*)urls {
    // Match the Windows direct-file drop bound; larger drops need a separate
    // confirmed folder-scan flow rather than blocking this Cocoa callback.
    if (urls.count == 0 || urls.count > 200) return nil;
    NSMutableArray<NSString*>* paths = [NSMutableArray arrayWithCapacity:urls.count];
    NSMutableSet<NSString*>* seen = [NSMutableSet setWithCapacity:urls.count];
    for (NSURL* url in urls) {
        if (!url.isFileURL) return nil;
        NSString* path = url.path.stringByStandardizingPath;
        BOOL directory = NO;
        if (path.length == 0 ||
            ![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] ||
            directory || !path.UTF8String) return nil;
        NSString* extension = path.pathExtension.lowercaseString;
        if (![@[@"wav", @"mp3", @"flac", @"ogg", @"aiff", @"aif", @"m4a", @"wma"]
                containsObject:extension]) return nil;
        if (![seen containsObject:path]) {
            [seen addObject:path];
            [paths addObject:path];
        }
    }
    return paths;
}
- (NSArray<NSURL*>*)fileURLsFromDraggingInfo:(id<NSDraggingInfo>)sender {
    NSPasteboard* pasteboard = [sender draggingPasteboard];
    NSArray<NSURL*>* urls = [pasteboard readObjectsForClasses:@[[NSURL class]]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    return urls.count > 0 && urls.count <= 20000 ? urls : nil;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    if (!g_appState || !([sender draggingSourceOperationMask] & NSDragOperationCopy))
        return NSDragOperationNone;
    NSArray<NSURL*>* urls = [self fileURLsFromDraggingInfo:sender];
    if ([self validatedDatabasePath:urls] ||
        (g_appState->database.isOpen() && !g_appState->dbSwitchInProgress &&
         !g_appState->isScanning.load() && !g_appState->scanCompletionPending.load() &&
         [self validatedAudioPaths:urls]) ||
        (g_appState->database.isOpen() && [self validatedFolderPaths:urls]))
        return NSDragOperationCopy;
    return NSDragOperationNone;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* urls = [self fileURLsFromDraggingInfo:sender];
    if (!urls) return NO;
    if ([self validatedDatabasePath:urls]) return [self acceptDatabaseURL:urls.firstObject];
    NSArray<NSString*>* audioPaths = [self validatedAudioPaths:urls];
    if (audioPaths) return [self acceptAudioPaths:audioPaths];
    return [self acceptFolderURLs:urls];
}
- (BOOL)acceptDatabaseURL:(NSURL*)url {
    if (!g_appState) return NO;
    NSString* path = [self validatedDatabasePath:@[url]];
    if (!path || !BeginSwitchDatabase(*g_appState, path.UTF8String)) return NO;
    g_appState->lastDropFirstPath = path.UTF8String;
    g_appState->lastDropTick = Platform::MonotonicMilliseconds();
    g_appState->lastDropSummary = "Drop received: database=1";
    return YES;
}
- (BOOL)acceptAudioPaths:(NSArray<NSString*>*)paths {
    if (!g_appState) return NO;
    AppState& state = *g_appState;
    if (!state.database.isOpen() || state.dbSwitchInProgress ||
        state.isScanning.load() || state.scanCompletionPending.load() || paths.count == 0)
        return NO;
    state.scanDropImportedCount = 0;
    state.scanDropExistingCount = 0;
    state.scanDropFailedCount = 0;
    std::string firstPlayablePath;
    for (NSString* path in paths) {
        const std::string filePath(path.UTF8String);
        try {
            if (state.database.fileExists(filePath)) {
                if (state.database.markFileAsManuallyImported(filePath)) {
                    ++state.scanDropExistingCount;
                    if (firstPlayablePath.empty()) firstPlayablePath = filePath;
                } else {
                    ++state.scanDropFailedCount;
                }
            } else {
                AudioRecord record;
                if (Database::readAudioRecordMetadataSnapshot(filePath, record) &&
                    state.database.addManuallyImportedFile(record)) {
                    ++state.scanDropImportedCount;
                    if (firstPlayablePath.empty()) firstPlayablePath = filePath;
                } else {
                    ++state.scanDropFailedCount;
                }
            }
        } catch (...) {
            ++state.scanDropFailedCount;
        }
    }
    if (state.scanDropImportedCount > 0) RefreshSearchResults(state, true);
    state.lastDropFirstPath = paths.firstObject.UTF8String;
    state.lastDropTick = Platform::MonotonicMilliseconds();
    state.lastDropSummary = "Drop received: folders=0, audioFiles=" + std::to_string(paths.count);
    state.showScanDropNotice = true;
    if (!firstPlayablePath.empty()) {
        state.audioEngine.loadFileAsync(firstPlayablePath, true);
        ScheduleDeferredWaveformDetail(state, firstPlayablePath, AppState::WAVEFORM_DEFAULT_DETAIL_PEAKS);
        ResetWaveformZoom(state);
    }
    return YES;
}
- (BOOL)acceptFolderURLs:(NSArray<NSURL*>*)urls {
    if (!g_appState || !g_appState->database.isOpen()) return NO;
    NSArray<NSString*>* paths = [self validatedFolderPaths:urls];
    if (!paths) return NO;
    AppState& state = *g_appState;
    if (state.isScanning.load() || state.scanCompletionPending.load()) {
        state.scanDialogAutoStart = false;
        state.scanIgnoredDropCount = static_cast<int>(paths.count);
        state.showScanBusyNotice = true;
        return YES;
    }
    state.pendingScanFolders.clear();
    state.pendingScanFolders.reserve(paths.count);
    for (NSString* path in paths) state.pendingScanFolders.emplace_back(path.UTF8String);
    state.lastDropFirstPath = state.pendingScanFolders.front();
    state.lastDropTick = Platform::MonotonicMilliseconds();
    state.lastDropSummary = "Drop received: folders=" + std::to_string(state.pendingScanFolders.size()) +
        ", audioFiles=0, recursive=" + (state.scanRecursive ? "on" : "off");
    state.scanDialogAutoStart = false;
    state.showScanDialog = true;
    return YES;
}
- (void)prepareOpenGL {
    [super prepareOpenGL];
    GLint swapInterval = 1;
    [self.openGLContext setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];
}
@end

#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
@interface HarkenQaDraggingInfo : NSObject
@property (nonatomic, strong) NSPasteboard* qaPasteboard;
@end
@implementation HarkenQaDraggingInfo
- (NSPasteboard*)draggingPasteboard { return self.qaPasteboard; }
- (NSDragOperation)draggingSourceOperationMask { return NSDragOperationCopy; }
@end
#endif

@interface HarkenAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    std::unique_ptr<AppState> state;
    bool imguiReady;
    bool osxReady;
    bool rendererReady;
    bool settingsLoaded;
    bool closing;
    int frameCount;
    int loadedReadingFontPx;
    int loadedListFontPx;
}
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) HarkenOpenGLView* glView;
@property (nonatomic, strong) NSTimer* renderTimer;
@end

@implementation HarkenAppDelegate
- (void)failStartup:(NSString*)message {
    applicationExitCode = 1;
    fprintf(stderr, "Harken Search startup failed: %s\n", message.UTF8String);
    RecordStage("StartupFailed");
    [self shutdown];
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    const auto layout = Platform::HarkenUserData::EnsureLayout();
    if (layout.readOnly || layout.activeSettingsDir.empty()) {
        [self failStartup:@"数据目录不可用，未改用其他目录。"];
        return;
    }
    const auto settingsDir = std::filesystem::path(Platform::HarkenUserData::WideToUtf8Path(layout.activeSettingsDir));
    g_appSettingsPathUtf8 = (settingsDir / "settings.ini").string();
    g_imguiIniPathUtf8 = (settingsDir / "imgui.ini").string();
    if (qaSmoke || qaControl) {
        const auto output = QaOutput();
        if (!Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(output.string()) ||
            !Platform::FileSystem::EnsureDirectoryExists(output.string())) {
            [self failStartup:@"QA 输出目录不可用。"];
            return;
        }
        runtimeLog.open(output / "events.jsonl", std::ios::app);
        if (!runtimeLog) { [self failStartup:@"无法写入 QA 运行记录。"]; return; }
    }
    RecordStage("StartupBegin");
    DiagnosticsLogger::instance().setOutputPath((settingsDir / "diagnostics.json").string());
    UserActionLog::instance().setOutputPath((settingsDir / "actions.jsonl").string());
    SetDiagnosticFileEnabled(true);

    NSRect frame = NSMakeRect(80, 80, 1280, 800);
    self.window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.releasedWhenClosed = NO;
    self.window.title = @"Harken Search";
    self.window.delegate = self;
    mainWindow = self.window;
    NSOpenGLPixelFormatAttribute attributes[] = {
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
        NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8, NSOpenGLPFADepthSize, 24,
        NSOpenGLPFAStencilSize, 8, NSOpenGLPFAAccelerated, 0
    };
    NSOpenGLPixelFormat* format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
    if (!format) { [self failStartup:@"OpenGL 3.2 上下文不可用。"]; return; }
    self.glView = [[HarkenOpenGLView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height) pixelFormat:format];
    self.glView.wantsBestResolutionOpenGLSurface = YES;
    [self.glView registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    self.window.contentView = self.glView;
    [self.glView.openGLContext makeCurrentContext];
    if (!self.glView.openGLContext) { [self failStartup:@"无法创建 OpenGL 视图。"]; return; }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imguiReady = true;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    // Native multi-window docking remains a separate Mac behavior card.
    io.IniFilename = g_imguiIniPathUtf8.c_str();
    state = std::make_unique<AppState>();
    g_appState = state.get();
    state->panelSimilarSounds.visible = false;
    if (!InitializeAppSettingsAndAudio(*state)) { [self failStartup:@"音频设备初始化失败。"]; return; }
    settingsLoaded = true;
    loadedReadingFontPx = SnapMacFontSizePx(state->settings.readingFontSize);
    loadedListFontPx = SnapMacFontSizePx(state->settings.listFontSize);
    LoadChineseFont(io, static_cast<float>(kChromeFontPx),
        static_cast<float>(loadedReadingFontPx), static_cast<float>(loadedListFontPx));
    ApplyTheme(state->settings.theme);
    if (state->settings.useCustomColors) ApplyCustomColors(state->settings);
    if (state->settings.theme == AppTheme::Custom && !state->settings.customThemeName.empty()) {
        const auto themePath = Platform::ThemeFiles::CustomThemePathUtf8();
        if (!themePath.empty()) ThemeColors::loadAndApplyThemeFile(themePath);
    }
    harken::theme_schedule::Tick(*state, true);
    osxReady = ImGui_ImplOSX_Init(self.glView);
    rendererReady = ImGui_ImplOpenGL3_Init("#version 150");
    if (!osxReady || !rendererReady) { [self failStartup:@"ImGui 后端初始化失败。"]; return; }
    InitializeModules();
    CoverModule::SetAppState(state.get());
    CoverModule::SetDatabase(&state->database);
    std::string library;
    AppState::StartupDatabaseSetupReason reason;
    if (ResolveStartupDatabaseSelectionForMount(*state, library, reason)) {
        if (!BeginSwitchDatabase(*state, library)) {
            state->startupNeedsDatabaseSetup = true;
            state->startupDatabaseSetupReason = AppState::StartupDatabaseSetupReason::MissingConfiguredDatabase;
        }
    } else {
        state->startupNeedsDatabaseSetup = true;
        state->startupDatabaseSetupReason = reason;
    }
    RecordStage("ProductInitialized");
    if (qaSmoke || qaControl) {
        std::ofstream snapshot(QaOutput() / "startup.txt", std::ios::app);
        snapshot << "settings=" << g_appSettingsPathUtf8 << "\n"
                 << "default_library_directory=" << GetDefaultDbDirUtf8() << "\n"
                 << "reading_font=" << state->settings.readingFontSize << "\n"
                 << "theme=" << static_cast<int>(state->settings.theme) << "\n"
                 << "theme_customized=" << ThemeColors::customized() << "\n"
                 << "theme_bg_red=" << ThemeColors::current().mainWindowBG.x << "\n"
                 << "needs_database=" << state->startupNeedsDatabaseSetup << "\n";
    }
    if (qaSmoke || qaControl) [self.window orderBack:nil];
    else [self.window makeKeyAndOrderFront:nil];
    self.renderTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0 target:self
        selector:@selector(renderFrame) userInfo:nil repeats:YES];
}
- (BOOL)captureFrame {
    NSRect pixels = [self.glView convertRectToBacking:self.glView.bounds];
    const int width = static_cast<int>(pixels.size.width), height = static_cast<int>(pixels.size.height);
    if (width < 1 || height < 1 || width > 16384 || height > 16384) return NO;
    std::vector<unsigned char> raw(static_cast<size_t>(width) * height * 4);
    // Keep earlier renderer errors separate from the framebuffer read result.
    for (GLenum prior = glGetError(); prior != GL_NO_ERROR; prior = glGetError())
        fprintf(stderr, "Mac renderer error before capture: 0x%x\n", prior);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    const GLenum readError = glGetError();
    if (readError != GL_NO_ERROR) {
        fprintf(stderr, "Mac framebuffer read failed: 0x%x\n", readError);
        return NO;
    }
    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil pixelsWide:width pixelsHigh:height
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bytesPerRow:width * 4 bitsPerPixel:32];
    for (int y = 0; y < height; ++y)
        std::memcpy(bitmap.bitmapData + static_cast<size_t>(y) * width * 4,
            raw.data() + static_cast<size_t>(height - 1 - y) * width * 4, static_cast<size_t>(width) * 4);
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    const auto path = (QaOutput() / "main-window.png").string();
    return Platform::HarkenUserData::PreviewIsolationAllowsDataPathUtf8(path) &&
        [png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES];
}
- (void)renderFrame {
    if (closing || !state) return;
    [self.glView.openGLContext makeCurrentContext];
    const int readingPx = SnapMacFontSizePx(state->settings.readingFontSize);
    const int listPx = SnapMacFontSizePx(state->settings.listFontSize);
    if (readingPx != loadedReadingFontPx || listPx != loadedListFontPx) {
        // The atlas and OpenGL texture must be refreshed before ImGui starts a frame.
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        LoadChineseFont(io, static_cast<float>(kChromeFontPx),
            static_cast<float>(readingPx), static_cast<float>(listPx));
        if (!io.Fonts->Build()) {
            [self failStartup:@"无法更新界面字体。"];
            return;
        }
        ImGui_ImplOpenGL3_DestroyDeviceObjects();
        if (!ImGui_ImplOpenGL3_CreateDeviceObjects()) {
            [self failStartup:@"无法更新界面字体纹理。"];
            return;
        }
        loadedReadingFontPx = readingPx;
        loadedListFontPx = listPx;
        RecordStage("FontAtlasRebuilt");
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplOSX_NewFrame(self.glView);
    ImGui::NewFrame();
    ++frameCount;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) MarkUiLeftClickNow();
    if (g_suppressImGuiDragTooltipFrames > 0) --g_suppressImGuiDragTooltipFrames;
    auto qaRequest = qaControl ? HarkenSearch::MacQa::Poll(*state) : HarkenSearch::MacQa::Request{};
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
    if (qaRequest.dropAttempted && qaRequest.error.empty()) {
        NSPasteboard* pasteboard = [NSPasteboard pasteboardWithUniqueName];
        NSMutableArray<NSURL*>* urls = [NSMutableArray arrayWithCapacity:qaRequest.dropPaths.size()];
        for (const auto& path : qaRequest.dropPaths) {
            NSString* nativePath = [NSString stringWithUTF8String:path.c_str()];
            if (nativePath) [urls addObject:[NSURL fileURLWithPath:nativePath]];
        }
        HarkenQaDraggingInfo* info = [[HarkenQaDraggingInfo alloc] init];
        info.qaPasteboard = pasteboard;
        if (urls.count == qaRequest.dropPaths.size() && [pasteboard writeObjects:urls] &&
            [self.glView draggingEntered:(id<NSDraggingInfo>)info] == NSDragOperationCopy) {
            qaRequest.dropAccepted = [self.glView performDragOperation:(id<NSDraggingInfo>)info];
        }
        [pasteboard releaseGlobally];
    }
#endif
    PumpAppDatabaseAndAudio(*state);
    PumpAppScanRefresh(*state, ImGui::IsPopupOpen(L(Dlg_ScanFolder)));
    state->visibleFilePaths.clear();
    state->warmupFramesRemaining = std::max(0, 4 - frameCount);
    if (state->dbSwitchSettleFramesRemaining > 0) --state->dbSwitchSettleFramesRemaining;
    harken::theme_schedule::Tick(*state);
    if (qaControl) {
        ThemeInspector::SetBridgeCollectionEnabled(true);
        ThemeInspector::BeginFrame();
    }
    RenderUI(*state);
    if (qaControl) ThemeInspector::EndFrame();
    GuardInfoMetadataContext(*state);
    RenderInfoMetadataLeavePrompt(*state);
    FlushSettingsDirty(*state);
    ProcessPendingProductModeToggle(*state);
    PumpAppContinuousPlayback(*state);
    const bool isMouseIdle = std::chrono::steady_clock::now() - state->lastMouseMoveTime >
        std::chrono::milliseconds(500);
    PumpAppWaveforms(*state, isMouseIdle);
    const bool contextMenuHeavyFreeze = ImGui::IsPopupOpen("RowContextMenu") ||
        ImGui::IsPopupOpen("##PlaylistSidebarContext");
    PumpAppCoverTextureUploads(*state, contextMenuHeavyFreeze, isMouseIdle);
    PumpAppBackgroundStatus(*state, isMouseIdle);
    ImGui::Render();
    const NSRect pixels = [self.glView convertRectToBacking:self.glView.bounds];
    glViewport(0, 0, static_cast<int>(pixels.size.width), static_cast<int>(pixels.size.height));
    const auto color = GetThemeClearColor();
    glClearColor(color.x, color.y, color.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (frameCount == 1) RecordStage("RealMainUiRendered");
    if (qaSmoke && frameCount == 90) {
        if ([self captureFrame]) RecordStage("ScreenshotSaved");
        else { applicationExitCode = 2; RecordStage("ScreenshotFailed"); }
    }
    if (qaControl && qaRequest.id) {
        const bool captured = qaRequest.capture && [self captureFrame];
        if (!HarkenSearch::MacQa::Reply(*state, qaRequest, captured)) {
            applicationExitCode = 4; RecordStage("QaReplyFailed");
        }
    }
    FlushDeferredMiniWaveTextureDeletes();
    [self.glView.openGLContext flushBuffer];
    if (qaControl && qaRequest.close) {
        RecordStage("NormalCloseRequested");
        Platform::RequestWindowClose((__bridge void*)self.window);
        return;
    }
    if (qaSmoke && frameCount == 100) {
        RecordStage("NormalCloseRequested");
        Platform::RequestWindowClose((__bridge void*)self.window);
    }
}
- (void)shutdown {
    if (closing) return;
    closing = true;
    [self.renderTimer invalidate];
    self.renderTimer = nil;
    [self.glView.openGLContext makeCurrentContext];
    if (state) {
        RecordStage("ShutdownBegin");
        StopAppRuntimeWorkers(*state, RecordStage);
        StopAppRelinkWorkers(*state, RecordStage);
        if (settingsLoaded) {
            if (!CloseAppRuntimeResources(*state, RecordStage)) applicationExitCode = 3;
        }
        else { state->database.close(); state->audioEngine.shutdown(); }
        if (settingsLoaded && imguiReady) ImGui::SaveIniSettingsToDisk(g_imguiIniPathUtf8.c_str());
        delete state->memoryBudget;
        state->memoryBudget = nullptr;
        CoverModule::SetMemoryBudget(nullptr);
        CoverModule::SetAppState(nullptr);
        CoverModule::SetDatabase(nullptr);
        g_appState = nullptr;
        state.reset();
    }
    if (rendererReady) { ImGui_ImplOpenGL3_Shutdown(); rendererReady = false; }
    if (osxReady) { ImGui_ImplOSX_Shutdown(); osxReady = false; }
    if (imguiReady) { ImGui::DestroyContext(); imguiReady = false; }
    [NSOpenGLContext clearCurrentContext];
    mainWindow = nil;
    RecordStage("ShutdownComplete");
}
- (BOOL)windowShouldClose:(NSWindow*)sender { (void)sender; [self shutdown]; return YES; }
- (void)applicationWillTerminate:(NSNotification*)notification { (void)notification; [self shutdown]; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    [self shutdown];
    [sender stop:nil];
    [sender postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0] atStart:NO];
    return NSTerminateCancel; // Return through main() to preserve the real validation exit code.
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { (void)sender; return YES; }
@end

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        Platform::HarkenUserData::ApplyPreviewIsolationProcessEnvironment();
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--mac-qa-smoke") == 0) qaSmoke = true;
            if (std::strcmp(argv[i], "--mac-qa-control") == 0) qaControl = true;
        }
#endif
        NSApplication* app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        HarkenAppDelegate* delegate = [[HarkenAppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return applicationExitCode;
}
