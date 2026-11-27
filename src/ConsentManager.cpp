#include "ConsentManager.h"
#include "Localization.h"   // Lw for consent dialog UI (not the fallback box)

#include "platform/filesystem.h"
#include "platform/system.h"
#include "platform/text_encoding.h"

#include <filesystem>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aris::legal {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

ConsentRecord readRecord(const std::string& path) {
    ConsentRecord record;
    std::string text;
    if (!Platform::FileSystem::ReadFileText(path, text)) return record;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        if (key == "eulaVersion") record.eulaVersion = value;
        else if (key == "privacyVersion") record.privacyVersion = value;
        else if (key == "acceptedAtUtc") record.acceptedAtUtc = value;
        else if (key == "appVersion") record.appVersion = value;
    }
    return record;
}

std::string consentRecordPath(const std::string& settingsPath) {
    return settingsPath + ".legal-consent";
}

}  // namespace

bool LoadLegalDocuments(const std::string& legalDocumentRootUtf8,
                        LegalDocuments& documents) {
    documents = {};
    const auto root = std::filesystem::u8path(legalDocumentRootUtf8);
    if (!Platform::FileSystem::ReadFileText(
            (root / "EULA.md").u8string(), documents.eulaText) ||
        !Platform::FileSystem::ReadFileText(
            (root / "PRIVACY_POLICY.md").u8string(), documents.privacyText)) {
        documents = {};
        return false;
    }
    if (trim(documents.eulaText).empty() || trim(documents.privacyText).empty()) {
        documents = {};
        return false;
    }
    return true;
}

