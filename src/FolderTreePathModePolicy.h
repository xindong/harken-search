#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace aura::folder_tree {

enum class PathMode {
    Absolute = 0,
    Pilot = 1,
};

struct FolderTreeLayer {
    std::string name;
    std::string path;
};

inline std::string NormalizeFolderPathForCompare(std::string path) {
    if (path.empty()) return {};
    if (path.rfind("\\\\?\\UNC\\", 0) == 0) {
        path = "\\\\" + path.substr(8);
    } else if (path.rfind("\\\\?\\", 0) == 0) {
        path = path.substr(4);
    }
    std::replace(path.begin(), path.end(), '/', '\\');
    while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) {
        path.pop_back();
    }
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return path;
}

inline std::string NormalizeFolderPathPreserved(std::string path) {
    if (path.empty()) return {};
    if (path.rfind("\\\\?\\UNC\\", 0) == 0) {
        path = "\\\\" + path.substr(8);
    } else if (path.rfind("\\\\?\\", 0) == 0) {
        path = path.substr(4);
    }
    std::replace(path.begin(), path.end(), '/', '\\');
    while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) {
        path.pop_back();
    }
    return path;
}

inline std::vector<std::string> SplitFolderPathSegments(const std::string& path) {
    std::vector<std::string> parts;
    const bool unc = path.size() >= 2 && path[0] == '\\' && path[1] == '\\';
    size_t i = unc ? 2 : 0;
    std::string current = unc ? "\\\\" : "";
    for (; i < path.size(); ++i) {
        const char ch = path[i];
        if (ch == '\\' || ch == '/') {
            if (current == "\\\\") continue;
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty() && current != "\\\\") parts.push_back(current);
    return parts;
}

inline bool PathIsInsideRoot(const std::string& pathNorm, const std::string& rootNorm) {
    if (rootNorm.empty() || pathNorm.empty()) return false;
    if (pathNorm == rootNorm) return true;
    if (pathNorm.size() <= rootNorm.size()) return false;
    if (pathNorm.compare(0, rootNorm.size(), rootNorm) != 0) return false;
    const char sep = pathNorm[rootNorm.size()];
    return sep == '\\' || sep == '/';
}

inline std::string OutermostMatchingLibraryRoot(
    const std::string& folderPath,
    const std::vector<std::string>& libraryRoots)
{
    const std::string pathNorm = NormalizeFolderPathForCompare(folderPath);
    std::string bestPreserved;
    std::string bestNorm;
    for (const std::string& root : libraryRoots) {
        const std::string preserved = NormalizeFolderPathPreserved(root);
        const std::string rootNorm = NormalizeFolderPathForCompare(root);
        if (!PathIsInsideRoot(pathNorm, rootNorm)) continue;
        if (bestNorm.empty() || rootNorm.size() < bestNorm.size()) {
            bestNorm = rootNorm;
            bestPreserved = preserved;
        }
    }
    return bestPreserved;
}

inline std::string JoinFolderPathPrefix(
    const std::vector<std::string>& parts,
    size_t count)
{
    std::string current;
    for (size_t i = 0; i < count && i < parts.size(); ++i) {
        if (current.empty()) {
            current = parts[i];
            continue;
        }
        if (current == "\\\\") current += parts[i];
        else current += "\\" + parts[i];
    }
    return current;
}

template <typename Node, typename NameOf, typename PathOf, typename ChildrenOf>
inline void InsertLayersByFullPath(
    Node& root,
    const std::vector<FolderTreeLayer>& layers,
    NameOf nameOf,
    PathOf pathOf,
    ChildrenOf childrenOf)
{
    Node* current = &root;
    for (const FolderTreeLayer& layer : layers) {
        auto& children = childrenOf(*current);
        Node* found = nullptr;
        for (auto& child : children) {
            if (pathOf(child) == layer.path) {
                found = &child;
                break;
            }
        }
        if (!found) {
            children.emplace_back();
            found = &children.back();
            nameOf(*found) = layer.name;
            pathOf(*found) = layer.path;
        }
        current = found;
    }
}

inline std::vector<FolderTreeLayer> BuildFolderTreeLayers(
    const std::string& folderPath,
    const std::vector<std::string>& libraryRoots,
    PathMode mode)
{
    const std::string preserved = NormalizeFolderPathPreserved(folderPath);
    const std::vector<std::string> parts = SplitFolderPathSegments(preserved);
    std::vector<FolderTreeLayer> layers;
    if (parts.empty()) return layers;

    size_t start = 0;
    if (mode == PathMode::Pilot) {
        const std::string matchedRoot = OutermostMatchingLibraryRoot(preserved, libraryRoots);
        if (!matchedRoot.empty()) {
            const std::vector<std::string> rootParts = SplitFolderPathSegments(matchedRoot);
            if (!rootParts.empty() && rootParts.size() <= parts.size()) {
                start = rootParts.size() - 1;
            }
        }
    }

    for (size_t i = start; i < parts.size(); ++i) {
        FolderTreeLayer layer;
        layer.name = parts[i];
        layer.path = JoinFolderPathPrefix(parts, i + 1);
        layers.push_back(std::move(layer));
    }
    return layers;
}

}  // namespace aura::folder_tree
