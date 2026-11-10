#include "file_watcher.h"

#ifdef _WIN32
#include <windows.h>
#include <fileapi.h>
#include <winbase.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <sys/stat.h>
#else
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <algorithm>

#include "DiagnosticsLogger.h"

#ifdef _WIN32

struct FileSystemWatcher::WatchInfo {
    HANDLE hDir = INVALID_HANDLE_VALUE;
    std::string path;
    OVERLAPPED overlapped = {};
    std::vector<uint8_t> buffer;
    std::atomic<bool> removed{false};
    std::atomic<bool> ioPending{false};
};

struct FileSystemWatcher::CompletionPortState {
    HANDLE handle = INVALID_HANDLE_VALUE;
};

#endif

FileSystemWatcher::FileSystemWatcher() {
#ifdef _WIN32
    m_completionPort = std::make_unique<CompletionPortState>();
    m_completionPort->handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
#elif defined(__APPLE__)
    // No init needed — streams created per-directory in addWatchDirectory
#endif
}

FileSystemWatcher::~FileSystemWatcher() {
    stop();
#ifdef _WIN32
    if (m_completionPort->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_completionPort->handle);
        m_completionPort->handle = INVALID_HANDLE_VALUE;
    }
#endif
}

bool FileSystemWatcher::addWatchDirectory(const std::string& directoryPath) {
    if (directoryPath.empty()) return false;

#ifdef _WIN32
    std::lock_guard<std::mutex> lock(m_watchesMutex);

    for (const auto& watch : m_watches) {
        if (watch && watch->path == directoryPath && !watch->removed.load()) {
            return true; // 已存在
        }
    }

    auto watch = std::make_shared<WatchInfo>();
    watch->path = directoryPath;
    watch->buffer.resize(64 * 1024); // 64KB
    watch->removed.store(false);
    watch->ioPending.store(false);

    // 如果 watcher 已经在运行，则立即 setup
    if (m_running.load()) {
        if (!setupWatch(*watch)) {
            return false;
        }
    }

    m_watches.push_back(watch);
    return true;
#elif defined(__APPLE__)
    std::lock_guard<std::mutex> lock(m_macWatchesMutex);
    for (const auto& w : m_macWatches) {
        if (w.path == directoryPath) return true; // already watched
    }
    MacWatchInfo info;
    info.path = directoryPath;
    m_macWatches.push_back(std::move(info));
    return true;
#else
    return false;
#endif
}

bool FileSystemWatcher::removeWatchDirectory(const std::string& directoryPath) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(m_watchesMutex);

    auto it = std::find_if(
        m_watches.begin(),
        m_watches.end(),
        [&](const std::shared_ptr<WatchInfo>& w) {
            return w && w->path == directoryPath;
        });

    if (it == m_watches.end()) {
        return false;
    }

    std::shared_ptr<WatchInfo> watch = *it;
    watch->removed.store(true);

    // 只 cancel，不立刻 free；completion 可能晚到
    if (watch->hDir != INVALID_HANDLE_VALUE) {
        CancelIoEx(watch->hDir, &watch->overlapped);
    }

    m_retiredWatches.push_back(watch);
    m_watches.erase(it);
    return true;
#elif defined(__APPLE__)
    std::lock_guard<std::mutex> lock(m_macWatchesMutex);
    auto it = std::find_if(m_macWatches.begin(), m_macWatches.end(),
                           [&](const MacWatchInfo& w) { return w.path == directoryPath; });
    if (it == m_macWatches.end()) return false;
    if (it->streamRef) {
        FSEventStreamStop((FSEventStreamRef)it->streamRef);
        FSEventStreamInvalidate((FSEventStreamRef)it->streamRef);
        FSEventStreamRelease((FSEventStreamRef)it->streamRef);
    }
    m_macWatches.erase(it);
    return true;
#else
    return false;
#endif
}

void FileSystemWatcher::setEventCallback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_callback = std::move(callback);
}

bool FileSystemWatcher::start() {
    if (m_running.load()) return true;

#if defined(__APPLE__)
    m_running.store(true);
    m_watchThread = std::thread(&FileSystemWatcher::watchThreadProc, this);
    return true;
#elif !defined(_WIN32)
    return false;
#else
    if (m_completionPort->handle == INVALID_HANDLE_VALUE) {
        m_completionPort->handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (m_completionPort->handle == INVALID_HANDLE_VALUE) {
            return false;
        }
    }

    m_running.store(true);

    {
        std::lock_guard<std::mutex> lock(m_watchesMutex);

        for (auto& watch : m_watches) {
            if (!watch) continue;
            watch->removed.store(false);

            if (watch->hDir == INVALID_HANDLE_VALUE) {
                if (!setupWatch(*watch)) {
                    // 回滚本次 start 过程中已打开的句柄
                    for (auto& w : m_watches) {
                        if (w) cleanupWatch(*w);
                    }
                    m_running.store(false);
                    return false;
                }
            }
        }
    }

    m_watchThread = std::thread(&FileSystemWatcher::watchThreadProc, this);
    return true;
#endif
}

