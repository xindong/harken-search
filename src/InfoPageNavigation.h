#pragma once

// UI-only page selection; independent of audio, database and drawing.
enum class InfoPage { Metadata = 0, Artwork = 1, Transfer = 2, Playback = 3 };
struct InfoPageNavigation {
    int visibleMask = 15;
    int selected = 1;
    // A window shortcut, independent of the selectable content pages.
    bool similarSoundsVisible = true;
    static bool Available(InfoPage page, bool diagnostics) {
        const int index = static_cast<int>(page);
        return index >= 0 && index < 4 && (diagnostics || page != InfoPage::Transfer);
    }
    bool IsVisible(InfoPage page, bool diagnostics) const {
        return Available(page, diagnostics) && (visibleMask & (1 << static_cast<int>(page))) != 0;
    }
    int VisibleCount(bool diagnostics) const {
        int count = 0;
        for (int i = 0; i < 4; ++i) if (IsVisible(static_cast<InfoPage>(i), diagnostics)) ++count;
        return count;
    }
    void Normalize(bool diagnostics) {
        visibleMask &= 15;
        if (!VisibleCount(diagnostics)) visibleMask |= 2;
        if (!IsVisible(static_cast<InfoPage>(selected), diagnostics)) {
            for (int i = 0; i < 4; ++i) {
                if (IsVisible(static_cast<InfoPage>(i), diagnostics)) { selected = i; break; }
            }
        }
    }
    bool Select(InfoPage page, bool diagnostics) {
        if (!IsVisible(page, diagnostics) || selected == static_cast<int>(page)) return false;
        selected = static_cast<int>(page);
        return true;
    }
    bool SetVisible(InfoPage page, bool visible, bool diagnostics) {
        if (!Available(page, diagnostics) || IsVisible(page, diagnostics) == visible) return false;
        if (!visible && VisibleCount(diagnostics) <= 1) return false;
        const int bit = 1 << static_cast<int>(page);
        visibleMask = visible ? visibleMask | bit : visibleMask & ~bit;
        Normalize(diagnostics);
        return true;
    }
};
