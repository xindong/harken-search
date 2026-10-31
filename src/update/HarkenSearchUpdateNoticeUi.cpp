#include "HarkenSearchUpdateNoticeUi.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

#include "../Localization.h"
#include "../Theme.h"
#include "../ui/ActionButtonStyle.h"
#include "../Version.h"
#include "platform/animation_preferences.h"

namespace aris::update {
namespace {
#if AURA_BUILD_DIAGNOSTICS
HarkenSearchUpdateNoticeQaSnapshot g_noticeQa;
void RecordBodyForQa() {
    const auto pos = ImGui::GetWindowPos();
    const auto size = ImGui::GetWindowSize();
    g_noticeQa.controls["body"] = {pos.x, pos.y, pos.x + size.x, pos.y + size.y};
    g_noticeQa.scrollY = ImGui::GetScrollY();
    g_noticeQa.scrollMaxY = ImGui::GetScrollMaxY();
}
#endif

constexpr float kPanelWidth = 760.0f;
constexpr float kCheckingHeight = 330.0f;
constexpr float kLatestHeight = 700.0f;
constexpr float kAvailableHeight = 700.0f;
constexpr float kDownloadingHeight = 700.0f;
constexpr float kRestartingHeight = 700.0f;
constexpr float kErrorHeight = 332.0f;
constexpr float kHistoryHeight = 700.0f;
constexpr float kContentLeft = 18.0f;
constexpr float kContentWidth = 724.0f;
constexpr float kActionButtonWidth = 160.0f;
constexpr float kActionButtonHeight = 36.0f;
constexpr const char* kReleaseDateLabel = u8"发布日期";

ImVec4 MixColor(const ImVec4& from, const ImVec4& to, float amount) {
    return ImVec4(
        from.x + (to.x - from.x) * amount,
        from.y + (to.y - from.y) * amount,
        from.z + (to.z - from.z) * amount,
        from.w + (to.w - from.w) * amount);
}

ImVec2 ScreenPoint(float x, float y) {
    const ImVec2 window = ImGui::GetWindowPos();
    return ImVec2(window.x + x, window.y + y);
}

void DrawPanelRect(
    float x,
    float y,
    float width,
    float height,
    const ImVec4& fill,
    const ImVec4& border,
    float rounding) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ScreenPoint(x, y);
    const ImVec2 max = ScreenPoint(x + width, y + height);
    draw->AddRectFilled(min, max, ImGui::GetColorU32(fill), rounding);
    draw->AddRect(min, max, ImGui::GetColorU32(border), rounding, 0, 1.0f);
}

void TextAt(const char* text, float x, float y, const ImVec4& color) {
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void TextWrappedAt(std::string_view text, float x, float y, float width, const ImVec4& color) {
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::PushTextWrapPos(x + width);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

bool ButtonAt(
    const char* id,
    const char* label,
    float x,
    float y,
    float width,
    float height,
    bool primary,
    bool enabled,
    const ThemeColorScheme& theme) {
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::PushID(id);
    ImGui::BeginDisabled(!enabled);
    const aura::ui::ActionButtonStyle::Scope buttonStyle(aura::ui::ActionButtonStyle::Make(
        theme, aura::ui::ActionButtonStyle::CurrentSurface(),
        primary ? aura::ui::ButtonVariant::PrimarySearch : aura::ui::ButtonVariant::Secondary));
    const bool clicked = aura::ui::ActionButtonStyle::Draw(label, ImVec2(width, height));
#if AURA_BUILD_DIAGNOSTICS
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();
    g_noticeQa.controls[id] = {min.x, min.y, max.x, max.y};
#endif
    ImGui::EndDisabled();
    ImGui::PopID();
    return clicked;
}

std::string DisplayVersion(std::string_view version) {
    if (version.empty()) return "--";
    const std::string display = SanitizeHarkenSearchUpdateDisplayText(version);
    if (version.front() == 'v' || version.front() == 'V') return display;
    return "v" + display;
}

void ResetNoticeScrollState(ReleaseNotesUiState& notes) {
    bool open = true;
    FinalizeHarkenSearchUpdateNoticeClose(open, notes);
}

HarkenSearchUpdateNoticeVisibleState ResolvePanelState(
    const HarkenSearchUpdateUiState& updateState, bool showHistory) {
    if (showHistory) return HarkenSearchUpdateNoticeVisibleState::History;
    if (updateState.checkInFlight && updateState.phase == HarkenSearchUpdateUiPhase::Idle) {
        return HarkenSearchUpdateNoticeVisibleState::Checking;
    }
    switch (updateState.phase) {
        case HarkenSearchUpdateUiPhase::Checking:
            return HarkenSearchUpdateNoticeVisibleState::Checking;
        case HarkenSearchUpdateUiPhase::Idle:
            return HarkenSearchUpdateNoticeVisibleState::Latest;
        case HarkenSearchUpdateUiPhase::Ready:
            return HarkenSearchUpdateNoticeVisibleState::Available;
        case HarkenSearchUpdateUiPhase::Downloading:
            return HarkenSearchUpdateNoticeVisibleState::Downloading;
        case HarkenSearchUpdateUiPhase::Restarting:
            return HarkenSearchUpdateNoticeVisibleState::Restarting;
        case HarkenSearchUpdateUiPhase::Disabled:
            return HarkenSearchUpdateNoticeVisibleState::Latest;
        case HarkenSearchUpdateUiPhase::Error:
            return HarkenSearchUpdateNoticeVisibleState::Error;
    }
    return HarkenSearchUpdateNoticeVisibleState::Error;
}

float PanelHeight(HarkenSearchUpdateNoticeVisibleState state) {
    switch (state) {
        case HarkenSearchUpdateNoticeVisibleState::Checking:
            return kCheckingHeight;
        case HarkenSearchUpdateNoticeVisibleState::Latest:
            return kLatestHeight;
        case HarkenSearchUpdateNoticeVisibleState::Available:
            return kAvailableHeight;
        case HarkenSearchUpdateNoticeVisibleState::Downloading:
            return kDownloadingHeight;
        case HarkenSearchUpdateNoticeVisibleState::Restarting:
            return kRestartingHeight;
        case HarkenSearchUpdateNoticeVisibleState::Error:
            return kErrorHeight;
        case HarkenSearchUpdateNoticeVisibleState::History:
            return kHistoryHeight;
    }
    return kErrorHeight;
}

void RenderHeader(
    bool& open,
    ReleaseNotesUiState& notes,
    const ThemeColorScheme& theme) {
    TextAt(notes.showHistory ? u8"更新历史" : u8"检查更新",
           kContentLeft, 22.0f, theme.interfaceText);
    if (!notes.showHistory && ButtonAt(
            "history", u8"更新历史", 594.0f, 20.0f, 108.0f, 24.0f,
            false, true, theme)) {
        notes.showHistory = true;
        notes.summaryScrollKey.clear();
    }
    if (notes.showHistory && ButtonAt(
            "back-to-update", L(Update_InlineBackToCheck), 564.0f, 20.0f, 138.0f, 24.0f,
            false, true, theme)) {
        notes.showHistory = false;
        notes.summaryScrollKey.clear();
    }
    if (ButtonAt(
            "close",
            u8"×",
            714.0f,
            20.0f,
            28.0f,
            22.0f,
            false,
            true,
            theme)) {
        FinalizeHarkenSearchUpdateNoticeClose(open, notes);
    }
}

void RenderStatusRow(
    HarkenSearchUpdateNoticeVisibleState state,
    const HarkenSearchUpdateUiState& updateState,
    const ThemeColorScheme& theme) {
    const char* label = u8"更新检查失败";
    ImVec4 color = theme.resultsText;
    switch (state) {
        case HarkenSearchUpdateNoticeVisibleState::Checking:
            label = u8"正在检查更新";
            break;
        case HarkenSearchUpdateNoticeVisibleState::Latest:
            label = u8"当前已是最新版本";
            break;
        case HarkenSearchUpdateNoticeVisibleState::Available:
            label = u8"有新版本可用";
            color = theme.resultsText;
            break;
        case HarkenSearchUpdateNoticeVisibleState::Downloading:
            label = u8"正在下载更新";
            break;
        case HarkenSearchUpdateNoticeVisibleState::Restarting:
            label = u8"正在准备更新";
            color = theme.resultsText;
            break;
        case HarkenSearchUpdateNoticeVisibleState::Error: {
            const auto failure = ExplainHarkenSearchUpdateFailure(updateState.lastError);
            label = failure.headline.empty() ? u8"更新检查失败" : failure.headline.c_str();
            color = theme.wwiseConnectFail;
            break;
        }
        case HarkenSearchUpdateNoticeVisibleState::History:
            label = u8"更新历史";
            break;
    }
    TextAt(label, kContentLeft, 71.0f, color);
}

void RenderVersionCard(
    HarkenSearchUpdateNoticeVisibleState state,
    const HarkenSearchUpdateUiState& updateState,
    const ThemeColorScheme& theme) {
    DrawPanelRect(
        kContentLeft,
        115.0f,
        kContentWidth,
        72.0f,
        theme.dialogPanelBG,
        theme.chromeBorder,
        8.0f);

    const std::string currentVersion = DisplayVersion(
        updateState.currentVersion.empty()
            ? std::string_view(SOUNDMANAGER_VERSION_STRING)
            : std::string_view(updateState.currentVersion));
    std::string candidateVersion = "--";
    if (state == HarkenSearchUpdateNoticeVisibleState::Checking) {
        candidateVersion = u8"检查中…";
    } else if (state == HarkenSearchUpdateNoticeVisibleState::Latest) {
        candidateVersion = currentVersion;
    } else if (!updateState.candidateVersion.empty()) {
        candidateVersion = DisplayVersion(updateState.candidateVersion);
    }
    const bool highlightCandidate = state == HarkenSearchUpdateNoticeVisibleState::Available ||
                                    state == HarkenSearchUpdateNoticeVisibleState::Downloading ||
                                    state == HarkenSearchUpdateNoticeVisibleState::Restarting;

    TextAt(u8"当前版本", 37.0f, 128.0f, theme.resultsSecondaryText);
    TextAt(currentVersion.c_str(), 37.0f, 148.0f, theme.resultsText);
    ImGui::GetWindowDrawList()->AddLine(
        ScreenPoint(378.0f, 126.0f),
        ScreenPoint(378.0f, 174.0f),
        ImGui::GetColorU32(theme.chromeBorder),
        1.0f);
    TextAt(u8"最新版本", 399.0f, 128.0f, theme.resultsSecondaryText);
    TextAt(
        candidateVersion.c_str(),
        399.0f,
        148.0f,
        highlightCandidate ? theme.resultsText : theme.resultsSecondaryText);
}

std::string FormatContributors(const ReleaseNotesEntry* release) {
    std::string result = u8"贡献者  ";
    bool hasContributor = false;
    if (release) {
        for (const auto& contributor : release->contributors) {
            if (contributor.empty()) {
                continue;
            }
            if (hasContributor) {
                result.append(", ");
            }
            result.append(SanitizeHarkenSearchUpdateDisplayText(contributor));
            hasContributor = true;
        }
    }
    if (!hasContributor) {
        result.append("--");
    }
    return result;
}

// Shared single-column content for target preview and history.
void RenderReleaseContents(const ReleaseNotesEntry& release, const ThemeColorScheme& theme) {
    const std::string metadata = DisplayVersion(release.version) + "   " +
        SanitizeHarkenSearchUpdateDisplayText(release.releaseDate);
    ImGui::TextColored(theme.resultsSecondaryText, "%s", metadata.c_str());
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.resultsText);
    if (!release.headline.empty()) {
        const auto headline = SanitizeHarkenSearchUpdateDisplayText(release.headline);
        ImGui::TextUnformatted(headline.c_str());
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
    }
    for (std::size_t index = 0; index < release.items.size(); ++index) {
        const auto& item = release.items[index];
        ImGui::PushID(static_cast<int>(index));
        const auto title = SanitizeHarkenSearchUpdateDisplayText(item.title);
        const auto author = SanitizeHarkenSearchUpdateDisplayText(item.author);
        if (ImGui::BeginTable("##TopicHeading", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch, 0.64f);
            ImGui::TableSetupColumn("author", ImGuiTableColumnFlags_WidthStretch, 0.36f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(title.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TableSetColumnIndex(1);
            ImGui::PushTextWrapPos(0.0f);
            const float authorWidth = ImGui::CalcTextSize(author.c_str()).x;
            const float remaining = ImGui::GetContentRegionAvail().x;
            if (authorWidth < remaining) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remaining - authorWidth);
            ImGui::TextColored(theme.resultsSecondaryText, "%s", author.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTable();
        }
        const auto text = SanitizeHarkenSearchUpdateDisplayText(item.text);
        ImGui::TextUnformatted(text.c_str());
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        ImGui::PopID();
    }
    for (const auto& note : release.notes) {
        const auto text = SanitizeHarkenSearchUpdateDisplayText(note);
        ImGui::TextUnformatted(text.c_str());
    }
    ImGui::PopStyleColor();
    const auto contributors = FormatContributors(&release);
    ImGui::TextColored(theme.resultsSecondaryText, "%s", contributors.c_str());
    ImGui::PopTextWrapPos();
}

void RenderUpdateLog(float y, float bottom, const char* sectionTitle,
    const ReleaseNotesEntry* release, ReleaseNotesUiState& notes,
    bool candidate, const ThemeColorScheme& theme) {
    ImGui::GetWindowDrawList()->AddLine(ScreenPoint(kContentLeft, y),
        ScreenPoint(kContentLeft + kContentWidth, y), ImGui::GetColorU32(theme.chromeBorder));
    TextAt(sectionTitle, kContentLeft, y + 12.0f, theme.resultsSecondaryText);
    const std::string key = std::string(sectionTitle) + ":" +
        (release ? release->version : (candidate ? notes.candidateNotesVersion : "missing"));
    const bool changed = notes.summaryScrollKey != key;
    notes.summaryScrollKey = key;
    ImGui::SetCursorPos(ImVec2(kContentLeft, y + 37.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.dialogBG);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 4.0f));
    ImGui::PushID(key.c_str());
    ImGui::BeginChild("##UpdateReleaseContents", ImVec2(kContentWidth, std::max(40.0f, bottom - y - 37.0f)),
        false, ImGuiWindowFlags_NoSavedSettings);
    if (changed) ImGui::SetScrollY(0.0f);
    if (release) {
        RenderReleaseContents(*release, theme);
    } else {
        const bool failed = candidate ? notes.candidateNotesFailed : notes.loadFailed;
        const char* message = failed ? L(Update_InlineReadFailed)
                                    : L(Update_InlineNoNotes);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.resultsSecondaryText);
        ImGui::TextWrapped("%s", message);
        ImGui::PopStyleColor();
    }
#if AURA_BUILD_DIAGNOSTICS
    g_noticeQa.version = release ? release->version : std::string{};
    g_noticeQa.itemCount = release ? release->items.size() : 0;
    RecordBodyForQa();
#endif
    ImGui::EndChild();
    ImGui::PopID();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void RenderDownloadProgress(
    const HarkenSearchUpdateUiState& updateState,
    const HarkenSearchUpdateNoticePresentation& presentation,
    const ThemeColorScheme& theme) {
    const bool totalUnknown = presentation.totalBytes == 0;
    const float progress = totalUnknown
        ? -1.0f
        : std::clamp(
              static_cast<float>(presentation.downloadedBytes) /
                  static_cast<float>(presentation.totalBytes),
              0.0f,
              1.0f);
    char progressText[96]{};
    if (totalUnknown) {
        std::snprintf(progressText, sizeof(progressText), u8"正在准备下载");
    } else {
        std::snprintf(
            progressText,
            sizeof(progressText),
            "%.0f%%  (%.1f / %.1f MB)",
            progress * 100.0f,
            static_cast<double>(presentation.downloadedBytes) / (1024.0 * 1024.0),
            static_cast<double>(presentation.totalBytes) / (1024.0 * 1024.0));
    }

    ImGui::SetCursorPos(ImVec2(kContentLeft, 199.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme.dialogPanelBG);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme.resultsText);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.interfaceText);
    ImGui::ProgressBar(progress, ImVec2(kContentWidth, 18.0f), progressText);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
}

void RenderHistory(ReleaseNotesUiState& notes, bool& open, float panelHeight,
    std::string_view installedVersion, const ThemeColorScheme& theme) {
    const auto history = BuildHarkenSearchReleaseNotesHistory(notes, installedVersion);
    ImGui::SetCursorPos(ImVec2(kContentLeft, 66.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.dialogBG);
    ImGui::BeginChild("##UpdateHistoryContents", ImVec2(kContentWidth, panelHeight - 134.0f),
        false, ImGuiWindowFlags_NoSavedSettings);
    if (history.empty()) {
        ImGui::TextColored(theme.resultsSecondaryText, "%s", notes.loadFailed
            ? L(Update_InlineHistoryReadFailed) : L(Update_InlineHistoryEmpty));
    } else {
        for (const auto* release : history) {
            ImGui::PushID(release->version.c_str());
            RenderReleaseContents(*release, theme);
            ImGui::Dummy(ImVec2(0.0f, 12.0f));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0.0f, 20.0f));
            ImGui::PopID();
        }
    }
#if AURA_BUILD_DIAGNOSTICS
    g_noticeQa.version = history.empty() ? std::string{} : history.front()->version;
    g_noticeQa.itemCount = history.size();
    RecordBodyForQa();
#endif
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (ButtonAt("history-dismiss", u8"知道了", 326.0f, panelHeight - 54.0f,
                 108.0f, 36.0f, false, true, theme))
        FinalizeHarkenSearchUpdateNoticeClose(open, notes);
}

void RenderCheckingWait(
    const HarkenSearchUpdateUiState& updateState,
    const ThemeColorScheme& theme) {
    (void)updateState;
    const char* message = u8"正在连接更新服务，请稍候…";
    TextWrappedAt(message, kContentLeft, 211.0f, kContentWidth, theme.resultsSecondaryText);
}

void RenderRestartNotice(float y, const ThemeColorScheme& theme) {
    TextAt(L(Update_BootstrapHandoff), kContentLeft, y, theme.resultsSecondaryText);
    TextAt(u8"关闭主窗口后，独立更新窗口会继续显示替换进度。",
           kContentLeft, y + 24.0f, theme.resultsSecondaryText);
    ImGui::SetCursorPos(ImVec2(kContentLeft, y + 42.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, theme.dialogPanelBG);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme.resultsText);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.interfaceText);
    const float progress = Platform::Animation::ClientAreaAnimationEnabled()
        ? -static_cast<float>(ImGui::GetTime() + 0.001) : 0.0f;
    ImGui::ProgressBar(progress, ImVec2(kContentWidth, 8.0f), "");
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
}

