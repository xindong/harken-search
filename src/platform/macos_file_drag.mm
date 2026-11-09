#import <Foundation/Foundation.h>
#include "macos_file_drag.h"

#include <filesystem>
#include <unordered_set>

namespace Platform::MacFileDrag {

bool PrepareFileURLs(const std::vector<std::string>& paths,
                     std::vector<PreparedFile>& prepared,
                     std::string& error) {
    prepared.clear();
    error.clear();
    if (paths.empty() || paths.size() > 200) {
        error = paths.empty() ? "没有可拖出的文件" : "一次最多拖出 200 个文件";
        return false;
    }

    std::unordered_set<std::string> seen;
    for (const auto& raw : paths) {
        if (raw.empty() || raw.find('\0') != std::string::npos) {
            error = "文件路径无效";
            prepared.clear();
            return false;
        }
        NSString* native = [NSString stringWithUTF8String:raw.c_str()];
        if (!native) {
            error = "文件路径编码无效";
            prepared.clear();
            return false;
        }
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(std::filesystem::path(raw), ec).lexically_normal();
        if (ec || !std::filesystem::is_regular_file(absolute, ec) || ec) {
            error = "所选音频文件不存在或无法读取";
            prepared.clear();
            return false;
        }
        NSString* nativeAbsolute = [NSString stringWithUTF8String:absolute.string().c_str()];
        if (!nativeAbsolute || ![NSFileManager.defaultManager isReadableFileAtPath:nativeAbsolute]) {
            error = "所选音频文件无法读取";
            prepared.clear();
            return false;
        }
        NSURL* url = [NSURL fileURLWithPath:nativeAbsolute isDirectory:NO];
        if (!url.isFileURL || !url.absoluteString.UTF8String) {
            error = "无法生成音频文件拖放地址";
            prepared.clear();
            return false;
        }
        const std::string path = absolute.string();
        if (seen.insert(path).second)
            prepared.push_back({path, url.absoluteString.UTF8String});
    }
    return !prepared.empty();
}

}
