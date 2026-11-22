#pragma once
// ThemeEditorSchema.h - Explicit group enum + field-to-group mapping table
// All ThemeColorScheme fields mapped to 7 editor groups via member pointers.

#include "Theme.h"
#include "ThemeEditorRelations.h"
#include "Localization.h"   // L(id) macro used by inline group-name helper (knife-B fix)
#include "imgui.h"
#include <cstddef>

// Editor Groups (user-facing sections in the theme editor)

// @stage7-contract
// API contract: theme editor schema is immutable process-lifetime metadata.
// GetThemeEditorItems returns a non-owning pointer to static entries and writes
// their count; entries carry the localization ledger id (labelId) instead of
// inline display strings; member pointers refer to caller-owned ThemeColorScheme
// values and no UI/theme object is retained or mutated by the schema helpers.
enum class ThemeEditorGroup {
    Foundation,        // 基础
    TopBar,            // 顶部
    Results,           // 列表
    Metadata,          // 右侧信息区
    Waveform,          // 波形区
    FilterBrowser,     // 左侧筛选区
    ButtonsAndStatus,  // 按钮/状态
    PresetDial,        // 预设拨盘
    Wwise,
};

// Field Entry: one color field in the editor

struct ThemeEditorItem {
    const char*        key;        // field identifier (e.g. "mainWindowBG")
    LStr               labelId;    // 10-D: display copy lives in the JSON ledger (zh/en/ja)
    ThemeEditorGroup   group;      // which group this belongs to
    ImVec4 ThemeColorScheme::* member;
    const ThemeColorOverride* colorOverride = nullptr;

    ImVec4 Color(const ThemeColorScheme& scheme) const {
        return colorOverride ? colorOverride->Resolve(scheme) : scheme.*member;
    }

    void SetColor(ThemeColorScheme& scheme, const ImVec4& color) const {
        if (colorOverride) scheme.*(colorOverride->member) = color;
        else scheme.*member = color;
    }
};

// Group display names

inline const char* ThemeEditorGroupNameZh(ThemeEditorGroup g) {
    switch (g) {
    case ThemeEditorGroup::Foundation:       return L(TEGroupName_Base);
    case ThemeEditorGroup::TopBar:           return L(TEGroupName_Top);
    case ThemeEditorGroup::Results:          return L(TEGroupName_List);
    case ThemeEditorGroup::Metadata:         return L(TEGroupName_RightInfo);
    case ThemeEditorGroup::Waveform:         return L(TEGroupName_Waveform);
    case ThemeEditorGroup::FilterBrowser:    return L(TEGroupName_LeftFilter);
    case ThemeEditorGroup::ButtonsAndStatus: return L(TEGroupName_Buttons);
    case ThemeEditorGroup::PresetDial:        return L(TEGroupName_PresetDial);
    case ThemeEditorGroup::Wwise:             return L(TEGroupName_Wwise);
    default:                                 return L(TEGroupName_Ungrouped);
    }
}

// The complete field mapping table (sorted by group)

