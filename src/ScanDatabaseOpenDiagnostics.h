#pragma once
#include <string>
#include <vector>
namespace aura {
std::string FormatScanDatabaseOpenFailure(const std::string& databasePath, const std::string& error, const std::vector<std::string>& folders);
}
