// Existing native specialty UI, hosted by the settings action dialog.
                LoadThesaurusJson();
                /* ThesaurusContent: settings page uses dialog popup scroll; no Child host */
                aura::ui::BeginSettingsGroup("##ThesaurusPage");

                ImGui::TextColored(COLOR_ACCENT, L(Set_ThesaurusTitle));
                aura::ui::NotifySettingsSearchLabeledItem(L(Set_ThesaurusTitle), aura::ui::SettingsSearchLabelKind::Text);
                std::string thesaurusPath = Platform::HarkenUserData::WideToUtf8Path(Platform::HarkenUserData::EnsureThesaurusFileUtf8(GetExeDirUtf8()).activePath);
#if defined(__APPLE__)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped(L(Set_ThesMacPath), thesaurusPath.c_str());
                ImGui::PopStyleColor();
#else
                ImGui::TextDisabled(L(Set_ThesPath), GetExeDirUtf8().c_str());
#endif
                if (!PathExistsUtf8(thesaurusPath)) {
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f), L(Set_ThesaurusNotReady));
#if defined(__APPLE__)
                    ImGui::TextWrapped("%s", L(Set_ThesMacNotWritable));
#else
                    ImGui::TextWrapped("%s", L(Set_ThesNotWritable));
#endif
                }
                ImGui::Separator();
                ImGui::Spacing();

                if (aura::ui::ActionButton(L(Set_ThesOpenJSON), ImVec2(160, 30))) {
                    RevealFileInExplorer(thesaurusPath);
                }
                if (HoveredForHelp()) aura::ui::Tooltip(L(Set_ThesShareHint));

                ImGui::SameLine();
                if (aura::ui::ActionButton(L(Set_ThesMerge), ImVec2(160, 30))) {
                    #if defined(_WIN32) || defined(__APPLE__)
                    const std::string importPath =
                        Platform::ThesaurusDialogs::OpenThesaurusJsonFileDialogUtf8(MainUiOwnerWindow());
                    if (!importPath.empty()) {
                        std::string json;
                        if (Platform::FileSystem::ReadFileText(importPath, json)) {
                            std::vector<ThesaurusGroup> imported;
                            ParseThesaurusJson(json, imported);
                            // 将他人的词库无缝追加到自己的后面
                            g_userThesaurus.insert(g_userThesaurus.end(), imported.begin(), imported.end());
                            SaveThesaurusJson();
                        }
                    }
                    #endif
                }
                ImGui::SameLine();
                if (aura::ui::ActionButton(L(Set_ThesResetDefault), ImVec2(100, 30))) {
#if defined(__APPLE__)
                    if (ResetMacThesaurusToDefaults()) editor.error.clear();
                    else editor.error = L(Set_ThesMacNotWritable);
#else
                    g_userThesaurusLoaded = false;
                    {
                        const std::string exe = GetExeDirUtf8();
                        Platform::FileSystem::RemoveFile(Platform::HarkenUserData::JoinExeFileUtf8(exe, Platform::HarkenUserData::CanonicalThesaurusFileName()));
                        Platform::FileSystem::RemoveFile(Platform::HarkenUserData::JoinExeFileUtf8(exe, Platform::HarkenUserData::LegacyArisThesaurusFileName()));
                        Platform::FileSystem::RemoveFile(Platform::HarkenUserData::JoinExeFileUtf8(exe, Platform::HarkenUserData::LegacyAuroThesaurusFileName()));
                        Platform::FileSystem::RemoveFile(Platform::HarkenUserData::JoinExeFileUtf8(exe, Platform::HarkenUserData::ThesaurusMigrationMarkerFileName()));
                        Platform::HarkenUserData::ResetThesaurusCacheForTests();
                    }
                    LoadThesaurusJson();
#endif
                }

                ImGui::Spacing();
                ImGui::Separator();

                static char trigBuf[256] = "";
                static char relBuf[512] = "";
                static int s_editingIndex = -1;  // -1 = 添加新规则;>=0 = 编辑该索引

                // Editing-mode hint banner (only when actively editing)
                if (s_editingIndex >= 0 && s_editingIndex < (int)g_userThesaurus.size()) {
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f),
                                       L(Set_ThesEditingHint), s_editingIndex + 1);
                } else if (s_editingIndex >= 0) {
                    // Stale index (rule deleted out from under us) — auto-reset.
                    s_editingIndex = -1;
                }

                ImGui::Text("%s", L(Set_ThesQuickAdd));
                const float ruleInputWidth=std::max(1.f,(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5f);
                ImGui::SetNextItemWidth(ruleInputWidth);
                HintAwareInputTextWithHint("##trig", L(Set_ThesTrigger), trigBuf, sizeof(trigBuf));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ruleInputWidth);
                HintAwareInputTextWithHint("##rel", L(Set_ThesExpand), relBuf, sizeof(relBuf));
                // BUG-20260912-02 并案: the edit-mode Cancel (L(Cancel)) shares the
                // footer's permanent Cancel text in the same Dlg_Settings window layer;
                // salt the whole add/edit control group. No copy changes.
                ImGui::PushID("thesaurus_row_edit");
                if (s_editingIndex < 0) {
                    // Add-new mode (existing behavior): one "Add" button.
                    if (aura::ui::ActionButton(L(Set_ThesAddBtn), ImVec2(80, 0))) {
                        ThesaurusGroup g;
                        g.triggers = SplitThesaurusWords(trigBuf);
                        g.related = SplitThesaurusWords(relBuf);
                        if (!g.triggers.empty() && !g.related.empty()) {
                            g_userThesaurus.insert(g_userThesaurus.begin(), g); // 新规则放在最前面优先匹配
                            SaveThesaurusJson();
                            memset(trigBuf, 0, sizeof(trigBuf));
                            memset(relBuf, 0, sizeof(relBuf));
                        }
                    }
                } else {
                    // Edit mode: "Save Changes" + "Cancel"
                    if (aura::ui::ActionButton(L(Set_ThesSaveEditBtn), ImVec2(0, 0), aura::ui::ButtonVariant::PrimarySearch)) {
                        ThesaurusGroup g;
                        g.triggers = SplitThesaurusWords(trigBuf);
                        g.related = SplitThesaurusWords(relBuf);
                        if (!g.triggers.empty() && !g.related.empty() &&
                            s_editingIndex < (int)g_userThesaurus.size()) {
                            g_userThesaurus[s_editingIndex] = std::move(g);  // 替换原位,顺序不变
                            SaveThesaurusJson();
                            memset(trigBuf, 0, sizeof(trigBuf));
                            memset(relBuf, 0, sizeof(relBuf));
                            s_editingIndex = -1;
                        }
                    }
                    su::Observe("thesaurus-save");
                    ImGui::SameLine();
                    if (aura::ui::ActionButton(L(Cancel), ImVec2(0, 0), aura::ui::ButtonVariant::Quiet)) {
                        memset(trigBuf, 0, sizeof(trigBuf));
                        memset(relBuf, 0, sizeof(relBuf));
                        s_editingIndex = -1;
                    }
                    su::Observe("thesaurus-cancel");
                }
                ImGui::PopID();  // BUG-20260912-02 并案

                ImGui::Spacing();
                ImGui::Text(L(Set_ThesCurrentRules), (int)g_userThesaurus.size());

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0, 0.2f));
                const float rulesListHeight = 220.0f;
                BeginTransientScrollbarChild("RulesList", ImVec2(0, rulesListHeight), true);
                if (BeginTransientScrollbarTable("ThesaurusRulesTable", 2,
                                      ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_NoSavedSettings)) {
                    ImGui::TableSetupColumn(L(RenRuleColText), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn(L(RenRuleColAction), ImGuiTableColumnFlags_WidthFixed, 150.0f);

                    for (int i = 0; i < (int)g_userThesaurus.size(); ++i) {
                        ImGui::PushID(i);

                        std::string tStr, rStr;
                        for (const auto& t : g_userThesaurus[i].triggers) tStr += t + " ";
                        for (const auto& r : g_userThesaurus[i].related) rStr += r + " ";

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushTextWrapPos();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", tStr.c_str());
                        ImGui::SameLine();
                        ImGui::TextDisabled(" => ");
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s", rStr.c_str());
                        ImGui::PopTextWrapPos();

                        ImGui::TableNextColumn();
                        // Highlight the row currently being edited.
                        const bool isEditingThisRow = (s_editingIndex == i);
                        if (aura::ui::ActionButton(L(Set_ThesEditBtn), ImVec2(65, 0),
                                isEditingThisRow ? aura::ui::ButtonVariant::Selected
                                                 : aura::ui::ButtonVariant::Secondary)) {
                            // Pre-fill the form with this rule's tokens (space-joined).
                            std::string t, r;
                            for (const auto& s : g_userThesaurus[i].triggers) { t += s; t += ' '; }
                            for (const auto& s : g_userThesaurus[i].related)  { r += s; r += ' '; }
                            std::snprintf(trigBuf, sizeof(trigBuf), "%s", t.c_str());
                            std::snprintf(relBuf,  sizeof(relBuf),  "%s", r.c_str());
                            s_editingIndex = i;
                        }
                        if(i==0)su::Observe("thesaurus-edit-first");
                        ImGui::SameLine();
                        if (aura::ui::ActionButton(L(Delete), ImVec2(-FLT_MIN, 0), aura::ui::ButtonVariant::Danger)) {
                            g_userThesaurus.erase(g_userThesaurus.begin() + i);
                            SaveThesaurusJson();
                            // Index drift handling: keep edit state in sync.
                            if (s_editingIndex == i) {
                                // The rule we were editing just got deleted — bail out of edit mode.
                                s_editingIndex = -1;
                                memset(trigBuf, 0, sizeof(trigBuf));
                                memset(relBuf, 0, sizeof(relBuf));
                            } else if (s_editingIndex > i) {
                                s_editingIndex--;
                            }
                            i--;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();

                aura::ui::EndSettingsGroup();
