// test_startup_reconciliation.cpp — T-296
// Verifies Database::enqueueStartupReconciliation dispatches one rescan
// per watched root so app startup / DB switch auto-rescans without user input.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "src/Database.h"

namespace fs = std::filesystem;

static int g_failures = 0;
static void check(bool cond, const char* desc) {
    if (cond) {
        std::printf("PASS: %s\n", desc);
    } else {
        std::printf("FAIL: %s\n", desc);
        ++g_failures;
    }
}

static fs::path makeTempDir(const char* tag) {
    fs::path p = fs::temp_directory_path() /
                 ("auro_t296_" + std::string(tag));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

static int queryMetaInt(Database& db, const char* key) {
    std::string sql = "SELECT value FROM app_meta WHERE key = '";
    sql += key;
    sql += "' LIMIT 1";
    Database::SqlResult res = db.executeRawQuery(sql);
    if (!res.success || res.rows.empty() || res.rows[0].empty()) return -1;
    return std::atoi(res.rows[0][0].c_str());
}

static void writeSizedFile(const fs::path& p, size_t byteCount) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    for (size_t i = 0; i < byteCount; ++i) {
        out.put(static_cast<char>('A' + (i % 26)));
    }
}

static std::string readTextFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    // Source contracts must not depend on Git's platform line-ending checkout.
    for (size_t pos = 0; (pos = text.find("\r\n", pos)) != std::string::npos;) {
        text.replace(pos, 2, "\n");
    }
    return text;
}

