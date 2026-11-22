
// DAWPanel.cpp — "Send to DAW" capsule widget + custom dropdown

// P2: Adaptive capsule [icon] Name [status dot] [Send] [v]
// P3: Dropdown with secondary capability line, optical icon sizing
// Reads from AppState.settings.dawTargets (user-configured).

#include "DAWPanel.h"
#include "AppState.h"
#include "HarkenSearchWidgets.h"
#include "Localization.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <string>

// Per-DAW vector icons — optical size normalization

void DrawDAWIcon(ImDrawList* dl, ImVec2 center, const std::string& dawName, ImU32 color) {
    std::string lower = dawName;
    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);

    // Per-icon optical scale (tuned so all look same visual weight at ~14px)
    float S = 0.56f;
    float T = 1.2f;

    auto P = [&](float x, float y) -> ImVec2 {
        return {center.x + (x - 12.0f) * S, center.y + (y - 12.0f) * S};
    };

    if (lower.find("reaper") != std::string::npos) {
        S = 0.58f;
        dl->AddCircle(center, 9.5f*S, color, 24, T);
        dl->AddBezierCubic(P(12,4), P(6,4), P(3,9), P(5,14), color, T);
        dl->AddBezierCubic(P(5,14), P(7,18), P(11,20), P(12,20), color, T);
        dl->AddBezierCubic(P(12,20), P(10,16), P(9,13), P(12,10), color, T);
        dl->AddLine(P(12,4), P(12,10), color, T);
        dl->AddLine(P(8,8), P(12,10), color, T * 0.8f);
    }
    else if (lower.find("pro tools") != std::string::npos || lower.find("protools") != std::string::npos) {
        S = 0.55f;
        dl->AddCircle(center, 10.0f*S, color, 20, T);
        dl->AddLine(P(8,17), P(12,7), color, T);
        dl->AddLine(P(12,7), P(16,17), color, T);
        dl->AddLine(P(9.5f,13), P(14.5f,13), color, T);
    }
    else if (lower.find("cubase") != std::string::npos || lower.find("nuendo") != std::string::npos) {
        S = 0.52f;
        ImVec2 pts[6] = {P(12,2), P(21,7), P(21,17), P(12,22), P(3,17), P(3,7)};
        dl->AddPolyline(pts, 6, color, ImDrawFlags_Closed, T);
        dl->AddCircle(center, 3.5f*S, color, 10, T);
    }
    else if (lower.find("logic") != std::string::npos) {
        S = 0.56f;
        dl->AddRect(P(3,3), P(21,21), color, 4.0f*S, 0, T);
        dl->AddCircle(center, 5.0f*S, color, 14, T);
        dl->AddCircleFilled(center, 1.8f*S, color, 8);
    }
    else if (lower.find("ableton") != std::string::npos) {
        S = 0.58f;
        dl->AddRect(P(3,3), P(21,21), color, 3.0f*S, 0, T);
        for (int i = 0; i < 4; ++i) {
            float x = 7.0f + i * 3.4f;
            dl->AddLine(P(x,7), P(x,17), color, T);
        }
    }
    else if (lower.find("fender") != std::string::npos) {
        // Simplified: box + single wave + F stem only
        S = 0.54f;
        dl->AddRect(P(3,3), P(21,21), color, 4.0f*S, 0, T);
        dl->AddBezierQuadratic(P(3,12), P(12,6), P(21,12), color, T*0.9f);
        dl->AddLine(P(8,8), P(8,17), color, T);
        dl->AddLine(P(8,8), P(14,8), color, T);
        dl->AddLine(P(8,12), P(12,12), color, T);
    }
    else if (lower.find("fl studio") != std::string::npos || lower.find("flstudio") != std::string::npos) {
        // Simplified: stem + body outline + one center leaf
        S = 0.52f;
        dl->AddBezierCubic(P(12,6), P(13,3), P(15,2), P(17,2), color, T);
        dl->AddBezierCubic(P(7,11), P(5,16), P(7,21), P(11,22), color, T);
        dl->AddBezierCubic(P(11,22), P(15,23), P(18,17), P(17,11), color, T);
        dl->AddBezierCubic(P(12,13), P(9.5f,12), P(9,8), P(12,6), color, T);
        dl->AddBezierCubic(P(12,6), P(15,8), P(14.5f,12), P(12,13), color, T);
    }
    else {
        dl->AddCircle(center, 5.0f*S, color, 12, T);
    }
}