void FileSystemWatcher::stop() {
    if (!m_running.load()) return;

    m_running.store(false);

#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(m_watchesMutex);

        for (auto& watch : m_watches) {
            if (watch && watch->hDir != INVALID_HANDLE_VALUE) {
                CancelIoEx(watch->hDir, &watch->overlapped);
            }
        }

        for (auto& watch : m_retiredWatches) {
            if (!watch) continue;
            watch->removed.store(true);
            if (watch->hDir != INVALID_HANDLE_VALUE) {
                CancelIoEx(watch->hDir, &watch->overlapped);
            }
        }
    }

    if (m_completionPort->handle != INVALID_HANDLE_VALUE) {
        PostQueuedCompletionStatus(m_completionPort->handle, 0, 0, nullptr);
    }
#elif defined(__APPLE__)
    // Wake the run loop so watchThreadProc exits
    if (m_runLoopRef) {
        CFRunLoopStop((CFRunLoopRef)m_runLoopRef);
    }
#endif

    if (m_watchThread.joinable()) {
        m_watchThread.join();
    }

#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(m_watchesMutex);

        // active watches：关闭句柄，但保留配置，方便下次 start() 重建
        for (auto& watch : m_watches) {
            if (watch) cleanupWatch(*watch);
        }

        // retired watches：现在可以真正回收
        for (auto& watch : m_retiredWatches) {
            if (watch) cleanupWatch(*watch);
        }
        m_retiredWatches.clear();
    }

    // 重建 completion port，避免 stop 前的旧 completion 污染下次 start
    if (m_completionPort->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_completionPort->handle);
        m_completionPort->handle = INVALID_HANDLE_VALUE;
    }
#elif defined(__APPLE__)
    {
        std::lock_guard<std::mutex> lock(m_macWatchesMutex);
        for (auto& w : m_macWatches) {
            if (w.streamRef) {
                FSEventStreamStop((FSEventStreamRef)w.streamRef);
                FSEventStreamInvalidate((FSEventStreamRef)w.streamRef);
                FSEventStreamRelease((FSEventStreamRef)w.streamRef);
                w.streamRef = nullptr;
            }
        }
    }
    m_runLoopRef = nullptr;
#endif
}

#ifdef _WIN32

bool FileSystemWatcher::setupWatch(WatchInfo& watch) {
    std::wstring widePath;
    int size = MultiByteToWideChar(CP_UTF8, 0, watch.path.c_str(), -1, nullptr, 0);
    if (size <= 0) return false;

    widePath.resize(size - 1);
    MultiByteToWideChar(CP_UTF8, 0, watch.path.c_str(), -1, &widePath[0], size);

    watch.hDir = CreateFileW(
        widePath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (watch.hDir == INVALID_HANDLE_VALUE) {
        DIAG_ACTION_FMT("Watcher", "CreateFileW failed for watch root: %s", watch.path.c_str());
        return false;
    }

    // 这里的 &watch 现在是 shared_ptr 持有的堆对象地址，稳定
    if (CreateIoCompletionPort(
            watch.hDir,
            m_completionPort->handle,
            reinterpret_cast<ULONG_PTR>(&watch),
            0) == nullptr) {
        DIAG_ACTION_FMT("Watcher", "CreateIoCompletionPort bind failed: %s", watch.path.c_str());
        CloseHandle(watch.hDir);
        watch.hDir = INVALID_HANDLE_VALUE;
        return false;
    }

    if (!issueRead(watch)) {
        DIAG_ACTION_FMT("Watcher", "Initial ReadDirectoryChangesW failed: %s", watch.path.c_str());
        CloseHandle(watch.hDir);
        watch.hDir = INVALID_HANDLE_VALUE;
        return false;
    }

    return true;
}

bool FileSystemWatcher::issueRead(WatchInfo& watch) {
    if (watch.removed.load()) return false;
    if (watch.hDir == INVALID_HANDLE_VALUE) return false;

    ZeroMemory(&watch.overlapped, sizeof(watch.overlapped));

    DWORD bytesReturned = 0;
    BOOL ok = ReadDirectoryChangesW(
        watch.hDir,
        watch.buffer.data(),
        static_cast<DWORD>(watch.buffer.size()),
        TRUE, // 递归监听子目录
        FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES |
        FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_CREATION,
        &bytesReturned,
        &watch.overlapped,
        nullptr
    );

    if (!ok) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            watch.ioPending.store(false);
            DIAG_ACTION_FMT(
                "Watcher",
                "ReadDirectoryChangesW failed immediately. path=%s err=%lu",
                watch.path.c_str(),
                static_cast<unsigned long>(err));
            return false;
        }
    }

    watch.ioPending.store(true);
    return true;
}

