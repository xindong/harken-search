#include "src/FolderTreePathModePolicy.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    } else {
        std::cout << "PASS: " << message << "\n";
    }
}

std::string LayerNames(const std::vector<aura::folder_tree::FolderTreeLayer>& layers) {
    std::string out;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (i) out += " / ";
        out += layers[i].name;
    }
    return out;
}

}  // namespace

int main() {
    using aura::folder_tree::BuildFolderTreeLayers;
    using aura::folder_tree::PathMode;

    const std::string folder = "H:\\Sound effect\\FX\\Fire";
    const std::vector<std::string> roots{"H:\\Sound effect\\FX"};

    const auto absolute = BuildFolderTreeLayers(folder, roots, PathMode::Absolute);
    Check(absolute.size() == 4, "absolute mode keeps disk-root layers");
    Check(LayerNames(absolute) == "H: / Sound effect / FX / Fire",
          "absolute names start at drive root");
    Check(absolute[0].path == "H:" &&
              absolute[1].path == "H:\\Sound effect" &&
              absolute[2].path == "H:\\Sound effect\\FX" &&
              absolute[3].path == "H:\\Sound effect\\FX\\Fire",
          "absolute layers keep full paths");

    const auto pilot = BuildFolderTreeLayers(folder, roots, PathMode::Pilot);
    Check(pilot.size() == 2, "pilot mode starts at library root last segment");
    Check(LayerNames(pilot) == "FX / Fire", "pilot names start at library root");
    Check(pilot[0].path == "H:\\Sound effect\\FX",
          "pilot root node path stays the full library root");
    Check(pilot[1].path == "H:\\Sound effect\\FX\\Fire",
          "pilot child node path stays the full subdirectory");

    const auto emptyRoots = BuildFolderTreeLayers(folder, {}, PathMode::Pilot);
    Check(emptyRoots.size() == absolute.size() &&
              LayerNames(emptyRoots) == LayerNames(absolute),
          "empty library roots fall back to absolute");

    const auto outside = BuildFolderTreeLayers(
        "D:\\Other\\Lib\\Shot", roots, PathMode::Pilot);
    Check(outside.size() == 4 && LayerNames(outside) == "D: / Other / Lib / Shot",
          "paths outside library roots stay absolute");

    const std::vector<std::string> nested{
        "H:\\Sound effect",
        "H:\\Sound effect\\FX",
    };
    const auto nestedLayers = BuildFolderTreeLayers(folder, nested, PathMode::Pilot);
    Check(nestedLayers.size() == 3 &&
              LayerNames(nestedLayers) == "Sound effect / FX / Fire",
          "nested library roots keep the parent above the child");
    Check(nestedLayers[0].path == "H:\\Sound effect" &&
              nestedLayers[1].path == "H:\\Sound effect\\FX",
          "nested root and parent keep their distinct full paths");

    const std::string unc = "\\\\server\\share\\FX\\Fire";
    const auto uncAbsolute = BuildFolderTreeLayers(unc, {}, PathMode::Absolute);
    Check(!uncAbsolute.empty() && uncAbsolute.front().path.rfind("\\\\server", 0) == 0,
          "UNC absolute first layer keeps \\\\server prefix");
    Check(uncAbsolute.back().path == "\\\\server\\share\\FX\\Fire",
          "UNC absolute leaf keeps the full UNC path");
    Check(uncAbsolute.front().path.find("server\\share") == std::string::npos ||
              uncAbsolute.front().path.rfind("\\\\", 0) == 0,
          "UNC absolute path is not rewritten as server\\share without prefix");

    struct Node {
        std::string name;
        std::string path;
        std::vector<Node> children;
    };
    auto insertByPath = [](Node& root, const std::vector<aura::folder_tree::FolderTreeLayer>& layers) {
        aura::folder_tree::InsertLayersByFullPath(
            root,
            layers,
            [](Node& node) -> std::string& { return node.name; },
            [](Node& node) -> std::string& { return node.path; },
            [](Node& node) -> std::vector<Node>& { return node.children; });
    };
    Node nestedTree;
    insertByPath(nestedTree, BuildFolderTreeLayers(
        "H:\\Sound effect", nested, PathMode::Pilot));
    insertByPath(nestedTree, BuildFolderTreeLayers(
        "H:\\Sound effect\\FX", nested, PathMode::Pilot));
    insertByPath(nestedTree, BuildFolderTreeLayers(
        folder, nested, PathMode::Pilot));
    Check(nestedTree.children.size() == 1 &&
              nestedTree.children[0].name == "Sound effect" &&
              nestedTree.children[0].children.size() == 1 &&
              nestedTree.children[0].children[0].name == "FX",
          "nested registered roots appear once below their parent");
    Check(nestedTree.children.size() == 1 &&
              nestedTree.children[0].children.size() == 1 &&
              nestedTree.children[0].children[0].path ==
                  "H:\\Sound effect\\FX",
          "nested tree child keeps the selectable full path");
    const std::vector<std::string> sameNameRoots{"H:\\LibA\\FX", "D:\\LibB\\FX"};
    Node tree;
    insertByPath(tree, BuildFolderTreeLayers("H:\\LibA\\FX\\Fire", sameNameRoots, PathMode::Pilot));
    insertByPath(tree, BuildFolderTreeLayers("D:\\LibB\\FX\\Shot", sameNameRoots, PathMode::Pilot));
    Check(tree.children.size() == 2, "same-name library roots stay separate top nodes");
    Check(tree.children[0].name == "FX" && tree.children[1].name == "FX",
          "pilot top names can still show the last segment");
    Check((tree.children[0].path == "H:\\LibA\\FX" &&
           tree.children[1].path == "D:\\LibB\\FX") ||
              (tree.children[0].path == "D:\\LibB\\FX" &&
               tree.children[1].path == "H:\\LibA\\FX"),
          "same-name library roots keep their full paths");

    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
