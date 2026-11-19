#pragma once

struct ImFont;
class MemoryBudget;
namespace harken::performance_monitor { class Monitor; }

namespace harken::diagnostic_overlay {
void PollLogExport();
bool DrawWindow(performance_monitor::Monitor& monitor, const MemoryBudget* memoryBudget, bool justOpened,
                ImFont* regularFont, ImFont* emphasisFont);
}
