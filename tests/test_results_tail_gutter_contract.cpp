#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

bool requireContains(const std::string& text, const std::string& needle, const char* message) {
    if (text.find(needle) != std::string::npos) return true;
    std::cerr << "FAIL: " << message << "\nmissing: " << needle << "\n";
    return false;
}

bool requireNotContains(const std::string& text, const std::string& needle, const char* message) {
    if (text.find(needle) == std::string::npos) return true;
    std::cerr << "FAIL: " << message << "\nforbidden: " << needle << "\n";
    return false;
}

} // namespace

int main() {
    const std::string mainCpp = readText(std::filesystem::current_path() / "src" / "main.cpp");
    if (mainCpp.empty()) {
        std::cerr << "FAIL: src/main.cpp is readable\n";
        return 1;
    }

    bool ok = true;
    ok &= requireContains(mainCpp, "constexpr int kResultsFilePathColumnIndex = 16",
                          "FilePath keeps its stable physical table index");
    ok &= requireContains(mainCpp, "const ImGuiTable* previousResultsTable = ImGui::TableFindByID(resultsTableId)",
                          "FilePath visibility is read from the active table's own persisted state");
    ok &= requireContains(mainCpp, "const bool filePathVisible = previousResultsTable &&",
                          "hidden FilePath is distinguished before table sizing");
    ok &= requireContains(mainCpp, "const float resultsTableInnerWidth = filePathVisible ? innerWidth : 0.0f",
                          "hidden FilePath does not reserve a trailing stretch gutter");
    ok &= requireContains(mainCpp, "const ImGuiTableFlags resultsSizingPolicy = filePathVisible ?",
                          "only an enabled FilePath opts the table into stretch sizing");
    ok &= requireContains(mainCpp, "resultsSizingPolicy |",
                          "results table combines the conditional sizing policy with stable flags");
    ok &= requireContains(mainCpp,
                          "BeginTransientScrollbarTable(resultsTableName, totalColumns, tableFlags, ImVec2(resultsTableOuterWidth, 0.0f), resultsTableInnerWidth)",
                          "results table keeps the visibility-aware inner width inside its viewport");
    ok &= requireNotContains(mainCpp, "TableSetColumnEnabled(kResultsFilePathColumnIndex, false)",
                             "tail fix must not mutate persisted column visibility or reorder row emission");

    if (!ok) return 1;
    std::cout << "PASS: results tail gutter contract\n";
    return 0;
}
