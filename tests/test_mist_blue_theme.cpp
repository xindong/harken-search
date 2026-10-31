#include "AppState.h"
#include "Theme.h"
#include "BuiltinThemeScheme.h"
#include "ui/TransportContrast.h"
#include "ui/MistBlueChrome.h"
#include <cstdio>
int main() {
    using namespace TransportContrast;
    auto s=MakeBuiltinScheme(AppTheme::LapisBlue);
    int failures=0;
    auto check=[&](bool ok,const char* name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);failures+=!ok;};
    check(s.filterBrowserBG.x>.98f && s.filterBrowserBG.y>.98f && s.filterBrowserBG.z>.98f,"white filter panel");
    for(auto bg:{s.resultsBG,s.metadataPanelBG,s.waveformBG,s.transportBarBG}) {
        check(Luminance(bg)>.035 && Luminance(bg)<.13 && bg.z-bg.x>.08,"navy major surface close to original");
        check(Ratio(s.interfaceText,bg)>=4.5,"readable shared text on blue surfaces");
        check(Ratio(bg,s.resultsBG)<1.6,"gentle panel-to-panel contrast");
    }
    // Approved toolbar/dialog reference: darker foundations, visibly raised controls.
    check(Luminance(s.toolbarBG)>.025 && s.toolbarBG.z-s.toolbarBG.x>.08,"toolbar retains navy hue");
    check(Ratio(s.menuBarBG,s.toolbarBG)>=1.15,"menu distinct from toolbar");
    check(Ratio(s.frameBgBase,s.dialogBG)>=1.4,"dialog controls raised above body");
    check(Ratio(s.historyCapsuleBG,s.toolbarBG)>=1.4,"search surface above toolbar");
    check(Ratio(s.modeCapsuleActiveBG,s.modeCapsuleBG)>=1.7,"selected navigation above ordinary shell");
    check(Ratio(s.interfaceText,s.toolbarBG)>=4.5,"toolbar text");
    check(s.waveformColor.x>.98f && s.waveformColor.y>.98f && s.waveformColor.z>.98f,"white waveform line");
    check(Ratio(s.waveformColor,s.waveformBG)>=4.5,"white waveform contrast");
    check(Ratio(s.chromeBorder,s.metadataPanelBG)>=3.0,"info boundary against panel");
    check(Ratio(s.chromeBorder,s.resultsBG)>=3.0,"info boundary against workspace");
    check(Ratio(s.filterBrowserText,s.filterBrowserBG)>=4.5,"filter text");
    check(Ratio(s.filterBrowserHeading,s.sidebarHeaderBG)>=4.5,"filter heading");
    check(Ratio(s.metadataPanelLabel,s.metadataPanelBG)>=4.5,"metadata labels");
    check(Ratio(s.resultsSecondaryText,s.resultsBG)>=4.5,"secondary result labels");
    ImVec4 colors[ImGuiCol_COUNT]{};
    MistBlueChrome::Apply(colors,s);
    const auto controls = MistBlueChrome::Controls(s);
    check(Ratio(controls.normal,s.dialogBG)>=1.4,"effective dialog control separation");
    check(Ratio(controls.normal,controls.title)>=1.5,"effective title/control hierarchy");
    for(auto bg:{controls.normal,controls.hovered,controls.active}) {
        check(Ratio(s.interfaceText,bg)>=4.5,"dialog text in normal hover and pressed states");
        check(bg.w==1.0f,"control surfaces remain opaque");
    }
    check(Ratio(controls.muted,s.dialogBG)>=4.5,"dialog help text");
    check(Ratio(colors[ImGuiCol_TextDisabled],s.frameBgBase)>=4.5,"muted label on input surface");
    check(Ratio(s.filterBrowserOutline,s.filterBrowserBG)>=3.0,"white filter boundary");
    for(auto role:{ImGuiCol_ButtonActive,ImGuiCol_TabActive,ImGuiCol_TabHovered,ImGuiCol_HeaderActive})
        check(Ratio(s.interfaceText,colors[role])>=4.5,"active button and tab text");
    auto tabInk=MistBlueChrome::ResultsTabText(s);
    for(int state=0;state<3;++state) {
        auto on=MistBlueChrome::EqState(s,s.transportBarBG,true,state>0,state==2);
        auto off=MistBlueChrome::EqState(s,s.transportBarBG,false,state>0,state==2);
        check(Ratio(on.button,off.button)>=3.0,"EQ active/bypass surfaces stay distinct across interaction states");
        check(Ratio(on.ink,on.button)>=4.5 && Ratio(off.ink,off.button)>=4.5,"EQ state text contrast");
        check(Ratio(on.signal,s.transportBarBG)>=3.0 && Ratio(off.signal,s.transportBarBG)>=3.0,"EQ signal remains readable in both states");
        auto pass=MistBlueChrome::EqModeState(s,s.transportBarBG,true,false,state>0,state==2);
        auto notch=MistBlueChrome::EqModeState(s,s.transportBarBG,true,true,state>0,state==2);
        check(Ratio(pass.button,notch.button)>=3.0,"EQ pass/notch modes have distinct surfaces");
        check(Ratio(pass.ink,pass.button)>=4.5 && Ratio(notch.ink,notch.button)>=4.5,"EQ mode labels remain readable");
    }
    for (auto preset : {AppTheme::Dark, AppTheme::Ocean, AppTheme::LapisBlue,
                        AppTheme::JadeMist, AppTheme::RoseCedar}) {
        const auto palette = MakeBuiltinScheme(preset);
        const auto fillInk = ForSurface(palette.interfaceText, palette.controlAccent, 4.5);
        const auto trackInk = ForSurface(palette.interfaceText, palette.frameBgBase, 4.5);
        check(Ratio(Quantize(fillInk), Quantize(palette.controlAccent)) >= 4.5,
              "Similar Sounds progress label on filled track");
        check(Ratio(Quantize(trackInk), Quantize(palette.frameBgBase)) >= 4.5,
              "Similar Sounds progress label on empty track");
    }
    check(Ratio(tabInk,s.resultsTabActive)>=4.5,"selected result tab text");
    return failures?1:0;
}
