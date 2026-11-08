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


auto DrawCurrentSettingsAvatar = [&](float size) {
    static GLuint texture=0;
    static unsigned loadedRevision=~0u;
    static std::string loadedPath;
    static bool loadedCustom=false;
    static double nextReadAttempt=0.0;
    const auto path=GetCustomAvatarPathUtf8();
    const bool changed=loadedRevision!=su::avatarImageRevision || loadedPath!=path || loadedCustom!=state.settings.hasCustomAvatar;
    const double now=ImGui::GetTime();
    if(changed || (state.settings.hasCustomAvatar && !texture && now>=nextReadAttempt)) {
        if(texture){glDeleteTextures(1,&texture);texture=0;}
        loadedRevision=su::avatarImageRevision;loadedPath=path;loadedCustom=state.settings.hasCustomAvatar;
        if(loadedCustom) {
            nextReadAttempt=now+1.0;
            std::vector<uint8_t> pixels;int w=0,h=0;
            if(LoadImageForCrop(path,w,h,pixels)) {
                glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
                glBindTexture(GL_TEXTURE_2D,0);
            }
        }
    }
    if(state.settings.hasCustomAvatar && texture)ImGui::Image((ImTextureID)(uintptr_t)texture,{size,size});
    else {
        const auto pos=ImGui::GetCursorScreenPos();ImGui::Dummy({size,size});
        RenderSettingsBuiltinAvatar(ImGui::GetWindowDrawList(),{pos.x+size*.5f,pos.y+size*.5f},size*.5f-2.f,
            ResolveSettingsBuiltinAvatarIndex(state.settings.diceBearAvatarIndex),false,false);
    }
    su::Observe("avatar-current");
};
