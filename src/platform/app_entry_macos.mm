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
#include "../ModuleUI.h"
#include "../CoverModule.h"
#include "../DiagnosticsLogger.h"
#include "../UserActionLog.h"
#include "../MemoryBudget.h"
#include "../WorkerThreadPolicy.h"
#include "../Utils.h"
#include "harken_user_data_layout.h"
#include "macos_qa_bridge.h"
#include "filesystem.h"
#include "system.h"
#include <algorithm>
#include <fstream>
#include <memory>
#include <filesystem>
#include <cstring>

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
}
void* HarkenSearch::UiHost::MainWindowOwner() noexcept { return (__bridge void*)mainWindow; }

@interface HarkenOpenGLView : NSOpenGLView
@end
@implementation HarkenOpenGLView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)prepareOpenGL {
    [super prepareOpenGL];
    GLint swapInterval = 1;
    [self.openGLContext setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];
}
@end

@interface HarkenAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    std::unique_ptr<AppState> state;
    bool imguiReady;
    bool osxReady;
    bool rendererReady;
    bool settingsLoaded;
    bool closing;
    int frameCount;
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
    LoadChineseFont(io, 14.0f, state->settings.readingFontSize, state->settings.readingFontSize);
    ApplyTheme(state->settings.theme);
    if (state->settings.useCustomColors) ApplyCustomColors(state->settings);
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
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplOSX_NewFrame(self.glView);
    ImGui::NewFrame();
    ++frameCount;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) MarkUiLeftClickNow();
    if (g_suppressImGuiDragTooltipFrames > 0) --g_suppressImGuiDragTooltipFrames;
    const auto qaRequest = qaControl ? HarkenSearch::MacQa::Poll(*state) : HarkenSearch::MacQa::Request{};
    PumpAppDatabaseAndAudio(*state);
    PumpAppScanRefresh(*state, ImGui::IsPopupOpen(L(Dlg_ScanFolder)));
    state->visibleFilePaths.clear();
    state->warmupFramesRemaining = std::max(0, 4 - frameCount);
    if (state->dbSwitchSettleFramesRemaining > 0) --state->dbSwitchSettleFramesRemaining;
    harken::theme_schedule::Tick(*state);
    RenderUI(*state);
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