// Small helpers

static void DrawPaperPlane(ImDrawList* dl, ImVec2 c, ImU32 col, float s = 1.f) {
    ImVec2 pts[3] = {{c.x-5*s,c.y-4*s},{c.x+5*s,c.y},{c.x-5*s,c.y+4*s}};
    dl->AddTriangleFilled(pts[0], pts[1], pts[2], col);
    dl->AddLine({c.x-5*s,c.y+4*s}, {c.x-1*s,c.y+.5f*s}, col, 1.0f);
}
static void DrawCheckmark(ImDrawList* dl, ImVec2 c, ImU32 col) {
    dl->AddLine({c.x-3.5f,c.y+.5f}, {c.x-.5f,c.y+3.f}, col, 1.6f);
    dl->AddLine({c.x-.5f,c.y+3.f}, {c.x+4.5f,c.y-3.f}, col, 1.6f);
}
static void DrawChevronDown(ImDrawList* dl, ImVec2 c, ImU32 col, float s = 1.f, float stroke = 1.3f) {
    dl->AddLine({c.x-3*s,c.y-1.5f*s}, {c.x,c.y+1.5f*s}, col, stroke);
    dl->AddLine({c.x,c.y+1.5f*s}, {c.x+3*s,c.y-1.5f*s}, col, stroke);
}

// Status dot color
static ImU32 GetStatusDotColor(const DawTarget& dt) {
    auto cap = GetDawCapability(dt);
    switch (cap) {
    case DawCapability::CanDirect:     return IM_COL32(80, 210, 80, 255);  // green
    case DawCapability::Experimental:  return IM_COL32(80, 210, 80, 255);
    case DawCapability::PasteOnly:     return IM_COL32(80, 210, 80, 180);  // light green
    case DawCapability::CanLaunch:     return IM_COL32(160, 160, 160, 200); // gray
    case DawCapability::ClipboardOnly: return IM_COL32(160, 160, 160, 160); // dim gray
    case DawCapability::AdapterMissing:return IM_COL32(220, 180, 60, 200);  // yellow
    case DawCapability::HostNotReady:  return IM_COL32(220, 180, 60, 200);  // yellow
    case DawCapability::BridgeOffline: return IM_COL32(220, 180, 60, 200);  // yellow
    case DawCapability::Unavailable:   return IM_COL32(210, 80, 80, 200);   // red
    case DawCapability::Unconfigured:  return IM_COL32(220, 180, 60, 200);  // yellow
    }
    return IM_COL32(160, 160, 160, 160);
}

static bool IsSupportedDawDisplayTarget(const DawTarget& target) {
    std::string name = target.name;
    std::string process = target.processName;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    std::transform(process.begin(), process.end(), process.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return name.find("reaper") != std::string::npos ||
           name.find("pro tools") != std::string::npos ||
           name.find("protools") != std::string::npos ||
           process.find("protools") != std::string::npos ||
           name.find("nuendo") != std::string::npos ||
           process.find("nuendo") != std::string::npos ||
           name.find("cubase") != std::string::npos ||
           process.find("cubase") != std::string::npos;
}

// Theme palette

struct Pal { ImU32 btnBg,btnH,btnA,bdr,txt,txtD,txtSub,popBg,selBg,hovBg,acc; };
static Pal GetPal() {
    // Token-driven palette — derive from the active theme's ImGui style colors.
    // Key trick: resting bg is FrameBg lifted 10% toward Text. On dark themes where
    // FrameBg nearly equals waveformBG (e.g. LapisBlue frameBgBase rgb 21,41,68 vs
    // waveformBG rgb 23,32,41), the raw FrameBg made the capsule vanish into the
    // panel. The 10% lift keeps it theme-accurate while giving the capsule enough
    // separation for the label to read as a button rather than flat text.
    auto C = [](ImVec4 v, float a) {
        return ImGui::GetColorU32(ImVec4(v.x, v.y, v.z, a));
    };
    auto LerpRGB = [](ImVec4 a, ImVec4 b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t,
                      a.y + (b.y - a.y) * t,
                      a.z + (b.z - a.z) * t,
                      1.0f);
    };
    ImVec4 fb  = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    ImVec4 fbh = ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered);
    ImVec4 fba = ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive);
    ImVec4 bd  = ImGui::GetStyleColorVec4(ImGuiCol_Border);
    ImVec4 pb  = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
    ImVec4 tx  = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    ImVec4 lifted = LerpRGB(fb, tx, 0.10f);  // capsule fill — separates from panel

    Pal p;
    p.btnBg  = C(lifted, 1.00f);
    p.btnH   = C(LerpRGB(fbh, tx, 0.10f), 1.00f);
    p.btnA   = C(fba, 1.00f);
    p.bdr    = C(bd,  0.60f);          // visible outline on any theme
    p.txt    = ImGui::GetColorU32(tx); // primary label — full theme text
    p.txtD   = C(tx,  0.72f);          // dropdown item name
    p.txtSub = C(tx,  0.48f);          // capability tag (secondary line)
    p.popBg  = C(pb,  pb.w);
    p.selBg  = C(fba, 0.95f);
    p.hovBg  = C(fbh, 0.85f);
    p.acc    = ImGui::GetColorU32(tx);
    return p;
}

