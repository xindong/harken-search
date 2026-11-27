"""REQ-20261126-UNIFIED-BATCH-EDIT: source and locale contracts.

Covers what the pure preview test cannot: one dialog with a target selector
instead of tabs, a guarded single-column write verified against the editable
column registry, and every new UI string in all three languages.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class BatchDialogContract(unittest.TestCase):
    def setUp(self) -> None:
        self.main = read("src/main.cpp")

    def test_old_tabs_are_gone(self) -> None:
        self.assertNotIn("##BatchOperationTabs", self.main)
        self.assertNotIn("state.batchDialogTab", self.main)

    def test_single_target_selector_drives_the_dialog(self) -> None:
        for key in ("Batch_EditTargetFileName", "Batch_EditTargetTitle",
                    "Batch_EditTargetDescription", "Batch_EditTargetKeywords",
                    "Batch_EditTargetCategory"):
            with self.subTest(key=key):
                self.assertIn("L(" + key + ")", self.main)
        self.assertIn("state.batchEditTarget == 0", self.main)
        self.assertIn('ImGui::Combo("##BatchEditTarget"', self.main)
        self.assertNotIn("ImGui::RadioButton(L(Batch_EditTarget", self.main)

    def test_tools_and_selection_open_one_dock_panel(self) -> None:
        start = self.main.index("if (toolsMenuOpen)")
        tools = self.main[start:self.main.index("ImGui::EndMenu()", start)]
        self.assertRegex(tools, r"MenuItem\(L\(Dlg_BatchOp\),[^\n]+\n\s*state\.openBatchEdit\(\)")
        self.assertNotIn("ImGui::OpenPopup(L(Dlg_BatchOp))", self.main)
        self.assertIn('renderFunctionFloatingWindow("mod-batch-edit"', self.main)
        self.assertIn('renderFunctionDockedModule(leaf, "mod-batch-edit"', self.main)
        self.assertIn('"mod-batch-edit"', read("src/DockTabGroups.h"))

    def test_targets_share_the_filename_queue_format(self) -> None:
        self.assertEqual(self.main.count('BeginTransientScrollbarChild("##BatchEditQueue"'), 1)
        self.assertTrue("DrawBatchEditQueue(state.renameRuleQueue" in self.main)
        self.assertTrue("DrawBatchEditQueue(state.batchRuleEditRules" in self.main)
        self.assertEqual(self.main.count("BatchEditQueueHeight("), 2)

    def test_targets_share_one_status_before_after_table(self) -> None:
        self.assertEqual(self.main.count('BeginTransientScrollbarTable("##BatchEditPreview"'), 1)
        self.assertTrue("DrawBatchEditPreviewTable(state.renamePreviewList" in self.main)
        self.assertTrue("DrawBatchEditPreviewTable(state.batchRuleEditPreview.rows" in self.main)

    def test_old_metadata_tab_body_removed(self) -> None:
        self.assertNotIn("s_batchMetaSaveSuccess", self.main)
        self.assertNotIn("BatchMetaDesc", self.main)


class GuardedWriteContract(unittest.TestCase):
    def setUp(self) -> None:
        self.database = read("src/Database.cpp")
        start = self.database.find("Database::applyMetadataPatchManyGuarded(")
        self.assertGreater(start, 0, "guarded batch writer is missing")
        end = self.database.find("bool Database::applyMetadataPatch(", start)
        self.assertGreater(end, start)
        self.body = self.database[start:end]

    def test_guard_uses_the_editable_column_registry(self) -> None:
        self.assertIn("EditableTextColumns().count(expectedColumn) == 0", self.body)
        self.assertIn("entry.second.fields.count(expectedColumn) != 1", self.body)

    def test_verification_runs_inside_one_immediate_transaction(self) -> None:
        self.assertIn("BEGIN IMMEDIATE TRANSACTION", self.body)
        verify = self.body.find("SELECT file_path FROM audio_files WHERE id = ?1")
        self.assertGreater(verify, 0)
        self.assertGreater(self.body.find("commitTransaction()"), verify)
        self.assertIn("GuardedPatchManyRejected: reason=preview_mismatch", self.body)

    def test_header_declares_guarded_writer(self) -> None:
        self.assertIn("applyMetadataPatchManyGuarded(", read("src/Database.h"))


class EditorContract(unittest.TestCase):
    def setUp(self) -> None:
        self.main = read("src/main.cpp")

    def test_editor_submits_only_changed_rows_of_the_chosen_column(self) -> None:
        start = self.main.find("bool ApplyBatchRuleEdit(AppState& state)")
        self.assertGreater(start, 0)
        body = self.main[start:self.main.find("struct BatchEditPaneLayout")]
        self.assertIn("if (!row.error.empty() || row.blockedEmpty || !row.changed) continue;", body)
        self.assertIn("patch.set(column, row.newValue)", body)
        self.assertIn("expected[row.id] = row.oldValue", body)
        # 写入走同一套后台批量会话，守护参数原样传下去。
        self.assertIn("applyMetadataPatchManyGuarded(", body)
        self.assertIn("items, column, expected,", body)
        self.assertIn("cancelFlag", body)

    def test_preview_is_dropped_when_conditions_change(self) -> None:
        self.assertIn("BuildRuleEditSignature(column, records, state.batchRuleEditRules)", self.main)
        self.assertIn("batchRuleEditStale = true", self.main)

    def test_metadata_apply_registers_one_undo_after_the_transaction(self) -> None:
        start = self.main.index("bool ApplyBatchRuleEdit(AppState& state)")
        end = self.main.index("struct BatchEditPaneLayout", start)
        body = self.main[start:end]
        self.assertIn("std::make_shared<MetadataPatchOp>()", body)
        self.assertIn("row.id, {{column, row.oldValue}}, {{column, row.newValue}}", body)
        # 后台写完再回填：撤销只在一整批成功时入栈一次。
        success = body.index("if (res.failed == 0 && res.success > 0)")
        self.assertGreater(body.index("undoStack.push("), success)
        self.assertEqual(body.count("undoStack.push("), 1)

    def test_idle_metadata_preview_reuses_the_existing_result_revision(self) -> None:
        start = self.main.index("static bool ValidateBatchMetadataPreview(")
        end = self.main.index("namespace aura::ui", start)
        body = self.main[start:end]
        self.assertLess(body.index("state.searchResultsGen_"), body.index("CollectRuleEditRecords("))
        self.assertLess(body.index("state.database.dataRevision()"), body.index("CollectRuleEditRecords("))
        self.assertIn("state.selectedIndicesSet", body)


class LocaleContract(unittest.TestCase):
    def test_new_keys_exist_in_three_languages(self) -> None:
        ledger = json.loads(read("locale/harken.ui.strings.json"))
        entries = {entry["id"]: entry for entry in ledger["entries"]}
        ids = [entry["id"] for entry in ledger["entries"]]
        self.assertEqual(len(ids), len(set(ids)), "duplicate locale ids")
        required = [
            "Batch_EditTarget", "Batch_EditTargetFileName", "Batch_EditTargetTitle",
            "Batch_EditTargetDescription", "Batch_EditTargetKeywords", "Batch_EditTargetCategory",
            "Batch_EditRules", "Batch_RuleEditApplyBtn", "Batch_RuleEditStale",
            "Batch_RuleEditAppliedFmt", "Batch_RuleEditFailedFmt",
            "Batch_RuleEditNoChanges", "Batch_RuleEditBlocked",
            "Batch_RuleEditOldValue", "Batch_RuleEditNewValue",
            "Batch_RuleEditValue", "Batch_RuleEditFixedValue", "Batch_RuleEditSeparator",
            "Batch_RuleModeSetFixed", "Batch_RuleModeAppendBefore", "Batch_RuleModeAppendAfter",
        ]
        for key in required:
            with self.subTest(key=key):
                self.assertIn(key, entries)
                for language in ("zh", "en", "ja"):
                    self.assertTrue(entries[key].get(language), f"{key} missing {language}")

    def test_dialog_is_renamed(self) -> None:
        ledger = json.loads(read("locale/harken.ui.strings.json"))
        entry = next(item for item in ledger["entries"] if item["id"] == "Dlg_BatchOp")
        self.assertEqual(entry["zh"], "批量编辑")
        self.assertEqual(entry["en"], "Batch Edit")

    def test_format_specifiers_keep_the_same_order(self) -> None:
        ledger = json.loads(read("locale/harken.ui.strings.json"))
        entries = {entry["id"]: entry for entry in ledger["entries"]}
        spec = re.compile(r"%(?:\d+)?[sd]")
        for key in ("RenPreview_Summary", "Batch_RuleEditAppliedFmt", "Batch_RuleEditFailedFmt"):
            entry = entries[key]
            with self.subTest(key=key):
                self.assertEqual(spec.findall(entry["zh"]), spec.findall(entry["en"]))
                self.assertEqual(spec.findall(entry["zh"]), spec.findall(entry["ja"]))


class UndoLabelsContract(unittest.TestCase):
    def test_edit_menu_uses_its_own_next_action_for_undo_and_redo(self) -> None:
        main = read("src/main.cpp")
        menu = main[main.index("if (editMenuOpen)"):main.index("ImGui::MenuItem(L(Undo_HistoryTitle))")]
        for action in ("Undo", "Redo"):
            with self.subTest(action=action):
                self.assertRegex(
                    menu,
                    rf"RenderUndoMenuLabel\(LStr::Undo_{action}Btn, LStr::Undo_{action}ActionFmt,\s*"
                    rf"state\.undoStack\.{action.lower()}Description\(\)\)",
                )
        batch_apply = main[main.index("bool ApplyBatchRuleEdit("):main.index("undoOp->entries.reserve")]
        self.assertNotIn('operationName = "Batch edit metadata"', batch_apply)
        self.assertIn("undoOp->targetColumn = column", batch_apply)

    def test_chinese_formats_quote_the_action_and_name_the_column(self) -> None:
        ledger = json.loads(read("locale/harken.ui.strings.json"))
        entries = {entry["id"]: entry for entry in ledger["entries"]}
        undo = entries["Undo_UndoActionFmt"]
        redo = entries["Undo_RedoActionFmt"]
        batch = entries["Undo_BatchEditColumnFmt"]
        self.assertEqual(undo["zh"] % (batch["zh"] % "标题"), "撤销「批量编辑标题」")
        self.assertEqual(redo["zh"] % (batch["zh"] % "关键词"), "重做「批量编辑关键词」")
        for entry in (undo, redo, batch, entries["Undo_EditColumnFmt"]):
            for language in ("zh", "en", "ja"):
                self.assertEqual(entry[language].count("%s"), 1)


if __name__ == "__main__":
    unittest.main()

