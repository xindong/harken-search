// ThemeEditor.cpp - Independent theme editor module
// Extracted from ModuleUI.cpp. Owns editor lifecycle, undo, draft, save/discard.
// Uses ThemeEditorSchema.h for explicit group-based field layout.

#include "ThemeEditor.h"
#include "ThemeSchedule.h"
#include "BuiltinThemeScheme.h"
#include "ThemeEditorSchema.h"
#include "ThemeInspector.h"
#include "ThemeLibraryFiles.h"

#include "AppState.h"
#include "Theme.h"
#include "Localization.h"
#include "Utils.h"
#include "WindowsMain.h"
#include "BuildConfig.h"
#include "ThemeEditorRelationState.h"
#include "platform/filesystem.h"
#include "platform/theme_json_dialogs.h"
#include "imgui.h"
#include "HarkenSearchWidgets.h"
#include "nlohmann_json.hpp"
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
#include "imgui_internal.h"
static nlohmann::json s_editorQaItems=nlohmann::json::array();
static ImVec4 s_editorQaBounds{};
static void ObserveEditorItem(const char* id) {
    const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
    const auto ca=ImGui::GetWindowDrawList()->GetClipRectMin(),cb=ImGui::GetWindowDrawList()->GetClipRectMax();
    s_editorQaItems.push_back({{"id",id},{"bounds",{a.x,a.y,b.x,b.y}},{"clip",{ca.x,ca.y,cb.x,cb.y}}});
}
#else
static void ObserveEditorItem(const char*) {}
#endif

#include <vector>
#include <string>
#include <array>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <filesystem>

// User theme files live next to settings.ini, never in the install directory.
extern std::string g_appSettingsPathUtf8;


// Editor State (file-static singleton)

struct ThemeEditorState {
    int schedulePeriod = -1;
    ThemeColorScheme initialDraft{};
    bool wasOpen          = false;
    bool savedThisSession = false;
    bool livePreview      = true;
    bool openCustomized   = false;
    AppTheme openTheme    = AppTheme::Dark;
    bool openUseCustomColors = false;
    std::string openCustomThemeName;

    ThemeColorScheme openSnapshot{};   // snapshot when editor opened (revert target)
    ThemeColorScheme draft{};          // working copy — editor only touches this
    ThemeEditorRelationState relationState;
    ThemeEditorRelationState openRelationState;

    struct UndoEntry {
        ThemeColorScheme draft{};
        ThemeEditorRelationState relationState;
    };
    std::vector<UndoEntry> undoStack;

    char        themeName[128] = "My Theme";
    std::string exportMsg;
    std::string importMsg;
    std::string lastImportPath;
    std::string lastExportPath;

    // Theme library: one JSON copy per imported theme, kept in user data.
    std::vector<std::string> savedThemes;
    bool        savedThemesLoaded = false;
    std::string activeSavedTheme;

    bool        inspectMode = false;
    std::string inspectScrollTarget;
    std::string inspectHighlightKey;
    float       inspectHighlightTimer = 0.f;
};

static ThemeEditorState g_ed;
static constexpr int kMaxUndoDepth = 30;

static std::filesystem::path ThemeSettingsFilePath() {
    if (!g_appSettingsPathUtf8.empty()) return std::filesystem::u8path(g_appSettingsPathUtf8);
    return std::filesystem::u8path(GetExeDirUtf8() + "\\settings.ini");
}

static std::filesystem::path ThemeLibraryDir() {
    return harken::theme_library::LibraryDirectory(ThemeSettingsFilePath());
}

static std::filesystem::path CustomThemeFilePath() {
    return harken::theme_library::CustomThemePath(ThemeSettingsFilePath());
}

static void RefreshSavedThemes() {
    g_ed.savedThemes.clear();
    for (const auto& path : harken::theme_library::ListThemes(ThemeLibraryDir())) {
        g_ed.savedThemes.push_back(path.u8string());
    }
    g_ed.savedThemesLoaded = true;
}

static const char* ThemePresetDisplayName(int presetIndex) {
    return (g_currentLanguage == AppLanguage::Chinese)
        ? ThemeColors::getPresetDisplayNameZh(presetIndex)
        : ThemeColors::getPresetDisplayNameEn(presetIndex);
}

static int ThemeDiagSettingsSlot(AppTheme theme) {
    switch (theme) {
        case AppTheme::Dark:      return 0;
        case AppTheme::Ocean:     return 1;
        case AppTheme::RoseCedar: return 2;
        case AppTheme::Custom:    return 3;
        case AppTheme::LapisBlue: return 4;
        case AppTheme::JadeMist:  return 5;
        default: return -1;
    }
}

static const char* ThemeDiagPublicThemeLabel(AppTheme theme) {
    const int themeSlotIndex = ThemeDiagSettingsSlot(theme);
    if (themeSlotIndex < 0) return "Other";
    return (g_currentLanguage == AppLanguage::Chinese)
        ? ThemeColors::getSettingsThemeDisplayNameZh(themeSlotIndex)
        : ThemeColors::getSettingsThemeDisplayNameEn(themeSlotIndex);
}

static ImVec4 ThemeEditorWithAlpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

static ImVec4 ThemeEditorMix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t);
}

static const char* ThemeEditorGroupHintZh(ThemeEditorGroup group) {
    switch (group) {
    case ThemeEditorGroup::TopBar:
        return L(TEGroupHint_TopBar);
    case ThemeEditorGroup::Results:
        return L(TEGroupHint_Results);
    case ThemeEditorGroup::Metadata:
        return L(TEGroupHint_Metadata);
    case ThemeEditorGroup::Waveform:
        return L(TEGroupHint_Waveform);
    case ThemeEditorGroup::FilterBrowser:
        return L(TEGroupHint_FilterBrowser);
    default:
        return nullptr;
    }
}

static const char* ThemeEditorLayerName(ThemeEditorLayer layer) {
    switch (layer) {
    case ThemeEditorLayer::Base:     return L(ThemeEditor_LayerBase);
    case ThemeEditorLayer::Regions:  return L(ThemeEditor_LayerRegions);
    case ThemeEditorLayer::Advanced: return L(ThemeEditor_LayerAdvanced);
    default:                         return L(ThemeEditor_LayerAdvanced);
    }
}

static const char* ThemeEditorRelationLabel(ThemeEditorRelationKind kind) {
    switch (kind) {
    case ThemeEditorRelationKind::Shared:            return L(ThemeEditor_RelationShared);
    case ThemeEditorRelationKind::Derived:           return L(ThemeEditor_RelationDerived);
    case ThemeEditorRelationKind::Legacy:            return L(ThemeEditor_RelationLegacy);
    case ThemeEditorRelationKind::SuspectedCoupling: return L(ThemeEditor_RelationCoupling);
    case ThemeEditorRelationKind::Independent:       return L(ThemeEditor_RelationIndependent);
    case ThemeEditorRelationKind::Base:
    default:                                          return nullptr;
    }
}

static const ThemeEditorRelation* ThemeEditorRelationFor(const ThemeEditorItem& item) {
    return FindThemeEditorRelation(item.key);
}

static bool ThemeEditorItemInLayer(const ThemeEditorItem& item, ThemeEditorLayer layer) {
    const ThemeEditorRelation* relation = ThemeEditorRelationFor(item);
    return relation && relation->layer == layer;
}

