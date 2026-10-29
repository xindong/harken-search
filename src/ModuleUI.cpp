// SoundManager - 新模块 UI 集成
// ModuleUI.cpp - UCS / 标签 / 嵌入 / 命名 / 传输 / 工作流 / 主题 / 字段 的 UI 渲染
//
// 每个模块的 UI 作为独立函数实现，在 RenderModuleDialogs() 中统一调度。

#include "ModuleUI.h"
#include "platform/filesystem.h"
#include "platform/system.h"
#include "AppState.h"
#include "Database.h"
#include "imgui.h"
#include "HarkenSearchWidgets.h"

#include "UCSCategory.h"
#include "UndoSystem.h"
#include "MetadataFields.h"
#include "TagSystem.h"
#include "EmbedScript.h"
#include "NamingScript.h"
#include "TransferEngine.h"
#include "WorkflowEngine.h"
#include "Theme.h"
#include "ThemeEditor.h"
#include "BuildConfig.h"
#include "Utils.h"
#include "UiHostServices.h"
#include "MetadataWriter.h"
#include "Localization.h"

#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef CopyFile
#undef CopyFile
#endif

namespace fs = std::filesystem;

static Platform::FileSystem::PathStatus ProbeUiPath(const fs::path& path) {
    return Platform::FileSystem::ProbePath(PathToUtf8(path));
}

// Forward declarations from main.cpp.
void RefreshSearchResults(AppState& state, bool resetPagination);
void SaveAppSettings(const AppState& state);
bool RenameFileOnDisk(AppState& state,
                      int64_t recordId,
                      const std::string& oldPath,
                      const std::string& newFileName);

// Helper: 将选中的 AudioRecord 转为 metadata map

static std::string GetStemmedFileNameForNaming(const AudioRecord& rec) {
    try {
        const std::string& candidate = rec.fileName.empty() ? rec.filePath : rec.fileName;
        return PathToUtf8(Utf8ToPath(candidate).stem());
    } catch (...) {
        return rec.fileName;
    }
}

static std::string GetExtendedNamingField(const AudioRecord& rec,
                                          const char* primaryKey,
                                          const char* secondaryKey = nullptr) {
    const std::string& primary = rec.getMeta(primaryKey);
    if (!primary.empty()) return primary;
    if (secondaryKey) {
        const std::string& secondary = rec.getMeta(secondaryKey);
        if (!secondary.empty()) return secondary;
    }
    return "";
}

static std::unordered_map<std::string, std::string> BuildMetadataMap(const AudioRecord& rec) {
    std::unordered_map<std::string, std::string> m;
    const std::string stemmedFileName = GetStemmedFileNameForNaming(rec);
    m["Filename"] = stemmedFileName;
    m["FilenameWithExtension"] = rec.fileName;
    m["FilePath"] = rec.filePath;
    m["FolderPath"] = rec.folderPath;
    m["Description"] = rec.description;
    m["Category"] = rec.category;
    m["Keywords"] = rec.keywords;
    m["Title"] = rec.title;
    m["Artist"] = rec.artist;
    m["Album"] = rec.album;
    m["Format"] = rec.format;
    m["SampleRate"] = std::to_string(rec.sampleRate);
    m["Channels"] = std::to_string(rec.channels);
    m["Duration"] = std::to_string(rec.durationSeconds);
    m["Extension"] = rec.extension;
    m["SubCategory"] = GetExtendedNamingField(rec, "subcategory");
    m["Library"] = GetExtendedNamingField(rec, "library");
    m["Designer"] = GetExtendedNamingField(rec, "designer");
    m["Show"] = GetExtendedNamingField(rec, "show_name", "show");
    m["FXName"] = GetExtendedNamingField(rec, "fx_name");
    m["ShortID"] = GetExtendedNamingField(rec, "short_id");
    m["LongID"] = GetExtendedNamingField(rec, "long_id");
    m["TrackTitle"] = GetExtendedNamingField(rec, "track_title");
    m["Scene"] = GetExtendedNamingField(rec, "scene");
    m["Take"] = GetExtendedNamingField(rec, "take_num", "take");
    m["Source"] = GetExtendedNamingField(rec, "source");
    m["CatID"] = GetExtendedNamingField(rec, "cat_id", "catid");
    m["CategoryFull"] = GetExtendedNamingField(rec, "category_full");
    if (m["Library"].empty()) m["Library"] = rec.album;
    if (m["Designer"].empty()) m["Designer"] = rec.artist;
    if (m["TrackTitle"].empty()) m["TrackTitle"] = rec.title;
    if (m["FXName"].empty()) m["FXName"] = stemmedFileName;
    return m;
}

static int FindNamingPresetIndexByName(const std::vector<NamingPreset>& presets,
                                       const std::string& presetName) {
    if (presetName.empty()) return -1;
    for (int i = 0; i < (int)presets.size(); ++i) {
        if (presets[i].name == presetName) return i;
    }
    return -1;
}

static bool IsCustomNamingPresetName(const AppState& state, const std::string& presetName) {
    return std::any_of(state.settings.namingCustomPresets.begin(),
                       state.settings.namingCustomPresets.end(),
                       [&](const NamingPreset& preset) { return preset.name == presetName; });
}

static void SyncNamingPresetState(AppState& state) {
    static bool s_namingPresetsHydrated = false;

    NamingScriptEngine::initialize();
    if (!s_namingPresetsHydrated) {
        for (const auto& preset : state.settings.namingCustomPresets) {
            NamingScriptEngine::addPreset(preset);
        }
        s_namingPresetsHydrated = true;
    }

    if (state.settings.namingPattern.empty()) {
        state.settings.namingPattern = "<Description>_<Category>";
    }
    snprintf(state.namingPatternBuffer, sizeof(state.namingPatternBuffer),
             "%s", state.settings.namingPattern.c_str());

    const auto& presets = NamingScriptEngine::getPresets();
    state.namingPresetIndex = FindNamingPresetIndexByName(presets, state.settings.namingPresetName);
    if (state.namingPresetIndex >= 0 &&
        state.namingPresetIndex < (int)presets.size() &&
        presets[state.namingPresetIndex].pattern != state.namingPatternBuffer) {
        state.namingPresetIndex = -1;
    }
}

static void PersistNamingDialogState(AppState& state) {
    state.settings.namingPattern = state.namingPatternBuffer;
    const auto& presets = NamingScriptEngine::getPresets();
    if (state.namingPresetIndex >= 0 &&
        state.namingPresetIndex < (int)presets.size() &&
        presets[state.namingPresetIndex].pattern == state.settings.namingPattern) {
        state.settings.namingPresetName = presets[state.namingPresetIndex].name;
    } else {
        state.settings.namingPresetName.clear();
        state.namingPresetIndex = -1;
    }
    SaveAppSettings(state);
}

static int GetActiveNamingMaxLength(const AppState& state) {
    const auto& presets = NamingScriptEngine::getPresets();
    if (state.namingPresetIndex >= 0 && state.namingPresetIndex < (int)presets.size()) {
        return presets[state.namingPresetIndex].maxLength;
    }
    return 255;
}

// 模块初始化

void InitializeModules() {
    UCSCategoryManager::initialize();
    MetadataFieldManager::initialize();
    TagSystemManager::initialize();
    NamingScriptEngine::initialize();
}

// 工具菜单

