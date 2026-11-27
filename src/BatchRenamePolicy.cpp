#include "BatchRenamePolicy.h"

#include "Utils.h"
#include "platform/filesystem.h"
#ifdef __APPLE__
#include "platform/text_encoding.h"
#endif

#include <cstdio>
#include <regex>
#include <set>
#include <utility>

namespace aura::rename {

#ifdef __APPLE__
static void DeleteMacUtf8Chars(std::string& name, int count, bool fromStart) {
    if (count <= 0) return;
    auto characters = Platform::TextEncoding::Utf8ToWideStrict(name);
    const auto length = static_cast<std::size_t>(count);
    if (length >= characters.size() ||
        Platform::TextEncoding::WideToUtf8Strict(characters) != name) {
        return;
    }
    characters.erase(fromStart ? 0 : characters.size() - length, length);
    name = Platform::TextEncoding::WideToUtf8Strict(characters);
}
#endif

// Check if a character is Chinese
static bool IsChineseCharBR(wchar_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF) ||   // CJK Unified Ideographs
           (c >= 0x3400 && c <= 0x4DBF) ||   // CJK Extension A
           (c >= 0x20000 && c <= 0x2A6DF);   // CJK Extension B
}

// Apply a single rename rule to a filename (without extension)
std::string ApplyRule(
    const std::string& nameWithoutExt,
    const RenameRule& rule,
    int sequenceNum,
    const std::string& localizedSuffixToken) {
    std::string result = nameWithoutExt;

    switch (rule.mode) {
        case RenameMode::TextReplace:
            if (!rule.searchText.empty()) {
                size_t pos = 0;
                while ((pos = result.find(rule.searchText, pos)) != std::string::npos) {
                    result.replace(pos, rule.searchText.length(), rule.replaceText);
                    pos += rule.replaceText.length();
                }
            }
            break;

        case RenameMode::RegexReplace:
            if (!rule.searchText.empty()) {
                try {
                    std::regex re(rule.searchText);
                    result = std::regex_replace(result, re, rule.replaceText);
                } catch (...) {
                    // Invalid regex, return unchanged
                }
            }
            break;

        case RenameMode::DeleteText:
            if (!rule.searchText.empty()) {
                size_t pos;
                while ((pos = result.find(rule.searchText)) != std::string::npos) {
                    result.erase(pos, rule.searchText.length());
                }
            }
            break;

        case RenameMode::AddPrefix:
            result = rule.searchText + result;
            break;

        case RenameMode::AddSuffix:
            result = result + rule.searchText;
            break;

        case RenameMode::DeleteStartChars:
            {
                int n = 0;
                try { n = std::stoi(rule.searchText); } catch (...) {}
#ifdef __APPLE__
                DeleteMacUtf8Chars(result, n, true);
#else
                if (n > 0 && n < (int)result.length()) {
                    // Handle UTF-8: convert to wstring, delete, convert back
                    std::wstring wstr = Utf8ToWide(result);
                    if (n < (int)wstr.length()) {
                        wstr = wstr.substr(n);
                    }
                    result = WideToUtf8(wstr);
                }
#endif
            }
            break;

        case RenameMode::DeleteEndChars:
            {
                int n = 0;
                try { n = std::stoi(rule.searchText); } catch (...) {}
#ifdef __APPLE__
                DeleteMacUtf8Chars(result, n, false);
#else
                if (n > 0) {
                    std::wstring wstr = Utf8ToWide(result);
                    if (n < (int)wstr.length()) {
                        wstr = wstr.substr(0, wstr.length() - n);
                    }
                    result = WideToUtf8(wstr);
                }
#endif
            }
            break;

        case RenameMode::DeleteChinese:
            {
                std::wstring wstr = Utf8ToWide(result);
                std::wstring filtered;
                for (wchar_t c : wstr) {
                    if (!IsChineseCharBR(c)) {
                        filtered += c;
                    }
                }
                result = WideToUtf8(filtered);
            }
            break;

        case RenameMode::DeleteEnglish:
            {
                std::wstring wstr = Utf8ToWide(result);
                std::wstring filtered;
                for (wchar_t c : wstr) {
                    if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))) {
                        filtered += c;
                    }
                }
                result = WideToUtf8(filtered);
            }
            break;

        case RenameMode::DeleteNumbers:
            {
                std::wstring wstr = Utf8ToWide(result);
                std::wstring filtered;
                for (wchar_t c : wstr) {
                    if (!(c >= L'0' && c <= L'9')) {
                        filtered += c;
                    }
                }
                result = WideToUtf8(filtered);
            }
            break;

        case RenameMode::DeletePunctuation:
            {
                std::wstring wstr = Utf8ToWide(result);
                std::wstring filtered;
                const std::wstring punctuation = L"!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
                const std::wstring cnPunctuation = L"，。！？、；：""''（）【】《》";
                for (wchar_t c : wstr) {
                    if (punctuation.find(c) == std::wstring::npos &&
                        cnPunctuation.find(c) == std::wstring::npos) {
                        filtered += c;
                    }
                }
                result = WideToUtf8(filtered);
            }
            break;

        case RenameMode::UpperToTitle:
            {
                std::wstring wstr = Utf8ToWide(result);
                bool prevSpace = true;
                for (size_t i = 0; i < wstr.length(); ++i) {
                    if (wstr[i] == L' ') {
                        prevSpace = true;
                    } else if (prevSpace && wstr[i] >= L'A' && wstr[i] <= L'Z') {
                        // Check if entire word is uppercase
                        size_t wordEnd = i + 1;
                        while (wordEnd < wstr.length() && wstr[wordEnd] != L' ' &&
                               wstr[wordEnd] >= L'A' && wstr[wordEnd] <= L'Z') {
                            wordEnd++;
                        }
                        if (wordEnd > i + 1) {
                            // Convert to title case
                            for (size_t j = i + 1; j < wordEnd; ++j) {
                                wstr[j] = wstr[j] - L'A' + L'a';
                            }
                        }
                        prevSpace = false;
                    } else {
                        prevSpace = false;
                    }
                }
                result = WideToUtf8(wstr);
            }
            break;

        case RenameMode::RemoveExtraBrackets:
            {
                // Remove content in parentheses (), square brackets [], or curly braces {}
                // Based on searchText: () [] {} or empty for all
                std::string pattern;
                if (rule.searchText.empty() || rule.searchText == "()") {
                    pattern = "\\([^)]*\\)";
                } else if (rule.searchText == "[]") {
                    pattern = "\\[[^\\]]*\\]";
                } else if (rule.searchText == "{}") {
                    pattern = "\\{[^}]*\\}";
                } else {
                    pattern = "\\([^)]*\\)|\\[[^\\]]*\\]|\\{[^}]*\\}";
                }
                try {
                    std::regex re(pattern);
                    result = std::regex_replace(result, re, "");
                } catch (...) {}
            }
            break;

        case RenameMode::ExtractBracketContent:
            {
                // Extract content from first [] brackets
                std::regex re("\\[([^\\]]*)\\]");
                std::smatch match;
                if (std::regex_search(result, match, re)) {
                    result = match[1].str();
                }
            }
            break;

        case RenameMode::InsertSequenceNumber:
            {
                // Format: prefix + number + suffix
                // searchText = format like "001" (determines padding)
                // replaceText = position ("prefix" or "suffix")
                int padding = rule.searchText.empty() ? 2 : (int)rule.searchText.length();
                char numBuf[32];
                snprintf(numBuf, sizeof(numBuf), "%0*d", padding, sequenceNum);
                if (rule.replaceText == "suffix" || rule.replaceText == localizedSuffixToken) {
                    result = result + numBuf;
                } else {
                    result = std::string(numBuf) + result;
                }
            }
            break;

        case RenameMode::ToLowercase:
            {
                std::wstring wstr = Utf8ToWide(result);
                for (wchar_t& c : wstr) {
                    if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
                }
                result = WideToUtf8(wstr);
            }
            break;

        case RenameMode::ToUppercase:
            {
                std::wstring wstr = Utf8ToWide(result);
                for (wchar_t& c : wstr) {
                    if (c >= L'a' && c <= L'z') c = c - L'a' + L'A';
                }
                result = WideToUtf8(wstr);
            }
            break;

        case RenameMode::ReplaceSpaces:
            {
                std::string replacement = rule.searchText.empty() ? "_" : rule.searchText;
                size_t pos = 0;
                while ((pos = result.find(' ', pos)) != std::string::npos) {
                    result.replace(pos, 1, replacement);
                    pos += replacement.length();
                }
            }
            break;

        case RenameMode::TrimSpaces:
            {
                // Trim leading spaces
                size_t start = result.find_first_not_of(' ');
                if (start == std::string::npos) {
                    result = "";
                } else {
                    size_t end = result.find_last_not_of(' ');
                    result = result.substr(start, end - start + 1);
                }
            }
            break;

        case RenameMode::DeleteFromMatchedTextToEnd:
            if (!rule.searchText.empty()) {
                const bool useLastMatch = (rule.replaceText == "last");
                const size_t pos = useLastMatch
                    ? result.rfind(rule.searchText)
                    : result.find(rule.searchText);
                if (pos != std::string::npos) {
                    result.erase(pos);
                }
            }
            break;

        default:
            break;
    }

    return result;
}