#ifdef _WIN32
namespace {

constexpr int kConsentDocumentControlId = 1001;

struct ConsentDialogData {
    std::wstring title;
    std::wstring body;
    bool automatedTest = false;
    bool initiallyDisabled = false;
    bool penultimateStillDisabled = false;
    bool exactBottomEnabled = false;
    bool prematureAcceptRejected = false;
    ConsentUiPresentedCallback onPresented = nullptr;
    bool presentedCallbackInvoked = false;
    bool presentedAfterVisibleInteractive = false;
};

constexpr wchar_t kConsentFlowMutexName[] = L"Local\\Aris.ConsentFlow.v1";

std::wstring utf8ToWide(const std::string& value) {
    return Platform::TextEncoding::Utf8ToWideStrict(value);
}

void appendWord(std::vector<unsigned char>& buffer, WORD value) {
    buffer.push_back(static_cast<unsigned char>(value & 0xff));
    buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
}

void appendWideString(std::vector<unsigned char>& buffer, const wchar_t* value) {
    while (*value) {
        appendWord(buffer, static_cast<WORD>(*value++));
    }
    appendWord(buffer, 0);
}

void alignDword(std::vector<unsigned char>& buffer) {
    while ((buffer.size() & 3U) != 0U) buffer.push_back(0);
}

bool isConsentDocumentAtBottom(HWND documentControl) {
    SCROLLINFO scrollInfo{};
    scrollInfo.cbSize = sizeof(scrollInfo);
    scrollInfo.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (!documentControl) return false;
    if (!GetScrollInfo(documentControl, SB_VERT, &scrollInfo)) return false;
    // Win32 nMax is inclusive. A page that begins at nMax - (nPage - 1)
    // therefore displays the final scroll unit.
    const int pageOffset = scrollInfo.nPage > 0
        ? static_cast<int>(scrollInfo.nPage - 1)
        : 0;
    return scrollInfo.nPos >= scrollInfo.nMax - pageOffset;
}

void updateConsentAcceptState(HWND dialog, ConsentDialogData* data) {
    const HWND documentControl = GetDlgItem(dialog, kConsentDocumentControlId);
    const bool atBottom = isConsentDocumentAtBottom(documentControl);
    EnableWindow(GetDlgItem(dialog, IDYES), atBottom ? TRUE : FALSE);
}

constexpr UINT kConsentTestPrematureAcceptMessage = WM_APP + 72;
constexpr UINT kConsentTestPenultimateMessage = WM_APP + 73;
constexpr UINT kConsentTestExactBottomMessage = WM_APP + 74;
constexpr UINT kConsentUiPresentedMessage = WM_APP + 75;

INT_PTR CALLBACK ConsentDialogProc(HWND dialog, UINT message,
                                   WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        auto* data = reinterpret_cast<ConsentDialogData*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        SetWindowTextW(dialog, data->title.c_str());
        SetDlgItemTextW(dialog, kConsentDocumentControlId, data->body.c_str());
        EnableWindow(GetDlgItem(dialog, IDYES), FALSE);
        data->initiallyDisabled = !IsWindowEnabled(GetDlgItem(dialog, IDYES));
        updateConsentAcceptState(dialog, data);
        SetFocus(GetDlgItem(dialog, kConsentDocumentControlId));
        return FALSE;
    }
    if (message == WM_SHOWWINDOW && wParam != FALSE) {
        auto* data = reinterpret_cast<ConsentDialogData*>(
            GetWindowLongPtrW(dialog, DWLP_USER));
        if (data && data->onPresented && !data->presentedCallbackInvoked) {
            PostMessageW(dialog, kConsentUiPresentedMessage, 0, 0);
        }
        return FALSE;
    }
    if (message == kConsentUiPresentedMessage) {
        auto* data = reinterpret_cast<ConsentDialogData*>(
            GetWindowLongPtrW(dialog, DWLP_USER));
        const HWND documentControl = GetDlgItem(dialog, kConsentDocumentControlId);
        if (data && data->onPresented && !data->presentedCallbackInvoked &&
            Platform::WindowIsVisible(dialog) && IsWindowEnabled(dialog) &&
            Platform::WindowIsVisible(documentControl) && IsWindowEnabled(documentControl)) {
            data->presentedCallbackInvoked = true;
            data->presentedAfterVisibleInteractive = true;
            data->onPresented();
            if (data->automatedTest) {
                PostMessageW(dialog, kConsentTestPrematureAcceptMessage, 0, 0);
            }
        }
        return TRUE;
    }
    if (message == kConsentTestPrematureAcceptMessage) {
        auto* data = reinterpret_cast<ConsentDialogData*>(
            GetWindowLongPtrW(dialog, DWLP_USER));
        SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), 0);
        data->prematureAcceptRejected =
            Platform::WindowIsValid(dialog) &&
            !IsWindowEnabled(GetDlgItem(dialog, IDYES));
        PostMessageW(dialog, kConsentTestPenultimateMessage, 0, 0);
        return TRUE;
    }
    if (message == kConsentTestPenultimateMessage) {
        auto* data = reinterpret_cast<ConsentDialogData*>(
            GetWindowLongPtrW(dialog, DWLP_USER));
        const HWND documentControl = GetDlgItem(dialog, kConsentDocumentControlId);
        SendMessageW(documentControl, EM_LINESCROLL, 0, INT_MAX);
        SendMessageW(documentControl, EM_LINESCROLL, 0, -1);
        updateConsentAcceptState(dialog, data);
        data->penultimateStillDisabled =
            !IsWindowEnabled(GetDlgItem(dialog, IDYES));
        PostMessageW(dialog, kConsentTestExactBottomMessage, 0, 0);
        return TRUE;
    }
    if (message == kConsentTestExactBottomMessage) {
        auto* data = reinterpret_cast<ConsentDialogData*>(
            GetWindowLongPtrW(dialog, DWLP_USER));
        const HWND documentControl = GetDlgItem(dialog, kConsentDocumentControlId);
        SendMessageW(documentControl, EM_LINESCROLL, 0, 1);
        updateConsentAcceptState(dialog, data);
        data->exactBottomEnabled = IsWindowEnabled(GetDlgItem(dialog, IDYES)) != FALSE;
        EndDialog(dialog, data->exactBottomEnabled ? IDYES : IDNO);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        const int command = LOWORD(wParam);
        if (command == kConsentDocumentControlId && HIWORD(wParam) == EN_VSCROLL) {
            updateConsentAcceptState(
                dialog, reinterpret_cast<ConsentDialogData*>(
                    GetWindowLongPtrW(dialog, DWLP_USER)));
            return TRUE;
        }
        if (command == IDYES || command == IDNO) {
            if (command == IDYES && !IsWindowEnabled(GetDlgItem(dialog, IDYES))) {
                return TRUE;
            }
            EndDialog(dialog, command);
            return TRUE;
        }
    } else if (message == WM_CLOSE) {
        EndDialog(dialog, IDNO);
        return TRUE;
    }
    return FALSE;
}