void RenderToolsMenu(AppState& state) {
    if (ImGui::BeginMenu(L(Menu_Tools))) {
        ImGui::TextDisabled("%s", L(Module_CategoryTags));
        if (ImGui::MenuItem(L(Module_UCSBrowser))) {
            state.showUCSBrowser = true;
            state.panelUCSBrowser.visible = true;
        }
        if (ImGui::MenuItem(L(Module_TagSystem))) {
            state.showTagBrowser = true;
            state.panelTagBrowser.visible = true;
        }
        if (ImGui::MenuItem(L(Module_FieldManager))) {
            state.showFieldManager = true;
        }

        ImGui::Separator();
        ImGui::TextDisabled("%s", L(Module_MetadataProcess));
        if (ImGui::MenuItem(L(Module_EmbedMetadata))) {
            state.showEmbedDialog = true;
        }
        if (ImGui::MenuItem(L(Module_NamingScript))) {
            state.showNamingDialog = true;
        }
        if (ImGui::MenuItem(L(Module_WorkflowEngine))) {
            state.showWorkflowDialog = true;
            state.panelWorkflow.visible = true;
        }

        ImGui::Separator();
        ImGui::TextDisabled("%s", L(Module_FileOps));
        if (ImGui::MenuItem(L(Module_FileTransfer))) {
            state.showTransferDialog = true;
            state.panelTransfer.visible = true;
        }

        if (ProductEdition::hasFeature("theme_editor")) {
            ImGui::Separator();
            ImGui::TextDisabled("%s", L(Module_Appearance));
            if (ImGui::MenuItem(L(Module_ThemeEditor))) {
                state.showThemeEditor = true;
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled(L(ModuleUI_MaintenanceMenu));
        {
            bool rebuilding = state.ftsRebuildProgress.load() >= 0.0f;
            if (ImGui::MenuItem(L(ModuleUI_RebuildFTS), nullptr, false, !rebuilding)) {
                if (state.database.isOpen()) {
                    state.ftsRebuildProgress.store(0.0f);
                    state.ftsRebuildCancel.store(false);
                    std::thread([&state]() {
                        const bool rebuilt = state.database.rebuildFtsIndexBatched(
                            2000, &state.ftsRebuildProgress, &state.ftsRebuildCancel);
                        state.ftsRebuildProgress.store(-1.0f); // done
                        state.loadErrorToast = rebuilt
                            ? L(Module_IdxRebuilt)
                            : std::string(L(ModuleUI_RebuildFTS)) + ": " + L(Export_Failed);
                        state.loadErrorToastUntil = ImGui::GetTime() + 3.0;
                    }).detach();
                }
            }
        }

        ImGui::EndMenu();
    }
}

// 1) UCS 分类浏览器

static int s_ucsApplyCount = 0;
static char s_ucsApplyMsgBuf[1024];
static char s_resultMsgBuf[512];
static void appendMsgFmt(std::string& msg, LStr key, int n) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), GetLocalizedString(key), n);
    msg += buf;
}
static std::string s_ucsApplyMsg;
static bool s_ucsConfirmOverwrite = false;
static std::string s_ucsPendingCatValue;
static int s_ucsPendingWithCat = 0;
static int s_ucsPendingTotal = 0;

