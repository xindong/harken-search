// Actions such as library repair and avatar editing have their own persistence.
// Pending preferences are resolved explicitly before entering these tools.
if (!editor.closePrompt && !editor.action.empty()) {
    if (editor.action=="theme-editor" || editor.action=="day-editor" || editor.action=="night-editor" || editor.action=="shortcuts") {
        if (editor.action=="shortcuts") state.showShortcutsDialog=true;
        else {state.themeScheduleEditorPeriod=editor.action=="day-editor"?0:editor.action=="night-editor"?1:-1;state.showThemeEditor=true;}
        editor.action.clear();editor.open=false;ImGui::CloseCurrentPopup();
    } else if (!ImGui::IsPopupOpen("###SettingsTool")) ImGui::OpenPopup("###SettingsTool");
}
ImGui::SetNextWindowSize(ImVec2(std::min(780.f,maxW-40),std::min(640.f,maxH-40)),ImGuiCond_Appearing);
ImGui::SetNextWindowPos(viewport->GetCenter(),ImGuiCond_Appearing,ImVec2(.5f,.5f));
ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(24,24));
if (ImGui::BeginPopupModal("###SettingsTool",nullptr,ImGuiWindowFlags_NoTitleBar)) {
    const auto& cs=ThemeColors::current();
    auto mixVec=[](const ImVec4& a,const ImVec4& b,float t){return ImVec4(a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,a.w+(b.w-a.w)*t);};
    const char* title=editor.action=="avatar"?text(u8"编辑头像","Edit avatar",u8"アバターの編集"):editor.action=="dictionary"?su::PageLabel(su::Page::Search):editor.action=="maintenance"?L(Set_DBMaint2):editor.action=="diagnostics"?su::PageLabel(su::Page::Diagnostics):editor.action=="shell"?L(Set_ShellIntegration):editor.action=="reset-all"?L(Set_ResetAllDefaults):editor.action=="reset-size"?L(Set_ResetDefaultSize):editor.action=="custom-colors"?L(Set_EditTheme):su::PageLabel(su::Page::Backup);
    ImGui::PushFont(GetUIFontEmphasis(),settingsTitlePx);ImGui::TextUnformatted(title);ImGui::PopFont();
    ImGui::TextWrapped("%s",text(u8"这里的操作有独立的保存方式。","Actions here save separately.",u8"ここでの操作は個別に保存されます。"));
    ImGui::Separator();
    ImGui::BeginChild("SettingsToolBody",ImVec2(0,-54),false);
    if(editor.action=="avatar") {
#include "SettingsAvatarTools.inl"
    } else if(editor.action=="dictionary") {
#include "SettingsThesaurusTools.inl"
    } else if(editor.action=="maintenance") {
#include "SettingsMaintenanceTools.inl"
    } else if(editor.action=="diagnostics") {
#if AURA_BUILD_DIAGNOSTICS
#include "SettingsDiagnosticTools.inl"
        ImGui::Text(L(Main_DiskLogFmt),GetDiagnosticFileSizeBytes()/(1024.0*1024.0),GetDiagnosticFileMaxBytes()/(1024.0*1024.0));
        if(aura::ui::ActionButton(L(Main_CleanDiskDiagBtn)))ClearDiagnosticFile();
#if !AURA_INTERNAL_BETA
        if(ImGui::CollapsingHeader(text(u8"开发工具","Developer tools",u8"開発ツール"))) {
#include "SettingsDeveloperTools.inl"
        }
#endif
#endif
    } else if(editor.action=="shell") {
#ifdef _WIN32
        const bool registered=IsRegisteredAsDefault();
        ImGui::TextUnformatted(registered?L(Set_ShellIsDefault):L(Set_ShellNotDefault));
        if(aura::ui::ActionButton(registered?L(Set_ShellUnsetDefault):L(Set_ShellSetDefault))){if(registered)RemoveDefaultPlayer();else SetAsDefaultPlayer();}
#endif
    } else if(editor.action=="reset-all") {
        ImGui::TextWrapped("%s",text(u8"将所有偏好设置恢复为默认值，保留界面语言。素材库和音频文件不会删除。重置后仍需点击“应用更改”。","Reset preferences to defaults while keeping the interface language. Libraries and audio files are kept. Apply the changes afterwards.",u8"表示言語を保持して設定を初期化します。ライブラリと音声ファイルは削除されません。最後に適用してください。"));
        if(aura::ui::ActionButton(L(Set_ConfirmReset),ImVec2(0,0),aura::ui::ButtonVariant::Danger)){
            const auto language=draft.language;draft=AppSettings{};draft.language=language;editor.resetAll=true;
            editor.action="reset-ready";
        }
    } else if(editor.action=="reset-ready") {
        ImGui::TextWrapped("%s",text(u8"默认值已暂存。关闭此窗口后，点击“应用更改”保存。","Defaults are staged. Close this window, then Apply to save.",u8"初期値を準備しました。この画面を閉じ、適用して保存してください。"));
    } else if(editor.action=="reset-size") {
        ImGui::TextWrapped("%s",L(Set_ResetDefaultSizeHint));
        if(aura::ui::ActionButton(L(Set_ResetDefaultSize))) {
            state.settings.rowHeight=28;state.settings.readingFontSize=14;state.settings.fontSize=kChromeFontPx;state.settings.listFontSize=14;
            state.settings.miniWaveformRmsBodyStyle=false;g_miniWaveformRmsBodyStyle=false;state.settings.waveformStyle=DefaultMainWaveformStyle();
            state.settings.rowHeight=state.settings.getEffectiveRowHeight();state.settings.waveformPanelHeight=200;
            state.panelSidebar.dockedWidth=250;state.panelSidebar.floatW=300;state.panelSidebar.floatH=400;
            state.panelFileInfo.dockedWidth=300;state.panelFileInfo.dockSide=DockSide::Right;state.panelFileInfo.floatW=300;state.panelFileInfo.floatH=400;
            state.panelWaveform.dockedHeight=200;state.panelWaveform.floatW=300;state.panelWaveform.floatH=400;
            MarkSettingsDirty(state);editor.Reset(state);
        }
    } else if(editor.action=="export") {
        ImGui::TextWrapped("%s",L(Set_ExportSettingsHint));
        if(aura::ui::ActionButton(L(Set_ExportSettings))) {
            const std::string destination=GetExeDirUtf8()+"\\aris_settings_export.ini";
            if(TrySaveAppSettings(state)&&Platform::FileSystem::CopyFile(g_appSettingsPathUtf8,destination,true)){
                Platform::OpenInFileManager(destination);editor.error=destination;
            } else editor.error=text(u8"导出失败，请检查目标目录是否可写。","Export failed. Check the destination folder.",u8"出力できません。保存先フォルダーを確認してください。");
        }
    } else if(editor.action=="import") {
        ImGui::TextWrapped("%s",text(u8"导入将替换当前保存的配置。建议先导出一份备份。","Import replaces your saved configuration. Export a backup first.",u8"読み込むと保存済みの設定を置き換えます。先にバックアップしてください。"));
        if(aura::ui::ActionButton(L(Set_ImportSettings))) {
            const auto path=Platform::SettingsDialogs::OpenSettingsImportFileDialogUtf8(g_hwnd);
            if(!path.empty()) {
                std::string contents;
                if(!Platform::FileSystem::ReadFileText(path,contents)||contents.find('=')==std::string::npos)editor.error=text(u8"无法读取有效的设置文件。","Not a readable settings file.",u8"有効な設定ファイルを読み込めません。");
                else if(Platform::FileSystem::CopyFile(path,g_appSettingsPathUtf8,true)){LoadAppSettings(state);editor.Reset(state);}
                else editor.error=text(u8"导入失败，请检查设置目录。","Import failed. Check the settings folder.",u8"読み込みに失敗しました。設定フォルダーを確認してください。");
            }
        }
    } else if(editor.action=="custom-colors") {
        bool changed=false;
        changed|=ImGui::ColorEdit3(L(Set_BgColor),(float*)&state.settings.customBgColor,ImGuiColorEditFlags_NoInputs);
        changed|=ImGui::ColorEdit3(L(Set_TextColor),(float*)&state.settings.customTextColor,ImGuiColorEditFlags_NoInputs);
        changed|=ImGui::ColorEdit3(L(Set_AccentColor),(float*)&state.settings.customAccentColor,ImGuiColorEditFlags_NoInputs);
        changed|=ImGui::ColorEdit3(L(Set_WaveformColor),(float*)&state.settings.customWaveformColor,ImGuiColorEditFlags_NoInputs);
        changed|=ImGui::ColorEdit3(L(Set_VolumeColor),(float*)&state.settings.customVolumeColor,ImGuiColorEditFlags_NoInputs);
        if(aura::ui::SmallActionButton(L(Set_ResetColors))){
            const AppSettings defaults;
            state.settings.customBgColor=defaults.customBgColor;state.settings.customTextColor=defaults.customTextColor;
            state.settings.customAccentColor=defaults.customAccentColor;state.settings.customWaveformColor=defaults.customWaveformColor;state.settings.customVolumeColor=defaults.customVolumeColor;changed=true;
        }
        if(changed){state.settings.theme=AppTheme::Custom;state.settings.useCustomColors=true;schedule::OnManualThemeSelected(state);ThemeColors::deactivateCustomTheme();ThemeColors::markPendingUiChromeResync();MarkSettingsDirty(state);editor.Reset(state);}
    }
    if(!editor.error.empty())ImGui::TextWrapped("%s",editor.error.c_str());
    ImGui::EndChild();
    ImGui::Separator();
    if(aura::ui::ActionButton(L(Main_CloseBtn),ImVec2(100,38),aura::ui::ButtonVariant::Quiet)) {
        editor.action.clear();editor.error.clear();
        if(!editor.resetAll)editor.Reset(state);
        ImGui::CloseCurrentPopup();
    }
    su::Observe("tool-close");
    ImGui::EndPopup();
}
ImGui::PopStyleVar();
