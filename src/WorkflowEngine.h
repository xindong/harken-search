#pragma once
// SoundManager - 工作流引擎模块
// WorkflowEngine.h - 可视化工作流管道、节点定义、批量元数据操作
// Workflow node/pipeline definitions for batch library operations.

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>

// Workflow Node Types

enum class WorkflowNodeType {
    // Field operations
    SetField,           // 设置字段为固定值
    CopyField,          // 复制/追加/前置字段
    FindAndReplace,     // 查找替换 (支持正则)
    FindAndReplaceWord, // 按整词查找替换
    
    // Text operations
    ChangeCase,         // 大小写转换
    AddRemoveChars,     // 首尾添加/删除字符
    AppendPrependField, // 追加/前置字段内容
    RemoveWords,        // 删除指定词语
    RemoveOccurrences,  // 删除匹配项
    RemoveMultipleSpaces, // 删除多余空格
    TakeFirstWords,     // 取前 N 个词
    TakeLastWords,      // 取后 N 个词
    SplitAndSort,       // 拆分排序
    SeparateItems,      // 分隔项目
    SeparateLastItems,  // 分隔最后的项目
    
    // Metadata operations
    CopyFirstRecord,    // 复制第一条记录
    CopyFolderName,     // 复制文件夹名到字段
    FilePathExtractor,  // 从文件路径提取信息
    FieldBuild,         // 从多字段组合新值
    
    // Category / UCS
    AutoSplit,          // 自动拆分 UCS 分类
    AssignCats,         // 分配分类
    UCSBuild,           // 构建 UCS 分类
    
    // Rating / Colour
    SetRating,          // 设置评分
    SetColour,          // 设置颜色标记
    
    // Numbering / Date
    Number,             // 自动编号
    Date,               // 日期操作
    SetUniqueID,        // 设置唯一ID
    
    // Conditional
    ProcessIf,          // 条件处理
    
    // Control
    Comment,            // 注释 (不执行)
    CodeExecute,        // 自定义代码执行
    
    // Find
    FindAndPlace,       // 查找并放置
    PublisherCheck,     // 版权检查
    
    COUNT
};

// Copy Action Type (for CopyField node)

enum class CopyAction {
    Copy,       // 覆盖复制
    Append,     // 追加到末尾
    Prepend     // 前置到开头
};

// Case Change Type

enum class CaseChangeType {
    ToUpperCase,    // 全部大写
    ToLowerCase,    // 全部小写
    ToTitleCase,    // 首字母大写
    ToSentenceCase, // 句首大写
    SwapCase        // 大小写交换
};

// Process If Condition

enum class ProcessIfCondition {
    Contains,       // 包含
    NotContains,    // 不包含
    Equals,         // 等于
    NotEquals,      // 不等于
    StartsWith,     // 开头匹配
    EndsWith,       // 结尾匹配
    IsEmpty,        // 为空
    IsNotEmpty,     // 非空
    RegexMatch      // 正则匹配
};

// Workflow Node (单个工作流节点)

struct WorkflowNode {
    int id = 0;
    WorkflowNodeType type = WorkflowNodeType::SetField;
    bool enabled = true;
    std::string userNote;           // 用户备注
    
// Parameters (按节点类型使用不同子集)
    
    // Field reference
    std::string field;              // 目标字段名
    std::string srcField;           // 源字段名 (CopyField)
    std::string destField;          // 目标字段名 (CopyField)
    
    // Value
    std::string value;              // 固定值 (SetField)
    std::string findText;           // 查找文本
    std::string replaceText;        // 替换文本
    
    // Options
    bool useRegex = false;          // 使用正则表达式
    CopyAction copyAction = CopyAction::Copy;
    CaseChangeType caseType = CaseChangeType::ToTitleCase;
    
    // ProcessIf
    ProcessIfCondition condition = ProcessIfCondition::Contains;
    std::string conditionField;     // 条件字段
    std::string conditionValue;     // 条件值
    
    // Number
    int startNumber = 1;
    int stepNumber = 1;
    int padDigits = 3;
    std::string numberPrefix;
    std::string numberSuffix;
    
    // Take words
    int wordCount = 1;
    
    // Add/Remove chars
    int charCount = 0;              // DeleteStartChars/DeleteEndChars
    std::string addText;            // AddPrefix/AddSuffix text
    
    // Separator
    std::string separator = ", ";
    
    // Custom code (CodeExecute)
    std::string customCode;
};

// Workflow (节点管道)

struct Workflow {
    std::string name;               // 工作流名称
    std::string description;        // 说明
    std::vector<WorkflowNode> nodes;// 节点列表 (按顺序执行)
    int64_t lastModified = 0;       // 最后修改时间
    