static void setEnvFlag(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int main() {
    const fs::path repoRoot = fs::path(__FILE__).parent_path().parent_path();
    const fs::path windowsMainPath = repoRoot / "src" / "WindowsMain.cpp";
    const fs::path mainPath = repoRoot / "src" / "main.cpp";
    const fs::path utilsPath = repoRoot / "src" / "Utils.cpp";
    const std::string windowsMain = readTextFile(windowsMainPath);
    const std::string mainSource = readTextFile(mainPath);
    const std::string utilsSource = readTextFile(utilsPath);
    check(!windowsMain.empty(), "WindowsMain.cpp source can be read for startup diagnostics contract");
    check(!mainSource.empty(), "main.cpp source can be read for startup empty-state contract");
    check(!utilsSource.empty(), "Utils.cpp source can be read for startup database canonicalization contract");
    check(windowsMain.find("DIAG_SYSTEM(\"App\", \"StartupLoadingStart\")") != std::string::npos,
          "Startup diagnostics include loading start marker");
    check(windowsMain.find("DIAG_SYSTEM(\"App\", \"StartupLoadingComplete") != std::string::npos,
          "Startup diagnostics include loading complete marker");
    check(windowsMain.find("BuildFlashUiWelcomeHtml") != std::string::npos,
          "Startup source includes a dedicated welcome HTML surface");
    check(windowsMain.find("return BuildFlashUiWelcomeHtml(settings, settings.userName);") != std::string::npos,
          "Startup login HTML delegates directly to the welcome surface");
    check(windowsMain.find("RunWebView2WelcomeScreen") == std::string::npos,
          "Startup no longer keeps a second dedicated welcome WebView stage");
    check(windowsMain.find("bool welcomeShown = false;") == std::string::npos,
          "Startup flow no longer keeps a redundant post-login welcomeShown guard");
    check(windowsMain.find("state.settings.showWelcomeOnStartup") == std::string::npos,
          "Startup welcome entry no longer depends on the old post-login preference gate");
    check(windowsMain.find("welcome_continue") != std::string::npos,
          "Startup welcome entry uses the continue message protocol");
    check(windowsMain.find("avatarPulse") != std::string::npos &&
              windowsMain.find("pulseHalo") != std::string::npos &&
              windowsMain.find("avatarInitials") != std::string::npos &&
              windowsMain.find("<div class=\"avatar-ring\"") == std::string::npos,
          "Startup welcome entry shows a minimal avatar pulse without a legacy spinner ring");
    check(windowsMain.find("BuildLocalDiceBearAvatarDataUri") != std::string::npos &&
              windowsMain.find("assets\\\\avatars\\\\dicebear\\\\lorelei-neutral") != std::string::npos &&
              windowsMain.find("startupAvatarDataUri") != std::string::npos,
          "Startup welcome entry reads the local DiceBear avatar pool without a runtime API request");
    check(windowsMain.find("loadingAvatarDataUri") != std::string::npos &&
              windowsMain.find("loadingAvatarBubble") != std::string::npos &&
              windowsMain.find("spinner-container{display:none!important;}") != std::string::npos &&
              windowsMain.find(".glass-card{width:min(312px,calc(100vw - 48px));min-height:300px") != std::string::npos,
          "Startup loading/preheat screen reuses the welcome-style avatar card instead of the old spinner card");
    check(windowsMain.find("Preparing your sound workspace...") != std::string::npos,
          "Startup welcome entry includes the sound-workspace warmup copy");
    check(windowsMain.find("\"Inter\", \"Noto Sans SC\", \"Noto Sans CJK SC\"") != std::string::npos,
          "Startup welcome entry uses an international font stack with CJK fallback");
    check(windowsMain.find("password.empty()") != std::string::npos &&
              windowsMain.find("const std::string displayNameUtf8") != std::string::npos,
          "Startup welcome entry displays backend user identity while legacy credential handling stays non-primary");
    check(windowsMain.find("switch (greeting.band)") != std::string::npos &&
              windowsMain.find("const std::string titleUtf8 = GetLocalizedString(greetKey);") != std::string::npos &&
              windowsMain.find("const std::string displayNameUtf8 = greeting.name.empty() ? std::string(L(Splash_DefaultUser)) : greeting.name;") != std::string::npos &&
              windowsMain.find("titleEl.textContent = welcomeTitle;") != std::string::npos &&
              windowsMain.find("nameLine.textContent = displayName;") != std::string::npos,
          "Startup welcome entry preserves the localized greeting phrase and user name");
    check(windowsMain.find("Startup welcome entry shown.") != std::string::npos,
          "Startup diagnostics include the welcome-entry marker");
    check(windowsMain.find("AutoReconcile: queued %d watched roots for rescan") != std::string::npos,
          "Startup diagnostics include auto-reconcile queued marker");
    check(windowsMain.find("AutoReconcile skipped: setting is opt-in and currently disabled") != std::string::npos,
          "Startup diagnostics include auto-reconcile skipped marker");
    check(windowsMain.find("Skipping startup search refresh for direct file playback.") != std::string::npos,
          "Startup diagnostics include direct-playback refresh skip marker");
    check(windowsMain.find("No database available on startup; waiting for explicit user setup.") != std::string::npos,
          "Startup diagnostics include explicit no-database onboarding marker");
    check(windowsMain.find("ResolveStartupDatabaseSelectionForMount") != std::string::npos,
          "Startup source includes guarded database selection instead of unconditional default fallback");
    check(mainSource.find("const bool requireDatabaseSetup = state.startupNeedsDatabaseSetup &&") != std::string::npos &&
              mainSource.find("(state.dbSwitchInProgress || !state.database.isOpen())") != std::string::npos &&
              mainSource.find("ImGui::OpenPopup(L(Dlg_NewDB));") != std::string::npos &&
              mainSource.find("RenderNoDatabaseSetupEmptyState(") == std::string::npos,
          "First-library setup reuses one new-database modal, without a second empty-state panel");
    check(mainSource.find("state.startupNeedsDatabaseSetup && !state.database.isOpen()") != std::string::npos,
          "No-database empty state is gated by explicit startup state");
    check(mainSource.find("DatabaseSetupButton(createLabel, ImVec2(setupButtonWidth, 32), setupPalette, DatabaseSetupAction::Create)") != std::string::npos &&
              mainSource.find("SwitchDatabase(state, fullPath, true, &newDbError, newDbCacheMode, confirmOverwrite, deferredStart)") != std::string::npos,
          "First-library setup creates the named database through the guarded switch flow");
    check(mainSource.find("OpenDatabaseAndRestart(state, openPath);") != std::string::npos ||
              mainSource.find("SwitchDatabase(state, openPath);") != std::string::npos,
          "No-database empty state opens the selected database through the guarded open/switch flow");
    check(windowsMain.find("title = Lw(WinTitle_NoDatabase);") != std::string::npos &&
              windowsMain.find("SetWindowTextW(hwnd, title.c_str());") != std::string::npos,
          "Startup no-database shell state updates the window title");
    check(utilsSource.find("if (pathUtf8.empty()) {\n        return {};\n    }") != std::string::npos,
          "Empty database paths remain empty instead of being canonicalized into a default startup database");
    check(utilsSource.find("if (pathUtf8.empty() || IsLegacyAuroOwnedDatabasePathUtf8(pathUtf8))") == std::string::npos,
          "Empty database paths no longer share the legacy-owned default-database fallback branch");
    const size_t loginBypassPos = windowsMain.find("const bool bypassStartupLogin =");
    const size_t loginBypassEnd = loginBypassPos == std::string::npos
                                      ? std::string::npos
                                      : windowsMain.find(";", loginBypassPos);
    const bool devModeRunsLoginGate =
        loginBypassPos != std::string::npos &&
        loginBypassEnd != std::string::npos &&
        windowsMain.substr(loginBypassPos, loginBypassEnd - loginBypassPos)
                .find("s_isDevMode") == std::string::npos;
    check(devModeRunsLoginGate,
          "Dev mode still runs the startup login gate");
    const size_t loadingBypassPos = windowsMain.find("const bool bypassStartupLoadingGate =");
    const bool devModeRunsLoadingGate =
        loadingBypassPos != std::string::npos &&
        windowsMain.find("startupBypassForDirectFilePlayback ||", loadingBypassPos) != std::string::npos &&
        windowsMain.find("s_isDevMode ||", loadingBypassPos) == std::string::npos;
    check(devModeRunsLoadingGate,
          "Dev mode still runs the startup loading and warmup gate");
    check(windowsMain.find("StartDeferredSidebarBootstrap(state, true, true);") != std::string::npos,
          "Startup always loads playlist metadata; dev mode must not hide previous playlists");
    check(windowsMain.find("StartDeferredSidebarBootstrap(state, true, !s_isDevMode);") == std::string::npos,
          "Startup playlist bootstrap must not be gated off by dev mode");
    const size_t loginCallStart =
        windowsMain.find("RunWebView2LoginScreen(hwnd, state.settings, &wv2Controller, &wv2Webview, &wv2Loader);");
    const size_t loadingCallStart =
        windowsMain.find("RunWebView2LoadingScreen(hwnd, state, state.settings, wv2Controller, wv2Webview, runStartupPhases, finalizeStartupOnUi);");
    check(loginCallStart != std::string::npos &&
              loadingCallStart != std::string::npos &&
              loginCallStart < loadingCallStart,
          "Welcome entry WebView runs before the loading stage");
    check(windowsMain.find("welcomeShown = true;") == std::string::npos,
          "Startup flow no longer tracks a second welcome stage before loading");

    {
        fs::path packageDir = makeTempDir("package_size");
        std::vector<std::pair<const char*, size_t>> files = {
            {"main.db", 11},
            {"main.db-wal", 13},
            {"main.db-shm", 17},
            {"blobs.db", 19},
            {"blobs.db-wal", 23},
            {"waveforms.db", 29},
            {"waveforms.db-shm", 31},
        };
        uint64_t expectedPackageBytes = 0;
        for (const auto& f : files) {
            writeSizedFile(packageDir / f.first, f.second);
            expectedPackageBytes += static_cast<uint64_t>(f.second);
        }

        uint64_t actualPackageBytes =
            Database::GetDatabasePackageSize((packageDir / "main.db").string());
        check(actualPackageBytes == expectedPackageBytes,
              "Database package size includes main DB, sidecars, and SQLite -wal/-shm files");

        RepairCenterSnapshot packageSnapshot =
            Database::InspectDatabasePackageForRepair((packageDir / "main.db").string(), 12);
        check(packageSnapshot.packageBytes == expectedPackageBytes,
              "Repair snapshot reports full package bytes");
        check(packageSnapshot.mainDbBytes == 11 &&
              packageSnapshot.blobsDbBytes == 19 &&
              packageSnapshot.waveformsDbBytes == 29,
              "Repair snapshot separates main, blob, and waveform DB bytes");
        check(packageSnapshot.walBytes == 36 && packageSnapshot.shmBytes == 48,
              "Repair snapshot separates SQLite WAL and SHM bytes");
        check(packageSnapshot.walSizeWarning && packageSnapshot.needsAttention,
              "Repair snapshot warns when WAL bytes exceed threshold");

        std::error_code ec;
        fs::remove_all(packageDir, ec);
    }

    fs::path dbDir = makeTempDir("db");
    std::string dbPath = (dbDir / "main.db").string();

    // Two real (empty) directories to act as watched roots — the worker
    // will scan them but it's a no-op since they're empty.
    fs::path rootA = makeTempDir("rootA");
    fs::path rootB = makeTempDir("rootB");

    Database db;
    bool opened = db.open(dbPath, true);
    check(opened, "Database opens at temp path");
    if (!opened) return 1;
    check(queryMetaInt(db, "cover_hash_version") == 2,
          "Startup cover hash migration records its version");

    Database::SqlResult sidecarRes =
        db.executeRawQuery("SELECT COUNT(*) FROM blobs_db.cover_thumbs;");
    check(sidecarRes.success, "Cover sidecar table is attached during startup");

    Database::CacheSizeReport cacheReport = db.getCacheSizeReport();
    check(cacheReport.packageBytes >= cacheReport.mainDbBytes,
          "Cache size report includes package and main DB bytes");
    check(cacheReport.mainPageCount > 0,
          "Cache size report includes SQLite page counts");
    std::string compactError;
    check(db.compactCachePackage(compactError),
          "Manual cache package compaction succeeds on a temp DB");

    // Startup reconciliation is explicit opt-in: normal startup must not
    // silently rescan roots and redo cover/waveform maintenance.
    setEnvFlag("AURA_ENABLE_STARTUP_RECONCILIATION", "0");
    db.mergeLibraryRoots({rootA.string(), rootB.string()});
    int n0 = db.enqueueStartupReconciliation();
    check(n0 == 0, "Startup reconciliation is disabled by default");

    setEnvFlag("AURA_ENABLE_STARTUP_RECONCILIATION", "1");
    // Register two roots and reconcile.
    db.mergeLibraryRoots({rootA.string(), rootB.string()});
    int n2 = db.enqueueStartupReconciliation();
    check(n2 == 2, "Two watched roots → dispatched 2");

    // Idempotent on the same call (worker may have processed by now,
    // but the dispatch count itself should equal the number of non-empty
    // roots regardless of dedupe).
    int n2b = db.enqueueStartupReconciliation();
    check(n2b == 2, "Second call also reports 2 dispatched");

    // Empty paths are skipped.
    db.mergeLibraryRoots({std::string()});
    int nSkip = db.enqueueStartupReconciliation();
    check(nSkip == 2, "Empty path in roots is skipped, count remains 2");

    fs::path missingRoot = fs::temp_directory_path() / "auro_t296_missing_root";
    std::error_code missingRootEc;
    fs::remove_all(missingRoot, missingRootEc);
    db.mergeLibraryRoots({missingRoot.string()});
    db.executeRawQuery(
        "INSERT OR REPLACE INTO waveforms_db.waveforms"
        "(file_id, status, error_code, retry_count, updated_at, cache_key) "
        "VALUES (987654321, 'failed', 'decode_failed', 1, 0, 'repair-test')");
    db.executeRawQuery(
        "INSERT OR REPLACE INTO cover_cache_registry"
        "(cover_key, rep_path, is_embedded, normalized_key) "
        "VALUES ('E|repair_missing_thumb', 'missing.wav', 1, 'E|repair_missing_thumb')");
    RepairCenterSnapshot repair = db.getRepairCenterSnapshot(
        (std::numeric_limits<uint64_t>::max)());
    check(repair.libraryRootCount == 3,
          "Repair snapshot counts persisted library roots");
    check(repair.missingLibraryRootCount == 1 &&
          repair.missingLibraryRoots.size() == 1,
          "Repair snapshot identifies missing library roots");
    check(repair.failedWaveformRows >= 1,
          "Repair snapshot counts failed waveform rows");
    check(repair.missingCoverThumbRows >= 1,
          "Repair snapshot counts cover registry rows missing thumbnails");
    check(repair.needsAttention,
          "Repair snapshot marks actionable health issues");

    db.close();

    // Cleanup
    std::error_code ec;
    fs::remove_all(dbDir, ec);
    fs::remove_all(rootA, ec);
    fs::remove_all(rootB, ec);

    if (g_failures == 0) {
        std::printf("\nAll T-296 startup reconciliation checks passed.\n");
        return 0;
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return 1;
}
