#pragma once

#include <string>
#include <vector>
#include <cstddef>

namespace Platform::MacFileDrag {

struct PreparedFile {
    std::string absolutePath;
    std::string fileUrl;
};

// Check the actual files before advertising them to another application.
bool PrepareFileURLs(const std::vector<std::string>& paths,
                     std::vector<PreparedFile>& prepared,
                     std::string& error);

// The Cocoa view starts the session on the next genuine mouse-drag event.
bool QueueFileDrag(const std::vector<std::string>& paths, std::string& error);
size_t PendingFileCount();
void CancelQueuedFileDrag();

// Called by Cocoa after a copy or cancellation to release shared UI drag state.
void CompleteFileDrag(bool copied);

}
