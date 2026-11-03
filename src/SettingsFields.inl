// Every row edits the shared draft used by both Common and its full category.
if(id=="pitch"||id=="pitchRange")draft.pitchRange=std::clamp(std::isfinite(draft.pitchRange)?draft.pitchRange:24.f,1.f,48.f);
if(id=="device") {
    const char* label=draft.audioOutputDeviceIndex<0?L(Set_SystemDefault):draft.audioOutputDeviceName.c_str();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12,8));
    if(ImGui::BeginCombo("##value",label)) {
        if(ImGui::Selectable(L(Set_SystemDefault),draft.audioOutputDeviceIndex<0)){draft.audioOutputDeviceIndex=-1;draft.audioOutputDeviceName.clear();editor.deviceIdentity.clear();editor.deviceEdited=true;}
        su::Observe("device-default");
        for(int i=0;i<(int)editor.devices.size();++i){
            const auto item=MakeOutputDeviceComboItem(i,editor.devices[i].name,editor.devices[i].isDefault,L(Set_OutputDeviceDefaultSuffix));
            const bool selected=draft.audioOutputDeviceIndex>=0 && (editor.deviceIdentity.empty()?draft.audioOutputDeviceIndex==i&&draft.audioOutputDeviceName==editor.devices[i].name:editor.deviceIdentity==editor.devices[i].deviceIdBlob);
            if(ImGui::Selectable(item.selectableId.c_str(),selected)){draft.audioOutputDeviceIndex=i;draft.audioOutputDeviceName=editor.devices[i].name;editor.deviceIdentity=editor.devices[i].deviceIdBlob;editor.deviceEdited=true;}
            su::Observe(("device-option-"+std::to_string(i)).c_str());
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Escape))ImGui::CloseCurrentPopup();
        ImGui::EndCombo();
    }
    ImGui::PopStyleVar();
}
else if(id=="theme") {
    int current=-1;for(int i=0;i<5;++i)if(su::StockThemes[i]==draft.theme)current=i;
    const float swatch=26.f,gap=8.f;
    for(int i=0;i<5;++i){
        if(i)ImGui::SameLine(0,gap);
        ImGui::PushID(i);const auto p=ImGui::GetCursorScreenPos();
        if(ImGui::InvisibleButton("##theme",ImVec2(swatch,swatch))){draft.theme=su::StockThemes[i];draft.useCustomColors=false;}
        su::Observe(("stock-"+std::to_string(i)).c_str());
        const auto theme=MakeBuiltinScheme(su::StockThemes[i]);auto* dl=ImGui::GetWindowDrawList();
        dl->AddRectFilled(p,ImVec2(p.x+swatch,p.y+swatch),ImGui::GetColorU32(theme.dialogBG),6);
        dl->AddRect(p,ImVec2(p.x+swatch,p.y+swatch),ImGui::GetColorU32(current==i?preview.controlAccent:preview.chromeBorder),6,0,current==i?2.f:1.f);
        if(current==i){const auto c=ImVec2(p.x+7,p.y+13);dl->AddLine(c,ImVec2(c.x+4,c.y+4),ImGui::GetColorU32(theme.interfaceText),1.6f);dl->AddLine(ImVec2(c.x+4,c.y+4),ImVec2(c.x+12,c.y-4),ImGui::GetColorU32(theme.interfaceText),1.6f);}
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",su::StockLabel(i));ImGui::PopID();
    }
    ImGui::TextDisabled("%s",su::StockLabel(current));
    if(draft.themeScheduleMode!=schedule::Mode::Off)ImGui::TextDisabled("%s",L(SetUX_UsedWhenScheduleIsOff));
}
else if(id=="automatic") {bool value=draft.themeScheduleMode!=schedule::Mode::Off;if(ImGui::Checkbox("##value",&value))draft.themeScheduleMode=value?schedule::Mode::Automatic:schedule::Mode::Off;}
else if(id=="day") {themedSelector(runtime.day);su::Observe("day-combo");}
else if(id=="night") {themedSelector(runtime.night);su::Observe("night-combo");}
else if(id=="themeEditor") {
#if AURA_BUILD_DIAGNOSTICS
    if(ProductEdition::hasFeature("theme_editor"))actionButton(L(Set_EditTheme),"theme-editor");
#endif
    actionButton(L(SetUX_CustomColors),"custom-colors");
    if(!draft.customThemeName.empty()){
        ImGui::TextWrapped("%s",draft.customThemeName.c_str());
        if(aura::ui::SmallActionButton(L(Main_ApplyCustomBtn))){draft.theme=AppTheme::Custom;draft.useCustomColors=false;}
    }
}
else if(id=="language") {int n=(int)draft.language;if(combo(n,{u8"简体中文","English",u8"日本語"}))draft.language=(AppLanguage)n;}
else if(id=="systemFont"||id=="listFont") {
    auto& value=id=="systemFont"?draft.readingFontSize:draft.listFontSize;
    int n=0;for(int i=0;i<kFontStepCount;++i)if(abs(value-kFontSteps[i])<abs(value-kFontSteps[n]))n=i;
    if(combo(n,{"10","11","12","13","14","16","18","20","22","24"}))value=kFontSteps[n];
}
else if(id=="rowHeight") {if(ImGui::SliderInt("##value",&draft.rowHeight,16,100,"%d px",ImGuiSliderFlags_AlwaysClamp))draft.listDensity=ListDensity::Custom;}
else if(id=="extension")ImGui::Checkbox("##value",&draft.showFileExtension);
else if(id=="waveMode") {if(combo(draft.waveformDisplayMode,{L(Set_ModeVolume2),L(Set_ModeLoudness2),L(Set_ModeSpectrum2)})&&draft.waveformDisplayMode==2)runtime.spectrum=true;}
else if(id=="waveStyle") {int n=draft.waveformStyle==3?1:0;if(combo(n,{L(Set_StyleRMS2),L(Set_StyleSolid)}))draft.waveformStyle=n==1?3:2;}
else if(id=="miniRms")ImGui::Checkbox("##value",&draft.miniWaveformRmsBodyStyle);
else if(id=="meterLayout") {int n=(int)draft.meterChannelLayout;if(combo(n,{"SMPTE / WAV","Film","Dolby Atmos Bed","Ambisonic AmbiX","1..N"}))draft.meterChannelLayout=(MeterChannelLayout)n;}
else if(id=="showLufs"||id=="showRms"||id=="showPeak") {
    const int count=(kEnableLufsUiAndDetection&&draft.showLoudnessMeter?1:0)+(draft.showRMSMeter?1:0)+(draft.showTruePeakMeter?1:0);
    bool& value=id=="showLufs"?draft.showLoudnessMeter:id=="showRms"?draft.showRMSMeter:draft.showTruePeakMeter;
    ImGui::BeginDisabled(count==1&&value);ImGui::Checkbox("##value",&value);su::Observe((id+"-control").c_str());ImGui::EndDisabled();
}
else if(id=="spectrumEnabled") {if(ImGui::Checkbox("##value",&runtime.spectrum)&&!runtime.spectrum&&draft.waveformDisplayMode==2)draft.waveformDisplayMode=0;}
else if(id=="fft") {
    const int sizes[]{512,1024,2048,4096,8192,16384};int n=0;for(int i=0;i<6;++i)if(sizes[i]==runtime.fft)n=i;
    ImGui::BeginDisabled(!runtime.spectrum);if(combo(n,{"512","1024","2048","4096","8192","16384"}))runtime.fft=sizes[n];su::Observe("fft-control");ImGui::EndDisabled();
}
else if(id=="spectrumBlend") {ImGui::BeginDisabled(!runtime.spectrum);ImGui::SliderFloat("##value",&runtime.blend,0,1,"%.2f",ImGuiSliderFlags_AlwaysClamp);ImGui::EndDisabled();}
else if(id=="spectrumSplit") {ImGui::BeginDisabled(!runtime.spectrum);ImGui::Checkbox("##value",&runtime.split);ImGui::EndDisabled();}
else if(id=="ripple")ImGui::Checkbox("##value",&draft.showClickRipple);
else if(id=="milliseconds")ImGui::Checkbox("##value",&draft.showMilliseconds);
else if(id=="frames")ImGui::Checkbox("##value",&draft.showFrameNumber);
else if(id=="frameRate") {ImGui::BeginDisabled(!draft.showFrameNumber);if(ImGui::InputFloat("##value",&draft.defaultFrameRate,0,0,"%.2f"))draft.defaultFrameRate=std::clamp(draft.defaultFrameRate,1.f,120.f);ImGui::EndDisabled();}
else if(id=="resetSize") actionButton(L(Set_ResetDefaultSize),"reset-size");
else if(id=="listening") {int n=(int)draft.listeningMode;if(combo(n,{L(Set_PreviewBrowse),L(Set_PreviewContinuous)}))draft.listeningMode=(ListeningMode)n;}
else if(id=="volume") {float value=draft.defaultVolume*100;if(ImGui::SliderFloat("##value",&value,0,120,"%.0f%%",ImGuiSliderFlags_AlwaysClamp))draft.defaultVolume=value/100;}
else if(id=="normalize")ImGui::Checkbox("##value",&draft.loudnessNormEnabled);
else if(id=="target") {
    ImGui::BeginDisabled(!draft.loudnessNormEnabled);
    const float values[]{-23,-16,-14};int preset=3;for(int i=0;i<3;++i)if(std::abs(draft.loudnessNormTargetLUFS-values[i])<.1f)preset=i;
    if(combo(preset,{"-23 LUFS (EBU R128)","-16 LUFS (Podcast)","-14 LUFS (Streaming)",L(Set_ThemeCustom)}))draft.loudnessNormTargetLUFS=preset<3?values[preset]:-18.f;
    if(preset==3){ImGui::SetNextItemWidth(controlW-2);ImGui::SliderFloat("##target",&draft.loudnessNormTargetLUFS,-40,-6,"%.1f LUFS",ImGuiSliderFlags_AlwaysClamp);}
    ImGui::EndDisabled();
}
else if(id=="fade")ImGui::Checkbox("##value",&draft.fadeInOutEnabled);
else if(id=="fadeMs") {ImGui::BeginDisabled(!draft.fadeInOutEnabled);ImGui::SliderInt("##value",&draft.fadeInOutMs,2,50,"%d ms",ImGuiSliderFlags_AlwaysClamp);ImGui::EndDisabled();}
else if(id=="period") {const int values[]{5,10,20,50};int n=1;for(int i=0;i<4;++i)if(values[i]==draft.audioPeriodSizeMs)n=i;if(combo(n,{"5 ms","10 ms","20 ms","50 ms"}))draft.audioPeriodSizeMs=values[n];ImGui::TextDisabled("%s",L(SetUX_AfterRestart));}
else if(id=="skipSilence")ImGui::Checkbox("##value",&draft.skipLeadingSilence);
else if(id=="silenceThreshold") {ImGui::BeginDisabled(!draft.skipLeadingSilence);ImGui::SliderFloat("##value",&draft.silenceThresholdDb,-80,-24,"%.1f dB",ImGuiSliderFlags_AlwaysClamp);ImGui::EndDisabled();}
else if(id=="pitch")ImGui::SliderFloat("##value",&draft.defaultPitch,-draft.pitchRange,draft.pitchRange,"%.1f st",ImGuiSliderFlags_AlwaysClamp);
else if(id=="pitchRange") {if(ImGui::SliderFloat("##value",&draft.pitchRange,1,48,"%.0f st",ImGuiSliderFlags_AlwaysClamp)){draft.pitchRange=std::clamp(std::isfinite(draft.pitchRange)?draft.pitchRange:24.f,1.f,48.f);draft.defaultPitch=std::clamp(draft.defaultPitch,-draft.pitchRange,draft.pitchRange);}}
else if(id=="monitor")combo(draft.monitorLayout,{L(Set_MonitorLayoutAuto),L(Set_MonitorLayoutStereo),L(Set_MonitorLayout51),L(Set_MonitorLayout71),L(Set_MonitorLayout712),L(Set_MonitorLayout714),L(Set_MonitorLayoutBinaural)});
else if(id=="downmix")combo(draft.downmixMode,{L(Set_DownmixAuto),L(Set_DownmixAlways),L(Set_DownmixNative)});
else if(id=="center") {ImGui::BeginDisabled(draft.downmixMode==2);ImGui::SliderFloat("##value",&draft.downmixCenterDb,-12,0,"%.1f dB",ImGuiSliderFlags_AlwaysClamp);ImGui::EndDisabled();}
else if(id=="surround") {ImGui::BeginDisabled(draft.downmixMode==2);ImGui::SliderFloat("##value",&draft.downmixSurroundDb,-12,0,"%.1f dB",ImGuiSliderFlags_AlwaysClamp);ImGui::EndDisabled();}
else if(id=="binaural")ImGui::Checkbox("##value",&draft.binauralMonitor);
else if(id=="deviceInfo") {ImGui::TextWrapped("%s",state.audioEngine.getCurrentOutputDeviceName().c_str());ImGui::TextWrapped("%s",L(Main_ChannelCapPendingNote));}
else if(id=="searchMode") {int n=draft.searchRealtime?0:1;if(combo(n,{L(Set_RealtimeSearch),L(Set_EnterSearch)}))draft.searchRealtime=n==0;}
else if(id=="suggest")combo(draft.suggestSourceMode,{L(Set_SuggestAll),L(Set_SuggestFilenameOnly),L(Set_SuggestKeywordOnly)});
else if(id=="renameTab")ImGui::Checkbox("##value",&draft.autoRenameTabBySearch);
else if(id=="dictionary")actionButton(L(SetUX_EditThesaurus),"dictionary");
else if(id=="perf") {
    if(combo(draft.memoryProfile,{L(Set_PerfEco),L(Set_PerfBalanced),L(Set_PerfHigh),L(Set_ThemeCustom)})){
        if(draft.memoryProfile==0){draft.fpsLimit=30;draft.thumbnailMaxResolution=256;draft.threadLimit=1;}
        if(draft.memoryProfile==1){draft.fpsLimit=60;draft.thumbnailMaxResolution=512;draft.threadLimit=0;}
        if(draft.memoryProfile==2){draft.fpsLimit=0;draft.thumbnailMaxResolution=512;draft.threadLimit=-1;}
    }
}
else if(id=="threads")ImGui::SliderInt("##value",&runtime.threads,0,32,L(SetUX_D0Auto),ImGuiSliderFlags_AlwaysClamp);
else if(id=="startupScan")ImGui::Checkbox("##value",&draft.autoReconcileOnStartup);
else if(id=="backfill")ImGui::Checkbox("##value",&draft.idleAssetBackfillEnabled);
else if(id=="idleMinutes") {ImGui::BeginDisabled(!draft.idleAssetBackfillEnabled);ImGui::SliderInt("##value",&draft.idleAssetBackfillDelayMinutes,1,240,L(SetUX_DMin),ImGuiSliderFlags_AlwaysClamp);ImGui::EndDisabled();}
else if(id=="database") {ImGui::TextWrapped("%s",state.currentDbPathUtf8.c_str());if(aura::ui::SmallActionButton(L(Set_OpenDBFolder)))Platform::OpenInFileManager(state.currentDbPathUtf8);}
else if(id=="maintenance")actionButton(L(SetUX_MaintenanceTools),"maintenance");
else if(id=="fps") {ImGui::BeginDisabled(draft.memoryProfile!=3);const int values[]{0,30,60,120,144};int n=0;for(int i=0;i<5;++i)if(draft.fpsLimit==values[i])n=i;if(combo(n,{L(Set_FPSNoLimit),"30 FPS","60 FPS","120 FPS","144 FPS"}))draft.fpsLimit=values[n];ImGui::EndDisabled();}
else if(id=="thumb") {ImGui::BeginDisabled(draft.memoryProfile!=3);int n=draft.thumbnailMaxResolution<=128?0:draft.thumbnailMaxResolution<=256?1:2;if(combo(n,{"128 px","256 px","512 px"}))draft.thumbnailMaxResolution=128<<n;ImGui::EndDisabled();}
else if(id=="remember")ImGui::Checkbox("##value",&draft.rememberLastFolder);
else if(id=="closeAction")combo(draft.closeButtonAction,{L(Set_CloseAsk),L(Set_CloseMinimize),L(Set_CloseDirect)});
else if(id=="guided")ImGui::Checkbox("##value",&draft.showGuidedMode);
else if(id=="confirm") {bool value=!draft.suppressConfirmDialogs;if(ImGui::Checkbox("##value",&value))draft.suppressConfirmDialogs=!value;}
else if(id=="hotkeyEnabled") {ImGui::Checkbox("##value",&draft.enableShowManagerHotkey);if(!draft.enableShowManagerHotkey)editor.recording=false;}
else if(id=="hotkey") {
    ImGui::BeginDisabled(!draft.enableShowManagerHotkey);
    if(aura::ui::ActionButton(editor.recording?L(Set_ListeningKeys):draft.showManagerHotkey.toString().c_str())){editor.recording=true;ImGui::ClearActiveID();}
    su::Observe("hotkey-record");
    if(editor.recording){
        if(ImGui::IsKeyPressed(ImGuiKey_Escape))editor.recording=false;
        else for(int k=ImGuiKey_NamedKey_BEGIN;k<ImGuiKey_NamedKey_END;++k){auto key=(ImGuiKey)k;
            if(key==ImGuiKey_LeftCtrl||key==ImGuiKey_RightCtrl||key==ImGuiKey_LeftShift||key==ImGuiKey_RightShift||key==ImGuiKey_LeftAlt||key==ImGuiKey_RightAlt||key==ImGuiKey_LeftSuper||key==ImGuiKey_RightSuper||key>=ImGuiKey_MouseLeft)continue;
            if(ImGui::IsKeyPressed(key)){auto& binding=draft.showManagerHotkey;binding.key=key;binding.ctrl=ImGui::GetIO().KeyCtrl;binding.shift=ImGui::GetIO().KeyShift;binding.alt=ImGui::GetIO().KeyAlt;editor.recording=false;break;}
        }
    }
    if(aura::ui::SmallActionButton(L(Set_ClearHotkey2)))draft.showManagerHotkey={ImGuiKey_None,false,false,false};
    ImGui::EndDisabled();
    if(!g_showManagerHotkeyRegisterOk&&state.settings.enableShowManagerHotkey)ImGui::TextWrapped("%s",L(Set_HotkeyConflict));
}
else if(id=="shortcuts")actionButton(L(SetUX_EditShortcuts),"shortcuts");
else if(id=="hideDrag")ImGui::Checkbox("##value",&draft.hideWindowOnDrag);
else if(id=="dragDelay") {ImGui::BeginDisabled(!draft.hideWindowOnDrag);ImGui::SliderInt("##value",&draft.hideWindowOnDragDelayMs,100,2000,"%d ms",ImGuiSliderFlags_AlwaysClamp);ImGui::EndDisabled();}
else if(id=="pauseDrag")ImGui::Checkbox("##value",&draft.pausePlaybackOnDragStart);
else if(id=="treePath") {int n=(int)draft.folderTreePathMode;if(combo(n,{L(Set_FolderTreePathAbsolute),L(Set_FolderTreePathPilot)}))draft.folderTreePathMode=(aura::folder_tree::PathMode)n;}
else if(id=="shell")actionButton(L(Set_ShellIntegration),"shell");
else if(id=="username")stringInput(draft.userName);
else if(id=="avatar")actionButton(L(SetUX_EditAvatar),"avatar");
else if(id=="exportPath") {stringInput(draft.defaultExportPath);if(aura::ui::SmallActionButton(L(Browse))){auto folder=OpenFolderDialog(g_hwnd);if(!folder.empty())draft.defaultExportPath=folder;}}
else if(id=="suffix")ImGui::Checkbox("##value",&draft.keepHarkenSearchOutputProcessingSuffix);
else if(id=="export")actionButton(L(Set_ExportSettings),"export");
else if(id=="import")actionButton(L(Set_ImportSettings),"import");
else if(id=="resetTheme") {if(aura::ui::ActionButton(L(Set_RestoreDefaultTheme))){draft.theme=AppTheme::Dark;draft.useCustomColors=false;draft.themeScheduleMode=schedule::Mode::Off;}}
else if(id=="resetAll")actionButton(L(Set_ResetAllDefaults),"reset-all");
else if(id=="logging")ImGui::Checkbox("##value",&draft.diagnosticMode);
else if(id=="overlay") {ImGui::BeginDisabled(!draft.diagnosticMode);ImGui::Checkbox("##value",&draft.showDiagnosticOverlay);ImGui::EndDisabled();}
else if(id=="diagnosticTools")actionButton(L(SetUX_LogTools),"diagnostics");
