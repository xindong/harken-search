#import <Foundation/Foundation.h>
#include "../WindowsMain.h"
#include "../DiagnosticFileLifecycle.h"
#include "../Utils.h"
#include "harken_user_data_layout.h"
#include "system.h"
#include <cstdarg>
#include <cwchar>
#include <filesystem>

namespace {
HarkenSearch::DiagnosticFileLifecycle& DiagnosticFile() {
    static HarkenSearch::DiagnosticFileLifecycle file(
        (std::filesystem::path(Platform::GetTempDirectory()) / "HarkenSearch_diag.log").string(),
        10 * 1024 * 1024);
    return file;
}

// Windows wide printf treats %s as wchar_t*, whereas POSIX requires %ls.
// Existing diagnostic call sites also use %hs for a narrow UTF-8 string.
std::wstring PortableWideFormat(const wchar_t* format) {
    std::wstring out;
    for (size_t i = 0; format[i]; ++i) {
        if (format[i] != L'%') { out += format[i]; continue; }
        out += L'%';
        if (format[i + 1] == L'%') { out += format[++i]; continue; }
        std::wstring spec;
        while (format[++i]) {
            const wchar_t c = format[i];
            if (std::wcschr(L"diouxXfFeEgGaAcCsSpn%", c)) {
                if (c == L's' && spec.find(L'l') == std::wstring::npos) {
                    const auto h = spec.find(L'h');
                    if (h != std::wstring::npos) spec.erase(h, 1);
                    else spec += L'l';
                }
                out += spec;
                out += c;
                break;
            }
            spec += c;
        }
        if (!format[i]) break;
    }
    return out;
}
}

void AppendDiagLogLine(const wchar_t* line) {
    if (!line) return;
    const auto text = "[" + std::to_string(Platform::MonotonicMilliseconds()) + "] " + WideToUtf8(line) + "\n";
    DiagnosticFile().append(text);
}
void AppendDiagLogF(const wchar_t* format, ...) {
    if (!format) return;
    wchar_t message[2048] = {};
    const auto portable = PortableWideFormat(format);
    va_list args;
    va_start(args, format);
    const int count = std::vswprintf(message, sizeof(message) / sizeof(*message), portable.c_str(), args);
    va_end(args);
    if (count < 0) AppendDiagLogLine(L"Diagnostic message formatting failed");
    else AppendDiagLogLine(message);
}
void SetDiagnosticFileEnabled(bool enabled) { DiagnosticFile().setEnabled(enabled); }
std::uint64_t GetDiagnosticFileSizeBytes() { return DiagnosticFile().sizeBytes(); }
std::uint64_t GetDiagnosticFileMaxBytes() { return DiagnosticFile().maxBytes(); }
bool ClearDiagnosticFile() { return DiagnosticFile().clear(); }

std::string BuildSysInfo() {
    NSProcessInfo* info = NSProcessInfo.processInfo;
    return std::string(info.operatingSystemVersionString.UTF8String) +
        " | CPUs: " + std::to_string(info.processorCount) +
        " | RAM: " + std::to_string(info.physicalMemory / (1024 * 1024)) + " MiB";
}