static bool ThemeEditorLayerContainsKey(ThemeEditorLayer layer, const char* key) {
    if (!key || !key[0]) return false;
    size_t count = 0;
    const ThemeEditorItem* items = GetThemeEditorItems(count);
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(items[i].key, key) == 0 && ThemeEditorItemInLayer(items[i], layer))
            return true;
    }
    return false;
}

static bool ThemeEditorHasVisibleKey(const char* key) {
    if (!key || !key[0]) return false;
    size_t count = 0;
    const ThemeEditorItem* items = GetThemeEditorItems(count);
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(items[i].key, key) == 0)
            return true;
    }
    return false;
}

// Internal helpers

static void ApplyDraftToLive(AppState& state);

static void PushUndo() {
    g_ed.undoStack.push_back({g_ed.draft, g_ed.relationState});
    if ((int)g_ed.undoStack.size() > kMaxUndoDepth)
        g_ed.undoStack.erase(g_ed.undoStack.begin());
}

ThemeEditorSavedAppearance ThemeEditorAppearanceForSettings(const AppState& state) {
    if (g_ed.wasOpen && g_ed.schedulePeriod >= 0)
        return {g_ed.openTheme, g_ed.openUseCustomColors};
    return {state.settings.theme, state.settings.useCustomColors};
}

static void ApplyDraftToLive(AppState& state) {
    // While previewing in the editor, treat the draft as the active custom theme
    // so the main loop does not re-sync stock theme tokens over the draft each frame.
    state.settings.theme = AppTheme::Custom;
    state.settings.useCustomColors = false;
    ThemeColors::activateCustomScheme(g_ed.draft);
    ThemeColors::applyToImGui();
    ThemeColors::markPendingUiChromeResync();
}

static void UndoLast(AppState& state) {
    if (g_ed.undoStack.empty()) return;
    const ThemeEditorState::UndoEntry& entry = g_ed.undoStack.back();
    g_ed.draft = entry.draft;
    g_ed.relationState = entry.relationState;
    g_ed.undoStack.pop_back();
    if (g_ed.livePreview) ApplyDraftToLive(state);
}

static void RestoreOpenAppearance(AppState& state) {
    state.settings.theme = g_ed.openTheme;
    state.settings.useCustomColors = g_ed.openUseCustomColors;
    state.settings.customThemeName = g_ed.openCustomThemeName;
    ThemeColors::current() = g_ed.openSnapshot;
    if (g_ed.openCustomized) {
        ThemeColors::activateCustomScheme(g_ed.openSnapshot);
    } else {
        ThemeColors::deactivateCustomTheme();
    }
    ThemeColors::applyToImGui();
    ThemeColors::markPendingUiChromeResync();
}

static void RestoreSnapshot(AppState& state) {
    if (g_ed.schedulePeriod >= 0) {
        g_ed.draft = g_ed.initialDraft;
        g_ed.relationState = g_ed.openRelationState;
        if (g_ed.livePreview) ApplyDraftToLive(state);
        return;
    }
    g_ed.draft = g_ed.openSnapshot;
    g_ed.relationState = g_ed.openRelationState;
    RestoreOpenAppearance(state);
}

static void LoadPresetToDraft(int idx) {
    g_ed.draft = ThemeColors::getPresetByIndex(idx);
    g_ed.relationState.ResetToValues(g_ed.draft);
    if (g_ed.livePreview && g_appState) ApplyDraftToLive(*g_appState);
}

static std::string BuildThemeJsonSuggestedPathUtf8(const char* fallbackStem) {
    std::string stem = TrimCopy(g_ed.themeName);
    if (stem.empty()) stem = fallbackStem;
    stem = SanitizeWindowsName(stem);
    if (stem.empty()) stem = fallbackStem;
    return GetExeDirUtf8() + "\\" + stem + ".json";
}

static std::string ThemeNameFromPathUtf8(const std::string& pathUtf8) {
    const fs::path path = Utf8ToPath(pathUtf8);
    std::string stem = WideToUtf8(path.stem().wstring());
    stem = TrimCopy(stem);
    if (stem.empty()) stem = "My Theme";
    return stem;
}

static void SetThemeNameFromPathUtf8(const std::string& pathUtf8) {
    const std::string themeName = ThemeNameFromPathUtf8(pathUtf8);
    std::snprintf(g_ed.themeName, sizeof(g_ed.themeName), "%s", themeName.c_str());
}