void RenderUCSBrowserContent(AppState& state) {
    if (!state.showUCSBrowser) return;


    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) { state.showUCSBrowser = false; }

    // 搜索栏
    ImGui::Text("%s", L(Module_SearchCategory));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##UCSSearch", state.ucsBrowserSearch, sizeof(state.ucsBrowserSearch));

    ImGui::Separator();

    std::string query = state.ucsBrowserSearch;

    // 搜索结果模式 vs 树形浏览模式
    if (!query.empty()) {
        // 搜索结果
        auto results = UCSCategoryManager::search(query, 50);
        ImGui::Text(L(Module_FoundMatches), (int)results.size());
        ImGui::Separator();

        ImGui::BeginChild("##UCSResults", ImVec2(0, -60), true);
        if (ImGui::BeginTable("##UCSTable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("CatID", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn(L(Col_MainCategory), ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn(L(MetaLabel_SubCategory), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(L(Main_LangChinese), ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableSetupColumn(L(Set_ColDescription), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)results.size(); i++) {
                const auto* e = results[i];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                bool selected = (state.ucsBrowserSelectedIdx == i);
                ImGui::PushID(e->catId.c_str());
                if (ImGui::Selectable(e->catId.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    state.ucsBrowserSelectedIdx = i;
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e->category.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e->subCategory.c_str());
                ImGui::TableNextColumn();
                std::string zh = e->category_zh;
                if (!e->subCategory_zh.empty()) zh += " / " + e->subCategory_zh;
                ImGui::TextUnformatted(zh.c_str());
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", e->explanation.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();

        // 应用到选中文件按钮
        auto selectedRecords = state.getSelectedRecords();
        ImGui::BeginDisabled(selectedRecords.empty() || state.ucsBrowserSelectedIdx < 0
                             || state.ucsBrowserSelectedIdx >= (int)results.size());
        if (aura::ui::ActionButton(L(ModuleUI_ApplyCategoryBtn), ImVec2(0, 0), aura::ui::ButtonVariant::PrimarySearch)) {
            const auto* entry = results[state.ucsBrowserSelectedIdx];
            std::string catValue = entry->category;
            if (!entry->subCategory.empty()) catValue += " - " + entry->subCategory;

            int withCat = 0;
            for (const auto& rec : selectedRecords) {
                if (!rec.category.empty()) withCat++;
            }
            if (withCat > 0 && !state.settings.suppressConfirmDialogs) {
                s_ucsConfirmOverwrite = true;
                s_ucsPendingCatValue = catValue;
                s_ucsPendingWithCat = withCat;
                s_ucsPendingTotal = (int)selectedRecords.size();
            } else {
                // all empty — apply directly with undo
                s_ucsApplyCount = 0;
                auto undoOp = std::make_unique<MetadataPatchOp>();
                undoOp->operationName = "UCS category assign";
                if (state.database.beginTransaction()) {
                    bool abort = false;
                    for (const auto& rec : selectedRecords) {
                        MetadataEditPatch patch;
                        patch.setCategory(catValue);
                        if (state.database.applyMetadataPatch(rec.id, patch)) {
                            MetadataPatchEntry e;
                            e.fileId = rec.id;
                            e.oldValues["category"] = rec.category;
                            e.newValues["category"] = catValue;
                            undoOp->entries.push_back(std::move(e));
                            s_ucsApplyCount++;
                        } else { abort = true; break; }
                    }
                    if (abort || !state.database.commitTransaction()) {
                        state.database.rollbackTransaction();
                        s_ucsApplyCount = 0;
                        s_ucsApplyMsg = L(Module_WriteFailRollback);
                    } else {
                        if (!undoOp->entries.empty())
                            state.undoStack.push(std::move(undoOp));
                        state.database.invalidateFacetQueryCache();
                        std::snprintf(s_ucsApplyMsgBuf, sizeof(s_ucsApplyMsgBuf), L(Module_UcsAppliedFmt),
                                        s_ucsApplyCount, catValue.c_str());
                        s_ucsApplyMsg = s_ucsApplyMsgBuf;
                    }
                } else {
                    s_ucsApplyMsg = L(Module_TxnBeginFail);
                }
                RefreshSearchResults(state, false);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text(L(Module_FilesSelected), (int)selectedRecords.size());

        // Overwrite confirmation popup
        if (s_ucsConfirmOverwrite) {
            ImGui::OpenPopup(u8"##UCSOverwriteConfirm");
            s_ucsConfirmOverwrite = false;
        }
        if (ImGui::BeginPopupModal(u8"##UCSOverwriteConfirm", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::Text(L(ModuleUI_UcsPendingCatCount), s_ucsPendingTotal, s_ucsPendingWithCat);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), L(ModuleUI_UcsNewCategoryFmt), s_ucsPendingCatValue.c_str());
            ImGui::Separator();

            // mode: 0=overwrite all, 1=fill empty only, 2=append suffix
            auto applyUcs = [&](int mode) {
                auto recs = state.getSelectedRecords();
                s_ucsApplyCount = 0;
                auto undoOp = std::make_unique<MetadataPatchOp>();
                undoOp->operationName = "UCS category assign";
                if (state.database.beginTransaction()) {
                    bool abort = false;
                    for (const auto& rec : recs) {
                        if (mode == 1 && !rec.category.empty()) continue; // fill empty only
                        std::string newCat;
                        if (mode == 2 && !rec.category.empty())
                            newCat = rec.category + " | " + s_ucsPendingCatValue; // append
                        else
                            newCat = s_ucsPendingCatValue;
                        MetadataEditPatch patch;
                        patch.setCategory(newCat);
                        if (state.database.applyMetadataPatch(rec.id, patch)) {
                            MetadataPatchEntry e;
                            e.fileId = rec.id;
                            e.oldValues["category"] = rec.category;
                            e.newValues["category"] = newCat;
                            undoOp->entries.push_back(std::move(e));
                            s_ucsApplyCount++;
                        } else { abort = true; break; }
                    }
                    if (abort || !state.database.commitTransaction()) {
                        state.database.rollbackTransaction();
                        s_ucsApplyCount = 0;
                        s_ucsApplyMsg = L(Module_WriteFailRollback);
                    } else {
                        if (!undoOp->entries.empty())
                            state.undoStack.push(std::move(undoOp));
                        state.database.invalidateFacetQueryCache();
                        std::snprintf(s_ucsApplyMsgBuf, sizeof(s_ucsApplyMsgBuf), L(Module_UcsAppliedFmt),
                                        s_ucsApplyCount, s_ucsPendingCatValue.c_str());
                        s_ucsApplyMsg = s_ucsApplyMsgBuf;
                    }
                } else {
                    s_ucsApplyMsg = L(Module_TxnBeginFail);
                }
                RefreshSearchResults(state, false);
            };

            if (aura::ui::ActionButton(L(ModuleUI_OverwriteAll), ImVec2(120, 0))) {
                applyUcs(0);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (aura::ui::ActionButton(L(ModuleUI_FillBlankOnly), ImVec2(120, 0))) {
                applyUcs(1);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (aura::ui::ActionButton(L(ModuleUI_AppendSuffix), ImVec2(100, 0))) {
                applyUcs(2);
                ImGui::CloseCurrentPopup();
            }
            if (HoveredForHelp())
                HarkenSearch::SetTooltip(L(ModuleUI_CategoryAppendTip));
            ImGui::SameLine();
            if (aura::ui::ActionButton(L(ModuleUI_Cancel), ImVec2(80, 0), aura::ui::ButtonVariant::Quiet)) {
                s_ucsApplyMsg.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (!s_ucsApplyMsg.empty()) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%s", s_ucsApplyMsg.c_str());
        }

    } else {
        // 树形浏览模式
        auto& tree = UCSCategoryManager::getCategoryTree();
        ImGui::Text(L(Module_CategoryStats), (int)tree.size(), UCSCategoryManager::getEntryCount());
        ImGui::Separator();

        ImGui::BeginChild("##UCSTree", ImVec2(0, 0), true);
        for (const auto& cat : tree) {
            std::string label = cat.name;
            if (!cat.name_zh.empty()) label += "  (" + cat.name_zh + ")";
            char nodeLabel[256];
            snprintf(nodeLabel, sizeof(nodeLabel), "%s  [%d]", label.c_str(), cat.totalCount);

            if (ImGui::TreeNode(nodeLabel)) {
                for (const auto& entry : cat.entries) {
                    std::string subLabel = entry.subCategory;
                    if (!entry.subCategory_zh.empty()) subLabel += "  (" + entry.subCategory_zh + ")";
                    subLabel += "  [" + entry.catId + "]";

                    ImGui::PushID(entry.catId.c_str());
                    if (ImGui::Selectable(subLabel.c_str())) {
                        // 选中子分类 → 切到搜索模式
                        snprintf(state.ucsBrowserSearch, sizeof(state.ucsBrowserSearch), "%s", entry.catId.c_str());
                    }
                    if (ImGui::IsItemHovered() && !entry.explanation.empty()) {
                        HarkenSearch::SetTooltip("%s\n%s", entry.explanation.c_str(), entry.synonyms.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();
    }

}

// 2) 标签系统面板

void RenderTagBrowserContent(AppState& state) {
    if (!state.showTagBrowser) return;


    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) { state.showTagBrowser = false; }

    // 清除所有过滤器按钮
    if (TagSystemManager::hasAnyFacetFilter()) {
        if (aura::ui::ActionButton(L(ModuleUI_ClearAllFilters))) {
            TagSystemManager::clearAllFacets();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", L(Module_FilterActive));
    }

    ImGui::Separator();

    // 逐维度显示
    auto dims = TagSystemManager::getAllDimensions();
    for (auto dim : dims) {
        auto& facet = TagSystemManager::getFacetState(dim);
        // 10-B O3: render-layer localized facet headers (WelcomeGreeting Band precedent);
        // getDimensionName/getDimensionNameZh stay as contract data helpers, search
        // matching (zh+en columns) untouched.
        static constexpr LStr kFacetDimNames[] = {
            LStr::Info_Category, LStr::Col_Mood, LStr::Col_FieldInstrument, LStr::TagDim_Tempo,
            LStr::TagDim_Usage, LStr::TagDim_Version, LStr::TagDim_Era, LStr::Col_Duration,
            LStr::TagDim_BpmRange,
        };
        const int dimIdx = static_cast<int>(dim);
        const char* dimName = (dimIdx >= 0 && dimIdx < static_cast<int>(sizeof(kFacetDimNames) / sizeof(kFacetDimNames[0])))
            ? GetLocalizedString(kFacetDimNames[dimIdx]) : "Unknown";

        // 带箭头的折叠标题
        bool open = ImGui::CollapsingHeader(dimName, facet.hasFilter() ? ImGuiTreeNodeFlags_DefaultOpen : 0);

        // 如果有过滤器, 显示小徽章
        if (facet.hasFilter()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), L(ModuleUI_FacetCountFmt), (int)facet.selectedValues.size());
        }

        if (open) {
            auto tags = TagSystemManager::getTagsByDimension(dim);
            ImGui::Indent(8.0f);

            for (const auto* tag : tags) {
                bool sel = facet.isSelected(tag->value);
                std::string label = tag->value;
                if (!tag->value_zh.empty()) label += "  " + tag->value_zh;

                ImGui::PushID(tag->value.c_str());
                if (ImGui::Checkbox(label.c_str(), &sel)) {
                    facet.toggle(tag->value);
                }
                ImGui::PopID();
            }

            // 清除本维度
            if (facet.hasFilter()) {
                if (ImGui::SmallButton(L(ModuleUI_Clear))) {
                    facet.clear();
                }
            }

            ImGui::Unindent(8.0f);
        }
    }

    ImGui::Separator();
    ImGui::TextWrapped(L(ModuleUI_TagFilterHint));

}

// 3) 元数据嵌入对话框

static EmbedConfig s_embedConfig;
static bool s_embedConfigInited = false;
static std::string s_embedResultMsg;
static int s_embedSuccessCount = 0;
static int s_embedFailCount = 0;

static void RenderEmbedDialog(AppState& state) {
    if (!state.showEmbedDialog) return;

    if (!s_embedConfigInited) {
        s_embedConfig = EmbedEngine::getDefaultConfig();
        s_embedConfigInited = true;
    }

    ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(L(ModuleUI_EmbedMetadataTitle), &state.showEmbedDialog)) {
        ImGui::End();
        return;
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) { state.showEmbedDialog = false; }

    ImGui::TextWrapped(L(ModuleUI_EmbedMetadataIntro));
    ImGui::Separator();

    // 选中文件信息
    auto selected = state.getSelectedRecords();
    ImGui::Text(L(ModuleUI_SelFileCountFmt), (int)selected.size());
    if (selected.empty()) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "%s", L(Module_SelectFilesFirst));
    }
    ImGui::Separator();

    // 嵌入格式复选框
    ImGui::Text("%s", L(Module_EmbedFormat));
    ImGui::Checkbox(L(ModuleUI_EmbedBext), &s_embedConfig.embedBEXT);
    if (HoveredForHelp()) HarkenSearch::SetTooltip(L(ModuleUI_EmbedBextTip));
    ImGui::Checkbox(L(ModuleUI_EmbedIxml), &s_embedConfig.embedIXML);
    if (HoveredForHelp()) HarkenSearch::SetTooltip(L(ModuleUI_EmbedIxmlTip));
    ImGui::Checkbox(L(ModuleUI_EmbedSteinberg), &s_embedConfig.embedSteinberg);
    if (HoveredForHelp()) HarkenSearch::SetTooltip(L(ModuleUI_EmbedSteinbergTip));
    ImGui::Checkbox(L(ModuleUI_EmbedId3), &s_embedConfig.embedID3);
    if (HoveredForHelp()) HarkenSearch::SetTooltip(L(ModuleUI_EmbedId3Tip));
    ImGui::Checkbox(L(ModuleUI_EmbedListInfo), &s_embedConfig.embedLISTInfo);
    ImGui::Checkbox(L(ModuleUI_EmbedAXml), &s_embedConfig.embedAXML);
    ImGui::Checkbox(L(ModuleUI_EmbedIxmlUser), &s_embedConfig.embedIXMLUser);
    ImGui::Checkbox(L(ModuleUI_EmbedCopyCover), &s_embedConfig.embedPicture);

    ImGui::Separator();

    // 预览
    if (!selected.empty()) {
        ImGui::Text("%s", L(Module_PreviewFirst));
        auto meta = BuildMetadataMap(selected[0]);
        EmbedExtraParams extra;
        BEXTData bext;
        IXMLData ixml;
        SteinbergData steinberg;
        ID3EmbedData id3;
        LISTInfoData list;

        EmbedEngine::buildEmbedData(meta, selected[0].format, s_embedConfig, extra,
                                     bext, ixml, steinberg, id3, list);

        if (ImGui::TreeNode(L(ModuleUI_PreviewBext))) {
            ImGui::Text(L(ModuleUI_FieldDescriptionFmt), bext.description.c_str());
            ImGui::Text(L(ModuleUI_FieldOriginatorFmt), bext.originator.c_str());
            ImGui::Text(L(ModuleUI_FieldDateFmt), bext.date.c_str());
            ImGui::TreePop();
        }
        if (ImGui::TreeNode(L(ModuleUI_PreviewId3))) {
            ImGui::Text(L(ModuleUI_FieldTitleFmt), id3.title.c_str());
            ImGui::Text(L(ModuleUI_FieldArtistFmt), id3.leadArtist.c_str());
            ImGui::Text(L(ModuleUI_FieldAlbumFmt), id3.album.c_str());
            ImGui::Text(L(ModuleUI_FieldCommentFmt), id3.comment.c_str());
            ImGui::TreePop();
        }
        if (ImGui::TreeNode(L(ModuleUI_PreviewSteinberg))) {
            ImGui::Text(L(ModuleUI_FieldCategoryFmt), steinberg.mediaCategoryPost.c_str());
            ImGui::Text(L(ModuleUI_FieldCommentFmt), steinberg.mediaComment.c_str());
            ImGui::Text(L(ModuleUI_FieldArtistFmt), steinberg.mediaArtist.c_str());
            ImGui::TreePop();
        }
    }

    ImGui::Separator();

    // 执行按钮
    ImGui::BeginDisabled(selected.empty());
    if (aura::ui::ActionButton(L(ModuleUI_RunEmbed), ImVec2(160, 35), aura::ui::ButtonVariant::PrimarySearch)) {
        s_embedSuccessCount = 0;
        s_embedFailCount = 0;
        if (state.database.beginTransaction()) {
            bool abort = false;
            for (const auto& rec : selected) {
                auto meta = BuildMetadataMap(rec);
                EmbedExtraParams extra;
                BEXTData bext;
                IXMLData ixml;
                SteinbergData steinberg;
                ID3EmbedData id3;
                LISTInfoData listInfo;

                EmbedEngine::buildEmbedData(meta, rec.format, s_embedConfig, extra,
                                             bext, ixml, steinberg, id3, listInfo);

                MetadataEditPatch patch;
                patch.setDescription(!id3.comment.empty() ? id3.comment : rec.description);
                patch.setKeywords(rec.keywords);
                patch.setCategory(!steinberg.mediaCategoryPost.empty() ? steinberg.mediaCategoryPost : rec.category);

                if (state.database.applyMetadataPatch(rec.id, patch)) {
                    s_embedSuccessCount++;
                } else {
                    s_embedFailCount++;
                    abort = true;
                    break;
                }
            }
            if (abort || !state.database.commitTransaction()) {
                state.database.rollbackTransaction();
                s_embedSuccessCount = 0;
                s_embedFailCount = (int)selected.size();
                s_embedResultMsg = L(Module_EmbedFailRollback);
            } else {
                state.database.invalidateFacetQueryCache();
                std::snprintf(s_resultMsgBuf, sizeof(s_resultMsgBuf), L(Module_EmbedDoneFmt), s_embedSuccessCount);
                s_embedResultMsg = s_resultMsgBuf;
                if (s_embedFailCount > 0) {
                    appendMsgFmt(s_embedResultMsg, LStr::Module_FailSuffixFmt, s_embedFailCount);
                }
            }
        } else {
            s_embedResultMsg = L(Module_TxnBeginFail);
            s_embedFailCount = (int)selected.size();
        }
        RefreshSearchResults(state, false);
    }
    ImGui::EndDisabled();

    if (!s_embedResultMsg.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(s_embedFailCount > 0 ? ImVec4(1, 0.7f, 0, 1) : ImVec4(0.3f, 0.9f, 0.3f, 1),
                           "%s", s_embedResultMsg.c_str());
    }

    ImGui::End();
}

// 4) 命名脚本对话框

static std::string s_namingResultMsg;
static int s_namingSuccessCount = 0;
static int s_namingFailCount = 0;
static char s_namingPresetNameBuffer[128] = "";
static std::string s_namingPresetMsg;

static void RenderNamingDialog(AppState& state) {
    if (!state.showNamingDialog) return;
    SyncNamingPresetState(state);

    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(L(ModuleUI_NamingScriptTitle), &state.showNamingDialog)) {
        ImGui::End();
        return;
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) { state.showNamingDialog = false; }

    ImGui::TextWrapped(L(ModuleUI_NamingScriptIntro));
    ImGui::Separator();

    // 预设选择
    auto& presets = NamingScriptEngine::getPresets();
    ImGui::Text("%s", L(Module_PresetTemplate));
    ImGui::SetNextItemWidth(300);
    if (ImGui::BeginCombo("##NamingPreset",
        state.namingPresetIndex >= 0 && state.namingPresetIndex < (int)presets.size()
            ? presets[state.namingPresetIndex].name.c_str() : L(Wf_Custom))) {
        for (int i = 0; i < (int)presets.size(); i++) {
            bool sel = (state.namingPresetIndex == i);
            ImGui::PushID(i);
            if (ImGui::Selectable(presets[i].name.c_str(), sel)) {
                state.namingPresetIndex = i;
                snprintf(state.namingPatternBuffer, sizeof(state.namingPatternBuffer),
                         "%s", presets[i].pattern.c_str());
                PersistNamingDialogState(state);
            }
            if (ImGui::IsItemHovered() && !presets[i].description.empty()) {
                HarkenSearch::SetTooltip("%s", presets[i].description.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    // 模板编辑
    ImGui::Text("%s", L(Module_NamingTemplate));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##NamingPattern", state.namingPatternBuffer, sizeof(state.namingPatternBuffer))) {
        PersistNamingDialogState(state);
    }
    ImGui::TextDisabled("%s", L(Module_NamingSyntax));
    if (aura::ui::ActionButton(L(ModuleUI_SavePreset))) {
        const char* currentPresetName =
            (state.namingPresetIndex >= 0 && state.namingPresetIndex < (int)presets.size())
                ? presets[state.namingPresetIndex].name.c_str()
                : "";
        snprintf(s_namingPresetNameBuffer, sizeof(s_namingPresetNameBuffer), "%s", currentPresetName);
        s_namingPresetMsg.clear();
        ImGui::OpenPopup("##SaveNamingPreset");
    }
    ImGui::SameLine();
    const bool canDeletePreset =
        state.namingPresetIndex >= 0 &&
        state.namingPresetIndex < (int)presets.size() &&
        IsCustomNamingPresetName(state, presets[state.namingPresetIndex].name);
    ImGui::BeginDisabled(!canDeletePreset);
    if (aura::ui::ActionButton(L(ModuleUI_DeletePreset), ImVec2(0, 0), aura::ui::ButtonVariant::Danger)) {
        const std::string presetName = presets[state.namingPresetIndex].name;
        state.settings.namingCustomPresets.erase(
            std::remove_if(state.settings.namingCustomPresets.begin(),
                           state.settings.namingCustomPresets.end(),
                           [&](const NamingPreset& preset) { return preset.name == presetName; }),
            state.settings.namingCustomPresets.end());
        NamingScriptEngine::removePreset(presetName);
        state.settings.namingPresetName = "Default";
        const NamingPreset* defaultPreset = NamingScriptEngine::getPreset("Default");
        state.settings.namingPattern = defaultPreset ? defaultPreset->pattern : "<Description>_<Category>";
        snprintf(state.namingPatternBuffer, sizeof(state.namingPatternBuffer), "%s",
                 state.settings.namingPattern.c_str());
        const auto& refreshedPresets = NamingScriptEngine::getPresets();
        state.namingPresetIndex = FindNamingPresetIndexByName(refreshedPresets, state.settings.namingPresetName);
        SaveAppSettings(state);
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("##SaveNamingPreset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(L(ModuleUI_PresetName));
        ImGui::SetNextItemWidth(280.0f);
        ImGui::InputText("##NamingPresetName", s_namingPresetNameBuffer, sizeof(s_namingPresetNameBuffer));
        if (!s_namingPresetMsg.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "%s", s_namingPresetMsg.c_str());
        }
        if (aura::ui::ActionButton(L(ModuleUI_Save), ImVec2(100, 0), aura::ui::ButtonVariant::PrimarySearch)) {
            std::string presetName = s_namingPresetNameBuffer;
            const size_t firstNonSpace = presetName.find_first_not_of(' ');
            if (firstNonSpace == std::string::npos) {
                presetName.clear();
            } else {
                presetName = presetName.substr(firstNonSpace);
                const size_t lastNonSpace = presetName.find_last_not_of(' ');
                presetName = presetName.substr(0, lastNonSpace + 1);
            }
            if (presetName.empty()) {
                s_namingPresetMsg = L(Module_PresetNameRequired);
            } else if (!IsCustomNamingPresetName(state, presetName) &&
                       FindNamingPresetIndexByName(presets, presetName) >= 0) {
                s_namingPresetMsg = L(Module_PresetBuiltinLocked);
            } else {
                NamingPreset preset;
                preset.name = presetName;
                preset.pattern = state.namingPatternBuffer;
                preset.maxLength = GetActiveNamingMaxLength(state);
                auto it = std::find_if(
                    state.settings.namingCustomPresets.begin(),
                    state.settings.namingCustomPresets.end(),
                    [&](const NamingPreset& item) { return item.name == preset.name; });
                if (it != state.settings.namingCustomPresets.end()) {
                    *it = preset;
                } else {
                    state.settings.namingCustomPresets.push_back(preset);
                }
                NamingScriptEngine::addPreset(preset);
                state.settings.namingPresetName = preset.name;
                state.settings.namingPattern = preset.pattern;
                const auto& refreshedPresets = NamingScriptEngine::getPresets();
                state.namingPresetIndex = FindNamingPresetIndexByName(refreshedPresets, preset.name);
                SaveAppSettings(state);
                s_namingPresetMsg.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (aura::ui::ActionButton(L(ModuleUI_Cancel), ImVec2(100, 0), aura::ui::ButtonVariant::Quiet)) {
            s_namingPresetMsg.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 可用字段列表
    if (ImGui::TreeNode(L(ModuleUI_AvailableFields))) {
        auto fields = MetadataFieldManager::getStringFieldNames();
        int col = 0;
        for (const auto& f : fields) {
            if (col > 0) ImGui::SameLine(0, 10);
            char btnLabel[128];
            snprintf(btnLabel, sizeof(btnLabel), "<%s>", f.c_str());
            if (ImGui::SmallButton(btnLabel)) {
                // 插入到模板末尾
                std::string cur = state.namingPatternBuffer;
                cur += "<" + f + ">";
                snprintf(state.namingPatternBuffer, sizeof(state.namingPatternBuffer), "%s", cur.c_str());
                PersistNamingDialogState(state);
            }
            col++;
            if (col >= 5) col = 0;
        }
        ImGui::TreePop();
    }

    ImGui::Separator();

    // 预览
    auto selected = state.getSelectedRecords();
    ImGui::Text(L(Module_PreviewSelected), (int)selected.size());
    const int activeNamingMaxLength = GetActiveNamingMaxLength(state);

    ImGui::BeginChild("##NamingPreview", ImVec2(0, -60), true);
    if (ImGui::BeginTable("##NamingPreviewTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn(L(RenTable_OldName), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(L(RenTable_NewName), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        int limit = std::min((int)selected.size(), 50);
        for (int i = 0; i < limit; i++) {
            auto meta = BuildMetadataMap(selected[i]);
            std::string baseName = NamingScriptEngine::applyTemplate(
                state.namingPatternBuffer, meta, activeNamingMaxLength);
            // 保留原扩展名
            std::string newName = baseName + "." + selected[i].extension;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(selected[i].fileName.c_str());
            ImGui::TableNextColumn();
            if (newName != selected[i].fileName) {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%s", newName.c_str());
            } else {
                ImGui::TextUnformatted(newName.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::BeginDisabled(selected.empty());
    if (aura::ui::ActionButton(L(ModuleUI_RunRename), ImVec2(140, 30), aura::ui::ButtonVariant::PrimarySearch)) {
        s_namingSuccessCount = 0;
        s_namingFailCount = 0;

        for (const auto& rec : selected) {
            auto meta = BuildMetadataMap(rec);
            std::string baseName = NamingScriptEngine::applyTemplate(
                state.namingPatternBuffer, meta, activeNamingMaxLength);
            std::string newFileName = baseName + "." + rec.extension;

            if (newFileName == rec.fileName) continue; // 没变化，跳过

            try {
                fs::path oldP = Utf8ToPath(rec.filePath);
                if (ProbeUiPath(oldP) != Platform::FileSystem::PathStatus::Present) {
                    s_namingFailCount++;
                    continue;
                }

                fs::path newP = oldP.parent_path() / Utf8ToPath(newFileName);
                if (ProbeUiPath(newP) != Platform::FileSystem::PathStatus::Missing) {
                    s_namingFailCount++;
                    continue;
                }

                if (!RenameFileOnDisk(state, rec.id, rec.filePath, newFileName)) {
                    s_namingFailCount++;
                    continue;
                }
                s_namingSuccessCount++;
            } catch (...) {
                s_namingFailCount++;
            }
        }

        std::snprintf(s_resultMsgBuf, sizeof(s_resultMsgBuf), L(Module_RenameDoneFmt), s_namingSuccessCount);
        s_namingResultMsg = s_resultMsgBuf;
        if (s_namingFailCount > 0) {
            appendMsgFmt(s_namingResultMsg, LStr::Module_FailSuffixFmt, s_namingFailCount);
        }

        // 刷新界面
        state.folderHierarchy = state.database.getFolderHierarchy(state.settings.folderTreePathMode);
        RefreshSearchResults(state, true);
    }
    ImGui::EndDisabled();

    if (!s_namingResultMsg.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(s_namingFailCount > 0 ? ImVec4(1, 0.7f, 0, 1) : ImVec4(0.3f, 0.9f, 0.3f, 1),
                           "%s", s_namingResultMsg.c_str());
    }

    ImGui::End();
}

// 5) 传输引擎对话框

static TransferPrefs s_transferPrefs;
static bool s_transferPrefsInited = false;
static std::string s_transferResultMsg;
static int s_transferSuccessCount = 0;
static int s_transferFailCount = 0;
static int s_transferSkipCount = 0;

void RenderTransferDialogContent(AppState& state) {
    if (!state.showTransferDialog) return;

    if (!s_transferPrefsInited) {
        s_transferPrefs = TransferEngine::getDefaultPrefs();
        s_transferPrefsInited = true;
    }


    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) { state.showTransferDialog = false; }

    auto selected = state.getSelectedRecords();
    ImGui::Text(L(ModuleUI_SelFileCountFmt), (int)selected.size());
    ImGui::Separator();

    // 目标文件夹
    ImGui::Text("%s", L(Module_TargetFolder));
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##TransferDest", state.transferDestFolder, sizeof(state.transferDestFolder));
    ImGui::SameLine();
    if (aura::ui::ActionButton(L(ModuleUI_TransferBrowseBtn))) {
#ifdef _WIN32
        std::string folder = Platform::BrowseFolderDialogModern(
            HarkenSearch::UiHost::MainWindowOwner(), L(Folder_PickAudioFolder));
        if (!folder.empty()) {
            snprintf(state.transferDestFolder, sizeof(state.transferDestFolder), "%s", folder.c_str());
        }
#endif
    }

    ImGui::Separator();

    // 文件夹命名策略
    ImGui::Text("%s", L(Module_FolderStrategy));
    auto strategies = TransferEngine::getFolderStrategyList();
    ImGui::SetNextItemWidth(350);
    if (ImGui::BeginCombo("##FolderStrategy",
        state.transferFolderStrategy >= 0 && state.transferFolderStrategy < (int)strategies.size()
            ? strategies[state.transferFolderStrategy].second.c_str() : "")) {
        for (int i = 0; i < (int)strategies.size(); i++) {
            if (ImGui::Selectable(strategies[i].second.c_str(), state.transferFolderStrategy == i)) {
                state.transferFolderStrategy = i;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    // 格式转换
    ImGui::Text("%s", L(Module_FormatConvert));

    // 文件类型
    auto fileTypes = TransferEngine::getFileTypeChoices();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(L(Module_ComboFileFormat),
        state.transferFileType >= 0 && state.transferFileType < (int)fileTypes.size()
            ? fileTypes[state.transferFileType].first.c_str() : "")) {
        for (int i = 0; i < (int)fileTypes.size(); i++) {
            if (ImGui::Selectable(fileTypes[i].first.c_str(), state.transferFileType == i)) {
                state.transferFileType = i;
            }
        }
        ImGui::EndCombo();
    }

    // 采样率
    auto sampleRates = TransferEngine::getSampleRateChoices();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(L(Col_SampleRate),
        state.transferSampleRate >= 0 && state.transferSampleRate < (int)sampleRates.size()
            ? sampleRates[state.transferSampleRate].first.c_str() : "")) {
        for (int i = 0; i < (int)sampleRates.size(); i++) {
            if (ImGui::Selectable(sampleRates[i].first.c_str(), state.transferSampleRate == i)) {
                state.transferSampleRate = i;
            }
        }
        ImGui::EndCombo();
    }

    // 位深度
    auto bitDepths = TransferEngine::getBitDepthChoices();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(L(Info_BitDepth),
        state.transferBitDepth >= 0 && state.transferBitDepth < (int)bitDepths.size()
            ? bitDepths[state.transferBitDepth].first.c_str() : "")) {
        for (int i = 0; i < (int)bitDepths.size(); i++) {
            if (ImGui::Selectable(bitDepths[i].first.c_str(), state.transferBitDepth == i)) {
                state.transferBitDepth = i;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    // 选项
    ImGui::Checkbox(L(ModuleUI_TransferEmbedMeta), &s_transferPrefs.embedMetadataOnXfers);
    ImGui::Checkbox(L(ModuleUI_TransferMixMono), &s_transferPrefs.sumToMono);

    // 已存在文件处理
    auto existBehaviors = TransferEngine::getExistingBehaviorChoices();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(L(Export_ExistingFile),
        state.transferExistingBehavior >= 0 && state.transferExistingBehavior < (int)existBehaviors.size()
            ? existBehaviors[state.transferExistingBehavior].first.c_str() : "")) {
        for (int i = 0; i < (int)existBehaviors.size(); i++) {
            if (ImGui::Selectable(existBehaviors[i].first.c_str(), state.transferExistingBehavior == i)) {
                state.transferExistingBehavior = i;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();

    // 路径预览
    if (!selected.empty() && strlen(state.transferDestFolder) > 0) {
        ImGui::Text("%s", L(Module_PathPreview));
        auto meta = BuildMetadataMap(selected[0]);
        std::string error;
        FolderNamingStrategy strategy = state.transferFolderStrategy < (int)strategies.size()
            ? strategies[state.transferFolderStrategy].first
            : FolderNamingStrategy::SingleFolder;
        std::string destPath = TransferEngine::generateDestFolderPath(strategy, state.transferDestFolder, meta, error);
        if (!error.empty()) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), L(ModuleUI_ErrorFmt), error.c_str());
        } else {
            ImGui::TextWrapped(L(ModuleUI_TransferDestFmt), destPath.c_str(), selected[0].fileName.c_str());
        }
    }

    ImGui::Separator();

    // 执行传输
   ImGui::BeginDisabled(selected.empty() || strlen(state.transferDestFolder) == 0);
   if (aura::ui::ActionButton(L(ModuleUI_TransferStart), ImVec2(140, 35), aura::ui::ButtonVariant::PrimaryTransfer)) {
        if (Platform::FileSystem::IsNetworkPath(state.transferDestFolder)) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", L(Write_BlockedNetworkLibrary));
        } else {
        s_transferSuccessCount = 0;
        s_transferFailCount = 0;
        s_transferSkipCount = 0;

        FolderNamingStrategy strategy = state.transferFolderStrategy < (int)strategies.size()
            ? strategies[state.transferFolderStrategy].first
            : FolderNamingStrategy::SingleFolder;

        ExistingFileBehavior existBehavior = state.transferExistingBehavior < (int)existBehaviors.size()
            ? existBehaviors[state.transferExistingBehavior].second
            : ExistingFileBehavior::SkipExisting;

        for (const auto& rec : selected) {
            try {
                auto meta = BuildMetadataMap(rec);
                std::string error;
                std::string destFolder = TransferEngine::generateDestFolderPath(
                    strategy, state.transferDestFolder, meta, error);

                if (!error.empty()) { s_transferFailCount++; continue; }

                // 创建目标文件夹
                fs::path destDir = Utf8ToPath(destFolder);
                if (!Platform::FileSystem::EnsureDirectoryExists(PathToUtf8(destDir))) {
                    s_transferFailCount++;
                    continue;
                }

                fs::path srcPath = Utf8ToPath(rec.filePath);
                fs::path destPath = destDir / srcPath.filename();
                auto destStatus = ProbeUiPath(destPath);
                if (destStatus != Platform::FileSystem::PathStatus::Missing) {
                    if (destStatus == Platform::FileSystem::PathStatus::Error) {
                        s_transferFailCount++;
                        continue;
                    }

                    // 处理已存在文件
                    if (existBehavior == ExistingFileBehavior::SkipExisting) {
                        s_transferSkipCount++;
                        continue;
                    } else if (existBehavior == ExistingFileBehavior::AlwaysCreateNew) {
                        // 添加序号
                        int num = 1;
                        fs::path stem = destPath.stem();
                        fs::path ext = destPath.extension();
                        while (destStatus == Platform::FileSystem::PathStatus::Present) {
                            destPath = destDir / (PathToUtf8(stem) + "_" + std::to_string(num++) + PathToUtf8(ext));
                            destStatus = ProbeUiPath(destPath);
                            if (destStatus == Platform::FileSystem::PathStatus::Error) {
                                s_transferFailCount++;
                                break;
                            }
                        }
                        if (destStatus == Platform::FileSystem::PathStatus::Error) continue;
                    }
                    // ReplaceExisting / UpdateMetadata → 直接覆盖
                }

                // 复制文件 (当前版本直接复制，不做格式转换)
                if (!(Platform::FileSystem::CopyFile)(
                        PathToUtf8(srcPath), PathToUtf8(destPath), true)) {
                    s_transferFailCount++;
                    continue;
                }
                s_transferSuccessCount++;
            } catch (...) {
                s_transferFailCount++;
            }
        }

        std::snprintf(s_resultMsgBuf, sizeof(s_resultMsgBuf), L(Module_TransferDoneFmt), s_transferSuccessCount);
        s_transferResultMsg = s_resultMsgBuf;
       if (s_transferSkipCount > 0) appendMsgFmt(s_transferResultMsg, LStr::Module_SkipSuffixFmt, s_transferSkipCount);
       if (s_transferFailCount > 0) appendMsgFmt(s_transferResultMsg, LStr::Module_FailSuffixFmt, s_transferFailCount);
        }
    }
    ImGui::EndDisabled();
    if (Platform::FileSystem::IsNetworkPath(state.transferDestFolder)) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", L(Write_BlockedNetworkLibrary));
    }

    if (!s_transferResultMsg.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(s_transferFailCount > 0 ? ImVec4(1, 0.7f, 0, 1) : ImVec4(0.3f, 0.9f, 0.3f, 1),
                           "%s", s_transferResultMsg.c_str());
    }

}

// 6) 工作流引擎对话框

static Workflow s_currentWorkflow;
static bool s_workflowInited = false;
static bool s_showWorkflowPreview = false;
static std::vector<std::pair<std::string, std::string>> s_workflowPreviewChanges; // field -> newValue
static std::string s_workflowPreviewError;
static std::string s_workflowExecMsg;
static int s_workflowExecSuccess = 0;
static int s_workflowExecFail = 0;
// F-011: 工作流执行历史（最近 5 次，支持撤销）
static std::deque<WorkflowExecHistory> s_workflowExecHistory;
static constexpr int kMaxWorkflowHistory = 5;

void RenderWorkflowDialogContent(AppState& state) {
    if (!state.showWorkflowDialog) return;

    if (!s_workflowInited) {
        s_currentWorkflow.name = u8"新工作流";
        s_workflowInited = true;
    }


    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) { state.showWorkflowDialog = false; }

    // 工作流管理栏
    ImGui::Text("%s", L(Module_WorkflowName));
    ImGui::SameLine();
    char wfName[128];
    snprintf(wfName, sizeof(wfName), "%s", s_currentWorkflow.name.c_str());
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("##WFName", wfName, sizeof(wfName))) {
        s_currentWorkflow.name = wfName;
    }

    ImGui::SameLine();

    // 预设加载
    auto& workflows = WorkflowEngine::getWorkflows();
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("##WFLoad", L(Module_LoadPresetEllipsis))) {
        for (int i = 0; i < (int)workflows.size(); i++) {
            auto& wf = workflows[i];
            ImGui::PushID(i);
            if (ImGui::Selectable(wf.name.c_str())) {
                s_currentWorkflow = wf;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (aura::ui::ActionButton(L(ModuleUI_Save), ImVec2(0, 0), aura::ui::ButtonVariant::PrimarySearch)) {
        WorkflowEngine::addWorkflow(s_currentWorkflow);
    }

    ImGui::Separator();

    // 左侧: 节点类型列表; 右侧: 当前工作流节点
    float leftW = 200;
    ImGui::BeginChild("##WFNodeTypes", ImVec2(leftW, -110), true);
    ImGui::TextDisabled("%s", L(Module_AddNode));

    auto nodeTypes = WorkflowEngine::getAvailableNodeTypes();
    for (auto nt : nodeTypes) {
        const char* name = WorkflowEngine::getNodeTypeNameZh(nt);
        ImGui::PushID(static_cast<int>(nt));
        if (ImGui::Selectable(name, false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(0)) {
                s_currentWorkflow.addNode(nt);
            }
        }
        if (HoveredForHelp()) {
            HarkenSearch::SetTooltip("%s", WorkflowEngine::getNodeTypeDescription(nt));
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 右侧: 工作流管道
    ImGui::BeginChild("##WFPipeline", ImVec2(0, -110), true);
    ImGui::TextDisabled(L(Module_WorkflowPipe), (int)s_currentWorkflow.nodes.size());
    ImGui::Separator();

    int deleteIdx = -1;
    int moveUpIdx = -1;
    int moveDownIdx = -1;

    for (int i = 0; i < (int)s_currentWorkflow.nodes.size(); i++) {
        auto& node = s_currentWorkflow.nodes[i];
        const char* typeName = WorkflowEngine::getNodeTypeNameZh(node.type);

        ImGui::PushID(i);

        // 启用/禁用
        ImGui::Checkbox("##en", &node.enabled);
        ImGui::SameLine();

        // 折叠节点详情
        bool nodeOpen = ImGui::TreeNode("##node", u8"%d. %s", i + 1, typeName);

        // 上/下/删除按钮
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
        if (ImGui::SmallButton(u8"↑") && i > 0) moveUpIdx = i;
        ImGui::SameLine();
        if (ImGui::SmallButton(u8"↓") && i < (int)s_currentWorkflow.nodes.size() - 1) moveDownIdx = i;
        ImGui::SameLine();
        if (ImGui::SmallButton(u8"×")) deleteIdx = i;

        if (nodeOpen) {
            // 根据节点类型显示不同参数 (使用临时 buffer 避免 std::string 直接操作)
            auto editStr = [](const char* label, std::string& str) {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s", str.c_str());
                ImGui::SetNextItemWidth(250);
                if (ImGui::InputText(label, buf, sizeof(buf))) {
                    str = buf;
                }
            };

            switch (node.type) {
            case WorkflowNodeType::SetField:
                editStr(L(Wf_ETargetField), node.field);
                editStr(L(Wf_ESetValue), node.value);
                break;
            case WorkflowNodeType::CopyField:
                editStr(L(Wf_ESrcField), node.srcField);
                editStr((std::string(L(Wf_ETargetField)) + "##dest").c_str(), node.destField);
                {
                    int act = (int)node.copyAction;
                    ImGui::RadioButton(L(ModuleUI_OpOverwrite), &act, 0); ImGui::SameLine();
                    ImGui::RadioButton(L(ModuleUI_OpAppend), &act, 1); ImGui::SameLine();
                    ImGui::RadioButton(L(ModuleUI_OpPrepend), &act, 2);
                    node.copyAction = (CopyAction)act;
                }
                break;
            case WorkflowNodeType::FindAndReplace:
            case WorkflowNodeType::FindAndReplaceWord:
                editStr((std::string(L(Wf_ETargetField)) + "##fr").c_str(), node.field);
                editStr(L(Wf_EFind), node.findText);
                editStr(L(Wf_EReplace), node.replaceText);
                ImGui::Checkbox(L(ModuleUI_UseRegex), &node.useRegex);
                break;
            case WorkflowNodeType::ChangeCase:
                editStr(L(Col_Field), node.field);
                {
                    int ct = (int)node.caseType;
                    ImGui::RadioButton(L(ModuleUI_CaseUpper), &ct, 0); ImGui::SameLine();
                    ImGui::RadioButton(L(ModuleUI_CaseLower), &ct, 1); ImGui::SameLine();
                    ImGui::RadioButton(L(ModuleUI_CaseTitle), &ct, 2);
                    node.caseType = (CaseChangeType)ct;
                }
                break;
            case WorkflowNodeType::Number:
                ImGui::InputInt(L(Wf_StartNumber), &node.startNumber);
                ImGui::InputInt(L(Wf_Step), &node.stepNumber);
                ImGui::InputInt(L(Wf_PadDigits), &node.padDigits);
                break;
            default:
                ImGui::TextDisabled("%s", L(Module_ParamEditorWIP));
                break;
            }

            // 备注
            editStr(L(MetaLabel_Notes), node.userNote);

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    // 执行延迟操作
    if (deleteIdx >= 0) s_currentWorkflow.nodes.erase(s_currentWorkflow.nodes.begin() + deleteIdx);
    if (moveUpIdx > 0) s_currentWorkflow.moveNodeUp(moveUpIdx);
    if (moveDownIdx >= 0) s_currentWorkflow.moveNodeDown(moveDownIdx);

    ImGui::EndChild();

    // 底部: 执行按钮
    ImGui::Separator();
    auto selected = state.getSelectedRecords();
    ImGui::Text(L(Module_WillProcess), (int)selected.size());
    ImGui::SameLine();

    ImGui::BeginDisabled(selected.empty() || s_currentWorkflow.nodes.empty());
    if (aura::ui::ActionButton(L(ModuleUI_PreviewResult), ImVec2(120, 30))) {
        s_workflowPreviewChanges.clear();
        s_workflowPreviewError.clear();
        s_showWorkflowPreview = true;

        if (!selected.empty()) {
            auto meta = BuildMetadataMap(selected[0]);
            auto result = WorkflowEngine::executeWorkflow(s_currentWorkflow, meta, 0);
            if (result.success) {
                for (auto& [field, value] : result.changedFields) {
                    s_workflowPreviewChanges.push_back({field, value});
                }
            } else {
                s_workflowPreviewError = result.error;
            }
        }
    }
    ImGui::SameLine();
    if (aura::ui::ActionButton(L(ModuleUI_RunWorkflow), ImVec2(120, 30), aura::ui::ButtonVariant::PrimarySearch)) {
        s_workflowExecSuccess = 0;
        s_workflowExecFail = 0;

        std::vector<std::pair<int64_t, MetadataEditPatch>> pending;
        pending.reserve(selected.size());

        for (int idx = 0; idx < (int)selected.size(); idx++) {
            const auto& rec = selected[idx];
            auto meta = BuildMetadataMap(rec);
            auto result = WorkflowEngine::executeWorkflow(s_currentWorkflow, meta, idx);

            if (!result.success) {
                s_workflowExecFail++;
                continue;
            }
            if (result.changedFields.empty()) {
                s_workflowExecSuccess++;
                continue;
            }

            MetadataEditPatch patch;

            for (const auto& [field, value] : result.changedFields) {
                if (field == "Description") {
                    patch.setDescription(value);
                } else if (field == "Keywords") {
                    patch.setKeywords(value);
                } else if (field == "Category") {
                    patch.setCategory(value);
                } else if (field == "Rating") {
                    try {
                        patch.setRating(value.empty() ? 0 : std::stoi(value));
                    } catch (...) {
                        patch.setRating(0);
                    }
                } else {
                    // Generic field: map workflow field names to DB column names
                    // WorkflowEngine uses MetadataFields names (e.g., "SubCategory", "Library")
                    // DB uses snake_case (e.g., "subcategory", "library")
                    static const std::unordered_map<std::string, std::string> kFieldToCol = {
                        {"SubCategory","subcategory"}, {"Library","library"}, {"Designer","designer"},
                        {"Show","show_name"}, {"Notes","notes"}, {"Composer","composer"},
                        {"Publisher","publisher"}, {"Manufacturer","manufacturer"}, {"Mood","mood"},
                        {"FXName","fx_name"}, {"ShortID","short_id"}, {"LongID","long_id"},
                        {"Artist","artist"}, {"Title","title"}, {"Album","album"},
                        {"CDTitle","cd_title"}, {"CDDescription","cd_description"}, {"Volume","volume"},
                        {"Arranger","arranger"}, {"Conductor","conductor"}, {"Performer","performer"},
                        {"BPM","bpm"}, {"Key","musical_key"}, {"Tempo","tempo"}, {"Usage","usage"},
                        {"FeaturedInstrument","featured_instrument"}, {"Version","version_tag"},
                        {"Era","era"}, {"TrackYear","track_year"}, {"ISRC","isrc"}, {"ISWC","iswc"},
                        {"Location","location"}, {"Microphone","microphone"}, {"MicPerspective","mic_perspective"},
                        {"RecType","rec_type"}, {"RecMedium","rec_medium"}, {"Scene","scene"},
                        {"Take","take_num"}, {"Tape","tape"}, {"UserComments","user_comments"},
                        {"UserCategory","user_category"}, {"VendorCategory","vendor_category"},
                        {"OpenTier","open_tier"}, {"Source","source"}, {"CatID","cat_id"},
                        {"CategoryFull","category_full"}, {"URL","url"}, {"Copyright","copyright"},
                        {"ShowCategory","show_category"}, {"ShowSubCategory","show_subcategory"},
                        {"TrackColour","track_colour"},
                    };
                    auto cit = kFieldToCol.find(field);
                    if (cit != kFieldToCol.end()) {
                        patch.set(cit->second, value);
                    }
                }
            }

            if (patch.isEmpty()) {
                s_workflowExecSuccess++;
                continue;
            }

            pending.push_back({rec.id, patch});
        }

        if (!state.database.beginTransaction()) {
            s_workflowExecFail += (int)pending.size();
            s_workflowExecMsg = L(Module_TxnBeginFail);
        } else {
            // F-011: 记录执行前的旧值（用于撤销）
            WorkflowExecHistory history;
            history.workflowName = s_currentWorkflow.name;
            history.timestamp = (int64_t)std::time(nullptr);
            for (const auto& [fileId, patch] : pending) {
                WorkflowExecEntry entry;
                entry.fileId = fileId;
                // 查找该文件的当前值作为 oldValues
                for (const auto& rec : selected) {
                    if (rec.id == fileId) {
                        for (const auto& [col, newVal] : patch.fields) {
                            if (col == "description") entry.oldValues[col] = rec.description;
                            else if (col == "keywords") entry.oldValues[col] = rec.keywords;
                            else if (col == "category") entry.oldValues[col] = rec.category;
                            else entry.oldValues[col] = rec.getMeta(col);
                        }
                        break;
                    }
                }
                for (const auto& [col, newVal] : patch.fields) entry.newValues[col] = newVal;
                history.entries.push_back(std::move(entry));
            }

            BatchMetadataApplyResult batchRes = state.database.applyMetadataPatchMany(pending);
            if (state.database.commitTransaction()) {
                if (batchRes.success > 0) {
                    state.database.invalidateFacetQueryCache();
                    // 保存执行历史
                    s_workflowExecHistory.push_front(std::move(history));
                    if ((int)s_workflowExecHistory.size() > kMaxWorkflowHistory)
                        s_workflowExecHistory.pop_back();
                }
                s_workflowExecSuccess += batchRes.success;
                s_workflowExecFail += batchRes.failed;
            } else {
                state.database.rollbackTransaction();
                s_workflowExecFail += (int)pending.size();
            }
            std::snprintf(s_resultMsgBuf, sizeof(s_resultMsgBuf), L(Module_WfExecDoneFmt), s_workflowExecSuccess);
            s_workflowExecMsg = s_resultMsgBuf;
            if (s_workflowExecFail > 0) {
                appendMsgFmt(s_workflowExecMsg, LStr::Module_FailSuffixFmt, s_workflowExecFail);
            }
        }
        RefreshSearchResults(state, false);
    }
    ImGui::EndDisabled();

    // 显示执行结果
    if (!s_workflowExecMsg.empty()) {
        ImGui::TextColored(s_workflowExecFail > 0 ? ImVec4(1, 0.7f, 0, 1) : ImVec4(0.3f, 0.9f, 0.3f, 1),
                           "%s", s_workflowExecMsg.c_str());
    }

    // 预览结果弹窗
    if (s_showWorkflowPreview) {
        ImGui::OpenPopup(L(ModuleUI_WorkflowPreviewTitle));
        s_showWorkflowPreview = false;
    }
    if (ImGui::BeginPopupModal(L(ModuleUI_WorkflowPreviewTitle), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        if (!s_workflowPreviewError.empty()) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), L(ModuleUI_ErrorFmt), s_workflowPreviewError.c_str());
        } else if (s_workflowPreviewChanges.empty()) {
            ImGui::Text("%s", L(Module_NoChanges));
        } else {
            ImGui::Text("%s", L(Module_PreviewChanges));
            ImGui::Separator();
            if (ImGui::BeginTable("##WFPreview", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
                ImGui::TableSetupColumn(L(Col_Field), ImGuiTableColumnFlags_WidthFixed, 150);
                ImGui::TableSetupColumn(L(Col_NewValue), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (auto& [field, value] : s_workflowPreviewChanges) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(field.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1), "%s", value.c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::Separator();
        if (aura::ui::ActionButton(L(ModuleUI_Close), ImVec2(120, 0), aura::ui::ButtonVariant::Quiet)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

}

// 7) 高级主题编辑器 → 已迁移至 ThemeEditor.h/cpp

// 8) 字段管理器

static void RenderFieldManager(AppState& state) {
    if (!state.showFieldManager) return;

    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(L(ModuleUI_FieldManagerTitle), &state.showFieldManager)) {
        ImGui::End();
        return;
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) { state.showFieldManager = false; }

    // 模板选择
    ImGui::Text("%s", L(Module_DBTemplate));
    auto templateTypes = MetadataFieldManager::getAllTemplateTypes();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##FieldTemplate",
        state.fieldTemplateIndex >= 0 && state.fieldTemplateIndex < (int)templateTypes.size()
            ? MetadataFieldManager::getTemplateName(templateTypes[state.fieldTemplateIndex]).c_str()
            : "")) {
        for (int i = 0; i < (int)templateTypes.size(); i++) {
            std::string name = MetadataFieldManager::getTemplateName(templateTypes[i]);
            ImGui::PushID(i);
            if (ImGui::Selectable(name.c_str(), state.fieldTemplateIndex == i)) {
                state.fieldTemplateIndex = i;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // 模板信息
    if (state.fieldTemplateIndex >= 0 && state.fieldTemplateIndex < (int)templateTypes.size()) {
        auto* tmpl = MetadataFieldManager::getTemplate(templateTypes[state.fieldTemplateIndex]);
        if (tmpl) {
            ImGui::SameLine();
            ImGui::TextDisabled(L(ModuleUI_FieldCountFmt), (int)tmpl->fieldNames.size());
        }
    }

    ImGui::Separator();

    // 字段列表
    ImGui::Text(L(Module_AllFields), MetadataFieldManager::getFieldCount());

    ImGui::BeginChild("##FieldList", ImVec2(0, 0), true);
    if (ImGui::BeginTable("##FieldTable", 6,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable)) {

        ImGui::TableSetupColumn(L(Col_FieldName), ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn(L(Col_FieldNameZh), ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn(L(Col_Type), ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn(L(Col_MaxLength), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(L(Col_Group), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(L(Set_ColDescription), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto& fields = MetadataFieldManager::getAllFields();
        const char* typeNames[] = { "String", "Integer", "Real", "DateTime", "Blob" };

        // 如果选择了模板，只显示模板内的字段
        const DatabaseTemplateDef* activeTemplate = nullptr;
        if (state.fieldTemplateIndex >= 0 && state.fieldTemplateIndex < (int)templateTypes.size()) {
            activeTemplate = MetadataFieldManager::getTemplate(templateTypes[state.fieldTemplateIndex]);
        }

        for (const auto& f : fields) {
            // 模板过滤
            if (activeTemplate) {
                bool inTemplate = false;
                for (const auto& fn : activeTemplate->fieldNames) {
                    if (fn == f.name) { inTemplate = true; break; }
                }
                if (!inTemplate) continue;
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.displayName_zh.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(typeNames[(int)f.type]);
            ImGui::TableNextColumn();
            if (f.type == FieldType::String) ImGui::Text("%d", f.maxLength);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.group.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", f.description.c_str());
        }

        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::End();
}

// 统一渲染入口

void RenderModuleDialogs(AppState& state) {
    RenderEmbedDialog(state);
    RenderNamingDialog(state);
    if (ProductEdition::hasFeature("theme_editor")) {
        RenderThemeEditor(state);
    }
    RenderFieldManager(state);
}
