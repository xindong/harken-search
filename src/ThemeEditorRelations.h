#pragma once

#include <cstddef>
#include <cstring>

enum class ThemeEditorLayer {
    Base,
    Regions,
    Advanced,
};

enum class ThemeEditorRelationKind {
    Base,
    Shared,
    Derived,
    Legacy,
    SuspectedCoupling,
    Independent,
};

struct ThemeEditorRelation {
    const char* key;
    ThemeEditorLayer layer;
    ThemeEditorRelationKind kind;
    const char* sourceKey;
};

inline bool ThemeEditorRelationCanFollow(const ThemeEditorRelation* relation) {
    return relation
        && relation->kind == ThemeEditorRelationKind::Shared
        && relation->sourceKey
        && relation->sourceKey[0] != '\0';
}

inline const ThemeEditorRelation* GetThemeEditorRelations(size_t& count) {
    static const ThemeEditorRelation kRelations[] = {
        {"mainWindowBG", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"menuBarBG", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"menuText", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, nullptr},
        {"frameBgBase", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "toolbarBG"},
        {"chromeBorder", ThemeEditorLayer::Base, ThemeEditorRelationKind::Independent, nullptr},
        {"popupBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "toolbarBG"},
        {"menuPopupBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Shared, "popupBG"},
        {"dialogBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "popupBG"},
        {"dialogPanelBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "dialogBG"},
        {"toolbarBG", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"historyCapsuleBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "toolbarBG"},
        {"historyCapsuleActiveBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "frameBgBase"},
        {"modeCapsuleBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "toolbarBG"},
        {"modeCapsuleActiveBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "frameBgBase"},
        {"toolbarAccent", ThemeEditorLayer::Base, ThemeEditorRelationKind::Independent, nullptr},
        {"controlAccent", ThemeEditorLayer::Base, ThemeEditorRelationKind::Independent, nullptr},
        {"transportBarBG", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"timecodeText", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"timecodeTextHover", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"interfaceText", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, nullptr},
        {"resultsBG", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"resultsHeaderBG", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"resultsText", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, "menuText"},
        {"resultsOutline", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"resultsSelection", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, nullptr},
        {"resultsActiveBorder", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "resultsSelection"},
        {"resultsScrollbar", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"resultsTabBG", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "tabBG"},
        {"resultsTabHover", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"resultsTabActive", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"resultsSecondaryText", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, nullptr},
        {"resultsPlayedText", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"resultsGradientStart", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "resultsBG"},
        {"resultsGradientEnd", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "resultsBG"},
        {"playlistGradientStart", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, nullptr},
        {"playlistGradientEnd", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, nullptr},
        {"playlistGradientHighlight", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, nullptr},
        {"playlistGradientShadow", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "resultsBG"},
        {"metadataPanelBG", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"metadataPanelText", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, "menuText"},
        {"metadataPanelLabel", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"metadataPanelScrollbar", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "resultsScrollbar"},
        {"coverWallBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Shared, "metadataPanelBG"},
        {"waveformBG", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"waveformPanelBorder", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "waveformBG"},
        {"waveformSplitterBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "waveformBG"},
        {"waveformSplitterLine", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "waveformSplitterBG"},
        {"waveformFooterBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "waveformBG"},
        {"waveformColor", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, nullptr},
        {"waveformSecondaryColor", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"waveformPositionLine", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, nullptr},
        {"miniWaveColor", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Shared, "waveformColor"},
        {"miniWaveBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, nullptr},
        {"waveformSelectionDim", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"waveformSelectionHearthTop", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"waveformSelectionHearthBot", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"waveformSelectionEdge", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"filterBrowserBG", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"filterBrowserText", ThemeEditorLayer::Base, ThemeEditorRelationKind::Base, nullptr},
        {"filterBrowserHeading", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"filterBrowserOutline", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, "resultsOutline"},
        {"filterBrowserSelection", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "resultsSelection"},
        {"filterBrowserScrollbar", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "resultsScrollbar"},
        {"filterBrowserSectionBG", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"filterBrowserButtonBG", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"filterBrowserTreeText", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "filterBrowserText"},
        {"filterBrowserTreeSelection", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "filterBrowserSelection"},
        {"filterBrowserSearchHitText", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "filterBrowserTreeText"},
        {"filterBrowserLocateTint", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Independent, nullptr},
        {"filterBrowserTagText", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "filterBrowserText"},
        {"transferButtonBG", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, nullptr},
        {"searchButtonBG", ThemeEditorLayer::Base, ThemeEditorRelationKind::Independent, nullptr},
        {"transferHistoryBG", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"transferHistoryText", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"spottingBG", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "transferHistoryBG"},
        {"spottingText", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "menuText"},
        {"diagnosticOverlayBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "dialogPanelBG"},
        {"diagnosticOverlayText", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Shared, "interfaceText"},
        {"diagnosticOverlayAccent", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Shared, "toolbarAccent"},
        {"diagnosticOverlayMutedText", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "interfaceText"},
        {"tabBG", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Legacy, nullptr},
        {"volumeColor", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, nullptr},
        {"volumeClipColor", ThemeEditorLayer::Base, ThemeEditorRelationKind::Shared, nullptr},
        {"meterBg", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "waveformBG"},
        {"meterBase", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "volumeColor"},
        {"meterMid", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "meterBase"},
        {"meterWarning", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "meterBase"},
        {"meterClip", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "volumeClipColor"},
        {"meterScaleText", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "resultsSecondaryText"},
        {"presetDialBg", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "toolbarBG"},
        {"presetDialCardActiveBg", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "modeCapsuleActiveBG"},
        {"presetDialCardActiveText", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Shared, "interfaceText"},
        {"presetDialCardInactiveText", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Shared, "resultsSecondaryText"},
        {"presetDialAccent", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "interfaceText"},
        {"sidebarRailDivider", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"sidebarIconInactive", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "resultsSecondaryText"},
        {"sidebarIconHover", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "interfaceText"},
        {"sidebarIconActive", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "controlAccent"},
        {"sidebarActiveStrip", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "sidebarIconActive"},
        {"sidebarSubpaneBG", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "filterBrowserSectionBG"},
        {"sidebarSubpaneDivider", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "filterBrowserOutline"},
        {"sidebarSubHeaderLine", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Base, nullptr},
        {"wwiseConnectOk", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "volumeColor"},
        {"wwiseConnectFail", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "volumeClipColor"},
        {"wwiseDiffNeutral", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "interfaceText"},
        {"wwiseDiffNew", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "menuText"},
        {"wwiseDiffReplaced", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Independent, nullptr},
        {"wwiseDiffUnresolved", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "volumeClipColor"},
        {"wwiseLeafBadge", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "volumeColor"},
        {"wwiseParentBadge", ThemeEditorLayer::Advanced, ThemeEditorRelationKind::Derived, "menuText"},
        {"wwiseLabelText", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "menuText"},
        {"wwiseBodyText", ThemeEditorLayer::Regions, ThemeEditorRelationKind::Shared, "interfaceText"},
    };
    count = sizeof(kRelations) / sizeof(kRelations[0]);
    return kRelations;
}

inline const ThemeEditorRelation* FindThemeEditorRelation(const char* key) {
    if (!key || !key[0]) return nullptr;
    size_t count = 0;
    const ThemeEditorRelation* relations = GetThemeEditorRelations(count);
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(relations[i].key, key) == 0)
            return &relations[i];
    }
    return nullptr;
}
