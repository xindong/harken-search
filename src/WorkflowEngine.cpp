// SoundManager - 工作流引擎模块
// WorkflowEngine.cpp - 节点执行、工作流管道实现
// Workflow node/pipeline definitions for batch library operations.

#include "WorkflowEngine.h"
#include "platform/filesystem.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <regex>
#include <ctime>

// Static Data

static std::vector<Workflow> s_workflows;

// Helpers

std::string WorkflowEngine::getMetaField(const std::unordered_map<std::string, std::string>& m, const std::string& key) {
    auto it = m.find(key);
    if (it != m.end()) return it->second;
    return "";
}

// Node Type Info

const char* WorkflowEngine::getNodeTypeName(WorkflowNodeType type) {
    switch (type) {
        case WorkflowNodeType::SetField:            return "Set Field";
        case WorkflowNodeType::CopyField:           return "Copy Field";
        case WorkflowNodeType::FindAndReplace:       return "Find and Replace";
        case WorkflowNodeType::FindAndReplaceWord:   return "Find and Replace Word";
        case WorkflowNodeType::ChangeCase:           return "Change Case";
        case WorkflowNodeType::AddRemoveChars:       return "Add/Remove Chars";
        case WorkflowNodeType::AppendPrependField:   return "Append/Prepend Field";
        case WorkflowNodeType::RemoveWords:          return "Remove Words";
        case WorkflowNodeType::RemoveOccurrences:    return "Remove Occurrences";
        case WorkflowNodeType::RemoveMultipleSpaces: return "Remove Multiple Spaces";
        case WorkflowNodeType::TakeFirstWords:       return "Take First Words";
        case WorkflowNodeType::TakeLastWords:        return "Take Last Words";
        case WorkflowNodeType::SplitAndSort:         return "Split and Sort";
        case WorkflowNodeType::SeparateItems:        return "Separate Items";
        case WorkflowNodeType::SeparateLastItems:    return "Separate Last Items";
        case WorkflowNodeType::CopyFirstRecord:      return "Copy First Record";
        case WorkflowNodeType::CopyFolderName:       return "Copy Folder Name";
        case WorkflowNodeType::FilePathExtractor:    return "FilePath Extractor";
        case WorkflowNodeType::FieldBuild:           return "Field Build";
        case WorkflowNodeType::AutoSplit:            return "Auto Split";
        case WorkflowNodeType::AssignCats:           return "Assign Categories";
        case WorkflowNodeType::UCSBuild:             return "UCS Build";
        case WorkflowNodeType::SetRating:            return "Set Rating";
        case WorkflowNodeType::SetColour:            return "Set Colour";
        case WorkflowNodeType::Number:               return "Number";
        case WorkflowNodeType::Date:                 return "Date";
        case WorkflowNodeType::SetUniqueID:          return "Set Unique ID";
        case WorkflowNodeType::ProcessIf:            return "Process If";
        case WorkflowNodeType::Comment:              return "Comment";
        case WorkflowNodeType::CodeExecute:          return "Code Execute";
        case WorkflowNodeType::FindAndPlace:         return "Find and Place";
        case WorkflowNodeType::PublisherCheck:        return "Publisher Check";
        default: return "Unknown";
    }
}

std::vector<WorkflowNodeType> WorkflowEngine::getAvailableNodeTypes() {
    return {
        WorkflowNodeType::SetField,
        WorkflowNodeType::CopyField,
        WorkflowNodeType::FindAndReplace,
        WorkflowNodeType::ChangeCase,
        WorkflowNodeType::AddRemoveChars,
        WorkflowNodeType::AppendPrependField,
        WorkflowNodeType::RemoveWords,
        WorkflowNodeType::RemoveOccurrences,
        WorkflowNodeType::RemoveMultipleSpaces,
        WorkflowNodeType::TakeFirstWords,
        WorkflowNodeType::TakeLastWords,
        WorkflowNodeType::SplitAndSort,
        WorkflowNodeType::CopyFolderName,
        WorkflowNodeType::FilePathExtractor,
        WorkflowNodeType::FieldBuild,
        WorkflowNodeType::AutoSplit,
        WorkflowNodeType::SetRating,
        WorkflowNodeType::SetColour,
        WorkflowNodeType::Number,
        WorkflowNodeType::Date,
        WorkflowNodeType::SetUniqueID,
        WorkflowNodeType::ProcessIf,
        WorkflowNodeType::Comment,
        WorkflowNodeType::CodeExecute
    };
}