static bool LoadEditorThemeFile(const std::string& pathUtf8, ThemeColorScheme& imported,
                                const char* source, AppTheme startTheme, bool livePreview) {
    std::string text;
    if (!Platform::FileSystem::ReadFileText(pathUtf8, text)) return false;
    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (!document.is_object()) return false;
    // The serialized scheme is the complete supported field list, including
    // Wwise colors that are not shown in the editor. Only validated top-level
    // values reach the legacy flat reader; nested metadata cannot override them.
    const auto supported = nlohmann::json::parse(ThemeColors::exportToJSON(ThemeColorScheme{}));
    nlohmann::json colors = nlohmann::json::object();
    for (auto field = supported.begin(); field != supported.end(); ++field) {
        const auto value = document.find(field.key());
        if (value == document.end()) continue;
        if (!value->is_string()) return false;
        const auto text = value->get<std::string>();
        // colorMixing / resultsGradient 是开关，不是颜色：原样透传，不按十六进制校验。
        if (field.key() == "colorMixing" || field.key() == "resultsGradient") {
            if (text != "0" && text != "1") return false;
            colors[field.key()] = text;
            continue;
        }
        if ((text.size() != 6 && text.size() != 8) || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return false;
        colors[field.key()] = text;
    }
    if (colors.empty()) return false;
    ThemeColorScheme loaded{};
    if (!ThemeColors::importFromJSON(colors.dump(), loaded)) return false;
    imported = loaded;
    if (source) ThemeColors::armImportDiagnostics(source, pathUtf8, (int)startTheme,
                                                  ThemeDiagPublicThemeLabel(startTheme), livePreview);
    return true;
}

static bool SaveCustomThemeFiles(const std::string& name, const ThemeColorScheme& scheme,
                                 std::string& statusMsg) {
    const auto json = ThemeColors::exportToJSON(scheme);
    std::string error;
    auto saveCopy = [&] {
        if (!harken::theme_library::SaveThemeCopy(ThemeLibraryDir(), name, json, error)) {
            statusMsg = L(TE_LibrarySaveFail);
            return false;
        }
        RefreshSavedThemes();
        return true;
    };
    if (!harken::theme_library::SaveThemeJson(CustomThemeFilePath(), json, error)) {
        statusMsg = L(TE_AppliedWriteFail);
        return false;
    }
    statusMsg.clear();
    saveCopy();
    return true;
}

static void ActivateInstalledTheme(AppState& state, const std::string& name,
                                   const ThemeColorScheme& scheme) {
    state.settings.theme = AppTheme::Custom;
    state.settings.customThemeName = name;
    state.settings.useCustomColors = false;
    ThemeColors::activateCustomScheme(scheme);
    ThemeColors::applyToImGui();
    ThemeColors::markPendingUiChromeResync();
}

static bool SaveDraftAsCustomTheme(AppState& state, std::string& statusMsg) {
    if (g_ed.schedulePeriod >= 0) {
        const auto period = g_ed.schedulePeriod == 0 ? harken::theme_schedule::Period::Day : harken::theme_schedule::Period::Night;
        std::string error;
        if (!harken::theme_schedule::SavePalette(period, g_ed.draft, error)) {
            statusMsg = std::string(L(ThemeSchedule_SaveFailed)) + " (" + error + ")";
            return false;
        }
        g_ed.savedThisSession = true;
        state.showThemeEditor = false;
        return true;
    }
    std::string themeName = TrimCopy(g_ed.themeName);
    if (themeName.empty()) themeName = "My Theme";

    if (!SaveCustomThemeFiles(themeName, g_ed.draft, statusMsg)) return false;

    std::snprintf(g_ed.themeName, sizeof(g_ed.themeName), "%s", themeName.c_str());
    ActivateInstalledTheme(state, g_ed.themeName, g_ed.draft);
    g_ed.savedThisSession = true;

    g_ed.openTheme = AppTheme::Custom;
    g_ed.openCustomized = true;
    g_ed.openUseCustomColors = false;
    g_ed.openCustomThemeName = g_ed.themeName;
    g_ed.openSnapshot = g_ed.draft;
    g_ed.openRelationState = g_ed.relationState;

#ifdef _WIN32
    SaveAppSettings(state);
#endif
    char savedMsg[512];
    std::snprintf(savedMsg, sizeof(savedMsg), L(TE_SaveAsFmt), g_ed.themeName);
    if (statusMsg.empty()) statusMsg = savedMsg;
    return true;
}

bool ImportThemeFromSettings(AppState& state, std::string& result) {
#ifdef _WIN32
    const auto filePath = Platform::ThemeDialogs::OpenThemeJsonFileDialogUtf8(
        g_hwnd, GetExeDirUtf8() + "\\theme_export.json");
    if (filePath.empty()) {
        result.clear();
        return false;
    }

    const auto startTheme = state.settings.theme;
    ThemeColorScheme imported{};
    if (!LoadEditorThemeFile(filePath, imported, nullptr, startTheme, false)) {
        result = L(TE_ImportFail);
        return false;
    }

    namespace library = harken::theme_library;
    const auto themeName = ThemeNameFromPathUtf8(filePath);
    if (g_appSettingsPathUtf8.empty() || state.showThemeEditor || g_ed.wasOpen) {
        result = L(SetUX_ThemeImportWriteFailed);
        return false;
    }
    const auto installed = library::InstallImportedTheme(ThemeSettingsFilePath(), themeName,
        ThemeColors::exportToJSON(imported), [&] {
            return harken::theme_schedule::TrySaveImportedAppearance(state, themeName,
                [&] { return TrySaveAppSettings(state); });
        });
    if (installed != library::ImportResult::Saved) {
        result = installed == library::ImportResult::RollbackFailed
            ? L(SetUX_ThemeImportRollbackFailed) : L(SetUX_ThemeImportWriteFailed);
        return false;
    }
    ThemeColors::armImportDiagnostics("SettingsThemeImport", filePath, (int)startTheme,
                                     ThemeDiagPublicThemeLabel(startTheme), false);
    ActivateInstalledTheme(state, themeName, imported);
    RefreshSavedThemes();
    result = L(SetUX_ThemeImported);
    return true;
#else
    result = L(TE_ImportUnsupported);
    return false;
#endif
}

enum class ThemeEditorRelationAction {
    None,
    Detach,
    Relink,
};

struct ThemeEditorRowResult {
    bool colorChanged = false;
    ThemeEditorRelationAction relationAction = ThemeEditorRelationAction::None;
};

// Color item + group rendering

static ThemeEditorRowResult DrawColorItem(const ThemeEditorItem& item) {
    ThemeEditorRowResult result;
    ImVec4 color = item.Color(g_ed.draft);
    const ThemeEditorRelation* relation = ThemeEditorRelationFor(item);
    const bool canFollow = ThemeEditorRelationCanFollow(relation);
    const bool linked = canFollow && g_ed.relationState.IsLinked(relation);

    ImGui::PushID(item.key);
    const auto& scheme = ThemeColors::current();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const float rowHeight = ImGui::GetFrameHeight() + 10.0f;
    const ImVec2 rowMax(rowOrigin.x + rowWidth, rowOrigin.y + rowHeight);
    const bool rowHovered = ImGui::IsMouseHoveringRect(rowOrigin, rowMax);
    const ImVec4 rowBg = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelBG, color, 0.16f), 0.92f);
    const ImVec4 rowBgHover = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelBG, color, 0.24f), 0.96f);
    const ImVec4 rowBorder = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelLabel, color, 0.26f), rowHovered ? 0.64f : 0.48f);
    const ImVec4 rowText = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelText, color, 0.10f), 1.0f);
    const ImVec4 rowKey = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelLabel, color, 0.18f), 0.92f);
    dl->AddRectFilled(rowOrigin, rowMax, ImGui::GetColorU32(rowHovered ? rowBgHover : rowBg), 6.0f);
    dl->AddRectFilledMultiColor(
        ImVec2(rowOrigin.x + 1.0f, rowOrigin.y + 1.0f),
        ImVec2(rowMax.x - 1.0f, rowOrigin.y + rowHeight * 0.48f),
        ImGui::GetColorU32(ThemeEditorWithAlpha(ThemeEditorMix(color, scheme.metadataPanelText, 0.18f), rowHovered ? 0.10f : 0.06f)),
        ImGui::GetColorU32(ThemeEditorWithAlpha(ThemeEditorMix(color, scheme.metadataPanelText, 0.10f), rowHovered ? 0.08f : 0.04f)),
        ImGui::GetColorU32(ThemeEditorWithAlpha(color, 0.0f)),
        ImGui::GetColorU32(ThemeEditorWithAlpha(color, 0.0f)));
    dl->AddRect(rowOrigin, rowMax, ImGui::GetColorU32(rowBorder), 6.0f, 0, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + 8.0f, rowOrigin.y + 5.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ThemeEditorWithAlpha(ThemeEditorMix(color, scheme.metadataPanelBG, 0.46f), 0.98f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ThemeEditorWithAlpha(ThemeEditorMix(color, scheme.metadataPanelBG, 0.32f), 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ThemeEditorWithAlpha(ThemeEditorMix(color, scheme.metadataPanelLabel, 0.24f), 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ThemeEditorWithAlpha(ThemeEditorMix(color, scheme.metadataPanelLabel, 0.18f), 0.72f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    if (linked) ImGui::BeginDisabled();
    result.colorChanged = ImGui::ColorEdit4(
        "##color", &color.x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview);
    if (result.colorChanged) item.SetColor(g_ed.draft, color);
    ObserveEditorItem(item.key);
    if (linked) ImGui::EndDisabled();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    ImGui::SameLine(0.0f, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, rowText);
    ImGui::TextUnformatted(GetLocalizedString(item.labelId));
    ImGui::PopStyleColor();
    if (relation) {
        const char* relationLabel = canFollow
            ? (linked ? L(ThemeEditor_RelationLinked) : L(ThemeEditor_RelationIndependent))
            : ThemeEditorRelationLabel(relation->kind);
        if (relationLabel) {
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, rowKey);
            ImGui::TextDisabled("%s", relationLabel);
            ObserveEditorItem((std::string("relation-state-") + item.key).c_str());
            if (relation->sourceKey && ImGui::IsItemHovered())
                HarkenSearch::SetTooltip(relation->sourceKey);
            ImGui::PopStyleColor();
        }

    }
    if (g_ed.schedulePeriod < 0) {
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, rowKey);
        ImGui::TextDisabled(L(ThemeEditor_ItemKeyFmt), item.key);
        ImGui::PopStyleColor();
    }
    if (relation && canFollow) {
        const char* actionLabel = linked
            ? L(ThemeEditor_RelationDetach)
            : L(ThemeEditor_RelationRelink);
        const float actionWidth = ImGui::CalcTextSize(actionLabel).x + 20.0f;
        ImGui::SetCursorScreenPos(ImVec2(rowMax.x - actionWidth - 8.0f, rowOrigin.y + 5.0f));
        if (ImGui::SmallButton(actionLabel)) {
            result.relationAction = linked
                ? ThemeEditorRelationAction::Detach
                : ThemeEditorRelationAction::Relink;
        }
        ObserveEditorItem((std::string("relation-action-") + item.key).c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x, rowOrigin.y + rowHeight + 4.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::PopID();

    return result;
}

static bool GroupContainsKey(ThemeEditorGroup group, ThemeEditorLayer layer, const char* key) {
    if (!key || !key[0]) return false;
    size_t count = 0;
    const ThemeEditorItem* items = GetThemeEditorItems(count);
    for (size_t i = 0; i < count; ++i) {
        if (items[i].group == group
            && ThemeEditorItemInLayer(items[i], layer)
            && std::strcmp(items[i].key, key) == 0)
            return true;
    }
    return false;
}

static void DrawGroup(ThemeEditorGroup group, ThemeEditorLayer layer) {
    size_t count = 0;
    const ThemeEditorItem* items = GetThemeEditorItems(count);

    bool hasItems = false;
    for (size_t i = 0; i < count; ++i) {
        if (items[i].group == group && ThemeEditorItemInLayer(items[i], layer)) {
            hasItems = true;
            break;
        }
    }
    if (!hasItems)
        return;

    const bool shouldAutoOpen = !g_ed.inspectScrollTarget.empty()
                  && GroupContainsKey(group, layer, g_ed.inspectScrollTarget.c_str());

    const auto& scheme = ThemeColors::current();
    const ImVec4 groupBg = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelBG, scheme.metadataPanelLabel, 0.14f), 0.96f);
    const ImVec4 groupHover = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelBG, scheme.metadataPanelLabel, 0.24f), 0.99f);
    const ImVec4 groupActive = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelBG, scheme.metadataPanelLabel, 0.34f), 1.0f);
    const ImVec4 groupText = ThemeEditorWithAlpha(scheme.metadataPanelText, 1.0f);
    const ImVec4 groupBorder = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelLabel, scheme.filterBrowserOutline, 0.20f), 0.58f);
    ImGui::PushStyleColor(ImGuiCol_Header, groupBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, groupHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, groupActive);
    ImGui::PushStyleColor(ImGuiCol_Text, groupText);
    ImGui::PushStyleColor(ImGuiCol_Border, groupBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);

    if (shouldAutoOpen)
        ImGui::SetNextItemOpen(true);
    const bool open = ImGui::CollapsingHeader(ThemeEditorGroupNameZh(group), ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    if (!open)
        return;

    ImGui::Indent(8.0f);
    if (const char* hint = ThemeEditorGroupHintZh(group)) {
        ImGui::TextDisabled("%s", hint);
        ImGui::Spacing();
    }
    for (size_t i = 0; i < count; ++i) {
        if (items[i].group != group || !ThemeEditorItemInLayer(items[i], layer)) continue;

        const bool isHighlighted = !g_ed.inspectHighlightKey.empty()
                                   && std::strcmp(items[i].key, g_ed.inspectHighlightKey.c_str()) == 0
                                   && g_ed.inspectHighlightTimer > 0.f;

        if (!g_ed.inspectScrollTarget.empty()
            && std::strcmp(items[i].key, g_ed.inspectScrollTarget.c_str()) == 0) {
            ImGui::SetScrollHereY(0.3f);
            g_ed.inspectHighlightKey = g_ed.inspectScrollTarget;
            g_ed.inspectHighlightTimer = 2.0f;
            g_ed.inspectScrollTarget.clear();
        }

        if (isHighlighted) {
            float alpha = std::min(g_ed.inspectHighlightTimer, 1.0f);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float rowW = ImGui::GetContentRegionAvail().x;
            float rowH = ImGui::GetFrameHeight() + 14.0f;
            ImVec2 p1(p0.x + rowW, p0.y + rowH);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, p1,
                IM_COL32(36, 158, 152, (int)(50.f * alpha)), 6.0f);
            dl->AddRect(p0, p1,
                IM_COL32(36, 158, 152, (int)(180.f * alpha)), 6.0f, ImDrawFlags_None, 2.0f);
        }

        const ImVec4 beforeValue = items[i].Color(g_ed.draft);
        const ThemeColorScheme beforeDraft = g_ed.draft;
        const ThemeEditorRelationState beforeRelations = g_ed.relationState;
        const ThemeEditorRowResult result = DrawColorItem(items[i]);
        const ImVec4 afterValue = items[i].Color(g_ed.draft);
        const bool colorChanged = result.colorChanged
            && std::memcmp(&beforeValue, &afterValue, sizeof(ImVec4)) != 0;

        if (colorChanged || result.relationAction != ThemeEditorRelationAction::None) {
            const ThemeColorScheme afterDraft = g_ed.draft;
            g_ed.draft = beforeDraft;
            g_ed.relationState = beforeRelations;
            PushUndo();
            g_ed.draft = afterDraft;

            if (result.relationAction == ThemeEditorRelationAction::Detach)
                g_ed.relationState.Detach(g_ed.draft, items[i].key);
            else if (result.relationAction == ThemeEditorRelationAction::Relink)
                g_ed.relationState.Relink(g_ed.draft, items[i].key);

            if (colorChanged || result.relationAction == ThemeEditorRelationAction::Relink)
                g_ed.relationState.Propagate(g_ed.draft);

            if (g_ed.livePreview && g_appState) ApplyDraftToLive(*g_appState);
        }
    }
    ImGui::Unindent(8.0f);
}

