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
        linkTarget.rootPath=[&]() {
            std::string commonPath;
            for (const auto& root : page.settings.roots) {
                if (!root.enabled) continue;
                const std::string preserved=aura::folder_tree::NormalizeFolderPathPreserved(root.path);
                if (commonPath.empty()) {
                    commonPath=preserved;
                    continue;
                }
                std::string commonNorm=aura::folder_tree::NormalizeFolderPathForCompare(commonPath);
                const std::string candidateNorm=aura::folder_tree::NormalizeFolderPathForCompare(preserved);
                if (aura::folder_tree::PathIsInsideRoot(candidateNorm,commonNorm)) continue;
                if (aura::folder_tree::PathIsInsideRoot(commonNorm,candidateNorm)) {
                    commonPath=preserved;
                    continue;
                }
                while (!commonPath.empty() && !aura::folder_tree::PathIsInsideRoot(candidateNorm,commonNorm)) {
                    const auto cut=commonPath.find_last_of("\\/");
                    if (cut==std::string::npos || cut<2) {
                        commonPath.clear();
                        break;
                    }
                    commonPath.erase(cut);
                    if (commonPath.size()==2) commonPath.push_back('\\');
                    commonNorm=aura::folder_tree::NormalizeFolderPathForCompare(commonPath);
                }
            }
            return commonPath;
        }();
        linkTarget.settingsPath=page.documentPath;
        const float rowRight=ImGui::GetWindowContentRegionMax().x;
        // 所有右侧控件共用同一条右边缘：内缩 12px，文字先量宽再定位。
        const float edgeRight=rowRight-12.f;
        auto placeRight=[&](float width) { ImGui::SameLine(std::max(ImGui::GetCursorPosX(),edgeRight-width)); };
        auto sectionHeading=[&](const char* number,const char* title,const char* meta) {
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
            ImGui::PushStyleColor(ImGuiCol_Separator,theme.chromeBorder);
            ImGui::Separator();
            ImGui::PopStyleColor();
        };
        auto copyable=[&](const char* label,const std::string& value) {
            ImGui::TextDisabled("%s",label);
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(),edgeRight-64.f));
            if (aura::ui::SmallActionButton(L(Main_CopyBtn))) ImGui::SetClipboardText(value.c_str());
            ImGui::TextWrapped("%s",value.c_str());
        };

        // —— 连接状态横条：左边当前库，右边开关组，一行放完 ——
        ImVec4 bandBg=theme.dialogPanelBG; bandBg.w=1.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg,bandBg);
        ImGui::BeginChild("##ext-ai-status",ImVec2(0,52),false,
            ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        {
            const char* stateText=page.settings.enabled?L(ExtAi_StateOn):L(ExtAi_StateOff);
            const float switchWidth=44.f;
            const float clusterWidth=ImGui::CalcTextSize(L(ExtAi_Enabled)).x+8.f+
                ImGui::CalcTextSize(stateText).x+10.f+switchWidth;
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
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(),edgeRight-24.f-switchWidth));
            bool enabled=page.settings.enabled;
            if (aura::ui::Toggle("##ext-ai-enabled",&enabled)) { page.settings.enabled=enabled; page.dirty=true; }
            su::Observe("ext-ai-enabled");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // —— 可操作目录 ——
        ImGui::Dummy(ImVec2(0,10));
        const char* pathModeLabel=state.settings.folderTreePathMode==aura::folder_tree::PathMode::Absolute
            ?L(Set_FolderTreePathAbsolute):L(Set_FolderTreePathPilot);
        sectionHeading("01",L(ExtAi_Folders),pathModeLabel);
        const float addWidth=112.f;
        const int tickedCount=(int)std::count_if(page.settings.roots.begin(),page.settings.roots.end(),
            [](const HarkenSearch::ExternalAiRoot& root) { return root.enabled; });
        char folderMeta[128]{};
        snprintf(folderMeta,sizeof(folderMeta),L(ExtAi_FolderCountFmt),tickedCount);
        ImGui::TextDisabled("%s · %s：%s",folderMeta,L(Set_FolderTreePathMode),pathModeLabel);
        placeRight(addWidth);
        if (aura::ui::ActionButton(L(ExtAi_AddRoot),ImVec2(addWidth,26))) {
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
                ImGui::AlignTextToFramePadding();
                if (aura::ui::SmallActionButton(L(Main_RemoveBtn2))) {
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
        sectionHeading("02",L(ExtAi_Clients),nullptr);
        const std::string homePath=[&]() {
            // 预览隔离构建里把“家目录”固定在隔离根内，QA 永远不碰真实用户配置。
            if (Platform::HarkenUserData::PreviewIsolationActive()) {
                const std::string isolatedRoot=Platform::HarkenUserData::PreviewIsolationRootUtf8();
                if (!isolatedRoot.empty()) return isolatedRoot+"\\profile\\home";
            }
            const auto environment=Platform::ReadEnvironmentVariable("USERPROFILE");
            return environment.status==Platform::EnvironmentStringStatus::Success ? environment.valueUtf8 : std::string{};
        }();
        const bool linkReady=hasConnector&&!linkTarget.libraryId.empty()&&!linkTarget.databasePath.empty()&&
            !linkTarget.rootPath.empty()&&!linkTarget.settingsPath.empty();
        if (!hasConnector) {
            ImGui::TextDisabled("%s",L(ExtAi_ConnectNoExe));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s",linkTarget.executablePath.c_str());
            ImGui::PopTextWrapPos();
        } else if (!linkReady) {
            ImGui::TextDisabled("%s",L(ExtAi_ConnectNotReady));
        }
        auto drawClientLogo=[&](HarkenSearch::ExternalAiClient client,const ImVec2& origin,bool connected) {
            ImDrawList* draw=ImGui::GetWindowDrawList();
            const ImVec4 tint=connected?theme.controlAccent:theme.resultsSecondaryText;
            const ImU32 border=ImGui::ColorConvertFloat4ToU32(theme.chromeBorder);
            const ImU32 color=ImGui::ColorConvertFloat4ToU32(tint);
            const ImVec2 max(origin.x+34.f,origin.y+34.f);
            draw->AddRectFilled(origin,max,ImGui::ColorConvertFloat4ToU32(theme.dialogPanelBG),7.f);
            draw->AddRect(origin,max,border,7.f,0,1.f);
            if (client==HarkenSearch::ExternalAiClient::Cindy) {
                const ImVec2 points[]{
                    {origin.x+23.f,origin.y+9.f},{origin.x+19.f,origin.y+7.f},
                    {origin.x+13.f,origin.y+8.f},{origin.x+9.f,origin.y+12.f},
                    {origin.x+9.f,origin.y+18.f},{origin.x+13.f,origin.y+23.f},
                    {origin.x+19.f,origin.y+24.f},{origin.x+23.f,origin.y+21.f}};
                draw->AddPolyline(points,8,color,0,1.8f);
                draw->AddCircle(ImVec2(origin.x+16.f,origin.y+16.f),2.3f,color,12,1.7f);
            } else {
                const ImVec2 hexagon[]{
                    {origin.x+17.f,origin.y+6.f},{origin.x+25.f,origin.y+11.f},
                    {origin.x+25.f,origin.y+21.f},{origin.x+17.f,origin.y+27.f},
                    {origin.x+9.f,origin.y+21.f},{origin.x+9.f,origin.y+11.f}};
                draw->AddPolyline(hexagon,6,color,ImDrawFlags_Closed,1.6f);
                draw->AddLine(ImVec2(origin.x+12.f,origin.y+12.f),ImVec2(origin.x+17.f,origin.y+9.f),color,1.5f);
                draw->AddLine(ImVec2(origin.x+17.f,origin.y+9.f),ImVec2(origin.x+22.f,origin.y+12.f),color,1.5f);
                draw->AddLine(ImVec2(origin.x+12.f,origin.y+20.f),ImVec2(origin.x+17.f,origin.y+23.f),color,1.5f);
                draw->AddLine(ImVec2(origin.x+17.f,origin.y+23.f),ImVec2(origin.x+22.f,origin.y+20.f),color,1.5f);
            }
        };
        const char* clientLabels[]{L(ExtAi_ClientCindy),L(ExtAi_ClientCodex)};
        const HarkenSearch::ExternalAiClient clients[]{HarkenSearch::ExternalAiClient::Cindy,
            HarkenSearch::ExternalAiClient::Codex};
        for (int clientIndex=0; clientIndex<2; ++clientIndex) {
            ImGui::PushID(clientIndex);
            const std::string configPath=HarkenSearch::ExternalAiClientConfigPath(clients[clientIndex],homePath);
            const bool detected=HarkenSearch::ExternalAiClientDetected(clients[clientIndex],homePath);
            const auto linkState=HarkenSearch::QueryExternalAiClientLink(clients[clientIndex],configPath,linkTarget);
            const bool connected=linkState.state==HarkenSearch::ExternalAiLinkState::Connected;
            if (clientIndex>0) {
                ImGui::PushStyleColor(ImGuiCol_Separator,theme.chromeBorder);
                ImGui::Separator();
                ImGui::PopStyleColor();
            }
            const ImVec2 logoOrigin=ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(34.f,34.f));
            drawClientLogo(clients[clientIndex],logoOrigin,connected);
            ImGui::SameLine(0.f,12.f);
            ImGui::BeginGroup();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(clientLabels[clientIndex]);
            const char* linkStateText=!detected?L(ExtAi_ClientNotDetected)
                :(connected?L(ExtAi_ClientConnected)
                :(linkState.state==HarkenSearch::ExternalAiLinkState::Failed?L(ExtAi_ClientUnreadable)
                :L(ExtAi_ClientDisconnected)));
            ImGui::TextDisabled("%s",linkStateText);
            ImGui::EndGroup();
            const float linkButtonWidth=96.f;
            placeRight(linkButtonWidth);
            ImGui::BeginDisabled(!linkReady||!detected);
            if (aura::ui::ActionButton(connected?L(ExtAi_DisconnectBtn):L(ExtAi_ConnectBtn),ImVec2(linkButtonWidth,28),
                    connected?aura::ui::ButtonVariant::Quiet:aura::ui::ButtonVariant::PrimarySearch)) {
                HarkenSearch::ExternalAiLinkStatus result;
                const bool changed=connected
                    ? HarkenSearch::UnlinkExternalAiClient(clients[clientIndex],configPath,result)
                    : HarkenSearch::LinkExternalAiClient(clients[clientIndex],configPath,linkTarget,result);
                char message[768];
                if (changed) {
                    snprintf(message,sizeof(message),connected?L(ExtAi_DisconnectedFmt):L(ExtAi_ConnectedFmt),configPath.c_str());
                } else {
                    snprintf(message,sizeof(message),L(ExtAi_LinkFailedFmt),
                        result.detail.empty()?L(ExtAi_LinkFailed):result.detail.c_str());
                }
                page.linkStatus=message;
                page.linkStatusUntil=ImGui::GetTime()+6.0;
            }
            ImGui::EndDisabled();
            su::Observe(clientIndex==0?"ext-ai-client-cindy":"ext-ai-client-codex");
            ImGui::PopID();
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
        const float accessButtonWidth=62.f;
        const float accessButtonGap=3.f;
        const float accessGroupWidth=accessButtonWidth*3.f+accessButtonGap*2.f;
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
                placeRight(accessGroupWidth);
                const int currentAccess=static_cast<int>(page.settings.capabilities[static_cast<std::size_t>(index)]);
                for (int option=0; option<3; ++option) {
                    if (option>0) ImGui::SameLine(0.f,accessButtonGap);
                    const bool selected=currentAccess==option;
                    const ImVec4 selectedBg=option==static_cast<int>(HarkenSearch::ExternalAiAccess::Editable)
                        ?theme.controlAccent:theme.resultsTabActive;
                    const ImVec4 selectedText=option==static_cast<int>(HarkenSearch::ExternalAiAccess::Editable)
                        ?theme.mainWindowBG:theme.interfaceText;
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,5.f);
                    ImGui::PushStyleColor(ImGuiCol_Button,selected?selectedBg:ImVec4(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,selected?selectedBg:theme.frameBgBase);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,selected?selectedBg:theme.frameBgBase);
                    ImGui::PushStyleColor(ImGuiCol_Text,selected?selectedText:theme.resultsSecondaryText);
                    if (ImGui::Button(accessLabels[option],ImVec2(accessButtonWidth,28.f))) {
                        page.settings.capabilities[static_cast<std::size_t>(index)]=
                            static_cast<HarkenSearch::ExternalAiAccess>(option);
                        page.dirty=true;
                    }
                    ImGui::PopStyleColor(4);
                    ImGui::PopStyleVar();
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

        // —— 连接详情 ——
        ImGui::Dummy(ImVec2(0,10));
        char detailsLabel[128];
        snprintf(detailsLabel,sizeof(detailsLabel),"%s  %s",L(ExtAi_ConnectionDetails),
            page.detailsOpen?u8"▾":u8"›");
        const float detailsWidth=ImGui::CalcTextSize(detailsLabel).x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),edgeRight-12.f-detailsWidth));
        if (aura::ui::SmallActionButton(detailsLabel,aura::ui::ButtonVariant::Quiet)) page.detailsOpen=!page.detailsOpen;
        su::Observe("ext-ai-details");
        if (page.detailsOpen) {
            copyable(L(ExtAi_LibraryId),page.libraryId);
            copyable(L(ExtAi_Document),page.documentPath);
            ImGui::TextDisabled("%s",L(ExtAi_ConnectorCommand));
            if (hasConnector && aura::ui::SmallActionButton(L(Main_CopyBtn),aura::ui::ButtonVariant::Quiet)) {
                ImGui::SetClipboardText(HarkenSearch::ExternalAiClientCommandLine(linkTarget).c_str());
            }
            ImGui::TextWrapped("%s",linkTarget.executablePath.c_str());
        }

        // —— 最近一次写入回执 ——
        {
            HarkenSearch::ExternalAiPlaylistReceiptStore receipts;
            receipts.Load(HarkenSearch::ExternalAiReceiptsPath(settingsDirectory));
            if (const auto* latest = receipts.Latest()) {
                char line[512];
                snprintf(line,sizeof(line),L(ExtAi_LastWriteFmt),
                    latest->added,latest->skippedExisting,latest->receiptId.c_str());
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
            : (page.dirty?L(ExtAi_Unsaved):L(ExtAi_FooterHint));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s",footerNote);
        placeRight(reloadWidth+8.f+saveWidth);
        if (aura::ui::ActionButton(L(ExtAi_Reload),ImVec2(reloadWidth,32),aura::ui::ButtonVariant::Quiet)) loadLibrary(databasePath);
        su::Observe("ext-ai-reload");
        placeRight(saveWidth);
        if (aura::ui::ActionButton(L(ExtAi_SaveCurrent),ImVec2(saveWidth,32),aura::ui::ButtonVariant::PrimarySearch)) saveDocument();
        su::Observe("ext-ai-save");
        if (!page.error.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::TextWrapped("%s",page.error.c_str()); ImGui::PopTextWrapPos(); }
    }
}
