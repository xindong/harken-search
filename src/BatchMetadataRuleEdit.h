#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Unified batch edit (REQ-20261126-UNIFIED-BATCH-EDIT).
// One target column per batch: each row starts from its stored value, the
// ordered rules transform it, and the preview is the only thing the write
// path may commit. Disk file names are handled by the existing rename path.

namespace aura::metadata {

enum class RuleEditColumn {
    FileName = 0,
    Title,
    Description,
    Keywords,
    Category
};

enum class RuleEditMode {
    Replace = 0,
    AddPrefix,
    AddSuffix,
    SetFixed,
    AppendBefore,
    AppendAfter
};

// Database column names accepted by the guarded batch write.
const char* RuleEditColumnName(RuleEditColumn column);
bool IsAllowedRuleEditColumn(const std::string& columnName);

struct RuleEditRule {
    RuleEditMode mode = RuleEditMode::Replace;
    std::string value;        // replace: search text; affix/append: text; fixed: unused
    std::string replacement;  // replace: replacement; fixed: the new value
    std::string separator;    // append: separator between old and new text
    bool enabled = true;
};

// Minimal projection of an audio record: only what the editor needs.
struct RuleEditRecord {
    int64_t id = 0;
    std::string fileName;
    std::string title;
    std::string description;
    std::string keywords;
    std::string category;
};

struct RuleEditRow {
    int64_t id = 0;
    std::string fileName;
    std::string oldValue;
    std::string newValue;
    bool changed = false;
    bool blockedEmpty = false;   // non-blank value would become blank
    std::string error;           // this row cannot be produced
};

struct RuleEditPreview {
    RuleEditColumn column = RuleEditColumn::Title;
    std::string signature;
    std::vector<RuleEditRow> rows;
    int changeCount = 0;
    int unchangedCount = 0;
    int blockedCount = 0;
    int errorCount = 0;

    bool HasChanges() const { return changeCount > 0; }
    bool CanApply() const {
        return !rows.empty() && blockedCount == 0 && errorCount == 0 && changeCount > 0;
    }
};

// Stable fingerprint: target column, participating ids and stored values, plus
// enabled rules in order. A preview is valid only while it keeps matching.
std::string BuildRuleEditSignature(
    RuleEditColumn column,
    const std::vector<RuleEditRecord>& records,
    const std::vector<RuleEditRule>& rules);

RuleEditPreview BuildRuleEditPreview(
    RuleEditColumn column,
    const std::vector<RuleEditRecord>& records,
    const std::vector<RuleEditRule>& rules);

} // namespace aura::metadata