// Main widget — adaptive capsule [icon][Name][dot][Send][v]

bool DrawSendToDAWWidget(AppState& state) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;

    auto& targets = state.settings.dawTargets;
    if (targets.empty()) return false;
    bool hasSupportedTarget = false;
    for (const auto& target : targets) {
        if (target.enabled && IsSupportedDawDisplayTarget(target)) {
            hasSupportedTarget = true;
            break;
        }
    }
    if (!hasSupportedTarget) return false;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Pal pal = GetPal();

    const float btnH     = 24.0f;
    const float arrowW   = 22.0f;
    // MacKit baseline: rounded field/button surfaces; keep the existing bottom-bar height.
    const float rounding = 7.0f;
    const float itemH    = 42.0f;  // taller to fit secondary line
    const float menuPad  = 6.0f;   // MacKit menu padding
    const float menuRounding = 9.0f;
    const float menuRowRounding = 6.0f;
    const float menuActionH = 30.0f;
    const float menuGap  = 6.0f;

    int& selIdx = state.selectedDAWIndex;
    if (selIdx < 0 || selIdx >= (int)targets.size()) selIdx = 0;
    if (!targets[selIdx].enabled || !IsSupportedDawDisplayTarget(targets[selIdx])) {
        for (int i = 0; i < (int)targets.size(); ++i) {
            if (targets[i].enabled && IsSupportedDawDisplayTarget(targets[i])) {
                selIdx = i;
                break;
            }
        }
    }
    const auto& selTarget = targets[selIdx];

    std::string label = std::string("Send to ") + selTarget.name;
    ImVec2 labelSz = ImGui::CalcTextSize(label.c_str());

    // Capsule layout: [plane 12px gap] [label] [gap] [dot 6px] [gap] [arrow]
    const float mainW  = 14 + labelSz.x + 8 + 6 + 8;
    const float totalW = mainW + arrowW;
    const ImVec2 pos   = ImGui::GetCursorScreenPos();
    bool sendClicked   = false;

    const bool isSending = state.dawSendFeedback.isSending();
    bool mainFocused = false, arrowFocused = false;

// Main button (click = send)
    {
        ImRect rc(pos, {pos.x+mainW, pos.y+btnH});
        ImGuiID id = window->GetID("##SendMainBtn");
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0,0,0,0));
        ImGui::ItemAdd(rc, id);
        ImGui::PopStyleColor();
        bool hov, held;
        if (ImGui::ButtonBehavior(rc, id, &hov, &held) && !isSending) sendClicked = true;
        mainFocused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        dl->AddRectFilled(rc.Min,rc.Max,held?pal.btnA:(hov?pal.btnH:pal.btnBg),rounding,ImDrawFlags_RoundCornersLeft);
        dl->AddRect(rc.Min,rc.Max,pal.bdr,rounding,ImDrawFlags_RoundCornersLeft);

        float cx = rc.Min.x + 4;
        if (isSending) {
            float t = (float)fmod(ImGui::GetTime() * 3.0, 6.28318530718);
            ImVec2 spinCenter = {cx+6, rc.Min.y+btnH*.5f};
            for (int i = 0; i < 3; ++i) {
                float angle = t + i * 2.094f;
                float dotAlpha = 0.3f + 0.7f * (float)(0.5 + 0.5 * cos(angle));
                ImU32 dotCol = ImGui::GetColorU32(ImVec4(
                    ((pal.txt >> 0) & 0xFF) / 255.0f,
                    ((pal.txt >> 8) & 0xFF) / 255.0f,
                    ((pal.txt >> 16) & 0xFF) / 255.0f,
                    dotAlpha));
                float dx = 4.0f * cosf(angle - 1.5708f);
                float dy = 4.0f * sinf(angle - 1.5708f);
                dl->AddCircleFilled({spinCenter.x + dx, spinCenter.y + dy}, 1.5f, dotCol);
            }
        } else {
            DrawPaperPlane(dl, {cx+6, rc.Min.y+btnH*.5f}, pal.txt, .75f);
        }
        cx += 14;
        // Label
        dl->AddText({cx, rc.Min.y+(btnH-labelSz.y)*.5f}, pal.txt, label.c_str());
        cx += labelSz.x + 8;
        // Status dot
        ImU32 dotCol = GetStatusDotColor(selTarget);
        dl->AddCircleFilled({cx+3, rc.Min.y+btnH*.5f}, 3.0f, dotCol);
        if (hov) HarkenSearch::SetTooltip(L(DAW_SendToTargetTooltip), selTarget.name.c_str());
    }