// Case Change

std::string WorkflowEngine::changeCase(const std::string& input, CaseChangeType type) {
    if (input.empty()) return input;
    
    switch (type) {
        case CaseChangeType::ToUpperCase: {
            std::string r = input;
            std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c){ return (char)std::toupper(c); });
            return r;
        }
        case CaseChangeType::ToLowerCase: {
            std::string r = input;
            std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            return r;
        }
        case CaseChangeType::ToTitleCase: {
            std::string r = input;
            bool capitalize = true;
            for (size_t i = 0; i < r.size(); i++) {
                if (capitalize && std::isalpha((unsigned char)r[i])) {
                    r[i] = (char)std::toupper((unsigned char)r[i]);
                    capitalize = false;
                } else if (r[i] == ' ' || r[i] == '-' || r[i] == '_') {
                    capitalize = true;
                } else {
                    r[i] = (char)std::tolower((unsigned char)r[i]);
                }
            }
            return r;
        }
        case CaseChangeType::ToSentenceCase: {
            std::string r = input;
            std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            if (!r.empty()) r[0] = (char)std::toupper((unsigned char)r[0]);
            return r;
        }
        case CaseChangeType::SwapCase: {
            std::string r = input;
            for (auto& c : r) {
                if (std::isupper((unsigned char)c)) c = (char)std::tolower((unsigned char)c);
                else if (std::islower((unsigned char)c)) c = (char)std::toupper((unsigned char)c);
            }
            return r;
        }
    }
    return input;
}

// Condition Check

bool WorkflowEngine::checkCondition(const std::string& value, ProcessIfCondition cond, const std::string& condValue) {
    switch (cond) {
        case ProcessIfCondition::Contains:    return value.find(condValue) != std::string::npos;
        case ProcessIfCondition::NotContains: return value.find(condValue) == std::string::npos;
        case ProcessIfCondition::Equals:      return value == condValue;
        case ProcessIfCondition::NotEquals:   return value != condValue;
        case ProcessIfCondition::StartsWith:  return value.substr(0, condValue.size()) == condValue;
        case ProcessIfCondition::EndsWith:    return value.size() >= condValue.size() && value.substr(value.size()-condValue.size()) == condValue;
        case ProcessIfCondition::IsEmpty:     return value.empty();
        case ProcessIfCondition::IsNotEmpty:  return !value.empty();
        case ProcessIfCondition::RegexMatch: {
            try { return std::regex_search(value, std::regex(condValue)); }
            catch (...) { return false; }
        }
    }
    return false;
}

// Node Executors

WorkflowResult WorkflowEngine::execSetField(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata) {
    WorkflowResult r;
    r.changedFields[node.field] = node.value;
    return r;
}

WorkflowResult WorkflowEngine::execCopyField(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata) {
    WorkflowResult r;
    std::string srcVal = getMetaField(metadata, node.srcField);
    std::string destVal = getMetaField(metadata, node.destField);
    
    switch (node.copyAction) {
        case CopyAction::Copy:
            r.changedFields[node.destField] = srcVal;
            break;
        case CopyAction::Append:
            r.changedFields[node.destField] = destVal + srcVal;
            break;
        case CopyAction::Prepend:
            r.changedFields[node.destField] = srcVal + destVal;
            break;
    }
    return r;
}

