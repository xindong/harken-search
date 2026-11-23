#pragma once
// Aris - Send to Wwise (F-034 S7) - Transfer Panel UI.
//
// Minimal-viable panel: fields for WwiseTransferConfig, a structure editor,
// Connect / Preview / Transfer buttons, and a simple result display. Aims
// for "audio designer can push 5 files to Wwise and see the result" rather
// than exposing every advanced interaction at once. A richer Diff tree and a
// WAAPI-backed Property Template browser remain tracked as polish items.
//
// The panel owns its own WaapiClient process-wide (one connection per
// application process; Wwise accepts a single WAAPI client here).

#include "WwiseStructureTemplate.h" // MetadataContext for staging sources

#include <string>
#include <vector>

namespace aura {
namespace wwise {

// @stage7-contract
// API contract: panel entry points are UI-thread-only and synchronous with
// the render loop. Open borrows source path/context values only for staging;
// the panel owns its copied pending state. Render is idempotent and safe when
// hidden; Shutdown releases the process-wide WAAPI connection and is intended
// for application shutdown, not concurrent background use.

// Open the panel and stage a batch of source files (typically from the
// right-click menu in the results list, S8). Populates internal pending-
// items state; `RenderWwiseTransferPanel()` will then show the panel next
// time the main loop calls it.
void OpenWwiseTransferPanel(const std::vector<std::string>& sourceWavs,
                            const std::vector<MetadataContext>& contexts);

// Render pass — call from main.cpp's ImGui render loop. No-op when the
// panel is not visible. Idempotent across frames.
void RenderWwiseTransferPanel();

// 传输任务的实时进度快照（供右下角任务浮层读取；非传输时为 active=false）。
struct WwiseTransferProgress {
    bool active = false;
    int done = 0;
    int total = 0;
};
WwiseTransferProgress GetWwiseTransferProgress();

// Closes the panel and disconnects the embedded WAAPI client. Called from
// app shutdown so the socket is torn down cleanly.
void ShutdownWwiseTransferPanel();

} // namespace wwise
}
