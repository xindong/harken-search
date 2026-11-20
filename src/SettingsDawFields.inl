#if defined(_WIN32)
ImVec4 dawCardFill=ImLerp(preview.dialogBG,preview.interfaceText,.06f);
dawCardFill.w=1.f;
const ImVec4 dawCardRule=ImLerp(dawCardFill,preview.interfaceText,.15f);
const ImVec4 dawSecondaryText=ImLerp(preview.interfaceText,dawCardFill,.25f);
const ImVec4 dawInactiveText=ImLerp(dawSecondaryText,dawCardFill,.18f);
const float disclosurePad=ImGui::GetStyle().ItemInnerSpacing.x;
const float disclosureGlyph=ImGui::GetFontSize()*.55f;
auto drawDawDisclosure=[&](const char* label,ImVec2 size,bool open,bool enabled,bool secondary=false,
                          const char* observationId=nullptr) {
    ImGui::PushStyleColor(ImGuiCol_NavCursor,preview.interfaceText);
    ImGui::BeginDisabled(!enabled);
    const bool pressed=ImGui::InvisibleButton(label,size,ImGuiButtonFlags_EnableNav);
    if (observationId) su::Observe(observationId);
    const bool hovered=enabled && ImGui::IsItemHovered();
    const bool held=enabled && ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    if (!ImGui::IsItemVisible()) return pressed;

    auto* draw=ImGui::GetWindowDrawList();
    const auto boundsMin=ImGui::GetItemRectMin(), boundsMax=ImGui::GetItemRectMax();
    if (hovered || held) {
        const auto fill=ImLerp(dawCardFill,preview.interfaceText,held ? .17f : .10f);
        draw->AddRectFilled(boundsMin,boundsMax,ImGui::GetColorU32(fill),ImGui::GetStyle().FrameRounding);
    }
    const ImVec4 labelColor=!enabled ? dawInactiveText : (secondary ? dawSecondaryText : preview.interfaceText);
    const ImU32 ink=ImGui::GetColorU32(labelColor);
    const ImVec2 center(boundsMax.x-disclosurePad-disclosureGlyph*.5f,(boundsMin.y+boundsMax.y)*.5f);
    const float span=disclosureGlyph*.4f, depth=disclosureGlyph*.22f;
    const ImVec2 labelMin(boundsMin.x+disclosurePad,center.y-ImGui::GetTextLineHeight()*.5f);
    const float labelRight=boundsMax.x-disclosureGlyph-disclosurePad*2.f;
    ImGui::PushStyleColor(ImGuiCol_Text,labelColor);
    ImGui::RenderTextEllipsis(draw,labelMin,{labelRight,boundsMax.y},labelRight,label,nullptr,nullptr);
    ImGui::PopStyleColor();
    const bool down=enabled && (open!=pressed);
    const ImVec2 arrow[]{
        down ? ImVec2(center.x-span,center.y-depth) : ImVec2(center.x-depth,center.y-span),
        down ? ImVec2(center.x,center.y+depth) : ImVec2(center.x+depth,center.y),
        down ? ImVec2(center.x+span,center.y-depth) : ImVec2(center.x-depth,center.y+span)
    };
    draw->AddPolyline(arrow,3,ink,0,std::max(1.f,disclosureGlyph*.16f));
    return pressed;
};
auto reaperStatusFor=[](const DawTarget& target) {
    return g_reaperSetupPageStatus && g_reaperSetupStatusPath==target.exePath
        ? &*g_reaperSetupPageStatus : nullptr;
};
auto canInstallReaper=[](const HarkenSearch::DawBridge::InstallStatus* status) {
    return status && !g_dawBridgeInstallInFlight.load() &&
        HarkenSearch::DawBridge::CanInstallMissingReaperBridge(*status);
};
auto finishReaperFocus=[&](bool requested) {
    if (!requested) return;
    ImGui::SetScrollHereY(.25f);
    editor.focusId.clear();
};
auto drawDawCard=[&](const su::Entry& entry,size_t index) {
    auto& targets=editor.dawTargets.Value();
    if (index>=targets.size()) return;
    auto& target=targets[index];
    // Only REAPER sending is implemented; the other DAWs share the same card but
    // cannot open their configuration yet.
    const bool sendReady=index==0;
    if (sendReady) g_reaperSetupPageProbePath=target.exePath;
    const auto* status=sendReady ? reaperStatusFor(target) : nullptr;
    const bool folderPath=IsReaperNamedPipeTarget(target);
    if (sendReady && g_reaperSetupPageStatus && g_reaperSetupStatusPath.empty()) {
        const auto& detected=*g_reaperSetupPageStatus;
        editor.reaperDetectedPath=detected.hostInstalled
            ? (folderPath ? detected.configuredRootPath : detected.hostExecutablePath) : std::string{};
    }
    const bool installable=sendReady && canInstallReaper(status);
    const std::string destination=installable
        ? status->configuredRootPath+"\n"+status->targetDirectory : std::string{};
    // Only the REAPER card may drop the consent: the other three cards are never
    // installable, so clearing it here erased the tick right after the user set it.
    if (sendReady && (!installable || editor.reaperConsentDestination!=destination)) editor.reaperInstallConsent=false;
    const bool focusPath=sendReady && editor.focusId=="daw-reaper";
    const bool focusSending=sendReady && editor.focusId=="daw-reaper-enabled";
    const bool focusDiagnostic=sendReady && editor.focusId=="daw-reaper-diagnostic";
    const bool focusInstallReason=sendReady && editor.focusId=="daw-reaper-install" && !installable;
    if (focusPath || focusSending || focusDiagnostic || focusInstallReason) editor.reaperConfigOpen=true;
    if (focusDiagnostic) editor.reaperDiagnosticsOpen=true;

    ImGui::PushID(entry.id.c_str());
    const ImVec2 cardOrigin=ImGui::GetCursorPos();
    const auto& style=ImGui::GetStyle();
    const float fontSize=ImGui::GetFontSize();
    const ImVec2 cardPadding(fontSize*.72f,fontSize*.40f);
    const float scrollbarReserve=std::max(0.f,style.ScrollbarSize-ImGui::GetCurrentWindowRead()->ScrollbarSizes.x);
    const float cardRight=cardOrigin.x+ImGui::GetContentRegionAvail().x-scrollbarReserve;
    const float contentLeft=cardOrigin.x+cardPadding.x;
    const float configRight=cardRight-cardPadding.x;
    const float frameHeight=ImGui::GetFrameHeight();
    const float headerY=cardOrigin.y+cardPadding.y;
    const float configWidth=ImGui::CalcTextSize(L(DawPrefs_Configure)).x+disclosureGlyph+disclosurePad*3.f;
    const float configX=configRight-configWidth;
    const float statusWidth=frameHeight*.70f;
    const float statusGap=style.ItemInnerSpacing.x;
    const float statusX=configX-statusGap-statusWidth;
    const float nameX=contentLeft+(installable ? frameHeight+style.ItemSpacing.x : 0.f);
    const float nameRight=std::max(nameX+1.f,(sendReady ? statusX : configX)-style.ItemSpacing.x);
    const char* name=entry.Label((int)g_currentLanguage);
    const ImVec2 nameSize=ImGui::CalcTextSize(name,nullptr,false,nameRight-nameX);
    const float nameY=headerY+std::max(0.f,(frameHeight-nameSize.y)*.5f);
    auto* draw=ImGui::GetWindowDrawList();
    ImDrawListSplitter cardLayers;
    cardLayers.Split(draw,2);
    cardLayers.SetCurrentChannel(draw,1);
    ImGui::BeginGroup();
    ImGui::SetCursorPos(ImVec2(contentLeft,headerY));
    ImGui::BeginGroup();
    if (installable) {
        const std::string checkboxLabel=std::string("##")+L(DawPrefs_Consent);
        if (ImGui::Checkbox(checkboxLabel.c_str(), &editor.reaperInstallConsent)) {
            editor.reaperConsentDestination=editor.reaperInstallConsent ? destination : std::string{};
        }
        su::Observe("daw-reaper-install-consent");
        if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) ImGui::SetTooltip("%s",L(DawPrefs_Consent));
    }
    ImGui::SetCursorPos(ImVec2(nameX,nameY));
    ImGui::PushTextWrapPos(nameRight);
    ImGui::TextUnformatted(name);
    ImGui::PopTextWrapPos();
    su::Observe((entry.id+"-name").c_str());

    if (sendReady) {
        ImGui::SetCursorPos(ImVec2(statusX,headerY));
        const bool pathConfigured=!target.exePath.empty() && status && status->hostInstalled && status->configuredPathExists;
        const char* pathState=pathConfigured ? L(DawPrefs_PathConfigured) : L(DawPrefs_PathConfigurable);
        const std::string statusLabel=std::string(pathState)+"###reaper-path-state";
        ImGui::InvisibleButton(statusLabel.c_str(),ImVec2(statusWidth,frameHeight),ImGuiButtonFlags_EnableNav);
        su::Observe("daw-reaper-status-icon");
        su::Observe(pathConfigured ? "daw-reaper-status-configured" : "daw-reaper-status-configurable");
        const auto iconMin=ImGui::GetItemRectMin();
        const auto iconMax=ImGui::GetItemRectMax();
        const ImVec2 center((iconMin.x+iconMax.x)*.5f,(iconMin.y+iconMax.y)*.5f);
        const float radius=std::clamp(ImGui::GetFontSize()*.42f,5.f,10.f);
        const ImU32 ink=ImGui::GetColorU32(ImGuiCol_Text);
        draw->AddCircle(center,radius,ink,0,1.4f);
        if (pathConfigured) {
            const ImVec2 check[]{ImVec2(center.x-radius*.48f,center.y),
                ImVec2(center.x-radius*.12f,center.y+radius*.33f),
                ImVec2(center.x+radius*.50f,center.y-radius*.38f)};
            draw->AddPolyline(check,3,ink,0,1.4f);
        } else {
            const float arm=radius*.48f;
            draw->AddLine(ImVec2(center.x-arm,center.y),ImVec2(center.x+arm,center.y),ink,1.4f);
            draw->AddLine(ImVec2(center.x,center.y-arm),ImVec2(center.x,center.y+arm),ink,1.4f);
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) ImGui::SetTooltip("%s",pathState);
    }
    ImGui::SetCursorPos(ImVec2(configX,headerY));
    const std::string configObservation=sendReady ? "daw-reaper-config" : entry.id+"-config-disabled";
    if (drawDawDisclosure(L(DawPrefs_Configure),ImVec2(configWidth,frameHeight),
                          editor.reaperConfigOpen,sendReady,false,configObservation.c_str())) {
        editor.reaperConfigOpen=!editor.reaperConfigOpen;
        if (!editor.reaperConfigOpen) editor.reaperDiagnosticsOpen=false;
        editor.reaperFocusPath=false;
    }
    ImGui::EndGroup();
    su::Observe(entry.id.c_str());

    if (sendReady && editor.reaperConfigOpen) {
        ImGui::SetCursorPosX(nameX);
        ImGui::BeginGroup();
        const ImU32 divider=ImGui::GetColorU32(dawCardRule);
        const auto drawCardDivider=[&] {
            const auto cursor=ImGui::GetCursorScreenPos();
            const float offsetX=cursor.x-ImGui::GetCursorPosX();
            draw->AddLine(ImVec2(offsetX+contentLeft,cursor.y),
                ImVec2(offsetX+configRight,cursor.y),divider);
            ImGui::Dummy(ImVec2(0,1.f));
        };
        drawCardDivider();
        const auto previousPath=target.exePath;
        const char* pathLabel=L(DawPrefs_ExecutablePath);
        const char* sendLabel=L(DawPrefs_EnableSend);
        const char* pathHint=L(DawPrefs_PathInputHint);
        const float selectorWidth=frameHeight;
        const float pathMinimum=ImGui::CalcTextSize(pathHint).x+style.FramePadding.x*2.f+selectorWidth;
        const float pathInlineWidth=configRight-nameX-ImGui::CalcTextSize(pathLabel).x-style.ItemSpacing.x;
        const bool pathFitsInline=pathInlineWidth>=pathMinimum;
        const float pathWidth=std::min(std::max(pathMinimum,fontSize*24.f),
                                       pathFitsInline ? pathInlineWidth : configRight-nameX);
        ImGui::AlignTextToFramePadding();
        const ImVec2 labelMin=ImGui::GetCursorScreenPos();
        const ImVec2 labelSize=ImGui::CalcTextSize(pathLabel,nullptr,false,std::max(1.f,configRight-nameX));
        ImGui::PushTextWrapPos(configRight);
        ImGui::TextUnformatted(pathLabel);
        ImGui::PopTextWrapPos();
        const ImVec2 labelMax(labelMin.x+labelSize.x+style.ItemSpacing.x,
            labelMin.y+std::max(labelSize.y,frameHeight));
        const bool pathLabelClicked=ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&
            ImGui::IsMouseHoveringRect(labelMin,labelMax);
        if (pathLabelClicked) editor.reaperFocusPath=true;
        if (pathFitsInline) ImGui::SameLine();
        ImGui::SetCursorPosX(configRight-pathWidth);
        char path[2048]{};
        std::snprintf(path,sizeof(path),"%s",target.exePath.c_str());
        ImGui::SetNextItemWidth(pathWidth-selectorWidth);
        if (focusPath || editor.reaperFocusPath) ImGui::SetKeyboardFocusHere();
        editor.reaperFocusPath=false;
        if (ImGui::InputTextWithHint("##reaper-path",pathHint,path,sizeof(path))) target.exePath=TrimCopy(path);
        const bool pathInputActive=ImGui::IsItemActive();
        const bool pathInputFocused=ImGui::IsItemFocused();
        const bool pathInputClicked=ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup|ImGuiHoveredFlags_NoNavOverride);
        const auto pathMin=ImGui::GetItemRectMin();
        const auto pathMax=ImGui::GetItemRectMax();
        const bool pathActive=pathInputActive||pathInputFocused;
        if (pathActive) {
            draw->AddRect(pathMin,pathMax,ImGui::GetColorU32(preview.controlAccent),
                          ImGui::GetStyle().FrameRounding,0,1.5f);
        }
        su::Observe("daw-reaper-path");
        finishReaperFocus(focusPath);
        ImGui::SameLine(0,0);
        bool browseRequested=false;
        const ImGuiID pathMenu=ImGui::GetID("##reaper-path-menu");
        if (ImGui::ArrowButton("##reaper-path-selector",ImGuiDir_Down)) ImGui::OpenPopup(pathMenu);
        su::Observe("daw-reaper-path-selector");
        const ImRect pathAnchor(pathMin,ImGui::GetItemRectMax());
        const bool selectorVisible=ImGui::IsItemVisible();
        if (selectorVisible) {
            const float menuWidth=pathAnchor.GetWidth();
            ImGui::SetNextWindowPos(pathAnchor.GetBL(),ImGuiCond_Appearing);
            ImGui::SetNextWindowSizeConstraints(ImVec2(menuWidth,0),ImVec2(menuWidth,FLT_MAX));
        }
        if (selectorVisible && ImGui::BeginComboPopup(pathMenu,pathAnchor,ImGuiComboFlags_PopupAlignLeft)) {
            if (pathInputClicked) {
                editor.reaperFocusPath=true;
                ImGui::CloseCurrentPopup();
            } else {
                if (!editor.reaperDetectedPath.empty()) {
                    const std::string detectedLabel=std::string(L(DawPrefs_UseDetectedPath))+": "+
                        editor.reaperDetectedPath+"###reaper-detected";
                    if (ImGui::Selectable(detectedLabel.c_str(),target.exePath==editor.reaperDetectedPath,0,
                                          ImVec2(ImGui::GetContentRegionAvail().x,0))) {
                        target.exePath=editor.reaperDetectedPath;
                    }
                    su::Observe("daw-reaper-use-detected-path");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",editor.reaperDetectedPath.c_str());
                }
                if (ImGui::Selectable(L(DawPrefs_ManualPath))) editor.reaperFocusPath=true;
                su::Observe("daw-reaper-manual-path");
                browseRequested=ImGui::Selectable(L(DawPrefs_Browse));
                su::Observe("daw-reaper-browse");
                ImGui::BeginDisabled(target.exePath.empty());
                if (ImGui::Selectable(L(DawPrefs_Clear))) target.exePath.clear();
                su::Observe("daw-reaper-clear");
                ImGui::EndDisabled();
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            }
            ImGui::EndCombo();
        }
        if (browseRequested) {
            const std::string picked=folderPath
                ? Platform::BrowseFolderDialogModern(g_hwnd,L(Main_ReaperRootDir))
                : Platform::ExecutableDialogs::OpenExecutableFileDialogUtf8(
                    g_hwnd,Utf8ToWide(pathLabel),Utf8ToWide(target.exePath));
            if (!picked.empty()) target.exePath=picked;
        }
        if (target.exePath!=previousPath) {
            editor.reaperInstallConsent=false;
            editor.reaperConsentDestination.clear();
            g_reaperSetupPageProbePath=target.exePath;
        }

        drawCardDivider();
        const float switchPadY=std::max(0.f,(24.f-ImGui::GetFontSize())*.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(ImGui::GetStyle().FramePadding.x,switchPadY));
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(configRight);
        ImGui::TextUnformatted(sendLabel);
        ImGui::PopTextWrapPos();
        if (ImGui::CalcTextSize(sendLabel).x+style.ItemSpacing.x+42.f<=configRight-nameX) ImGui::SameLine();
        if (focusSending) ImGui::SetKeyboardFocusHere();
        transportSwitch(target.enabled,configRight-ImGui::GetCursorPosX()+2.f);
        su::Observe("daw-reaper-enabled");
        ImGui::PopStyleVar();
        finishReaperFocus(focusSending);
        status=reaperStatusFor(target);
        // The send pipeline already keeps a live bridge status for this target; the
        // settings page shows the same one the old DAW window used instead of
        // reporting "not detected" when the setup probe cannot resolve the path.
        const auto liveStatusValue=ReaperBridgeUiStatusForTarget(target);
        const auto* liveStatus=liveStatusValue ? &*liveStatusValue : nullptr;
        const auto* statusForCopy=liveStatus ? liveStatus : status;
        ImGui::PushTextWrapPos(configRight);
        ImGui::BeginGroup();
        if (g_dawBridgeInstallInFlight.load()) {
            ImGui::TextWrapped("%s",L(REAPER_InstallBusy));
        } else if (liveStatus) {
            const ImVec4 signal=liveStatus->sendChannelAvailable ? preview.meterBase
                : (liveStatus->state==HarkenSearch::DawBridge::InstallState::WaitingForHostExit
                    ? preview.meterWarning : preview.meterClip);
            ImGui::PushStyleColor(ImGuiCol_Text,ImLerp(preview.interfaceText,signal,.55f));
            ImGui::TextWrapped("%s",liveStatus->detail.Render().c_str());
            ImGui::PopStyleColor();
            if (!liveStatus->remediation.empty()) ImGui::TextWrapped("%s",liveStatus->remediation.Render().c_str());
            ImGui::PushStyleColor(ImGuiCol_Text,dawSecondaryText);
            ImGui::TextWrapped(L(Main_ReaperStatusFmt),
                liveStatus->hostExecutablePath.empty() ? "-" : liveStatus->hostExecutablePath.c_str());
            ImGui::TextWrapped(L(Main_ReaperResourceFmt),
                liveStatus->resourcePath.empty() ? "-" : liveStatus->resourcePath.c_str());
            ImGui::TextWrapped(L(Main_ReaperUserPluginsFmt),
                liveStatus->targetDirectory.empty() ? "-" : liveStatus->targetDirectory.c_str());
            ImGui::TextWrapped(L(Main_ReaperBridgeInfoFmt),
                liveStatus->installedVersion.empty() ? "-" : liveStatus->installedVersion.c_str(),
                liveStatus->installedProtocolVersion,
                liveStatus->installedArchitecture.empty() ? "-" : liveStatus->installedArchitecture.c_str(),
                liveStatus->handshakeCode.c_str());
            ImGui::PopStyleColor();
        } else if (!status) {
            ImGui::TextWrapped("%s",L(REAPER_AutoDetecting));
        } else if (!status->hostInstalled && status->detail.empty()) {
            ImGui::TextWrapped("%s",L(DawPrefs_NotFound));
        } else if (status->legacyFilePresent && !status->filePresent) {
            ImGui::TextWrapped("%s",L(DawPrefs_ExistingComponent));
        } else if (status->hostRunning && HarkenSearch::DawBridge::NeedsReaperSetupNotice(*status)) {
            ImGui::TextWrapped("%s",L(DawPrefs_CloseReaperToInstall));
        } else {
            ImGui::TextWrapped("%s",status->detail.Render().c_str());
        }
        ImGui::EndGroup();
        if (!canInstallReaper(status)) su::Observe("daw-reaper-install-reason");
        finishReaperFocus(focusInstallReason);
        drawCardDivider();
        if (focusDiagnostic && !statusForCopy) ImGui::SetKeyboardFocusHere();
        if (drawDawDisclosure(L(DawPrefs_ReaperDiagnostics),ImVec2(configRight-nameX,frameHeight),
                              editor.reaperDiagnosticsOpen,true,true)) {
            editor.reaperDiagnosticsOpen=!editor.reaperDiagnosticsOpen;
        }
        su::Observe("daw-reaper-diagnostic-disclosure");
        if (editor.reaperDiagnosticsOpen) {
            if (!liveStatus && status && !status->remediation.empty()) ImGui::TextWrapped("%s",status->remediation.Render().c_str());
            const float horizontalPadding=style.FramePadding.x*2.f;
            const char* copyLabel=L(DawPrefs_CopyDiagnostic);
            const char* textEnd=ImGui::FindRenderedTextEnd(copyLabel);
            const float copyTextWidth=std::max(1.f,std::floor(configRight-nameX-horizontalPadding));
            std::string copyButtonLabel;
            for (const char* lineStart=copyLabel; lineStart<textEnd;) {
                const char* lineEnd=ImGui::GetFont()->CalcWordWrapPosition(fontSize,lineStart,textEnd,copyTextWidth);
                copyButtonLabel.append(lineStart,lineEnd);
                lineStart=ImTextCalcWordWrapNextLineStart(lineEnd,textEnd);
                if (lineStart<textEnd) copyButtonLabel.push_back('\n');
            }
            copyButtonLabel+="###ReaperCopyDiagnostic";
            if (focusDiagnostic && statusForCopy) ImGui::SetKeyboardFocusHere();
            ImGui::BeginDisabled(!statusForCopy);
            if (aura::ui::SmallActionButton(copyButtonLabel.c_str()) && statusForCopy) {
                const auto summary=HarkenSearch::DawBridge::BuildReaperBridgeDiagnosticSummary(*statusForCopy,true);
                ImGui::SetClipboardText(summary.c_str());
                state.loadErrorToast=L(REAPER_DiagCopied);
                state.loadErrorToastUntil=ImGui::GetTime()+4.0;
            }
            su::Observe("daw-reaper-diagnostic");
            ImGui::EndDisabled();
        }
        finishReaperFocus(focusDiagnostic);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
    }
    ImGui::SetCursorPos(ImVec2(cardOrigin.x,ImGui::GetCursorPosY()-ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(cardRight-cardOrigin.x,cardPadding.y));
    ImGui::EndGroup();
    su::Observe((entry.id+"-card").c_str());
    const auto cardMin=ImGui::GetItemRectMin(), cardMax=ImGui::GetItemRectMax();
    const float cardRounding=fontSize*.8f;
    cardLayers.SetCurrentChannel(draw,0);
    draw->AddRectFilled(cardMin,cardMax,ImGui::GetColorU32(dawCardFill),cardRounding);
    draw->AddRect(cardMin,cardMax,ImGui::GetColorU32(preview.chromeBorder),cardRounding);
    cardLayers.Merge(draw);
    ImGui::PopID();
};
auto drawReaperInstallAction=[&] {
    auto& target=editor.dawTargets.Value().front();
    const auto* status=reaperStatusFor(target);
    const bool installable=canInstallReaper(status);
    if (!installable) return;
    ImGui::PushID("daw-reaper-install");
    const float width=ImGui::CalcTextSize(L(DawPrefs_InstallSelected)).x+30.f;
    const bool focusInstall=editor.focusId=="daw-reaper-install";
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+std::max(0.f,ImGui::GetContentRegionAvail().x-width));
    if (focusInstall && editor.reaperInstallConsent) ImGui::SetKeyboardFocusHere();
    ImGui::BeginDisabled(!editor.reaperInstallConsent);
    if (aura::ui::ActionButton(L(DawPrefs_InstallSelected),ImVec2(width,0),aura::ui::ButtonVariant::PrimarySearch)
        && editor.reaperInstallConsent) {
        editor.reaperInstallConsent=false;
        editor.reaperConsentDestination.clear();
        DawTarget resolved=target;
        resolved.exePath=status->configuredRootPath;
        auto options=BuildReaperBridgeProbeOptions(resolved);
        options.inspectLegacyComponent=true;
        if (Platform::HarkenUserData::PreviewIsolationActive()) {
            options.pipeNameOverride=LR"(\\.\pipe\HarkenSearch-PreviewReaperSetup)";
        }
        if (StartReaperBridgeUpdateAfterExit(options, false, true)) {
            if (target.exePath.empty()) {
                target.exePath=IsReaperNamedPipeTarget(target)
                    ? resolved.exePath : status->hostExecutablePath;
            }
            state.loadErrorToast=L(REAPER_InstallBusy);
            state.loadErrorToastUntil=ImGui::GetTime()+5.0;
        }
    }
    su::Observe("daw-reaper-install-button");
    su::Observe("daw-reaper-install");
    ImGui::EndDisabled();
    finishReaperFocus(focusInstall);
    ImGui::PopID();
};
#endif

