#include "PreferenceDraft.h"
#include "SettingsCatalog.h"
#include <cassert>
#include <string>

struct Preferences { int volume = 80; bool realtime = true; std::string device = "system"; int windowWidth = 900; };

int main() {
    using namespace harken::settings;
    const auto volume = Search("  VOLUME  ", false);
    assert(std::any_of(volume.begin(), volume.end(), [](auto entry) { return entry->id == "volume"; }));
    assert(!Search(u8"静音 阈值", false).empty());
    assert(Search("this-does-not-exist", true).empty());
    assert(Search(u8"诊断", false).empty());
    assert(!Search(u8"诊断", true).empty());
    for (const auto& entry : Catalog()) {
        assert(!entry.id.empty() && !entry.zh.empty() && !entry.en.empty() && !entry.ja.empty());
        auto hits = Search(entry.zh, true);
        assert(std::find(hits.begin(), hits.end(), &entry) != hits.end());
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
}
