// Existing native specialty UI, hosted by the settings action dialog.
                auto& diag = DiagnosticsLogger::instance();

                if (state.settings.diagnosticMode) {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), L(Set_DiagnosticsEnabled));
                    ImGui::Text(L(Set_DiagCollected), (int)diag.entryCount());
                    ImGui::Text(L(Set_DiagFPS), diag.currentFps());
                    bool showDiagOverlay = state.settings.showDiagnosticOverlay;
                    if (aura::ui::Checkbox(L(Set_DiagShowOverlay), &showDiagOverlay)) {
                        state.settings.showDiagnosticOverlay = showDiagOverlay;
                        MarkSettingsDirty(state);
                    }
                    if (HoveredForHelp()) {
                        aura::ui::Tooltip(L(Set_DiagOverlayHint));
                    }
                    // 分项监听：只记录一种类型，避免缓冲被多种日志占满
                    {
                        static const char* const kDiagCategories[] = {
                            "", "CoverWall", "Waveform", "Database", "Search", "Cover", "Memory", "Frame",
                            "UI", "Audio", "Playlist", "Filter", "Export", "Diagnostics", "Pointer"
                        };
                        static const char* const kDiagCategoryLabels[] = {
                            u8"全部", u8"CoverWall", u8"Waveform", u8"Database", u8"Search", u8"Cover",
                            u8"Memory", u8"Frame", u8"UI", u8"Audio", u8"Playlist", u8"Filter", u8"Export",
                            u8"Diagnostics", u8"Pointer"
                        };
                        const int nCat = (int)(sizeof(kDiagCategories) / sizeof(kDiagCategories[0]));
                        int currentIdx = 0;
                        for (int i = 0; i < nCat; i++) {
                            if (state.settings.diagnosticListenCategory == kDiagCategories[i]) {
                                currentIdx = i;
                                break;
                            }
                        }
                        ImGui::SetNextItemWidth(180);
                        if (ImGui::Combo(L(Set_DiagFilterType), &currentIdx, kDiagCategoryLabels, nCat)) {
                            state.settings.diagnosticListenCategory = kDiagCategories[currentIdx];
                            diag.setCategoryFilter(state.settings.diagnosticListenCategory);
                            MarkSettingsDirty(state);
                        }
                        if (HoveredForHelp()) {
                            aura::ui::Tooltip(L(Set_DiagFilterHint));
                        }
                    }
                    ImGui::Spacing();
                    ImGui::Text("%s", L(Set_DiagBufferSize));
                    ImGui::Separator();
                    ImGui::TextDisabled("%s", L(Set_DiagBufferHint));

                    ImGui::Spacing();
                    ImGui::Text("%s", L(Set_DiagExportMgmt));
                    ImGui::Separator();

                    if (aura::ui::ActionButton(L(Set_DiagExportJSON), ImVec2(180, 0))) {
                        // Export to exe directory
                        std::string exportPath = GetExeDirUtf8() + "\\aura_diagnostics.json";
                        if (diag.exportJson(exportPath)) {
                            Platform::OpenInFileManager(exportPath);
                        }
                    }
                    if (HoveredForHelp()) {
                        aura::ui::Tooltip(L(Set_DiagExportHint));
                    }

                    ImGui::SameLine();
                    if (aura::ui::ActionButton(L(Set_DiagCopyClipboard), ImVec2(140, 0))) {
                        std::string json = diag.exportJsonString();
                        ImGui::SetClipboardText(json.c_str());
                    }
                    if (HoveredForHelp()) {
                        aura::ui::Tooltip(L(Set_DiagCopyHint));
                    }

                    ImGui::SameLine();
                    if (aura::ui::ActionButton(L(Set_DiagClearLog), ImVec2(100, 0), aura::ui::ButtonVariant::Danger)) {
                        diag.clear();
                    }
                } else {
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", L(Set_DiagDisabledHint1));
                    ImGui::TextDisabled("%s", L(Set_DiagDisabledHint2));
                    ImGui::TextDisabled("%s", L(Set_DiagDisabledHint3));
                }
