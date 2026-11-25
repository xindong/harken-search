#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <unordered_map>
#include <chrono>

// 文件系统监听器（Windows ReadDirectoryChangesW / Mac FSEvents）
// 用于百万级文件的增量更新，避免全量扫描
// @stage7-contract
// API contract: FileSystemWatcher owns its worker thread and callback state.
// Directory paths and callbacks are copied at registration; callbacks run on
// the watcher thread and must marshal UI/database work themselves. Callers
// must stop before destruction, may add/remove watches outside callbacks, and
// must treat NeedRescan as a recovery signal rather than a complete event
// stream. Methods report lifecycle/OS failure through bool and never transfer
// ownership of native handles to callers.
class FileSystemWatcher {
public:
    enum class EventType {
        FileAdded,
        FileModified,
        FileDeleted,
        FileRenamed,
        DirectoryAdded,
        DirectoryDeleted,

        // 监听缓冲区溢出 / 事件丢失 / 可疑零字节完成时，
        // 明确通知下游：这个 root 需要做一次保守重扫
        NeedRescan
    };

    struct FileEvent {
        EventType type{};
        std::string path;        // 普通事件：变更路径；NeedRescan：watch 根路径
        std::string oldPath;     // FileRenamed 时的旧路径
        bool isDirectory = false;
        // 注意：
        // 1) 对普通 watcher 事件，这个字段不再保证准确，默认 false
        // 2) 下游必须在自己的 worker 线程中自行做 exists / is_directory / stat
        // 3) 对 NeedRescan，可将 path 当作 watch 根目录
    };

    using EventCallback = std::function<void(const FileEvent& event)>;

    FileSystemWatcher();
    ~FileSystemWatcher();

// 事件合并节流 (F-023)
    /// 设置事件合并窗口（毫秒）。同一路径在窗口内的多次 Modified 合并为一次。
    /// 默认 0 = 不合并。推荐值：100-200ms。
    void setCoalesceWindowMs(int ms) { m_coalesceWindowMs = ms; }
    int  getCoalesceWindowMs() const { return m_coalesceWindowMs; }

    // 添加要监听的目录（支持多个根目录）
    bool addWatchDirectory(const std::string& directoryPath);

    // 移除监听目录
    bool removeWatchDirectory(const std::string& directoryPath);

    // 设置事件回调函数
    void setEventCallback(EventCallback callback);

    // 开始监听（启动后台线程）
    bool start();

    // 停止监听
    void stop();

    // 检查是否正在运行
    bool isRunning() const { return m_running.load(); }

private:
#ifdef _WIN32
    struct WatchInfo;
    struct CompletionPortState;

    void watchThreadProc();
    void processWindowsEvents(WatchInfo& watch);

    bool setupWatch(WatchInfo& watch);
    bool issueRead(WatchInfo& watch);
    void retryUnavailableWatches();
    void rearmWatch(WatchInfo& watch);
    void cleanupWatch(WatchInfo& watch);
    void emitNeedRescan(const WatchInfo& watch, const char* reason);

    // active watches：当前正在被业务层持有的 watch 根
    std::vector<std::shared_ptr<WatchInfo>> m_watches;

    // retired watches：remove 后先挂起，等 stop/join 后再真正 cleanup/free
    std::vector<std::shared_ptr<WatchInfo>> m_retiredWatches;

    std::mutex m_watchesMutex;
    std::unique_ptr<CompletionPortState> m_completionPort;
#elif defined(__APPLE__)
    // macOS FSEvents implementation (F-031)
public:
    // Called from static C callback (fsEventsCallback) — must be accessible
    void processAppleEvents(size_t numEvents, const char* const eventPaths[],
                            const uint32_t eventFlags[], const uint64_t eventIds[]);
private:
    void watchThreadProc();

    struct MacWatchInfo {
        std::string path;
        void* streamRef = nullptr;  // FSEventStreamRef (opaque to avoid CoreServices include)
    };
    std::vector<MacWatchInfo> m_macWatches;
    std::mutex m_macWatchesMutex;
    void* m_runLoopRef = nullptr;   // CFRunLoopRef
#else
    // Linux inotify (future)
    void watchThreadProc();
#endif

    std::thread m_watchThread;
    std::atomic<bool> m_running{false};
    EventCallback m_callback;
    std::mutex m_callbackMutex;

    // F-023: 事件合并节流
    int m_coalesceWindowMs = 0; // 0 = disabled
    // 用于合并同一路径的 FileModified 事件（path → first_seen_time）
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_pendingModified;
    std::mutex m_coalesceMutex;

    // 内部：如果合并窗口开启，事件先进 pending，在 flush 时统一触发
    void coalesceAndEmit(const FileEvent& event);
    void flushCoalescedEvents();
};
