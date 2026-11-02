// Included inside DrawDialogs so the existing native specialty operations keep
// their original owners. Draft state and the searchable catalog are separate.
{
    namespace su = harken::settings;
    namespace schedule = harken::theme_schedule;
    auto& editor=su::SettingsEditor();
    auto& draft=editor.app.Value();
    auto& runtime=editor.runtime.Value();
    const auto text=su::Text;
    auto apply=[&]() -> bool {
        editor.error.clear();
        if (!editor.ChangedCount()) return true;
        const auto oldSettings=su::CaptureSettings(state);
        const auto oldRuntime=su::CaptureRuntime(state);
        auto next=editor.app.MergeInto(editor.resetAll ? AppSettings{} : oldSettings);
        if (editor.resetAll) next.language=draft.language;
        const auto nextRuntime=editor.runtime.MergeInto(oldRuntime);
        // Device identity is checked again at Apply, since a USB device can
        // disappear or reorder while the settings dialog is open.
        if (next.audioOutputDeviceIndex!=oldSettings.audioOutputDeviceIndex || next.audioOutputDeviceName!=oldSettings.audioOutputDeviceName) {
            if (next.audioOutputDeviceIndex>=0) {
                auto devices=state.audioEngine.enumerateOutputDevices();
                int resolved=ResolveOutputDeviceSelection(devices,next.audioOutputDeviceIndex,next.audioOutputDeviceName);
                if(next.audioOutputDeviceIndex<(int)editor.devices.size() && !editor.devices[next.audioOutputDeviceIndex].deviceIdBlob.empty()) {
                    const auto& identity=editor.devices[next.audioOutputDeviceIndex].deviceIdBlob;
                    const auto found=std::find_if(devices.begin(),devices.end(),[&](const auto& device){return device.deviceIdBlob==identity;});
                    resolved=found==devices.end()?-2:static_cast<int>(found-devices.begin());
                }
                if (resolved<0) { editor.error=text(u8"输出设备已变化，请重新选择。","Output devices changed. Choose a device again.",u8"出力デバイスが変更されました。選択し直してください。"); editor.devices=std::move(devices); return false; }
                next.audioOutputDeviceIndex=resolved;
            }
            if (!state.audioEngine.switchOutputDevice(next.audioOutputDeviceIndex)) {
                editor.error=text(u8"无法切换输出设备，修改已保留。","Could not switch output device. Your changes are kept.",u8"出力デバイスを切り替えられません。変更は保持されています。"); return false;
            }
        }
        const bool deviceChanged=next.audioOutputDeviceIndex!=oldSettings.audioOutputDeviceIndex || next.audioOutputDeviceName!=oldSettings.audioOutputDeviceName;
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
            if (deviceChanged) state.audioEngine.switchOutputDevice(oldSettings.audioOutputDeviceIndex);
            editor.error=restored ? text(u8"保存失败，修改仍保留；请检查设置目录是否可写后重试。","Save failed. Changes are kept; check the settings folder and retry.",u8"保存できません。変更は保持されています。設定フォルダーを確認して再試行してください。")
                : text(u8"保存失败，日夜主题文件未能全部恢复。请检查设置目录后重试。","Save failed and some palette files could not be restored. Check the settings folder.",u8"保存に失敗し、一部のテーマファイルを復元できませんでした。設定フォルダーを確認してください。");
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
        editor.page=su::Page::Common; editor.query[0]=0; editor.focusId.clear();
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
        const auto winPos=ImGui::GetWindowPos(),winSize=ImGui::GetWindowSize();
        state.settings.settingsDialogWidth=winSize.x; state.settings.settingsDialogHeight=winSize.y;
        float navTextW=0;for(int i=0;i<=8;++i)navTextW=std::max(navTextW,ImGui::CalcTextSize(su::PageLabel((su::Page)i)).x);
        const float navW=std::min(winSize.x*.34f,std::max(212.f,navTextW+32.f));
        const float headerH=std::max(68.f,ImGui::GetFrameHeight()+30.f),footerH=editor.error.empty()?72.f:106.f;
        const float contentX=navW+28.f,contentW=winSize.x-contentX-28.f;
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
        su::observation.open=true; su::observation.page=(int)editor.page; su::observation.pending=editor.ChangedCount();
        su::observation.draftVolume=draft.defaultVolume; su::observation.draftRealtime=draft.searchRealtime; su::observation.draftTheme=(int)draft.theme;
        su::observation.prompt=editor.closePrompt;su::observation.bounds={winPos.x,winPos.y,winPos.x+winSize.x,winPos.y+winSize.y};
#endif
        ImGui::GetWindowDrawList()->AddRectFilled(winPos,ImVec2(winPos.x+navW,winPos.y+winSize.y),ImGui::GetColorU32(preview.dialogPanelBG));
        ImGui::SetCursorPos(ImVec2(24,22));
        ImGui::PushFont(GetUIFontEmphasis()); ImGui::SetWindowFontScale(1.6f); ImGui::TextUnformatted(L(Dlg_Settings)); ImGui::SetWindowFontScale(1.f); ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(12,headerH+8));
        ImGui::BeginChild("SettingsNavigation",ImVec2(navW-24,winSize.y-headerH-65),false,ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0,4));
        for (int i=0;i<=8;++i) {
#if !AURA_BUILD_DIAGNOSTICS
            if (i==8) continue;
#endif
            if (i==0 || i==5 || i==7) {
                if (i) ImGui::Dummy(ImVec2(0,12));
                ImGui::TextDisabled("%s",i==0?text(u8"日常使用","EVERYDAY",u8"日常使用"):i==5?text(u8"工作方式","WORKFLOW",u8"作業環境"):text(u8"维护","MAINTENANCE",u8"メンテナンス"));
            }
            const auto page=static_cast<su::Page>(i);
            ImGui::PushID(i);
            if (ImGui::Selectable(su::PageLabel(page),editor.page==page,0,ImVec2(0,34))) {editor.page=page;editor.query[0]=0;editor.focusId.clear();editor.highlightId.clear();}
            su::Observe(("nav-"+std::to_string(i)).c_str());
            ImGui::PopID();
        }
        ImGui::PopStyleVar();ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(24,winSize.y-36)); ImGui::TextDisabled("Harken Search");
        ImGui::SetCursorPos(ImVec2(contentX,18));
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(contentW-52);
        ImGui::InputTextWithHint("##settings-search",text(u8"搜索设置…  Ctrl+K","Search settings…  Ctrl+K",u8"設定を検索…  Ctrl+K"),editor.query,sizeof(editor.query)); su::Observe("search");
        ImGui::SameLine();
        // Borderless close glyph, including hovered and pressed states.
        const auto xOrigin=ImGui::GetCursorScreenPos();
        const float xSize=ImGui::GetFrameHeight();
        if (ImGui::InvisibleButton("##settings-close",ImVec2(xSize,xSize))) requestClose(); su::Observe("close");
        const float inset=xSize*.33f; const ImU32 xColor=ImGui::GetColorU32(ImGui::IsItemHovered()?ImGuiCol_Text:ImGuiCol_TextDisabled);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(xOrigin.x+inset,xOrigin.y+inset),ImVec2(xOrigin.x+xSize-inset,xOrigin.y+xSize-inset),xColor,1.7f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(xOrigin.x+xSize-inset,xOrigin.y+inset),ImVec2(xOrigin.x+inset,xOrigin.y+xSize-inset),xColor,1.7f);
        if (!editor.recording && !editor.hadChildPopup && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetCurrentContext()->OpenPopupStack.Size==ImGui::GetCurrentContext()->BeginPopupStack.Size && ImGui::IsKeyPressed(ImGuiKey_Escape)) requestClose();
        ImGui::SetCursorPos(ImVec2(contentX,headerH));
        ImGui::BeginChild("SettingsBody",ImVec2(contentW,winSize.y-headerH-footerH),false);
        if(editor.previousPage!=editor.page || editor.previousSearching!=(editor.query[0]!=0))ImGui::SetScrollY(0);
        editor.previousPage=editor.page;editor.previousSearching=editor.query[0]!=0;
        auto heading=[&](const char* title){ImGui::PushFont(GetUIFontEmphasis());ImGui::TextUnformatted(title);ImGui::PopFont();ImGui::Separator();};
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
        auto themedSelector=[&](int& value){
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12,8));
            if(ImGui::BeginCombo("##value",su::StockLabel(value))){
                for(int i=0;i<5;++i){if(ImGui::Selectable(su::StockLabel(i),value==i))value=i;su::Observe(("theme-option-"+std::to_string(i)).c_str());}
                if(ImGui::IsKeyPressed(ImGuiKey_Escape))ImGui::CloseCurrentPopup();
                ImGui::EndCombo();
            }
            ImGui::PopStyleVar();
        };
        auto drawEntry=[&](const su::Entry& entry){
            const auto& id=entry.id;
            if (id=="showLufs" && !kEnableLufsUiAndDetection) return;
            if ((id=="day" || id=="night") && draft.themeScheduleMode==schedule::Mode::Off) return;
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
            ImGui::TextDisabled("%s",text(u8"搜索结果","SEARCH RESULTS",u8"検索結果"));
            const auto results=su::Search(editor.query,AURA_BUILD_DIAGNOSTICS!=0);
            if(results.empty()) ImGui::TextWrapped("%s",text(u8"没有找到设置，试试“音量”“字体”或“快捷键”。","No settings found. Try volume, font, or shortcut.",u8"設定が見つかりません。音量、フォント、ショートカットなどを検索してください。"));
            for(const auto* entry:results){
                ImGui::PushID(entry->id.c_str());
                const std::string label=std::string(entry->Label((int)g_currentLanguage))+"  >  "+su::PageLabel(entry->page);
                if(ImGui::Selectable(label.c_str(),false,0,ImVec2(0,42))){editor.page=entry->page;editor.focusId=entry->id;editor.highlightId=entry->id;editor.highlightQuery=editor.query;editor.query[0]=0;}
                su::Observe(("result-"+entry->id).c_str());ImGui::PopID();
            }
            ImGui::PopStyleVar();
        }else{
            ImGui::TextDisabled("%s",text(u8"偏好设置","PREFERENCES",u8"環境設定"));
            ImGui::PushFont(GetUIFontEmphasis());ImGui::SetWindowFontScale(1.7f);ImGui::TextUnformatted(su::PageLabel(editor.page));ImGui::SetWindowFontScale(1.f);ImGui::PopFont();ImGui::Dummy(ImVec2(0,8));
            if(editor.page==su::Page::Common){
                for(const auto& group:std::vector<std::pair<const char*,std::vector<std::string>>>{
                    {text(u8"试听","Listening",u8"試聴"),{"device","volume","normalize"}},
                    {text(u8"界面","Interface",u8"画面"),{"theme","systemFont"}},
                    {text(u8"使用习惯","Your workflow",u8"使い方"),{"searchMode","remember","closeAction"}}}){
                    heading(group.first);for(const auto& id:group.second){const auto it=std::find_if(su::Catalog().begin(),su::Catalog().end(),[&](const auto& e){return e.id==id;});if(it!=su::Catalog().end())drawEntry(*it);}
                }
            }else{
                for(const auto& entry:su::Catalog())if(entry.page==editor.page && entry.group==0)drawEntry(entry);
                for(int group=1;group<=6;++group){
                    const bool has=std::any_of(su::Catalog().begin(),su::Catalog().end(),[&](const auto& e){return e.page==editor.page && e.group==group;});if(!has)continue;
                    if(!editor.focusId.empty()){const auto target=std::find_if(su::Catalog().begin(),su::Catalog().end(),[&](const auto& e){return e.id==editor.focusId;});if(target!=su::Catalog().end()&&target->group==group)ImGui::SetNextItemOpen(true);}
                    const char* label=group==1?text(u8"波形、频谱与计量","Waveform, spectrum & meters",u8"波形・スペクトル・メーター"):group==2?text(u8"显示细节","Display details",u8"表示の詳細"):group==3?text(u8"高级音频设置","Advanced audio",u8"オーディオの詳細"):group==4?text(u8"声道与监听","Channels & monitoring",u8"チャンネルとモニター"):group==5?text(u8"自定义性能","Custom performance",u8"カスタム性能"):text(u8"拖拽与系统集成","Drag & system integration",u8"ドラッグとシステム統合");
                    if(ImGui::CollapsingHeader(label))for(const auto& entry:su::Catalog())if(entry.page==editor.page && entry.group==group)drawEntry(entry);
                }
            }
        }
        ImGui::Dummy(ImVec2(0,20));ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(contentX,winSize.y-footerH));ImGui::Separator();
        const float buttonsW=editor.ChangedCount()?320.f:100.f;
        ImGui::SetCursorPos(ImVec2(contentX,winSize.y-50));
        if(!editor.error.empty()){ImGui::SetCursorPosY(winSize.y-footerH+10);ImGui::PushTextWrapPos(winSize.x-28);ImGui::TextWrapped("%s",editor.error.c_str());ImGui::PopTextWrapPos();}
        else if(editor.ChangedCount())ImGui::Text(text(u8"%d 项更改尚未应用","%d changes pending",u8"未適用の変更：%d"),(int)editor.ChangedCount());
        else ImGui::TextDisabled("%s",text(u8"没有待应用的更改","No pending changes",u8"未適用の変更はありません"));
        ImGui::SetCursorPos(ImVec2(winSize.x-buttonsW-28,winSize.y-56));
        if(editor.ChangedCount()){
            if(aura::ui::ActionButton(text(u8"撤销修改","Revert",u8"元に戻す"),ImVec2(140,38),aura::ui::ButtonVariant::Quiet))editor.Reset(state);su::Observe("revert");
            ImGui::SameLine();
            if(aura::ui::ActionButton(text(u8"应用更改","Apply",u8"適用"),ImVec2(168,38),aura::ui::ButtonVariant::PrimarySearch))apply();su::Observe("apply");
        }else{if(aura::ui::ActionButton(text(u8"完成","Done",u8"完了"),ImVec2(100,38)))requestClose();su::Observe("done");}
        if(editor.closePrompt)ImGui::OpenPopup("###SettingsUnsaved");
        ImGui::SetNextWindowSize(ImVec2(520,0),ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(24,24));
        if(ImGui::BeginPopupModal("###SettingsUnsaved",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar)){
            ImGui::TextUnformatted(text(u8"还有未应用的更改","Unapplied changes",u8"未適用の変更があります"));
            ImGui::TextWrapped("%s",editor.closeAfterPrompt?text(u8"关闭前，选择应用或放弃这些修改。","Apply or discard your changes before closing.",u8"閉じる前に変更を適用するか破棄してください。"):
                text(u8"这个入口有独立的保存方式。先处理当前设置修改，再继续。","This tool saves separately. Apply or discard the current changes first.",u8"このツールは別に保存します。先に現在の変更を処理してください。"));
            if(!editor.error.empty())ImGui::TextWrapped("%s",editor.error.c_str());
            if(aura::ui::ActionButton(text(u8"继续修改","Keep editing",u8"編集を続ける"))){editor.closePrompt=false;editor.action.clear();ImGui::CloseCurrentPopup();}su::Observe("keep-editing");
            if(aura::ui::ActionButton(editor.closeAfterPrompt?text(u8"放弃并关闭","Discard & close",u8"破棄して閉じる"):text(u8"放弃并继续","Discard & continue",u8"破棄して続ける"))){editor.Reset(state);editor.closePrompt=false;ImGui::CloseCurrentPopup();if(editor.closeAfterPrompt)editor.open=false;}su::Observe("discard");
            if(aura::ui::ActionButton(editor.closeAfterPrompt?text(u8"应用并关闭","Apply & close",u8"適用して閉じる"):text(u8"应用并继续","Apply & continue",u8"適用して続ける"),ImVec2(0,0),aura::ui::ButtonVariant::PrimarySearch))if(apply()){editor.closePrompt=false;ImGui::CloseCurrentPopup();if(editor.closeAfterPrompt)editor.open=false;}su::Observe("apply-close");
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        if(!editor.open){ImGui::CloseCurrentPopup();MarkSettingsDirty(state);}
#include "SettingsActions.inl"
        editor.hadChildPopup=ImGui::GetCurrentContext()->OpenPopupStack.Size>ImGui::GetCurrentContext()->BeginPopupStack.Size;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(5);ImGui::PopStyleColor(10);
}
