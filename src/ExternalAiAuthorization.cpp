#include "ExternalAiAuthorization.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

namespace HarkenSearch {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

struct CapabilityName {
    const char* key;
    ExternalAiCapability capability;
};

constexpr std::array<CapabilityName, 9> CapabilityNames{{
    {"searchAudio", ExternalAiCapability::SearchAudio},
    {"findSimilar", ExternalAiCapability::FindSimilar},
    {"findFolders", ExternalAiCapability::FindFolders},
    {"readFileName", ExternalAiCapability::ReadFileName},
    {"readMetadata", ExternalAiCapability::ReadMetadata},
    {"editMetadata", ExternalAiCapability::EditMetadata},
    {"aiNamingMetadata", ExternalAiCapability::AiNamingMetadata},
    {"readPlaylist", ExternalAiCapability::ReadPlaylist},
    {"editPlaylist", ExternalAiCapability::EditPlaylist},
}};

constexpr std::size_t MaxRoots = 32;
constexpr std::size_t MaxPathBytes = 4096;

std::optional<ExternalAiCapability> CapabilityFromKey(const std::string& key)
{
    for (const auto& entry : CapabilityNames) {
        if (key == entry.key) {
            return entry.capability;
        }
    }
    return std::nullopt;
}

std::optional<ExternalAiAccess> AccessFromValue(const std::string& value)
{
    if (value == "none") {
        return ExternalAiAccess::Denied;
    }
    if (value == "read") {
        return ExternalAiAccess::ReadOnly;
    }
    if (value == "edit") {
        return ExternalAiAccess::Editable;
    }
    return std::nullopt;
}

std::optional<fs::path> Canonical(const std::string& value)
{
    if (value.empty() || value.size() > MaxPathBytes || value.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    auto path = fs::u8path(value).make_preferred();
    if (!path.is_absolute()) {
        return std::nullopt;
    }
    for (const auto& component : path.relative_path()) {
        if (component == "." || component == "..") {
            return std::nullopt;
        }
    }
    std::error_code error;
    auto resolved = fs::weakly_canonical(path, error);
    if (error || resolved.empty() || !resolved.is_absolute()) {
        return std::nullopt;
    }
    return resolved;
}

#ifdef _WIN32
std::wstring Fold(const std::wstring& value)
{
    std::wstring folded = value;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t unit) {
        return unit >= L'A' && unit <= L'Z' ? static_cast<wchar_t>(unit - L'A' + L'a') : unit;
    });
    return folded;
}
#endif

std::vector<fs::path::string_type> Components(const fs::path& path)
{
    std::vector<fs::path::string_type> parts;
    for (const auto& part : path) {
        parts.push_back(part.native());
    }
    return parts;
}

bool SameComponent(const fs::path::string_type& left, const fs::path::string_type& right)
{
#ifdef _WIN32
    return Fold(left) == Fold(right);
#else
    return left == right;
#endif
}

