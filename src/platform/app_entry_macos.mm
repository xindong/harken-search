
// macOS Application Entry Point (F-031)
//
// This file replaces WindowsMain.cpp on macOS.
// Creates NSApplication + Cocoa window + OpenGL context + ImGui loop.

#ifdef __APPLE__

#define FolderInfo MacOSLegacyFolderInfo
#import <Cocoa/Cocoa.h>
#undef FolderInfo
#import <OpenGL/gl.h>
#import <OpenGL/OpenGL.h>

#include "imgui.h"
#include "imgui_impl_osx.h"
#include "imgui_impl_opengl3.h"
#include "AppState.h"
#include "BatchRenamePolicy.h"
#include "Localization.h"

#include <memory>
#include <string_view>

// NSOpenGLView subclass for ImGui integration

@interface AuroOpenGLView : NSOpenGLView
@end

@implementation AuroOpenGLView

- (void)prepareOpenGL {
    [super prepareOpenGL];
    // Enable vsync
    GLint swapInt = 1;
    [[self openGLContext] setValues:&swapInt forParameter:NSOpenGLContextParameterSwapInterval];
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

@end

// Application Delegate

@interface AuroAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    std::unique_ptr<AppState> _appState;
    std::string _libraryError;
    FolderInfo _folderTree;
    std::string _selectedFolder;
    char _renamePrefix[256];
    std::vector<RenamePreview> _renamePreview;
}
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) AuroOpenGLView* glView;
@property (nonatomic, strong) NSTimer* renderTimer;
@property (nonatomic, assign) BOOL shouldClose;
- (void)openLibraryAtPath:(const char*)path;
- (void)selectFolder:(const std::string&)path;
- (void)refreshRenamePreview;
@end

@implementation AuroAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    // Create window
    NSRect frame = NSMakeRect(0, 0, 920, 600);
    NSUInteger styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:styleMask
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
    [self.window setTitle:@"Harken Search"];
    [self.window setContentMinSize:NSMakeSize(560, 360)];
    [self.window center];
    [self.window setDelegate:self];

    // Create OpenGL pixel format
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADepthSize, 24,
        NSOpenGLPFAStencilSize, 8,
        NSOpenGLPFAAccelerated,
        0
    };
    NSOpenGLPixelFormat* pixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];

    // Create OpenGL view
    self.glView = [[AuroOpenGLView alloc] initWithFrame:frame pixelFormat:pixelFormat];
    [self.window setContentView:self.glView];
    [[self.glView openGLContext] makeCurrentContext];

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGui_ImplOSX_Init(self.glView);
    ImGui_ImplOpenGL3_Init("#version 150");

    _appState = std::make_unique<AppState>();
    g_appState = _appState.get();

    [self.window makeKeyAndOrderFront:nil];
    self.shouldClose = NO;

    // Start render timer
    self.renderTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/60.0
                                    target:self
                                  selector:@selector(renderFrame)
                                  userInfo:nil
                                   repeats:YES];
}

- (void)refreshRenamePreview {
    _renamePreview.clear();
    const std::string prefix = _renamePrefix;
    if (prefix.empty() || prefix.find_first_of("/\\") != std::string::npos) return;

    std::vector<aura::rename::PreviewInput> inputs;
    inputs.reserve(_appState->searchResults.size());
    for (const auto& record : _appState->searchResults) {
        inputs.push_back({record.fileName, record.filePath, record.id});
    }
    const aura::rename::PreviewLabels labels{
        GetLocalizedString(LStr::RenStatus_NoRules, AppLanguage::English),
        GetLocalizedString(LStr::RenStatus_NoChange, AppLanguage::English),
        GetLocalizedString(LStr::RenStatus_EmptyName, AppLanguage::English),
        GetLocalizedString(LStr::RenStatus_Duplicate, AppLanguage::English),
        GetLocalizedString(LStr::RenStatus_Exists, AppLanguage::English),
        GetLocalizedString(LStr::MacBaseline_RenameReady, AppLanguage::English),
        GetLocalizedString(LStr::RenStatus_Suffix, AppLanguage::English),
    };
    _renamePreview = aura::rename::BuildPreview(inputs, {{RenameMode::AddPrefix, prefix, "", ""}}, labels);
}

- (void)refreshSearch {
    SearchFilter filter;
    filter.keyword = _appState->searchBuffer;
    filter.folderPath = _selectedFolder;
    _appState->searchResults = _appState->database.searchFiles(filter, AppState::PAGE_SIZE);
    _appState->totalFileCount = _appState->database.searchFilesCount(filter);
    [self refreshRenamePreview];
}

- (void)selectFolder:(const std::string&)path {
    if (_selectedFolder == path) return;
    _selectedFolder = path;
    [self refreshSearch];
}

