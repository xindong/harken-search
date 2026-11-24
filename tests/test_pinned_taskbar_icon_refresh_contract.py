# -*- coding: utf-8 -*-
"""任务栏固定项图标刷新：只定向刷新用默认图标的自家固定项，不清缓存、不改快捷方式。"""

from pathlib import Path
import re
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

    def test_headless_entry_runs_before_the_single_instance_broker(self) -> None:
        # 安装器以非提权用户启动本 EXE：即使主程序正开着也必须只刷新并退出，不能附着到已有窗口。
        flag = MAIN.index("--refresh-pinned-taskbar-icons")
        broker = MAIN.index("CreateMutexW(nullptr, FALSE, auroMutexName.c_str())")
        self.assertLess(flag, broker)
        entry = MAIN[flag:broker]
        self.assertIn("Platform::RefreshPinnedTaskbarIcons();", entry)
        self.assertIn("return 0;", entry)

    def test_startup_refresh_stays_off_the_ui_path(self) -> None:
        self.assertRegex(
            MAIN,
            r"std::thread\(\[\] \{ \(void\)Platform::RefreshPinnedTaskbarIcons\(\); \}\)\.detach\(\);",
        )
        icon_block = MAIN.index("SendMessageW(hwnd, WM_SETICON, ICON_SMALL")
        self.assertGreater(MAIN.index("Platform::RefreshPinnedTaskbarIcons();", icon_block), icon_block)

    def test_installer_refreshes_as_the_original_non_elevated_user(self) -> None:
        entries = [line for line in INSTALLER.splitlines()
                   if "--refresh-pinned-taskbar-icons" in line]
        self.assertEqual(1, len(entries), entries)
        entry = entries[0]
        self.assertIn('Filename: "{app}\\{#MyAppExeName}"', entry)
        # 提权进程发 Shell 通知会被 UIPI 挡住，必须以安装前的当前用户身份执行。
        self.assertIn("runasoriginaluser", entry)
        # 应用内静默升级同样要在首次启动前刷新，因此不能跳过静默安装。
        self.assertNotIn("skipifsilent", entry)

    def test_installer_launch_entry_keeps_its_existing_behaviour(self) -> None:
        launches = [line for line in INSTALLER.splitlines()
                    if "LaunchProgram" in line and "Filename:" in line]
        self.assertEqual(1, len(launches))
        self.assertIn("postinstall skipifsilent", launches[0])


if __name__ == "__main__":
    unittest.main()
