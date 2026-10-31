#include "BuiltinThemeScheme.h"

#include "AppState.h"
#include "Theme.h"

#include "imgui.h"

// Built-in preset theme data — hardcoded, no external JSON required.
// AARRGGBB hex → ImVec4(r, g, b, a)
ThemeColorScheme MakeBuiltinScheme(AppTheme t) {
    auto H = [](unsigned c) {
        return ImVec4(((c >> 16) & 0xFF) / 255.0f,
                      ((c >>  8) & 0xFF) / 255.0f,
                      ( c        & 0xFF) / 255.0f,
                      ((c >> 24) & 0xFF) / 255.0f);
    };
    ThemeColorScheme s;
    switch (t) {

// 经典深色 · Classic Dark
    case AppTheme::Dark:
        s.mainWindowBG           = H(0xff0f141b);
        s.menuBarBG              = H(0xff20262e);
        s.menuText               = H(0xffe6ecf3);
        s.frameBgBase            = H(0xff18202a);
        s.chromeBorder           = H(0xff34465d);
        s.popupBG                = H(0xf910141a);
        s.menuPopupBG            = H(0xf910141a);
        s.dialogBG               = H(0xf90f141a);
        s.dialogPanelBG          = H(0xff151c25);
        s.toolbarBG              = H(0xff10151c);
        s.historyCapsuleBG       = H(0xff11161d);
        s.historyCapsuleActiveBG = H(0xff4a5668); // dark slate-blue — readable with white text (was #bfc1c5 near-white, WCAG fail)
        s.modeCapsuleBG          = H(0xff11161d);
        s.modeCapsuleActiveBG    = H(0xff1e3a70); // visible blue (was near-black #0e1c33)
        s.toolbarAccent          = H(0xff6f98c1);
        s.controlAccent          = H(0xff6f98c1);
        s.transportBarBG         = H(0xff090e13);
        s.timecodeText           = H(0xedffdb4d);
        s.timecodeTextHover      = H(0xffffdb4d);
        s.interfaceText          = H(0xffffffff);
        s.resultsBG              = H(0xff131922);
        s.resultsHeaderBG        = H(0xff10161e);
        s.resultsText            = H(0xffe6ecf3);
        s.resultsOutline         = H(0xff31445b);
        s.resultsSelection       = H(0xff6f98c1);
        s.resultsActiveBorder    = H(0x50546b84);
        s.resultsScrollbar       = H(0xff7288a3);
        s.resultsTabBG           = H(0xf40b1117);
        s.resultsTabHover        = H(0xff141d28);
        s.resultsTabActive       = H(0xff223042);
        s.resultsSecondaryText   = H(0xff8893a3);
        s.resultsPlayedText      = H(0xfff2cc84);
        s.resultsGradientStart   = H(0xff131922);
        s.resultsGradientEnd     = H(0xff17202c);
        s.playlistGradientStart  = H(0xff6f98c1);
        s.playlistGradientEnd    = H(0xff496584);
        s.playlistGradientHighlight = H(0xffffffff);
        s.playlistGradientShadow = H(0xff131922);
        s.metadataPanelBG        = H(0xff11171f);
        s.metadataPanelText      = H(0xffe6ecf3);
        s.metadataPanelLabel     = H(0xff94a0b0);
        s.metadataPanelScrollbar = H(0xff7288a3);
        s.coverWallBG            = H(0xff10161d);
        s.waveformBG             = H(0xff0b0f15);
        s.waveformPanelBorder    = H(0xff2d3a4d);
        s.waveformSplitterBG     = H(0xff131720);
        s.waveformSplitterLine   = H(0xff1c2637);
        s.waveformFooterBG       = H(0xff0b0f14);
        s.waveformColor          = H(0xffafd7ff);
        s.waveformSecondaryColor = H(0xff8ab1c1);
        s.waveformPositionLine   = H(0xffffc832);
        s.miniWaveColor          = H(0xffafd7ff);
        s.miniWaveBG             = H(0x00000000);
        s.meterBg                = H(0xff11151c);
        s.meterBase              = H(0xff66b772);
        s.meterMid               = H(0xff8cc67f);
        s.meterWarning           = H(0xffd1c66b);
        s.meterClip              = H(0xffe06059);
        s.meterScaleText         = H(0xff8893a3);
        s.filterBrowserBG        = H(0xff0d141d);
        s.filterBrowserText      = H(0xffe6ecf3);
        s.filterBrowserHeading   = H(0xffc1c9d1);
        s.filterBrowserOutline   = H(0xff31445b);
        s.filterBrowserSelection = H(0xff6f98c1);
        s.filterBrowserScrollbar = H(0xff7288a3);
        s.filterBrowserSectionBG = H(0xff1a2533);
        s.filterBrowserButtonBG  = H(0xff1b2a3a);
        s.filterBrowserTreeText  = H(0xffe6ecf3);
        s.filterBrowserTreeSelection = H(0xff6f98c1);
        s.filterBrowserSearchHitText = H(0xff86aacd);
        s.filterBrowserLocateTint= H(0xff304a65);
        s.transferButtonBG       = H(0xff304a65);
        s.searchButtonBG         = H(0xff6f98c1);
        s.transferHistoryBG      = H(0xff131922);
        s.transferHistoryText    = H(0xffe6ecf3);
        s.spottingBG             = H(0xff131922);
        s.spottingText           = H(0xffe6ecf3);
        s.diagnosticOverlayBG    = H(0xea151c25);
        s.diagnosticOverlayText  = H(0xffe6ecf3);
        s.diagnosticOverlayAccent= H(0xff6f98c1);
        s.diagnosticOverlayMutedText = H(0xff8d939a);
        s.tabBG                  = H(0xff0b1117);
        s.volumeColor            = H(0xff31c750);
        s.volumeClipColor        = H(0xffff3333);
        s.filterBrowserTagText   = H(0xffc4d0df); // light slate — readable on dark frameBg
        s.presetDialCardActiveText   = H(0xffffffff); // white — visible on dark toolbar
        s.presetDialCardInactiveText = H(0xff8893a3); // muted gray
        s.presetDialAccent           = H(0xff6f98c1); // blue accent underline

// Wwise Transfer (F-034 /colorize) — Dark 语义色
        s.wwiseConnectOk         = H(0xff67e66b); // 鲜绿 — connected / Imported
        s.wwiseConnectFail       = H(0xffe66b6b); // 红 — disconnected / Failed
        s.wwiseDiffNeutral       = H(0xff9da9b8); // 蓝灰 — NoChange / Skipped
        s.wwiseDiffNew           = H(0xff85e68a); // 亮绿 — New 节点
        s.wwiseDiffReplaced      = H(0xffffb84d); // 琥珀 — Replaced 节点
        s.wwiseDiffUnresolved    = H(0xffff7b6e); // 暖红 — Unresolved 节点
        s.wwiseLeafBadge         = H(0xff67be7a); // 叶绿 — leaf 层标识
        s.wwiseParentBadge       = H(0xff7898c0); // 钢蓝 — parent 层标识

// Sidebar Activity Bar — Dark (玄夜)
        s.sidebarRailBG          = H(0xff070c14);  // slightly darker than filterBrowserBG
        s.sidebarRailDivider     = H(0x246f98c1);  // accent @ 14% alpha
        s.sidebarIconInactive    = H(0xff6f7f96);  // muted blue-gray
        s.sidebarIconHover       = H(0xffb1bccb);
        s.sidebarIconActive      = H(0xff6f98c1);  // = filterBrowserSelection
        s.sidebarActiveStrip     = H(0xff6f98c1);
        s.sidebarHeaderBG        = H(0xff070c14);  // = sidebarRailBG (unified frame)
        s.sidebarStatusBarBG     = H(0xff0a1119);  // filterBrowserBG x 0.925
        s.sidebarSubpaneBG       = H(0xff141d29);
        s.sidebarSubpaneDivider  = H(0xff243244);
        s.sidebarSubHeaderLine   = H(0x246f98c1);
        // F-004-ext Hearth: uses struct defaults (紫/橙渐变 + 金边)
        break;

// 极光 · Aurora
    case AppTheme::Ocean:
        s.mainWindowBG           = H(0xff141516);
        s.menuBarBG              = H(0xff0b131f);
        s.menuText               = H(0xffe2ebf4);
        s.frameBgBase            = H(0xff182739);
        s.chromeBorder           = H(0xff233a5c);
        s.popupBG                = H(0xf7101a28);
        s.menuPopupBG            = H(0xf7101a28);
        s.dialogBG               = H(0xf90f1826);
        s.dialogPanelBG          = H(0xff101b29);
        s.toolbarBG              = H(0xff0b131f);
        s.historyCapsuleBG       = H(0xff0b131d);
        s.historyCapsuleActiveBG = H(0xff026171);
        s.modeCapsuleBG          = H(0xff0b131e);
        s.modeCapsuleActiveBG    = H(0xff026171);
        s.toolbarAccent          = H(0xff38a8be);
        s.controlAccent          = H(0xff38a8be);
        s.transportBarBG         = H(0xff0a121c);
        s.timecodeText           = H(0xf9a3deea);
        s.timecodeTextHover      = H(0xffa7dfeb);
        s.interfaceText          = H(0xffffffff);
        s.resultsBG              = H(0xff0e1725);
        s.resultsHeaderBG        = H(0xff0b131f);
        s.resultsText            = H(0xffe2ebf4);
        s.resultsOutline         = H(0xff233a5c);
        s.resultsSelection       = H(0xff38a8be);
        s.resultsActiveBorder    = H(0xd8d0efee);
        s.resultsScrollbar       = H(0xff2d4f77);
        s.resultsTabBG           = H(0xf40b131f);
        s.resultsTabHover        = H(0xff285684);
        s.resultsTabActive       = H(0xff306699);
        s.resultsSecondaryText   = H(0xff7d8da2);
        s.resultsPlayedText      = H(0xfff2a5bf);
        s.resultsGradientStart   = H(0xff0e1925);
        s.resultsGradientEnd     = H(0xff0d2837);
        s.playlistGradientStart  = H(0xff38a8be);
        s.playlistGradientEnd    = H(0xff206b7a);
        s.playlistGradientHighlight = H(0xffffffff);
        s.playlistGradientShadow = H(0xff0e1725);
        s.metadataPanelBG        = H(0xff101b2c);
        s.metadataPanelText      = H(0xffe2ebf4);
        s.metadataPanelLabel     = H(0xff8ba2c0);
        s.metadataPanelScrollbar = H(0xff2d4f77);
        s.coverWallBG            = H(0xff0c1420);
        s.waveformBG             = H(0xff070b12);
        s.waveformPanelBorder    = H(0xff17263c);
        s.waveformSplitterBG     = H(0xff0d1522);
        s.waveformSplitterLine   = H(0xff101b2a);
        s.waveformFooterBG       = H(0xff070b12);
        s.waveformColor          = H(0xffc3f0ff);
        s.waveformSecondaryColor = H(0xff3d5c82);
        s.waveformPositionLine   = H(0xff362f32);
        s.miniWaveColor          = H(0xffc5f8fe);
        s.miniWaveBG             = H(0x00000000);
        s.meterBg                = H(0xff0c131d);
        s.meterBase              = H(0xff66b772);
        s.meterMid               = H(0xff8cc67f);
        s.meterWarning           = H(0xffd1c66b);
        s.meterClip              = H(0xffe06059);
        s.meterScaleText         = H(0xff7d8da2);
        s.filterBrowserBG        = H(0xff0c1622);
        s.filterBrowserText      = H(0xffe2ebf4);
        s.filterBrowserHeading   = H(0xffbecad7);
        s.filterBrowserOutline   = H(0xff233a5c);
        s.filterBrowserSelection = H(0xff38a8be);
        s.filterBrowserScrollbar = H(0xff2d4f77);
        s.filterBrowserSectionBG = H(0xff0c2f3e);
        s.filterBrowserButtonBG  = H(0xff193c54);
        s.filterBrowserTreeText  = H(0xffe2ebf4);
        s.filterBrowserTreeSelection = H(0xff38a8be);
        s.filterBrowserSearchHitText = H(0xff38c8db);
        s.filterBrowserLocateTint= H(0xff3a82bc);
        s.transferButtonBG       = H(0xff3a82bc);
        s.searchButtonBG         = H(0xff38a8be);
        s.transferHistoryBG      = H(0xff0e1725);
        s.transferHistoryText    = H(0xffe2ebf4);
        s.spottingBG             = H(0xff0e1725);
        s.spottingText           = H(0xffe2ebf4);
        s.diagnosticOverlayBG    = H(0xea101b29);
        s.diagnosticOverlayText  = H(0xffe2ebf4);
        s.diagnosticOverlayAccent= H(0xff38a8be);
        s.diagnosticOverlayMutedText = H(0xff8a939e);
        s.tabBG                  = H(0xff0b131f);
        s.volumeColor            = H(0xff31c750);
        s.volumeClipColor        = H(0xffff3333);
        s.filterBrowserTagText   = H(0xffbfd1e6);
        s.presetDialBg               = H(0xff151834);
        s.presetDialCardActiveBg     = H(0xfffafafc);
        s.presetDialCardActiveText   = H(0xffffffff);
        s.presetDialCardInactiveText = H(0xff7d8da2);
        s.presetDialAccent           = H(0xff38a8be);

        // ?? Wwise Transfer (F-034 /colorize) ? Ocean semantic tones ??
        s.wwiseConnectOk         = H(0xff7fd6a8);
        s.wwiseConnectFail       = H(0xffe08070);
        s.wwiseDiffNeutral       = H(0xff90a8c0);
        s.wwiseDiffNew           = H(0xff90e0b8);
        s.wwiseDiffReplaced      = H(0xfff5c078);
        s.wwiseDiffUnresolved    = H(0xffff9078);
        s.wwiseLeafBadge         = H(0xff7bc0a0);
        s.wwiseParentBadge       = H(0xff7098c0);

        // ?? Sidebar Activity Bar ? Ocean / Aurora ??
        s.sidebarRailBG          = H(0xff081018);
        s.sidebarRailDivider     = H(0x2638a8be);
        s.sidebarIconInactive    = H(0xff5e7a8e);
        s.sidebarIconHover       = H(0xff9dbdd0);
        s.sidebarIconActive      = H(0xff38a8be);
        s.sidebarActiveStrip     = H(0xff38a8be);
        s.sidebarHeaderBG        = H(0xff081018);
        s.sidebarStatusBarBG     = H(0xff0d1522);
        s.sidebarSubpaneBG       = H(0xff2e384a);
        s.sidebarSubpaneDivider  = H(0xff475263);
        s.sidebarSubHeaderLine   = H(0x2638a8be);

        // ?? Hearth waveform selection glow ??
        s.waveformSelectionDim       = H(0x7a06141a);
        s.waveformSelectionHearthTop = H(0x4750dcb4);
        s.waveformSelectionHearthBot = H(0x47f0c850);
        s.waveformSelectionEdge      = H(0xffffeb78);

        break;

// 柔雾粉 · Soft Blush
    case AppTheme::RoseCedar:
        // Impeccable refresh: restrained blush/rose-gray foundations; rose is
        // signal only, while waveform/data keeps a cool blue information axis.
        s.mainWindowBG           = H(0xfff6edf1);
        s.menuBarBG              = H(0xfff1e5ea);
        s.menuText               = H(0xff3b2931);
        s.frameBgBase            = H(0xfffcf9fa);
        s.chromeBorder           = H(0xffb996a7);
        s.popupBG                = H(0xfffcf9fa);
        s.menuPopupBG            = H(0xfffcf9fa);
        s.dialogBG               = H(0xfff6edf1);
        s.dialogPanelBG          = H(0xfffcf9fa);
        s.toolbarBG              = H(0xffeadbe2);
        s.historyCapsuleBG       = H(0xfff7eef2);
        s.historyCapsuleActiveBG = H(0xffb85c7a);
        s.modeCapsuleBG          = H(0xfff7eef2);
        s.modeCapsuleActiveBG    = H(0xffb85c7a);
        s.toolbarAccent          = H(0xffb85c7a);
        s.controlAccent          = H(0xffb85c7a);
        s.transportBarBG         = H(0xffe1cfd7);
        s.timecodeText           = H(0xff6f5864);
        s.timecodeTextHover      = H(0xff9f4968);
        s.interfaceText          = H(0xff3b2931);
        s.resultsBG              = H(0xfffcf9fa);
        s.resultsHeaderBG        = H(0xfff0e1e7);
        s.resultsText            = H(0xff3b2931);
        s.resultsOutline         = H(0xffc3a3b2);
        s.resultsSelection       = H(0xffc78399);
        s.resultsActiveBorder    = H(0x486fafd2);
        s.resultsScrollbar       = H(0xffcdb7c1);
        s.resultsTabBG           = H(0xfff2e5ea);
        s.resultsTabHover        = H(0xffe8d7de);
        s.resultsTabActive       = H(0xffe3c7d2);
        s.resultsSecondaryText   = H(0xff6f5864);
        s.resultsPlayedText      = H(0xff9f4968);
        s.resultsGradientStart   = H(0xfffcf9fa);
        s.resultsGradientEnd     = H(0xfff6edf1);
        s.playlistGradientStart  = H(0xffeadbe2);
        s.playlistGradientEnd    = H(0xfff7eef2);
        s.playlistGradientHighlight = H(0xfffffbfc);
        s.playlistGradientShadow = H(0xffded0d6);
        s.metadataPanelBG        = H(0xfff7eef2);
        s.metadataPanelText      = H(0xff3b2931);
        s.metadataPanelLabel     = H(0xff604852);
        s.metadataPanelScrollbar = H(0xffcdb7c1);
        s.coverWallBG            = H(0xfffcf9fa);
        s.waveformBG             = H(0xfffcf9fa);
        s.waveformPanelBorder    = H(0xffc3a3b2);
        s.waveformSplitterBG     = H(0xffeadbe2);
        s.waveformSplitterLine   = H(0xffcfb6c2);
        s.waveformFooterBG       = H(0xfff6edf1);
        s.waveformColor          = H(0xff6fafd2);
        s.waveformSecondaryColor = H(0xff91c4db);
        s.waveformPositionLine   = H(0xff9f4968);
        s.miniWaveColor          = H(0xff6fafd2);
        s.miniWaveBG             = H(0x00000000);
        s.meterBg                = H(0xfff1e5ea);
        s.meterBase              = H(0xff4f8f70);
        s.meterMid               = H(0xff6c8d48);
        s.meterWarning           = H(0xffa07828);
        s.meterClip              = H(0xffc95a5f);
        s.meterScaleText         = H(0xff6f5864);
        s.filterBrowserBG        = H(0xfff4eaee);
        s.filterBrowserText      = H(0xff3b2931);
        s.filterBrowserHeading   = H(0xff6f5864);
        s.filterBrowserOutline   = H(0xffaa8496);
        s.filterBrowserSelection = H(0xffead6de);
        s.filterBrowserScrollbar = H(0xffcdb7c1);
        s.filterBrowserSectionBG = H(0xfffcf9fa);
        s.filterBrowserButtonBG  = H(0xfffcf9fa);
        s.filterBrowserTreeText  = H(0xff3b2931);
        s.filterBrowserTreeSelection = H(0xffead6de);
        s.filterBrowserSearchHitText = H(0xffb85c7a);
        s.filterBrowserLocateTint= H(0xff6fafd2);
        s.transferButtonBG       = H(0xff6fafd2);
        s.searchButtonBG         = H(0xfffcf9fa);
        s.transferHistoryBG      = H(0xfffcf9fa);
        s.transferHistoryText    = H(0xff3b2931);
        s.spottingBG             = H(0xfffcf9fa);
        s.spottingText           = H(0xff3b2931);
        s.diagnosticOverlayBG    = H(0xeafbf7f8);
        s.diagnosticOverlayText  = H(0xff3b2931);
        s.diagnosticOverlayAccent= H(0xffb85c7a);
        s.diagnosticOverlayMutedText = H(0xf66f5864);
        s.tabBG                  = H(0xffeadbe2);
        s.volumeColor            = H(0xff6fafd2);
        s.volumeClipColor        = H(0xffc95a5f);
        s.filterBrowserTagText   = H(0xff3b2931);
        s.presetDialCardActiveText   = H(0xff3b2931);
        s.presetDialCardInactiveText = H(0xff6f5864);
        s.presetDialAccent           = H(0xff6fafd2);

        // ?? Wwise Transfer (F-034 /colorize) ? RoseCedar semantic colors, muted for light theme ??
        s.wwiseConnectOk         = H(0xff3d8f68); // connected / Imported
        s.wwiseConnectFail       = H(0xffb85a5f); // disconnected / Failed
        s.wwiseDiffNeutral       = H(0xff7c6570); // NoChange / Skipped
        s.wwiseDiffNew           = H(0xff4b9b72); // New node
        s.wwiseDiffReplaced      = H(0xffb07a45); // Replaced node
        s.wwiseDiffUnresolved    = H(0xffb85a5f); // Unresolved node
        s.wwiseLeafBadge         = H(0xff4f8f70); // leaf badge
        s.wwiseParentBadge       = H(0xff7c6570); // parent badge

        // ?? Sidebar Activity Bar ? RoseCedar: light chrome, muted mauve icons ??
        s.sidebarRailBG          = H(0xffefe2e7);
        s.sidebarRailDivider     = H(0x26d8c4cd);
        s.sidebarIconInactive    = H(0xff9c828d);
        s.sidebarIconHover       = H(0xff6a4b58);
        s.sidebarIconActive      = H(0xffb85c7a);
        s.sidebarActiveStrip     = H(0xffb85c7a);
        s.sidebarHeaderBG        = H(0xffefe2e7);
        s.sidebarStatusBarBG     = H(0xffe6d6dd);
        s.sidebarSubHeaderLine   = H(0x26000000);
        s.waveformSelectionDim       = H(0x38503f4a);
        s.waveformSelectionHearthTop = H(0x42b85c7a);
        s.waveformSelectionHearthBot = H(0x427c6570);
        s.waveformSelectionEdge      = H(0xff6fafd2);
        break;

    case AppTheme::LapisBlue:
        // Level-one refinement: preserve the navy canvas, gently separate nearby surfaces.
        s.mainWindowBG           = H(0xff34485f);
        s.menuBarBG              = H(0xff2a3e55);
        s.menuText               = H(0xfff5f8fc);
        s.frameBgBase            = H(0xff3b526c);
        s.chromeBorder           = H(0xff99b0c6);
        s.popupBG                = H(0xff26394d);
        s.menuPopupBG            = H(0xff26394d);
        s.dialogBG               = H(0xff26394d);
        s.dialogPanelBG          = H(0xff2b4056);
        s.toolbarBG              = H(0xff223246);
        s.historyCapsuleBG       = H(0xff354c65);
        s.historyCapsuleActiveBG = H(0xff3d6390);
        s.modeCapsuleBG          = H(0xff26394d);
        s.modeCapsuleActiveBG    = H(0xff3d6390);
        s.toolbarAccent          = H(0xffaccbe8);
        s.controlAccent          = H(0xffaccbe8);
        s.transportBarBG         = H(0xff2e445c);
        s.timecodeText           = H(0xfff5f8fc);
        s.timecodeTextHover      = H(0xffaccbe8);
        s.interfaceText          = H(0xfff5f8fc);
        s.resultsBG              = H(0xff34485f);
        s.resultsHeaderBG        = H(0xff30445b);
        s.resultsText            = H(0xfff5f8fc);
        s.resultsOutline         = H(0xff99b0c6);
        s.resultsSelection       = H(0xff456181);
        s.resultsActiveBorder    = H(0xffaccbe8);
        s.resultsScrollbar       = H(0xff829bb5);
        s.resultsTabBG           = H(0xff384d65);
        s.resultsTabHover        = H(0xff456181);
        s.resultsTabActive       = H(0xff456181);
        s.resultsSecondaryText   = H(0xffe5ecf4);
        s.resultsPlayedText      = H(0xffe5c69d);
        s.resultsGradientStart   = H(0xff34485f);
        s.resultsGradientEnd     = H(0xff384d65);
        s.playlistGradientStart  = H(0xff354b64);
        s.playlistGradientEnd    = H(0xff354b64);
        s.playlistGradientHighlight = H(0xff405a76);
        s.playlistGradientShadow = H(0xff2a4058);
        s.metadataPanelBG        = H(0xff374d67);
        s.metadataPanelText      = H(0xfff5f8fc);
        s.metadataPanelLabel     = H(0xffe5ecf4);
        s.metadataPanelScrollbar = H(0xff829bb5);
        s.coverWallBG            = H(0xff354b64);
        s.waveformBG             = H(0xff2b4056);
        s.waveformPanelBorder    = H(0xff99b0c6);
        s.waveformSplitterBG     = H(0xff2a4058);
        s.waveformSplitterLine   = H(0xff99b0c6);
        s.waveformFooterBG       = H(0xff2e445c);
        s.waveformColor          = H(0xffffffff);
        s.waveformSecondaryColor = H(0xffb6cade);
        s.waveformPositionLine   = H(0xffe5c69d);
        s.miniWaveColor          = H(0xffffffff);
        s.miniWaveBG             = H(0x00000000);
        s.meterBg                = H(0xff253a50);
        s.meterBase              = H(0xff85c5aa);
        s.meterMid               = H(0xffadc68c);
        s.meterWarning           = H(0xffe3bd79);
        s.meterClip              = H(0xffe68b88);
        s.meterScaleText         = H(0xfff5f8fc);
        s.filterBrowserBG        = H(0xffffffff);
        s.filterBrowserText      = H(0xff203b55);
        s.filterBrowserHeading   = H(0xff203b55);
        s.filterBrowserOutline   = H(0xff8195a9);
        s.filterBrowserSelection = H(0xffdceaf5);
        s.filterBrowserScrollbar = H(0xff6587a5);
        s.filterBrowserSectionBG = H(0xffedf3f8);
        s.filterBrowserButtonBG  = H(0xffe3edf5);
        s.filterBrowserTreeText  = H(0xff203b55);
        s.filterBrowserTreeSelection = H(0xff203b55);
        s.filterBrowserSearchHitText = H(0xff366387);
        s.filterBrowserLocateTint= H(0xff527fa5);
        s.transferButtonBG       = H(0xff7197bb);
        s.searchButtonBG         = H(0xff7197bb);
        s.transferHistoryBG      = H(0xff32485f);
        s.transferHistoryText    = H(0xfff5f8fc);
        s.spottingBG             = H(0xff32485f);
        s.spottingText           = H(0xfff5f8fc);
        s.diagnosticOverlayBG    = H(0xff32485f);
        s.diagnosticOverlayText  = H(0xfff5f8fc);
        s.diagnosticOverlayAccent= H(0xffaccbe8);
        s.diagnosticOverlayMutedText = H(0xffe5ecf4);
        s.tabBG                  = H(0xff30445b);
        s.volumeColor            = H(0xff7197bb);
        s.volumeClipColor        = H(0xffe68b88);
        s.filterBrowserTagText   = H(0xff203b55);
        s.presetDialCardActiveText   = H(0xfff5f8fc);
        s.presetDialCardInactiveText = H(0xffe5ecf4);
        s.presetDialAccent           = H(0xffaccbe8);

        s.wwiseConnectOk         = H(0xff0e7c50); // 深翠绿 — connected / Imported
        s.wwiseConnectFail       = H(0xffb83a3a); // 深红 — disconnected / Failed
        s.wwiseDiffNeutral       = H(0xff5a6b85); // 琉璃灰 — NoChange / Skipped
        s.wwiseDiffNew           = H(0xff1d8c5f); // 鲜绿 — New 节点
        s.wwiseDiffReplaced      = H(0xffc77a1f); // 琉璃金 — Replaced 节点
        s.wwiseDiffUnresolved    = H(0xffa33f3f); // 深珊瑚 — Unresolved 节点
        s.wwiseLeafBadge         = H(0xff1e7a8c); // 琉璃青 — leaf 层标识
        s.wwiseParentBadge       = H(0xff3f5c8a); // 深琉璃 — parent 层标识

        s.sidebarRailBG          = H(0xfff4f7fa);
        s.sidebarRailDivider     = H(0xff8195a9);
        s.sidebarIconInactive    = H(0xff536c82);
        s.sidebarIconHover       = H(0xff294c6b);
        s.sidebarIconActive      = H(0xff386e9d);
        s.sidebarActiveStrip     = H(0xff386e9d);
        s.sidebarHeaderBG        = H(0xffffffff);
        s.sidebarStatusBarBG     = H(0xffffffff);
        s.sidebarSubpaneBG       = H(0xffedf3f8);
        s.sidebarSubpaneDivider  = H(0xff8195a9);
        s.sidebarSubHeaderLine   = H(0xff8195a9);
        s.waveformSelectionDim       = H(0x80081026);  // rgba(8,16,38,0.50)
        s.waveformSelectionHearthTop = H(0x478c64f0);  // 紫 rgba(140,100,240, 0.28)
        s.waveformSelectionHearthBot = H(0x47dc64c8);  // 玫 rgba(220,100,200, 0.28)
        s.waveformSelectionEdge      = H(0xffd9e8f5);
        break;

// 海玻璃 · Sea Glass
    case AppTheme::JadeMist:
        s.mainWindowBG           = H(0xfffdfbf7);
        s.menuBarBG              = H(0xffffeed4);
        s.menuText               = H(0xff01405f);
        s.frameBgBase            = H(0xffffffff);
        s.chromeBorder           = H(0x55888ebf);
        s.popupBG                = H(0xffffeed4);
        s.menuPopupBG            = H(0xfffff6ea);
        s.dialogBG               = H(0xfffdf7ef);
        s.dialogPanelBG          = H(0xfff8efe4);
        s.toolbarBG              = H(0xffffeed4);
        s.historyCapsuleBG       = H(0xffe8eaf5);
        s.historyCapsuleActiveBG = H(0xffffffff); // restore original JadeMist stock pill.
        s.modeCapsuleBG          = H(0xffe8eaf5);
        s.modeCapsuleActiveBG    = H(0xffffffff);
        s.toolbarAccent          = H(0xffca9584);
        s.controlAccent          = H(0xff246e68);
        s.transportBarBG         = H(0xff0e3d48);
        s.timecodeText           = H(0xfffff3e8);
        s.timecodeTextHover      = H(0xffca9584);
        s.interfaceText          = H(0xff01405f);
        s.resultsBG              = H(0xffffffff);
        s.resultsHeaderBG        = H(0xffffeed4);
        s.resultsText            = H(0xff01405f);
        s.resultsOutline         = H(0xffcdd6e8);
        s.resultsSelection       = H(0xff246e68);
        s.resultsScrollbar       = H(0xff98aac8);
        s.resultsTabBG           = H(0xffffeed4);
        s.resultsTabHover        = H(0xffa5b4d4);
        s.resultsTabActive       = H(0xff78a8a0);
        s.resultsSecondaryText   = H(0xff7180a8);
        s.resultsPlayedText      = H(0xffca9584);
        s.resultsGradientStart   = H(0xffffffff);
        s.resultsGradientEnd     = H(0xfff2f0f8);
        s.playlistGradientStart  = H(0xff246e68);
        s.playlistGradientEnd    = H(0xff698adb);
        s.playlistGradientHighlight = H(0xffffeed4);
        s.playlistGradientShadow = H(0xff888ebf);
        s.metadataPanelBG        = H(0xfffdfbf7);
        s.metadataPanelText      = H(0xff01405f);
        s.metadataPanelLabel     = H(0xffca9584);
        s.metadataPanelScrollbar = H(0xffa5b4d4);
        s.coverWallBG            = H(0xffffffff);
        s.waveformBG             = H(0xfffdfbf7);
        s.waveformPanelBorder    = H(0xffcdd6e8);
        s.waveformSplitterBG     = H(0xfff5f3fa);
        s.waveformSplitterLine   = H(0xffe8eaf5);
        s.waveformFooterBG       = H(0xff0e3d48);
        s.waveformColor          = H(0xff24908a);
        s.waveformSecondaryColor = H(0xffa5b4d4);
        s.waveformPositionLine   = H(0xffca9584);
        s.meterBg                = H(0xfff5f7fb);
        s.meterBase              = H(0xff99cabe);
        s.meterMid               = H(0xff246e68);
        s.meterWarning           = H(0xffca9584);
        s.meterClip              = H(0xffd85a5a);
        s.meterScaleText         = H(0xff01405f);
        s.filterBrowserBG        = H(0xffffffff);
        s.filterBrowserText      = H(0xff01405f);
        s.filterBrowserHeading   = H(0xff698adb);
        s.filterBrowserOutline   = H(0xffcdd6e8);
        s.filterBrowserSelection = H(0xff246e68);
        s.filterBrowserScrollbar = H(0xffa5b4d4);
        s.filterBrowserSectionBG = H(0xffffeed4);
        s.filterBrowserButtonBG  = H(0xfffdfbf7);
        s.filterBrowserTreeText  = H(0xff01405f);
        s.filterBrowserTreeSelection = H(0xff698adb);
        s.filterBrowserSearchHitText = H(0xff1d6d55);
        s.filterBrowserLocateTint= H(0xffca9584);
        s.transferButtonBG       = H(0xff246e68);
        s.searchButtonBG         = H(0xffca9584);
        s.transferHistoryBG      = H(0xffffeed4);
        s.transferHistoryText    = H(0xff7180a8);
        s.spottingBG             = H(0xffffeed4);
        s.spottingText           = H(0xff01405f);
        s.tabBG                  = H(0xffffeed4);
        s.volumeColor            = H(0xff246e68);
        s.volumeClipColor        = H(0xffd85a5a);
        s.filterBrowserTagText   = H(0xff1a4a60); // dark teal — readable on white/cream frameBg
        s.presetDialCardActiveText   = H(0xff01405f); // deep teal — readable on warm cream toolbar
        s.presetDialCardInactiveText = H(0xff7180a8); // muted periwinkle
        s.presetDialAccent           = H(0xff246e68); // teal accent underline

// Wwise Transfer (F-034 /colorize) — JadeMist 语义色（暖奶油底，jade 主调）
        s.wwiseConnectOk         = H(0xff286e5d); // 玉绿（与 transferButtonBG 同源）— connected / Imported
        s.wwiseConnectFail       = H(0xffa04848); // 暖红 — disconnected / Failed
        s.wwiseDiffNeutral       = H(0xff7a6f62); // 暖灰 — NoChange / Skipped
        s.wwiseDiffNew           = H(0xff3d8c6e); // 鲜玉 — New 节点
        s.wwiseDiffReplaced      = H(0xffc28a30); // 雾金 — Replaced 节点
        s.wwiseDiffUnresolved    = H(0xffb85a4a); // 陶土红 — Unresolved 节点
        s.wwiseLeafBadge         = H(0xff2e7c68); // 玉叶 — leaf 层标识
        s.wwiseParentBadge       = H(0xff7a6080); // 雾紫 — parent 层标识

// Sidebar Activity Bar — JadeMist (霁翠) — light theme, dark icons
        s.sidebarRailBG          = H(0xfff2ede7);  // warm ivory, darker than pure white
        s.sidebarRailDivider     = H(0x26246e68);
        s.sidebarIconInactive    = H(0xff98a59f);  // sage gray
        s.sidebarIconHover       = H(0xff4d655f);  // deeper sage
        s.sidebarIconActive      = H(0xff246e68);  // jade teal
        s.sidebarActiveStrip     = H(0xff246e68);
        s.sidebarHeaderBG        = H(0xfff2ede7);
        s.sidebarStatusBarBG     = H(0xffeceae5);  // off-white warm
        s.sidebarSubpaneBG       = H(0xfff4efe7);
        s.sidebarSubpaneDivider  = H(0xffc9c8bd);
        s.sidebarSubHeaderLine   = H(0x26000000);
        // F-004-ext Hearth: 翠黄辉光（翠→黄绿 + 亮黄绿边，alpha 0.28 保波形清晰）
        s.waveformSelectionDim       = H(0x730a1c14);  // rgba(10,28,20,0.45)
        s.waveformSelectionHearthTop = H(0x4778dc8c);  // 翠 rgba(120,220,140, 0.28)
        s.waveformSelectionHearthBot = H(0x47f0dc64);  // 黄绿 rgba(240,220,100, 0.28)
        s.waveformSelectionEdge      = H(0xfffff08c);  // 亮黄绿 rgba(255,240,140, 1.0)
        break;

    default: break;
    }
    return s;
}