void RenderErrorDetails(
    const HarkenSearchUpdateUiState& updateState,
    const HarkenSearchUpdateNoticePresentation& presentation,
    ReleaseNotesUiState& notes,
    const ThemeColorScheme& theme) {
    const auto failure = ExplainHarkenSearchUpdateFailure(updateState.lastError);
    const char* fallback = updateState.phase == HarkenSearchUpdateUiPhase::Disabled
        ? u8"当前构建未配置正式更新通道。"
        : (failure.guidance.empty() ? u8"请稍后重试。" : failure.guidance.c_str());
    std::string errorScrollKey(fallback);
    if (!presentation.lastError.empty()) {
        errorScrollKey.push_back('\n');
        errorScrollKey.append(presentation.lastError.data(), presentation.lastError.size());
    }
    const bool resetErrorScroll = notes.errorScrollKey != errorScrollKey;
    notes.errorScrollKey = errorScrollKey;
    ImGui::SetCursorPos(ImVec2(kContentLeft, 199.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.dialogPanelBG);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.chromeBorder);
    ImGui::PushID(errorScrollKey.c_str());
    ImGui::BeginChild("##UpdateErrorDetails",
        ImVec2(kContentWidth, 54.0f),
        true,
        ImGuiWindowFlags_NoSavedSettings);
    if (resetErrorScroll) {
        ImGui::SetScrollY(0.0f);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.resultsSecondaryText);
    ImGui::TextUnformatted(fallback);
    if (!presentation.lastError.empty()) {
        ImGui::Spacing();
        ImGui::TextUnformatted(
            presentation.lastError.data(),
            presentation.lastError.data() + presentation.lastError.size());
    }
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopID();
    ImGui::PopStyleColor(2);
}

