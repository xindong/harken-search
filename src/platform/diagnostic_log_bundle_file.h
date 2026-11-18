#pragma once

#include "DiagnosticLogBundle.h"

#include <cstddef>
#include <cstdint>

namespace Platform::DiagnosticLogBundleFile {

HarkenSearch::DiagnosticLogBundleResult Publish(
    const std::filesystem::path& root, int64_t clickMs,
    const void* bytes, size_t size, bool empty) noexcept;

} // namespace Platform::DiagnosticLogBundleFile