// Arrow button (click = dropdown)
    {
        ImVec2 p2(pos.x+mainW, pos.y);
        ImRect rc(p2, {p2.x+arrowW, p2.y+btnH});
        ImGuiID id = window->GetID("##SendArrowBtn");
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0,0,0,0));
        ImGui::ItemAdd(rc, id);
        ImGui::PopStyleColor();
        bool hov, held;
        if (ImGui::ButtonBehavior(rc, id, &hov, &held))
            ImGui::OpenPopup("##SendDAWPopup");
        arrowFocused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        dl->AddRectFilled(rc.Min,rc.Max,held?pal.btnA:(hov?pal.btnH:pal.btnBg),rounding,ImDrawFlags_RoundCornersRight);
        dl->AddRect(rc.Min,rc.Max,pal.bdr,rounding,ImDrawFlags_RoundCornersRight);
        dl->AddLine({rc.Min.x,rc.Min.y+5},{rc.Min.x,rc.Max.y-5},pal.bdr);
        DrawChevronDown(dl,{rc.Min.x+arrowW*.5f,rc.Min.y+btnH*.5f},pal.txt,1.15f,1.5f);
    }
    ImGui::ItemSize({totalW, btnH});
    if (mainFocused || arrowFocused) {
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
        dl->AddRect({pos.x-2,pos.y-2},{pos.x+totalW+2,pos.y+btnH+2},
            ImGui::GetColorU32(ImVec4(accent.x,accent.y,accent.z,.7f)),rounding+2,0,1.5f);
    }

// Dropdown popup — MacKit menu shell (padding, rounding, 1 px border, tight rows)
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImColor(pal.popBg).Value);
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
    state.qaSendWidgetMenuOpen = false;
    if (state.qaSendWidgetMenuRequested) {
        state.qaSendWidgetMenuRequested = false;
        ImGui::OpenPopup("##SendDAWPopup");
    }
