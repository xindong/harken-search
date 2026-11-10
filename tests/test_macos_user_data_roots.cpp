#include "platform/harken_user_data_layout.h"
#include <cstdlib>
#include <iostream>
#include <filesystem>

int main() {
    namespace data = Platform::HarkenUserData;
    ::unsetenv("ARIS_PREVIEW_ISOLATION");
    const auto roots = data::ResolveRoots(); // Resolve only: never create daily data.
    const auto normal = data::WideToUtf8Path(data::CanonicalRoot(roots));
    if (!std::filesystem::path(normal).is_absolute() ||
        normal.find("/Library/Application Support/HarkenSearch") == std::string::npos ||
        data::PreviewIsolationActive()) return 1;
    std::cout << "PASS normal root resolves to Application Support without creating it\n";
    ::setenv("ARIS_PREVIEW_ISOLATION", "1", 1);
    const auto refused = data::EnsureLayout();
    if (!data::PreviewIsolationActive() || !data::PreviewIsolationRootUtf8().empty() ||
        !refused.readOnly || refused.noticeUtf8.empty() || !refused.activeSettingsDir.empty() ||
        data::PreviewIsolationAllowsDataPathUtf8(normal)) return 1;
    std::cout << "PASS isolation without a compiled root fails closed\n";
    return 0;
}
