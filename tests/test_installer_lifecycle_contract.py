from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class InstallerLifecycleContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.installer = (ROOT / "installer" / "HarkenSearch_Setup.iss").read_text(
            encoding="utf-8"
        )

    def test_installer_leaves_reaper_component_setup_to_the_app(self) -> None:
        installer = self.installer.lower()
        forbidden = (
            "[components]",
            "dawsendcomponents",
            "configurereaper",
            "installreapercomponent",
            "harkenreapersetup.exe",
            "reaper.exe",
            "extracttemporaryfile('reaper_harken.dll')",
        )
        self.assertEqual([marker for marker in forbidden if marker in installer], [])
        self.assertIn(
            'Source: "{#SourceDir}\\reaper_harken.dll"; DestDir: "{app}"; Flags: ignoreversion',
            self._files_section().splitlines(),
        )

    def test_formal_installer_stages_runtime_payload(self) -> None:
        required_entries = (
            'Source: "{#SourceDir}\\lucide.ttf"; DestDir: "{app}";',
            'Source: "{#SourceDir}\\splash_beta.png"; DestDir: "{app}";',
            'Source: "{#SourceDir}\\assets\\*"; DestDir: "{app}\\assets";',
            'Source: "{#SourceDir}\\HarkenSearchProToolsBridge.exe"; DestDir: "{app}";',
        )
        for entry in required_entries:
            with self.subTest(entry=entry):
                self.assertIn(entry, self.installer)

        reaper_entry = next(
            line
            for line in self.installer.splitlines()
            if "reaper_harken.dll" in line and line.lstrip().startswith("Source:")
        )
        self.assertNotIn("skipifsourcedoesntexist", reaper_entry)

    def test_silent_uninstall_preserves_user_data_without_explicit_opt_in(self) -> None:
        self.assertIn("UninstallSilent", self.installer)
        self.assertIn("/DELETEUSERDATA", self.installer)
        self.assertIn("ShouldDeleteUserDataOnUninstall", self.installer)

    def test_uninstall_does_not_delete_another_installations_shell_registration(self) -> None:
        self.assertNotIn(
            "RegDeleteKeyIncludingSubkeys(HKEY_CLASSES_ROOT, 'Directory\\shell\\ArisScan')",
            self.installer,
        )

    def test_harken_appid_is_distinct_from_legacy_aris_appid(self) -> None:
        self.assertIn('#define MyAppId "{{31B6E215-3D2B-43EA-88C8-72D73ABC9A0C}}"', self.installer)
        self.assertIn("A8B3C9D1-E4F5-4A6B-8C9D-0E1F2A3B4C5D", self.installer)
        self.assertNotIn('#define MyAppId "{{A8B3C9D1-E4F5-4A6B-8C9D-0E1F2A3B4C5D}}"', self.installer)
        self.assertIn("{A8B3C9D1-E4F5-4A6B-8C9D-0E1F2A3B4C5D}_is1", self.installer)

    def test_hkcu_uninstall_is_not_elevated_on_file_existence_alone(self) -> None:
        body = self._function_body("ExecLegacyUninstaller")
        exists_at = body.index("FileExists(ExePath)")
        trusted_at = body.index("IsTrustedLegacyUninstaller(ExePath, InstallLocation)")
        self.assertLess(trusted_at, exists_at)
        self.assertIn("not IsTrustedLegacyUninstaller(ExePath, InstallLocation)", body)
        self.assertNotIn("Exec(ExePath", body[:trusted_at])
        self.assertIn("Exec(TrustedLegacyStagedExe", body)
        trusted = self._function_body("IsTrustedLegacyUninstaller")
        self.assertIn("IsUserProfileInstallDir(InstallLocation)", trusted)
        self.assertIn("unins000.exe", trusted)
        self.assertIn("AddBackslash(InstallLocation) + 'unins000.exe'", trusted)
        guard = "&& defined(TrustedSignerSubject)"
        self.assertIn("#if defined(SignToolPath)", trusted)
        self.assertLess(trusted.index(guard), trusted.index("ExtractTemporaryFile"))
        self.assertIn("FileExists(ToolPath)", trusted)
        self.assertIn("FileCopy(ExePath, StageExe, False)", trusted)
        self.assertIn("SigntoolLeafSubject(ToolPath, StageExe, Subject)", trusted)
        self.assertIn("TrustedLegacyStagedExe := StageExe", trusted)
        self.assertIn('Source: "{#SignToolPath}"; DestDir: "{tmp}"; Flags: dontcopy', self.installer)
        self.assertNotIn("Windows Kits", self.installer)

    def test_signature_identity_matches_leaf_subject_not_whole_output(self) -> None:
        subject = self._function_body("SigntoolLeafSubject")
        self.assertIn("verify /pa /v", subject)
        self.assertIn("ResultCode <> 0", subject)
        self.assertIn("Signing Certificate Chain:", subject)
        self.assertIn("Issued to:", subject)
        trusted = self._function_body("IsTrustedLegacyUninstaller")
        self.assertIn("{#TrustedSignerSubject}", trusted)
        self.assertNotIn("Pos('Aris', Output)", trusted)
        self.assertNotIn("Pos('Harken Search', Output)", trusted)

    def test_untrusted_legacy_uninstaller_skips_instead_of_blocking_upgrade(self) -> None:
        caller = self._function_body("UninstallLegacyArisIfPresent")
        self.assertIn("ExecLegacyUninstaller(UninstallString, InstallLocation, Attempted)", caller)
        self.assertIn("else if Attempted then", caller)
        attempted_block = caller[caller.index("else if Attempted") : caller.index("DeleteLegacyStaleFiles")]
        self.assertIn("CustomMessage('LegacyUninstallFailed')", attempted_block)
        self.assertIn("chinesesimp.LegacyUninstallFailed=卸载旧版本失败。已停止安装", self.installer)
        self.assertIn("english.LegacyUninstallFailed=The old version could not be uninstalled. Setup has stopped", self.installer)
        self.assertIn("拒绝执行旧卸载程序", caller)

    def test_exec_and_log_output_helper_is_defined(self) -> None:
        self.assertIn("function ExecAndLogOutput", self.installer)
        helper = self._function_body("ExecAndLogOutput")
        self.assertIn("ExpandConstant('{cmd}')", helper)
        self.assertIn("> \"", helper)
        self.assertIn("LoadStringFromFile(OutFile, Output)", helper)

    def test_upgrade_silently_uninstalls_legacy_aris_when_idle(self) -> None:
        self.assertIn("/VERYSILENT", self.installer)
        self.assertIn("/NORESTART", self.installer)
        self.assertIn("UninstallString", self.installer)
        self.assertIn("InstallLocation", self.installer)
        self.assertIn("HarkenSearch.exe", self.installer)
        self.assertIn("HarkenSearchBootstrap.exe", self.installer)
        self.assertIn("Auro.exe", self.installer)
        self.assertIn("请先退出后再安装", self.installer)
        self.assertIn("不会强行关闭", self.installer)
        self.assertIn("卸载旧版本失败", self.installer)

    def test_legacy_uninstall_runs_at_install_step_not_wizard_start(self) -> None:
        init_start = self.installer.index("function InitializeSetup")
        init_end = self.installer.index("\nend;", init_start)
        initialize_setup = self.installer[init_start:init_end]
        self.assertNotIn("UninstallLegacyArisIfPresent", initialize_setup)
        self.assertNotIn("/VERYSILENT", initialize_setup)
        step_start = self.installer.index("procedure CurStepChanged")
        step_body = self.installer[step_start:]
        self.assertIn("ssInstall", step_body)
        self.assertIn("UninstallLegacyArisIfPresent", step_body)

    def test_stale_brand_files_are_deleted_from_install_dir_only(self) -> None:
        self.assertIn("[InstallDelete]", self.installer)
        self.assertIn("[UninstallDelete]", self.installer)
        for stale in (
            "ArisBootstrapCore.lib",
            "aris_thesaurus.json",
            "aura_diagnostics_live.jsonl",
            "user_actions.log",
        ):
            with self.subTest(stale=stale):
                self.assertIn(stale, self.installer)
        self.assertNotIn("ArisBootstrapCore.lib", self._files_section())
        self.assertNotIn("{localappdata}\\SoundManager", self.installer)
        self.assertNotIn("{localappdata}\\HarkenSearch", self.installer)
        self.assertNotIn("{userappdata}\\HarkenSearch", self.installer)

    def _function_body(self, name: str) -> str:
        marker = f"function {name}"
        start = self.installer.index(marker)
        end = self.installer.index("\nend;", start)
        return self.installer[start:end]

    def _files_section(self) -> str:
        marker = "[Files]"
        start = self.installer.index(marker)
        rest = self.installer[start + len(marker) :]
        next_section = rest.find("\n[")
        return rest if next_section < 0 else rest[:next_section]


if __name__ == "__main__":
    unittest.main()