std::vector<unsigned char> makeConsentDialogTemplate() {
    std::vector<unsigned char> buffer;
    const auto appendDword = [&buffer](DWORD value) {
        buffer.push_back(static_cast<unsigned char>(value & 0xff));
        buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
        buffer.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
        buffer.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
    };

    appendDword(DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    appendDword(0);
    appendWord(buffer, 3);
    appendWord(buffer, 0); appendWord(buffer, 0);
    appendWord(buffer, 460); appendWord(buffer, 320);
    appendWord(buffer, 0); appendWord(buffer, 0);
    appendWideString(buffer, Lw(Consent_Title).c_str());    appendWord(buffer, 9); appendWideString(buffer, L"Segoe UI");

    alignDword(buffer);
    appendDword(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_HSCROLL);
    appendDword(0);
    appendWord(buffer, 8); appendWord(buffer, 8);
    appendWord(buffer, 444); appendWord(buffer, 260);
    appendWord(buffer, kConsentDocumentControlId);
    appendWord(buffer, 0xffff); appendWord(buffer, 0x0081);
    appendWord(buffer, 0); appendWord(buffer, 0);

    alignDword(buffer);
    appendDword(WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON);
    appendDword(0);
    appendWord(buffer, 260); appendWord(buffer, 280);
    appendWord(buffer, 90); appendWord(buffer, 24);
    appendWord(buffer, IDYES);
    appendWord(buffer, 0xffff); appendWord(buffer, 0x0080);
    appendWideString(buffer, Lw(Consent_Accept).c_str());
    appendWord(buffer, 0);

    alignDword(buffer);
    appendDword(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON);
    appendDword(0);
    appendWord(buffer, 360); appendWord(buffer, 280);
    appendWord(buffer, 90); appendWord(buffer, 24);
    appendWord(buffer, IDNO);
    appendWord(buffer, 0xffff); appendWord(buffer, 0x0080);
    appendWideString(buffer, Lw(Consent_Decline).c_str());
    appendWord(buffer, 0);
    return buffer;
}

ConsentResult reportTechnicalFailure(HWND owner) {
    Platform::ShowMessageBox(
        owner,
        L"无法完成法律文本确认。请检查法律文档是否完整、可读取，并确认设置目录可写后重试。",
        L"Harken Search 启动失败", MB_OK | MB_ICONERROR);
    return ConsentResult::TechnicalFailure;
}

ConsentResult showConsentDocument(HWND owner, const std::wstring& title,
                                  const std::wstring& body,
                                  ConsentUiPresentedCallback onPresented = nullptr) {
    ConsentDialogData data{title, body, false, false, false, false};
    data.onPresented = onPresented;
    const auto dialogTemplate = makeConsentDialogTemplate();
    const INT_PTR result = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<const DLGTEMPLATE*>(dialogTemplate.data()), owner,
        ConsentDialogProc, reinterpret_cast<LPARAM>(&data));
    if (result == IDYES) return ConsentResult::Accepted;
    if (result == IDNO) return ConsentResult::Rejected;
    return reportTechnicalFailure(owner);
}

}  // namespace

ConsentDialogRuntimeTestResult RunConsentDialogRuntimeTests() {
    static bool presentedCallbackObserved = false;
    presentedCallbackObserved = false;
    const auto markPresented = []() { presentedCallbackObserved = true; };
    std::wstring body;
    for (int i = 0; i < 240; ++i) {
        body += L"可滚动法律正文测试行 " + std::to_wstring(i) + L"\r\n";
    }
    ConsentDialogData data{L"Harken Search 法律文本测试", body, true, false, false, false};
    data.onPresented = markPresented;
    const auto dialogTemplate = makeConsentDialogTemplate();
    const INT_PTR result = DialogBoxIndirectParamW(
        GetModuleHandleW(nullptr),
        reinterpret_cast<const DLGTEMPLATE*>(dialogTemplate.data()), nullptr,
        ConsentDialogProc, reinterpret_cast<LPARAM>(&data));
    return {
        result != -1,
        data.initiallyDisabled,
        data.penultimateStillDisabled,
        data.exactBottomEnabled,
        data.prematureAcceptRejected,
        !isConsentDocumentAtBottom(
            reinterpret_cast<HWND>(static_cast<LONG_PTR>(-1))),
        presentedCallbackObserved,
        data.presentedAfterVisibleInteractive,
    };
}
#endif

ConsentManager::ConsentManager(std::string settingsPath) : settingsPath_(std::move(settingsPath)) {}

