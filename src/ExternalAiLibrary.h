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
    nlohmann::json SearchAudio(const std::string& query, int limit, int offset = 0);
    nlohmann::json FindFolders(const std::string& query, int limit, int offset = 0);
    nlohmann::json FindSimilar(std::int64_t referenceId, int limit, int offset = 0);
    nlohmann::json GetMetadata(std::int64_t fileId);
    bool ApplyMetadata(std::int64_t fileId, const nlohmann::json& fields, std::string& error);

    // Resolves a file ID to its path relative to the opened library root so a
    // caller can authorize the reference before running a similarity search.
    bool RelativePath(std::int64_t fileId, std::string& relativePath);

private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

}