    int nextNodeId = 1;
    
    int addNode(WorkflowNodeType type) {
        WorkflowNode n;
        n.id = nextNodeId++;
        n.type = type;
        nodes.push_back(n);
        return n.id;
    }
    
    void removeNode(int nodeId) {
        nodes.erase(
            std::remove_if(nodes.begin(), nodes.end(),
                           [nodeId](const WorkflowNode& n) { return n.id == nodeId; }),
            nodes.end());
    }
    
    void moveNodeUp(int index) {
        if (index > 0 && index < (int)nodes.size()) {
            std::swap(nodes[index], nodes[index - 1]);
        }
    }
    
    void moveNodeDown(int index) {
        if (index >= 0 && index < (int)nodes.size() - 1) {
            std::swap(nodes[index], nodes[index + 1]);
        }
    }
};

// Workflow Execution Result

struct WorkflowResult {
    // Changed fields: fieldName -> newValue
    std::unordered_map<std::string, std::string> changedFields;
    bool success = true;
    std::string error;
};

// F-011: 工作流执行历史记录（用于撤销）
struct WorkflowExecEntry {
    int64_t fileId = 0;
    std::unordered_map<std::string, std::string> oldValues; // field → 执行前的值
    std::unordered_map<std::string, std::string> newValues; // field → 执行后的值
};

struct WorkflowExecHistory {
    std::string workflowName;
    int64_t timestamp = 0;
    std::vector<WorkflowExecEntry> entries;
    bool undone = false;
};

// Workflow Engine

// @stage7-contract
// API contract: WorkflowEngine executes pure node/workflow transformations
// over caller-owned metadata and returns owned result/history values. Registry
// accessors expose process-local workflow storage; callers must coordinate
// mutations on the owning UI thread. File import/export borrows UTF-8/JSON
// inputs and reports parse/IO failure through bool without changing workflow
// semantics.
class WorkflowEngine {
public:
// Node Type Info
    
    static const char* getNodeTypeName(WorkflowNodeType type);
    
    // 获取所有可用的节点类型
    static std::vector<WorkflowNodeType> getAvailableNodeTypes();
    
// 节点执行
    
    // 执行单个节点
    // metadata: 当前记录的所有字段 (field name -> value)
    // Returns: 变更的字段
    static WorkflowResult executeNode(
        const WorkflowNode& node,
        const std::unordered_map<std::string, std::string>& metadata,
        int sequenceNumber = 0);
    
    // 执行整个工作流
    static WorkflowResult executeWorkflow(
        const Workflow& workflow,
        const std::unordered_map<std::string, std::string>& metadata,
        int sequenceNumber = 0);
    
// 工作流预设管理
    
    static std::vector<Workflow>& getWorkflows();
    static Workflow* getWorkflow(const std::string& name);
    static void addWorkflow(const Workflow& wf);
    static void removeWorkflow(const std::string& name);
    
// 序列化

    // 简单的文本格式序列化
    static std::string serializeWorkflow(const Workflow& wf);
    static bool deserializeWorkflow(const std::string& data, Workflow& outWf);

// JSON 持久化 (F-011)

    /// 保存所有工作流到 JSON 文件
    static bool saveWorkflowsToFile(const std::string& pathUtf8);
    /// 从 JSON 文件加载工作流（追加到现有列表，同名覆盖）
    static bool loadWorkflowsFromFile(const std::string& pathUtf8);
    /// 导出单个工作流为 JSON 字符串（用于分享）
    static std::string exportWorkflowJson(const Workflow& wf);
    /// 从 JSON 字符串导入单个工作流
    static bool importWorkflowJson(const std::string& json, Workflow& outWf);
    
private:
    // 节点执行器
    static WorkflowResult execSetField(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata);
    static WorkflowResult execCopyField(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata);
    static WorkflowResult execFindReplace(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata);
    static WorkflowResult execChangeCase(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata);
    static WorkflowResult execAddRemoveChars(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata);
    static WorkflowResult execRemoveMultipleSpaces(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata);
    static WorkflowResult execTakeWords(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata, bool takeLast);
    static WorkflowResult execNumber(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata, int seq);
    static WorkflowResult execCopyFolderName(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata);
    static WorkflowResult execProcessIf(const WorkflowNode& node, const std::unordered_map<std::string, std::string>& metadata);
    
    // Helpers
    static std::string getMetaField(const std::unordered_map<std::string, std::string>& m, const std::string& key);
    static std::string changeCase(const std::string& input, CaseChangeType type);
    static bool checkCondition(const std::string& value, ProcessIfCondition cond, const std::string& condValue);
};
