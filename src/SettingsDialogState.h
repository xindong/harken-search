#pragma once
#include "AppState.h"
#include "Localization.h"
#include "imgui_internal.h"
#include "PreferenceDraft.h"
#include "PreferencePreview.h"
#include "SettingsCatalog.h"
#include "BuiltinThemeScheme.h"
#include "ThemeSchedule.h"
#include "ThemeSchedulePaletteCodec.h"

namespace harken::settings {
inline unsigned avatarImageRevision=0;
inline const char* PageLabel(Page page) {
    switch (page) {
    case Page::Common: return L(SetUX_Common);
    case Page::Appearance: return L(SetUX_Appearance);
    case Page::Audio: return L(SetUX_AudioPlayback);
    case Page::Search: return L(SetUX_SearchThesaurus);
    case Page::Library: return L(SetUX_LibraryScanning);
    case Page::Interaction: return L(SetUX_ControlsShortcuts);
    case Page::Personal: return L(SetUX_ProfileExport);
    case Page::Backup: return L(SetUX_BackupRestore);
    case Page::Diagnostics: return L(SetUX_DiagnosticsLogs);
    }
    return "";
}
inline const std::array<AppTheme,5> StockThemes{AppTheme::Dark, AppTheme::RoseCedar, AppTheme::LapisBlue, AppTheme::Ocean, AppTheme::JadeMist};
inline const char* StockLabel(int index) {
    const LStr labels[]{LStr::Set_ThemeDark,LStr::Set_ThemeRoseCedar,LStr::Set_ThemeLapisBlue,LStr::Set_ThemeOcean,LStr::Set_ThemeJadeMist};
    return index >= 0 && index < 5 ? GetLocalizedString(labels[index]) : L(Set_ThemeCustom);
}
inline int PaletteIndex(theme_schedule::Period period) {
    for (int i=0;i<5;++i) if (theme_schedule::PalettesEqual(theme_schedule::Palette(period),MakeBuiltinScheme(StockThemes[i]))) return i;
    return -1;
}
struct RuntimeValues {
    bool spectrum = false;
    int fft = 2048;
    float blend = .5f;
    bool split = false;
    int threads = 0;
    int day = -1, night = -1;
};
inline RuntimeValues CaptureRuntime(const AppState& state) {
    return {state.showSpectrogram, state.spectrogramFFTSize, state.spectrogramBlend,
        state.spectrogramSplit, state.scanWorkerThreads,
        theme_schedule::Ready() ? PaletteIndex(theme_schedule::Period::Day) : -1,
        theme_schedule::Ready() ? PaletteIndex(theme_schedule::Period::Night) : -1};
}
inline void WriteRuntime(AppState& state, const RuntimeValues& value) {
    state.showSpectrogram=value.spectrum; state.spectrogramFFTSize=value.fft;
    state.spectrogramBlend=value.blend; state.spectrogramSplit=value.split;
    state.scanWorkerThreads=value.threads;
}
inline PreferencePreview<AppSettings>& AppearancePreview() {
    static PreferencePreview<AppSettings> preview{
        {[](const AppSettings& a,const AppSettings& b){return a.theme==b.theme && a.useCustomColors==b.useCustomColors;},
         [](AppSettings& a,const AppSettings& b){a.theme=b.theme;a.useCustomColors=b.useCustomColors;}},
        Field(&AppSettings::readingFontSize),Field(&AppSettings::listFontSize),
        {[](const AppSettings& a,const AppSettings& b){return a.rowHeight==b.rowHeight && a.listDensity==b.listDensity;},
         [](AppSettings& a,const AppSettings& b){a.rowHeight=b.rowHeight;a.listDensity=b.listDensity;}}
    };
    return preview;
}
inline AppSettings CaptureSettings(const AppState& state) {
    auto settings=AppearancePreview().ForPersistence(state.settings);
    if (const auto manual=theme_schedule::ManualAppearanceForSettings(state)) {
        settings.theme=manual->theme; settings.useCustomColors=manual->useCustomColors;
    }
    return settings;
}
struct Editor {
    PreferenceDraft<AppSettings> app{
#define PREF(name) Field(&AppSettings::name)
        PREF(audioOutputDeviceIndex),PREF(audioOutputDeviceName),PREF(audioPeriodSizeMs),PREF(transportEffectVisibility),PREF(listeningMode),
        PREF(defaultVolume),PREF(loudnessNormEnabled),PREF(loudnessNormTargetLUFS),PREF(fadeInOutEnabled),PREF(fadeInOutMs),
        PREF(skipLeadingSilence),PREF(silenceThresholdDb),PREF(defaultPitch),PREF(pitchRange),
        PREF(monitorLayout),PREF(downmixMode),PREF(downmixCenterDb),PREF(downmixSurroundDb),PREF(binauralMonitor),
        PREF(theme),PREF(themeScheduleMode),PREF(useCustomColors),PREF(language),PREF(readingFontSize),PREF(listFontSize),PREF(rowHeight),PREF(listDensity),
        PREF(showFileExtension),PREF(waveformDisplayMode),PREF(waveformStyle),PREF(miniWaveformRmsBodyStyle),PREF(meterChannelLayout),
        PREF(showLoudnessMeter),PREF(showRMSMeter),PREF(showTruePeakMeter),PREF(showClickRipple),
        PREF(showMilliseconds),PREF(showFrameNumber),PREF(defaultFrameRate),PREF(searchRealtime),PREF(suggestSourceMode),PREF(autoRenameTabBySearch),
        PREF(memoryProfile),PREF(fpsLimit),PREF(thumbnailMaxResolution),PREF(threadLimit),PREF(autoReconcileOnStartup),
        PREF(idleAssetBackfillEnabled),PREF(idleAssetBackfillDelayMinutes),PREF(rememberLastFolder),PREF(closeButtonAction),
        PREF(showGuidedMode),PREF(suppressConfirmDialogs),PREF(enableShowManagerHotkey),
        Field(&AppSettings::showManagerHotkey,[](const KeyBinding& a,const KeyBinding& b){return a.key==b.key && a.ctrl==b.ctrl && a.alt==b.alt && a.shift==b.shift;}),
        PREF(hideWindowOnDrag),PREF(hideWindowOnDragDelayMs),PREF(pausePlaybackOnDragStart),PREF(folderTreePathMode),
        PREF(userName),PREF(defaultExportPath),PREF(keepHarkenSearchOutputProcessingSuffix),
        PREF(diagnosticMode),PREF(showDiagnosticOverlay)
#undef PREF
    };
    PreferenceDraft<RuntimeValues> runtime{Field(&RuntimeValues::spectrum),Field(&RuntimeValues::fft),Field(&RuntimeValues::blend),
        Field(&RuntimeValues::split),Field(&RuntimeValues::threads),Field(&RuntimeValues::day),Field(&RuntimeValues::night)};
    bool open=false, closePrompt=false, closeAfterPrompt=false, recording=false, resetAll=false;
    Page page=Page::Common;
    Page previousPage=Page::Common;
    bool previousSearching=false, hadChildPopup=false;
    char query[256]{};
    std::string focusId, highlightId, highlightQuery, error, action;
    std::vector<OutputDeviceInfo> devices;
    std::vector<uint8_t> deviceIdentity, baselineDeviceIdentity;
    bool deviceEdited=false;
    void Reset(const AppState& state) { app.Begin(CaptureSettings(state)); runtime.Begin(CaptureRuntime(state)); deviceIdentity=state.audioEngine.getCurrentOutputDeviceId(); baselineDeviceIdentity=deviceIdentity; deviceEdited=false; error.clear(); recording=false; resetAll=false; }
    size_t ChangedCount() const { return app.ChangedCount()+runtime.ChangedCount()+(resetAll ? 1 : 0)+(deviceEdited&&deviceIdentity!=baselineDeviceIdentity ? 1 : 0); }
};
inline Editor& SettingsEditor() { static Editor editor; return editor; }
inline void BeginAppearancePreview(AppState& state) {
    auto& editor=SettingsEditor();
    if(editor.open)AppearancePreview().Begin(state.settings,editor.app.Baseline(),editor.app.Value());
}
inline void EndAppearancePreview(AppState& state) { AppearancePreview().Restore(state.settings); }
#if AURA_BUILD_DIAGNOSTICS && ARIS_PREVIEW_ISOLATION
inline bool savePreviewOnce=false, previewSaveResult=false;
struct Observation {
    struct Item { std::string id; ImVec4 bounds, clip; bool disabled=false; float fontPixels=0; };
    struct Text { std::string id, text; ImVec4 bounds,clip; };
    int frame=0; bool open=false, prompt=false; int page=0; size_t pending=0;
    float draftVolume=0; bool draftRealtime=false; int draftTheme=0;
    ImVec4 bounds{};
    ImVec4 thesaurusTextColor{};
    std::vector<Item> items;
    std::vector<Text> texts;
};
inline Observation observation;
inline bool failSaveOnce=false;
inline void ObserveText(const char* id,const char* text,ImVec2 pos,ImVec4 clip,ImFont* font,float size) {
    const auto extent=font->CalcTextSizeA(size,FLT_MAX,0.f,text);
    observation.texts.push_back({id,text,{pos.x,pos.y,pos.x+extent.x,pos.y+extent.y},clip});
}
inline void Observe(const char* id) {
    const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
    const auto ca=ImGui::GetWindowDrawList()->GetClipRectMin(),cb=ImGui::GetWindowDrawList()->GetClipRectMax();
    observation.items.push_back({id,{a.x,a.y,b.x,b.y},{ca.x,ca.y,cb.x,cb.y},(ImGui::GetCurrentContext()->CurrentItemFlags&ImGuiItemFlags_Disabled)!=0,ImGui::GetFontSize()});
}
#else
inline void Observe(const char*) {}
inline void ObserveText(const char*,const char*,ImVec2,ImVec4,ImFont*,float) {}
#endif
}