void FileSystemWatcher::cleanupWatch(WatchInfo& watch) {
    if (watch.hDir != INVALID_HANDLE_VALUE) {
        if (watch.ioPending.load()) {
            CancelIoEx(watch.hDir, &watch.overlapped);
        }
        CloseHandle(watch.hDir);
        watch.hDir = INVALID_HANDLE_VALUE;
    }

    watch.ioPending.store(false);
    ZeroMemory(&watch.overlapped, sizeof(watch.overlapped));
}

void FileSystemWatcher::emitNeedRescan(const WatchInfo& watch, const char* reason) {
    EventCallback callbackCopy;
    {
        std::lock_guard<std::mutex> callbackLock(m_callbackMutex);
        callbackCopy = m_callback;
    }

    DIAG_ACTION_FMT("Watcher", "NeedRescan root=%s reason=%s", watch.path.c_str(), reason ? reason : "");

    if (!callbackCopy) return;

    FileEvent ev;
    ev.type = EventType::NeedRescan;
    ev.path = watch.path;   // 根目录
    ev.oldPath.clear();
    ev.isDirectory = true;  // 仅对 NeedRescan，这个字段有明确语义

    callbackCopy(ev);
}

void FileSystemWatcher::watchThreadProc() {
    while (m_running.load()) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED* overlapped = nullptr;

        BOOL success = GetQueuedCompletionStatus(
            m_completionPort->handle,
            &bytesTransferred,
            &completionKey,
            &overlapped,
            1000
        );

        // 显式退出信号
        if (completionKey == 0 && overlapped == nullptr) {
            break;
        }

        WatchInfo* watch = reinterpret_cast<WatchInfo*>(completionKey);

        if (!success) {
            DWORD error = GetLastError();

            if (error == WAIT_TIMEOUT) {
                continue;
            }

            if (watch != nullptr) {
                watch->ioPending.store(false);

                // stop/remove 导致的取消，不当错误处理
                if (error == ERROR_OPERATION_ABORTED) {
                    continue;
                }

                // 关键：明确拦截缓冲区溢出，不能静默跳过
                if (error == ERROR_NOTIFY_ENUM_DIR) {
                    DIAG_ACTION_FMT(
                        "Watcher",
                        "CRITICAL: Buffer overflow on %s. Rescan required.",
                        watch->path.c_str());
                    emitNeedRescan(*watch, "ERROR_NOTIFY_ENUM_DIR");
                } else {
                    DIAG_ACTION_FMT(
                        "Watcher",
                        "GetQueuedCompletionStatus failed. path=%s err=%lu",
                        watch->path.c_str(),
                        static_cast<unsigned long>(error));
                }

                if (!watch->removed.load() && m_running.load()) {
                    if (!issueRead(*watch)) {
                        DIAG_ACTION_FMT("Watcher", "Re-arm after failure failed: %s", watch->path.c_str());
                    }
                }
            }

            continue;
        }

        if (watch == nullptr) {
            continue;
        }

        watch->ioPending.store(false);

        // 已退休的 watch：忽略晚到 completion，不重新 arm
        if (watch->removed.load()) {
            continue;
        }

        // 某些环境下，success == TRUE 但 bytesTransferred == 0，也不能静默吞掉
        if (bytesTransferred == 0) {
            DIAG_ACTION_FMT(
                "Watcher",
                "WARNING: Zero bytes transferred on %s (possible overflow/drop).",
                watch->path.c_str());
            emitNeedRescan(*watch, "zero-bytes-completion");
        } else {
            processWindowsEvents(*watch);
        }

        if (!watch->removed.load() && m_running.load()) {
            if (!issueRead(*watch)) {
                DIAG_ACTION_FMT("Watcher", "Re-arm failed: %s", watch->path.c_str());
            }
        }
    }
}

