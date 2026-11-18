#pragma once

#include "Database.h"

#include <string>

namespace aura::cover_cache {

struct PersistenceContext {
    Database& database;
    std::string databasePathUtf8;
    bool persistenceEnabled = true;
};

// Collects updates on the UI thread and persists batches on a worker.
void PumpPersistence(PersistenceContext context);
// Finish queued work before application shutdown.
void ShutdownPersistence();

}  // namespace aura::cover_cache
