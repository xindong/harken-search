#pragma once

#include "nlohmann_json.hpp"

#include <string>
#include <vector>

namespace HarkenSearch {

// Capabilities an external local MCP client can ask for. Read capabilities gate
// library queries; file-name and metadata reads are checked separately so one
// can be shared while the other stays closed.
enum class ExternalAiCapability {
    SearchAudio,
    FindSimilar,
    FindFolders,
    ReadFileName,
    ReadMetadata,
    EditMetadata,
};

enum class ExternalAiAccess {
    Denied,
    ReadOnly,
    Editable,
};

// Per-library authorization document for the external AI connector.
//
// The document is the only source of truth for one library: the master switch,
// the indexed roots the user ticked, and the access level of each capability.
// Every request re-reads a freshly parsed document, so revoking a root or
// lowering a capability applies to the next request even while a client keeps
// the connector process alive.
class ExternalAiAuthorization final {
public:
    // Returns false and fills error for malformed documents. Unknown capability
    // names and unknown access levels are rejected: a typo must not silently
    // grant or widen access.
    static bool Parse(const std::string& json, ExternalAiAuthorization& out, std::string& error);

    bool enabled() const { return enabled_; }
    bool sampleLibrary() const { return sampleLibrary_; }
    const std::string& libraryId() const { return libraryId_; }
    const std::vector<std::string>& roots() const { return roots_; }

    // Decides one call. libraryId must be the identity of the database the
    // request actually uses; a document for another library never matches.
    // targetPath is the absolute file or folder the request touches and may be
    // empty for library-wide queries. Containment is resolved on the canonical
    // path, so a junction, symlink, ".." component or shared name prefix cannot
    // step outside a granted root.
    ExternalAiAccess Authorize(ExternalAiCapability capability, const std::string& libraryId,
        const std::string& targetPath) const;

private:
    struct Grant {
        ExternalAiCapability capability;
        ExternalAiAccess access;
    };

    bool enabled_ = false;
    bool sampleLibrary_ = false;
    std::string libraryId_;
    std::vector<std::string> roots_;
    std::vector<Grant> grants_;
};

}  // namespace HarkenSearch
