#include "FileBrowser.h"
#include "platform/filesystem.h"
#include "platform/text_encoding.h"
#include <algorithm>
#include <set>

namespace fs = std::filesystem;

// Convert filesystem path to UTF-8 string (handles Chinese characters on Windows)
static std::string PathToUtf8(const fs::path& p) {
    try {
        return Platform::TextEncoding::WideToUtf8Permissive(p.wstring());
    } catch (...) {
        return {};
    }
}

// Convert UTF-8 string to filesystem path
static fs::path Utf8ToPath(const std::string& str) {
    if (str.empty()) return fs::path();
    const std::wstring wide = Platform::TextEncoding::Utf8ToWidePermissive(str);
    if (wide.empty()) return fs::path(str);
    return fs::path(wide);
}

// Supported audio extensions
static const std::set<std::string> AUDIO_EXTENSIONS = {
    ".wav", ".wave", ".mp3", ".flac", ".ogg", ".aiff", ".aif", ".m4a", ".wma", ".wv"
};

FileBrowser::FileBrowser() {
    // Start at user's home or current directory
    m_currentPath = Platform::FileSystem::CurrentDirectory();
    if (m_currentPath.empty()) m_currentPath = ".";
    refresh();
}

FileBrowser::~FileBrowser() {
    cancelDirScan();
    cancelScan();
}

void FileBrowser::cancelDirScan() {
    if (m_dirActiveScan) {
        m_dirActiveScan->cancelled.store(true);
        m_dirActiveScan.reset();
    }
    if (m_dirScanThread.joinable()) {
        m_dirScanThread.detach();
    }
    m_dirScanInProgress.store(false);
}

void FileBrowser::cancelScan() {
    // Non-blocking: flag the active scan so its thread exits early, then detach.
    if (m_activeScan) {
        m_activeScan->cancelled.store(true);
        m_activeScan.reset();
    }
    if (m_scanThread.joinable()) {
        m_scanThread.detach();
    }
    m_scanInProgress.store(false);
}

void FileBrowser::setScope(BrowserScope scope) {
    if (scope == m_scope) return;
    if (scope == BrowserScope::RecursiveAudio) {
        m_scope = scope;
        buildRecursiveEntries();
    } else {
        cancelScan();
        m_recursiveEntries.clear();
        m_recursiveAudioFileCount = 0;
        m_scope = scope;
    }
}

bool FileBrowser::tickScanResult() {
    if (!m_activeScan) return false;
    std::lock_guard<std::mutex> lock(m_activeScan->mutex);
    if (m_activeScan->ready) {
        m_recursiveEntries = std::move(m_activeScan->entries);
        m_recursiveAudioFileCount = m_activeScan->audioFileCount;
        m_activeScan->ready = false;
        m_activeScan.reset();
        m_scanInProgress.store(false);
        return true;
    }
    return false;
}

bool FileBrowser::tickDirScanResult() {
    if (!m_dirActiveScan) return false;
    std::lock_guard<std::mutex> lock(m_dirActiveScan->mutex);
    if (m_dirActiveScan->ready) {
        m_entries = std::move(m_dirActiveScan->entries);
        m_entriesAudioFileCount = m_dirActiveScan->audioFileCount;
        m_dirActiveScan->ready = false;
        m_dirActiveScan.reset();
        m_dirScanInProgress.store(false);
        return true;
    }
    return false;
}

