# -*- coding: utf-8 -*-
"""任务栏固定项图标刷新：只定向刷新用默认图标的自家固定项，不清缓存、不改快捷方式。"""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "WindowsMain.cpp").read_text(encoding="utf-8")
PLATFORM = (ROOT / "src" / "platform" / "system_win32.cpp").read_text(encoding="utf-8")
MACOS = (ROOT / "src" / "platform" / "system_macos.mm").read_bytes()
SYSTEM_H = (ROOT / "src" / "platform" / "system.h").read_text(encoding="utf-8")
HEADER = (ROOT / "src" / "platform" / "pinned_taskbar_icon_refresh.h").read_text(encoding="utf-8")
INSTALLER = (ROOT / "installer" / "HarkenSearch_Setup.iss").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    end = source.index("\n}\n", start)
    return source[start:end]


class PinnedTaskbarIconRefreshContract(unittest.TestCase):
    def setUp(self) -> None:
        self.refresh = function_body(PLATFORM, "std::uint32_t RefreshPinnedTaskbarIcons()")

    def test_platform_api_is_declared_for_the_client_and_the_installer_entry(self) -> None:
        self.assertIn("std::uint32_t RefreshPinnedTaskbarIcons();", SYSTEM_H)
        self.assertIn("std::uint32_t RefreshPinnedTaskbarIcons()", PLATFORM)
        self.assertIn(b"std::uint32_t RefreshPinnedTaskbarIcons()", MACOS)

    def test_refresh_is_targeted_and_never_wipes_the_icon_cache(self) -> None:
        # 恢复旧图标只允许对单个固定项发更新通知；清空或重建整个图标缓存会波及别的应用。
        self.assertIn("SHCNE_UPDATEITEM", self.refresh)
        self.assertIn("SHCNF_PATHW", self.refresh)
        self.assertNotIn("SHCNE_ASSOCCHANGED", self.refresh)
        self.assertNotIn("SHCNE_ASSOCCHANGED", HEADER)
        self.assertLessEqual(self.refresh.count("SHChangeNotify("), 2)

    def test_only_pins_using_the_app_default_icon_are_refreshed(self) -> None:
        # 命中判定必须先过纯函数；用户自选图标和别的应用的固定项在通知前就被排除。
        self.assertIn("PinnedShortcutUsesDefaultAppIcon", self.refresh)
        self.assertLess(self.refresh.index("PinnedShortcutUsesDefaultAppIcon"),
                        self.refresh.index("SHChangeNotify("))
        self.assertIn("bool PinnedShortcutUsesDefaultAppIcon(", HEADER)
        self.assertIn("iconIndex <= 0", HEADER)
        self.assertIn("PinnedIconPathsEqual(icon, exePath)", HEADER)

    def test_pins_are_only_read_never_rewritten(self) -> None:
        for writer in ("SetPath", "SetIconLocation", "SetArguments", "SetWorkingDirectory", "->Save("):
            self.assertNotIn(writer, self.refresh)

    def test_pinned_shortcuts_come_from_the_current_users_pin_folder(self) -> None:
        self.assertIn("FOLDERID_RoamingAppData", self.refresh)
        self.assertIn("User Pinned", PLATFORM)
        self.assertIn("*.lnk", self.refresh)
        # 读取快捷方式一律只读打开，刷新不会改动固定项内容。
        self.assertIn("STGM_READ", PLATFORM)
        self.assertIn("ReadShortcutTarget(shortcutPath", self.refresh)

    def test_platform_file_has_no_cross_module_link_dependency(self) -> None:
        # 多个小测试目标只链接本文件，取 EXE 路径必须自带，不能依赖 executable-path 模块。
        self.assertNotIn("GetExecutablePath", PLATFORM)
        self.assertIn("GetModuleFileNameW", PLATFORM)

    def test_headless_entry_runs_before_the_single_instance_broker(self) -> None:
        # 安装器以非提权用户启动本 EXE：即使主程序正开着也必须只刷新并退出，不能附着到已有窗口。
        flag = MAIN.index("--refresh-pinned-taskbar-icons")
        broker = MAIN.index("CreateMutexW(nullptr, FALSE, auroMutexName.c_str())")
        self.assertLess(flag, broker)
        entry = MAIN[flag:broker]
        self.assertIn("Platform::RefreshPinnedTaskbarIcons();", entry)
        self.assertIn("return 0;", entry)

    def test_installer_entry_does_not_hash_the_exe_first(self) -> None:
        # 安装器在结束页就能点启动；刷新入口若排在整包 SHA-512 校验之后，
        # 会多出上秒级延迟，首次启动会先看到旧图标。
        flag = MAIN.index("--refresh-pinned-taskbar-icons")
        integrity = MAIN.index("EXE integrity check")
        self.assertLess(flag, integrity)

    def test_icon_read_failure_is_not_treated_as_default_icon(self) -> None:
        # 空 iconLocation 代表「没设过图标」。GetIconLocation 失败时必须整条跳过，
        # 否则会把没读出来的固定项当成默认图标去刷新。
        read = PLATFORM[PLATFORM.index("bool ReadShortcutTarget("):PLATFORM.index("CurrentExecutablePath")]
        self.assertIn("iconLocationRead", read)
        icon_check = read.index("if (iconLocationRead)")
        self.assertGreater(read.index("iconLocation.assign(icon);"), icon_check)
        self.assertGreater(read.index("targetPath.assign(target);"), icon_check)

    def test_startup_refresh_stays_off_the_ui_path(self) -> None:
        self.assertRegex(
            MAIN,
            r"std::thread\(\[\] \{ \(void\)Platform::RefreshPinnedTaskbarIcons\(\); \}\)\.detach\(\);",
        )
        icon_block = MAIN.index("SendMessageW(hwnd, WM_SETICON, ICON_SMALL")
        self.assertGreater(MAIN.index("Platform::RefreshPinnedTaskbarIcons();", icon_block), icon_block)

    def test_startup_refresh_is_disabled_in_preview_builds(self) -> None:
        # 预览/QA 构建不得对真实用户的任务栏固定项发 shell 通知。
        icon_block = MAIN.index("SendMessageW(hwnd, WM_SETICON, ICON_SMALL")
        guarded = MAIN[icon_block:]
        self.assertRegex(
            guarded,
            r"#if !ARIS_PREVIEW_ISOLATION\n(?:[^\n]*\n){0,6}\s*std::thread\(\[\] \{ \(void\)Platform::RefreshPinnedTaskbarIcons\(\); \}\)\.detach\(\);\n#endif",
        )

    def test_installer_icon_refresh_failure_does_not_interrupt_installation(self) -> None:
        self.assertNotRegex(INSTALLER, r"(?m)^Filename:.*--refresh-pinned-taskbar-icons")
        self.assertIn("procedure RefreshPinnedIconsAfterInstall;", INSTALLER)
        refresh = INSTALLER.split("procedure RefreshPinnedIconsAfterInstall;", 1)[1]
        refresh = refresh.split("procedure CurStepChanged", 1)[0]
        self.assertIn("if not ExecAsOriginalUser", refresh)
        self.assertIn("'--refresh-pinned-taskbar-icons'", refresh)
        self.assertIn("ewWaitUntilTerminated", refresh)
        self.assertIn("except", refresh)
        self.assertIn("Log(", refresh)
        self.assertNotIn("MsgBox(", refresh)
        self.assertIn("RefreshPinnedIconsAfterInstall;", INSTALLER.split("procedure CurStepChanged", 1)[1])

    def test_installer_only_launches_from_checked_interactive_finish(self) -> None:
        self.assertIn("WizardForm.RunList.AddCheckBox", INSTALLER)
        self.assertIn("WizardForm.RunList.Checked[FinishedLaunchIndex]", INSTALLER)
        self.assertIn("WizardForm.RunList.Items.Delete(FinishedLaunchIndex);", INSTALLER)
        self.assertIn("not WizardSilent", INSTALLER)
        self.assertIn("procedure LaunchInstalledHarken;", INSTALLER)
        launch = INSTALLER.split("procedure LaunchInstalledHarken;", 1)[1]
        launch = launch.split("function NextButtonClick", 1)[0]
        self.assertIn("ShellExecAsOriginalUser('runas'", launch)
        self.assertIn("CustomMessage('PostInstallLaunchFailed')", launch)
        self.assertIn("ewNoWait", launch)


if __name__ == "__main__":
    unittest.main()