void RenderActionBar(
    float y,
    HarkenSearchUpdateNoticeVisibleState state,
    bool& open,
    const HarkenSearchUpdateUiState& updateState,
    const HarkenSearchUpdateNoticeActions& actions,
    const HarkenSearchUpdateNoticePresentation& presentation,
    const ThemeColorScheme& theme) {
    static HarkenSearchUpdateNoticeActionGate actionGate;
    const std::string gateKey = std::to_string(static_cast<int>(state)) + ":" +
        updateState.candidateVersion + ":" + updateState.lastError;
    actionGate.Enter(gateKey, presentation.operationGeneration);
    const char* primaryLabel = u8"检查更新";
    bool primaryEnabled = false;
    bool confirmRestart = false;
    switch (state) {
        case HarkenSearchUpdateNoticeVisibleState::Latest:
            primaryEnabled = static_cast<bool>(actions.requestCheck);
            break;
        case HarkenSearchUpdateNoticeVisibleState::Available:
            primaryLabel = u8"立即更新";
            primaryEnabled = static_cast<bool>(actions.confirmRestart);
            confirmRestart = true;
            break;
        case HarkenSearchUpdateNoticeVisibleState::Error:
            primaryLabel = u8"重试";
            primaryEnabled = static_cast<bool>(actions.requestCheck);
            break;
        case HarkenSearchUpdateNoticeVisibleState::Checking:
            primaryLabel = u8"检查中…";
            primaryEnabled = false;
            break;
        case HarkenSearchUpdateNoticeVisibleState::Downloading:
        case HarkenSearchUpdateNoticeVisibleState::Restarting:
            break;
        case HarkenSearchUpdateNoticeVisibleState::History:
            primaryEnabled = false;
            break;
    }

    if (ButtonAt(
            "primary-action",
            primaryLabel,
            215.0f,
            y,
            kActionButtonWidth,
            kActionButtonHeight,
            true,
            primaryEnabled,
            theme)) {
        if (!actionGate.TryConsume()) {
            return;
        }
        const bool accepted = confirmRestart
            ? actions.confirmRestart()
            : actions.requestCheck();
        if (!accepted) actionGate.ReleaseRejectedAction();
    }
    if (ButtonAt(
            "secondary-action",
            u8"稍后",
            385.0f,
            y,
            kActionButtonWidth,
            kActionButtonHeight,
            false,
            true,
            theme)) {
        open = false;
    }
}

}  // namespace

