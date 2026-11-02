"""Compile the actual startup divider statements against real ImGui, headlessly."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    out = parser.parse_args().build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    function = source.split("static void RenderDatabaseSetupSeparator(", 1)[1]
    block = function.split("ImGui::PushStyleColor(ImGuiCol_Separator, separatorColor);", 1)[1].split("ImGui::PopStyleColor();", 1)[0]
    cpp = r'''
#include "imgui.h"
#include <cmath>
#include <iostream>
int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 1000);
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    int failures = 0;
    for (float width : {320.0f, 560.0f, 1200.0f}) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(30, 30));
        ImGui::SetNextWindowSize(ImVec2(width, 500));
        ImGui::Begin("test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        float available = ImGui::GetContentRegionAvail().x;
        float panelWidth = available < 560 ? available : 560;
        float contentWidth = panelWidth - 32;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available-panelWidth)/2 + 16);
        auto start = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        int first = drawList->VtxBuffer.Size;
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1,1,1,1));
''' + block + r'''
        ImGui::PopStyleColor();
        bool ok = drawList->VtxBuffer.Size > first;
        float right = start.x;
        for (int i=first; i<drawList->VtxBuffer.Size; ++i) {
            auto p = drawList->VtxBuffer[i].pos;
            ok &= p.x >= start.x && p.x <= start.x + contentWidth;
            if (p.x > right) right=p.x;
        }
        ok &= std::fabs(right - start.x - contentWidth) < 0.01f;
        ok &= std::fabs(ImGui::GetCursorScreenPos().y - start.y - ImGui::GetStyle().ItemSpacing.y) < 0.01f;
        std::cout << (ok ? "PASS " : "FAIL ") << width << " divider bounds and spacing\n";
        failures += !ok;
        ImGui::End(); ImGui::Render();
    }
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
'''
    path = out / "separator.cpp"
    path.write_text(cpp, encoding="utf-8")
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    install = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    setup = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
    imgui = ROOT / "thirdparty/imgui"
    sources = " ".join(f'"{imgui / name}"' for name in ["imgui.cpp", "imgui_draw.cpp", "imgui_widgets.cpp", "imgui_tables.cpp"])
    exe = out / "separator.exe"
    command = f'call "{setup}" >nul && cl /nologo /EHsc /std:c++17 /utf-8 /I"{imgui}" "{path}" {sources} /Fe:"{exe}"'
    env = dict(os.environ)
    env["PATH"] = str(vswhere.parent) + os.pathsep + env.get("PATH", "")
    result = subprocess.run('cmd.exe /d /s /c "' + command + '"', cwd=out, env=env)
    return result.returncode or subprocess.run([str(exe)], cwd=out).returncode

if __name__ == "__main__":
    raise SystemExit(main())
