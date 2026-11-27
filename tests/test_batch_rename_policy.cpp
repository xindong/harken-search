#include "BatchRenamePolicy.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

RenameRule Rule(
    RenameMode mode,
    std::string search = {},
    std::string replace = {}) {
    RenameRule rule;
    rule.mode = mode;
    rule.searchText = std::move(search);
    rule.replaceText = std::move(replace);
    return rule;
}

aura::rename::PreviewLabels Labels() {
    return {
        "NO_RULES",
        "NO_CHANGE",
        "EMPTY",
        "DUPLICATE",
        "EXISTS",
        "READY",
        u8"后缀",
    };
}

struct TempDirectory {
    fs::path path;

    TempDirectory() {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
            ("aris-batch-rename-policy-" + std::to_string(stamp));
        fs::create_directories(path);
    }

    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

} // namespace

int main() {
    using aura::rename::ApplyRule;
    using aura::rename::ApplyRulesToFileName;
    using aura::rename::BuildPreview;
    using aura::rename::PreviewInput;

    if (!Check(
            ApplyRule(
                "boom_boom",
                Rule(RenameMode::TextReplace, "boom", "hit"),
                0,
                u8"后缀") == "hit_hit",
            "text replacement should preserve all-occurrence order")) return 1;

    if (!Check(
            ApplyRule(
                "take",
                Rule(RenameMode::RegexReplace, "[", "x"),
                0,
                u8"后缀") == "take",
            "invalid regex should leave the input unchanged")) return 1;

    if (!Check(
            ApplyRule(
                u8"中文ABC",
                Rule(RenameMode::DeleteStartChars, "2"),
                0,
                u8"后缀") == "ABC",
            "front deletion should count UTF-8 characters")) return 1;
    if (!Check(
            ApplyRule(
                u8"中文ABC",
                Rule(RenameMode::DeleteEndChars, "3"),
                0,
                u8"后缀") == u8"中文",
            "end deletion should count UTF-8 characters")) return 1;
#ifdef __APPLE__
    if (!Check(
            ApplyRulesToFileName(
                u8"🎵中文.wav",
                {Rule(RenameMode::DeleteStartChars, "1")},
                0,
                u8"后缀") == u8"中文.wav",
            "front deletion should remove one supplementary character without changing the extension")) return 1;
    if (!Check(
            ApplyRule(
                u8"中文🎵",
                Rule(RenameMode::DeleteEndChars, "1"),
                0,
                u8"后缀") == u8"中文",
            "end deletion should not split a supplementary character")) return 1;
    if (!Check(
            ApplyRule(u8"中文", Rule(RenameMode::DeleteStartChars, "2"), 0, u8"后缀") == u8"中文" &&
                ApplyRule(u8"中文", Rule(RenameMode::DeleteEndChars, "3"), 0, u8"后缀") == u8"中文",
            "character deletion should leave a stem unchanged if nothing would remain")) return 1;
    const std::string incompleteName = "take\xE4\xB8";
    if (!Check(
            ApplyRule(incompleteName, Rule(RenameMode::DeleteEndChars, "1"), 0, u8"后缀") == incompleteName,
            "character deletion should leave invalid UTF-8 unchanged")) return 1;
#endif

    if (!Check(
            ApplyRule(
                "a_cut_b_cut_c",
                Rule(RenameMode::DeleteFromMatchedTextToEnd, "cut", "first"),
                0,
                u8"后缀") == "a_",
            "delete-to-end should use the first match by default")) return 1;
    if (!Check(
            ApplyRule(
                "a_cut_b_cut_c",
                Rule(RenameMode::DeleteFromMatchedTextToEnd, "cut", "last"),
                0,
                u8"后缀") == "a_cut_b_",
            "delete-to-end should support the last match")) return 1;

    if (!Check(
            ApplyRule(
                "take",
                Rule(RenameMode::InsertSequenceNumber, "003", u8"后缀"),
                7,
                u8"后缀") == "take007",
            "localized suffix token should preserve suffix placement")) return 1;
    if (!Check(
            ApplyRule(
                "take",
                Rule(RenameMode::InsertSequenceNumber, "003", "suffix"),
                7,
                u8"后缀") == "take007",
            "literal suffix token should preserve suffix placement")) return 1;

    if (!Check(
            ApplyRulesToFileName(
                "take.v1.wav",
                {Rule(RenameMode::AddPrefix, "PRE_")},
                1,
                u8"后缀") == "PRE_take.v1.wav",
            "rules should transform the stem and append the extension once")) return 1;

    TempDirectory temp;
    const std::string root = temp.path.u8string();
    const auto input = [&](const std::string& fileName, int64_t id) {
        return PreviewInput{
            fileName,
            (temp.path / fs::u8path(fileName)).u8string(),
            id,
        };
    };

    auto previews = BuildPreview({input("plain.wav", 1)}, {}, Labels());
    if (!Check(
            previews.size() == 1 && previews[0].newName == "plain.wav" &&
                !previews[0].willChange && previews[0].status == "NO_RULES",
            "empty rule queue should preserve the no-rules preview")) return 1;

    previews = BuildPreview(
        {input("plain.wav", 1)},
        {Rule(RenameMode::TextReplace, "missing", "x")},
        Labels());
    if (!Check(
            previews.size() == 1 && !previews[0].willChange &&
                previews[0].status == "NO_CHANGE",
            "unchanged output should preserve the no-change preview")) return 1;

    previews = BuildPreview(
        {input("clip", 1)},
        {Rule(RenameMode::DeleteText, "clip")},
        Labels());
    if (!Check(
            previews.size() == 1 && previews[0].newName.empty() &&
                !previews[0].willChange && previews[0].status == "EMPTY",
            "empty output should be rejected before conflict checks")) return 1;

    previews = BuildPreview(
        {input("same.wav", 1), input("same.wav", 2)},
        {Rule(RenameMode::AddPrefix, "new_")},
        Labels());
    if (!Check(
            previews.size() == 2 && previews[0].willChange &&
                previews[0].status == "READY" && !previews[1].willChange &&
                previews[1].status == "DUPLICATE",
            "only the first ready name should reserve a duplicate target")) return 1;

    {
        std::ofstream existing(temp.path / "new_exists.wav", std::ios::binary);
        existing.put('x');
    }
    previews = BuildPreview(
        {input("exists.wav", 3)},
        {Rule(RenameMode::AddPrefix, "new_")},
        Labels());
    if (!Check(
            previews.size() == 1 && !previews[0].willChange &&
                previews[0].status == "EXISTS",
            "on-disk target should be classified after pure conflicts")) return 1;

    std::cout << "PASS: batch rename rule and preview policy contracts\n";
    return 0;
}