static void DrawEditorLayer(ThemeEditorLayer layer) {
    const bool shouldAutoOpen = !g_ed.inspectScrollTarget.empty()
                                && ThemeEditorLayerContainsKey(layer, g_ed.inspectScrollTarget.c_str());
    if (shouldAutoOpen)
        ImGui::SetNextItemOpen(true);
    if (!ImGui::CollapsingHeader(ThemeEditorLayerName(layer), ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::Indent(8.0f);
    ImGui::PushID(static_cast<int>(layer));
    DrawGroup(ThemeEditorGroup::Foundation, layer);
    DrawGroup(ThemeEditorGroup::TopBar, layer);
    DrawGroup(ThemeEditorGroup::Results, layer);
    DrawGroup(ThemeEditorGroup::Metadata, layer);
    DrawGroup(ThemeEditorGroup::Waveform, layer);
    DrawGroup(ThemeEditorGroup::FilterBrowser, layer);
    DrawGroup(ThemeEditorGroup::ButtonsAndStatus, layer);
    DrawGroup(ThemeEditorGroup::PresetDial, layer);
    DrawGroup(ThemeEditorGroup::Wwise, layer);
    ImGui::PopID();
    ImGui::Unindent(8.0f);
}

// Public API

void ResetThemeEditorSession() {
    g_ed = ThemeEditorState{};
    std::snprintf(g_ed.themeName, sizeof(g_ed.themeName), "My Theme");
}

#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
bool ThemeScheduleEditorQaSetColor(const std::string& key, const ImVec4& value) {
    if (!g_ed.wasOpen || g_ed.schedulePeriod < 0 || !g_appState || !g_appState->showThemeEditor) return false;
    size_t count = 0;
    const auto* fields = GetThemeEditorItems(count);
    for (size_t i = 0; i < count; ++i) {
        if (key == fields[i].key) {
            PushUndo();
            g_ed.relationState.Detach(g_ed.draft, key.c_str());
            fields[i].SetColor(g_ed.draft, value);
            g_ed.relationState.Propagate(g_ed.draft);
            if (g_ed.livePreview) ApplyDraftToLive(*g_appState);
            return true;
        }
    }
    return false;
}
std::string ThemeScheduleEditorQaStatus() { return g_ed.exportMsg; }
std::string ThemeScheduleEditorQaObservation() {
    auto popups=nlohmann::json::array();
    auto relationStates=nlohmann::json::object();
    size_t relationCount=0;
    const ThemeEditorRelation* relations=GetThemeEditorRelations(relationCount);
    for(size_t i=0;i<relationCount;++i){
        if(ThemeEditorRelationCanFollow(&relations[i]))
            relationStates[relations[i].key]=g_ed.relationState.IsLinked(&relations[i]);
    }
    for(const auto* w:ImGui::GetCurrentContext()->Windows)
        if(w->WasActive && (w->Flags&ImGuiWindowFlags_Popup))
            popups.push_back({{"name",w->Name},{"bounds",{w->Pos.x,w->Pos.y,w->Pos.x+w->Size.x,w->Pos.y+w->Size.y}}});
    const auto r=s_editorQaBounds;
    return nlohmann::json({{"frame",ImGui::GetFrameCount()},{"items",s_editorQaItems},{"popups",popups},
        {"bounds",{r.x,r.y,r.z,r.w}},{"live_preview",g_ed.livePreview},{"relation_states",relationStates},
        {"draft_bg",ThemeColors::toARGBHex(g_ed.draft.mainWindowBG)},
        {"draft_wwise_ok",ThemeColors::toARGBHex(g_ed.draft.wwiseConnectOk)}}).dump();
}
#endif

void RenderThemeEditor(AppState& state) {
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    s_editorQaItems=nlohmann::json::array();
#endif
// Closed → revert if not saved
    if (!state.showThemeEditor) {
        if (g_ed.wasOpen) {
            g_ed.inspectMode = false;
            ThemeInspector::SetActive(false);
            if (g_ed.schedulePeriod >= 0) {
                if (!harken::theme_schedule::ControlsTheme(state)) RestoreOpenAppearance(state);
                harken::theme_schedule::Invalidate();
                state.themeScheduleEditorPeriod = -1;
                state.showSettingsDialog = true;
            } else {
                state.settings.theme = g_ed.openTheme;
                state.settings.useCustomColors = g_ed.openUseCustomColors;
                state.settings.customThemeName = g_ed.openCustomThemeName;
                ThemeColors::current() = g_ed.openSnapshot;
                if (g_ed.openCustomized) {
                    ThemeColors::activateCustomScheme(g_ed.openSnapshot);
                } else {
                    ThemeColors::deactivateCustomTheme();
                }
                ThemeColors::applyToImGui();
                ThemeColors::markPendingUiChromeResync();
            }
            g_ed.wasOpen = false;
        }
        return;
    }

// First frame open → snapshot
    if (!g_ed.wasOpen) {
        g_ed.wasOpen          = true;
        g_ed.schedulePeriod = state.themeScheduleEditorPeriod;
        g_ed.savedThisSession = false;
        g_ed.openCustomized   = ThemeColors::customized();
        g_ed.openTheme        = state.settings.theme;
        g_ed.openUseCustomColors = state.settings.useCustomColors;
        g_ed.openCustomThemeName = state.settings.customThemeName;
        g_ed.openSnapshot     = ThemeColors::current();
        g_ed.draft            = ThemeColors::current();
        if (g_ed.schedulePeriod >= 0) {
            g_ed.draft = harken::theme_schedule::Palette(g_ed.schedulePeriod == 0
                ? harken::theme_schedule::Period::Day : harken::theme_schedule::Period::Night);
            g_ed.livePreview = true;
            ApplyDraftToLive(state);
        }
        g_ed.initialDraft = g_ed.draft;
        g_ed.relationState.ResetToValues(g_ed.draft);
        g_ed.openRelationState = g_ed.relationState;
        g_ed.undoStack.clear();
        g_ed.exportMsg.clear();
        g_ed.importMsg.clear();
        g_ed.savedThemesLoaded = false;
        g_ed.activeSavedTheme.clear();
        g_ed.lastImportPath = GetExeDirUtf8() + "\\theme_export.json";
        g_ed.lastExportPath = BuildThemeJsonSuggestedPathUtf8("theme_export");

        if (!state.settings.customThemeName.empty())
            std::snprintf(g_ed.themeName, sizeof(g_ed.themeName), "%s",
                          state.settings.customThemeName.c_str());
        else
            std::snprintf(g_ed.themeName, sizeof(g_ed.themeName), "My Theme");

        g_ed.lastExportPath = BuildThemeJsonSuggestedPathUtf8("theme_export");
    }

    const auto& scheme = ThemeColors::current();
    const ImVec4 editorWindowBg = scheme.metadataPanelBG;
    const ImVec4 editorWindowEdge = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelLabel, scheme.filterBrowserOutline, 0.20f), 0.56f);
    const ImVec4 editorAccent = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelLabel, scheme.metadataPanelText, 0.06f), 1.0f);
    const ImVec4 editorChildBg = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelBG, scheme.filterBrowserBG, 0.08f), 1.0f);
    const ImVec4 editorChildEdge = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelLabel, scheme.metadataPanelText, 0.14f), 0.50f);
    const ImVec4 editorConsoleBg = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelBG, scheme.metadataPanelLabel, 0.10f), 0.99f);
    const ImVec4 editorConsoleBgAlt = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelBG, scheme.filterBrowserBG, 0.06f), 0.99f);
    const ImVec4 editorConsoleEdge = ThemeEditorWithAlpha(ThemeEditorMix(scheme.metadataPanelLabel, scheme.filterBrowserOutline, 0.26f), 0.46f);
    const ImVec4 editorStatusOk = ThemeEditorWithAlpha(ThemeEditorMix(scheme.volumeColor, scheme.metadataPanelText, 0.10f), 1.0f);
    const ImVec4 editorStatusInfo = ThemeEditorWithAlpha(ThemeEditorMix(scheme.controlAccent, scheme.metadataPanelText, 0.18f), 1.0f);
    const float editorWindowLuma = scheme.mainWindowBG.x * 0.299f + scheme.mainWindowBG.y * 0.587f + scheme.mainWindowBG.z * 0.114f;
    const bool editorLightTheme = editorWindowLuma >= 0.72f;
    const ImVec4 editorScrimBase = ThemeEditorMix(
        ThemeEditorMix(scheme.mainWindowBG, scheme.dialogBG, 0.54f),
        scheme.chromeBorder,
        editorLightTheme ? 0.06f : 0.14f);
    const ImVec4 editorNavDimBg = ThemeEditorWithAlpha(editorScrimBase, editorLightTheme ? 0.14f : 0.24f);
    const ImVec4 editorModalDimBg = ThemeEditorWithAlpha(editorScrimBase, editorLightTheme ? 0.22f : 0.36f);

