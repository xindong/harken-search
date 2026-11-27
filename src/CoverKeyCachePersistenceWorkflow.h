#pragma once

class Database;

namespace aura::cover_cache {

void BindPersistence(const Database& database);
bool PumpPersistence();
void StopPersistence();
void FlushPersistence();

}  // namespace aura::cover_cache
