#include "BatchMetadataRuleEdit.h"

#include <cctype>

namespace aura::metadata {

namespace {

bool IsBlank(const std::string& text) {
    for (const unsigned char ch : text) {
        if (ch > 0x7f) return false;              // multi-byte UTF-8 is content
        if (std::isspace(ch) == 0) return false;
    }
    return true;
}

std::string RuleSignature(const RuleEditRule& rule) {
    if (!rule.enabled) return std::string();
    std::string out = std::to_string(static_cast<int>(rule.mode));
    out += ':';
    out += rule.value;
    out += '>';
    out += rule.replacement;
    out += '|';
    out += rule.separator;
    return out;
}

std::string StoredValue(RuleEditColumn column, const RuleEditRecord& record) {
    switch (column) {
    case RuleEditColumn::FileName:    return record.fileName;
    case RuleEditColumn::Description: return record.description;
    case RuleEditColumn::Keywords:    return record.keywords;
    case RuleEditColumn::Category:    return record.category;
    case RuleEditColumn::Title:
    default:                          return record.title;
    }
}

void ReplaceAll(std::string& text, const std::string& search, const std::string& replacement) {
    size_t pos = 0;
    while ((pos = text.find(search, pos)) != std::string::npos) {
        text.replace(pos, search.length(), replacement);
        pos += replacement.length();
    }
}

std::string ApplyRuleToText(const std::string& input, const RuleEditRule& rule, bool& usable) {
    std::string result = input;
    switch (rule.mode) {
    case RuleEditMode::Replace:
        if (rule.value.empty()) {
            usable = false;
            return input;
        }
        ReplaceAll(result, rule.value, rule.replacement);
        break;
    case RuleEditMode::AddPrefix:
        result = rule.value + result;
        break;
    case RuleEditMode::AddSuffix:
        result += rule.value;
        break;
    case RuleEditMode::SetFixed:
        result = rule.replacement;
        break;
    case RuleEditMode::AppendBefore:
        result = IsBlank(result) ? rule.value : rule.value + rule.separator + result;
        break;
    case RuleEditMode::AppendAfter:
        result = IsBlank(result) ? rule.value : result + rule.separator + rule.value;
        break;
    }
    return result;
}

} // namespace

const char* RuleEditColumnName(RuleEditColumn column) {
    switch (column) {
    case RuleEditColumn::FileName:    return "file_name";
    case RuleEditColumn::Description: return "description";
    case RuleEditColumn::Keywords:    return "keywords";
    case RuleEditColumn::Category:    return "category";
    case RuleEditColumn::Title:
    default:                          return "title";
    }
}

bool IsAllowedRuleEditColumn(const std::string& columnName) {
    return columnName == "title" || columnName == "description" ||
           columnName == "keywords" || columnName == "category";
}

std::string BuildRuleEditSignature(
    RuleEditColumn column,
    const std::vector<RuleEditRecord>& records,
    const std::vector<RuleEditRule>& rules) {
    std::string signature = RuleEditColumnName(column);
    signature += '|';
    for (const auto& record : records) {
        signature += std::to_string(record.id);
        signature += ':';
        signature += StoredValue(column, record);
        signature += '|';
    }
    for (const auto& rule : rules) {
        signature += RuleSignature(rule);
        signature += '|';
    }
    return signature;
}

RuleEditPreview BuildRuleEditPreview(
    RuleEditColumn column,
    const std::vector<RuleEditRecord>& records,
    const std::vector<RuleEditRule>& rules) {
    RuleEditPreview preview;
    preview.column = column;
    preview.signature = BuildRuleEditSignature(column, records, rules);
    preview.rows.reserve(records.size());

    for (const auto& record : records) {
        RuleEditRow row;
        row.id = record.id;
        row.fileName = record.fileName;
        row.oldValue = StoredValue(column, record);
        row.newValue = row.oldValue;

        bool usable = true;
        for (const auto& rule : rules) {
            if (!rule.enabled) continue;
            row.newValue = ApplyRuleToText(row.newValue, rule, usable);
            if (!usable) break;
        }

        if (!usable) {
            row.error = "invalid rule";
        } else {
            row.changed = row.newValue != row.oldValue;
            row.blockedEmpty = row.changed && !IsBlank(row.oldValue) && IsBlank(row.newValue);
        }

        if (!row.error.empty()) preview.errorCount++;
        else if (row.blockedEmpty) preview.blockedCount++;
        else if (row.changed) preview.changeCount++;
        else preview.unchangedCount++;

        preview.rows.push_back(std::move(row));
    }

    return preview;
}

} // namespace aura::metadata

