// Existing native specialty UI, hosted by the settings action dialog.
                auto ResolveSettingsBuiltinAvatarIndex = [&](int explicitIndex) {
                    if (explicitIndex >= 1 && explicitIndex <= 36) return explicitIndex;
                    uint32_t h = 2166136261u;
                    const std::string seed = state.settings.userName.empty() ? std::string("Harken Search") : state.settings.userName;
                    for (unsigned char c : seed) {
                        h ^= c;
                        h *= 16777619u;
                    }
                    return 1 + (int)(h % 36u);
                };

                auto RenderSettingsBuiltinAvatar = [](ImDrawList* dl,
                                                      ImVec2 center,
                                                      float radius,
                                                      int avatarIndex,
                                                      bool hovered,
                                                      bool selected) {
                    const int idx = (std::max)(1, (std::min)(36, avatarIndex));
                    const float hue = (float)(idx - 1) / 36.0f;
                    float accentHue = hue + 0.09f;
                    if (accentHue > 1.0f) accentHue -= 1.0f;
                    float br = 0.0f, bg = 0.0f, bb = 0.0f;
                    float ar = 0.0f, ag = 0.0f, ab = 0.0f;
                    ImGui::ColorConvertHSVtoRGB(hue, 0.50f, hovered ? 0.70f : 0.56f, br, bg, bb);
                    ImGui::ColorConvertHSVtoRGB(accentHue, 0.34f, hovered ? 0.96f : 0.86f, ar, ag, ab);

                    const ImU32 bgCol = IM_COL32((int)(br * 255), (int)(bg * 255), (int)(bb * 255), 255);
                    const ImU32 faceCol = IM_COL32((int)(ar * 255), (int)(ag * 255), (int)(ab * 255), 255);
                    const ImU32 inkCol = IM_COL32(34, 42, 56, 230);
                    const ImU32 cheekCol = IM_COL32(255, 132, 150, 96);
                    const ImU32 hiCol = IM_COL32(255, 255, 255, hovered ? 92 : 58);

                    dl->AddCircleFilled(center, radius, bgCol, 48);
                    dl->AddCircleFilled(ImVec2(center.x - radius * 0.22f, center.y + radius * 0.04f), radius * 0.62f, faceCol, 40);
                    dl->AddCircleFilled(ImVec2(center.x + radius * 0.18f, center.y + radius * 0.02f), radius * 0.58f, faceCol, 40);

                    const float hairSweep = ((idx % 5) - 2) * radius * 0.08f;
                    dl->PathClear();
                    dl->PathArcTo(ImVec2(center.x + hairSweep, center.y - radius * 0.02f), radius * 0.70f, 3.50f, 5.95f, 18);
                    dl->PathLineTo(ImVec2(center.x + radius * 0.58f, center.y - radius * 0.08f));
                    dl->PathLineTo(ImVec2(center.x - radius * 0.56f, center.y + radius * 0.02f));
                    dl->PathFillConvex(IM_COL32(46, 48, 70, 222));

                    const float eyeY = center.y - radius * 0.02f;
                    const float eyeDx = radius * (0.20f + (idx % 3) * 0.025f);
                    dl->AddCircleFilled(ImVec2(center.x - eyeDx, eyeY), radius * 0.055f, inkCol, 12);
                    dl->AddCircleFilled(ImVec2(center.x + eyeDx, eyeY), radius * 0.055f, inkCol, 12);
                    dl->AddCircleFilled(ImVec2(center.x - radius * 0.34f, center.y + radius * 0.18f), radius * 0.10f, cheekCol, 12);
                    dl->AddCircleFilled(ImVec2(center.x + radius * 0.34f, center.y + radius * 0.18f), radius * 0.10f, cheekCol, 12);
                    if ((idx % 2) == 0) {
                        dl->AddBezierCubic(ImVec2(center.x - radius * 0.16f, center.y + radius * 0.23f),
                                           ImVec2(center.x - radius * 0.06f, center.y + radius * 0.34f),
                                           ImVec2(center.x + radius * 0.08f, center.y + radius * 0.34f),
                                           ImVec2(center.x + radius * 0.18f, center.y + radius * 0.23f),
                                           inkCol, 1.7f, 16);
                    } else {
                        dl->AddLine(ImVec2(center.x - radius * 0.13f, center.y + radius * 0.25f),
                                    ImVec2(center.x + radius * 0.13f, center.y + radius * 0.25f),
                                    inkCol, 1.6f);
                    }
                    dl->AddCircle(center, radius - 0.8f, hiCol, 48, 1.0f);
                    if (selected) {
                        dl->AddCircle(center, radius + 2.0f, IM_COL32(255, 255, 255, 230), 48, 2.2f);
                    }
                };

                static int s_pendingBuiltInAvatarIndex = 0;

                auto DrawSettingsAvatarPreview = [&](const CoverModule::TextureHandle& customTex) {
                    const ImVec2 size(64.0f, 64.0f);
                    if (state.settings.hasCustomAvatar && customTex.hasTexture) {
                        ImGui::Image((ImTextureID)(uintptr_t)customTex.textureId, size);
                        return;
                    }
                    const ImVec2 topLeft = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##SettingsAvatarActivePreview", size);
                    const ImVec2 center(topLeft.x + size.x * 0.5f, topLeft.y + size.y * 0.5f);
                    const int idx = ResolveSettingsBuiltinAvatarIndex(state.settings.diceBearAvatarIndex);
                    RenderSettingsBuiltinAvatar(ImGui::GetWindowDrawList(), center, size.x * 0.5f - 2.0f, idx,
                                                ImGui::IsItemHovered(), !state.settings.hasCustomAvatar);
                    if (state.settings.hasCustomAvatar && !customTex.hasTexture && ImGui::IsItemHovered()) {
                        aura::ui::Tooltip(L(Avatar_LoadingTooltip));
                    }
                };

