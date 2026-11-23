#include "SimilarityIndexer.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#endif

namespace {

std::size_t WorkingSetBytes(bool peak) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        return static_cast<std::size_t>(peak ? counters.PeakWorkingSetSize
                                             : counters.WorkingSetSize);
    }
#endif
    return 0;
}

std::string Escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char character : value) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: SimilarityConcurrencyBench <library.db> <model-dir> <concurrency> <out.json>\n";
        return 2;
    }
    const std::string databasePath = argv[1];
    const std::string modelDirectory = argv[2];
    const std::string outputPath = argv[4];
    int concurrency = 1;
    try {
        concurrency = std::stoi(argv[3]);
    } catch (const std::exception&) {
        std::cerr << "invalid concurrency: " << argv[3] << '\n';
        return 2;
    }

    std::size_t invalidated = 0;
    {
        HarkenSearch::SimilarityAnalysisStore store;
        if (!store.Open(databasePath)) {
            std::cerr << "library open failed: " << store.Error() << '\n';
            return 3;
        }
        std::vector<HarkenSearch::SimilarityFile> files;
        if (!store.Files(files)) {
            std::cerr << "library read failed\n";
            return 3;
        }
        for (const auto& file : files) {
            store.Invalidate(file.id);
            ++invalidated;
        }
    }

    const std::size_t baselineBytes = WorkingSetBytes(false);
    const auto started = std::chrono::steady_clock::now();
    const auto result = HarkenSearch::BuildSimilarityIndex(
        databasePath, modelDirectory, {}, {},
        HarkenSearch::SimilarityIndexScope::ValidateAll, concurrency);
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    const std::size_t peakBytes = WorkingSetBytes(true);
    const double itemsPerMinute = elapsedMs > 0.0
        ? double(result.progress.completed) * 60000.0 / elapsedMs : 0.0;

    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "cannot write report: " << outputPath << '\n';
        return 4;
    }
    out.setf(std::ios::fixed);
    out.precision(2);
    out << "{\n"
        << "  \"concurrency\": " << concurrency << ",\n"
        << "  \"ok\": " << (result.ok ? "true" : "false") << ",\n"
        << "  \"cancelled\": " << (result.cancelled ? "true" : "false") << ",\n"
        << "  \"total\": " << result.progress.total << ",\n"
        << "  \"completed\": " << result.progress.completed << ",\n"
        << "  \"saved\": " << result.progress.saved << ",\n"
        << "  \"skipped\": " << result.progress.skipped << ",\n"
        << "  \"failed\": " << result.progress.failed << ",\n"
        << "  \"invalidated\": " << invalidated << ",\n"
        << "  \"elapsedMs\": " << elapsedMs << ",\n"
        << "  \"itemsPerMinute\": " << itemsPerMinute << ",\n"
        << "  \"baselineWorkingSetBytes\": " << baselineBytes << ",\n"
        << "  \"peakWorkingSetBytes\": " << peakBytes << ",\n"
        << "  \"peakDeltaBytes\": " << (peakBytes > baselineBytes ? peakBytes - baselineBytes : 0) << ",\n"
        << "  \"modelReady\": " << (result.modelError.empty() ? "true" : "false") << ",\n"
        << "  \"modelError\": \"" << Escape(result.modelError) << "\",\n"
        << "  \"error\": \"" << Escape(result.error) << "\"\n"
        << "}\n";
    if (!out) {
        std::cerr << "report write failed\n";
        return 4;
    }
    std::cout << "concurrency=" << concurrency << " elapsed_ms=" << elapsedMs
              << " completed=" << result.progress.completed << "/" << result.progress.total
              << " peak_mib=" << (peakBytes / (1024.0 * 1024.0)) << '\n';
    return result.ok ? 0 : 1;
}