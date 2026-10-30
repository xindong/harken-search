import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_UI = (ROOT / "src" / "ModuleUI.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")


class ModuleUiFilesystemContract(unittest.TestCase):
    def test_naming_routes_file_state_queries_through_pal(self):
        self.assertIn('#include "platform/filesystem.h"', MODULE_UI)
        self.assertIn("static Platform::FileSystem::PathStatus ProbeUiPath", MODULE_UI)
        self.assertIn("Platform::FileSystem::ProbePath(PathToUtf8(path))", MODULE_UI)

        naming = MODULE_UI[
            MODULE_UI.index("static void RenderNamingDialog"):
            MODULE_UI.index("static TransferPrefs")
        ]
        old_probe = naming.index("ProbeUiPath(oldP)")
        new_probe = naming.index("ProbeUiPath(newP)")
        rename = naming.index("RenameFileOnDisk")
        self.assertLess(old_probe, new_probe)
        self.assertLess(new_probe, rename)
        self.assertIn("!= Platform::FileSystem::PathStatus::Present", naming)
        self.assertIn("!= Platform::FileSystem::PathStatus::Missing", naming)

    def test_transfer_preserves_conflict_policy_and_routes_io_through_pal(self):
        transfer = MODULE_UI[
            MODULE_UI.index("void RenderTransferDialogContent"):
            MODULE_UI.index("void RenderModuleDialogs")
        ]
        for token in (
            "Platform::FileSystem::EnsureDirectoryExists",
            "ProbeUiPath(destPath)",
            "Platform::FileSystem::PathStatus::Error",
            "Platform::FileSystem::PathStatus::Missing",
            "Platform::FileSystem::PathStatus::Present",
            "ExistingFileBehavior::SkipExisting",
            "ExistingFileBehavior::AlwaysCreateNew",
            "Platform::FileSystem::CopyFile",
        ):
            self.assertIn(token, transfer)
        self.assertIn("while (destStatus == Platform::FileSystem::PathStatus::Present)", transfer)
        self.assertIn("PathToUtf8(srcPath)", transfer)
        self.assertIn("PathToUtf8(destPath)", transfer)
        self.assertIn("true)", transfer)
        self.assertLess(transfer.index("EnsureDirectoryExists"), transfer.index("ProbeUiPath(destPath)"))
        self.assertLess(transfer.index("ProbeUiPath(destPath)"), transfer.index("CopyFile"))

    def test_direct_io_is_gone_while_ui_and_pure_path_contracts_stay_fixed(self):
        for token in ("fs::exists(", "fs::create_directories(", "fs::copy_file("):
            self.assertNotIn(token, MODULE_UI)
        for token in (
            "oldP.parent_path()",
            "srcPath.filename()",
            "destPath.stem()",
            "destPath.extension()",
            'ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver)',
            'aura::ui::ActionButton(L(ModuleUI_RunRename), ImVec2(140, 30), aura::ui::ButtonVariant::PrimarySearch)',
            'aura::ui::ActionButton(L(ModuleUI_TransferStart), ImVec2(140, 35), aura::ui::ButtonVariant::PrimaryTransfer)',
            'L(Module_RenameDoneFmt)',
            'L(Module_TransferDoneFmt)',
            "Platform::BrowseFolderDialogModern(",
            "HarkenSearch::UiHost::MainWindowOwner()",
            'L(Folder_PickAudioFolder)',
        ):
            self.assertIn(token, MODULE_UI)
        self.assertNotIn("OpenFolderDialog(g_hwnd)", MODULE_UI)

    def test_contract_is_registered(self):
        self.assertIn("TestModuleUiFilesystemContract", CMAKE)
        self.assertIn("tests/test_module_ui_filesystem_contract.py", CMAKE)


if __name__ == "__main__":
    unittest.main()