void FileSystemWatcher::processWindowsEvents(WatchInfo& watch) {
    EventCallback callbackCopy;
    {
        std::lock_guard<std::mutex> callbackLock(m_callbackMutex);
        callbackCopy = m_callback;
    }

    if (!callbackCopy) return;

    FILE_NOTIFY_INFORMATION* pNotify =
        reinterpret_cast<FILE_NOTIFY_INFORMATION*>(watch.buffer.data());

    std::vector<FileEvent> events;
    events.reserve(32);

    std::string pendingOldPath;

    do {
        FileEvent event;
        event.isDirectory = false; // 重要：watcher 不做 stat，交给下游 worker
        event.oldPath.clear();

        int utf8Size = WideCharToMultiByte(
            CP_UTF8,
            0,
            pNotify->FileName,
            pNotify->FileNameLength / sizeof(WCHAR),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (utf8Size > 0) {
            std::string fileName(utf8Size, 0);
            WideCharToMultiByte(
                CP_UTF8,
                0,
                pNotify->FileName,
                pNotify->FileNameLength / sizeof(WCHAR),
                &fileName[0],
                utf8Size,
                nullptr,
                nullptr);

            std::string fullPath = watch.path;
            if (!fullPath.empty() && fullPath.back() != '\\' && fullPath.back() != '/') {
                fullPath += "\\";
            }
            fullPath += fileName;

            event.path = fullPath;
            bool shouldEmit = false;

            switch (pNotify->Action) {
                case FILE_ACTION_ADDED:
                    event.type = EventType::FileAdded;
                    shouldEmit = true;
                    break;

                case FILE_ACTION_REMOVED:
                    event.type = EventType::FileDeleted;
                    shouldEmit = true;
                    break;

                case FILE_ACTION_MODIFIED:
                    event.type = EventType::FileModified;
                    shouldEmit = true;
                    break;

                case FILE_ACTION_RENAMED_OLD_NAME:
                    pendingOldPath = fullPath;
                    break;

                case FILE_ACTION_RENAMED_NEW_NAME:
                    if (!pendingOldPath.empty()) {
                        event.type = EventType::FileRenamed;
                        event.oldPath = pendingOldPath;
                        event.path = fullPath;
                        pendingOldPath.clear();
                        shouldEmit = true;
                    } else {
                        // Treat an unmatched NEW_NAME as an added file.
                        event.type = EventType::FileAdded;
                        shouldEmit = true;
                    }
                    break;

                default:
                    break;
            }

            if (shouldEmit) {
                events.push_back(std::move(event));
            }
        }

        if (pNotify->NextEntryOffset == 0) {
            break;
        }

        pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
            reinterpret_cast<uint8_t*>(pNotify) + pNotify->NextEntryOffset);

    } while (true);

    // 缓冲截断 / 跨批 rename：只记日志，不盲发 delete
    if (!pendingOldPath.empty()) {
        DIAG_ACTION_FMT(
            "Watcher",
            "Orphan OLD_NAME detected, skip blind delete for safety: %s",
            pendingOldPath.c_str());
    }

    // 解锁后批量回调
    for (const auto& ev : events) {
        callbackCopy(ev);
    }
}

#endif // _WIN32

// 事件合并节流 (F-023)

void FileSystemWatcher::coalesceAndEmit(const FileEvent& event) {
    if (m_coalesceWindowMs <= 0) {
        // 不合并，直接回调
        std::lock_guard<std::mutex> lk(m_callbackMutex);
        if (m_callback) m_callback(event);
        return;
    }

    // 只合并 FileModified 事件
    if (event.type != EventType::FileModified) {
        // 非 Modified 事件直接发出
        flushCoalescedEvents(); // 先刷掉 pending
        std::lock_guard<std::mutex> lk(m_callbackMutex);
        if (m_callback) m_callback(event);
        return;
    }

    std::lock_guard<std::mutex> lk(m_coalesceMutex);
    auto now = std::chrono::steady_clock::now();
    auto it = m_pendingModified.find(event.path);
    if (it == m_pendingModified.end()) {
        m_pendingModified[event.path] = now;
    }
    // 已有 pending，忽略（合并）
}

