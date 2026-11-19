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

    // Resolves a file ID to its path relative to the opened library root so a
    // caller can authorize the reference before running a similarity search.
    bool RelativePath(std::int64_t fileId, std::string& relativePath);

private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

}