bool InsideRoot(const fs::path& root, const fs::path& target)
{
    const auto rootParts = Components(root);
    const auto targetParts = Components(target);
    if (rootParts.empty() || targetParts.size() < rootParts.size()) {
        return false;
    }
    for (std::size_t index = 0; index < rootParts.size(); ++index) {
        if (!SameComponent(rootParts[index], targetParts[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool ExternalAiAuthorization::Parse(const std::string& json, ExternalAiAuthorization& out, std::string& error)
{
    out = ExternalAiAuthorization{};
    Json document = Json::parse(json, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        error = "authorization document must be a JSON object";
        return false;
    }
    for (auto field = document.begin(); field != document.end(); ++field) {
        if (field.key() != "libraryId" && field.key() != "enabled" && field.key() != "sampleLibrary" &&
            field.key() != "roots" && field.key() != "capabilities" &&
            field.key() != "bridgeToken") {
            error = "unknown field '" + field.key() + "'";
            return false;
        }
    }
    const auto enabled = document.find("enabled");
    if (enabled == document.end() || !enabled->is_boolean()) {
        error = "enabled must be a boolean";
        return false;
    }
    const auto libraryId = document.find("libraryId");
    if (libraryId == document.end() || !libraryId->is_string() || libraryId->get<std::string>().empty()) {
        error = "libraryId must be a non-empty string";
        return false;
    }
    const auto sample = document.find("sampleLibrary");
    if (sample != document.end() && !sample->is_boolean()) {
        error = "sampleLibrary must be a boolean";
        return false;
    }
    const auto roots = document.find("roots");
    if (roots != document.end()) {
        if (!roots->is_array() || roots->size() > MaxRoots) {
            error = "roots must be an array of at most 32 paths";
            return false;
        }
        for (const auto& root : *roots) {
            // Legacy documents store a plain path string; the ticked form stores
            // {path, enabled}. A string means ticked, so old files keep working.
            if (root.is_string()) {
                if (root.get<std::string>().empty()) {
                    error = "each root must be a non-empty string";
                    return false;
                }
                out.roots_.push_back({root.get<std::string>(), true});
                continue;
            }
            if (!root.is_object()) {
                error = "each root must be a path or an object with path and enabled";
                return false;
            }
            for (auto field = root.begin(); field != root.end(); ++field) {
                if (field.key() != "path" && field.key() != "enabled") {
                    error = "unknown root field '" + field.key() + "'";
                    return false;
                }
            }
            const auto path = root.find("path");
            const auto ticked = root.find("enabled");
            if (path == root.end() || !path->is_string() || path->get<std::string>().empty()) {
                error = "root path must be a non-empty string";
                return false;
            }
            if (ticked != root.end() && !ticked->is_boolean()) {
                error = "root enabled must be a boolean";
                return false;
            }
            out.roots_.push_back({path->get<std::string>(),
                ticked == root.end() ? true : ticked->get<bool>()});
        }
    }
    const auto bridgeToken = document.find("bridgeToken");
    if (bridgeToken != document.end()) {
        if (!bridgeToken->is_string()) {
            error = "bridgeToken must be a string";
            return false;
        }
        const auto value = bridgeToken->get<std::string>();
        if (value.size() > 128) {
            error = "bridgeToken is too long";
            return false;
        }
        out.bridgeToken_ = value;
    }
    const auto capabilities = document.find("capabilities");
    if (capabilities != document.end()) {
        if (!capabilities->is_object()) {
            error = "capabilities must be an object";
            return false;
        }
        for (auto entry = capabilities->begin(); entry != capabilities->end(); ++entry) {
            const auto capability = CapabilityFromKey(entry.key());
            if (!capability) {
                error = "unknown capability '" + entry.key() + "'";
                return false;
            }
            if (!entry.value().is_string()) {
                error = "capability '" + entry.key() + "' must be a string";
                return false;
            }
            const auto access = AccessFromValue(entry.value().get<std::string>());
            if (!access) {
                error = "capability '" + entry.key() + "' has unknown access level";
                return false;
            }
            out.grants_.push_back({*capability, *access});
        }
    }
    out.enabled_ = enabled->get<bool>();
    out.sampleLibrary_ = sample != document.end() && sample->get<bool>();
    out.libraryId_ = libraryId->get<std::string>();
    return true;
}

ExternalAiAccess ExternalAiAuthorization::AccessFor(ExternalAiCapability capability) const
{
    const auto grant = std::find_if(grants_.begin(), grants_.end(), [capability](const Grant& entry) {
        return entry.capability == capability;
    });
    return grant == grants_.end() ? ExternalAiAccess::Denied : grant->access;
}

ExternalAiAccess ExternalAiAuthorization::Authorize(ExternalAiCapability capability, const std::string& libraryId,
    const std::string& targetPath) const
{
    if (!enabled_ || libraryId != libraryId_) {
        return ExternalAiAccess::Denied;
    }
    auto grant = std::find_if(grants_.begin(), grants_.end(), [capability](const Grant& entry) {
        return entry.capability == capability;
    });
    if (grant == grants_.end() || grant->access == ExternalAiAccess::Denied) {
        return ExternalAiAccess::Denied;
    }
    // No ticked directory means no authorized scope. Adding a path to the
    // settings document alone never opens the library.
    const auto anyTicked = std::any_of(roots_.begin(), roots_.end(),
        [](const ExternalAiRoot& root) { return root.enabled; });
    if (!anyTicked) {
        return ExternalAiAccess::Denied;
    }
    // Every capability is scoped to the ticked directories, so a request
    // without a resolvable target inside one of them is denied.
    const auto target = Canonical(targetPath);
    if (!target) {
        return ExternalAiAccess::Denied;
    }
    bool contained = false;
    for (const auto& rootValue : roots_) {
        if (!rootValue.enabled) continue;
        const auto root = Canonical(rootValue.path);
        if (root && InsideRoot(*root, *target)) {
            contained = true;
            break;
        }
    }
    if (!contained) {
        return ExternalAiAccess::Denied;
    }
    if (grant->access == ExternalAiAccess::Editable) {
        // Sample libraries stay read-only. An editable grant never widens
        // access to the shipped sample data.
        return sampleLibrary_ ? ExternalAiAccess::ReadOnly : ExternalAiAccess::Editable;
    }
    return grant->access;
}

}  // namespace HarkenSearch
