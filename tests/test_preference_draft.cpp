#include "PreferenceDraft.h"
#include "SettingsCatalog.h"
#include <cassert>
#include <cstdio>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <string>

struct Preferences { int volume = 80; bool realtime = true; std::string device = "system"; int windowWidth = 900; };

int main() {
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    using namespace harken::settings;
    g_currentLanguage = AppLanguage::Japanese;
    const auto volume = Search("  VOLUME  ", false);
    assert(std::any_of(volume.begin(), volume.end(), [](auto entry) { return entry->id == "volume"; }));
    assert(g_currentLanguage == AppLanguage::Japanese);
    assert(std::string(GetLocalizedString(LStr::Menu_File, AppLanguage::English)) == "File");
    assert(std::string(GetLocalizedString(static_cast<LStr>(-1), AppLanguage::Chinese)) == "???");
    assert(!Search(u8"静音 阈值", false).empty());
    assert(Search("this-does-not-exist", true).empty());
    assert(Search(u8"诊断", false).empty());
    assert(!Search(u8"诊断", true).empty());
    const auto available = Search("theme", true, false, false);
    assert(!available.empty());
    const auto& entries = Catalog();
    const auto theme = std::find_if(entries.begin(), entries.end(), [](const auto& e) { return e.id == "theme"; });
    const auto automatic = std::find_if(entries.begin(), entries.end(), [](const auto& e) { return e.id == "automatic"; });
    assert(theme != entries.end() && automatic != entries.end());
    assert(Available(*theme, true, false, true));
    assert(!Available(*theme, true, true, true));
    assert(Available(*automatic, true, false, true) && Available(*automatic, true, true, true));
    const auto scheduledSearch = Search("theme", true, true);
    assert(std::none_of(scheduledSearch.begin(), scheduledSearch.end(), [](auto e) { return e->id == "theme"; }));
    assert(std::none_of(available.begin(), available.end(), [](auto e) { return e->id == "day" || e->id == "night"; }));
    const auto visibleLufs = Search("LUFS", true, false, false);
    assert(std::none_of(visibleLufs.begin(), visibleLufs.end(), [](auto e) { return e->id == "showLufs"; }));
    for (const auto& entry : Catalog()) {
        assert(!entry.id.empty() && !entry.zh.empty() && !entry.en.empty() && !entry.ja.empty());
        auto hits = Search(entry.zh, true);
        assert((std::find(hits.begin(), hits.end(), &entry) != hits.end()) == Available(entry, true, true, true));
    }
    Preferences live;
    PreferenceDraft<Preferences> edit({Field(&Preferences::volume), Field(&Preferences::realtime), Field(&Preferences::device)});
    edit.Begin(live);
    edit.Value().volume = 42;
    assert(live.volume == 80 && edit.ChangedCount() == 1);
    edit.Value().volume = 80;
    assert(edit.ChangedCount() == 0);
    edit.Value().volume = 42;
    live.windowWidth = 1200;
    live.device = "external";
    auto candidate = edit.MergeInto(live);
    assert(candidate.volume == 42 && candidate.device == "external" && candidate.windowWidth == 1200);
    int writes = 0;
    assert(!edit.Commit(live, [&] { ++writes; assert(live.volume == 42); return false; }));
    assert(live.volume == 80 && live.device == "external" && edit.Value().volume == 42 && edit.ChangedCount() == 1);
    assert(edit.Commit(live, [&] { ++writes; return true; }));
    assert(live.volume == 42 && edit.ChangedCount() == 0 && writes == 2);
    edit.Value().device = "other";
    edit.Discard(live);
    assert(edit.Value().device == "external" && edit.ChangedCount() == 0);
    assert(edit.Commit(live, [&] { ++writes; return true; }));
    assert(writes == 2);
    edit.Value().realtime = false;
    edit.Begin(live);
    assert(edit.Value().realtime);
    live.volume = 42;
    edit.Begin(live);
    edit.Value() = Preferences{};
    edit.Value().volume = 42;
    // Reset, then return one value to its opening value: keep the displayed edit.
    if (edit.MergeInto(Preferences{}, true).volume != 42) {
        std::fprintf(stderr, "reset discarded a subsequent edit\n"); return 1;
    }
}
