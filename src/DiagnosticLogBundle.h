#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

struct DiagLogSnapshot;

namespace HarkenSearch {

struct DiagnosticLogBuildLabels {
    std::string_view version;
    std::string_view buildId;
};

enum class DiagnosticLogBundleStatus {
    Written,
    InvalidRoot,
    EncodeFailed,
    WriteFailed,
    PublishFailed,
    NameUnavailable,
};

struct DiagnosticLogBundleResult {
    DiagnosticLogBundleStatus status = DiagnosticLogBundleStatus::EncodeFailed;
    std::filesystem::path path;
    bool empty = false;
};

// Synchronous worker API. The root is a local output directory; path is set
// only after successful publication. Labels, when supplied, are build data.
[[nodiscard]] DiagnosticLogBundleResult WriteDiagnosticLogBundle(
    const DiagLogSnapshot& snapshot, const std::filesystem::path& root,
    std::optional<DiagnosticLogBuildLabels> labels = std::nullopt) noexcept;

} // namespace HarkenSearch
