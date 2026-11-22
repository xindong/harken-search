#pragma once

struct ImFont;
class MemoryBudget;
namespace harken::performance_monitor { class Monitor; }

namespace harken::diagnostic_overlay {
void PollLogExport();
// QA-only entry: starts the same export path as the button without mouse input.
bool StartExportForQa();
bool DrawWindow(performance_monitor::Monitor& monitor, const MemoryBudget* memoryBudget, bool justOpened,
                ImFont* regularFont, ImFont* emphasisFont);
}
