#pragma once
#include "platform/filesystem.h"
#include <string>
#include <utility>
#include <vector>
#include <atomic>
#include <chrono>

namespace harken::settings {
template<class Writer = Platform::FileSystem::StreamWriter>
bool SaveKeyValueFile(const std::string& pathUtf8, const std::vector<std::pair<std::string,std::string>>& items) {
    Writer writer;
    static std::atomic<unsigned long long> sequence{0};
    const std::string pending=pathUtf8+".pending-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++sequence);
    if (!writer.Open(pending, false, true)) return false;
    bool written=writer.Write("# Aris settings (auto-generated)\n");
    for (const auto& it:items) {
        written=writer.Write(it.first)&&written;
        written=writer.Write("=")&&written;
        written=writer.Write(it.second)&&written;
        written=writer.Write("\n")&&written;
    }
    const bool flushed=written && writer.Flush();
    const bool closed=writer.CloseChecked();
    if (!flushed || !closed || !Platform::FileSystem::AtomicFileReplace(pathUtf8,pending)) {
        Platform::FileSystem::RemoveFile(pending);
        return false;
    }
    return true;
}
}