void FileSystemWatcher::flushCoalescedEvents() {
    if (m_coalesceWindowMs <= 0) return;

    std::vector<std::string> ready;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_coalesceMutex);
        for (auto it = m_pendingModified.begin(); it != m_pendingModified.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (elapsed >= m_coalesceWindowMs) {
                ready.push_back(it->first);
                it = m_pendingModified.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (!ready.empty()) {
        std::lock_guard<std::mutex> lk(m_callbackMutex);
        if (m_callback) {
            for (const auto& path : ready) {
                FileEvent ev;
                ev.type = EventType::FileModified;
                ev.path = path;
                m_callback(ev);
            }
        }
    }
}

// macOS FSEvents implementation (F-031)

#ifdef __APPLE__

// Static C callback for FSEventStream — forwards to the watcher instance
static void fsEventsCallback(ConstFSEventStreamRef /*streamRef*/,
                             void* clientCallBackInfo,
                             size_t numEvents,
                             void* eventPaths,
                             const FSEventStreamEventFlags eventFlags[],
                             const FSEventStreamEventId /*eventIds*/[]) {
    auto* watcher = static_cast<FileSystemWatcher*>(clientCallBackInfo);
    const char* const* paths = static_cast<const char* const*>(eventPaths);
    watcher->processAppleEvents(numEvents, paths, eventFlags, nullptr);
}

void FileSystemWatcher::processAppleEvents(size_t numEvents,
                                            const char* const eventPaths[],
                                            const uint32_t eventFlags[],
                                            const uint64_t /*eventIds*/[]) {
    // coalesceAndEmit() and flushCoalescedEvents() own callback synchronization.
    // Holding m_callbackMutex here would request the same non-recursive lock again.
    for (size_t i = 0; i < numEvents; ++i) {
        const std::string path(eventPaths[i]);
        const uint32_t flags = eventFlags[i];

        // MustScanSubDirs → NeedRescan (buffer overflow equivalent)
        if (flags & kFSEventStreamEventFlagMustScanSubDirs) {
            FileEvent ev;
            ev.type = EventType::NeedRescan;
            ev.path = path;
            coalesceAndEmit(ev);
            continue;
        }

        // Determine event type from flags
        if (flags & kFSEventStreamEventFlagItemCreated) {
            FileEvent ev;
            bool isDir = (flags & kFSEventStreamEventFlagItemIsDir) != 0;
            ev.type = isDir ? EventType::DirectoryAdded : EventType::FileAdded;
            ev.path = path;
            ev.isDirectory = isDir;
            coalesceAndEmit(ev);
        }
        if (flags & kFSEventStreamEventFlagItemRemoved) {
            FileEvent ev;
            bool isDir = (flags & kFSEventStreamEventFlagItemIsDir) != 0;
            ev.type = isDir ? EventType::DirectoryDeleted : EventType::FileDeleted;
            ev.path = path;
            ev.isDirectory = isDir;
            coalesceAndEmit(ev);
        }
        if (flags & kFSEventStreamEventFlagItemRenamed) {
            // FSEvents doesn't provide old+new path pairs like Windows;
            // emit as rename with only path (downstream must check exists)
            FileEvent ev;
            ev.type = EventType::FileRenamed;
            ev.path = path;
            coalesceAndEmit(ev);
        }
        if (flags & kFSEventStreamEventFlagItemModified) {
            FileEvent ev;
            ev.type = EventType::FileModified;
            ev.path = path;
            coalesceAndEmit(ev);
        }
    }
}

void FileSystemWatcher::watchThreadProc() {
    // Create a dedicated CFRunLoop for this thread
    m_runLoopRef = (void*)CFRunLoopGetCurrent();

    {
        std::lock_guard<std::mutex> lock(m_macWatchesMutex);
        for (auto& w : m_macWatches) {
            CFStringRef cfPath = CFStringCreateWithCString(
                kCFAllocatorDefault, w.path.c_str(), kCFStringEncodingUTF8);
            CFArrayRef pathsToWatch = CFArrayCreate(
                kCFAllocatorDefault, (const void**)&cfPath, 1, &kCFTypeArrayCallBacks);

            FSEventStreamContext ctx = {};
            ctx.info = this;

            FSEventStreamRef stream = FSEventStreamCreate(
                kCFAllocatorDefault,
                &fsEventsCallback,
                &ctx,
                pathsToWatch,
                kFSEventStreamEventIdSinceNow,
                0.3,  // 300ms latency (batching window)
                kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);

            FSEventStreamScheduleWithRunLoop(stream, (CFRunLoopRef)m_runLoopRef,
                                              kCFRunLoopDefaultMode);
            FSEventStreamStart(stream);
            w.streamRef = (void*)stream;

            CFRelease(pathsToWatch);
            CFRelease(cfPath);
        }
    }

    // Run the loop until stop() calls CFRunLoopStop
    while (m_running.load()) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
        flushCoalescedEvents();
    }
}

#endif // __APPLE__
