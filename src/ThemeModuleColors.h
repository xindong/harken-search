#pragma once

#include "imgui.h"

struct ThemeColorScheme;

struct ThemeModuleColors {
    struct Surface {
        ImVec4 canvas;
        ImVec4 toolbar;
        ImVec4 panel;
        ImVec4 elevated;
        ImVec4 input;
        ImVec4 border;
    } surface;

    struct TopBar {
        ImVec4 background;
        ImVec4 accent;
        ImVec4 controlAccent;
        ImVec4 text;
        ImVec4 border;
    } topBar;

    struct Controls {
        ImVec4 accent;
        ImVec4 frame;
        ImVec4 border;
        ImVec4 searchAction;
        ImVec4 transferAction;
    } controls;

    struct Results {
        ImVec4 background;
        ImVec4 header;
        ImVec4 text;
        ImVec4 secondaryText;
        ImVec4 outline;
        ImVec4 frameOutline;
        ImVec4 selection;
        ImVec4 activeBorder;
        ImVec4 scrollbar;
        ImVec4 tabBackground;
        ImVec4 tabHover;
        ImVec4 tabActive;
        ImVec4 gradientStart;
        ImVec4 gradientEnd;
        ImVec4 playedText;
    } results;

    struct Metadata {
        ImVec4 background;
        ImVec4 text;
        ImVec4 label;
        ImVec4 scrollbar;
        ImVec4 coverWall;
    } metadata;

    struct FilterBrowser {
        ImVec4 background;
        ImVec4 section;
        ImVec4 button;
        ImVec4 outline;
        ImVec4 text;
        ImVec4 heading;
        ImVec4 treeText;
        ImVec4 selection;
        ImVec4 treeSelection;
        ImVec4 locate;
        ImVec4 searchHitText;
        ImVec4 tagText;
    } filterBrowser;

    struct Waveform {
        ImVec4 background;
        ImVec4 primary;
        ImVec4 secondary;
        ImVec4 playhead;
        ImVec4 border;
        ImVec4 splitter;
        ImVec4 splitterLine;
        ImVec4 footer;
    } waveform;

    struct Transfer {
        ImVec4 action;
        ImVec4 historyBackground;
        ImVec4 historyText;
    } transfer;

    struct Navigation {
        ImVec4 historyBackground;
        ImVec4 historyActive;
        ImVec4 modeBackground;
        ImVec4 modeActive;
    } navigation;

    struct Signal {
        ImVec4 level;
        ImVec4 clip;
        ImVec4 playhead;
    } signal;

    struct Transport {
        ImVec4 background;
        ImVec4 accent;
        ImVec4 text;
        ImVec4 timecodeText;
        ImVec4 timecodeHover;
        ImVec4 level;
        ImVec4 waveform;
        ImVec4 meterWarning;
        ImVec4 border;
    } transport;

    struct Meter {
        ImVec4 background;
        ImVec4 base;
        ImVec4 mid;
        ImVec4 warning;
        ImVec4 clip;
        ImVec4 scaleText;
    } meter;

    struct Diagnostic {
        ImVec4 background;
        ImVec4 text;
        ImVec4 accent;
        ImVec4 mutedText;
    } diagnostic;

    struct Overlay {
        ImVec4 background;
        ImVec4 panel;
        ImVec4 border;
    } overlay;

    struct Sidebar {
        ImVec4 railDivider;
        ImVec4 inactive;
        ImVec4 hover;
        ImVec4 active;
        ImVec4 activeStrip;
        ImVec4 subpane;
        ImVec4 divider;
    } sidebar;

    struct Wwise {
        ImVec4 connected;
        ImVec4 failed;
        ImVec4 neutral;
        ImVec4 newItem;
        ImVec4 replaced;
        ImVec4 unresolved;
        ImVec4 leafBadge;
        ImVec4 parentBadge;
        ImVec4 labelText;
        ImVec4 bodyText;
    } wwise;
};

ThemeModuleColors ResolveThemeModuleColors(const ThemeColorScheme& scheme);
