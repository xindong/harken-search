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
        ImGui::TextWrapped("%s",L(ExtAi_Intro));
        ImGui::Dummy(ImVec2(0,4));
        auto copyable=[&](const char* label,const std::string& value) {
            const float rowRight=ImGui::GetWindowContentRegionMax().x;
            ImGui::TextDisabled("%s",label);
            ImGui::SameLine(rowRight-64.f);
            if (aura::ui::SmallActionButton(L(Main_CopyBtn))) ImGui::SetClipboardText(value.c_str());
            ImGui::PushTextWrapPos(0.0f); ImGui::TextUnformatted(value.c_str()); ImGui::PopTextWrapPos();
        };
        copyable(L(ExtAi_LibraryId),page.libraryId);
        copyable(L(ExtAi_Document),page.documentPath);
        ImGui::Dummy(ImVec2(0,6));
        bool enabled=page.settings.enabled;
        if (ImGui::Checkbox(L(ExtAi_Enabled),&enabled)) { page.settings.enabled=enabled; page.dirty=true; }
        su::Observe("ext-ai-enabled");
        ImGui::Dummy(ImVec2(0,4));
        heading(L(ExtAi_Roots));
        if (page.settings.roots.empty()) {
            ImGui::TextDisabled("%s",L(ExtAi_NoRoots));
        }
        for (std::size_t index=0; index<page.settings.roots.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            const float rowRight=ImGui::GetWindowContentRegionMax().x;
            ImGui::AlignTextToFramePadding();
            ImGui::PushTextWrapPos(rowRight-96.f);
            ImGui::TextUnformatted(page.settings.roots[index].c_str());
            ImGui::PopTextWrapPos();
            ImGui::SameLine(rowRight-78.f);
            if (aura::ui::SmallActionButton(L(Main_RemoveBtn2))) {
                page.settings.roots.erase(page.settings.roots.begin()+static_cast<std::ptrdiff_t>(index));
                page.dirty=true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (aura::ui::ActionButton(L(ExtAi_AddRoot),ImVec2(168,30))) {
            const std::string picked=OpenFolderDialog(g_hwnd);
            if (!picked.empty()) {
                std::string root=picked;
                for (char& unit : root) if (unit=='/') unit='\\';
                while (root.size()>3 && root.back()=='\\') root.pop_back();
                if (std::find(page.settings.roots.begin(),page.settings.roots.end(),root)==page.settings.roots.end()) {
                    page.settings.roots.push_back(root);
                    page.dirty=true;
                }
            }
        }
        su::Observe("ext-ai-add-root");
        ImGui::Dummy(ImVec2(0,4));
        heading(L(ExtAi_Capabilities));
        const char* capabilityLabels[]{L(ExtAi_CapSearchAudio),L(ExtAi_CapFindSimilar),L(ExtAi_CapFindFolders),
            L(ExtAi_CapReadFileName),L(ExtAi_CapReadMetadata),L(ExtAi_CapEditMetadata)};
        const char* accessLabels[]{L(ExtAi_AccessNone),L(ExtAi_AccessRead),L(ExtAi_AccessEdit)};
        for (std::size_t index=0; index<HarkenSearch::ExternalAiCapabilities.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            const float rowRight=ImGui::GetWindowContentRegionMax().x;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(capabilityLabels[index]);
            ImGui::SameLine(rowRight-186.f);
            ImGui::SetNextItemWidth(184.f);
            int access=static_cast<int>(page.settings.capabilities[index]);
            if (combo(access,{accessLabels[0],accessLabels[1],accessLabels[2]})) {
                page.settings.capabilities[index]=static_cast<HarkenSearch::ExternalAiAccess>(access);
                page.dirty=true;
            }
            ImGui::PopID();
        }
        ImGui::TextWrapped("%s",L(ExtAi_EditNote));
        ImGui::Dummy(ImVec2(0,6));
        if (aura::ui::ActionButton(L(ModuleUI_Save),ImVec2(150,32),aura::ui::ButtonVariant::PrimarySearch)) saveDocument();
        su::Observe("ext-ai-save");
        ImGui::SameLine();
        if (aura::ui::ActionButton(L(ExtAi_Reload),ImVec2(130,32),aura::ui::ButtonVariant::Quiet)) loadLibrary(databasePath);
        su::Observe("ext-ai-reload");
        ImGui::SameLine();
        if (!page.status.empty() && ImGui::GetTime()<page.statusUntil) ImGui::TextDisabled("%s",page.status.c_str());
        else if (page.dirty) ImGui::TextDisabled("%s",L(ExtAi_Unsaved));
        if (!page.error.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::TextWrapped("%s",page.error.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::Dummy(ImVec2(0,4));
        heading(L(ExtAi_Clients));
        ImGui::TextWrapped("%s",L(ExtAi_ConnectHint));
        const std::string homePath=[&]() {
            // 预览隔离构建里把“家目录”固定在隔离根内，QA 永远不碰真实用户配置。
            if (Platform::HarkenUserData::PreviewIsolationActive()) {
                const std::string isolatedRoot=Platform::HarkenUserData::PreviewIsolationRootUtf8();
                if (!isolatedRoot.empty()) return isolatedRoot+"\\profile\\home";
            }
            const auto environment=Platform::ReadEnvironmentVariable("USERPROFILE");
            return environment.status==Platform::EnvironmentStringStatus::Success ? environment.valueUtf8 : std::string{};
        }();
        HarkenSearch::ExternalAiLinkTarget linkTarget;
        linkTarget.executablePath=GetExeDirUtf8()+"\\HarkenExternalAiMcp.exe";
        linkTarget.libraryId=page.libraryId;
        linkTarget.databasePath=databasePath;
        linkTarget.rootPath=page.settings.roots.empty()?std::string{}:page.settings.roots.front();
        linkTarget.settingsPath=page.documentPath;
        const bool connectorReady=Platform::FileSystem::Exists(linkTarget.executablePath);
        const bool linkReady=connectorReady&&!linkTarget.libraryId.empty()&&!linkTarget.databasePath.empty()&&
            !linkTarget.rootPath.empty()&&!linkTarget.settingsPath.empty();
        if (!connectorReady) {
            ImGui::TextDisabled("%s",L(ExtAi_ConnectNoExe));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s",linkTarget.executablePath.c_str());
            ImGui::PopTextWrapPos();
        } else if (!linkReady) {
            ImGui::TextDisabled("%s",L(ExtAi_ConnectNotReady));
        }
        const char* clientLabels[]{L(ExtAi_ClientCindy),L(ExtAi_ClientCodex)};
        const HarkenSearch::ExternalAiClient clients[]{HarkenSearch::ExternalAiClient::Cindy,
            HarkenSearch::ExternalAiClient::Codex};
        for (int clientIndex=0; clientIndex<2; ++clientIndex) {
            ImGui::PushID(clientIndex);
            const std::string configPath=HarkenSearch::ExternalAiClientConfigPath(clients[clientIndex],homePath);
            const bool detected=HarkenSearch::ExternalAiClientDetected(clients[clientIndex],homePath);
            const auto linkState=HarkenSearch::QueryExternalAiClientLink(clients[clientIndex],configPath,linkTarget);
            const bool connected=linkState.state==HarkenSearch::ExternalAiLinkState::Connected;
            const float rowRight=ImGui::GetWindowContentRegionMax().x;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(clientLabels[clientIndex]);
            ImGui::SameLine(rowRight-236.f);
            if (!detected) ImGui::TextDisabled("%s",L(ExtAi_ClientNotDetected));
            else if (connected) ImGui::TextDisabled("%s",L(ExtAi_ClientConnected));
            else if (linkState.state==HarkenSearch::ExternalAiLinkState::Failed) ImGui::TextDisabled("%s",L(ExtAi_ClientUnreadable));
            else ImGui::TextDisabled("%s",L(ExtAi_ClientDisconnected));
            ImGui::SameLine(rowRight-100.f);
            ImGui::BeginDisabled(!linkReady||!detected);
            if (aura::ui::ActionButton(connected?L(ExtAi_DisconnectBtn):L(ExtAi_ConnectBtn),ImVec2(96,28),
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
            if (!detected) {
                ImGui::SameLine();
                ImGui::BeginDisabled(!linkReady);
                if (aura::ui::SmallActionButton(L(Main_CopyBtn),aura::ui::ButtonVariant::Quiet)) {
                    ImGui::SetClipboardText(HarkenSearch::ExternalAiClientCommandLine(linkTarget).c_str());
                }
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        if (!page.linkStatus.empty() && ImGui::GetTime()<page.linkStatusUntil) {
            ImGui::PushTextWrapPos(0.0f); ImGui::TextDisabled("%s",page.linkStatus.c_str()); ImGui::PopTextWrapPos();
        }
        su::Observe("ext-ai-clients");
    }
}