// Custom avatar
                {
                    static CoverModule::TextureHandle s_avatarTex;
                    static bool                       s_avatarTexDirty = true;
                    static int                        s_avatarTexRetryFrames = 0;
                    static AvatarCropState            s_cropState;
                    static std::vector<uint8_t>       s_cropPixels;
                    static int                        s_cropSrcW = 0, s_cropSrcH = 0;
                    static GLuint                     s_cropPreviewGLTex = 0;

                    if (state.settings.hasCustomAvatar) {
                        if (s_avatarTexDirty) {
                            s_avatarTex = {};
                            s_avatarTexRetryFrames = 0;
                        }
                        // Existing avatar preview is backed by the same async image texture cache as covers.
                        // Keep retrying while the settings page is visible; otherwise the first pending frame
                        // can permanently leave this preview blank until another setting toggles dirty state.
                        if (s_avatarTexDirty || (!s_avatarTex.hasTexture && s_avatarTexRetryFrames < 240)) {
                            if (CoverModule::GetCoverTextureFromImagePath(GetCustomAvatarPathUtf8(), s_avatarTex, true)) {
                                s_avatarTexDirty = false;
                                s_avatarTexRetryFrames = 0;
                            } else {
                                ++s_avatarTexRetryFrames;
                            }
                        }
                    } else {
                        s_avatarTex = {};
                        s_avatarTexDirty = false;
                        s_avatarTexRetryFrames = 0;
                    }

                    DrawSettingsAvatarPreview(s_avatarTex);

                    ImGui::SameLine();
                    ImGui::BeginGroup();

                    if (aura::ui::ActionButton(L(Set_AvatarChangeLabelZh))) {
                        std::string selected =
                            Platform::AvatarDialogs::OpenAvatarImageFileDialogUtf8(MainUiOwnerWindow());
                        if (!selected.empty()) {
                            s_cropPixels.clear();
                            if (LoadImageForCrop(selected, s_cropSrcW, s_cropSrcH, s_cropPixels)) {
                                s_cropState = InitCropState(s_cropSrcW, s_cropSrcH);
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

                    if (state.settings.hasCustomAvatar) {
                        ImGui::SameLine();
                        if (aura::ui::ActionButton(L(Set_AvatarRemoveLabelZh))) {
                            const std::string avatarPath = GetCustomAvatarPathUtf8();
                            Platform::FileSystem::RemoveFile(avatarPath);
                            CoverModule::EvictCoverTextureByImagePath(avatarPath);
                            state.settings.hasCustomAvatar = false;
                            s_avatarTexDirty = true;
                            s_avatarTexRetryFrames = 0;
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
                        constexpr float kPreviewSize = 256.f;

                        if (s_cropPreviewGLTex != 0 && s_cropSrcW > 0) {
                            const AvatarSourceRect r = ComputeSourceRect(s_cropState);
                            const float u0 = (float)r.x / s_cropSrcW;
                            const float v0 = (float)r.y / s_cropSrcH;
                            const float u1 = (float)(r.x + r.w) / s_cropSrcW;
                            const float v1 = (float)(r.y + r.h) / s_cropSrcH;

                            ImGui::Image((ImTextureID)(uintptr_t)s_cropPreviewGLTex,
                                         {kPreviewSize, kPreviewSize},
                                         {u0, v0}, {u1, v1});

                            if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                                const float scale = (1.0f / s_cropState.zoom) / kPreviewSize;
                                s_cropState.panX -= delta.x * scale;
                                s_cropState.panY -= delta.y * scale;
                                ClampCropState(s_cropState);
                            }

                            if (ImGui::IsItemHovered()) {
                                const float wheel = ImGui::GetIO().MouseWheel;
                                if (wheel != 0.0f) {
                                    s_cropState.zoom = std::max(
                                        ComputeMinZoom(s_cropSrcW, s_cropSrcH),
                                        s_cropState.zoom * (1.0f + wheel * 0.1f));
                                    ClampCropState(s_cropState);
                                }
                            }
                        }

                        if (aura::ui::ActionButton(L(Main_SaveBtn2), ImVec2(0, 0), aura::ui::ButtonVariant::PrimarySearch)) {
                            const std::string dest = GetCustomAvatarPathUtf8();
                            if (SaveCroppedAvatarPng(s_cropPixels, s_cropSrcW, s_cropSrcH,
                                                      s_cropState, dest, 512)) {
                                CoverModule::EvictCoverTextureByImagePath(dest);
                                state.settings.hasCustomAvatar = true;
                                s_avatarTexDirty = true;
                                s_avatarTexRetryFrames = 0;
                                MarkSettingsDirty(state);
                            }
                            s_cropPixels.clear();
                            if (s_cropPreviewGLTex) { glDeleteTextures(1, &s_cropPreviewGLTex); s_cropPreviewGLTex = 0; }
                            s_cropSrcW = s_cropSrcH = 0;
                            s_cropState = {};
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (aura::ui::ActionButton(L(Main_CancelBtn2), ImVec2(0, 0), aura::ui::ButtonVariant::Quiet)) {
                            s_cropPixels.clear();
                            if (s_cropPreviewGLTex) { glDeleteTextures(1, &s_cropPreviewGLTex); s_cropPreviewGLTex = 0; }
                            s_cropSrcW = s_cropSrcH = 0;
                            s_cropState = {};
                            ImGui::CloseCurrentPopup();
                        }

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