#if AURA_BUILD_DIAGNOSTICS
HarkenSearchUpdateNoticeQaSnapshot GetHarkenSearchUpdateNoticeQaSnapshot() { return g_noticeQa; }
#endif

void RenderHarkenSearchUpdateNoticeUi(
    bool& open,
    ReleaseNotesUiState& notes,
    const HarkenSearchUpdateUiState& updateState,
    const HarkenSearchUpdateNoticeActions& actions,
    const HarkenSearchUpdateNoticePresentation& presentation) {
    if (!open) {
        ResetNoticeScrollState(notes);
        return;
    }
#if AURA_BUILD_DIAGNOSTICS
    g_noticeQa = {};
    g_noticeQa.visible = true;
    g_noticeQa.history = notes.showHistory;
#endif
    const auto& theme = ThemeColors::current();
    const HarkenSearchUpdateNoticeVisibleState state = ResolvePanelState(updateState, notes.showHistory);
    const float panelHeight = std::min(PanelHeight(state), std::max(420.0f, ImGui::GetMainViewport()->WorkSize.y - 40.0f));
    const bool rendersUpdateSummary = state == HarkenSearchUpdateNoticeVisibleState::Latest ||
                                      state == HarkenSearchUpdateNoticeVisibleState::Available ||
                                      state == HarkenSearchUpdateNoticeVisibleState::Downloading ||
                                      state == HarkenSearchUpdateNoticeVisibleState::Restarting;
    if (!rendersUpdateSummary && state != HarkenSearchUpdateNoticeVisibleState::History) {
        notes.summaryScrollKey.clear();
        notes.expandedReleaseVersion.clear();
    }
    if (state != HarkenSearchUpdateNoticeVisibleState::Error) {
        notes.errorScrollKey.clear();
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        viewport->GetCenter(),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, panelHeight), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.dialogBG);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.chromeBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin(presentation.windowId, &open, flags)) {
        RenderHeader(open, notes, theme);
        if (state == HarkenSearchUpdateNoticeVisibleState::History) {
            RenderHistory(notes, open, panelHeight,
                updateState.currentVersion.empty() ? SOUNDMANAGER_VERSION_STRING : updateState.currentVersion, theme);
        } else {
            RenderStatusRow(state, updateState, theme);
            RenderVersionCard(state, updateState, theme);
        }

        const std::string_view releaseVersion = state == HarkenSearchUpdateNoticeVisibleState::Latest
            ? std::string_view(
                  updateState.currentVersion.empty()
                      ? SOUNDMANAGER_VERSION_STRING
                      : updateState.currentVersion)
            : std::string_view(updateState.candidateVersion);
        const bool candidate = state != HarkenSearchUpdateNoticeVisibleState::Latest;
        const ReleaseNotesEntry* release = candidate
            ? SelectHarkenSearchReleaseNotes(notes, releaseVersion, true)
            : SelectHarkenSearchLocalDisplayReleaseNotes(notes, releaseVersion);
        const float actionY = panelHeight - 54.0f;

        switch (state) {
            case HarkenSearchUpdateNoticeVisibleState::Checking:
                RenderCheckingWait(updateState, theme);
                RenderActionBar(275.0f, state, open, updateState, actions, presentation, theme);
                break;
            case HarkenSearchUpdateNoticeVisibleState::Latest:
                RenderUpdateLog(199.0f, actionY - 18.0f, u8"本机版本", release, notes, false, theme);
                RenderActionBar(actionY, state, open, updateState, actions, presentation, theme);
                break;
            case HarkenSearchUpdateNoticeVisibleState::Available:
                RenderUpdateLog(199.0f, actionY - 63.0f, u8"本次更新", release, notes, true, theme);
                TextWrappedAt(L(Update_InlineRestartNote),
                    kContentLeft, actionY - 44.0f, kContentWidth, theme.resultsSecondaryText);
                RenderActionBar(actionY, state, open, updateState, actions, presentation, theme);
                break;
            case HarkenSearchUpdateNoticeVisibleState::Downloading:
                RenderDownloadProgress(updateState, presentation, theme);
                RenderUpdateLog(229.0f, actionY - 18.0f, u8"本次更新", release, notes, true, theme);
                RenderActionBar(actionY, state, open, updateState, actions, presentation, theme);
                break;
            case HarkenSearchUpdateNoticeVisibleState::Restarting:
                RenderUpdateLog(199.0f, actionY - 94.0f, u8"本次更新", release, notes, true, theme);
                RenderRestartNotice(actionY - 82.0f, theme);
                RenderActionBar(actionY, state, open, updateState, actions, presentation, theme);
                break;
            case HarkenSearchUpdateNoticeVisibleState::Error:
                RenderErrorDetails(updateState, presentation, notes, theme);
                RenderActionBar(277.0f, state, open, updateState, actions, presentation, theme);
                break;
            case HarkenSearchUpdateNoticeVisibleState::History:
                break;
        }
    }
    ImGui::End();
    if (!open) {
        ResetNoticeScrollState(notes);
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

}  // namespace aris::update