void FileBrowser::startDirScan() {
    cancelDirScan();
    m_entries.clear();
    m_entriesAudioFileCount = 0;
    m_dirScanInProgress.store(true);

    auto scan = std::make_shared<ScanState>();
    m_dirActiveScan = scan;

    std::string pathCopy = m_currentPath;
    bool audioFilter = m_audioFilterEnabled;

    m_dirScanThread = std::thread([scan, pathCopy, audioFilter]() {
        std::vector<FileEntry> result;
        size_t audioFileCount = 0;
        try {
            fs::path dirPath = Utf8ToPath(pathCopy);

            // ".." parent entry always first, not included in the alphabetical sort
            fs::path parent = dirPath.parent_path();
            if (!parent.empty() && parent != dirPath) {
                FileEntry pe;
                pe.name = "..";
                pe.fullPath = PathToUtf8(parent);
                pe.isDirectory = true;
                result.push_back(pe);
            }

            std::vector<FileEntry> dirs, files;
            Platform::FileSystem::DirectoryIterator iterator;
            if (iterator.Open(pathCopy, false, true)) {
                for (;;) {
                    if (scan->cancelled.load()) break;
                    Platform::FileSystem::DirectoryEntry entry;
                    std::string iteratorError;
                    const auto status = iterator.Next(entry, &iteratorError);
                    if (status == Platform::FileSystem::DirectoryReadStatus::End) break;
                    if (status == Platform::FileSystem::DirectoryReadStatus::Error) {
                        dirs.clear();
                        files.clear();
                        break;
                    }

                    const fs::path entryPath = Utf8ToPath(entry.pathUtf8);
                    FileEntry fe;
                    fe.fullPath = entry.pathUtf8;
                    fe.name = PathToUtf8(entryPath.filename());
                    fe.isDirectory = entry.isDirectory;

                    if (!fe.isDirectory) {
                        fe.extension = PathToUtf8(entryPath.extension());
                        std::transform(fe.extension.begin(), fe.extension.end(),
                                       fe.extension.begin(), ::tolower);

                        // Skip macOS AppleDouble resource fork files (._xxx)
                        if (fe.name.find("._") != std::string::npos) {
                            size_t pos = fe.name.find("._");
                            if (pos == 0 || (pos > 0 && fe.name[pos - 1] == ' '))
                                continue;
                        }

                        if (audioFilter && !FileBrowser::isAudioFile(fe.extension))
                            continue;

                        fe.fileSize = entry.fileSize;
                        ++audioFileCount;
                        files.push_back(std::move(fe));
                    } else {
                        dirs.push_back(std::move(fe));
                    }
                }
            }

            if (!scan->cancelled.load()) {
                auto nameSorter = [](const FileEntry& a, const FileEntry& b) {
                    return a.name < b.name;
                };
                std::sort(dirs.begin(), dirs.end(), nameSorter);
                std::sort(files.begin(), files.end(), nameSorter);
                for (auto& d : dirs) result.push_back(std::move(d));
                for (auto& f : files) result.push_back(std::move(f));
            }
        } catch (...) {}

        std::lock_guard<std::mutex> lock(scan->mutex);
        if (!scan->cancelled.load()) {
            scan->entries = std::move(result);
            scan->audioFileCount = audioFileCount;
            scan->ready = true;
        }
    });
}

void FileBrowser::setRootPath(const std::string& path) {
    navigateTo(path);
}

void FileBrowser::navigateTo(const std::string& path) {
    try {
        fs::path targetPath = Utf8ToPath(path);
        if (Platform::FileSystem::Exists(path) && Platform::FileSystem::IsDirectory(path)) {
            m_currentPath = Platform::FileSystem::WeaklyCanonical(path);
            if (m_currentPath.empty()) return;
            m_selectedIndex = -1;

            // Notify immediately so the address bar updates without waiting for scan
            if (m_onDirectoryChanged) {
                m_onDirectoryChanged(m_currentPath);
            }

            // Kick off async current-dir scan (non-blocking)
            startDirScan();

            if (m_scope == BrowserScope::RecursiveAudio) {
                buildRecursiveEntries();
            }
        }
    } catch (...) {
        // Invalid path, keep current
    }
}

void FileBrowser::navigateUp() {
    try {
        fs::path current = Utf8ToPath(m_currentPath);
        fs::path parent = current.parent_path();
        if (!parent.empty() && parent != current) {
            navigateTo(PathToUtf8(parent));
        }
    } catch (...) {}
}

void FileBrowser::refresh() {
    startDirScan();
}


void FileBrowser::setSelectedIndex(int index) {
    if (index >= -1 && index < (int)m_entries.size()) {
        m_selectedIndex = index;

        if (m_selectedIndex >= 0 && m_onFileSelected) {
            m_onFileSelected(m_entries[m_selectedIndex]);
        }
    }
}

const FileEntry* FileBrowser::getSelectedEntry() const {
    if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_entries.size()) {
        return &m_entries[m_selectedIndex];
    }
    return nullptr;
}

void FileBrowser::selectCurrent() {
    if (m_selectedIndex >= 0 && m_onFileSelected) {
        m_onFileSelected(m_entries[m_selectedIndex]);
    }
}