auto drawDawFields=[&](const std::string& id,float controlW) {
    if (id=="daw-reaper-install") {
        ImGui::TextWrapped("%s", L(DawPrefs_InstallUnavailable));
        return;
    }
    for (size_t index=0;index<su::kSettingsDawPresets.size();++index) {
        if (su::kSettingsDawPresets[index].id!=id) continue;
        auto& target=editor.dawTargets.Value()[index];
        const bool reaper=index==0;
        ImGui::Checkbox(L(DawPrefs_EnableSend), &target.enabled);
        su::Observe((id+"-enabled").c_str());
        const auto previousPath=target.exePath;
        ImGui::SetNextItemWidth(controlW-2);
        const bool folderPath=reaper && IsReaperNamedPipeTarget(target);
        const char* pathHint=folderPath ? L(Main_ReaperRootDir) : L(DawPrefs_ExecutablePath);
        char path[2048]{};
        std::snprintf(path,sizeof(path),"%s",target.exePath.c_str());
        if (ImGui::InputTextWithHint("##daw-path",pathHint,path,sizeof(path))) target.exePath=TrimCopy(path);
        su::Observe((id+"-path").c_str());
        if (aura::ui::SmallActionButton(L(DawPrefs_Browse))) {
            const std::string picked=folderPath
                ? OpenFolderDialog(g_hwnd)
                : Platform::ExecutableDialogs::OpenExecutableFileDialogUtf8(
                    g_hwnd, Utf8ToWide(pathHint), Utf8ToWide(target.exePath));
            if (!picked.empty()) target.exePath=picked;
        }
        su::Observe((id+"-browse").c_str());
        ImGui::SameLine();
        if (aura::ui::SmallActionButton(L(DawPrefs_Clear))) target.exePath.clear();
        if (reaper) {
            if (target.exePath.empty() && g_reaperSetupPageStatus && g_reaperSetupStatusPath.empty() &&
                g_reaperSetupPageStatus->hostInstalled) {
                if (aura::ui::SmallActionButton(L(DawPrefs_UseDetectedPath))) {
                    target.exePath=folderPath ? g_reaperSetupPageStatus->configuredRootPath
                                              : g_reaperSetupPageStatus->hostExecutablePath;
                }
                su::Observe("daw-reaper-use-detected-path");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", g_reaperSetupPageStatus->configuredRootPath.c_str());
                }
            }
            if (target.exePath!=previousPath) editor.reaperInstallConsent=false;
        }
        break;
    }
};
