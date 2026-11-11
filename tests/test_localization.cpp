#include "Localization.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
int main() {
    int failures = 0;
    for (AppLanguage language : {AppLanguage::Chinese, AppLanguage::English}) {
        g_currentLanguage = language;
        const char* languageName = language == AppLanguage::Chinese ? "Chinese" : "English";
        const int missing = CheckTranslationCompleteness(language);
        const char* title = L(Similar_Title);
        const char* find = L(Similar_Find);
        const char* indexing = L(Similar_Indexing);
        const char* score = L(Similar_ColumnScore);
        const char* expectedTitle = language == AppLanguage::Chinese ? u8"相似音效" : "Similar Sounds";
        const char* expectedFind = language == AppLanguage::Chinese ? u8"查找相似音效" : "Find similar sounds";
        const char* expectedIndexing = language == AppLanguage::Chinese ? u8"正在分析特征…" : "Analyzing features...";
        const char* expectedScore = language == AppLanguage::Chinese ? u8"匹配分" : "Match score";

        std::cout << languageName << ": completeness_missing=" << missing
                  << ", Similar_Title=" << (title ? title : "<null>")
                  << ", Similar_Find=" << (find ? find : "<null>")
                  << ", Similar_Indexing=" << (indexing ? indexing : "<null>")
                  << ", Similar_ColumnScore=" << (score ? score : "<null>") << '\n';

        // Auth_ContactAdmin is intentionally empty in both production arrays:
        // the open-source build does not require administrator authorization.
        if (missing != 1 || !L(Auth_ContactAdmin) || *L(Auth_ContactAdmin)) {
            std::cerr << "FAIL: unexpected localization completeness baseline for "
                      << languageName << '\n';
            ++failures;
        }
        if (!title || std::strcmp(title, expectedTitle) != 0 ||
            !find || std::strcmp(find, expectedFind) != 0 ||
            !indexing || std::strcmp(indexing, expectedIndexing) != 0 ||
            !score || std::strcmp(score, expectedScore) != 0) {
            std::cerr << "FAIL: similarity localization mapping for "
                      << languageName << '\n';
            ++failures;
        }
        // First-cut freeze: do not rewrite these four visible labels.
        const char* expectedInfo = language == AppLanguage::Chinese ? u8"Info" : "Info";
        const char* expectedDemo = language == AppLanguage::Chinese ? u8"Demo窗口" : "Demo Window";
        const char* expectedDetach = language == AppLanguage::Chinese ? u8"Info独立窗口" : "Info Detached Window";
        const char* expectedLanguage = language == AppLanguage::Chinese ? u8"界面语言 / Language:" : "UI Language / Language:";
        if (std::strcmp(L(View_InfoPanel), expectedInfo) != 0 ||
            std::strcmp(L(View_DemoWindow), expectedDemo) != 0 ||
            std::strcmp(L(Panel_InfoDetach), expectedDetach) != 0 ||
            std::strcmp(L(Set_UILanguage), expectedLanguage) != 0) {
            std::cerr << "FAIL: frozen menu labels changed for "
                      << languageName << '\n';
            ++failures;
        }
    }
    g_currentLanguage = AppLanguage::Japanese;
    {
        const int missingJa = CheckTranslationCompleteness(AppLanguage::Japanese);
        const char* title = L(Similar_Title);
        std::cout << "Japanese: completeness_missing=" << missingJa
                  << ", Similar_Title=" << (title ? title : "<null>") << '\n';
        if (missingJa != 1) {
            std::cerr << "FAIL: Japanese completeness should leave only Auth_ContactAdmin empty, got "
                      << missingJa << "\n";
            ++failures;
        }
        if (!title || std::strcmp(title, u8"類似サウンド") != 0) {
            std::cerr << "FAIL: Japanese copy should show the ledger ja value\n";
            ++failures;
        }
        if (std::strcmp(L(View_InfoPanel), "Info") != 0 ||
            std::strcmp(L(View_DemoWindow), u8"デモウィンドウ") != 0 ||
            std::strcmp(L(Panel_InfoDetach), u8"Info 独立ウィンドウ") != 0 ||
            std::strcmp(L(Set_UILanguage), u8"表示言語 / Language:") != 0) {
            std::cerr << "FAIL: frozen labels' ja values changed\n";
            ++failures;
        }
    }
    // --- Knife-6 W6-1: wide accessor (Lw) contract ---------------------
    g_currentLanguage = AppLanguage::Chinese;
    {
        if (GetLocalizedStringW(LStr::View_InfoPanel) != std::wstring(L"Info")) {
            std::cerr << "FAIL: Lw ASCII\n"; ++failures;
        }
        if (GetLocalizedStringW(LStr::Similar_Title) != std::wstring(L"相似音效")) {
            std::cerr << "FAIL: Lw CJK zh\n"; ++failures;
        }
        const std::wstring save = GetLocalizedStringW(LStr::Main_WriteCoverProgress);  // starts with U+1F4BE
        if (save.size() < 2 || save[0] != 0xD83D || save[1] != 0xDCBE) {
            std::cerr << "FAIL: Lw astral surrogate pair\n"; ++failures;
        }
        if (!GetLocalizedStringW(LStr::Auth_ContactAdmin).empty()) {  // intentionally-empty cell
            std::cerr << "FAIL: Lw empty cell must stay empty, no crash\n"; ++failures;
        }
        if (GetLocalizedStringW(static_cast<LStr>(-1)) != std::wstring(L"???")) {
            std::cerr << "FAIL: Lw negative-id fallback\n"; ++failures;
        }
        if (GetLocalizedStringW(static_cast<LStr>(999999)) != std::wstring(L"???")) {
            std::cerr << "FAIL: Lw out-of-range fallback\n"; ++failures;
        }
    }
    g_currentLanguage = AppLanguage::Japanese;
    {
        const std::wstring demo = GetLocalizedStringW(LStr::View_DemoWindow);  // デモウィンドウ
        if (demo.empty() || demo[0] != L'デ') {
            std::cerr << "FAIL: Lw ja\n"; ++failures;
        }
    }
    g_currentLanguage = AppLanguage::Chinese;
    // --------------------------------------------------------------------
    if (failures) std::cerr << "FAIL: similarity localization contract\n";
    else std::cout << "PASS: similarity localization contract\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