WorkflowResult WorkflowEngine::execFindReplace(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata) {
    WorkflowResult r;
    std::string val = getMetaField(metadata, node.field);
    
    if (node.useRegex) {
        try {
            std::regex re(node.findText);
            val = std::regex_replace(val, re, node.replaceText);
        } catch (const std::exception& e) {
            r.success = false;
            r.error = std::string("Regex error: ") + e.what();
            return r;
        }
    } else {
        // Simple string replace all
        size_t pos = 0;
        while ((pos = val.find(node.findText, pos)) != std::string::npos) {
            val.replace(pos, node.findText.length(), node.replaceText);
            pos += node.replaceText.length();
        }
    }
    
    r.changedFields[node.field] = val;
    return r;
}

WorkflowResult WorkflowEngine::execChangeCase(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata) {
    WorkflowResult r;
    std::string val = getMetaField(metadata, node.field);
    r.changedFields[node.field] = changeCase(val, node.caseType);
    return r;
}

WorkflowResult WorkflowEngine::execAddRemoveChars(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata) {
    WorkflowResult r;
    std::string val = getMetaField(metadata, node.field);
    
    if (!node.addText.empty()) {
        // Prefix or suffix based on type
        if (node.type == WorkflowNodeType::AddRemoveChars) {
            // Convention: positive charCount = prefix, negative = suffix
            if (node.charCount >= 0) {
                val = node.addText + val;
            } else {
                val = val + node.addText;
            }
        }
    } else if (node.charCount > 0) {
        // Delete chars from start
        if (node.charCount < (int)val.size()) {
            val = val.substr(node.charCount);
        } else {
            val.clear();
        }
    } else if (node.charCount < 0) {
        // Delete chars from end
        int removeCount = -node.charCount;
        if (removeCount < (int)val.size()) {
            val = val.substr(0, val.size() - removeCount);
        } else {
            val.clear();
        }
    }
    
    r.changedFields[node.field] = val;
    return r;
}

WorkflowResult WorkflowEngine::execRemoveMultipleSpaces(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata) {
    WorkflowResult r;
    std::string val = getMetaField(metadata, node.field);
    
    // Remove multiple consecutive spaces
    std::string result;
    bool lastSpace = false;
    for (char c : val) {
        if (c == ' ') {
            if (!lastSpace) result += c;
            lastSpace = true;
        } else {
            result += c;
            lastSpace = false;
        }
    }
    
    // Trim leading/trailing spaces
    size_t start = result.find_first_not_of(' ');
    if (start == std::string::npos) result.clear();
    else {
        size_t end = result.find_last_not_of(' ');
        result = result.substr(start, end - start + 1);
    }
    
    r.changedFields[node.field] = result;
    return r;
}

WorkflowResult WorkflowEngine::execTakeWords(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata, bool takeLast) {
    WorkflowResult r;
    std::string val = getMetaField(metadata, node.field);
    
    std::istringstream iss(val);
    std::vector<std::string> words;
    std::string word;
    while (iss >> word) words.push_back(word);
    
    int n = node.wordCount;
    if (n <= 0) n = 1;
    
    std::string result;
    if (takeLast) {
        int start = std::max(0, (int)words.size() - n);
        for (int i = start; i < (int)words.size(); i++) {
            if (!result.empty()) result += " ";
            result += words[i];
        }
    } else {
        for (int i = 0; i < n && i < (int)words.size(); i++) {
            if (!result.empty()) result += " ";
            result += words[i];
        }
    }
    
    r.changedFields[node.field] = result;
    return r;
}

WorkflowResult WorkflowEngine::execNumber(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata, int seq) {
    WorkflowResult r;
    
    int num = node.startNumber + seq * node.stepNumber;
    std::ostringstream oss;
    oss << node.numberPrefix << std::setfill('0') << std::setw(node.padDigits) << num << node.numberSuffix;
    
    r.changedFields[node.field] = oss.str();
    return r;
}

