#include "CoverKeyCachePersistenceWorkflow.h"

#include "CoverModule.h"
#include "DiagnosticsLogger.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aura::cover_cache {
namespace {

std::vector<CoverKeyCacheUpdate> g_pendingUpdates;
std::string g_lastDatabasePath;
bool g_pendingFolderPropagation = true;
uint64_t g_lastPropagationTick = 0;

}  // namespace

void PumpPersistence(PersistenceContext context) {
    DiagUiOperationScope phase("UI.CoverKeyPersistence");
    if (!context.persistenceEnabled || !context.database.isOpen()) return;

    static std::vector<CoverModule::CoverKeyCacheUpdate> moduleUpdates;
    if (g_lastDatabasePath != context.databasePathUtf8) {
        g_lastDatabasePath = context.databasePathUtf8;
        g_pendingFolderPropagation = true;
        g_lastPropagationTick = 0;
    }

    const size_t count = CoverModule::DrainCoverKeyCacheUpdates(moduleUpdates, 128);
    if (count > 0) {
        g_pendingUpdates.clear();
        g_pendingUpdates.reserve(moduleUpdates.size());
        for (const auto& update : moduleUpdates) {
            if (update.filePathUtf8.empty()) continue;
            CoverKeyCacheUpdate persisted;
            persisted.filePath = update.filePathUtf8;
            persisted.updateEmbedded = update.updateEmbedded;
            persisted.updateFolder = update.updateFolder;
            persisted.embeddedCoverKey = update.embeddedCoverKey;
            persisted.embeddedCoverKeyMtime = update.embeddedCoverKeyMtime;
            persisted.folderCoverKey = update.folderCoverKey;
            persisted.folderCoverSrcPath = update.folderCoverSrcPathUtf8;
            persisted.folderCoverSrcMtime = update.folderCoverSrcMtime;
            g_pendingUpdates.push_back(std::move(persisted));
        }
        if (!g_pendingUpdates.empty()) {
            DIAG_METRIC("CoverPersistence", "BatchSize", (double)g_pendingUpdates.size());
            context.database.updateCoverKeyCacheBatch(g_pendingUpdates);
            for (const auto& update : g_pendingUpdates) {
                if (!update.embeddedCoverKey.empty() || !update.folderCoverKey.empty()) {
                    g_pendingFolderPropagation = true;
                    break;
                }
            }
        }
    }

    if (g_pendingFolderPropagation) {
        // Folder propagation is performed by the background cover pipeline.
        g_pendingFolderPropagation = false;
    }
}

}  // namespace aura::cover_cache
