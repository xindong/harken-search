// test_workflow_engine.cpp — Unit tests for WorkflowEngine (F-011)
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "../src/WorkflowEngine.h"

static int failed = 0;
static int passed = 0;

static void CHECK(bool cond, const char* desc) {
    if (cond) { ++passed; std::printf("  PASS: %s\n", desc); }
    else      { ++failed; std::printf("  FAIL: %s\n", desc); }
}

using Meta = std::unordered_map<std::string, std::string>;

int main() {
    std::printf("=== WorkflowEngine Tests ===\n");

    // ── Workflow struct: addNode / removeNode ──
    std::printf("\n--- Workflow struct ---\n");
    {
        Workflow wf;
        wf.name = "test";
        CHECK(wf.nodes.empty(), "initial nodes empty");

        int id1 = wf.addNode(WorkflowNodeType::SetField);
        CHECK(wf.nodes.size() == 1, "addNode: size = 1");
        CHECK(wf.nodes[0].type == WorkflowNodeType::SetField, "addNode: type correct");
        CHECK(wf.nodes[0].id == id1, "addNode: id assigned");
        CHECK(wf.nodes[0].enabled, "addNode: enabled by default");

        int id2 = wf.addNode(WorkflowNodeType::CopyField);
        int id3 = wf.addNode(WorkflowNodeType::ChangeCase);
        CHECK(wf.nodes.size() == 3, "addNode: 3 nodes");
        CHECK(id2 != id1 && id3 != id2, "addNode: ids unique");

        wf.removeNode(id2);
        CHECK(wf.nodes.size() == 2, "removeNode: size = 2");
        CHECK(wf.nodes[0].id == id1, "removeNode: first remains");
        CHECK(wf.nodes[1].id == id3, "removeNode: third remains");

        wf.removeNode(999);
        CHECK(wf.nodes.size() == 2, "removeNode nonexistent: no change");
    }

    // ── Workflow struct: moveNodeUp / moveNodeDown ──
    std::printf("\n--- moveNodeUp / moveNodeDown ---\n");
    {
        Workflow wf;
        wf.addNode(WorkflowNodeType::SetField);
        wf.addNode(WorkflowNodeType::CopyField);
        wf.addNode(WorkflowNodeType::ChangeCase);

        auto t0 = wf.nodes[0].type;
        auto t1 = wf.nodes[1].type;
        auto t2 = wf.nodes[2].type;

        wf.moveNodeDown(0);
        CHECK(wf.nodes[0].type == t1, "moveDown(0): swapped 0<->1");
        CHECK(wf.nodes[1].type == t0, "moveDown(0): swapped 0<->1 (b)");
        CHECK(wf.nodes[2].type == t2, "moveDown(0): index 2 unchanged");

        wf.moveNodeUp(1);
        CHECK(wf.nodes[0].type == t0, "moveUp(1): restored original");
        CHECK(wf.nodes[1].type == t1, "moveUp(1): restored original (b)");

        wf.moveNodeUp(0);
        CHECK(wf.nodes[0].type == t0, "moveUp(0): no-op at boundary");

        wf.moveNodeDown(2);
        CHECK(wf.nodes[2].type == t2, "moveDown(last): no-op at boundary");
    }

    // ── getNodeTypeName ──
    std::printf("\n--- getNodeTypeName ---\n");
    {
        CHECK(std::strcmp(WorkflowEngine::getNodeTypeName(WorkflowNodeType::SetField), "Set Field") == 0,
              "SetField name");
        CHECK(std::strcmp(WorkflowEngine::getNodeTypeName(WorkflowNodeType::CopyField), "Copy Field") == 0,
              "CopyField name");
        CHECK(std::strcmp(WorkflowEngine::getNodeTypeName(WorkflowNodeType::FindAndReplace), "Find and Replace") == 0,
              "FindAndReplace name");
        CHECK(std::strcmp(WorkflowEngine::getNodeTypeName(WorkflowNodeType::ChangeCase), "Change Case") == 0,
              "ChangeCase name");
        CHECK(std::strcmp(WorkflowEngine::getNodeTypeName(WorkflowNodeType::Comment), "Comment") == 0,
              "Comment name");
        CHECK(std::strcmp(WorkflowEngine::getNodeTypeName(WorkflowNodeType::Number), "Number") == 0,
              "Number name");
    }

    // ── getAvailableNodeTypes ──
    std::printf("\n--- getAvailableNodeTypes ---\n");
    {
        auto types = WorkflowEngine::getAvailableNodeTypes();
        CHECK(types.size() >= 10, "at least 10 available types");

        bool hasSetField = false, hasComment = false;
        for (auto t : types) {
            if (t == WorkflowNodeType::SetField) hasSetField = true;
            if (t == WorkflowNodeType::Comment) hasComment = true;
        }
        CHECK(hasSetField, "available types include SetField");
        CHECK(hasComment, "available types include Comment");
    }

    // ── executeNode: SetField ──
    std::printf("\n--- executeNode: SetField ---\n");
    {
        WorkflowNode node;
        node.type = WorkflowNodeType::SetField;
        node.enabled = true;
        node.field = "Description";
        node.value = "Gunshot impact";

        Meta meta;
        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.success, "SetField: success");
        CHECK(r.changedFields.count("Description") == 1, "SetField: field present");
        CHECK(r.changedFields["Description"] == "Gunshot impact", "SetField: value correct");
    }

    // ── executeNode: CopyField (Copy / Append / Prepend) ──
    std::printf("\n--- executeNode: CopyField ---\n");
    {
        Meta meta;
        meta["Source"] = "hello";
        meta["Dest"] = "world";

        WorkflowNode node;
        node.type = WorkflowNodeType::CopyField;
        node.enabled = true;
        node.srcField = "Source";
        node.destField = "Dest";

        node.copyAction = CopyAction::Copy;
        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Dest"] == "hello", "CopyField Copy: overwrites dest");

        node.copyAction = CopyAction::Append;
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Dest"] == "worldhello", "CopyField Append: appends src to dest");

        node.copyAction = CopyAction::Prepend;
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Dest"] == "helloworld", "CopyField Prepend: prepends src to dest");
    }

    // ── executeNode: FindAndReplace (literal) ──
    std::printf("\n--- executeNode: FindAndReplace (literal) ---\n");
    {
        Meta meta;
        meta["Description"] = "The quick brown fox jumps";

        WorkflowNode node;
        node.type = WorkflowNodeType::FindAndReplace;
        node.enabled = true;
        node.field = "Description";
        node.findText = "quick";
        node.replaceText = "slow";
        node.useRegex = false;

        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.success, "FindReplace literal: success");
        CHECK(r.changedFields["Description"] == "The slow brown fox jumps",
              "FindReplace literal: replaced");
    }

    // ── executeNode: FindAndReplace (regex) ──
    std::printf("\n--- executeNode: FindAndReplace (regex) ---\n");
    {
        Meta meta;
        meta["Name"] = "SFX_Gun_Shot_01";

        WorkflowNode node;
        node.type = WorkflowNodeType::FindAndReplace;
        node.enabled = true;
        node.field = "Name";
        node.findText = "_\\d+$";
        node.replaceText = "";
        node.useRegex = true;

        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.success, "FindReplace regex: success");
        CHECK(r.changedFields["Name"] == "SFX_Gun_Shot",
              "FindReplace regex: trailing number removed");
    }

    // ── executeNode: FindAndReplace (bad regex) ──
    std::printf("\n--- executeNode: FindAndReplace (bad regex) ---\n");
    {
        Meta meta;
        meta["X"] = "test";

        WorkflowNode node;
        node.type = WorkflowNodeType::FindAndReplace;
        node.enabled = true;
        node.field = "X";
        node.findText = "[invalid(";
        node.replaceText = "";
        node.useRegex = true;

        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(!r.success, "FindReplace bad regex: not success");
        CHECK(!r.error.empty(), "FindReplace bad regex: error message set");
    }

    // ── executeNode: ChangeCase (all 5 types) ──
    std::printf("\n--- executeNode: ChangeCase ---\n");
    {
        Meta meta;
        meta["Title"] = "hello world";

        WorkflowNode node;
        node.type = WorkflowNodeType::ChangeCase;
        node.enabled = true;
        node.field = "Title";

        node.caseType = CaseChangeType::ToUpperCase;
        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Title"] == "HELLO WORLD", "ChangeCase: upper");

        node.caseType = CaseChangeType::ToLowerCase;
        meta["Title"] = "HELLO WORLD";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Title"] == "hello world", "ChangeCase: lower");

        node.caseType = CaseChangeType::ToTitleCase;
        meta["Title"] = "hello world test";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Title"] == "Hello World Test", "ChangeCase: title");

        node.caseType = CaseChangeType::ToSentenceCase;
        meta["Title"] = "HELLO WORLD";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Title"] == "Hello world", "ChangeCase: sentence");

        node.caseType = CaseChangeType::SwapCase;
        meta["Title"] = "Hello";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Title"] == "hELLO", "ChangeCase: swap");
    }

    // ── executeNode: ChangeCase TitleCase with separators ──
    std::printf("\n--- ChangeCase: TitleCase separators ---\n");
    {
        Meta meta;
        meta["F"] = "hello-world_test";

        WorkflowNode node;
        node.type = WorkflowNodeType::ChangeCase;
        node.enabled = true;
        node.field = "F";
        node.caseType = CaseChangeType::ToTitleCase;

        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["F"] == "Hello-World_Test", "TitleCase: capitalizes after - and _");
    }

    // ── executeNode: AddRemoveChars (add prefix / suffix) ──
    std::printf("\n--- executeNode: AddRemoveChars ---\n");
    {
        Meta meta;
        meta["Name"] = "Impact";

        WorkflowNode node;
        node.type = WorkflowNodeType::AddRemoveChars;
        node.enabled = true;
        node.field = "Name";

        node.addText = "SFX_";
        node.charCount = 0;
        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Name"] == "SFX_Impact", "AddRemoveChars: add prefix");

        node.charCount = -1;
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Name"] == "ImpactSFX_", "AddRemoveChars: add suffix");

        node.addText = "";
        node.charCount = 3;
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Name"] == "act", "AddRemoveChars: delete 3 from start");

        node.charCount = -3;
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Name"] == "Imp", "AddRemoveChars: delete 3 from end");
    }

    // ── executeNode: RemoveMultipleSpaces ──
    std::printf("\n--- executeNode: RemoveMultipleSpaces ---\n");
    {
        Meta meta;
        meta["Desc"] = "  hello   world  test  ";

        WorkflowNode node;
        node.type = WorkflowNodeType::RemoveMultipleSpaces;
        node.enabled = true;
        node.field = "Desc";

        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Desc"] == "hello world test",
              "RemoveMultipleSpaces: collapsed + trimmed");
    }

    // ── executeNode: TakeFirstWords ──
    std::printf("\n--- executeNode: TakeFirstWords ---\n");
    {
        Meta meta;
        meta["Desc"] = "The quick brown fox jumps over";

        WorkflowNode node;
        node.type = WorkflowNodeType::TakeFirstWords;
        node.enabled = true;
        node.field = "Desc";
        node.wordCount = 3;

        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Desc"] == "The quick brown", "TakeFirstWords: first 3");
    }

    // ── executeNode: TakeLastWords ──
    std::printf("\n--- executeNode: TakeLastWords ---\n");
    {
        Meta meta;
        meta["Desc"] = "The quick brown fox jumps over";

        WorkflowNode node;
        node.type = WorkflowNodeType::TakeLastWords;
        node.enabled = true;
        node.field = "Desc";
        node.wordCount = 2;

        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Desc"] == "jumps over", "TakeLastWords: last 2");
    }

    // ── executeNode: Number ──
    std::printf("\n--- executeNode: Number ---\n");
    {
        Meta meta;

        WorkflowNode node;
        node.type = WorkflowNodeType::Number;
        node.enabled = true;
        node.field = "Index";
        node.startNumber = 1;
        node.stepNumber = 1;
        node.padDigits = 3;
        node.numberPrefix = "SFX_";
        node.numberSuffix = "_v1";

        auto r0 = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r0.changedFields["Index"] == "SFX_001_v1", "Number seq=0: SFX_001_v1");

        auto r4 = WorkflowEngine::executeNode(node, meta, 4);
        CHECK(r4.changedFields["Index"] == "SFX_005_v1", "Number seq=4: SFX_005_v1");

        node.startNumber = 10;
        node.stepNumber = 5;
        node.padDigits = 4;
        node.numberPrefix = "";
        node.numberSuffix = "";
        auto r2 = WorkflowEngine::executeNode(node, meta, 2);
        CHECK(r2.changedFields["Index"] == "0020", "Number start=10 step=5 seq=2: 0020");
    }

    // ── executeNode: CopyFolderName ──
    std::printf("\n--- executeNode: CopyFolderName ---\n");
    {
        Meta meta;
        meta["FilePath"] = "C:/SFX/Guns/Pistol/shot_01.wav";

        WorkflowNode node;
        node.type = WorkflowNodeType::CopyFolderName;
        node.enabled = true;
        node.field = "Category";

        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Category"] == "Pistol", "CopyFolderName: extracts parent folder");

        meta["FilePath"] = "D:\\Library\\Ambience\\rain.wav";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Category"] == "Ambience", "CopyFolderName: backslash path");
    }

    // ── executeNode: CopyFolderName (Pathname fallback) ──
    std::printf("\n--- CopyFolderName: Pathname fallback ---\n");
    {
        Meta meta;
        meta["Pathname"] = "/mnt/sfx/Foley/footstep.wav";

        WorkflowNode node;
        node.type = WorkflowNodeType::CopyFolderName;
        node.enabled = true;
        node.field = "Folder";

        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields["Folder"] == "Foley", "CopyFolderName: uses Pathname fallback");
    }

    // ── executeNode: ProcessIf (all conditions) ──
    std::printf("\n--- executeNode: ProcessIf ---\n");
    {
        WorkflowNode node;
        node.type = WorkflowNodeType::ProcessIf;
        node.enabled = true;
        node.field = "Tag";
        node.value = "Outdoor";
        node.conditionField = "Category";

        // Contains
        Meta meta; meta["Category"] = "Ambience Forest";
        node.condition = ProcessIfCondition::Contains;
        node.conditionValue = "Forest";
        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.count("Tag") == 1, "ProcessIf Contains match: applies");

        node.conditionValue = "City";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.empty(), "ProcessIf Contains no match: skips");

        // NotContains
        node.condition = ProcessIfCondition::NotContains;
        node.conditionValue = "City";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.count("Tag") == 1, "ProcessIf NotContains: applies when absent");

        // Equals
        meta["Category"] = "Gun";
        node.condition = ProcessIfCondition::Equals;
        node.conditionValue = "Gun";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.count("Tag") == 1, "ProcessIf Equals: match applies");

        node.conditionValue = "gun";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.empty(), "ProcessIf Equals: case-sensitive, no match");

        // NotEquals
        node.condition = ProcessIfCondition::NotEquals;
        node.conditionValue = "gun";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.count("Tag") == 1, "ProcessIf NotEquals: applies when not equal");

        // StartsWith
        meta["Category"] = "SFX_Impact";
        node.condition = ProcessIfCondition::StartsWith;
        node.conditionValue = "SFX_";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.count("Tag") == 1, "ProcessIf StartsWith: match applies");

        // EndsWith
        node.condition = ProcessIfCondition::EndsWith;
        node.conditionValue = "Impact";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.count("Tag") == 1, "ProcessIf EndsWith: match applies");

        // IsEmpty / IsNotEmpty
        meta["Category"] = "";
        node.condition = ProcessIfCondition::IsEmpty;
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.count("Tag") == 1, "ProcessIf IsEmpty: applies on empty");

        meta["Category"] = "x";
        node.condition = ProcessIfCondition::IsNotEmpty;
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.count("Tag") == 1, "ProcessIf IsNotEmpty: applies on non-empty");

        // RegexMatch
        meta["Category"] = "SFX_Gun_01";
        node.condition = ProcessIfCondition::RegexMatch;
        node.conditionValue = "SFX_.*_\\d+";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.count("Tag") == 1, "ProcessIf RegexMatch: applies on match");

        node.conditionValue = "^AMB_";
        r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.empty(), "ProcessIf RegexMatch: skips on no match");
    }

    // ── executeNode: Comment (no-op) ──
    std::printf("\n--- executeNode: Comment ---\n");
    {
        WorkflowNode node;
        node.type = WorkflowNodeType::Comment;
        node.enabled = true;
        node.userNote = "This is a comment";

        Meta meta;
        meta["X"] = "Y";
        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.empty(), "Comment: no fields changed");
    }

    // ── executeNode: disabled node skipped ──
    std::printf("\n--- Disabled node ---\n");
    {
        WorkflowNode node;
        node.type = WorkflowNodeType::SetField;
        node.enabled = false;
        node.field = "X";
        node.value = "Y";

        Meta meta;
        auto r = WorkflowEngine::executeNode(node, meta, 0);
        CHECK(r.changedFields.empty(), "disabled node: no changes");
    }

    // ── executeWorkflow: pipeline chains changes ──
    std::printf("\n--- executeWorkflow: pipeline ---\n");
    {
        Workflow wf;
        wf.name = "test_pipeline";

        wf.addNode(WorkflowNodeType::SetField);
        wf.nodes.back().field = "Description";
        wf.nodes.back().value = "gunshot impact close";

        wf.addNode(WorkflowNodeType::ChangeCase);
        wf.nodes.back().field = "Description";
        wf.nodes.back().caseType = CaseChangeType::ToUpperCase;

        wf.addNode(WorkflowNodeType::TakeFirstWords);
        wf.nodes.back().field = "Description";
        wf.nodes.back().wordCount = 2;

        Meta meta;
        auto r = WorkflowEngine::executeWorkflow(wf, meta, 0);
        CHECK(r.success, "pipeline: success");
        CHECK(r.changedFields["Description"] == "GUNSHOT IMPACT",
              "pipeline: set -> upper -> take2 = GUNSHOT IMPACT");
    }

    // ── executeWorkflow: disabled node in pipeline ──
    std::printf("\n--- executeWorkflow: disabled node in pipeline ---\n");
    {
        Workflow wf;
        wf.name = "skip_test";

        wf.addNode(WorkflowNodeType::SetField);
        wf.nodes.back().field = "X";
        wf.nodes.back().value = "hello";

        wf.addNode(WorkflowNodeType::ChangeCase);
        wf.nodes.back().field = "X";
        wf.nodes.back().caseType = CaseChangeType::ToUpperCase;
        wf.nodes.back().enabled = false;

        Meta meta;
        auto r = WorkflowEngine::executeWorkflow(wf, meta, 0);
        CHECK(r.changedFields["X"] == "hello", "pipeline: disabled case node skipped, stays lowercase");
    }

    // ── executeWorkflow: error halts pipeline ──
    std::printf("\n--- executeWorkflow: error halts ---\n");
    {
        Workflow wf;
        wf.name = "error_test";

        wf.addNode(WorkflowNodeType::SetField);
        wf.nodes.back().field = "X";
        wf.nodes.back().value = "test";

        wf.addNode(WorkflowNodeType::FindAndReplace);
        wf.nodes.back().field = "X";
        wf.nodes.back().findText = "[bad(";
        wf.nodes.back().replaceText = "";
        wf.nodes.back().useRegex = true;

        wf.addNode(WorkflowNodeType::SetField);
        wf.nodes.back().field = "Y";
        wf.nodes.back().value = "should not reach";

        Meta meta;
        auto r = WorkflowEngine::executeWorkflow(wf, meta, 0);
        CHECK(!r.success, "error pipeline: not success");
        CHECK(r.changedFields.count("Y") == 0, "error pipeline: Y not set (halted)");
    }

    // ── Workflow management: add / get / remove ──
    std::printf("\n--- Workflow management ---\n");
    {
        while (!WorkflowEngine::getWorkflows().empty()) {
            WorkflowEngine::removeWorkflow(WorkflowEngine::getWorkflows().front().name);
        }

        Workflow wf1;
        wf1.name = "Alpha";
        wf1.description = "First workflow";
        WorkflowEngine::addWorkflow(wf1);
        CHECK(WorkflowEngine::getWorkflows().size() == 1, "add Alpha: size = 1");

        Workflow wf2;
        wf2.name = "Beta";
        wf2.description = "Second workflow";
        WorkflowEngine::addWorkflow(wf2);
        CHECK(WorkflowEngine::getWorkflows().size() == 2, "add Beta: size = 2");

        auto* found = WorkflowEngine::getWorkflow("Alpha");
        CHECK(found != nullptr, "getWorkflow Alpha: found");
        CHECK(found->description == "First workflow", "getWorkflow Alpha: desc correct");

        CHECK(WorkflowEngine::getWorkflow("Nonexistent") == nullptr, "getWorkflow missing: null");

        Workflow wf1b;
        wf1b.name = "Alpha";
        wf1b.description = "Updated first";
        WorkflowEngine::addWorkflow(wf1b);
        CHECK(WorkflowEngine::getWorkflows().size() == 2, "overwrite Alpha: size still 2");
        CHECK(WorkflowEngine::getWorkflow("Alpha")->description == "Updated first",
              "overwrite Alpha: desc updated");

        WorkflowEngine::removeWorkflow("Alpha");
        CHECK(WorkflowEngine::getWorkflows().size() == 1, "remove Alpha: size = 1");
        CHECK(WorkflowEngine::getWorkflow("Alpha") == nullptr, "remove Alpha: gone");

        WorkflowEngine::removeWorkflow("Nonexistent");
        CHECK(WorkflowEngine::getWorkflows().size() == 1, "remove nonexistent: no change");

        WorkflowEngine::removeWorkflow("Beta");
    }

    // ── Serialization roundtrip ──
    std::printf("\n--- Serialization roundtrip ---\n");
    {
        Workflow wf;
        wf.name = "SerTest";
        wf.description = "serialization test";

        wf.addNode(WorkflowNodeType::SetField);
        wf.nodes.back().field = "Desc";
        wf.nodes.back().value = "hello";

        wf.addNode(WorkflowNodeType::FindAndReplace);
        wf.nodes.back().field = "Name";
        wf.nodes.back().findText = "old";
        wf.nodes.back().replaceText = "new";
        wf.nodes.back().useRegex = true;

        wf.addNode(WorkflowNodeType::Comment);
        wf.nodes.back().userNote = "a note";
        wf.nodes.back().enabled = false;

        std::string serialized = WorkflowEngine::serializeWorkflow(wf);
        CHECK(!serialized.empty(), "serialize: non-empty output");

        Workflow restored;
        bool ok = WorkflowEngine::deserializeWorkflow(serialized, restored);
        CHECK(ok, "deserialize: success");
        CHECK(restored.name == "SerTest", "roundtrip: name preserved");
        CHECK(restored.description == "serialization test", "roundtrip: desc preserved");
        CHECK(restored.nodes.size() == 3, "roundtrip: 3 nodes");

        if (restored.nodes.size() >= 3) {
            CHECK(restored.nodes[0].type == WorkflowNodeType::SetField, "roundtrip: node0 type");
            CHECK(restored.nodes[0].field == "Desc", "roundtrip: node0 field");
            CHECK(restored.nodes[0].value == "hello", "roundtrip: node0 value");

            CHECK(restored.nodes[1].type == WorkflowNodeType::FindAndReplace, "roundtrip: node1 type");
            CHECK(restored.nodes[1].useRegex, "roundtrip: node1 useRegex");

            CHECK(restored.nodes[2].type == WorkflowNodeType::Comment, "roundtrip: node2 type");
            CHECK(!restored.nodes[2].enabled, "roundtrip: node2 disabled preserved");
            CHECK(restored.nodes[2].userNote == "a note", "roundtrip: node2 userNote");
        }
    }

    // ── exportWorkflowJson / importWorkflowJson ──
    std::printf("\n--- export/import JSON ---\n");
    {
        Workflow wf;
        wf.name = "ExportTest";
        wf.description = "export test";
        wf.addNode(WorkflowNodeType::ChangeCase);
        wf.nodes.back().field = "Title";

        std::string exported = WorkflowEngine::exportWorkflowJson(wf);
        CHECK(!exported.empty(), "export: non-empty");

        Workflow imported;
        bool ok = WorkflowEngine::importWorkflowJson(exported, imported);
        CHECK(ok, "import: success");
        CHECK(imported.name == "ExportTest", "import: name preserved");
    }

    // ── WorkflowResult defaults ──
    std::printf("\n--- WorkflowResult defaults ---\n");
    {
        WorkflowResult r;
        CHECK(r.success, "default: success = true");
        CHECK(r.error.empty(), "default: error empty");
        CHECK(r.changedFields.empty(), "default: no changed fields");
    }

    std::printf("\n=== WorkflowEngine: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