inline const ThemeEditorItem* GetThemeEditorItems(size_t& count) {
    static const ThemeEditorItem kItems[] = {
// 基础
        {"mainWindowBG", LStr::ThemeCol_mainWindowBG, ThemeEditorGroup::Foundation,      &ThemeColorScheme::mainWindowBG},
        {"interfaceText", LStr::ThemeCol_interfaceText, ThemeEditorGroup::Foundation,      &ThemeColorScheme::interfaceText},
        {"frameBgBase", LStr::ThemeCol_frameBgBase, ThemeEditorGroup::Foundation,      &ThemeColorScheme::frameBgBase},
        {"chromeBorder", LStr::ThemeCol_chromeBorder, ThemeEditorGroup::Foundation,      &ThemeColorScheme::chromeBorder},
        {"tabBG", LStr::ThemeCol_tabBG, ThemeEditorGroup::Foundation,      &ThemeColorScheme::tabBG},

// 顶部
        {"menuBarBG", LStr::ThemeCol_menuBarBG, ThemeEditorGroup::TopBar,          &ThemeColorScheme::menuBarBG},
        {"menuText", LStr::ThemeCol_menuText, ThemeEditorGroup::TopBar,          &ThemeColorScheme::menuText},
        {"popupBG", LStr::ThemeCol_popupBG, ThemeEditorGroup::TopBar,         &ThemeColorScheme::popupBG},
        {"menuPopupBG", LStr::ThemeCol_menuPopupBG, ThemeEditorGroup::TopBar,          &ThemeColorScheme::menuPopupBG},
        {"dialogBG", LStr::ThemeCol_dialogBG, ThemeEditorGroup::TopBar,        &ThemeColorScheme::dialogBG},
        {"dialogPanelBG", LStr::ThemeCol_dialogPanelBG, ThemeEditorGroup::TopBar,        &ThemeColorScheme::dialogPanelBG},
        {"toolbarBG", LStr::ThemeCol_toolbarBG, ThemeEditorGroup::TopBar,          &ThemeColorScheme::toolbarBG},
        {"historyCapsuleBG", LStr::ThemeCol_historyCapsuleBG, ThemeEditorGroup::TopBar,      &ThemeColorScheme::historyCapsuleBG},
        {"historyCapsuleActiveBG", LStr::ThemeCol_historyCapsuleActiveBG, ThemeEditorGroup::TopBar, &ThemeColorScheme::historyCapsuleActiveBG},
        {"modeCapsuleBG", LStr::ThemeCol_modeCapsuleBG, ThemeEditorGroup::TopBar,          &ThemeColorScheme::modeCapsuleBG},
        {"modeCapsuleActiveBG", LStr::ThemeCol_modeCapsuleActiveBG, ThemeEditorGroup::TopBar, &ThemeColorScheme::modeCapsuleActiveBG},
        {"toolbarAccent", LStr::ThemeCol_toolbarAccent, ThemeEditorGroup::TopBar,          &ThemeColorScheme::toolbarAccent},
        {"controlAccent", LStr::ThemeCol_controlAccent, ThemeEditorGroup::TopBar,          &ThemeColorScheme::controlAccent},
        {"transportBarBG", LStr::ThemeCol_transportBarBG, ThemeEditorGroup::TopBar,          &ThemeColorScheme::transportBarBG},
        {"timecodeText", LStr::ThemeCol_timecodeText, ThemeEditorGroup::TopBar,          &ThemeColorScheme::timecodeText},
        {"timecodeTextHover", LStr::ThemeCol_timecodeTextHover, ThemeEditorGroup::TopBar,      &ThemeColorScheme::timecodeTextHover},

// 列表
        {"resultsBG", LStr::ThemeCol_resultsBG, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsBG},
        {"resultsHeaderBG", LStr::ThemeCol_resultsHeaderBG, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsHeaderBG},
        {"resultsText", LStr::ThemeCol_resultsText, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsText},
        {"resultsOutline", LStr::ThemeCol_resultsOutline, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsOutline},
        {"resultsSelection", LStr::ThemeCol_resultsSelection, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsSelection},
        {"resultsActiveBorder", LStr::ThemeCol_resultsActiveBorder, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsActiveBorder},
        {"resultsScrollbar", LStr::ThemeCol_resultsScrollbar, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsScrollbar},
        {"resultsTabBG", LStr::ThemeCol_resultsTabBG, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsTabBG},
        {"resultsTabHover", LStr::ThemeCol_resultsTabHover, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsTabHover},
        {"resultsTabActive", LStr::ThemeCol_resultsTabActive, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsTabActive},
        {"resultsSecondaryText", LStr::ThemeCol_resultsSecondaryText, ThemeEditorGroup::Results, &ThemeColorScheme::resultsSecondaryText},
        {"resultsPlayedText", LStr::ThemeCol_resultsPlayedText, ThemeEditorGroup::Results,       &ThemeColorScheme::resultsPlayedText},
        {"resultsGradientStart", LStr::ThemeCol_resultsGradientStart, ThemeEditorGroup::Results, &ThemeColorScheme::resultsGradientStart},
        {"resultsGradientEnd", LStr::ThemeCol_resultsGradientEnd, ThemeEditorGroup::Results, &ThemeColorScheme::resultsGradientEnd},
        {"playlistGradientStart", LStr::ThemeCol_playlistGradientStart, ThemeEditorGroup::Results, &ThemeColorScheme::playlistGradientStart},
        {"playlistGradientEnd", LStr::ThemeCol_playlistGradientEnd, ThemeEditorGroup::Results, &ThemeColorScheme::playlistGradientEnd},
        {"playlistGradientHighlight", LStr::ThemeCol_playlistGradientHighlight, ThemeEditorGroup::Results, &ThemeColorScheme::playlistGradientHighlight},
        {"playlistGradientShadow", LStr::ThemeCol_playlistGradientShadow, ThemeEditorGroup::Results, &ThemeColorScheme::playlistGradientShadow},

// 右侧信息区
        {"metadataPanelBG", LStr::ThemeCol_metadataPanelBG, ThemeEditorGroup::Metadata,      &ThemeColorScheme::metadataPanelBG},
        {"metadataPanelText", LStr::ThemeCol_metadataPanelText, ThemeEditorGroup::Metadata,      &ThemeColorScheme::metadataPanelText},
        {"metadataPanelLabel", LStr::ThemeCol_metadataPanelLabel, ThemeEditorGroup::Metadata,      &ThemeColorScheme::metadataPanelLabel},
        {"metadataPanelScrollbar", LStr::ThemeCol_metadataPanelScrollbar, ThemeEditorGroup::Metadata,      &ThemeColorScheme::metadataPanelScrollbar},
        {"coverWallBG", LStr::ThemeCol_coverWallBG, ThemeEditorGroup::Metadata,      &ThemeColorScheme::coverWallBG},

// 波形区
        {"waveformBG", LStr::ThemeCol_waveformBG, ThemeEditorGroup::Waveform,      &ThemeColorScheme::waveformBG},
        {"waveformPanelBorder", LStr::ThemeCol_waveformPanelBorder, ThemeEditorGroup::Waveform,    &ThemeColorScheme::waveformPanelBorder},
        {"waveformSplitterBG", LStr::ThemeCol_waveformSplitterBG, ThemeEditorGroup::Waveform,  &ThemeColorScheme::waveformSplitterBG},
        {"waveformSplitterLine", LStr::ThemeCol_waveformSplitterLine, ThemeEditorGroup::Waveform, &ThemeColorScheme::waveformSplitterLine},
        {"waveformFooterBG", LStr::ThemeCol_waveformFooterBG, ThemeEditorGroup::Waveform,    &ThemeColorScheme::waveformFooterBG},
        {"waveformColor", LStr::ThemeCol_waveformColor, ThemeEditorGroup::Waveform,      &ThemeColorScheme::waveformColor},
        {"waveformSecondaryColor", LStr::ThemeCol_waveformSecondaryColor, ThemeEditorGroup::Waveform, &ThemeColorScheme::waveformSecondaryColor},
        {"waveformPositionLine", LStr::ThemeCol_waveformPositionLine, ThemeEditorGroup::Waveform,      &ThemeColorScheme::waveformPositionLine},
        {"waveformSelectionDim", LStr::ThemeCol_waveformSelectionDim, ThemeEditorGroup::Waveform,      &ThemeColorScheme::waveformSelectionDim},
        {"waveformSelectionHearthTop", LStr::ThemeCol_waveformSelectionHearthTop, ThemeEditorGroup::Waveform,      &ThemeColorScheme::waveformSelectionHearthTop},
        {"waveformSelectionHearthBot", LStr::ThemeCol_waveformSelectionHearthBot, ThemeEditorGroup::Waveform,   &ThemeColorScheme::waveformSelectionHearthBot},
        {"waveformSelectionEdge", LStr::ThemeCol_waveformSelectionEdge, ThemeEditorGroup::Waveform,      &ThemeColorScheme::waveformSelectionEdge},
        {"miniWaveColor", LStr::ThemeCol_miniWaveColor, ThemeEditorGroup::Waveform,    &ThemeColorScheme::miniWaveColor},
        {"miniWaveBG", LStr::ThemeCol_miniWaveBG, ThemeEditorGroup::Waveform,    &ThemeColorScheme::miniWaveBG},

// 左侧筛选区
        {"filterBrowserBG", LStr::ThemeCol_filterBrowserBG, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserBG},
        {"filterBrowserText", LStr::ThemeCol_filterBrowserText, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserText},
        {"filterBrowserHeading", LStr::ThemeCol_filterBrowserHeading, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserHeading},
        {"filterBrowserOutline", LStr::ThemeCol_filterBrowserOutline, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserOutline},
        {"filterBrowserSelection", LStr::ThemeCol_filterBrowserSelection, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserSelection},
        {"filterBrowserScrollbar", LStr::ThemeCol_filterBrowserScrollbar, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserScrollbar},
        {"filterBrowserSectionBG", LStr::ThemeCol_filterBrowserSectionBG, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserSectionBG},
        {"filterBrowserButtonBG", LStr::ThemeCol_filterBrowserButtonBG, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserButtonBG},
        {"filterBrowserTreeText", LStr::ThemeCol_filterBrowserTreeText, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserTreeText},
        {"sidebarSubpaneBG", LStr::ThemeCol_sidebarSubpaneBG, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::sidebarSubpaneBG},
        {"sidebarSubpaneDivider", LStr::ThemeCol_sidebarSubpaneDivider, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::sidebarSubpaneDivider},
        {"filterBrowserTreeSelection", LStr::ThemeCol_filterBrowserTreeSelection, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserTreeSelection},
        {"filterBrowserSearchHitText", LStr::ThemeCol_filterBrowserSearchHitText, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserSearchHitText},
        {"filterBrowserLocateTint", LStr::ThemeCol_filterBrowserLocateTint, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserLocateTint},
        {"filterBrowserTagText", LStr::ThemeCol_filterBrowserTagText, ThemeEditorGroup::FilterBrowser, &ThemeColorScheme::filterBrowserTagText},

// 按钮/状态
        {"transferButtonBG", LStr::ThemeCol_transferButtonBG, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::transferButtonBG},
        {"searchButtonBG", LStr::ThemeCol_searchButtonBG, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::searchButtonBG},
        {"volumeColor", LStr::ThemeCol_volumeColor, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::volumeColor},
        {"volumeClipColor", LStr::ThemeCol_volumeClipColor, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::volumeClipColor},
        {"meterBg", LStr::ThemeCol_meterBg, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::meterBg},
        {"meterBase", LStr::ThemeCol_meterBase, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::meterBase},
        {"meterMid", LStr::ThemeCol_meterMid, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::meterMid},
        {"meterWarning", LStr::ThemeCol_meterWarning, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::meterWarning},
        {"meterClip", LStr::ThemeCol_meterClip, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::meterClip},
        {"meterScaleText", LStr::ThemeCol_meterScaleText, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::meterScaleText},
        {"transferHistoryBG", LStr::ThemeCol_transferHistoryBG, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::transferHistoryBG},
        {"transferHistoryText", LStr::ThemeCol_transferHistoryText, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::transferHistoryText},
        {"spottingBG", LStr::ThemeCol_spottingBG, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::spottingBG},
        {"spottingText", LStr::ThemeCol_spottingText, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::spottingText},
        {"diagnosticOverlayBG", LStr::ThemeCol_diagnosticOverlayBG, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::diagnosticOverlayBG},
        {"diagnosticOverlayText", LStr::ThemeCol_diagnosticOverlayText, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::diagnosticOverlayText},
        {"diagnosticOverlayAccent", LStr::ThemeCol_diagnosticOverlayAccent, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::diagnosticOverlayAccent},
        {"diagnosticOverlayMutedText", LStr::ThemeCol_diagnosticOverlayMutedText, ThemeEditorGroup::ButtonsAndStatus, &ThemeColorScheme::diagnosticOverlayMutedText},

// 预设拨盘
        {"presetDialBg", LStr::ThemeCol_presetDialBg, ThemeEditorGroup::PresetDial, &ThemeColorScheme::presetDialBg},
        {"presetDialCardActiveBg", LStr::ThemeCol_presetDialCardActiveBg, ThemeEditorGroup::PresetDial, &ThemeColorScheme::presetDialCardActiveBg},
        {"presetDialCardActiveText", LStr::ThemeCol_presetDialCardActiveText, ThemeEditorGroup::PresetDial, &ThemeColorScheme::presetDialCardActiveText},
        {"presetDialCardInactiveText", LStr::ThemeCol_presetDialCardInactiveText, ThemeEditorGroup::PresetDial, &ThemeColorScheme::presetDialCardInactiveText},
        {"presetDialAccent", LStr::ThemeCol_presetDialAccent, ThemeEditorGroup::PresetDial, &ThemeColorScheme::presetDialAccent},
        {kWwiseLabelTextColor.key, LStr::ThemeCol_wwiseLabelText, ThemeEditorGroup::Wwise, nullptr, &kWwiseLabelTextColor},
        {kWwiseBodyTextColor.key, LStr::ThemeCol_wwiseBodyText, ThemeEditorGroup::Wwise, nullptr, &kWwiseBodyTextColor},
    };

    count = sizeof(kItems) / sizeof(kItems[0]);
    return kItems;
}

inline const ThemeEditorItem* FindThemeEditorItem(const char* key) {
    if (!key || !key[0]) return nullptr;
    size_t count = 0;
    const ThemeEditorItem* items = GetThemeEditorItems(count);
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(items[i].key, key) == 0)
            return &items[i];
    }
    return nullptr;
}
