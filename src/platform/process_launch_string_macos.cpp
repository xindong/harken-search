#ifdef __APPLE__

#include "process_launch.h"
#include "system.h"
#include "text_encoding.h"

#include <string>
#include <string_view>

namespace Platform {
namespace {

std::wstring QuoteCommandArgument(std::wstring_view value) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            slashes = 0;
        } else {
            quoted.append(slashes, L'\\');
            quoted.push_back(character);
            slashes = 0;
        }
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

} // namespace

bool LaunchProcess(const std::string& exePathUtf8, const std::string& argsUtf8) {
    const std::wstring executable = TextEncoding::Utf8ToWideStrict(exePathUtf8);
    if (executable.empty()) return false;
    const std::wstring arguments = TextEncoding::Utf8ToWideStrict(argsUtf8);
    if (!argsUtf8.empty() && arguments.empty()) return false;
    std::wstring commandLine = QuoteCommandArgument(executable);
    if (!arguments.empty()) {
        commandLine.push_back(L' ');
        commandLine += arguments;
    }
    return LaunchProcess(executable, commandLine, ProcessLaunchOptions{}).status ==
           ProcessLaunchStatus::Ok;
}

} // namespace Platform

#endif