WorkflowResult WorkflowEngine::execCopyFolderName(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata) {
    WorkflowResult r;
    std::string filePath = getMetaField(metadata, "FilePath");
    if (filePath.empty()) filePath = getMetaField(metadata, "Pathname");
    
    if (!filePath.empty()) {
        // Extract folder name
        size_t lastSep = filePath.find_last_of("/\\");
        if (lastSep != std::string::npos && lastSep > 0) {
            std::string parentPath = filePath.substr(0, lastSep);
            size_t parentSep = parentPath.find_last_of("/\\");
            std::string folderName;
            if (parentSep != std::string::npos) {
                folderName = parentPath.substr(parentSep + 1);
            } else {
                folderName = parentPath;
            }
            r.changedFields[node.field] = folderName;
        }
    }
    return r;
}

WorkflowResult WorkflowEngine::execProcessIf(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata) {
    WorkflowResult r;
    std::string val = getMetaField(metadata, node.conditionField);
    
    if (!checkCondition(val, node.condition, node.conditionValue)) {
        // Condition not met - skip (return empty changes)
        r.success = true;
        return r;
    }
    
    // Condition met - apply the action
    if (!node.field.empty() && !node.value.empty()) {
        r.changedFields[node.field] = node.value;
    }
    return r;
}

// Execute Node

WorkflowResult WorkflowEngine::executeNode(
    const WorkflowNode& node,
    const std::unordered_map<std::string, std::string>& metadata,
    int sequenceNumber) {
    
    if (!node.enabled) return WorkflowResult(); // skip disabled
    
    switch (node.type) {
        case WorkflowNodeType::SetField:            return execSetField(node, metadata);
        case WorkflowNodeType::CopyField:           return execCopyField(node, metadata);
        case WorkflowNodeType::FindAndReplace:
        case WorkflowNodeType::FindAndReplaceWord:  return execFindReplace(node, metadata);
        case WorkflowNodeType::ChangeCase:          return execChangeCase(node, metadata);
        case WorkflowNodeType::AddRemoveChars:      return execAddRemoveChars(node, metadata);
        case WorkflowNodeType::RemoveMultipleSpaces:return execRemoveMultipleSpaces(node, metadata);
        case WorkflowNodeType::TakeFirstWords:      return execTakeWords(node, metadata, false);
        case WorkflowNodeType::TakeLastWords:       return execTakeWords(node, metadata, true);
        case WorkflowNodeType::Number:              return execNumber(node, metadata, sequenceNumber);
        case WorkflowNodeType::CopyFolderName:      return execCopyFolderName(node, metadata);
        case WorkflowNodeType::ProcessIf:           return execProcessIf(node, metadata);
        case WorkflowNodeType::Comment:             return WorkflowResult(); // no-op
        default: {
            WorkflowResult r;
            r.success = true;
            return r;
        }
    }
}

// Execute Workflow

WorkflowResult WorkflowEngine::executeWorkflow(
    const Workflow& workflow,
    const std::unordered_map<std::string, std::string>& metadata,
    int sequenceNumber) {
    
    WorkflowResult finalResult;
    
    // Merge metadata with changes accumulated so far
    auto currentMeta = metadata;
    
    for (const auto& node : workflow.nodes) {
        if (!node.enabled) continue;
        
        auto nodeResult = executeNode(node, currentMeta, sequenceNumber);
        
        if (!nodeResult.success) {
            finalResult.success = false;
            finalResult.error = nodeResult.error;
            return finalResult;
        }
        
        // Apply changes to running metadata
        for (const auto& kv : nodeResult.changedFields) {
            currentMeta[kv.first] = kv.second;
            finalResult.changedFields[kv.first] = kv.second;
        }
    }
    
    return finalResult;
}

// Workflow Management

std::vector<Workflow>& WorkflowEngine::getWorkflows() {
    return s_workflows;
}

Workflow* WorkflowEngine::getWorkflow(const std::string& name) {
    for (auto& wf : s_workflows) {
        if (wf.name == name) return &wf;
    }
    return nullptr;
}

void WorkflowEngine::addWorkflow(const Workflow& wf) {
    for (auto& existing : s_workflows) {
        if (existing.name == wf.name) { existing = wf; return; }
    }
    s_workflows.push_back(wf);
}

