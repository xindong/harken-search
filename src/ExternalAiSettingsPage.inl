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
        ImGui::SameLine();
        if (!page.status.empty() && ImGui::GetTime()<page.statusUntil) ImGui::TextDisabled("%s",page.status.c_str());
        else if (page.dirty) ImGui::TextDisabled("%s",L(ExtAi_Unsaved));
        if (!page.error.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::TextWrapped("%s",page.error.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::Dummy(ImVec2(0,4));
        heading(L(ExtAi_Connect));
        ImGui::TextWrapped("%s",L(ExtAi_ConnectHint));
        if (page.settings.roots.empty()) {
            ImGui::TextDisabled("%s",L(ExtAi_NoRoots));
        } else {
            const std::string command="codex mcp add harken -- \""+GetExeDirUtf8()+"\\HarkenExternalAiMcp.exe\" --library-db \""+databasePath+
                "\" --library-root \""+page.settings.roots.front()+"\" --settings \""+page.documentPath+"\" --library-id "+page.libraryId;
            ImGui::PushTextWrapPos(0.0f); ImGui::TextUnformatted(command.c_str()); ImGui::PopTextWrapPos();
            if (aura::ui::SmallActionButton(L(Main_CopyBtn))) ImGui::SetClipboardText(command.c_str());
            su::Observe("ext-ai-command");
        }
    }
}