- (void)openLibraryAtPath:(const char*)path {
    _libraryError.clear();
    _folderTree = {};
    _selectedFolder.clear();
    _renamePrefix[0] = '\0';
    _renamePreview.clear();
    _appState->searchBuffer[0] = '\0';
    _appState->searchResults.clear();
    _appState->totalFileCount = 0;
    if (!_appState->database.openReadOnly(path)) {
        _libraryError = _appState->database.getLastError();
        return;
    }

    bool hasLibrary = false;
    _appState->database.execQuery(
        "SELECT 1 FROM main.sqlite_master WHERE type='table' AND name='audio_files'",
        [&](sqlite3_stmt*) { hasLibrary = true; });
    if (!hasLibrary) {
        _appState->database.close();
        _libraryError = GetLocalizedString(LStr::MacBaseline_NotLibrary, AppLanguage::English);
        return;
    }
    _folderTree = _appState->database.getFolderHierarchy();
    [self refreshSearch];
}

- (void)openLibrary {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = [NSString stringWithUTF8String:GetLocalizedString(
        LStr::MacBaseline_OpenLibraryTitle, AppLanguage::English)];
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        if (response != NSModalResponseOK) return;
        const char* path = panel.URL.fileSystemRepresentation;
        if (path) [self openLibraryAtPath:path];
    }];
}

- (void)drawFolder:(const FolderInfo&)folder {
    const auto& label = folder.displayName.empty() ? folder.name : folder.displayName;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (folder.subfolders.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (_selectedFolder == folder.path) flags |= ImGuiTreeNodeFlags_Selected;
    const bool opened = ImGui::TreeNodeEx(folder.path.c_str(), flags, "%s", label.c_str());
    if ((ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) ||
        (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
        [self selectFolder:folder.path];
    }
    if (opened) {
        for (const auto& child : folder.subfolders) [self drawFolder:child];
        ImGui::TreePop();
    }
}

- (void)drawLibrary {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Harken Search##Library", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Harken Search");
    if (ImGui::Button(GetLocalizedString(LStr::MacBaseline_OpenLibrary, AppLanguage::English))) [self openLibrary];
    if (!_libraryError.empty()) {
        ImGui::TextWrapped(GetLocalizedString(LStr::MacBaseline_OpenLibraryError, AppLanguage::English), _libraryError.c_str());
    } else if (!_appState->database.isOpen()) {
        ImGui::TextUnformatted(GetLocalizedString(LStr::MacBaseline_OpenLibraryHint, AppLanguage::English));
    } else {
        ImGui::SameLine();
        const auto& path = _appState->database.getMainDbPath();
        ImGui::TextUnformatted(path.c_str());
        if (ImGui::BeginChild("LibraryFolders", ImVec2(0.0f, 110.0f), true)) {
            if (ImGui::Selectable(GetLocalizedString(LStr::MacBaseline_AllFolders, AppLanguage::English),
                                  _selectedFolder.empty())) [self selectFolder:std::string()];
            for (const auto& folder : _folderTree.subfolders) [self drawFolder:folder];
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-90.0f);
        const bool submitted = ImGui::InputTextWithHint(
            "##LibrarySearch", GetLocalizedString(LStr::MacBaseline_SearchPlaceholder, AppLanguage::English),
            _appState->searchBuffer,
            sizeof(_appState->searchBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button(GetLocalizedString(LStr::MacBaseline_Search, AppLanguage::English)) || submitted) [self refreshSearch];
        ImGui::Text(GetLocalizedString(LStr::MacBaseline_Results, AppLanguage::English),
                    static_cast<long long>(_appState->totalFileCount));
        if (_appState->totalFileCount > AppState::PAGE_SIZE) {
            ImGui::Text(GetLocalizedString(LStr::MacBaseline_ResultLimit, AppLanguage::English), AppState::PAGE_SIZE);
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##RenamePrefix",
                                    GetLocalizedString(LStr::MacBaseline_RenamePrefix, AppLanguage::English),
                                    _renamePrefix, sizeof(_renamePrefix))) [self refreshRenamePreview];
        if (std::string_view(_renamePrefix).find_first_of("/\\") != std::string_view::npos) {
            ImGui::TextUnformatted(GetLocalizedString(LStr::MacBaseline_RenameInvalidPrefix, AppLanguage::English));
        }
        if (ImGui::BeginChild("LibraryResults")) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(_appState->searchResults.size()));
            while (clipper.Step()) {
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                    if (_renamePreview.empty()) {
                        ImGui::TextUnformatted(_appState->searchResults[index].fileName.c_str());
                    } else {
                        const auto& row = _renamePreview[index];
                        ImGui::Text("%s -> %s [%s]", row.oldName.c_str(), row.newName.c_str(), row.status.c_str());
                    }
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

- (void)renderFrame {
    if (self.shouldClose) return;

    [[self.glView openGLContext] makeCurrentContext];

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplOSX_NewFrame(self.glView);
    ImGui::NewFrame();

    [self drawLibrary];

    ImGui::Render();

    NSRect bounds = [self.glView bounds];
    glViewport(0, 0, (int)bounds.size.width, (int)bounds.size.height);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    [[self.glView openGLContext] flushBuffer];
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    self.shouldClose = YES;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    [self.renderTimer invalidate];
    self.renderTimer = nil;
    [[self.glView openGLContext] makeCurrentContext];
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplOSX_Shutdown();
    ImGui::DestroyContext();
    g_appState = nullptr;
    _appState.reset();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

@end

// macOS main() entry point

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        AuroAppDelegate* delegate = [[AuroAppDelegate alloc] init];
        [app setDelegate:delegate];

        [app run];
    }
    return 0;
}

#endif // __APPLE__
