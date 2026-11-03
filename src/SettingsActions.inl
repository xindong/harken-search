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
    const char* title=editor.action=="avatar"?L(SetUX_EditAvatarAction):editor.action=="dictionary"?su::PageLabel(su::Page::Search):editor.action=="maintenance"?L(Set_DBMaint2):editor.action=="diagnostics"?su::PageLabel(su::Page::Diagnostics):editor.action=="shell"?L(Set_ShellIntegration):editor.action=="reset-all"?L(Set_ResetAllDefaults):editor.action=="reset-size"?L(Set_ResetDefaultSize):editor.action=="custom-colors"?L(Set_EditTheme):su::PageLabel(su::Page::Backup);
    ImGui::PushFont(GetUIFontEmphasis(),settingsTitlePx);ImGui::TextUnformatted(title);ImGui::PopFont();
    ImGui::TextWrapped("%s",L(SetUX_ActionsHereSaveSeparately));
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
        if(ImGui::CollapsingHeader(L(SetUX_DeveloperTools))) {
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
        ImGui::TextWrapped("%s",L(SetUX_ResetPreferencesToDefaultsWhileKeepingTheInterfaceLanguageLibrariesAndAudioFilesAreKeptApplyTheChangesAfterwards));
        if(aura::ui::ActionButton(L(Set_ConfirmReset),ImVec2(0,0),aura::ui::ButtonVariant::Danger)){
            const auto language=draft.language;draft=AppSettings{};draft.language=language;editor.resetAll=true;
            const auto preset=GetReadableSpectrogramPreset();
            runtime.spectrum=true;runtime.fft=preset.fftSize;runtime.blend=preset.blend;runtime.split=preset.split;runtime.threads=0;
            editor.deviceIdentity.clear();editor.deviceEdited=true;
            editor.action="reset-ready";
        }
        su::Observe("reset-confirm");
    } else if(editor.action=="reset-ready") {
        ImGui::TextWrapped("%s",L(SetUX_DefaultsAreStagedCloseThisWindowThenApplyToSave));
    } else if(editor.action=="reset-size") {
        ImGui::TextWrapped("%s",L(Set_ResetDefaultSizeHint));
        if(aura::ui::ActionButton(L(Set_ResetDefaultSize))) {
            state.settings.rowHeight=28;state.settings.readingFontSize=kDefaultReadingFontPx;state.settings.fontSize=kChromeFontPx;state.settings.listFontSize=kDefaultListFontPx;
            state.settings.miniWaveformRmsBodyStyle=false;g_miniWaveformRmsBodyStyle=false;state.settings.waveformStyle=DefaultMainWaveformStyle();
            state.settings.rowHeight=state.settings.getEffectiveRowHeight();state.settings.waveformPanelHeight=200;
            state.panelSidebar.dockedWidth=250;state.panelSidebar.floatW=300;state.panelSidebar.floatH=400;
            state.panelFileInfo.dockedWidth=300;state.panelFileInfo.dockSide=DockSide::Right;state.panelFileInfo.floatW=300;state.panelFileInfo.floatH=400;
            state.panelWaveform.dockedHeight=200;state.panelWaveform.floatW=300;state.panelWaveform.floatH=400;
            MarkSettingsDirty(state);editor.Reset(state);
        }
        su::Observe("reset-size-confirm");
    } else if(editor.action=="export") {
        ImGui::TextWrapped("%s",L(Set_ExportSettingsHint));
        if(aura::ui::ActionButton(L(Set_ExportSettings))) {
#if defined(__APPLE__)
            editor.error.clear();
            const std::string destination=Platform::SettingsDialogs::SaveSettingsExportFileDialogUtf8(MainUiOwnerWindow());
#else
            const std::string destination=GetExeDirUtf8()+"\\aris_settings_export.ini";
#endif
            if(!destination.empty()) {
                if(TrySaveAppSettings(state)&&Platform::FileSystem::CopyFile(g_appSettingsPathUtf8,destination,true)){
                    Platform::OpenInFileManager(destination);editor.error=destination;
                } else editor.error=L(SetUX_ExportFailedCheckTheDestinationFolder);
            }
        }
    } else if(editor.action=="import") {
        ImGui::TextWrapped("%s",L(SetUX_ImportReplacesYourSavedConfigurationExportABackupFirst));
        if(aura::ui::ActionButton(L(Set_ImportSettings))) {
            const auto path=Platform::SettingsDialogs::OpenSettingsImportFileDialogUtf8(MainUiOwnerWindow());
            if(!path.empty()) {
                std::string contents;
                if(!Platform::FileSystem::ReadFileText(path,contents)||contents.find('=')==std::string::npos)editor.error=L(SetUX_NotAReadableSettingsFile);
                else if(Platform::FileSystem::CopyFile(path,g_appSettingsPathUtf8,true)){LoadAppSettings(state);editor.Reset(state);}
                else editor.error=L(SetUX_ImportFailedCheckTheSettingsFolder);
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
        if(changed){if(state.settings.theme==AppTheme::Custom)state.settings.theme=AppTheme::Dark;state.settings.useCustomColors=true;schedule::OnManualThemeSelected(state);ThemeColors::deactivateCustomTheme();ThemeColors::markPendingUiChromeResync();MarkSettingsDirty(state);editor.Reset(state);}
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