ConsentRecord ConsentManager::load() const {
    return readRecord(consentRecordPath(settingsPath_));
}

bool ConsentManager::requiresConsent(const std::string& eulaVersion,
                                     const std::string& privacyVersion) const {
    const ConsentRecord record = load();
    return record.eulaVersion != eulaVersion || record.privacyVersion != privacyVersion ||
           record.acceptedAtUtc.empty() || record.appVersion.empty();
}

bool ConsentManager::recordConsent(const std::string& appVersion,
                                   const std::string& acceptedAtUtc,
                                   const std::string& eulaVersion,
                                   const std::string& privacyVersion) const {
    const std::string path = consentRecordPath(settingsPath_);
    const auto separator = path.find_last_of("\\/");
    if (separator != std::string::npos &&
        !Platform::FileSystem::EnsureDirectoryExists(path.substr(0, separator))) {
        return false;
    }
    std::ostringstream output;
    output << "# Aris legal consent (auto-generated)\n"
           << "eulaVersion=" << eulaVersion << '\n'
           << "privacyVersion=" << privacyVersion << '\n'
           << "acceptedAtUtc=" << acceptedAtUtc << '\n'
           << "appVersion=" << appVersion << '\n';
    return Platform::FileSystem::WriteFileText(path, output.str());
}

bool ConsentManager::recordCurrentConsent(const std::string& appVersion,
                                          const std::string& eulaVersion,
                                          const std::string& privacyVersion) const {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream timestamp;
    timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return recordConsent(appVersion, timestamp.str(), eulaVersion, privacyVersion);
}

ConsentResult EnsureRequiredConsent(void* ownerWindow,
                                    const std::string& settingsPath,
                                    const std::string& appVersion,
                                    const std::string& legalDocumentRootUtf8,
                                    ConsentUiPresentedCallback onFirstConsentUiPresented) {
#ifdef _WIN32
    void* consentMutex = nullptr;
    bool joinedExistingFlow = false;
    bool abandonedConsentFlow = false;
    if (!Platform::AcquireNamedMutex(kConsentFlowMutexName, consentMutex,
                                     joinedExistingFlow, abandonedConsentFlow)) {
        return reportTechnicalFailure(nullptr);
    }
    struct ConsentMutexGuard {
        void* handle;
        ~ConsentMutexGuard() {
            Platform::ReleaseNamedMutex(handle);
        }
    } consentMutexGuard{consentMutex};
#endif

    ConsentManager manager(settingsPath);
    if (!manager.requiresConsent(kCurrentEulaVersion, kCurrentPrivacyVersion)) {
        return ConsentResult::Accepted;
    }

#ifdef _WIN32
    // If another launch just completed the only permitted first-run flow but
    // no acceptance record exists, this launch exits instead of opening a
    // second sequential dialog flow.
    if (joinedExistingFlow && !abandonedConsentFlow) {
        return ConsentResult::Rejected;
    }

    LegalDocuments documents;
    if (!LoadLegalDocuments(legalDocumentRootUtf8, documents)) {
        return reportTechnicalFailure(reinterpret_cast<HWND>(ownerWindow));
    }
    const std::wstring eulaBody = utf8ToWide(documents.eulaText);
    const std::wstring privacyBody = utf8ToWide(documents.privacyText);
    if (eulaBody.empty() || privacyBody.empty()) return reportTechnicalFailure(reinterpret_cast<HWND>(ownerWindow));
    const auto owner = reinterpret_cast<HWND>(ownerWindow);
    const ConsentResult eulaResult = showConsentDocument(owner, Lw(Consent_EulaCaption),
                                                         eulaBody, onFirstConsentUiPresented);
    if (eulaResult != ConsentResult::Accepted) return eulaResult;
    const ConsentResult privacyResult = showConsentDocument(owner, Lw(Consent_PrivacyCaption),                                                            privacyBody);
    if (privacyResult != ConsentResult::Accepted) return privacyResult;
#else
    (void)ownerWindow;
    (void)legalDocumentRootUtf8;
    return ConsentResult::TechnicalFailure;
#endif
    return manager.recordCurrentConsent(appVersion)
        ? ConsentResult::Accepted
#ifdef _WIN32
        : reportTechnicalFailure(reinterpret_cast<HWND>(ownerWindow));
#else
        : ConsentResult::TechnicalFailure;
#endif
}

}  // namespace aris::legal
