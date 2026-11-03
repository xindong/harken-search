// SoundManager - 国际化模块
// Localization.cpp - 多语言字符串管理 (中文/英文)
// 中英表由 locale/harken.ui.strings.json 在编译期生成，运行时不读 JSON。

#include "Localization.h"
#include "BuildConfig.h"
#include <cstdio>
#include <string>

// 内测版（AURA_INTERNAL_BETA=1）：剥离开发者 UI 相关本地化字符串的敏感词
// （dev_mode.txt / settings_dev.ini / imgui_dev.ini 等），保证 exe 的 .rdata 段
// 不含这些字面量。对应 UI 入口已在 main.cpp 中被 #if 门控，运行时永不渲染。
#if AURA_INTERNAL_BETA
  #define AURA_DEV_LOC_ZH(s) u8"（内测版不显示）"
  #define AURA_DEV_LOC_EN(s) "(hidden in beta)"
#else
  #define AURA_DEV_LOC_ZH(s) s
  #define AURA_DEV_LOC_EN(s) s
#endif

// Global Language State

AppLanguage g_currentLanguage = AppLanguage::Chinese;

#include "LocalizationStrings.generated.h"

static_assert(
    sizeof(g_strings_zh) / sizeof(g_strings_zh[0]) == static_cast<size_t>(LStr::_Count),
    "generated zh table must match LStr");
static_assert(
    sizeof(g_strings_en) / sizeof(g_strings_en[0]) == static_cast<size_t>(LStr::_Count),
    "generated en table must match LStr");
static_assert(
    sizeof(g_strings_ja) / sizeof(g_strings_ja[0]) == static_cast<size_t>(LStr::_Count),
    "generated ja table must match LStr");

const char* GetLocalizedString(LStr id) {
    return GetLocalizedString(id, g_currentLanguage);
}

const char* GetLocalizedString(LStr id, AppLanguage language) {
    int idx = (int)id;
    if (idx < 0 || idx >= (int)LStr::_Count) return "???";
    switch (language) {
    case AppLanguage::Chinese:  return g_strings_zh[idx];
    case AppLanguage::Japanese: {
        const char* ja = g_strings_ja[idx];
        if (ja && ja[0] != '\0') return ja;
        return g_strings_en[idx];
    }
    default: return g_strings_en[idx];
    }
}

const char* GetMagicEQModeLabel(bool enabled, int mode) {
    if (!enabled) return L(MagicEQ_ModeBypass);
    if (mode == 1) return L(MagicEQ_ModeNotch);
    return L(MagicEQ_ModePass);
}

const char* GetMagicEQFrequencyLabel(float lowHz, float highHz) {
    static thread_local char buffer[64];
    const auto formatFrequency = [](float hz, char* output, size_t outputSize) {
        if (hz >= 1000.0f) std::snprintf(output, outputSize, "%.1fk", hz / 1000.0f);
        else std::snprintf(output, outputSize, "%.0f", hz);
    };
    char low[16] = {}, high[16] = {};
    formatFrequency(lowHz, low, sizeof(low));
    formatFrequency(highHz, high, sizeof(high));
    std::snprintf(buffer, sizeof(buffer), "%s–%s Hz", low, high);
    return buffer;
}

// Knife-6 W6-1: UTF-8 (const char* ledger cell) -> platform wide string.
// Written defensive: never reads past the NUL / buffer end, never loops
// forever, maps malformed bytes to U+FFFD. On Win32 wchar_t==16 so astral
// code points are encoded as UTF-16 surrogate pairs; elsewhere wchar_t==32.
static void AppendWide(std::wstring& out, unsigned cp) {
    if (sizeof(wchar_t) == 2) {
        if (cp < 0x10000) {
            out.push_back(static_cast<wchar_t>(cp));
        } else if (cp <= 0x10FFFF) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(0xFFFD));
        }
    } else {
        out.push_back(static_cast<wchar_t>(cp));
    }
}

std::wstring GetLocalizedStringW(LStr id) {
    const char* s = GetLocalizedString(id);   // handles out-of-range -> "???", ja-empty -> en
    if (!s) return std::wstring(L"???");
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    size_t n = 0;
    while (p[n]) ++n;
    std::wstring out;
    size_t i = 0;
    while (i < n) {
        unsigned b0 = p[i];
        unsigned cp;
        size_t len;
        if (b0 < 0x80)            { cp = b0 & 0x7Fu;      len = 1; }
        else if ((b0 >> 5) == 0x6){ cp = b0 & 0x1Fu;      len = 2; }
        else if ((b0 >> 4) == 0xE){ cp = b0 & 0x0Fu;      len = 3; }
        else if ((b0 >> 3) == 0x1E){ cp = b0 & 0x07u;     len = 4; }
        else                      { cp = 0xFFFDu;         len = 1; }  // invalid lead byte
        if (len > 1 && i + len > n) { AppendWide(out, 0xFFFDu); ++i; continue; }  // truncated
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            unsigned bk = p[i + k];
            if ((bk >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (bk & 0x3Fu);
        }
        const bool overlong =
            (len == 2 && cp < 0x80u) || (len == 3 && cp < 0x800u) ||
            (len == 4 && cp < 0x10000u);
        const bool bad = !ok || overlong || cp > 0x10FFFFu ||
                         (cp >= 0xD800u && cp <= 0xDFFFu);
        AppendWide(out, bad ? 0xFFFDu : cp);
        i += bad ? 1u : len;   // resync one byte on any error; always advances
    }
    return out;
}
int GetTotalStringCount() {
    return (int)LStr::_Count;
}

int CheckTranslationCompleteness(AppLanguage lang) {
    int total = (int)LStr::_Count;
    int missing = 0;
    const char** arr = nullptr;
    switch (lang) {
    case AppLanguage::Chinese:  arr = g_strings_zh; break;
    case AppLanguage::English:  arr = g_strings_en; break;
    case AppLanguage::Japanese: arr = g_strings_ja; break;
    }
    if (!arr) return total;
    for (int i = 0; i < total; ++i) {
        if (!arr[i] || arr[i][0] == '\0') missing++;
    }
    return missing;
}
