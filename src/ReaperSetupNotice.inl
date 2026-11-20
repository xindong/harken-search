static void DrawReaperSetupNotice(AppState& state) {
    if (!g_reaperSetupNoticePending || state.settings.reaperSetupNoticeDismissed ||
        state.startupNeedsDatabaseSetup || state.showSettingsDialog || state.showUpdateCenter ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;

    const auto* viewport = ImGui::GetMainViewport();
    const auto& theme = ThemeColors::current();
    const float width = std::min(378.f, viewport->WorkSize.x - 32.f);
    if (width <= 0.f) return;
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 16.f,
               viewport->WorkPos.y + viewport->WorkSize.y - 152.f),
        ImGuiCond_Always, ImVec2(1.f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.dialogBG);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.interfaceText);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.chromeBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 12.f));
    {
        if (ImGui::Begin("###HarkenReaperSetupNotice", nullptr, ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNavFocus)) {
            ReadingTextScope reading;
            ImGui::TextWrapped("%s", L(DawPrefs_Notice));
            harken::settings::Observe("reaper-setup-notice");
            const bool open = aura::ui::SmallActionButton(L(DawPrefs_GoToSettings));
            harken::settings::Observe("reaper-setup-go-settings");
            ImGui::SameLine();
            const bool dismiss = aura::ui::SmallActionButton(L(DawPrefs_NoticeAcknowledge));
            harken::settings::Observe("reaper-setup-dismiss");
            if (open || dismiss) {
                g_reaperSetupNoticePending = false;
                state.settings.reaperSetupNoticeDismissed = true;
                MarkSettingsDirty(state);
                if (open) {
                    state.openDawIntegrationSettings = true;
                    state.showSettingsDialog = true;
                }
            }
        }
        ImGui::End();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
}