#endif
    ImGui::PushStyleColor(ImGuiCol_Border,  ImColor(pal.bdr).Value);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{menuPad,menuPad});
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,menuRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{0,0});
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize,1.0f);

    int enabledCount = 0;
    for (auto& dt : targets) if (dt.enabled && IsSupportedDawDisplayTarget(dt)) enabledCount++;
    float popH = enabledCount * itemH + 2.0f * menuActionH + 2.0f * menuPad + 8.0f;
    ImGui::SetNextWindowPos({pos.x, pos.y - popH - menuGap}, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize({std::max(totalW, 260.0f) + 2.0f * menuPad, 0});

    if (ImGui::BeginPopup("##SendDAWPopup")) {
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        state.qaSendWidgetMenuOpen = true;
#endif
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        ImGuiWindow* pw = ImGui::GetCurrentWindow();
        const float popW = ImGui::GetContentRegionAvail().x;
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
        state.qaSendWidgetMenuRows = 0;
        state.qaSendWidgetMenuFrame = ImGui::GetFrameCount();
        state.qaSendWidgetMenuId = ImGui::GetID("##SendDAWPopup");
#endif

        for (int i = 0; i < (int)targets.size(); ++i) {
            const auto& dt = targets[i];
            if (!dt.enabled) continue;
            if (!IsSupportedDawDisplayTarget(dt)) continue;
            bool isSel = (selIdx == i);

            ImVec2 ip = ImGui::GetCursorScreenPos();
            ImVec2 isz(popW, itemH);
            ImRect irc(ip, {ip.x+isz.x, ip.y+isz.y});

            ImGui::PushID(i);
            ImGuiID iid = pw->GetID("##di");
            ImGui::ItemAdd(irc, iid);
            if (isSel) ImGui::SetItemDefaultFocus();
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION && !AURA_INTERNAL_BETA
            state.qaSendWidgetMenuRows++;
#endif
            bool ih, iheld;
            if (ImGui::ButtonBehavior(irc, iid, &ih, &iheld)) {
                selIdx = i; ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();

            // Row background — MacKit menu hover/selected rounding
            ImU32 rowBg = isSel ? pal.selBg : (ih ? pal.hovBg : IM_COL32(0,0,0,0));
            if (rowBg & 0xFF000000)
                pdl->AddRectFilled(ip,{ip.x+isz.x,ip.y+isz.y},rowBg,menuRowRounding);

            // Status dot (left edge)
            ImU32 dotCol = GetStatusDotColor(dt);
            pdl->AddCircleFilled({ip.x+10, ip.y+itemH*.5f-4}, 3.0f, dotCol);

            // Vector icon
            ImU32 icol = isSel ? pal.acc : (dt.enabled ? pal.txtD : IM_COL32(80,80,80,120));
            DrawDAWIcon(pdl, {ip.x+28, ip.y+itemH*.5f-4}, dt.name, icol);

            // Primary text: name
            ImVec2 nameSz = ImGui::CalcTextSize(dt.name.c_str());
            float textX = ip.x + 48;
            pdl->AddText({textX, ip.y+5}, isSel?pal.acc:pal.txtD, dt.name.c_str());

            // Secondary text: capability tag
            const char* capTag = GetCapabilityTag(dt);
            pdl->AddText({textX, ip.y+5+nameSz.y+1}, pal.txtSub, capTag);

            // Shortcut hint (slots 1-3): show full key combo for discoverability
            if (i < 3) {
                char hint[20]; snprintf(hint, sizeof(hint), "Ctrl+Shift+%d", i+1);
                ImVec2 hsz = ImGui::CalcTextSize(hint);
                float hx = ip.x + isz.x - 34 - hsz.x;  // leave room for the checkmark
                pdl->AddText({hx, ip.y+(itemH-hsz.y)*.5f}, pal.txtSub, hint);
            }

            // Checkmark
            if (isSel) DrawCheckmark(pdl,{ip.x+isz.x-14, ip.y+itemH*.5f-1}, pal.acc);

            ImGui::ItemSize(isz);
        }

// Bottom actions
        ImGui::Dummy({0,4});
        ImVec2 sepP = ImGui::GetCursorScreenPos();
        pdl->AddLine({sepP.x+8,sepP.y},{sepP.x+popW-8,sepP.y},pal.bdr);
        ImGui::Dummy({0,4});

        // "Configure..." row
        auto drawMenuAction = [&](const char* label, bool* clicked) {
            ImVec2 ap = ImGui::GetCursorScreenPos();
            ImVec2 asz(popW, menuActionH);
            ImRect arc(ap,{ap.x+asz.x,ap.y+asz.y});
            ImGuiID aid = pw->GetID(label);
            ImGui::ItemAdd(arc, aid);
            bool ah, aheld;
            if (ImGui::ButtonBehavior(arc,aid,&ah,&aheld)) *clicked = true;
            if (ah) pdl->AddRectFilled(ap,{ap.x+asz.x,ap.y+asz.y},pal.hovBg,menuRowRounding);
            ImVec2 tsz = ImGui::CalcTextSize(label);
            pdl->AddText({ap.x+12,ap.y+(menuActionH-tsz.y)*.5f}, pal.txtD, label);
            ImGui::ItemSize(asz);
        };

        bool openCfg = false, doRefresh = false;
        drawMenuAction("Configure DAW...", &openCfg);
        drawMenuAction("Refresh Status", &doRefresh);

        if (openCfg) {
            state.openDawIntegrationSettings = true;
            state.showSettingsDialog = true;
            ImGui::CloseCurrentPopup();
        }
        if (doRefresh) {
            // Force immediate re-detect
            for (auto& dt : targets) dt.lastDetectTime = 0.0;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    return sendClicked;
}
