
// DiagnosticsLoggerStub — 上线构建用空实现（AURA_BUILD_DIAGNOSTICS=0 时链接此文件）

// 与 DiagnosticsLogger.cpp 二选一，由 CMake 根据 AURA_BUILD_DIAGNOSTICS 决定。

#include "DiagnosticsLogger.h"

DiagnosticsLogger& DiagnosticsLogger::instance() {
    static DiagnosticsLogger s_instance;
    return s_instance;
}

DiagnosticsLogger::DiagnosticsLogger() = default;
DiagnosticsLogger::~DiagnosticsLogger() = default;

void DiagnosticsLogger::setEnabled(bool) {}
void DiagnosticsLogger::setOutputPath(const std::string&) {}
void DiagnosticsLogger::setCategoryFilter(const std::string&) {}
std::string DiagnosticsLogger::getCategoryFilter() const { return {}; }
void DiagnosticsLogger::logAction(const char*, const char*) {}
void DiagnosticsLogger::logPerformance(const char*, const char*, double) {}
void DiagnosticsLogger::logMetric(const char*, const char*, double) {}
void DiagnosticsLogger::logSystem(const char*, const char*) {}
bool DiagnosticsLogger::logSystemAt(const char*, const char*, int64_t) { return false; }
bool DiagnosticsLogger::exportJson(const std::string&) const { return false; }
std::string DiagnosticsLogger::exportJsonString() const { return {}; }
size_t DiagnosticsLogger::entryCount() const { return 0; }
void DiagnosticsLogger::clear() {}
void DiagnosticsLogger::setMaxEntries(size_t) {}
size_t DiagnosticsLogger::maxEntries() const { return 0; }
void DiagnosticsLogger::tickFrame() {}
float DiagnosticsLogger::currentFps() const { return 0.0f; }
double DiagnosticsLogger::currentMemoryMb() const { return 0.0; }
uint64_t DiagnosticsLogger::droppedCount() const { return 0; }
int DiagnosticsLogger::copyRecent(DiagLogEntry*, int) const { return 0; }
std::string DiagnosticsLogger::getRecentJsonLines(int) const { return {}; }
std::string DiagnosticsLogger::getRecentFilteredLines(int) const { return {}; }
bool DiagnosticsLogger::exportJsonLinesCrashSafe(const wchar_t*) { return false; }
