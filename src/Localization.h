#pragma once
// SoundManager - 国际化模块
// Localization.h - 多语言字符串管理 (中文/英文)
// LStr 枚举由 locale/harken.ui.strings.json 在编译期生成，不要手改生成头。

// @stage7-contract
// API contract: localization identifiers are stable enum values and lookup
// functions return owned display strings. Changing the current language is a
// process-wide UI operation; callers perform it on the UI thread and must not
// retain references to mutable localization storage.

// Language Options

enum class AppLanguage {
    Chinese = 0,   // 中文
    English = 1,   // English
    Japanese = 2   // 日本語 (F-021)
};

// Current language (defined in Localization.cpp)
extern AppLanguage g_currentLanguage;

#include "LocalizationGenerated.h"
#include <string>

// Localization Functions

// Get localized string by ID
const char* GetLocalizedString(LStr id);
// Explicit-language lookup for multilingual indexes; does not change UI language.
const char* GetLocalizedString(LStr id, AppLanguage language);
const char* GetMagicEQModeLabel(bool enabled, int mode);
const char* GetMagicEQFrequencyLabel(float lowHz, float highHz);

// Shortcut macro for localization
#define L(id) GetLocalizedString(LStr::id)

// Knife-6 W6-1: wide (UTF-16 on Win32) accessor for the Win32 native UI
// surface (SetWindowTextW / MessageBoxW / tray / common dialogs). The ledger
// tables are compile-time generated and always resident, but this entry point
// is written defensively so a not-ready / out-of-range / malformed input never
// crashes the startup / crash-reporting dialogs that are the whole point of a
// pre-initialization fallback path:
//   - out-of-range id or null table cell  -> returns L"???"
//   - Japanese cell empty                  -> falls back to English (same as L())
//   - malformed UTF-8 bytes                -> U+FFFD replacement, no crash
// Returns by value; the caller owns the buffer. Call on the UI thread (same
// contract as L()/g_currentLanguage).
std::wstring GetLocalizedStringW(LStr id);

// Wide shortcut macro. Use as SetWindowTextW(hwnd, Lw(id).c_str()).
#define Lw(id) GetLocalizedStringW(LStr::id)

// H1b (RQ-20260911-04, architecture constraint 4): worker-safe ledger text.
// Workers must NOT call L()/GetLocalizedString (unsynchronized g_currentLanguage
// read). They build a LedgerText storing the ledger KEY + runtime args; the UI
// render site resolves it on the main thread via Render(). Plain/dynamic text
// keeps working through the implicit std::string paths verbatim.
#include <cstdio>
#include <vector>
struct LedgerText {
    struct Part {
        bool keyed = false;
        LStr key{};
        unsigned char argMode = 0;  // 0 none, 1 int, 2 string
        int n = 0;
        std::string arg;
        std::string lit;            // literal / dynamic content (rendered verbatim)
    };
    std::vector<Part> parts;        // 1-2 parts at product sites
    LedgerText() = default;
    LedgerText(const char* lit) { Part p; p.lit = lit ? lit : ""; parts.push_back(std::move(p)); }
    LedgerText(std::string lit) { Part p; p.lit = std::move(lit); parts.push_back(std::move(p)); }
    static LedgerText keyed(LStr k) { LedgerText t; t.setKey(k); return t; }
    void setKey(LStr k) { parts.clear(); Part p; p.keyed = true; p.key = k; parts.push_back(std::move(p)); }
    void setKeyInt(LStr k, int v) { parts.clear(); Part p; p.keyed = true; p.key = k; p.argMode = 1; p.n = v; parts.push_back(std::move(p)); }
    void setKeyStr(LStr k, std::string s) { parts.clear(); Part p; p.keyed = true; p.key = k; p.argMode = 2; p.arg = std::move(s); parts.push_back(std::move(p)); }
    void clear() { parts.clear(); }
    bool empty() const {
        for (const Part& p : parts) if (p.keyed || !p.lit.empty()) return false;
        return true;
    }
    bool operator==(const char* lit) const {
        return parts.size() == 1 && !parts[0].keyed && parts[0].lit == lit;
    }
    bool operator!=(const char* lit) const { return !(*this == lit); }
    LedgerText& operator=(std::string lit) { parts.clear(); parts.push_back(Part{}); parts[0].lit = std::move(lit); return *this; }
    LedgerText& operator=(const char* lit) { parts.clear(); parts.push_back(Part{}); parts[0].lit = lit ? lit : ""; return *this; }
    LedgerText& operator+=(const std::string& more) {
        if (!more.empty()) {
            if (!parts.empty() && !parts.back().keyed) parts.back().lit += more;
            else { Part p; p.lit = more; parts.push_back(std::move(p)); }
        }
        return *this;
    }
    LedgerText& operator+=(const LedgerText& more) {
        for (const Part& p : more.parts) parts.push_back(p);
        return *this;
    }
    // UI thread only (reads g_currentLanguage, same contract as L()).
    std::string Render() const {
        std::string out;
        for (const Part& p : parts) {
            if (!p.keyed) { out += p.lit; continue; }
            const char* fmt = GetLocalizedString(p.key);
            if (p.argMode == 0) { out += fmt; continue; }
            char buf[2048];
            if (p.argMode == 1) std::snprintf(buf, sizeof(buf), fmt, p.n);
            else std::snprintf(buf, sizeof(buf), fmt, p.arg.c_str());
            out += buf;
        }
        return out;
    }
};

// 翻译完整性校验 (F-021)
/// 返回指定语言中缺失翻译（空字符串）的 LStr ID 数量
int CheckTranslationCompleteness(AppLanguage lang);
/// 返回总字符串 ID 数
int GetTotalStringCount();
