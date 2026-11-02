// Existing native specialty UI, hosted by the settings action dialog.
                {
                    std::string exeDir = GetExeDirUtf8();
                    std::string devModePath = exeDir + "\\dev_mode.txt";
                    bool devModeExists = PathExistsUtf8(devModePath);
                    static std::string s_publishUserDefaultsStatus;
                    static bool s_publishUserDefaultsOk = false;
                    if (devModeExists) {
                        ImGui::TextColored(mixVec(cs.volumeColor, cs.metadataPanelText, 0.18f), L(Set_DevModeEnabled));
                        ImGui::TextDisabled("%s", L(Set_DevDebugHint));
                        if (aura::ui::ActionButton(L(Set_PublishUserDefaults), ImVec2(220, 0))) {
                            s_publishUserDefaultsOk = PublishCurrentDevConfigToUserDefaults(state, &s_publishUserDefaultsStatus);
                        }
                        if (HoveredForHelp()) {
                            aura::ui::Tooltip(L(Set_PublishUserDefaultsHint));
                        }
                        if (!s_publishUserDefaultsStatus.empty()) {
                            ImGui::TextColored(
                                s_publishUserDefaultsOk
                                    ? mixVec(cs.volumeColor,     cs.metadataPanelText, 0.18f)
                                    : mixVec(cs.volumeClipColor, cs.metadataPanelText, 0.16f),
                                "%s", s_publishUserDefaultsStatus.c_str());
                        }
                        if (aura::ui::ActionButton(L(Set_DeleteDevMode), ImVec2(260, 0))) {
                            Platform::FileSystem::RemoveFile(devModePath);
                        }
                    } else {
                        ImGui::TextDisabled("%s", L(Set_DevModeHint));
                        if (aura::ui::ActionButton(L(Set_CreateDevMode), ImVec2(220, 0))) {
                            Platform::FileSystem::WriteFileText(devModePath, "# Aris dev mode\n");
                        }
                    }
                    if (aura::ui::ActionButton(L(Set_OpenExeDir), ImVec2(220, 0))) {
                        RevealFileInExplorer(Platform::HarkenUserData::JoinExeFileUtf8(exeDir, Platform::HarkenUserData::CanonicalThesaurusFileName()));
                    }
                    if (HoveredForHelp()) aura::ui::Tooltip(L(Set_OpenExeDirHint));
                }
