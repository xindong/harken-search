#pragma once

#include "nlohmann_json.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace HarkenSearch {

class ExternalAiLibrary final {
public:
    ExternalAiLibrary();
    ~ExternalAiLibrary();
    ExternalAiLibrary(const ExternalAiLibrary&) = delete;
    ExternalAiLibrary& operator=(const ExternalAiLibrary&) = delete;

    bool Open(const std::string& databasePath, const std::string& libraryRoot, std::string& error);
    nlohmann::json SearchAudio(const std::string& query, int limit);
    nlohmann::json FindFolders(const std::string& query, int limit);
    nlohmann::json FindSimilar(std::int64_t referenceId, int limit);

private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

}