// Apply all rules in queue to a filename
std::string ApplyRulesToFileName(
    const std::string& fileName,
    const std::vector<RenameRule>& rules,
    int sequenceNum,
    const std::string& localizedSuffixToken) {
    // Split filename and extension
    fs::path p = Utf8ToPath(fileName);
    std::string nameWithoutExt = PathToUtf8(p.stem());
    std::string ext = PathToUtf8(p.extension());

    std::string result = nameWithoutExt;
    for (const auto& rule : rules) {
        result = ApplyRule(result, rule, sequenceNum, localizedSuffixToken);
    }

    return result + ext;
}

std::vector<RenamePreview> BuildPreview(
    const std::vector<PreviewInput>& inputs,
    const std::vector<RenameRule>& rules,
    const PreviewLabels& labels) {
    std::vector<RenamePreview> previews;
    previews.reserve(inputs.size());
    std::set<std::string> newNames;

    int sequenceNumber = 1;
    for (const auto& input : inputs) {
        RenamePreview preview;
        preview.oldName = input.fileName;
        preview.filePath = input.filePath;
        preview.recordId = input.recordId;

        if (rules.empty()) {
            preview.newName = input.fileName;
            preview.willChange = false;
            preview.status = labels.noRules;
        } else {
            preview.newName = ApplyRulesToFileName(
                input.fileName,
                rules,
                sequenceNumber++,
                labels.localizedSuffixToken);
            preview.willChange = preview.newName != preview.oldName;

            if (!preview.willChange) {
                preview.status = labels.noChange;
            } else if (preview.newName.empty()) {
                preview.status = labels.emptyName;
                preview.willChange = false;
            } else if (newNames.find(preview.newName) != newNames.end()) {
                preview.status = labels.duplicate;
                preview.willChange = false;
            } else {
                const fs::path directory = Utf8ToPath(input.filePath).parent_path();
                const fs::path targetPath =
                    directory / Utf8ToPath(preview.newName);
                if (Platform::FileSystem::Exists(PathToUtf8(targetPath))) {
                    preview.status = labels.targetExists;
                    preview.willChange = false;
                } else {
                    preview.status = labels.ready;
                    newNames.insert(preview.newName);
                }
            }
        }

        previews.push_back(std::move(preview));
    }

    return previews;
}

} // namespace aura::rename
