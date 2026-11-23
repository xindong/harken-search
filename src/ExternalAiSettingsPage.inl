// External AI authorization page. The document is saved immediately, because
// the file this page writes is the same file the connector reads: keeping a
// second copy in the preferences draft could drift from what the client sees.
{
    auto& page=su::ExternalAiPage();
    const std::string databasePath=state.currentDbPathUtf8;
    const std::string settingsDirectory=[&]() {
        const auto separator=g_appSettingsPathUtf8.find_last_of("\\/");
        return separator==std::string::npos ? std::string{} : g_appSettingsPathUtf8.substr(0,separator);
    }();
    auto saveDocument=[&]() -> bool {
        if (!page.loaded || page.documentPath.empty()) return false;
        std::string error;
        if (!HarkenSearch::SaveExternalAiSettings(page.documentPath,page.settings,error)) {
            page.error=error.empty() ? L(ExtAi_SaveFailed) : error;
            return false;
        }
        page.dirty=false; page.error.clear();
        if (page.settings.enabled &&
            page.settings.capability(HarkenSearch::ExternalAiCapability::EditPlaylist)
                == HarkenSearch::ExternalAiAccess::Editable &&
            !page.libraryId.empty()) {
            HarkenSearch::StartExternalAiBridgeServer(page.libraryId);
        } else {
            HarkenSearch::StopExternalAiBridgeServer();
        }
        page.status=L(ExtAi_Saved); page.statusUntil=ImGui::GetTime()+4.0;
        return true;
    };
    auto loadLibrary=[&](const std::string& library) {
        page=su::ExternalAiPageState{};
        page.databasePath=library;
        page.loaded=true;
        if (library.empty()) return;
        page.libraryId=HarkenSearch::ExternalAiLibraryIdForPath(library);
        page.documentPath=HarkenSearch::ExternalAiSettingsPath(settingsDirectory,page.libraryId);
        if (page.libraryId.empty() || page.documentPath.empty()) { page.loaded=false; page.error=L(ExtAi_Unavailable); return; }
        std::string error;
        HarkenSearch::ExternalAiSettings loaded;
        if (!HarkenSearch::LoadExternalAiSettings(page.documentPath,page.libraryId,loaded,error)) {
            page.settings.libraryId=page.libraryId;
            page.error=error.empty() ? L(ExtAi_LoadFailed) : error;
        } else {
            page.settings=loaded;
        }
    };
    if (!page.loaded || page.databasePath!=databasePath) {
        // Switching libraries keeps the previous library's pending edits in its
        // own file, then reads the new library's document.
        if (page.loaded && page.dirty) saveDocument();
        loadLibrary(databasePath);
    }
    if (editor.focusId=="externalAi") { ImGui::SetScrollHereY(.25f); editor.focusId.clear(); }
    su::Observe("ext-ai-page");
    if (databasePath.empty()) {
        ImGui::TextWrapped("%s",L(ExtAi_NoLibrary));
    } else {
        // 版面按 2026-11-20 的模块化方案：连接状态横条 → 可操作目录（带外框）→
        // 分项权限（查找 / 文件名与元数据两组无框行）→ 连接客户端 → 底部保存。
        const std::string libraryName=[&]() {
            std::string path=databasePath;
            const auto first=path.find_last_of("\\/");
            if (first!=std::string::npos) path.erase(first);
            const auto second=path.find_last_of("\\/");
            if (second!=std::string::npos) path=path.substr(second+1);
            return path.empty()?databasePath:path;
        }();
        const auto& theme=ThemeColors::current();
        const auto databaseRoots=state.database.getLibraryRoots();
        const auto displayRootPath=[&](const std::string& root) {
            if (state.settings.folderTreePathMode==aura::folder_tree::PathMode::Absolute) {
                return aura::folder_tree::NormalizeFolderPathPreserved(root);
            }
            const auto layers=aura::folder_tree::BuildFolderTreeLayers(root,databaseRoots,state.settings.folderTreePathMode);
            std::string displayed;
            for (const auto& layer : layers) {
                if (!displayed.empty()) displayed += " / ";
                displayed += layer.name;
            }
            return displayed.empty()?root:displayed;
        };
        const bool hasConnector=Platform::FileSystem::Exists(GetExeDirUtf8()+"\\HarkenExternalAiMcp.exe");
        HarkenSearch::ExternalAiLinkTarget linkTarget;
        linkTarget.executablePath=GetExeDirUtf8()+"\\HarkenExternalAiMcp.exe";
        linkTarget.libraryId=page.libraryId;
        linkTarget.databasePath=databasePath;
        const auto linkRoot=HarkenSearch::ExternalAiLinkRootFor(page.settings.roots);
        linkTarget.rootPath=linkRoot.path;
        linkTarget.settingsPath=page.documentPath;
        const float rowRight=ImGui::GetWindowContentRegionMax().x;
        // 所有右侧控件共用同一条右边缘：内缩 12px，文字先量宽再定位。
        const float edgeRight=rowRight-12.f;
        auto placeRight=[&](float width) { ImGui::SameLine(std::max(ImGui::GetCursorPosX(),edgeRight-width)); };
        const ImVec2 framePadding=ImGui::GetStyle().FramePadding;
        auto smallActionWidth=[&](const char* label) {
            return ImGui::CalcTextSize(label,nullptr,true).x+2.f*framePadding.x;
        };
        auto fittedButtonPadding=[&](const char* label,const ImVec2& size) {
            const ImVec2 textSize=ImGui::CalcTextSize(label,nullptr,true);
            return ImVec2(std::min(framePadding.x,std::max(0.f,(size.x-textSize.x)*.5f)),
                          std::min(framePadding.y,std::max(0.f,(size.y-textSize.y)*.5f)));
        };
        auto itemCenterY=[]() {
            return (ImGui::GetItemRectMin().y+ImGui::GetItemRectMax().y)*.5f;
        };
        auto placeCenteredY=[](float centerY,float height) {
            ImVec2 position=ImGui::GetCursorScreenPos();
            position.y=centerY-height*.5f;
            ImGui::SetCursorScreenPos(position);
        };
        auto sectionHeading=[&](const char* number,const char* title,const char* meta,bool showSeparator=true) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8.f,4.f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(theme.controlAccent,"%s",number);
            ImGui::SameLine();
            ImGui::PushFont(GetUIFontEmphasis(),settingsBodyPx);
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
            if (meta && *meta) {
                const float metaWidth=ImGui::CalcTextSize(meta).x;
                ImGui::SameLine(std::max(ImGui::GetCursorPosX()+16.f,edgeRight-metaWidth));
                ImGui::TextDisabled("%s",meta);
            }
            ImGui::PopStyleVar();
            if (showSeparator) {
                ImGui::PushStyleColor(ImGuiCol_Separator,theme.chromeBorder);
                ImGui::Separator();
                ImGui::PopStyleColor();
            }
        };
        auto copyable=[&](const char* label,const std::string& value,const char* copyId) {
            const char* copyLabel=L(AI_CopyMenu);
            ImGui::TextDisabled("%s",label);
            placeRight(smallActionWidth(copyLabel));
            ImGui::PushID(copyId);
            if (aura::ui::SmallActionButton(copyLabel)) ImGui::SetClipboardText(value.c_str());
            su::Observe(copyId);
            ImGui::PopID();
            ImGui::TextWrapped("%s",value.c_str());
        };

        // —— 连接状态横条：左边当前库，右边开关组，一行放完 ——
        ImVec4 bandBg=theme.dialogPanelBG; bandBg.w=1.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg,bandBg);
        ImGui::BeginChild("##ext-ai-status",ImVec2(0,52),false,
            ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        {
            const char* stateText=page.settings.enabled?L(ExtAi_StateOn):L(ExtAi_StateOff);
            const float switchSlotWidth=44.f;
            const float clusterWidth=ImGui::CalcTextSize(L(ExtAi_Enabled)).x+8.f+
                ImGui::CalcTextSize(stateText).x+10.f+switchSlotWidth;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY()+8.f);
            ImGui::SetCursorPosX(12.f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s",L(ExtAi_CurrentLibrary));
            ImGui::SameLine();
            ImGui::TextUnformatted(libraryName.c_str());
            ImGui::SameLine(std::max(ImGui::GetCursorPosX()+24.f,edgeRight-24.f-clusterWidth));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(L(ExtAi_Enabled));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s",stateText);
            su::Observe("ext-ai-master-state");
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(),edgeRight-24.f-switchSlotWidth));
            placeCenteredY(ImGui::GetWindowPos().y+ImGui::GetWindowHeight()*.5f,18.f);
            bool enabled=page.settings.enabled;
            if (aura::ui::Toggle("##ext-ai-enabled",&enabled)) {
                std::string error;
                if (HarkenSearch::SetExternalAiMasterSwitch(page.documentPath,page.settings,enabled,error)) {
                    page.error.clear();
                    if (enabled) {
                        page.dirty=true;
                    } else {
                        HarkenSearch::StopExternalAiBridgeServer();
                        page.status=L(ExtAi_StateOff);
                        page.statusUntil=ImGui::GetTime()+4.0;
                    }
                } else {
                    page.error=error.empty()?L(ExtAi_SaveFailed):std::move(error);
                }
            }
            su::Observe("ext-ai-enabled");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // —— 可操作目录 ——
        ImGui::Dummy(ImVec2(0,10));
        const char* pathModeLabel=state.settings.folderTreePathMode==aura::folder_tree::PathMode::Absolute
            ?L(Set_FolderTreePathAbsolute):L(Set_FolderTreePathPilot);
        sectionHeading("01",L(ExtAi_Folders),pathModeLabel);
        const ImVec2 addButtonSize(112.f,26.f);
        const int tickedCount=(int)std::count_if(page.settings.roots.begin(),page.settings.roots.end(),
            [](const HarkenSearch::ExternalAiRoot& root) { return root.enabled; });
        char folderMeta[128]{};
        snprintf(folderMeta,sizeof(folderMeta),L(ExtAi_FolderCountFmt),tickedCount);
        const char* addLabel=L(ExtAi_AddRoot);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,fittedButtonPadding(addLabel,addButtonSize));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s · %s：%s",folderMeta,L(Set_FolderTreePathMode),pathModeLabel);
        su::Observe("ext-ai-directory-summary");
        placeRight(addButtonSize.x);
        const bool addClicked=aura::ui::ActionButton(addLabel,addButtonSize);
        ImGui::PopStyleVar();
        if (addClicked) {
            const std::string picked=OpenFolderDialog(g_hwnd);
            if (!picked.empty()) {
                std::string root=picked;
                for (char& unit : root) if (unit=='/') unit='\\';
                while (root.size()>3 && root.back()=='\\') root.pop_back();
                const bool known=std::any_of(page.settings.roots.begin(),page.settings.roots.end(),
                    [&](const HarkenSearch::ExternalAiRoot& entry) { return entry.path==root; });
                if (!known) {
                    page.settings.roots.push_back({root,true});
                    page.dirty=true;
                }
            }
        }
        su::Observe("ext-ai-add-root");
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(10.0f,8.0f));
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong,ThemeColors::current().chromeBorder);
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight,ThemeColors::current().chromeBorder);
        if (ImGui::BeginTable("##ext-ai-folders",3,
                ImGuiTableFlags_BordersOuter|ImGuiTableFlags_BordersInnerH|
                ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("##folder-tick",ImGuiTableColumnFlags_WidthFixed,26.0f);
            ImGui::TableSetupColumn("##folder-path",ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##folder-action",ImGuiTableColumnFlags_WidthFixed,64.0f);
            if (page.settings.roots.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s",L(ExtAi_NoRoots));
            }
            for (std::size_t index=0; index<page.settings.roots.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool ticked=page.settings.roots[index].enabled;
                if (ImGui::Checkbox("##ticked",&ticked)) {
                    page.settings.roots[index].enabled=ticked;
                    page.dirty=true;
                }
                su::Observe("ext-ai-root-tick");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s",displayRootPath(page.settings.roots[index].path).c_str());
                ImGui::TableSetColumnIndex(2);
                const char* removeLabel=L(Main_RemoveBtn2);
                const float cellLeft=ImGui::GetCursorPosX();
                const float cellRight=cellLeft+ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosX(std::max(cellLeft,std::min(edgeRight,cellRight)-smallActionWidth(removeLabel)));
                ImGui::AlignTextToFramePadding();
                const bool removeClicked=aura::ui::SmallActionButton(removeLabel);
                su::Observe("ext-ai-root-remove");
                if (removeClicked) {
                    page.settings.roots.erase(page.settings.roots.begin()+static_cast<std::ptrdiff_t>(index));
                    page.dirty=true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        // —— 连接客户端 ——
        ImGui::Dummy(ImVec2(0,12));
        sectionHeading("02",L(ExtAi_Clients),nullptr,false);
        const auto environmentValue=[](const char* name) {
            const auto environment=Platform::ReadEnvironmentVariable(name);
            return environment.status==Platform::EnvironmentStringStatus::Success ? environment.valueUtf8 : std::string{};
        };
        const HarkenSearch::ExternalAiClientConfigInputs clientInputs{
            environmentValue("USERPROFILE"),environmentValue("APPDATA"),environmentValue("CODEX_HOME"),
            Platform::HarkenUserData::PreviewIsolationActive(),Platform::HarkenUserData::PreviewIsolationRootUtf8()};
        const bool linkReady=page.settings.enabled&&!page.dirty&&hasConnector&&
            !linkRoot.spansVolumes&&!linkTarget.libraryId.empty()&&
            !linkTarget.databasePath.empty()&&!linkTarget.rootPath.empty()&&!linkTarget.settingsPath.empty();
        if (!hasConnector) {
            ImGui::TextDisabled("%s",L(ExtAi_ConnectNoExe));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s",linkTarget.executablePath.c_str());
            ImGui::PopTextWrapPos();
        } else if (linkRoot.spansVolumes) {
            ImGui::TextDisabled("%s",L(ExtAi_ConnectMultiVolume));
        } else if (!linkReady) {
            ImGui::TextDisabled("%s",L(ExtAi_ConnectNotReady));
        }
        struct ClientCleanupFailure {
            bool revoked;
            std::string detail;
        };
        static std::unordered_map<std::string,ClientCleanupFailure> cleanupFailures;
        const char* clientLabels[]{L(ExtAi_ClientCindy),L(ExtAi_ClientCodex)};
        const char* clientIcons[]{"assets/icons/brand/mcp-client-cindy.png","assets/icons/brand/mcp-client-codex.png"};
        const HarkenSearch::ExternalAiClient clients[]{HarkenSearch::ExternalAiClient::Cindy,
            HarkenSearch::ExternalAiClient::Codex};
        const float clientIconSize=20.f;
        const float clientInsetX=12.f;
        const float clientInsetY=8.f;
        const float clientTextGap=4.f;
        const float clientActionGap=8.f;
        const ImVec2 clientSwitchSize(36.f,28.f);
        const float clientTrackHeight=18.f;
        const float clientCardWidth=ImGui::GetContentRegionAvail().x;
        const float clientCardLeft=ImGui::GetCursorScreenPos().x;
        const float clientIconX=clientCardLeft+clientInsetX;
        const float clientTextX=clientIconX+clientIconSize+clientInsetX;
        const float clientSwitchX=clientCardLeft+clientCardWidth-clientInsetX-clientSwitchSize.x;
        auto placeClientCell=[](float screenX,float screenY) {
            ImGui::SameLine(0.f,0.f);
            ImGui::SetCursorScreenPos(ImVec2(screenX,screenY));
        };
        ImDrawList* clientDraw=ImGui::GetWindowDrawList();
        float clientDividerY=0.f;
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(ImGui::GetStyle().ItemSpacing.x,0.f));
        for (int clientIndex=0; clientIndex<2; ++clientIndex) {
            ImGui::PushID(clientIndex);
            const auto location=HarkenSearch::ResolveExternalAiClientConfigLocation(clients[clientIndex],clientInputs);
            const auto& configPath=location.configPath;
            const auto format=std::filesystem::u8path(configPath).extension()==".toml"?
                HarkenSearch::ExternalAiClient::Codex:HarkenSearch::ExternalAiClient::Cindy;
            const auto linkState=HarkenSearch::QueryExternalAiClientLink(format,configPath,linkTarget);
            const bool connected=linkState.state==HarkenSearch::ExternalAiLinkState::Connected;
            const bool canDisconnect=linkState.canDisconnect;
            if (connected || (!canDisconnect && linkState.state!=HarkenSearch::ExternalAiLinkState::Failed))
                cleanupFailures.erase(configPath);
            const auto cleanupFailure=cleanupFailures.find(configPath);
            const bool cleanupFailed=cleanupFailure!=cleanupFailures.end();
            const bool previouslyRevoked=cleanupFailed&&cleanupFailure->second.revoked;
            const bool showCleanup=!connected&&!configPath.empty()&&(canDisconnect||cleanupFailed);
            const bool canConnect=!configPath.empty()&&linkReady&&!showCleanup&&
                linkState.state==HarkenSearch::ExternalAiLinkState::Disconnected;
            const bool switchEnabled=connected?canDisconnect:canConnect;
            const char* clientLabel=clientIndex==0&&format==HarkenSearch::ExternalAiClient::Codex?
                L(ExtAi_ClientCindyCodex):clientLabels[clientIndex];
            const char* linkStateText=connected?
                (page.settings.enabled?L(ExtAi_ClientConnected):L(ExtAi_ClientConfiguredLibraryOff)):
                (cleanupFailed?(previouslyRevoked?L(ExtAi_ClientRevokedCleanupFailed):L(ExtAi_ClientCleanupFailed)):
                (!location.detected?L(ExtAi_ClientNotDetected):
                (configPath.empty()?L(ExtAi_ClientNoConfig):
                (linkState.state==HarkenSearch::ExternalAiLinkState::Failed?L(ExtAi_ClientUnreadable):
                (linkState.state==HarkenSearch::ExternalAiLinkState::Conflict?L(ExtAi_ClientKeyConflict):
                (linkState.needsRelink?L(ExtAi_ClientRelink):L(ExtAi_ClientDisconnected)))))));
            const char* cleanupLabel=cleanupFailed?L(ExtAi_ClientRetryCleanupBtn):L(ExtAi_ClientCleanupBtn);
            const float cleanupWidth=showCleanup?smallActionWidth(cleanupLabel):0.f;
            const float cleanupX=clientSwitchX-clientActionGap-cleanupWidth;
            const float textRight=(showCleanup?cleanupX:clientSwitchX)-clientInsetX;
            const float textWidth=std::max(1.f,textRight-clientTextX);
            const float nameHeight=ImGui::CalcTextSize(clientLabel,nullptr,false,textWidth).y;
            const float stateHeight=ImGui::CalcTextSize(linkStateText,nullptr,false,textWidth).y;
            const float textHeight=nameHeight+clientTextGap+stateHeight;
            const float rowHeight=std::max(clientSwitchSize.y,textHeight)+2.f*clientInsetY;
            const float rowTop=ImGui::GetCursorScreenPos().y;
            const float rowCenter=rowTop+rowHeight*.5f;
            const float nameY=rowCenter-textHeight*.5f;
            if (clientIndex>0) clientDividerY=rowTop;
            ImGui::Dummy(ImVec2(clientCardWidth,rowHeight));

            placeClientCell(clientIconX,rowCenter-clientIconSize*.5f);
            ImGui::Dummy(ImVec2(clientIconSize,clientIconSize));
            su::Observe(clientIndex==0?"ext-ai-client-cindy-logo":"ext-ai-client-codex-logo");
            if (ImGui::IsItemVisible()) {
                const auto asset=aura::ui::LoadPackagedAsset(clientIcons[clientIndex]);
                if (asset.glId!=0&&asset.w>0&&asset.h>0) {
                    const float scale=clientIconSize/static_cast<float>(std::max(asset.w,asset.h));
                    const ImVec2 halfSize(asset.w*scale*.5f,asset.h*scale*.5f);
                    const float centerX=clientIconX+clientIconSize*.5f;
                    clientDraw->AddImage(static_cast<ImTextureID>(asset.glId),
                        ImVec2(centerX-halfSize.x,rowCenter-halfSize.y),ImVec2(centerX+halfSize.x,rowCenter+halfSize.y),
                        ImVec2(0.f,0.f),ImVec2(1.f,1.f),ImGui::GetColorU32(theme.interfaceText));
                }
            }
            placeClientCell(clientTextX,nameY);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+textWidth);
            ImGui::TextUnformatted(clientLabel);
            su::Observe(clientIndex==0?"ext-ai-client-cindy-name":"ext-ai-client-codex-name");
            placeClientCell(clientTextX,nameY+nameHeight+clientTextGap);
            ImGui::TextDisabled("%s",linkStateText);
            su::Observe(clientIndex==0?"ext-ai-client-cindy-state":"ext-ai-client-codex-state");
            ImGui::PopTextWrapPos();
            if (cleanupFailed) ImGui::SetItemTooltip("%s",cleanupFailure->second.detail.c_str());

            const float actionY=rowCenter-clientSwitchSize.y*.5f;
            bool cleanupPressed=false;
            if (showCleanup) {
                placeClientCell(cleanupX,actionY);
                const ImVec2 cleanupSize(cleanupWidth,clientSwitchSize.y);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,fittedButtonPadding(cleanupLabel,cleanupSize));
                cleanupPressed=aura::ui::ActionButton(cleanupLabel,cleanupSize,aura::ui::ButtonVariant::Quiet);
                ImGui::PopStyleVar();
                su::Observe(clientIndex==0?"ext-ai-client-cindy-cleanup":"ext-ai-client-codex-cleanup");
            }
            placeClientCell(clientSwitchX,actionY);
            ImGui::BeginDisabled(!switchEnabled);
            const bool switchPressed=ImGui::InvisibleButton("##ext-ai-client-switch",clientSwitchSize,ImGuiButtonFlags_EnableNav);
            su::Observe(clientIndex==0?"ext-ai-client-cindy":"ext-ai-client-codex");
            if (ImGui::IsItemVisible()) {
                const float trackLeft=ImGui::GetItemRectMin().x;
                const float trackRight=ImGui::GetItemRectMax().x;
                const float radius=clientTrackHeight*.5f;
                const float thumbX=connected?trackRight-radius:trackLeft+radius;
                clientDraw->AddRectFilled(ImVec2(trackLeft,rowCenter-radius),ImVec2(trackRight,rowCenter+radius),
                    ImGui::GetColorU32(connected?theme.controlAccent:theme.frameBgBase),radius);
                clientDraw->AddCircleFilled(ImVec2(thumbX,rowCenter),clientTrackHeight*.4f,
                    ImGui::GetColorU32(theme.interfaceText),20);
                if (switchEnabled) ImGui::SetItemTooltip("%s",connected?L(ExtAi_DisconnectBtn):L(ExtAi_ConnectBtn));
            }
            ImGui::EndDisabled();

            const bool unlinkRequested=cleanupPressed||(switchPressed&&connected&&canDisconnect);
            const bool linkRequested=switchPressed&&canConnect;
            if (unlinkRequested||linkRequested) {
                HarkenSearch::ExternalAiLinkStatus result;
                const bool changed=unlinkRequested
                    ? HarkenSearch::UnlinkExternalAiClient(format,configPath,result)
                    : HarkenSearch::LinkExternalAiClient(format,configPath,linkTarget,result);
                char message[768];
                if (changed) {
                    cleanupFailures.erase(configPath);
                    snprintf(message,sizeof(message),unlinkRequested?L(ExtAi_DisconnectedFmt):L(ExtAi_ConnectedFmt),configPath.c_str());
                } else {
                    const bool revoked=unlinkRequested&&(result.revoked||previouslyRevoked);
                    const char* detail=result.detail.empty()?L(ExtAi_LinkFailed):result.detail.c_str();
                    if (cleanupPressed||revoked) cleanupFailures[configPath]={revoked,detail};
                    snprintf(message,sizeof(message),revoked?L(ExtAi_RevokedCleanupFailedFmt):L(ExtAi_LinkFailedFmt),detail);
                }
                page.linkStatus=message;
                page.linkStatusUntil=ImGui::GetTime()+6.0;
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        su::Observe("ext-ai-clients-card");
        if (ImGui::IsItemVisible()) {
            const ImVec2 cardMin=ImGui::GetItemRectMin();
            const ImVec2 cardMax=ImGui::GetItemRectMax();
            const ImU32 border=ImGui::GetColorU32(theme.chromeBorder);
            clientDraw->AddRect(cardMin,cardMax,border,8.f);
            clientDraw->AddLine(ImVec2(cardMin.x+1.f,clientDividerY),ImVec2(cardMax.x-1.f,clientDividerY),border);
        }
        if (!page.linkStatus.empty() && ImGui::GetTime()<page.linkStatusUntil) {
            ImGui::PushTextWrapPos(0.0f); ImGui::TextDisabled("%s",page.linkStatus.c_str()); ImGui::PopTextWrapPos();
        }
        su::Observe("ext-ai-clients");

        // —— 分项权限 ——
        ImGui::Dummy(ImVec2(0,12));
        sectionHeading("03",L(ExtAi_Capabilities),L(ExtAi_FooterHint));
        const char* capabilityLabels[]{L(ExtAi_CapSearchAudio),L(ExtAi_CapFindSimilar),L(ExtAi_CapFindFolders),
            L(ExtAi_CapReadFileName),L(ExtAi_CapReadMetadata),L(ExtAi_CapEditMetadata),L(ExtAi_CapAiNaming),
            L(ExtAi_CapReadPlaylist),L(ExtAi_CapEditPlaylist)};
        const char* accessLabels[]{L(ExtAi_AccessNone),L(ExtAi_AccessRead),L(ExtAi_AccessEdit)};
        const int groupStarts[]{0,3,7};
        const int groupEnds[]{2,6,8};
        const int groupCount=3;
        const ImVec2 accessButtonSize(62.f,28.f);
        const float accessButtonGap=3.f;
        const float accessGroupWidth=accessButtonSize.x*3.f+accessButtonGap*2.f;
        for (int group=0; group<groupCount; ++group) {
            const char* groupTitle=group==0?L(ExtAi_GroupLookup)
                :(group==1?L(ExtAi_GroupMetadata):L(ExtAi_GroupPlaylist));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8.f,4.f));
            ImGui::TextColored(theme.controlAccent,"%s",groupTitle);
            ImGui::PopStyleVar();
            for (int index=groupStarts[group]; index<=groupEnds[group]; ++index) {
                ImGui::PushID(index);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(capabilityLabels[index]);
                const float capabilityCenterY=itemCenterY();
                su::Observe("ext-ai-capability-label");
                placeRight(accessGroupWidth);
                const int currentAccess=static_cast<int>(page.settings.capabilities[static_cast<std::size_t>(index)]);
                for (int option=0; option<3; ++option) {
                    if (option>0) ImGui::SameLine(0.f,accessButtonGap);
                    placeCenteredY(capabilityCenterY,accessButtonSize.y);
                    const bool selected=currentAccess==option;
                    const ImVec4 selectedBg=option==static_cast<int>(HarkenSearch::ExternalAiAccess::Editable)
                        ?theme.controlAccent:theme.resultsTabActive;
                    const ImVec4 selectedText=option==static_cast<int>(HarkenSearch::ExternalAiAccess::Editable)
                        ?theme.mainWindowBG:theme.interfaceText;
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,5.f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,fittedButtonPadding(accessLabels[option],accessButtonSize));
                    ImGui::PushStyleColor(ImGuiCol_Button,selected?selectedBg:ImVec4(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,selected?selectedBg:theme.frameBgBase);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,selected?selectedBg:theme.frameBgBase);
                    ImGui::PushStyleColor(ImGuiCol_Text,selected?selectedText:theme.resultsSecondaryText);
                    if (ImGui::Button(accessLabels[option],accessButtonSize)) {
                        page.settings.capabilities[static_cast<std::size_t>(index)]=
                            static_cast<HarkenSearch::ExternalAiAccess>(option);
                        page.dirty=true;
                    }
                    ImGui::PopStyleColor(4);
                    ImGui::PopStyleVar(2);
                    su::Observe((std::string("ext-ai-capability-")+std::to_string(index)+"-"+std::to_string(option)).c_str());
                }
                ImGui::PopID();
                ImGui::PushStyleColor(ImGuiCol_Separator,theme.chromeBorder);
                ImGui::Separator();
                ImGui::PopStyleColor();
            }
        }
        ImGui::Dummy(ImVec2(0,4));
        ImGui::TextWrapped("%s",L(ExtAi_EditNote));

        ImGui::Dummy(ImVec2(0,10));
        const std::string commandLine=linkReady?HarkenSearch::ExternalAiClientCommandLine(linkTarget):std::string{};
        const std::string copyPromptLink=std::string(L(ExtAi_CopyAiPrompt))+"###ext-ai-copy-ai-prompt";
        ImGui::TextUnformatted(L(ExtAi_AiPromptRow));
        su::Observe("ext-ai-ai-prompt-label");
        placeRight(ImGui::CalcTextSize(copyPromptLink.c_str(),nullptr,true).x);
        ImGui::BeginDisabled(!linkReady);
        ImGui::PushStyleColor(ImGuiCol_TextLink,ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (ImGui::TextLink(copyPromptLink.c_str()) && linkReady) {
            char aiPrompt[4096];
            snprintf(aiPrompt,sizeof(aiPrompt),L(ExtAi_AiSetupPromptFmt),commandLine.c_str());
            ImGui::SetClipboardText(aiPrompt);
        }
        su::Observe("ext-ai-copy-ai-prompt");
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("%s",L(ExtAi_AiPromptNote));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0,4));
        ImGui::PushStyleColor(ImGuiCol_Separator,theme.chromeBorder);
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::SetNextItemOpen(page.detailsOpen,ImGuiCond_Always);
        page.detailsOpen=ImGui::TreeNodeEx("ext-ai-details",
            ImGuiTreeNodeFlags_NoTreePushOnOpen|ImGuiTreeNodeFlags_NoAutoOpenOnLog,"%s",L(ExtAi_ManualConnect));
        su::Observe("ext-ai-details");
        if (page.detailsOpen) {
            copyable(L(ExtAi_Document),page.documentPath,"ext-ai-copy-auth-file");
            ImGui::Dummy(ImVec2(0,4));
            if (linkReady) {
                copyable(L(ExtAi_ConnectorCommand),commandLine,"ext-ai-copy-command");
            } else {
                ImGui::TextDisabled("%s",L(ExtAi_ConnectorCommand));
                ImGui::TextWrapped("%s",linkTarget.executablePath.c_str());
            }
        }

        // —— 最近一次写入回执 ——
        {
            HarkenSearch::ExternalAiPlaylistReceiptStore receipts;
            receipts.Load(HarkenSearch::ExternalAiReceiptsPath(settingsDirectory));
            if (const auto* latest = receipts.Latest()) {
                char line[512];
                snprintf(line,sizeof(line),L(ExtAi_LastWriteFmt),
                    latest->added,latest->skippedExisting);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s",line);
                ImGui::PopTextWrapPos();
                su::Observe("ext-ai-last-write");
            }
        }

        // —— 底部页脚：说明在左，保存与重新读取在右 ——
        ImGui::Dummy(ImVec2(0,14));
        ImGui::PushStyleColor(ImGuiCol_Separator,ThemeColors::current().chromeBorder);
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0,8));
        const float saveWidth=132.f;
        const float reloadWidth=112.f;
        const char* footerNote=!page.status.empty() && ImGui::GetTime()<page.statusUntil
            ? page.status.c_str()
            : (page.dirty?L(ExtAi_Unsaved):"");
        if (*footerNote) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s",footerNote);
        }
        placeRight(reloadWidth+8.f+saveWidth);
        if (aura::ui::ActionButton(L(ExtAi_Reload),ImVec2(reloadWidth,32),aura::ui::ButtonVariant::Quiet)) loadLibrary(databasePath);
        su::Observe("ext-ai-reload");
        placeRight(saveWidth);
        if (aura::ui::ActionButton(L(ExtAi_SaveCurrent),ImVec2(saveWidth,32),aura::ui::ButtonVariant::PrimarySearch)) saveDocument();
        su::Observe("ext-ai-save");
        if (!page.error.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::TextWrapped("%s",page.error.c_str()); ImGui::PopTextWrapPos(); }
    }
}