void FileBrowser::activateCurrent() {
    const FileEntry* entry = getSelectedEntry();
    if (!entry) return;

    if (entry->isDirectory) {
        navigateTo(entry->fullPath);
    } else if (m_onFileSelected) {
        m_onFileSelected(*entry);
    }
}

void FileBrowser::buildRecursiveEntries() {
    // Non-blocking cancel: detach any in-flight scan, it will exit via cancelled flag.
    cancelScan();
    m_recursiveEntries.clear();
    m_recursiveAudioFileCount = 0;
    m_scanInProgress.store(true);

    // Create a new ScanState shared between FileBrowser and the thread.
    // If we cancel again before the thread finishes, the thread's copy of 'scan'
    // keeps ScanState alive; it will exit early when scan->cancelled is set.
    auto scan = std::make_shared<ScanState>();
    m_activeScan = scan;

    std::string pathCopy = m_currentPath;
    // Capture 'scan' by value (shared_ptr copy), NOT 'this'.
    // This ensures the thread never touches FileBrowser members directly.
    m_scanThread = std::thread([scan, pathCopy]() {
        std::vector<FileEntry> results;
        bool iteratorFailed = false;
        try {
            Platform::FileSystem::DirectoryIterator iterator;
            if (iterator.Open(pathCopy, true, true)) {
                for (;;) {
                    if (scan->cancelled.load()) break;
                    Platform::FileSystem::DirectoryEntry entry;
                    std::string iteratorError;
                    const auto status = iterator.Next(entry, &iteratorError);
                    if (status == Platform::FileSystem::DirectoryReadStatus::End) break;
                    if (status == Platform::FileSystem::DirectoryReadStatus::Error) {
                        iteratorFailed = true;
                        break;
                    }
                    if (!entry.isRegularFile) continue;
                    const fs::path entryPath = Utf8ToPath(entry.pathUtf8);
                    FileEntry fe;
                    fe.fullPath  = entry.pathUtf8;
                    fe.name      = PathToUtf8(entryPath.filename());
                    fe.extension = PathToUtf8(entryPath.extension());
                    std::transform(fe.extension.begin(), fe.extension.end(),
                                   fe.extension.begin(), ::tolower);
                    if (!FileBrowser::isAudioFile(fe.extension)) continue;
                    fe.isDirectory = false;
                    fe.fileSize = entry.fileSize;
                    std::string relStr = Platform::FileSystem::RelativePath(
                        PathToUtf8(entryPath.parent_path()), pathCopy);
#ifdef _WIN32
                    std::replace(relStr.begin(), relStr.end(), '/', '\\');
#endif
                    if (relStr != ".") fe.relativePath = relStr;
                    results.push_back(std::move(fe));
                }
            }
            if (!scan->cancelled.load() && !iteratorFailed) {
                std::sort(results.begin(), results.end(),
                          [](const FileEntry& a, const FileEntry& b) {
                              return a.fullPath < b.fullPath;
                          });
            }
        } catch (...) {}

        std::lock_guard<std::mutex> lock(scan->mutex);
        scan->entries = std::move(results);
        scan->audioFileCount = scan->entries.size();
        scan->ready = !scan->cancelled.load();
    });
}

int FileBrowser::selectByPath(const std::string& path) {
    const auto& list = activeEntries();
    for (int i = 0; i < (int)list.size(); ++i) {
        if (list[i].fullPath == path) {
            m_selectedIndex = i;
            return i;
        }
    }
    return -1;
}

bool FileBrowser::isAudioFile(const std::string& extension) {
    return AUDIO_EXTENSIONS.find(extension) != AUDIO_EXTENSIONS.end();
}

std::string FileBrowser::formatFileSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unitIndex = 0;
    double size = (double)bytes;

    while (size >= 1024.0 && unitIndex < 3) {
        size /= 1024.0;
        unitIndex++;
    }

    char buffer[32];
    if (unitIndex == 0) {
        snprintf(buffer, sizeof(buffer), "%llu B", (unsigned long long)bytes);
    } else {
        snprintf(buffer, sizeof(buffer), "%.1f %s", size, units[unitIndex]);
    }
    return buffer;
}
