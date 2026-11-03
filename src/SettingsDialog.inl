// Included inside DrawDialogs so the existing native specialty operations keep
// their original owners. Draft state and the searchable catalog are separate.
{
    namespace su = harken::settings;
    namespace schedule = harken::theme_schedule;
    auto& editor=su::SettingsEditor();
    auto& draft=editor.app.Value();
    auto& runtime=editor.runtime.Value();
    auto apply=[&]() -> bool {
        editor.error.clear();
        if (!editor.ChangedCount()) return true;
        const auto oldSettings=su::CaptureSettings(state);
        const auto oldRuntime=su::CaptureRuntime(state);
        const float oldEngineVolume=state.audioEngine.getVolume();
        auto next=editor.app.MergeInto(oldSettings,editor.resetAll);
        next.rowHeight=next.getEffectiveRowHeight();
        const auto nextRuntime=editor.runtime.MergeInto(oldRuntime,editor.resetAll);
        // Device identity is checked again at Apply, since a USB device can
        // disappear or reorder while the settings dialog is open.
        const auto previousDeviceId=state.audioEngine.getCurrentOutputDeviceId();
        const int previousDeviceIndex=state.audioEngine.getCurrentOutputDeviceIndex();
        const auto previousDeviceName=state.audioEngine.getCurrentOutputDeviceName();
        auto restoreOutput=[&]() {
            const int index=ResolveOutputDeviceIdentity(editor.devices,previousDeviceIndex,previousDeviceName,previousDeviceId);
            const bool ok=index>=-1 && state.audioEngine.switchOutputDevice(index) && state.audioEngine.getCurrentOutputDeviceIndex()==index;
            state.settings.audioOutputDeviceIndex=state.audioEngine.getCurrentOutputDeviceIndex();
            state.settings.audioOutputDeviceName=state.settings.audioOutputDeviceIndex<0?std::string{}:state.audioEngine.getCurrentOutputDeviceName();
            return ok;
        };
        const bool deviceChanged=editor.deviceEdited || next.audioOutputDeviceIndex!=oldSettings.audioOutputDeviceIndex || next.audioOutputDeviceName!=oldSettings.audioOutputDeviceName;
        if (deviceChanged) {
            editor.devices=state.audioEngine.enumerateOutputDevices();
            const int resolved=ResolveOutputDeviceIdentity(editor.devices,next.audioOutputDeviceIndex,next.audioOutputDeviceName,editor.deviceIdentity);
            if (resolved<-1) { editor.error=L(SetUX_OutputDevicesChangedChooseADeviceAgain); return false; }
            next.audioOutputDeviceIndex=resolved;
            next.audioOutputDeviceName=resolved<0?std::string{}:editor.devices[resolved].name;
            if (!state.audioEngine.switchOutputDevice(resolved) || state.audioEngine.getCurrentOutputDeviceIndex()!=resolved) {
                editor.error=restoreOutput()?L(SetUX_CouldNotSwitchOutputDeviceYourChangesAreKept):L(SetUX_OutputRestoreFailed);
                return false;
            }
        }
        const auto previousDay=schedule::Palette(schedule::Period::Day);
        const auto previousNight=schedule::Palette(schedule::Period::Night);
        bool dayWritten=false,nightWritten=false;
        std::string paletteError;
        bool saved=true;
        if (nextRuntime.day!=oldRuntime.day && nextRuntime.day>=0) {
            saved=schedule::SavePalette(schedule::Period::Day,MakeBuiltinScheme(su::StockThemes[nextRuntime.day]),paletteError); dayWritten=saved;
        }
        if (saved && nextRuntime.night!=oldRuntime.night && nextRuntime.night>=0) {
            saved=schedule::SavePalette(schedule::Period::Night,MakeBuiltinScheme(su::StockThemes[nextRuntime.night]),paletteError); nightWritten=saved;
        }
        const auto nextMode=next.themeScheduleMode;
        schedule::SetMode(state,schedule::Mode::Off);
        next.themeScheduleMode=schedule::Mode::Off;
        state.settings=next;
        state.settings.syncFromListeningMode();
        su::WriteRuntime(state,nextRuntime);
        schedule::SetMode(state,nextMode);
        const bool volumeApplied=saved && !next.loudnessNormEnabled &&
            (editor.resetAll || next.defaultVolume!=oldSettings.defaultVolume || next.loudnessNormEnabled!=oldSettings.loudnessNormEnabled);
        if(volumeApplied)state.audioEngine.setVolume(next.defaultVolume);
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
        if (su::failSaveOnce) { su::failSaveOnce=false; saved=false; }
#endif
        if (saved) saved=TrySaveAppSettings(state);
        if (!saved) {
            std::string rollbackError;
            bool restored=true;
            if (dayWritten) restored &= schedule::SavePalette(schedule::Period::Day,previousDay,rollbackError);
            if (nightWritten) restored &= schedule::SavePalette(schedule::Period::Night,previousNight,rollbackError);
            schedule::SetMode(state,schedule::Mode::Off);
            state.settings=oldSettings; state.settings.themeScheduleMode=schedule::Mode::Off;
            schedule::SetMode(state,oldSettings.themeScheduleMode);
            su::WriteRuntime(state,oldRuntime);
            const bool outputRestored=!deviceChanged || restoreOutput();
            if(volumeApplied)state.audioEngine.setVolume(oldEngineVolume);
            editor.error=restored ? L(SetUX_SaveFailedChangesAreKeptCheckTheSettingsFolderAndRetry)
                : L(SetUX_SaveFailedAndSomePaletteFilesCouldNotBeRestoredCheckTheSettingsFolder);
            if (!outputRestored) editor.error += std::string(" ")+L(SetUX_OutputRestoreFailed);
            return false;
        }
        g_currentLanguage=state.settings.language;
        g_miniWaveformRmsBodyStyle=state.settings.miniWaveformRmsBodyStyle;
        g_hideWindowOnDrag=state.settings.hideWindowOnDrag;
        g_hideWindowOnDragDelayMs=static_cast<UINT>(state.settings.hideWindowOnDragDelayMs);
        state.audioEngine.setFadeInOutEnabled(state.settings.fadeInOutEnabled);
        state.audioEngine.setFadeInOutMs(state.settings.fadeInOutMs);
        state.database.setSettings(&state.settings);
        if (oldSettings.folderTreePathMode!=state.settings.folderTreePathMode) StartDeferredSidebarBootstrap(state,true,false);
        if (oldSettings.enableShowManagerHotkey!=state.settings.enableShowManagerHotkey ||
            oldSettings.showManagerHotkey.toString()!=state.settings.showManagerHotkey.toString()) ApplyShowManagerGlobalHotkey(state.settings);
        if (oldSettings.idleAssetBackfillEnabled!=state.settings.idleAssetBackfillEnabled || oldSettings.idleAssetBackfillDelayMinutes!=state.settings.idleAssetBackfillDelayMinutes) {
            state.lastInputTime=std::chrono::steady_clock::now(); ScheduleNextIdleAssetBackfillCheck(state,state.lastInputTime);
        }
        if (nextRuntime.spectrum && (oldRuntime.fft!=nextRuntime.fft || !oldRuntime.spectrum)) state.spectrogramNeedsUpdate=true;
        if (oldSettings.theme!=state.settings.theme || oldSettings.useCustomColors!=state.settings.useCustomColors) {
            ThemeColors::deactivateCustomTheme();
            if (state.settings.theme==AppTheme::Custom && !state.settings.customThemeName.empty()) {
                ThemeColors::loadAndApplyThemeFile(GetExeDirUtf8()+"\\theme_custom.json",nullptr,false,"SettingsApply",(int)oldSettings.theme,ThemeDiagName(oldSettings.theme),false);
            }
            ThemeColors::markPendingUiChromeResync();
        }
        DevTools_SetEnabled(state.settings.diagnosticMode); SetDiagnosticFileEnabled(state.settings.diagnosticMode);
        editor.Reset(state);
        return true;
    };
    auto requestClose=[&]() {
        if (editor.ChangedCount()) { editor.closePrompt=true; editor.closeAfterPrompt=true; }
        else { editor.open=false; ImGui::CloseCurrentPopup(); }
    };
    auto requestAction=[&](const char* action) {
        editor.action=action;
        if (editor.ChangedCount()) {editor.closePrompt=true;editor.closeAfterPrompt=false;}
    };
    if (state.showSettingsDialog) {
        editor.Reset(state); editor.open=true; editor.closePrompt=false; editor.action.clear();
        editor.page=su::Page::Appearance; editor.query[0]=0; editor.focusId.clear();
        editor.devices=state.audioEngine.enumerateOutputDevices();
        ImGui::OpenPopup("###HarkenSettings"); state.showSettingsDialog=false;
    }
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
    su::observation={}; su::observation.frame=ImGui::GetFrameCount();
#endif
    const auto* viewport=ImGui::GetMainViewport();
    const float maxW=std::max(640.f,viewport->WorkSize.x-40.f), maxH=std::max(480.f,viewport->WorkSize.y-40.f);
    const ImVec2 minimum(std::min(880.f,maxW),std::min(600.f,maxH));
    float savedW=state.settings.settingsDialogWidth, savedH=state.settings.settingsDialogHeight;
    if (savedW<=860 && savedH<=560) {savedW=1120;savedH=800;}
    ImGui::SetNextWindowSize(ImVec2(std::clamp(savedW,minimum.x,maxW),std::clamp(savedH,minimum.y,maxH)),ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(viewport->GetCenter(),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSizeConstraints(minimum,ImVec2(maxW,maxH));
    // A local theme preview changes only this settings surface. Runtime and
    // persisted theme stay untouched until Apply succeeds.
    const auto preview=(draft.theme!=AppTheme::Custom) ? MakeBuiltinScheme(draft.theme) : ThemeColors::current();
    ImGui::PushStyleColor(ImGuiCol_PopupBg,preview.dialogBG);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,preview.dialogBG);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,preview.dialogBG);
    ImGui::PushStyleColor(ImGuiCol_Text,preview.interfaceText);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,preview.metadataPanelText);
    ImGui::PushStyleColor(ImGuiCol_Border,preview.chromeBorder);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,preview.frameBgBase);
    ImGui::PushStyleColor(ImGuiCol_CheckMark,preview.controlAccent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,preview.controlAccent);
    ImGui::PushStyleColor(ImGuiCol_Header,ThemeColors::popupSelectionBG(preview));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(12,8));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(12,10));
    if (ImGui::BeginPopupModal("###HarkenSettings",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse)) {
        {
        ReadingTextScope reading;
        // Settings is a reading surface with a comfortable minimum. Keep the
        // saved global preference untouched and honor larger chosen sizes.
        const float settingsBodyPx=std::max(18.f,static_cast<float>(state.settings.readingFontSize));
        const float settingsTitlePx=settingsBodyPx*(4.f/3.f);
        ImGui::PushFont(GetUIFontRegular(),settingsBodyPx);
        const auto winPos=ImGui::GetWindowPos(),winSize=ImGui::GetWindowSize();
        state.settings.settingsDialogWidth=winSize.x; state.settings.settingsDialogHeight=winSize.y;
        float navTextW=0;for(int i=0;i<=8;++i)navTextW=std::max(navTextW,ImGui::CalcTextSize(su::PageLabel((su::Page)i)).x);
        const float navW=std::min(winSize.x*.34f,std::max(212.f,navTextW+60.f));
        const float contentX=navW+28.f,contentW=winSize.x-contentX-28.f;
        const float errorHeight=editor.error.empty()?0.f:ImGui::CalcTextSize(editor.error.c_str(),nullptr,false,contentW).y;
        const float headerH=std::max(68.f,ImGui::GetFrameHeight()+30.f),footerH=editor.error.empty()?72.f:std::max(106.f,errorHeight+82.f);
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
        su::observation.open=true; su::observation.page=(int)editor.page; su::observation.pending=editor.ChangedCount();
        su::observation.draftVolume=draft.defaultVolume; su::observation.draftRealtime=draft.searchRealtime; su::observation.draftTheme=(int)draft.theme;
        su::observation.prompt=editor.closePrompt;su::observation.bounds={winPos.x,winPos.y,winPos.x+winSize.x,winPos.y+winSize.y};
#endif
        ImGui::GetWindowDrawList()->AddRectFilled(winPos,ImVec2(winPos.x+navW,winPos.y+winSize.y),ImGui::GetColorU32(preview.dialogPanelBG));
        ImGui::SetCursorPos(ImVec2(24,22));
        ImGui::PushFont(GetUIFontEmphasis(),settingsTitlePx); ImGui::TextUnformatted(L(Dlg_Settings)); su::Observe("settings-title"); ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(12,headerH+8));
        ImGui::BeginChild("SettingsNavigation",ImVec2(navW-24,winSize.y-headerH-65),false,ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0,4));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign,ImVec2(0,.5f));
        // Keep native Selectable focus/activation, then paint the rounded row
        // and inset label inside exactly the same hit rectangle.
        ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0,0,0,0));
        const float navigationRowH=std::max(44.f,ImGui::GetTextLineHeight()+20.f);
        auto navTint=[&](float amount){
            return ImVec4(preview.dialogPanelBG.x+(preview.controlAccent.x-preview.dialogPanelBG.x)*amount,
                preview.dialogPanelBG.y+(preview.controlAccent.y-preview.dialogPanelBG.y)*amount,
                preview.dialogPanelBG.z+(preview.controlAccent.z-preview.dialogPanelBG.z)*amount,1.f);
        };
        for (int i=1;i<=8;++i) {
#if !AURA_BUILD_DIAGNOSTICS
            if (i==8) continue;
#endif
            const auto page=static_cast<su::Page>(i);
            ImGui::PushID(i);
            // Keep the painted row, text alignment and hit rectangle together.
            if (ImGui::Selectable(su::PageLabel(page),editor.page==page,ImGuiSelectableFlags_NoPadWithHalfSpacing,ImVec2(ImGui::GetContentRegionAvail().x,navigationRowH))) {editor.page=page;editor.query[0]=0;editor.focusId.clear();editor.highlightId.clear();}
            su::Observe(("nav-"+std::to_string(i)).c_str());
            const auto rowMin=ImGui::GetItemRectMin(),rowMax=ImGui::GetItemRectMax();
            const bool selected=editor.page==page,hovered=ImGui::IsItemHovered();
            const bool pressed=hovered&&ImGui::IsItemActive();
            auto* navDraw=ImGui::GetWindowDrawList();
            if(selected||hovered)navDraw->AddRectFilled(rowMin,rowMax,ImGui::GetColorU32(navTint(pressed?.20f:selected?.14f:.06f)),6.f);
            if(selected){
                const float indicatorH=std::min(20.f,ImGui::GetTextLineHeight());
                const float indicatorY=(rowMin.y+rowMax.y-indicatorH)*.5f;
                navDraw->AddRectFilled(ImVec2(rowMin.x+8,indicatorY),ImVec2(rowMin.x+11,indicatorY+indicatorH),ImGui::GetColorU32(preview.controlAccent),1.5f);
            }
            if(ImGui::IsItemFocused()&&ImGui::GetIO().NavVisible)
                navDraw->AddRect(rowMin,rowMax,ImGui::GetColorU32(preview.controlAccent),6.f,0,1.f);
            const ImVec2 labelPos(rowMin.x+24,rowMin.y+(navigationRowH-ImGui::GetTextLineHeight())*.5f);
            const ImVec4 labelClip(rowMin.x+24,rowMin.y,rowMax.x-12,rowMax.y);
            navDraw->AddText(selected?GetUIFontEmphasis():ImGui::GetFont(),ImGui::GetFontSize(),labelPos,ImGui::GetColorU32(preview.interfaceText),su::PageLabel(page),nullptr,0,&labelClip);
            ImGui::PopID();
        }
        ImGui::PopStyleColor(4);ImGui::PopStyleVar(2);ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(contentX,18));
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(contentW-52);
        ImGui::InputTextWithHint("##settings-search",L(SetUX_SearchSettingsCtrlK),editor.query,sizeof(editor.query)); su::Observe("search");
        if(ImGui::IsItemActive())editor.recording=false;
        ImGui::SameLine();
        // Borderless close glyph, including hovered and pressed states.
        const auto xOrigin=ImGui::GetCursorScreenPos();
        const float xSize=ImGui::GetFrameHeight();
        if (ImGui::InvisibleButton("##settings-close",ImVec2(xSize,xSize))) requestClose(); su::Observe("close");
        const float inset=xSize*.33f; const ImU32 xColor=ImGui::GetColorU32(ImGui::IsItemHovered()?ImGuiCol_Text:ImGuiCol_TextDisabled);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(xOrigin.x+inset,xOrigin.y+inset),ImVec2(xOrigin.x+xSize-inset,xOrigin.y+xSize-inset),xColor,1.7f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(xOrigin.x+xSize-inset,xOrigin.y+inset),ImVec2(xOrigin.x+inset,xOrigin.y+xSize-inset),xColor,1.7f);
        if (editor.page!=su::Page::Interaction || editor.query[0] || !draft.enableShowManagerHotkey) editor.recording=false;
        if (!editor.recording && !editor.hadChildPopup && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetCurrentContext()->OpenPopupStack.Size==ImGui::GetCurrentContext()->BeginPopupStack.Size && ImGui::IsKeyPressed(ImGuiKey_Escape)) requestClose();
        ImGui::SetCursorPos(ImVec2(contentX,headerH));
        // The modal already paints this surface. Painting its translucent
        // background again makes a dark rectangle during theme previews.
        ImGui::BeginChild("SettingsBody",ImVec2(contentW,winSize.y-headerH-footerH),false,ImGuiWindowFlags_NoBackground);
        if(editor.previousPage!=editor.page || editor.previousSearching!=(editor.query[0]!=0))ImGui::SetScrollY(0);
        editor.previousPage=editor.page;editor.previousSearching=editor.query[0]!=0;
        auto heading=[&](const char* title){ImGui::PushFont(GetUIFontEmphasis(),settingsBodyPx);ImGui::TextUnformatted(title);ImGui::PopFont();ImGui::Separator();};
        auto actionButton=[&](const char* label,const char* action){if(aura::ui::ActionButton(label))requestAction(action);};
        auto combo=[&](int& value,std::initializer_list<const char*> options){
            std::vector<const char*> list(options);bool changed=false;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12,8));
            if(ImGui::BeginCombo("##value",value>=0&&value<(int)list.size()?list[value]:"",ImGuiComboFlags_HeightLarge)){
                for(int i=0;i<(int)list.size();++i){ImGui::PushID(i);if(ImGui::Selectable(list[i],value==i)){value=i;changed=true;}su::Observe(("option-"+std::to_string(i)).c_str());ImGui::PopID();}
                if(ImGui::IsKeyPressed(ImGuiKey_Escape))ImGui::CloseCurrentPopup();
                ImGui::EndCombo();
            }
            ImGui::PopStyleVar();
            return changed;
        };
        auto stringInput=[&](std::string& value){char buffer[2048]{};std::snprintf(buffer,sizeof(buffer),"%s",value.c_str());if(ImGui::InputText("##value",buffer,sizeof(buffer)))value=buffer;};
        auto themedSelector=[&](int& value,const char* editAction=nullptr){
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12,8));
            if(ImGui::BeginCombo("##value",su::StockLabel(value))){
                for(int i=0;i<5;++i){if(ImGui::Selectable(su::StockLabel(i),value==i))value=i;su::Observe(("theme-option-"+std::to_string(i)).c_str());}
#if AURA_BUILD_DIAGNOSTICS
                if(editAction && ProductEdition::hasFeature("theme_editor")){
                    ImGui::Separator();
                    if(ImGui::MenuItem(L(ThemeSchedule_EditPalette)))requestAction(editAction);
                    su::Observe(editAction);
                }
#endif
                if(ImGui::IsKeyPressed(ImGuiKey_Escape))ImGui::CloseCurrentPopup();
                ImGui::EndCombo();
            }
            ImGui::PopStyleVar();
        };
        auto drawEntry=[&](const su::Entry& entry){
            const auto& id=entry.id;
            if (!su::Available(entry,AURA_BUILD_DIAGNOSTICS!=0,draft.themeScheduleMode!=schedule::Mode::Off,kEnableLufsUiAndDetection)) return;
            const float rowY=ImGui::GetCursorPosY(), rowX=ImGui::GetCursorPosX();
            ImGui::PushID(id.c_str());
            if(editor.focusId==id){ImGui::SetScrollHereY(.25f);editor.focusId.clear();}
            const float rowW=ImGui::GetContentRegionAvail().x;
            const float controlW=std::min(270.f,rowW*.44f),controlX=rowX+rowW-controlW;
            ImGui::AlignTextToFramePadding();
            const char* entryLabel=entry.Label((int)g_currentLanguage);
            aura::ui::BeginSettingsSearchHighlight(editor.highlightId==id?editor.highlightQuery.c_str():"",entryLabel,entry.keywords.c_str());
            ImGui::PushTextWrapPos(controlX-20);aura::ui::DrawSettingsSearchableLabel(entryLabel);ImGui::PopTextWrapPos();
            aura::ui::EndSettingsSearchHighlight();
            const float labelBottom=ImGui::GetCursorPosY();
            ImGui::SetCursorPos(ImVec2(controlX,rowY)); ImGui::SetNextItemWidth(controlW-2);
            ImGui::BeginGroup();ImGui::PushTextWrapPos(rowX+rowW-2);
#include "SettingsFields.inl"
            ImGui::PopTextWrapPos();ImGui::EndGroup();
            su::Observe(id.c_str());
            const float bottom=std::max(labelBottom,ImGui::GetCursorPosY());
            ImGui::SetCursorPos(ImVec2(rowX,std::max(rowY+42.f,bottom)));ImGui::Separator();
            ImGui::PopID();
        };
        const bool searching=editor.query[0]!=0;
        if(searching){
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0,10));
            ImGui::TextDisabled("%s",L(SetUX_SEARCHRESULTS));
            const auto results=su::Search(editor.query,AURA_BUILD_DIAGNOSTICS!=0,draft.themeScheduleMode!=schedule::Mode::Off,kEnableLufsUiAndDetection);
            if(results.empty()) ImGui::TextWrapped("%s",L(SetUX_NoSettingsFoundTryVolumeFontOrShortcut));
            for(const auto* entry:results){
                ImGui::PushID(entry->id.c_str());
                const std::string label=std::string(entry->Label((int)g_currentLanguage))+"  >  "+su::PageLabel(entry->page);
                if(ImGui::Selectable(label.c_str(),false,0,ImVec2(0,42))){editor.page=entry->page;editor.focusId=entry->id;editor.highlightId=entry->id;editor.highlightQuery=editor.query;editor.query[0]=0;}
                su::Observe(("result-"+entry->id).c_str());ImGui::PopID();
            }
            ImGui::PopStyleVar();
        }else{
            ImGui::TextDisabled("%s",L(SetUX_PREFERENCES));
            ImGui::PushFont(GetUIFontEmphasis(),settingsTitlePx);ImGui::TextUnformatted(su::PageLabel(editor.page));su::Observe("page-title");ImGui::PopFont();ImGui::Dummy(ImVec2(0,8));
            if(editor.page==su::Page::Common){
                for(const auto& group:std::vector<std::pair<const char*,std::vector<std::string>>>{
                    {L(SetUX_Listening),{"device","volume","normalize"}},
                    {L(SetUX_Interface),{"theme","systemFont"}},
                    {L(SetUX_YourWorkflow),{"searchMode","remember","closeAction"}}}){
                    heading(group.first);for(const auto& id:group.second){const auto it=std::find_if(su::Catalog().begin(),su::Catalog().end(),[&](const auto& e){return e.id==id;});if(it!=su::Catalog().end())drawEntry(*it);}
                }
            }else{
                for(const auto& entry:su::Catalog())if(entry.page==editor.page && entry.group==0)drawEntry(entry);
                for(int group=1;group<=6;++group){
                    const bool has=std::any_of(su::Catalog().begin(),su::Catalog().end(),[&](const auto& e){return e.page==editor.page && e.group==group;});if(!has)continue;
                    if(!editor.focusId.empty()){const auto target=std::find_if(su::Catalog().begin(),su::Catalog().end(),[&](const auto& e){return e.id==editor.focusId;});if(target!=su::Catalog().end()&&target->group==group)ImGui::SetNextItemOpen(true);}
                    const char* label=group==1?L(SetUX_WaveformSpectrumMeters):group==2?L(SetUX_DisplayDetails):group==3?L(SetUX_AdvancedAudio):group==4?L(SetUX_ChannelsMonitoring):group==5?L(SetUX_CustomPerformance):L(SetUX_DragSystemIntegration);
                    if(ImGui::CollapsingHeader(label))for(const auto& entry:su::Catalog())if(entry.page==editor.page && entry.group==group)drawEntry(entry);
                }
            }
        }
        ImGui::Dummy(ImVec2(0,20));ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(contentX,winSize.y-footerH));ImGui::Separator();
        const float buttonsW=editor.ChangedCount()?320.f:100.f;
        ImGui::SetCursorPos(ImVec2(contentX,winSize.y-50));
        if(!editor.error.empty()){ImGui::SetCursorPosY(winSize.y-footerH+10);ImGui::PushTextWrapPos(winSize.x-28);ImGui::TextWrapped("%s",editor.error.c_str());su::Observe("save-error");ImGui::PopTextWrapPos();}
        else if(editor.ChangedCount())ImGui::Text(L(SetUX_DChangesPending),(int)editor.ChangedCount());
        else ImGui::TextDisabled("%s",L(SetUX_NoPendingChanges));
        ImGui::SetCursorPos(ImVec2(winSize.x-buttonsW-28,winSize.y-56));
        if(editor.ChangedCount()){
            if(aura::ui::ActionButton(L(SetUX_Revert),ImVec2(140,38),aura::ui::ButtonVariant::Quiet))editor.Reset(state);su::Observe("revert");
            ImGui::SameLine();
            if(aura::ui::ActionButton(L(SetUX_Apply),ImVec2(168,38),aura::ui::ButtonVariant::PrimarySearch))apply();su::Observe("apply");
        }else{if(aura::ui::ActionButton(L(Done),ImVec2(100,38)))requestClose();su::Observe("done");}
        if(editor.closePrompt)ImGui::OpenPopup("###SettingsUnsaved");
        ImGui::SetNextWindowSize(ImVec2(520,0),ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(24,24));
        if(ImGui::BeginPopupModal("###SettingsUnsaved",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar)){
            ImGui::TextUnformatted(L(SetUX_UnappliedChanges));
            ImGui::TextWrapped("%s",editor.closeAfterPrompt?L(SetUX_ApplyOrDiscardYourChangesBeforeClosing):
                L(SetUX_ThisToolSavesSeparatelyApplyOrDiscardTheCurrentChangesFirst));
            if(!editor.error.empty())ImGui::TextWrapped("%s",editor.error.c_str());
            if(aura::ui::ActionButton(L(SetUX_KeepEditing))){editor.closePrompt=false;editor.action.clear();ImGui::CloseCurrentPopup();}su::Observe("keep-editing");
            if(aura::ui::ActionButton(editor.closeAfterPrompt?L(SetUX_DiscardClose):L(SetUX_DiscardContinue))){editor.Reset(state);editor.closePrompt=false;ImGui::CloseCurrentPopup();if(editor.closeAfterPrompt)editor.open=false;}su::Observe("discard");
            if(aura::ui::ActionButton(editor.closeAfterPrompt?L(SetUX_ApplyClose):L(SetUX_ApplyContinue),ImVec2(0,0),aura::ui::ButtonVariant::PrimarySearch))if(apply()){editor.closePrompt=false;ImGui::CloseCurrentPopup();if(editor.closeAfterPrompt)editor.open=false;}su::Observe("apply-close");
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        if(!editor.open){ImGui::CloseCurrentPopup();MarkSettingsDirty(state);}
#include "SettingsActions.inl"
        editor.hadChildPopup=ImGui::GetCurrentContext()->OpenPopupStack.Size>ImGui::GetCurrentContext()->BeginPopupStack.Size;
        ImGui::PopFont();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(5);ImGui::PopStyleColor(10);
}