// Window
    ImGui::SetNextWindowSize(ImVec2(720, 760), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(false, ImGuiCond_Appearing);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, editorWindowBg);
    ImGui::PushStyleColor(ImGuiCol_Border, ThemeEditorWithAlpha(editorWindowEdge, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, scheme.popupBG);
    ImGui::PushStyleColor(ImGuiCol_NavWindowingDimBg, editorNavDimBg);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, editorModalDimBg);
    const char* windowTitle = g_ed.schedulePeriod == 0 ? L(ThemeSchedule_EditDay)
        : g_ed.schedulePeriod == 1 ? L(ThemeSchedule_EditNight) : L(ThemeEditor_WindowTitle);
    if (!ImGui::Begin(windowTitle, &state.showThemeEditor)) {
        ImGui::End();
        ImGui::PopStyleColor(5);
        return;
    }
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    const auto qaPos=ImGui::GetWindowPos(),qaSize=ImGui::GetWindowSize();
    s_editorQaBounds={qaPos.x,qaPos.y,qaPos.x+qaSize.x,qaPos.y+qaSize.y};
#endif
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape))
        state.showThemeEditor = false;

    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetWindowPos();
        ImVec2 p1 = ImVec2(p0.x + ImGui::GetWindowSize().x, p0.y + ImGui::GetWindowSize().y);
        dl->AddRectFilledMultiColor(
            p0, p1,
            ImGui::GetColorU32(ThemeEditorWithAlpha(ThemeEditorMix(editorWindowBg, editorAccent, 0.05f), 0.18f)),
            ImGui::GetColorU32(ThemeEditorWithAlpha(ThemeEditorMix(editorWindowBg, editorAccent, 0.03f), 0.12f)),
            ImGui::GetColorU32(ThemeEditorWithAlpha(ThemeEditorMix(editorWindowBg, scheme.mainWindowBG, 0.10f), 0.00f)),
            ImGui::GetColorU32(ThemeEditorWithAlpha(ThemeEditorMix(editorWindowBg, scheme.mainWindowBG, 0.10f), 0.06f)));
        dl->AddLine(ImVec2(p0.x, p0.y + 34.0f), ImVec2(p1.x, p0.y + 34.0f),
            ImGui::GetColorU32(ThemeEditorWithAlpha(editorAccent, 0.18f)), 1.0f);
    }

    const float topConsoleH = 92.0f;
    const float bottomConsoleH = g_ed.schedulePeriod >= 0 ? 82.0f : 102.0f;
    if (!g_ed.savedThemesLoaded) RefreshSavedThemes();
    const float libraryH =
        (g_ed.schedulePeriod >= 0 || g_ed.savedThemes.empty()) ? 0.0f : 118.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, editorConsoleBg);
    ImGui::PushStyleColor(ImGuiCol_Border, editorConsoleEdge);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::BeginChild("##ThemeEditorTopConsole", ImVec2(0, topConsoleH), true);
    if (g_ed.schedulePeriod >= 0) {
        ImGui::TextUnformatted(g_ed.schedulePeriod == 0 ? L(ThemeSchedule_DayEditorHint) : L(ThemeSchedule_NightEditorHint));
        ImGui::SameLine();
        if (ImGui::Checkbox(L(ThemeEditor_LivePreview), &g_ed.livePreview)) {
            if (g_ed.livePreview) ApplyDraftToLive(state);
            else {
                const auto period = harken::theme_schedule::SelectedPeriod(state);
                if (period) {
                    ThemeColors::activateCustomScheme(harken::theme_schedule::Palette(*period));
                    ThemeColors::applyToImGui();
                    ThemeColors::markPendingUiChromeResync();
                } else {
                    RestoreOpenAppearance(state);
                }
            }
        }
        ObserveEditorItem("live-preview");
        ImGui::Separator();
        ImGui::TextUnformatted(L(Module_ColorPreset));
        const AppTheme seeds[] = {AppTheme::RoseCedar, AppTheme::JadeMist, AppTheme::Dark, AppTheme::Ocean, AppTheme::LapisBlue};
        for (int i = g_ed.schedulePeriod == 0 ? 0 : 2; i < (g_ed.schedulePeriod == 0 ? 2 : 5); ++i) {
            ImGui::SameLine();
            if (aura::ui::ActionButton(ThemeDiagPublicThemeLabel(seeds[i]))) {
                PushUndo();
                g_ed.draft = MakeBuiltinScheme(seeds[i]);
                g_ed.relationState.ResetToValues(g_ed.draft);
                if (g_ed.livePreview) ApplyDraftToLive(state);
            }
        }
    } else {
    ImGui::TextColored(ThemeEditorWithAlpha(editorAccent, 0.92f), L(ThemeEditor_ColorSession));
    ImGui::SameLine();
    ImGui::TextDisabled(L(ThemeEditor_PresetSectionHint));
    ImGui::Separator();

    ImGui::Text(L(ThemeEditor_ThemeNameLabel));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputText("##ThemeName", g_ed.themeName, sizeof(g_ed.themeName));
    ObserveEditorItem("theme-name");
    ImGui::SameLine();
    if (ImGui::Checkbox(L(ThemeEditor_LivePreview), &g_ed.livePreview)) {
        if (g_ed.livePreview) ApplyDraftToLive(state);
        else RestoreOpenAppearance(state);
    }

    ImGui::SameLine();
    {
        // Checkbox 会直接改草稿：先留一份切换前的副本，撤销才能回到原来的开关状态。
        bool mixing = g_ed.draft.colorMixing;
        if (ImGui::Checkbox(L(ThemeEditor_ColorMixing), &mixing)) {
            PushUndo();
            g_ed.draft.colorMixing = mixing;
            ThemeColors::resolveDerivedColors(g_ed.draft);
            g_ed.relationState.ResetToValues(g_ed.draft);
            if (g_ed.livePreview) ApplyDraftToLive(state);
        }
    }
    ObserveEditorItem("color-mixing");

    ImGui::SameLine();
    {
        bool gradient = g_ed.draft.resultsGradient;
        if (ImGui::Checkbox(L(ThemeEditor_ResultsGradient), &gradient)) {
            PushUndo();
            g_ed.draft.resultsGradient = gradient;
            if (g_ed.livePreview) ApplyDraftToLive(state);
        }
    }
    ObserveEditorItem("results-gradient");

    static int presetIdx = 0;
    ImGui::Text("%s", L(Module_ColorPreset));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("##PresetCombo", ThemePresetDisplayName(presetIdx))) {
        for (int i = 0; i < ThemeColors::getPresetCount(); ++i) {
            bool sel = (presetIdx == i);
            ImGui::PushID(i);
            if (ImGui::Selectable(ThemePresetDisplayName(i), sel)) {
                PushUndo();
                presetIdx = i;
                LoadPresetToDraft(i);
            }
            ObserveEditorItem(("preset-option-" + std::to_string(i)).c_str());
            if (sel) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ObserveEditorItem("preset-combo");

    }
    ImGui::SameLine();
    if (aura::ui::ActionButton(L(ThemeEditor_RestoreOnOpen))) {
        PushUndo();
        RestoreSnapshot(state);
    }

    ImGui::SameLine();
    {
        bool canUndo = !g_ed.undoStack.empty();
        if (!canUndo) ImGui::BeginDisabled();
        if (aura::ui::ActionButton(L(ThemeEditor_UndoBtn))) {
            UndoLast(state);
        }
        if (!canUndo) ImGui::EndDisabled();
    }

    if (g_ed.schedulePeriod < 0) {
        ImGui::SameLine(0.0f, 16.0f);
        const ImVec4 inspectOn  = ImVec4(0.14f, 0.62f, 0.60f, 1.0f);
        const bool wasInspect = g_ed.inspectMode;
        if (wasInspect) {
            ImGui::PushStyleColor(ImGuiCol_Button, inspectOn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ThemeEditorWithAlpha(inspectOn, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ThemeEditorWithAlpha(inspectOn, 0.70f));
        }
        if (ImGui::Button(g_ed.inspectMode ? L(TE_InspectOn) : L(TE_InspectBtn))) {
            g_ed.inspectMode = !g_ed.inspectMode;
            ThemeInspector::SetActive(g_ed.inspectMode);
        }
        if (wasInspect)
            ImGui::PopStyleColor(3);
        if (HoveredForHelp())
            HarkenSearch::SetTooltip(L(ThemeEditor_TokenHighlightHint));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    // Ctrl+Z shortcut
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !g_ed.undoStack.empty()) {
        UndoLast(state);
    }

// Inspect mode: consume scroll requests + decay highlight
    if (g_ed.inspectMode) {
        const char* scrollReq = ThemeInspector::ConsumeScrollRequest();
        if (scrollReq && ThemeEditorHasVisibleKey(scrollReq))
            g_ed.inspectScrollTarget = scrollReq;
        else if (scrollReq)
            g_ed.inspectScrollTarget.clear();
    }
    if (g_ed.inspectHighlightTimer > 0.f)
        g_ed.inspectHighlightTimer -= ImGui::GetIO().DeltaTime;

// Color groups
    ImGui::PushStyleColor(ImGuiCol_ChildBg, editorChildBg);
    ImGui::PushStyleColor(ImGuiCol_Border, editorChildEdge);
    ImGui::BeginChild("##ThemeColors", ImVec2(0, -bottomConsoleH - 8.0f - libraryH), true);
    DrawEditorLayer(ThemeEditorLayer::Base);
    DrawEditorLayer(ThemeEditorLayer::Regions);
    DrawEditorLayer(ThemeEditorLayer::Advanced);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

// Theme library: imported themes kept on this machine
    if (libraryH > 0.0f) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, editorChildBg);
        ImGui::PushStyleColor(ImGuiCol_Border, editorChildEdge);
        ImGui::BeginChild("##ThemeLibrary", ImVec2(0, libraryH - 8.0f), true);
        ImGui::TextDisabled("%s", L(ThemeEditor_SavedThemes));
        for (int i = 0; i < (int)g_ed.savedThemes.size(); ++i) {
            const std::string& themePath = g_ed.savedThemes[(size_t)i];
            const std::string label = std::filesystem::u8path(themePath).stem().u8string();
            ImGui::PushID(i);
            const bool activeTheme = (themePath == g_ed.activeSavedTheme);
            if (ImGui::Selectable(label.c_str(), activeTheme)) {
                const AppTheme startTheme = state.settings.theme;
                ThemeColorScheme imported = g_ed.draft;
                if (LoadEditorThemeFile(themePath, imported, "ThemeEditorLibrary", startTheme, true)) {
                    PushUndo();
                    g_ed.draft = imported;
                    g_ed.relationState.ResetToValues(g_ed.draft);
                    g_ed.lastImportPath = themePath;
                    g_ed.activeSavedTheme = themePath;
                    std::snprintf(g_ed.themeName, sizeof(g_ed.themeName), "%s", label.c_str());
                    ApplyDraftToLive(state);
                    char m[1024];
                    std::snprintf(m, sizeof(m), L(TE_ImportAppliedFmt), themePath.c_str());
                    g_ed.importMsg = m;
                } else {
                    g_ed.importMsg = L(TE_ImportFail);
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", themePath.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

// Bottom toolbar
    ImGui::PushStyleColor(ImGuiCol_ChildBg, editorConsoleBgAlt);
    ImGui::PushStyleColor(ImGuiCol_Border, editorConsoleEdge);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::BeginChild("##ThemeEditorBottomConsole", ImVec2(0, bottomConsoleH), true);
    if (g_ed.schedulePeriod >= 0) {
        if (aura::ui::ActionButton(L(ThemeSchedule_Save), ImVec2(140,0), aura::ui::ButtonVariant::PrimaryTransfer))
            SaveDraftAsCustomTheme(state, g_ed.exportMsg);
        ImGui::SameLine();
        if (aura::ui::ActionButton(L(ThemeSchedule_Cancel))) state.showThemeEditor = false;
        if (!g_ed.exportMsg.empty()) ImGui::TextWrapped("%s", g_ed.exportMsg.c_str());
        else ImGui::TextWrapped("%s", L(ThemeSchedule_SaveHint));
    } else {
    ImGui::TextColored(ThemeEditorWithAlpha(editorAccent, 0.88f), L(ThemeEditor_OutputApply));
    ImGui::SameLine();
    ImGui::TextDisabled(L(ThemeEditor_SaveSectionHint));
    ImGui::Separator();

    {
        bool nameEmpty = (g_ed.themeName[0] == '\0');
        if (nameEmpty) ImGui::BeginDisabled();
        if (aura::ui::ActionButton(L(ThemeEditor_SaveAsNew), ImVec2(130, 0), aura::ui::ButtonVariant::PrimaryTransfer)) {
            SaveDraftAsCustomTheme(state, g_ed.exportMsg);
        }
        if (nameEmpty) ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (aura::ui::ActionButton(L(ThemeEditor_Discard))) {
        PushUndo();
        RestoreSnapshot(state);
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // Import
    if (aura::ui::ActionButton(L(ThemeEditor_ImportJson), ImVec2(100, 0))) {
#ifdef _WIN32
        const std::string suggestedPath = g_ed.lastImportPath.empty()
            ? (GetExeDirUtf8() + "\\theme_export.json")
            : g_ed.lastImportPath;
        std::string filePath = Platform::ThemeDialogs::OpenThemeJsonFileDialogUtf8(g_hwnd, suggestedPath);
        if (!filePath.empty()) {
            const AppTheme startTheme = state.settings.theme;
            ThemeColorScheme imported = g_ed.draft;
            if (LoadEditorThemeFile(filePath, imported, "ThemeEditorImport", startTheme, g_ed.livePreview)) {
                PushUndo();
                g_ed.draft = imported;
                g_ed.relationState.ResetToValues(g_ed.draft);
                g_ed.lastImportPath = filePath;
                g_ed.lastExportPath = filePath;
                SetThemeNameFromPathUtf8(filePath);
                if (g_ed.livePreview) ApplyDraftToLive(state);
                { char m[1024]; std::snprintf(m, sizeof(m), L(TE_ImportAppliedFmt), filePath.c_str()); g_ed.importMsg = m; }
                std::string libraryError;
                if (!harken::theme_library::SaveThemeCopy(ThemeLibraryDir(),
                                                          ThemeNameFromPathUtf8(filePath),
                                                          ThemeColors::exportToJSON(imported),
                                                          libraryError)) {
                    g_ed.importMsg = std::string(L(TE_LibrarySaveFail)) + " (" + libraryError + ")";
                } else {
                    RefreshSavedThemes();
                }
            } else {
                g_ed.importMsg = L(TE_ImportFail);
            }
        }
#else
        g_ed.importMsg = L(TE_ImportUnsupported);
#endif
    }

    ImGui::SameLine();

    if (aura::ui::ActionButton(L(ThemeEditor_ImportCreateTheme), ImVec2(148, 0), aura::ui::ButtonVariant::PrimarySearch)) {
#ifdef _WIN32
        const std::string suggestedPath = g_ed.lastImportPath.empty()
            ? (GetExeDirUtf8() + "\\theme_export.json")
            : g_ed.lastImportPath;
        std::string filePath = Platform::ThemeDialogs::OpenThemeJsonFileDialogUtf8(g_hwnd, suggestedPath);
        if (!filePath.empty()) {
            const AppTheme startTheme = state.settings.theme;
            ThemeColorScheme imported = g_ed.draft;
            if (LoadEditorThemeFile(filePath, imported, "ThemeEditorImportCreate", startTheme, true)) {
                PushUndo();
                g_ed.draft = imported;
                g_ed.relationState.ResetToValues(g_ed.draft);
                g_ed.lastImportPath = filePath;
                g_ed.lastExportPath = filePath;
                SetThemeNameFromPathUtf8(filePath);
                ApplyDraftToLive(state);
                if (SaveDraftAsCustomTheme(state, g_ed.exportMsg)) {
                    char m[512]; std::snprintf(m, sizeof(m), L(TE_ImportCreatedFmt), g_ed.themeName); g_ed.importMsg = m;
                } else {
                    g_ed.importMsg = L(TE_ImportFail);
                }
            } else {
                g_ed.importMsg = L(TE_ImportFail);
            }
        }
#else
        g_ed.importMsg = L(TE_ImportUnsupported);
#endif
    }

    ImGui::SameLine();

    // Export
    if (aura::ui::ActionButton(L(ThemeEditor_ExportJson), ImVec2(100, 0))) {
        std::string json = ThemeColors::exportToJSON(g_ed.draft);
        ImGui::SetClipboardText(json.c_str());

#ifdef _WIN32
        const std::string suggestedPath = g_ed.lastExportPath.empty()
            ? BuildThemeJsonSuggestedPathUtf8("theme_export")
            : g_ed.lastExportPath;
        std::string filePath = Platform::ThemeDialogs::SaveThemeJsonFileDialogUtf8(g_hwnd, suggestedPath);
        if (!filePath.empty()) {
            if (ThemeColors::saveThemeToFile(filePath, g_ed.draft)) {
                g_ed.lastExportPath = filePath;
                g_ed.lastImportPath = filePath;
                { char m[1024]; std::snprintf(m, sizeof(m), L(TE_ExportClipboardFmt), filePath.c_str()); g_ed.exportMsg = m; }
            } else {
                g_ed.exportMsg = L(TE_CopiedWriteFail);
            }
        } else {
            g_ed.exportMsg = L(TE_Copied);
        }
#else
        g_ed.exportMsg = L(TE_ExportUnsupportedClipboard);
#endif
    }

    if (!g_ed.exportMsg.empty()) {
        ImGui::TextColored(editorStatusOk, "%s", g_ed.exportMsg.c_str());
    } else {
        ImGui::TextDisabled(L(ThemeEditor_ExportHint));
    }
    if (!g_ed.importMsg.empty()) {
        ImGui::TextColored(editorStatusInfo, "%s", g_ed.importMsg.c_str());
    } else {
        ImGui::TextDisabled(L(ThemeEditor_ImportHint));
    }

// DEV only: C++ preset code generation
#if AURA_BUILD_DIAGNOSTICS
    ImGui::Separator();
    ImGui::TextDisabled(L(ThemeEditor_DevSavePreset));

    auto deriveFuncName = [](const char* name) -> std::string {
        std::string raw = TrimCopy(name);
        if (raw.empty()) return "getMyPreset";
        std::string result = "get";
        bool nextUpper = true;
        for (char ch : raw) {
            if (ch == ' ' || ch == '_' || ch == '-') { nextUpper = true; continue; }
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')))
                continue;
            result += nextUpper ? (char)toupper((unsigned char)ch) : ch;
            nextUpper = false;
        }
        return result.size() <= 3 ? "getMyPreset" : result;
    };

    static char s_presetFuncName[64] = "";
    static bool s_funcNameSyncedFromThemeName = true;
    if (s_funcNameSyncedFromThemeName || s_presetFuncName[0] == '\0') {
        std::string derived = deriveFuncName(g_ed.themeName);
        std::snprintf(s_presetFuncName, sizeof(s_presetFuncName), "%s", derived.c_str());
    }
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("##PresetFuncName", s_presetFuncName, sizeof(s_presetFuncName)))
        s_funcNameSyncedFromThemeName = false;
    ImGui::SameLine();
    static std::string s_presetSaveMsg;
    if (aura::ui::ActionButton(L(ThemeEditor_GenerateCpp))) {
        size_t fieldCount = 0;
        const ThemeEditorItem* fields = GetThemeEditorItems(fieldCount);

        auto fmtColor = [](const ImVec4& c) -> std::string {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "ImVec4(%.3ff, %.3ff, %.3ff, %.3ff)", c.x, c.y, c.z, c.w);
            return buf;
        };

        std::string funcName = TrimCopy(s_presetFuncName);
        if (funcName.empty()) funcName = deriveFuncName(g_ed.themeName);

        std::ostringstream out;
        out << "// Theme: " << g_ed.themeName << "\n";
        out << "ThemeColorScheme ThemeColors::" << funcName << "() {\n";
        out << "    ThemeColorScheme s;\n";
        for (size_t i = 0; i < fieldCount; ++i) {
            const auto& f = fields[i];
            if (f.colorOverride && !(g_ed.draft.*(f.colorOverride->member))) continue;
            const ImVec4 val = f.Color(g_ed.draft);
            // Pad field name for alignment
            std::string padded = std::string("s.") + f.key;
            while (padded.size() < 28) padded += ' ';
            out << "    " << padded << " = " << fmtColor(val) << ";\n";
        }
        out << "    return s;\n";
        out << "}\n";

        std::string code = out.str();
        ImGui::SetClipboardText(code.c_str());

        try {
            std::string exeDir = GetExeDirUtf8();
            std::string filePath = exeDir + "\\theme_preset_code.cpp";
            if (Platform::FileSystem::WriteFileText(filePath, code)) {
                s_presetSaveMsg = "Copied to clipboard + " + filePath;
            } else {
                s_presetSaveMsg = "Copied to clipboard (file write failed)";
            }
        } catch (...) {
            s_presetSaveMsg = "Copied to clipboard (file write failed)";
        }
    }
    if (!s_presetSaveMsg.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(editorStatusOk, "%s", s_presetSaveMsg.c_str());
    }
#endif
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    ImGui::End();
    ImGui::PopStyleColor(5);
}
