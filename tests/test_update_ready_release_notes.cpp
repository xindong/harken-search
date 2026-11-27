#include "platform/update_persistence_port_test_hooks.h"
#include "update/UpdatePersistencePort.h"
#pragma warning(push, 0)
#include "../thirdparty/rlottie/src/lottie/zip/miniz.h"
#pragma warning(pop)
#include <filesystem>
#include <fstream>
#include <cstdio>
namespace fs = std::filesystem;
int main(int argc, char** argv) {
    if (argc < 2) return 2; // Caller supplies an isolated test directory.
    const auto root = fs::absolute(fs::u8path(argv[1]));
    if (fs::exists(root)) return 3; // Never reuse or erase an existing fixture.
    fs::create_directories(root / "staging");
    auto created = Platform::UpdatePersistence::Testing::CreateForTrustedRoot(root.u8string());
    if (!created.ok()) return 4;
    int failed = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failed;
    };
    const std::string expected = "{\"test\":\"only notes, never extract executables\"}";
    const auto writeZip = [&](const char* name, const std::string& data,
                              const char* alternateName = nullptr,
                              const std::string& alternateData = {}) {
        mz_zip_archive zip{};
        if (!mz_zip_writer_init_heap(&zip, 0, 0)) return false;
        bool ok = mz_zip_writer_add_mem(&zip, name, data.data(), data.size(), MZ_BEST_SPEED) != 0;
        if (ok && alternateName) {
            ok = mz_zip_writer_add_mem(&zip, alternateName, alternateData.data(),
                                       alternateData.size(), MZ_BEST_SPEED) != 0;
        }
        void* bytes = nullptr;
        size_t size = 0;
        ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &bytes, &size);
        mz_zip_writer_end(&zip);
        std::ofstream out(root / "staging/test.zip", std::ios::binary);
        if (ok) out.write(static_cast<const char*>(bytes), size);
        mz_free(bytes);
        return ok && out.good();
    };
    check(writeZip("aris_release_notes.json", expected), "fixture ZIP created");
    auto observed = created.port->ObserveStagedPatch("staging/test.zip");
    std::string result;
    check(created.port->ReadReadyReleaseNotes("staging/test.zip", observed.identity.size,
          observed.identity.sha256, result) && result == expected, "exact verified archive returns full notes");
    check(!fs::exists(root / "aris_release_notes.json"), "read does not extract files to disk");
    check(!created.port->ReadReadyReleaseNotes("staging/test.zip", observed.identity.size + 1,
          observed.identity.sha256, result) && result.empty(), "size mismatch clears stale output");
    check(!created.port->ReadReadyReleaseNotes("staging/test.zip", observed.identity.size,
          std::string(64, '0'), result), "hash mismatch rejected");
    check(!created.port->ReadReadyReleaseNotes("../test.zip", observed.identity.size,
          observed.identity.sha256, result), "outside-root read rejected");
    const std::string v2Notes = "{\"test\":\"notes from the new product\"}";
    check(writeZip("harkensearch_release_notes.json", v2Notes), "v2-only fixture created");
    observed = created.port->ObserveStagedPatch("staging/test.zip");
    check(created.port->ReadReadyReleaseNotes("staging/test.zip", observed.identity.size,
          observed.identity.sha256, result) && result == v2Notes, "v2 filename is readable");
    check(writeZip("aris_release_notes.json", expected, "harkensearch_release_notes.json",
                   v2Notes), "dual-filename fixture created");
    observed = created.port->ObserveStagedPatch("staging/test.zip");
    check(created.port->ReadReadyReleaseNotes("staging/test.zip", observed.identity.size,
          observed.identity.sha256, result) && result == v2Notes, "v2 filename wins regardless of ZIP order");
    check(writeZip("../aris_release_notes.json", expected), "wrong-entry fixture created");
    observed = created.port->ObserveStagedPatch("staging/test.zip");
    check(!created.port->ReadReadyReleaseNotes("staging/test.zip", observed.identity.size,
          observed.identity.sha256, result), "only exact archive entry is read");
    check(writeZip("aris_release_notes.json", std::string(8U * 1024U * 1024U + 1, 'a')), "large fixture created");
    observed = created.port->ObserveStagedPatch("staging/test.zip");
    check(!created.port->ReadReadyReleaseNotes("staging/test.zip", observed.identity.size,
          observed.identity.sha256, result) && result.empty(), "oversize notes are bounded and discarded");
    if (argc == 3) {
        fs::copy_file(fs::u8path(argv[2]), root / "staging/published.zip");
        observed = created.port->ObserveStagedPatch("staging/published.zip");
        check(created.port->ReadReadyReleaseNotes("staging/published.zip", observed.identity.size,
              observed.identity.sha256, result), "published update ZIP notes read through real port");
        std::ofstream output(root / "published-notes.json", std::ios::binary);
        output << result;
    }
    return failed ? 1 : 0;
}
