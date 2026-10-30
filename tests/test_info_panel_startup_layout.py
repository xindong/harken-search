"""Run the product's panel defaults and settings statements in isolation.

The real declarations and load/save code are compiled verbatim. Unrelated
audio/database/UI dependencies are excluded; no production settings are used.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def between(text: str, start: str, end: str) -> str:
    return text.split(start, 1)[1].split(end, 1)[0]

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    header = (ROOT / 'src/AppState.h').read_text(encoding='utf-8')
    source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
    enum = re.search(r'enum class DockSide\s*\{.*?\};', header, re.S).group()
    panel = re.search(r'struct PanelState\s*\{.*?\n\};', header, re.S).group()
    members = between(header, '// Panel states\n', '    std::vector<ModulePanelInstanceState> modulePanels;')
    legacy = between(header, '// Legacy compatibility\n', '    // Folder auto-rename')
    load = between(source.split('void LoadAppSettings(', 1)[1],
                   '// Panel states (new unified system)', '// UI sizing.')
    save = between(source.split('bool TrySaveAppSettings(', 1)[1],
                   '// Panel states (new unified system)', '// UI sizing')
    navigation = (ROOT / "src/InfoPageNavigation.h").read_text(encoding="utf-8").replace("#pragma once", "")
    cpp = navigation + "\n#define AURA_BUILD_DIAGNOSTICS 1\n" + r'''
#include <string>
#include <map>
#include <vector>
#include <utility>
#include <iostream>
using Values = std::map<std::string, std::string>;
int ParseInt(const std::string& s, int fallback) { try { return std::stoi(s); } catch (...) { return fallback; } }
float ParseFloat(const std::string& s, float fallback) { try { return std::stof(s); } catch (...) { return fallback; } }
bool ParseBool(const std::string& s, bool fallback) { return s.empty() ? fallback : s == "1" || s == "true"; }
''' + enum + '\n' + panel + '\nstruct AppState { using PanelState = ::PanelState;\n' + members + r'''
bool settingsDirty = false;
struct { InfoPageNavigation infoPages; } settings;
''' + legacy + '\n};\nvoid Load(AppState& state, Values kv) {\n' + load + r'''
}
Values Save(const AppState& state) {
std::vector<std::pair<std::string, std::string>> items;
''' + save + r'''
return Values(items.begin(), items.end());
}
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        if (!ok) ++failures;
    };
    AppState fresh;
    check(fresh.panelSidebar.dockSide == DockSide::Left && fresh.panelFileInfo.dockSide == DockSide::Right, "fresh profile: Filter left, Info right");
    AppState missing; Load(missing, {{"theme", "2"}});
    check(missing.panelFileInfo.dockSide == DockSide::Right, "settings without panel positions: Info right");
    Values old{{"panelFileInfo_dockSide", "0"}, {"fileInfoDockSide", "1"}, {"panelFileInfo_dockedWidth", "277"}, {"panelSidebar_dockedWidth", "321"}};
    AppState repaired; Load(repaired, old);
    check(repaired.panelFileInfo.dockSide == DockSide::Right, "legacy wrong left record repaired");
    check(repaired.panelFileInfo.dockedWidth == 277 && repaired.panelSidebar.dockedWidth == 321, "existing widths preserved");
    check(repaired.settingsDirty, "legacy repair scheduled for persistence");
    auto saved = Save(repaired);
    check(saved["panelFileInfo_dockSide"] == "1", "wrong left record replaced on save");
    AppState reopened; Load(reopened, saved);
    check(reopened.panelFileInfo.dockSide == DockSide::Right, "reopen remains right");
    repaired.settings.infoPages.visibleMask = 9;
    repaired.settings.infoPages.selected = 3;
    AppState pagesReopened; Load(pagesReopened, Save(repaired));
    check(pagesReopened.settings.infoPages.visibleMask == 9 && pagesReopened.settings.infoPages.selected == 3, "page visibility and selection survive real settings roundtrip");
    check(missing.settings.infoPages.visibleMask == 15 && missing.settings.infoPages.selected == 1, "legacy settings default to artwork and all available pages");
    check(Save(missing)["infoShowSimilarSounds"] == "1", "legacy settings keep the Similar Sounds shortcut visible");
    Values shortcutSettings = Save(repaired);
    shortcutSettings["infoShowSimilarSounds"] = "0";
    AppState hiddenShortcut; Load(hiddenShortcut, shortcutSettings);
    check(Save(hiddenShortcut)["infoShowSimilarSounds"] == "0", "hidden Similar Sounds shortcut survives real settings roundtrip");
    check(hiddenShortcut.settings.infoPages.visibleMask == 9 && hiddenShortcut.settings.infoPages.selected == 3, "hiding shortcut preserves page visibility and selection");
    shortcutSettings["infoShowSimilarSounds"] = "1";
    AppState restoredShortcut; Load(restoredShortcut, shortcutSettings);
    check(Save(restoredShortcut)["infoShowSimilarSounds"] == "1", "restored Similar Sounds shortcut survives real settings roundtrip");
    repaired.panelFileInfo.dockSide = DockSide::Left;
    AppState deliberatelyLeft; Load(deliberatelyLeft, Save(repaired));
    check(deliberatelyLeft.panelFileInfo.dockSide == DockSide::Left, "later intentional left dock survives reopen");
    old["fileInfoDockSide"] = "0";
    AppState legacyLeft; Load(legacyLeft, old);
    check(legacyLeft.panelFileInfo.dockSide == DockSide::Left, "consistent historical left choice preserved");
    old["fileInfoDockSide"] = "1"; old["panelFileInfo_floating"] = "1";
    AppState floating; Load(floating, old);
    check(floating.panelFileInfo.floating && floating.panelFileInfo.dockSide == DockSide::Left, "floating panel untouched");
    old["panelFileInfo_floating"] = "0";
    for (int side : {1, 2, 3}) {
        old["panelFileInfo_dockSide"] = std::to_string(side);
        AppState other; Load(other, old);
        check(static_cast<int>(other.panelFileInfo.dockSide) == side, "existing right/top/bottom preserved");
    }
    return failures ? 1 : 0;
}
'''
    cpp_path = out / 'info_panel_startup.cpp'
    cpp_path.write_text(cpp, encoding='utf-8')
    exe = out / ('info_panel_startup.exe' if os.name == 'nt' else 'info_panel_startup')
    if os.name == 'nt':
        vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
        setup = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
        command = f'call "{setup}" >nul && cl /nologo /EHsc /std:c++17 /utf-8 "{cpp_path}" /Fe:"{exe}"'
        env = dict(os.environ)
        env['PATH'] = str(vswhere.parent) + os.pathsep + env.get('PATH', '')
        compiled = subprocess.run('cmd.exe /d /s /c "' + command + '"', cwd=out, env=env)
    else:
        compiled = subprocess.run([shutil.which('c++') or 'c++', '-std=c++17', str(cpp_path), '-o', str(exe)], cwd=out)
    if compiled.returncode:
        return compiled.returncode
    return subprocess.run([str(exe)], cwd=out).returncode

if __name__ == '__main__':
    raise SystemExit(main())
