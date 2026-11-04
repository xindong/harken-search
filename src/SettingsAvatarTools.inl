                static int s_pendingBuiltInAvatarIndex = 0;

// Custom avatar
                {
                    static AvatarCropState            s_cropState;
                    static std::vector<uint8_t>       s_cropPixels;
                    static int                        s_cropSrcW = 0, s_cropSrcH = 0;
                    static GLuint                     s_cropPreviewGLTex = 0;
                    static bool s_cropSaveFailed = false;

                    DrawCurrentSettingsAvatar(64.f);

                    ImGui::SameLine();
                    ImGui::BeginGroup();

                    if (aura::ui::ActionButton(L(Set_AvatarChangeLabelZh))) {
                        std::string selected =
                            Platform::AvatarDialogs::OpenAvatarImageFileDialogUtf8(g_hwnd);
                        if (!selected.empty()) {
                            s_cropPixels.clear();
                            if (LoadImageForCrop(selected, s_cropSrcW, s_cropSrcH, s_cropPixels)) {
                                s_cropState = InitCropState(s_cropSrcW, s_cropSrcH);
                                s_cropSaveFailed = false;
                                if (s_cropPreviewGLTex) { glDeleteTextures(1, &s_cropPreviewGLTex); s_cropPreviewGLTex = 0; }
                                glGenTextures(1, &s_cropPreviewGLTex);
                                glBindTexture(GL_TEXTURE_2D, s_cropPreviewGLTex);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_cropSrcW, s_cropSrcH, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_cropPixels.data());
                                glBindTexture(GL_TEXTURE_2D, 0);
                                ImGui::OpenPopup(L(AvatarCropTitle));
                            }
                        }
                    }

                    su::Observe("avatar-change");

                    if (state.settings.hasCustomAvatar) {
                        ImGui::SameLine();
                        if (aura::ui::ActionButton(L(Set_AvatarRemoveLabelZh))) {
                            const std::string avatarPath = GetCustomAvatarPathUtf8();
                            Platform::FileSystem::RemoveFile(avatarPath);
                            CoverModule::EvictCoverTextureByImagePath(avatarPath);
                            state.settings.hasCustomAvatar = false;
                            ++su::avatarImageRevision;
                            MarkSettingsDirty(state);
                        }
                    }
                    const std::string settingsAvatarPickerOpenLabel =
                        std::string(L(Set_AvatarPickerOpenLabelZh)) + "##settings-avatar-picker-open";
                    if (aura::ui::ActionButton(settingsAvatarPickerOpenLabel.c_str())) {
                        s_pendingBuiltInAvatarIndex = ResolveSettingsBuiltinAvatarIndex(state.settings.diceBearAvatarIndex);
                        ImGui::OpenPopup(L(Set_AvatarPickerTitleZh));
                    }

                    ImGui::EndGroup();

                    if (BeginTransientScrollbarPopupModal(L(AvatarCropTitle), nullptr,
                            ImGuiWindowFlags_AlwaysAutoResize)) {
                        const float available = std::max(180.f, std::min(560.f,ImGui::GetIO().DisplaySize.x-96.f));
                        const float kPreviewSize = std::min(320.f,available-108.f);
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+available);
                        ImGui::TextWrapped("%s",L(AvatarCropHelp));
                        ImGui::PopTextWrapPos();
                        ImGui::Spacing();
                        if (s_cropPreviewGLTex != 0 && s_cropSrcW > 0) {
                            const ImVec2 origin=ImGui::GetCursorScreenPos();
                            ImGui::InvisibleButton("##avatar-crop-canvas",{kPreviewSize,kPreviewSize});
                            su::Observe("avatar-canvas");
                            if(ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left,0.f)) {
                                const auto delta=ImGui::GetIO().MouseDelta;
                                s_cropState.panX-=delta.x/(s_cropState.zoom*kPreviewSize);
                                s_cropState.panY-=delta.y/(s_cropState.zoom*kPreviewSize);
                            }
                            if(ImGui::IsItemHovered()) {
                                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                                s_cropState.zoom*=std::pow(1.1f,ImGui::GetIO().MouseWheel);
                            }
                            ClampCropState(s_cropState);
                            const auto r=ComputeSourceRect(s_cropState);
                            const ImVec2 uv0((float)r.x/s_cropSrcW,(float)r.y/s_cropSrcH);
                            const ImVec2 uv1((float)(r.x+r.w)/s_cropSrcW,(float)(r.y+r.h)/s_cropSrcH);
                            auto* dl=ImGui::GetWindowDrawList();
                            const auto tex=(ImTextureID)(uintptr_t)s_cropPreviewGLTex;
                            dl->AddImage(tex,origin,{origin.x+kPreviewSize,origin.y+kPreviewSize},uv0,uv1);
                            dl->AddRect(origin,{origin.x+kPreviewSize,origin.y+kPreviewSize},ImGui::GetColorU32(cs.chromeBorder),0,0,1.f);
                            ImGui::SameLine(0,16);ImGui::BeginGroup();
                            ImGui::TextUnformatted(L(AvatarCropPreview));
                            const auto previewPos=ImGui::GetCursorScreenPos();
                            dl->AddImageRounded(tex,previewPos,{previewPos.x+80,previewPos.y+80},uv0,uv1,IM_COL32_WHITE,40.f);
                            ImGui::Dummy({80,80});
                            ImGui::Image(tex,{48,48},uv0,uv1);
                            ImGui::EndGroup();
                            ImGui::SetNextItemWidth(kPreviewSize);
                            if(ImGui::SliderFloat(L(AvatarCropZoom),&s_cropState.zoom,1.f,8.f,"%.1fx",ImGuiSliderFlags_AlwaysClamp))ClampCropState(s_cropState);
                            su::Observe("avatar-zoom");
                            if(aura::ui::ActionButton(L(AvatarCropReset)))s_cropState=InitCropState(s_cropSrcW,s_cropSrcH);
                            su::Observe("avatar-reset");
                        }
                        if(s_cropSaveFailed){ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+available);ImGui::TextWrapped("%s",L(AvatarCropSaveFailed));ImGui::PopTextWrapPos();su::Observe("avatar-save-error");}

                        auto finishCrop=[&](){
                            s_cropPixels.clear();
                            if(s_cropPreviewGLTex){glDeleteTextures(1,&s_cropPreviewGLTex);s_cropPreviewGLTex=0;}
                            s_cropSrcW=s_cropSrcH=0;s_cropState={};s_cropSaveFailed=false;
                            ImGui::CloseCurrentPopup();
                        };
                        if(aura::ui::ActionButton(L(Main_SaveBtn2),ImVec2(100,0),aura::ui::ButtonVariant::PrimarySearch)) {
                            const auto dest=GetCustomAvatarPathUtf8();
                            const bool previousCustom=state.settings.hasCustomAvatar;
                            if(SaveCroppedAvatarWithSettings(s_cropPixels,s_cropSrcW,s_cropSrcH,s_cropState,dest,[&] {
                                state.settings.hasCustomAvatar=true;
                                return TrySaveAppSettings(state);
                            },512)) {
                                CoverModule::EvictCoverTextureByImagePath(dest);
                                ++su::avatarImageRevision;
                                finishCrop();
                            } else {
                                state.settings.hasCustomAvatar=previousCustom;
                                s_cropSaveFailed=true;
                            }
                        }
                        su::Observe("avatar-save");ImGui::SameLine();
                        if(aura::ui::ActionButton(L(Main_CancelBtn2),ImVec2(100,0),aura::ui::ButtonVariant::Quiet))finishCrop();
                        su::Observe("avatar-cancel");

                        ImGui::EndPopup();
                    }
                }

                // Built-in avatar picker is intentionally modal: keep the settings page compact,
                // and show a larger pending preview that uses the same renderer as the cells.
                {
                    ImGui::SeparatorText(L(Set_BuiltinAvatarLabelZh));
                    const int activeBuiltInIdx = ResolveSettingsBuiltinAvatarIndex(state.settings.diceBearAvatarIndex);
                    if (state.settings.hasCustomAvatar) {
                        ImGui::TextDisabled("%s", L(Set_AvatarCustomPriorityLabelZh));
                    } else if (state.settings.diceBearAvatarIndex == 0) {
                        ImGui::TextDisabled("%s", L(Set_AvatarAutoLabelZh));
                    } else {
                        ImGui::TextDisabled(L(Set_AvatarStatusLabelZh), state.settings.diceBearAvatarIndex);
                    }
                    ImGui::SameLine(0.0f, 12.0f);
                    const std::string settingsAvatarPickerOpenSmallLabel =
                        std::string(L(Set_AvatarPickerOpenLabelZh)) + "##settings-avatar-picker-open-small";
                    if (aura::ui::SmallActionButton(settingsAvatarPickerOpenSmallLabel.c_str())) {
                        s_pendingBuiltInAvatarIndex = activeBuiltInIdx;
                        ImGui::OpenPopup(L(Set_AvatarPickerTitleZh));
                    }
                    ImGui::SameLine();
                    if (!state.settings.hasCustomAvatar && state.settings.diceBearAvatarIndex != 0) {
                        if (aura::ui::SmallActionButton(L(Set_AvatarResetAutoLabelZh))) {
                            state.settings.diceBearAvatarIndex = 0;
                            MarkSettingsDirty(state);
                        }
                    }

                    ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_Appearing);
                    if (BeginTransientScrollbarPopupModal(L(Set_AvatarPickerTitleZh), nullptr, ImGuiWindowFlags_NoResize)) {
                        constexpr float kSettingsAvatarPickerCellSize = 72.0f;
                        constexpr float kSettingsAvatarPickerCellGap = 12.0f;
                        constexpr float kSettingsAvatarPendingPreviewSize = 104.0f;

                        ImGui::TextUnformatted(L(Set_AvatarPickerTitleZh));
                        ImGui::Spacing();

                        ImGui::BeginGroup();
                        const ImVec2 pendingTopLeft = ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##PendingBuiltInAvatarPreview", ImVec2(kSettingsAvatarPendingPreviewSize, kSettingsAvatarPendingPreviewSize));
                        const ImVec2 pendingPreviewCenter(pendingTopLeft.x + kSettingsAvatarPendingPreviewSize * 0.5f,
                                                          pendingTopLeft.y + kSettingsAvatarPendingPreviewSize * 0.5f);
                        RenderSettingsBuiltinAvatar(ImGui::GetWindowDrawList(), pendingPreviewCenter,
                                                    kSettingsAvatarPendingPreviewSize * 0.5f - 3.0f,
                                                    s_pendingBuiltInAvatarIndex,
                                                    ImGui::IsItemHovered(), true);
                        ImGui::EndGroup();
                        ImGui::SameLine(0.0f, 18.0f);
                        ImGui::BeginGroup();
                        ImGui::TextDisabled("%s", L(Set_AvatarPickerActualLabelZh));
                        ImGui::Text(L(Set_AvatarStatusLabelZh), s_pendingBuiltInAvatarIndex);
                        ImGui::Spacing();
                        if (aura::ui::ActionButton(L(Set_AvatarPickerUseLabelZh), ImVec2(130.0f, 0.0f), aura::ui::ButtonVariant::PrimarySearch)) {
                            state.settings.hasCustomAvatar = false;
                            ++su::avatarImageRevision;
                            state.settings.diceBearAvatarIndex = s_pendingBuiltInAvatarIndex;
                            MarkSettingsDirty(state);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (aura::ui::ActionButton(L(Cancel), ImVec2(90.0f, 0.0f), aura::ui::ButtonVariant::Quiet)) {
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndGroup();

                        ImGui::Separator();
                        const float gridStartX = ImGui::GetCursorPosX();
                        for (int i = 1; i <= 36; ++i) {
                            const int col = (i - 1) % 6;
                            if (col == 0) {
                                ImGui::SetCursorPosX(gridStartX);
                            } else {
                                ImGui::SameLine(0.0f, kSettingsAvatarPickerCellGap);
                            }
                            ImGui::PushID(i);
                            const ImVec2 topLeft = ImGui::GetCursorScreenPos();
                            if (ImGui::InvisibleButton("##avModal", ImVec2(kSettingsAvatarPickerCellSize, kSettingsAvatarPickerCellSize))) {
                                s_pendingBuiltInAvatarIndex = i;
                            }
                            const bool hov = ImGui::IsItemHovered();
                            const bool pendingSel = s_pendingBuiltInAvatarIndex == i;
                            const ImVec2 center(topLeft.x + kSettingsAvatarPickerCellSize * 0.5f,
                                                topLeft.y + kSettingsAvatarPickerCellSize * 0.5f);
                            const float radius = kSettingsAvatarPickerCellSize * 0.5f - 2.0f;
                            RenderSettingsBuiltinAvatar(ImGui::GetWindowDrawList(), center, radius, i, hov, pendingSel);
                            if (hov) {
                                char avatarTip[64];
                                std::snprintf(avatarTip, sizeof(avatarTip), L(Set_AvatarStatusLabelZh), i);
                                aura::ui::Tooltip(avatarTip);
                            }                            ImGui::PopID();
                        }

                        ImGui::EndPopup();
                    }
                }
                ImGui::Spacing();
