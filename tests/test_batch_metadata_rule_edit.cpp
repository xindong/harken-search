// tests/test_batch_metadata_rule_edit.cpp
// REQ-20261126-UNIFIED-BATCH-EDIT: preview logic for the unified batch editor.
// Pure logic only — the guarded database write is covered by
// tests/test_batch_metadata_rule_edit_contract.py and the isolated-library run.

#include <cstdio>
#include <string>
#include <vector>

#include "src/BatchMetadataRuleEdit.h"

static int g_failures = 0;
static void check(bool cond, const char* desc) {
    if (cond) {
        std::printf("PASS: %s\n", desc);
    } else {
        std::printf("FAIL: %s\n", desc);
        ++g_failures;
    }
}

namespace md = aura::metadata;

static md::RuleEditRecord record(int64_t id, const char* name, const char* title,
                                 const char* description, const char* keywords = "",
                                 const char* category = "") {
    md::RuleEditRecord r;
    r.id = id;
    r.fileName = name;
    r.title = title;
    r.description = description;
    r.keywords = keywords;
    r.category = category;
    return r;
}

static md::RuleEditRule rule(md::RuleEditMode mode, const char* value,
                             const char* replacement = "", const char* separator = "") {
    md::RuleEditRule r;
    r.mode = mode;
    r.value = value;
    r.replacement = replacement;
    r.separator = separator;
    return r;
}