void WorkflowEngine::removeWorkflow(const std::string& name) {
    s_workflows.erase(
        std::remove_if(s_workflows.begin(), s_workflows.end(),
                        [&](const Workflow& w) { return w.name == name; }),
        s_workflows.end());
}

// Serialization (简单文本格式)

std::string WorkflowEngine::serializeWorkflow(const Workflow& wf) {
    std::ostringstream oss;
    oss << "WORKFLOW|" << wf.name << "|" << wf.description << "\n";
    for (const auto& node : wf.nodes) {
        oss << "NODE|" << (int)node.type << "|" << node.field << "|" << node.value
            << "|" << node.findText << "|" << node.replaceText
            << "|" << (node.useRegex ? "1" : "0") << "|" << (node.enabled ? "1" : "0")
            << "|" << node.userNote << "\n";
    }
    oss << "END\n";
    return oss.str();
}

bool WorkflowEngine::deserializeWorkflow(const std::string& data, Workflow& outWf) {
    std::istringstream iss(data);
    std::string line;
    
    while (std::getline(iss, line)) {
        if (line.substr(0, 9) == "WORKFLOW|") {
            // Parse header
            std::istringstream hss(line.substr(9));
            std::getline(hss, outWf.name, '|');
            std::getline(hss, outWf.description, '|');
        } else if (line.substr(0, 5) == "NODE|") {
            // Parse node
            std::istringstream nss(line.substr(5));
            WorkflowNode node;
            std::string typeStr, regexStr, enabledStr;
            
            std::getline(nss, typeStr, '|');
            node.type = (WorkflowNodeType)std::stoi(typeStr);
            std::getline(nss, node.field, '|');
            std::getline(nss, node.value, '|');
            std::getline(nss, node.findText, '|');
            std::getline(nss, node.replaceText, '|');
            std::getline(nss, regexStr, '|');
            node.useRegex = (regexStr == "1");
            std::getline(nss, enabledStr, '|');
            node.enabled = (enabledStr != "0");
            std::getline(nss, node.userNote, '|');
            
            node.id = outWf.nextNodeId++;
            outWf.nodes.push_back(node);
        } else if (line == "END") {
            break;
        }
    }
    
    return !outWf.name.empty();
}

// JSON 持久化 (F-011) — 使用已有的文本序列化格式，多工作流合并存储

bool WorkflowEngine::saveWorkflowsToFile(const std::string& pathUtf8) {
    std::string content;
    const auto& workflows = getWorkflows();
    for (const auto& wf : workflows) {
        content += serializeWorkflow(wf);
    }
    return Platform::FileSystem::WriteFileText(pathUtf8, content);
}

bool WorkflowEngine::loadWorkflowsFromFile(const std::string& pathUtf8) {
    std::string content;
    if (!Platform::FileSystem::ReadFileText(pathUtf8, content)) return false;

    // Split into blocks delimited by "END\n"
    size_t pos = 0;
    while (pos < content.size()) {
        size_t endPos = content.find("END\n", pos);
        if (endPos == std::string::npos) endPos = content.find("END", pos);
        if (endPos == std::string::npos) break;

        std::string block = content.substr(pos, endPos - pos + 3); // include "END"
        Workflow wf;
        if (deserializeWorkflow(block, wf) && !wf.name.empty()) {
            // Overwrite if same name exists
            auto& existing = getWorkflows();
            bool replaced = false;
            for (auto& ew : existing) {
                if (ew.name == wf.name) {
                    ew = wf;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) addWorkflow(wf);
        }

        pos = endPos + 3;
        // Skip newlines
        while (pos < content.size() && (content[pos] == '\n' || content[pos] == '\r')) pos++;
    }

    return true;
}

std::string WorkflowEngine::exportWorkflowJson(const Workflow& wf) {
    // Use the text serialization format (compatible, portable)
    return serializeWorkflow(wf);
}

bool WorkflowEngine::importWorkflowJson(const std::string& json, Workflow& outWf) {
    return deserializeWorkflow(json, outWf);
}
