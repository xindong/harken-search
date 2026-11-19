#include "SameFolderScanRuntime.h"

#include "platform/filesystem.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>

namespace aura::same_folder {

struct SameFolderScanRuntime::Impl {
    void ensureStarted() {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true)) return;
        worker = std::thread([this]() { workerLoop(); });
    }

    bool isCanceled(uint64_t generation) const {
        return cancelGeneration.load(std::memory_order_acquire) != generation;
    }

    void pushChunk(SameFolderScanChunk&& chunk) {
        std::lock_guard<std::mutex> lock(outputMutex);
        outputQueue.push_back(std::move(chunk));
    }

    void workerLoop() {
        while (running.load(std::memory_order_acquire)) {
            SameFolderScanRequest request;
            {
                std::unique_lock<std::mutex> lock(requestMutex);
                requestCv.wait(lock, [this]() {
                    return !running.load(std::memory_order_acquire) ||
                           pendingRequest.has_value();
                });
                if (!running.load(std::memory_order_acquire)) break;
                request = *pendingRequest;
                pendingRequest.reset();
            }

            SameFolderScanChunk chunk;
            chunk.generation = request.generation;
            chunk.filePaths.reserve(128);

            const auto folderStatus =
                Platform::FileSystem::ProbePath(request.folderPathUtf8);
            if (folderStatus != Platform::FileSystem::PathStatus::Present ||
                !Platform::FileSystem::IsDirectory(request.folderPathUtf8)) {
                chunk.done = true;
                chunk.error = request.missingDirectoryErrorUtf8;
                pushChunk(std::move(chunk));
                continue;
            }

            Platform::FileSystem::DirectoryIterator iterator;
            if (!iterator.Open(request.folderPathUtf8, false, true)) {
                chunk.done = true;
                chunk.error = request.missingDirectoryErrorUtf8;
                pushChunk(std::move(chunk));
                continue;
            }

            Platform::FileSystem::DirectoryEntry entry;
            std::string iteratorError;
            while (iterator.Next(entry, &iteratorError) ==
                   Platform::FileSystem::DirectoryReadStatus::Entry) {
                if (isCanceled(request.generation)) break;
                if (!entry.isRegularFile) continue;

                std::string extension =
                    std::filesystem::u8path(entry.pathUtf8).extension().u8string();
                std::transform(
                    extension.begin(), extension.end(), extension.begin(),
                    [](unsigned char value) {
                        return static_cast<char>(std::tolower(value));
                    });
                static const std::unordered_set<std::string> kAudioExtensions = {
                    ".wav", ".wave", ".mp3", ".flac", ".ogg", ".aiff", ".aif", ".m4a", ".wma", ".wv"};
                if (kAudioExtensions.find(extension) == kAudioExtensions.end()) {
                    continue;
                }

                chunk.filePaths.push_back(entry.pathUtf8);
                if (chunk.filePaths.size() >= 128) {
                    pushChunk(SameFolderScanChunk{
                        request.generation,
                        std::move(chunk.filePaths),
                        false,
                        {}});
                    chunk.filePaths.clear();
                    chunk.filePaths.reserve(128);
                }
            }

            if (!isCanceled(request.generation)) {
                if (!chunk.filePaths.empty()) {
                    pushChunk(SameFolderScanChunk{
                        request.generation,
                        std::move(chunk.filePaths),
                        false,
                        {}});
                }
                SameFolderScanChunk done;
                done.generation = request.generation;
                done.done = true;
                if (!iteratorError.empty()) done.error = iteratorError;
                pushChunk(std::move(done));
            }
        }
    }

    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex requestMutex;
    std::condition_variable requestCv;
    std::optional<SameFolderScanRequest> pendingRequest;
    std::mutex outputMutex;
    std::deque<SameFolderScanChunk> outputQueue;
    std::atomic<uint64_t> cancelGeneration{0};
};

SameFolderScanRuntime::SameFolderScanRuntime()
    : impl_(std::make_unique<Impl>()) {}

SameFolderScanRuntime::~SameFolderScanRuntime() {
    stop();
}

void SameFolderScanRuntime::submitLatest(
    const SameFolderScanRequest& request) {
    impl_->ensureStarted();
    impl_->cancelGeneration.store(request.generation, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(impl_->requestMutex);
        impl_->pendingRequest.reset();
        impl_->pendingRequest = request;
    }
    impl_->requestCv.notify_one();
}

bool SameFolderScanRuntime::tryPopChunk(SameFolderScanChunk& chunk) {
    std::lock_guard<std::mutex> lock(impl_->outputMutex);
    if (impl_->outputQueue.empty()) return false;
    chunk = std::move(impl_->outputQueue.front());
    impl_->outputQueue.pop_front();
    return true;
}

void SameFolderScanRuntime::stop() {
    bool expected = true;
    if (!impl_->running.compare_exchange_strong(expected, false)) return;
    {
        std::lock_guard<std::mutex> lock(impl_->requestMutex);
        impl_->requestCv.notify_all();
    }
    if (impl_->worker.joinable()) impl_->worker.join();
}

} // namespace aura::same_folder
