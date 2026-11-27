
// macOS Application Entry Point (F-031)
//
// This file replaces WindowsMain.cpp on macOS.
// Creates NSApplication + Cocoa window + OpenGL context + ImGui loop.

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/OpenGL.h>

#include "imgui.h"
#include "imgui_impl_osx.h"
#include "platform/shim_window.h"
#include "imgui_impl_opengl3.h"

// Forward declaration — the main app loop lives in main.cpp
// On macOS, we call into it after setting up the window/context
extern void AuroMainLoop(bool shouldClose);
extern void AuroInit();
extern void AuroShutdown();

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

@interface AuroAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) AuroOpenGLView* glView;
@property (nonatomic, assign) BOOL shouldClose;
@end

@implementation AuroAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    // Create window
    NSRect frame = NSMakeRect(100, 100, 1440, 900);
    NSUInteger styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:styleMask
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
    [self.window setTitle:@"Harken Search"];
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

    ImGui::StyleColorsDark();

    ImGui_ImplOSX_Init(self.glView);
    ImGui_ImplOpenGL3_Init("#version 150");

    // Application state and database initialization are not yet wired on macOS.
    // AuroInit();

    // Hand the Cocoa window to the platform shim so the shared UI's
    // GetWindowPlacement/IsZoomed/GetForegroundWindow have a real target.
    Platform::Shim::RegisterMainWindow((__bridge void*)self.window);
    [self.window makeKeyAndOrderFront:nil];
    self.shouldClose = NO;

    // Start render timer
    [NSTimer scheduledTimerWithTimeInterval:1.0/60.0
                                    target:self
                                  selector:@selector(renderFrame)
                                  userInfo:nil
                                   repeats:YES];
}

- (void)renderFrame {
    if (self.shouldClose) return;

    [[self.glView openGLContext] makeCurrentContext];

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplOSX_NewFrame(self.glView);
    ImGui::NewFrame();

    // The macOS entry point does not yet invoke the main renderer.
    // AuroMainLoop(self.shouldClose);

    // Placeholder: show demo window
    ImGui::ShowDemoWindow();

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
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplOSX_Shutdown();
    ImGui::DestroyContext();
    // Application shutdown is not yet wired on macOS.
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
