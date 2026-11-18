#include "CoverKeyCachePersistenceWorkflow.h"
#include "CoverModule.h"
#include "DiagnosticsLogger.h"

#include <chrono>
#include <deque>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace aura::cover_cache {
namespace {
using Clock = std::chrono::steady_clock;
struct Batch {
    std::string path;
    std::vector<CoverKeyCacheUpdate> updates;
};
std::deque<Batch> pending;
std::future<bool> running;
Batch active;
Clock::time_point readyAt;
Clock::time_point retryAt;

void Merge(CoverKeyCacheUpdate& dst, const CoverKeyCacheUpdate& src) {
    if (src.updateEmbedded) {
        dst.updateEmbedded = true;
        dst.embeddedCoverKey = src.embeddedCoverKey;
        dst.embeddedCoverKeyMtime = src.embeddedCoverKeyMtime;
    }
    if (src.updateFolder) {
        dst.updateFolder = true;
        dst.folderCoverKey = src.folderCoverKey;
        dst.folderCoverSrcPath = src.folderCoverSrcPath;
        dst.folderCoverSrcMtime = src.folderCoverSrcMtime;
    }
}

void Add(const std::string& path, CoverKeyCacheUpdate update) {
    if (pending.empty() || pending.back().path != path || pending.back().updates.size() >= 1024) {
        pending.push_back({path, {}});
    }
    auto& items = pending.back().updates;
    for (auto& item : items) {
        if (item.filePath == update.filePath) {
            Merge(item, update);
            return;
        }
    }
    items.push_back(std::move(update));
}

bool WriteBatch(const Batch& batch) {
    Database db;
    if (!db.openCoverPersistenceConnection(batch.path)) {
        DIAG_SYSTEM("CoverPersistence", "Background database open failed");
        return false;
    }
    const bool ok = db.updateCoverKeyCacheBatch(batch.updates, true);
    if (!ok) DIAG_SYSTEM("CoverPersistence", "Background write failed; retrying");
    return ok;
}
}

void PumpPersistence(PersistenceContext context) {
    DiagUiOperationScope phase("UI.CoverKeyPersistence");
    if (running.valid() && running.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        bool ok = false;
        try { ok = running.get(); } catch (...) { ok = false; }
        if (ok) {
            if (active.path == context.databasePathUtf8 && context.database.isOpen())
                context.database.clearFacetCountMemoryCache();
        } else {
            pending.push_front(std::move(active));
            retryAt = Clock::now() + std::chrono::seconds(2);
        }
        active = {};
    }
    if (context.persistenceEnabled && context.database.isOpen() && !context.databasePathUtf8.empty()) {
        std::vector<CoverModule::CoverKeyCacheUpdate> updates;
        if (CoverModule::DrainCoverKeyCacheUpdates(updates, 1024) != 0) {
            if (pending.empty()) readyAt = Clock::now() + std::chrono::milliseconds(250);
            for (const auto& item : updates) {
                if (item.filePathUtf8.empty()) continue;
                CoverKeyCacheUpdate u;
                u.filePath = item.filePathUtf8;
                u.updateEmbedded = item.updateEmbedded;
                u.updateFolder = item.updateFolder;
                u.embeddedCoverKey = item.embeddedCoverKey;
                u.embeddedCoverKeyMtime = item.embeddedCoverKeyMtime;
                u.folderCoverKey = item.folderCoverKey;
                u.folderCoverSrcPath = item.folderCoverSrcPathUtf8;
                u.folderCoverSrcMtime = item.folderCoverSrcMtime;
                Add(context.databasePathUtf8, std::move(u));
            }
        }
    }
    if (!running.valid() && !pending.empty() && Clock::now() >= readyAt && Clock::now() >= retryAt) {
        active = std::move(pending.front());
        pending.pop_front();
        DIAG_METRIC("CoverPersistence", "BatchSize", static_cast<double>(active.updates.size()));
        running = std::async(std::launch::async, WriteBatch, active);
    }
}

void ShutdownPersistence() {
    if (running.valid()) {
        bool ok = false;
        try { ok = running.get(); } catch (...) { ok = false; }
        if (!ok) pending.push_front(std::move(active));
        active = {};
    }
    while (!pending.empty()) {
        Batch batch = std::move(pending.front());
        pending.pop_front();
        if (!WriteBatch(batch))
            DIAG_SYSTEM("CoverPersistence", "Shutdown write failed; cover cache will be rebuilt");
    }
}

}  // namespace aura::cover_cache