int main() {
    const std::vector<md::RuleEditRecord> records = {
        record(1, "Step_Wood_01.wav", "Step_Wood_01", "footstep on wood", "step, wood"),
        record(2, "Wind_03.wav", "Wind_03", "Step_Wood_01", "wind"),
        record(3, "Step_Wood_04.wav", "Step_Wood_04", "", ""),
    };
    const std::vector<md::RuleEditRule> chain = {
        rule(md::RuleEditMode::Replace, "Step_", "Footstep_"),
        rule(md::RuleEditMode::Replace, "Footstep_Wood", "Footstep_Plank"),
    };

    md::RuleEditPreview forward =
        md::BuildRuleEditPreview(md::RuleEditColumn::Title, records, chain);
    check(forward.rows.size() == 3, "preview keeps one row per selected record");
    check(forward.rows[0].newValue == "Footstep_Plank_01",
          "rules apply in list order (second rule sees first rule output)");
    check(forward.rows[1].newValue == "Wind_03" && !forward.rows[1].changed,
          "unmatched value stays unchanged");
    check(forward.changeCount == 2 && forward.unchangedCount == 1,
          "change counters reflect per-row results");
    check(forward.CanApply(), "preview with changes can be applied");

    const std::vector<md::RuleEditRule> reversed = {chain[1], chain[0]};
    md::RuleEditPreview backward =
        md::BuildRuleEditPreview(md::RuleEditColumn::Title, records, reversed);
    check(backward.rows[0].newValue == "Footstep_Wood_01",
          "swapping rule order changes the previewed result");

    md::RuleEditPreview description =
        md::BuildRuleEditPreview(md::RuleEditColumn::Description, records, chain);
    check(description.rows[1].newValue == "Footstep_Plank_01",
          "description column starts from the stored description");
    check(!description.rows[0].changed,
          "description without a match is untouched even when its title matches");

    const std::vector<md::RuleEditRule> fixed = {rule(md::RuleEditMode::SetFixed, "", "Shared")};
    md::RuleEditPreview fixedPreview =
        md::BuildRuleEditPreview(md::RuleEditColumn::Category, records, fixed);
    check(fixedPreview.changeCount == 3 && fixedPreview.rows[2].newValue == "Shared",
          "set fixed value writes the same value to every selected row");

    const std::vector<md::RuleEditRule> appendAfter = {rule(md::RuleEditMode::AppendAfter, "wood", "", ", ")};
    md::RuleEditPreview appendPreview =
        md::BuildRuleEditPreview(md::RuleEditColumn::Keywords, records, appendAfter);
    check(appendPreview.rows[0].newValue == "step, wood, wood",
          "append after keeps the old value and adds separator + text");
    check(appendPreview.rows[2].newValue == "wood",
          "append on a blank value writes only the new text");

    const std::vector<md::RuleEditRule> appendBefore = {rule(md::RuleEditMode::AppendBefore, "sfx", "", " ")};
    md::RuleEditPreview beforePreview =
        md::BuildRuleEditPreview(md::RuleEditColumn::Keywords, records, appendBefore);
    check(beforePreview.rows[1].newValue == "sfx wind",
          "append before puts the text first");

    const std::vector<md::RuleEditRule> affixes = {
        rule(md::RuleEditMode::AddPrefix, "SFX_"),
        rule(md::RuleEditMode::AddSuffix, "_v2"),
    };
    md::RuleEditPreview affixPreview =
        md::BuildRuleEditPreview(md::RuleEditColumn::Title, {records[1]}, affixes);
    check(affixPreview.rows[0].newValue == "SFX_Wind_03_v2",
          "prefix and suffix rules chain in order");

    const std::vector<md::RuleEditRule> emptySearch = {rule(md::RuleEditMode::Replace, "", "x")};
    md::RuleEditPreview brokenPreview =
        md::BuildRuleEditPreview(md::RuleEditColumn::Title, records, emptySearch);
    check(brokenPreview.errorCount == 3 && !brokenPreview.CanApply(),
          "empty search text marks every row as invalid");

    const std::vector<md::RuleEditRule> wipe = {rule(md::RuleEditMode::Replace, "Wind_03", "")};
    md::RuleEditPreview wipePreview =
        md::BuildRuleEditPreview(md::RuleEditColumn::Title, {records[1]}, wipe);
    check(wipePreview.rows[0].blockedEmpty && !wipePreview.CanApply(),
          "clearing a non-blank value is blocked instead of written");

    const std::vector<md::RuleEditRecord> blank = {record(9, "a.wav", "   ", "")};
    md::RuleEditPreview blankPreview =
        md::BuildRuleEditPreview(md::RuleEditColumn::Title, blank, chain);
    check(!blankPreview.rows[0].changed && !blankPreview.CanApply(),
          "blank stored value stays blank and is never written");

    std::vector<md::RuleEditRule> disabled = chain;
    disabled[0].enabled = false;
    md::RuleEditPreview disabledPreview =
        md::BuildRuleEditPreview(md::RuleEditColumn::Title, records, disabled);
    check(!disabledPreview.rows[0].changed, "disabled rule does not contribute");

    const std::string signature = md::BuildRuleEditSignature(md::RuleEditColumn::Title, records, chain);
    check(signature == md::BuildRuleEditSignature(md::RuleEditColumn::Title, records, chain),
          "signature is stable for identical conditions");
    std::vector<md::RuleEditRule> editedRules = chain;
    editedRules[1].replacement = "Other";
    check(signature != md::BuildRuleEditSignature(md::RuleEditColumn::Title, records, editedRules),
          "signature changes when a rule changes");
    std::vector<md::RuleEditRecord> editedRecords = records;
    editedRecords[0].title = "Step_Wood_01b";
    check(signature != md::BuildRuleEditSignature(md::RuleEditColumn::Title, editedRecords, chain),
          "signature changes when a stored value changes");
    check(signature != md::BuildRuleEditSignature(md::RuleEditColumn::Keywords, records, chain),
          "signature changes with the target column");

    check(md::IsAllowedRuleEditColumn("title") && md::IsAllowedRuleEditColumn("description") &&
              md::IsAllowedRuleEditColumn("keywords") && md::IsAllowedRuleEditColumn("category"),
          "title, description, keywords and category are allowed guard columns");
    check(!md::IsAllowedRuleEditColumn("duration") && !md::IsAllowedRuleEditColumn("sample_rate"),
          "technical columns are rejected as guard targets");

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}

