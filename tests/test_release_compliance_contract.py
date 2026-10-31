from __future__ import annotations

from pathlib import Path
import hashlib
import importlib.util
import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
GENERIC_VENDOR = "Sound" + "miner"
GENERIC_VENDOR_STYLE = GENERIC_VENDOR + " Style"
GENERIC_VENDOR_PARITY = GENERIC_VENDOR + " parity"
PRIVATE_DLL = "sound" + "miner" + "ski.dll"
NUENDO_WDL_NAME = "NuendoSound" + "minerWdlClient"
PRIVATE_ALIAS = "github-" + "zhongziyu"
LOCAL_PATH_E = "E:" + "\\XDGit"
LOCAL_PATH_C = "C:" + "\\Users\\"
PRIVATE_DISASSEMBLER_MARKER = "Ghi" + "dra"
DICE_API = "api." + "dicebear.com"
DICE_AVATARS = "avatars." + "dicebear.com"


def load_checker_module():
    module_path = REPO_ROOT / "tools" / "check_release_compliance.py"
    spec = importlib.util.spec_from_file_location("release_compliance_checker", module_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_file(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def test_rejects_retired_research_test_names_and_renamed_protocol_copies(tmp_path):
    checker = load_checker_module()
    root = tmp_path / "repo"
    for name in ("test_nuendoski_proto.cpp", "test_nuendo_ski_probe.cpp", "test_steinberg_sdk_adapter.cpp"):
        write_file(root / "tests" / name, "// retired fixture\n")
    write_file(root / "tests/renamed_protocol.cpp", "void Encode" + GENERIC_VENDOR + "SpotRequest();\n")
    assert len(checker.find_private_research_file_issues(root)) == 3
    violations = checker.find_test_source_violations(root)
    assert any(path == Path("tests/renamed_protocol.cpp") for path, _, _ in violations)


def test_product_brand_check_includes_localization_but_keeps_audit_notes(tmp_path):
    checker = load_checker_module()
    root = tmp_path / "repo"
    write_file(root / "locale/strings.json", '"Advanced Spectrogram (iZotope RX Mode)"\n')
    write_file(root / "src/spectrum.cpp", "// RX-style heat map\n")
    write_file(root / "docs/audit.md", "Earlier iZotope wording was removed.\n")
    paths = {path.as_posix() for path, _, _ in checker.find_product_brand_violations(root)}
    assert paths == {"locale/strings.json", "src/spectrum.cpp"}


def test_renamed_research_tools_are_scanned_with_exact_policy_exception(tmp_path):
    checker = load_checker_module()
    root = tmp_path / "repo"
    marker = GENERIC_VENDOR + "Message"
    write_file(root / "tools/transport_fixture.py", marker)
    write_file(root / "tools/check_release_compliance.py", marker)
    write_file(root / "tools/nested/check_release_compliance.py", marker)
    paths = {path.as_posix() for path, _, _ in checker.find_tool_source_violations(root)}
    assert paths == {"tools/transport_fixture.py", "tools/nested/check_release_compliance.py"}


def test_release_scans_native_binary_strings_in_both_encodings(tmp_path):
    checker = load_checker_module()
    root = tmp_path / "Release"
    root.mkdir()
    (root / "HarkenSearch.exe").write_bytes(b"MZ\0" + "iZoToPe RX Mode".encode("utf-8"))
    (root / "renamed.dll").write_bytes(b"MZ\0" + (GENERIC_VENDOR + "Message").encode("utf-16-le"))
    paths = {path.as_posix() for path, _, _ in checker.find_release_provenance_violations(root)}
    assert paths == {"HarkenSearch.exe", "renamed.dll"}


def test_current_repository_has_no_retired_research_or_product_branding():
    checker = load_checker_module()
    assert checker.find_private_research_file_issues(REPO_ROOT) == []
    assert checker.find_product_brand_violations(REPO_ROOT) == []


def build_repo(tmp_path: Path) -> Path:
    repo_root = tmp_path / "repo"
    for directory in (
        "src",
        "tests",
        "docs",
        "docs/plans",
        "docs/process",
        "tools",
        "tuizi",
        "LICENSES",
    ):
        (repo_root / directory).mkdir(parents=True, exist_ok=True)
    write_file(repo_root / "README.md", "# temp repo\n")
    write_file(repo_root / "BUILD_WINDOWS.md", "# build\n")
    write_file(repo_root / "THIRD_PARTY_NOTICES.md", (REPO_ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8"))
    write_file(repo_root / "ASSET_PROVENANCE.md", "# assets\n")
    write_file(repo_root / "PRIVACY.md", "# privacy\n")
    write_file(repo_root / "AI_FEATURE_DISCLOSURE.md", "# ai\n")
    write_file(
        repo_root / "CMakeLists.txt",
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.16)",
                'option(AURO_ENABLE_AUBIO_BPM "Link GPL aubio for BPM detection (makes binary GPL)" OFF)',
                'option(AURO_BUNDLE_AVID_PTSL "Allow bundling Avid PTSLC_CPP.dll after manual license confirmation" OFF)',
                "thirdparty/imgui/",
                "nlohmann_json",
                "xxhash",
                "miniaudio",
                "stb_image",
                "thirdparty/sqlite",
                "thirdparty/whisper.cpp",
                "thirdparty/ixwebsocket",
                "thirdparty/rlottie",
                "thirdparty/webview2",
                "thirdparty/lucide/lucide.ttf",
                "assets/avatars",
                "installer/Languages/ChineseSimplified.isl",
                "reaper_extension",
                "tweetnacl",
                "thirdparty/ixwebsocket/third_party/cli11",
                "thirdparty/ixwebsocket/third_party/cpp-linenoise",
                "thirdparty/ixwebsocket/third_party/msgpack11",
                "AURO_ENABLE_AUBIO_BPM",
                "AURO_BUNDLE_AVID_PTSL",
                "",
            ]
        ),
    )
    write_file(repo_root / "THIRD_PARTY_COMPONENTS.json", (REPO_ROOT / "THIRD_PARTY_COMPONENTS.json").read_text(encoding="utf-8"))
    component_payload = __import__("json").loads((REPO_ROOT / "THIRD_PARTY_COMPONENTS.json").read_text(encoding="utf-8"))
    for component in component_payload["components"]:
        for relative in component["license_files"] + component["notice_files"]:
            target = repo_root / relative
            if not target.exists():
                write_file(target, "example\n")
    write_file(repo_root / "tuizi" / "vite.config.ts", "export default {}\n")
    write_file(
        repo_root / "src" / "Version.h",
        "\n".join(
            [
                "#define SOUNDMANAGER_VERSION_MAJOR 1",
                "#define SOUNDMANAGER_VERSION_MINOR 0",
                "#define SOUNDMANAGER_VERSION_PATCH 31",
                '#define SOUNDMANAGER_VERSION_STRING "1.0.31"',
                "",
            ]
        ),
    )
    write_file(repo_root / "CMakeLists.txt", (repo_root / "CMakeLists.txt").read_text(encoding="utf-8").replace(
        "cmake_minimum_required(VERSION 3.16)",
        "cmake_minimum_required(VERSION 3.16)\nproject(HarkenSearch VERSION 1.0.31 LANGUAGES C CXX)",
        1,
    ))
    write_file(
        repo_root / "installer" / "HarkenSearch_Setup.iss",
        '#ifndef MyAppVersion\n#define MyAppVersion "1.0.31"\n#endif\n',
    )
    write_file(
        repo_root / "resources.rc",
        "\n".join(
            [
                "1 VERSIONINFO",
                " FILEVERSION 1,0,31,0",
                " PRODUCTVERSION 1,0,31,0",
                "BEGIN",
                '    VALUE "FileVersion", "1.0.31"',
                '    VALUE "ProductVersion", "1.0.31"',
                "END",
                "",
            ]
        ),
    )
    write_file(repo_root / "LICENSES" / "README.md", "# licenses\n")
    checker = load_checker_module()
    for file_name in checker.REQUIRED_LICENSE_FILES:
        write_file(repo_root / "LICENSES" / file_name, "example\n")
        with (repo_root / "LICENSES" / "README.md").open("a", encoding="utf-8") as handle:
            handle.write(f"- `{file_name}`\n")
    return repo_root


def build_release_dir(tmp_path: Path) -> Path:
    release_dir = tmp_path / "Release"
    release_dir.mkdir(parents=True, exist_ok=True)
    for name in (
        "LICENSE",
        "THIRD_PARTY_COMPONENTS.json",
        "third-party-sbom.cdx.json",
        "ASSET_PROVENANCE.md",
        "PRIVACY.md",
        "AI_FEATURE_DISCLOSURE.md",
    ):
        write_file(release_dir / name, f"{name}\n")
    write_file(release_dir / "THIRD_PARTY_NOTICES.md", (REPO_ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8"))
    checker = load_checker_module()
    component_source = REPO_ROOT / "THIRD_PARTY_COMPONENTS.json"
    component_payload = __import__("json").loads(component_source.read_text(encoding="utf-8"))
    for component in component_payload["components"]:
        for relative in component["license_files"] + component["notice_files"]:
            if relative == "thirdparty/BINARY_PROVENANCE.md":
                continue
            target = release_dir / relative
            if target.name == "THIRD_PARTY_NOTICES.md":
                continue
            write_file(target, "example\n")
    write_file(release_dir / "THIRD_PARTY_COMPONENTS.json", __import__("json").dumps(component_payload))
    result = __import__("subprocess").run(
        ["python", str(REPO_ROOT / "tools" / "generate_third_party_sbom.py"), "--manifest", str(release_dir / "THIRD_PARTY_COMPONENTS.json"), "--output", str(release_dir / "third-party-sbom.cdx.json")],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    write_file(release_dir / "LICENSES" / "README.md", "# licenses\n")
    for file_name in checker.REQUIRED_LICENSE_FILES:
        write_file(release_dir / "LICENSES" / file_name, "example\n")
        with (release_dir / "LICENSES" / "README.md").open("a", encoding="utf-8") as handle:
            handle.write(f"- `{file_name}`\n")
    for name, payload in {
        "HarkenSearchBootstrap.exe": b"MZ-bootstrap-fixture",
        "WebView2Loader.dll": b"webview2-loader",
        "rlottie.dll": b"rlottie-core",
        "rlottie-image-loader.dll": b"rlottie-image-loader",
        "lucide.ttf": b"lucide",
        "reaper_harken.dll": b"reaper",
    }.items():
        (release_dir / name).write_bytes(payload)
    write_file(release_dir / "assets" / "avatars" / "placeholder.svg", "<svg></svg>\n")

    def digest(name: str) -> str:
        return hashlib.sha256((release_dir / name).read_bytes()).hexdigest()

    write_file(
        release_dir / "BINARY_PROVENANCE.md",
        "\n".join([
            "# BINARY_PROVENANCE",
            "",
            "| Artifact | Source URL | Version/commit | Built by us | Build command | License | SHA256 |",
            "|---|---|---|---|---|---|---|",
            f"| `WebView2Loader.dll` | x | x | x | x | x | `{digest('WebView2Loader.dll')}` |",
            f"| `rlottie.dll` | x | x | x | x | x | `{digest('rlottie.dll')}` |",
            f"| `rlottie-image-loader.dll` | x | x | x | x | x | `{digest('rlottie-image-loader.dll')}` |",
            "",
        ]),
    )
    return release_dir


def test_detects_private_analysis_markers_but_allows_generic_product_benchmarks(tmp_path):
    checker = load_checker_module()
    repo_root = build_repo(tmp_path)
    write_file(repo_root / "src" / "z_last.cpp", GENERIC_VENDOR + "\n")
    write_file(repo_root / "src" / "a_first.cpp", "prefix " + PRIVATE_DISASSEMBLER_MARKER.upper() + " suffix\n")

    violations = checker.find_violations(repo_root=repo_root)

    assert violations == [(Path("src/a_first.cpp"), 1, PRIVATE_DISASSEMBLER_MARKER)]


def test_scans_cmakelists_and_blocks_sensitive_paths_and_private_aliases(tmp_path):
    checker = load_checker_module()
    repo_root = build_repo(tmp_path)
    write_file(
        repo_root / "CMakeLists.txt",
        "\n".join([
            LOCAL_PATH_E + "\\repo",
            LOCAL_PATH_C + "User\\repo",
            "git@" + PRIVATE_ALIAS + ":xindong/aris.git",
        ]),
    )

    violations = checker.find_violations(repo_root=repo_root)
    markers = {(path.as_posix(), line, marker) for path, line, marker in violations}

    assert ("CMakeLists.txt", 1, LOCAL_PATH_E) in markers
    assert ("CMakeLists.txt", 2, LOCAL_PATH_C) in markers
    assert ("CMakeLists.txt", 3, PRIVATE_ALIAS) in markers


def test_only_explicit_audit_policy_paths_are_skipped(tmp_path):
    checker = load_checker_module()
    repo_root = build_repo(tmp_path)
    write_file(repo_root / "docs" / "plans" / "draft.md", PRIVATE_DISASSEMBLER_MARKER + "\n")
    write_file(
        repo_root / "docs" / "plans" / "2026-07-09-release-compliance-remediation-plan.md",
        PRIVATE_DISASSEMBLER_MARKER + "\n",
    )

    violations = checker.find_violations(repo_root=repo_root)

    assert violations == [(Path("docs/plans/draft.md"), 1, PRIVATE_DISASSEMBLER_MARKER)]


def test_private_research_references_are_not_allowlisted_in_build_or_main(tmp_path):
    checker = load_checker_module()
    repo_root = build_repo(tmp_path)
    write_file(repo_root / "CMakeLists.txt", "src/" + NUENDO_WDL_NAME + ".cpp\n")
    write_file(
        repo_root / "src" / "main.cpp",
        "\n".join([
            '#include "' + NUENDO_WDL_NAME + '.h"',
            "const auto probe = Aura::NuendoSound" + "minerWdl::ProbeWdlSharedMemory();",
        ]),
    )

    violations = checker.find_violations(repo_root=repo_root)

    paths = {path.as_posix() for path, _, _ in violations}
    assert "CMakeLists.txt" in paths
    assert "src/main.cpp" in paths


def test_clean_repo_passes(tmp_path, capsys):
    checker = load_checker_module()
    repo_root = build_repo(tmp_path)
    write_file(repo_root / "src" / "clean.cpp", "all clear\n")

    exit_code = checker.run(repo_root=repo_root)
    output = capsys.readouterr().out

    assert exit_code == 0
    assert "PASS" in output
    assert "license-bundle" in output.lower()


def test_product_version_drift_fails_source_compliance(tmp_path, capsys):
    checker = load_checker_module()
    repo_root = build_repo(tmp_path)
    write_file(
        repo_root / "installer" / "HarkenSearch_Setup.iss",
        '#ifndef MyAppVersion\n#define MyAppVersion "1.0.30"\n#endif\n',
    )

    exit_code = checker.run(repo_root=repo_root)
    output = capsys.readouterr().out

    assert exit_code == 1
    assert "FAIL" in output
    assert "installer/HarkenSearch_Setup.iss" in output.replace("\\", "/")
    assert "1.0.30" in output


def test_main_like_runner_returns_one_and_fail_output_on_violations(tmp_path, capsys):
    checker = load_checker_module()
    repo_root = build_repo(tmp_path)
    write_file(
        repo_root / "src" / "bridge.cpp",
        "Reverse-Engineered" + " From " + PRIVATE_DLL + "\n",
    )

    exit_code = checker.run(repo_root=repo_root)
    output = capsys.readouterr().out

    assert exit_code == 1
    assert "FAIL" in output
    assert "src/bridge.cpp" in output.replace("\\", "/")


def test_checker_source_still_exposes_marker_vocabulary_and_release_mode():
    checker_source = (REPO_ROOT / "tools" / "check_release_compliance.py").read_text(
        encoding="utf-8"
    )

    assert "PTSLC_CPP.dll" in checker_source
    assert PRIVATE_DLL in checker_source
    assert "--release-dir" in checker_source
    assert DICE_API in checker_source
    assert DICE_AVATARS in checker_source


def test_release_contract_removes_private_nuendo_surface_instead_of_quarantining_it():
    cmake_source = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    build_windows = (REPO_ROOT / "BUILD_WINDOWS.md").read_text(encoding="utf-8")
    main_source = (REPO_ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

    assert "option(ARIS_PUBLIC_RELEASE" in cmake_source
    assert "ARIS_PRIVATE_NUENDO_SOURCES" not in cmake_source
    assert "ARIS_ENABLE_PRIVATE_NUENDO_RESEARCH" not in cmake_source
    assert "AuroNuendoSkiBridge" not in cmake_source
    assert "https://github.com/xindong/harken-search.git" in build_windows
    assert "-DARIS_PUBLIC_RELEASE=ON -DAURO_BUNDLE_AVID_PTSL=OFF" in build_windows
    assert "build_public/Release" in build_windows
    assert "ARIS_ENABLE_PRIVATE_NUENDO_RESEARCH" not in build_windows
    assert NUENDO_WDL_NAME not in main_source
    assert "TrySendNuendoSkiDirectSpotJob" not in main_source


def test_source_scan_includes_binary_provenance_and_build_doc_utf8_sections():
    checker_source = (REPO_ROOT / "tools" / "check_release_compliance.py").read_text(
        encoding="utf-8"
    )
    build_windows = (REPO_ROOT / "BUILD_WINDOWS.md").read_text(encoding="utf-8")

    assert "thirdparty/BINARY_PROVENANCE.md" in checker_source
    for token in [
        "Harken Search Windows 打包教程",
        "环境要求",
        "获取源码",
        "公开发布安全构建",
        "可选 SDK 说明",
        "不要只拷一个 exe。",
    ]:
        assert token in build_windows


def test_third_party_notice_and_license_bundle_are_in_sync():
    notices_source = (REPO_ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
    license_readme = (REPO_ROOT / "LICENSES" / "README.md").read_text(encoding="utf-8")
    license_dir = REPO_ROOT / "LICENSES"

    for token in [
        "DiceBear",
        "Lorelei Neutral",
        "Lucide",
        "WebView2",
        "whisper.cpp",
        "IXWebSocket",
        "rlottie",
    ]:
        assert token in notices_source

    expected_license_files = [
        "ImGui-MIT.txt",
        "IXWebSocket-BSD-3-Clause.txt",
        "whisper.cpp-MIT.txt",
        "rlottie-Licensing.txt",
        "rlottie-MIT.txt",
        "WebView2-LICENSE.txt",
        "WebView2-NOTICE.txt",
        "Lucide-LICENSE.txt",
        "DiceBear-MIT.txt",
        "CC0-1.0.txt",
        "Klangfreund-LUFSMeter-MIT.txt",
        "REAPER-SDK-zlib.txt",
        "TweetNaCl-PUBLIC-DOMAIN.txt",
        "CLI11-BSD-3-Clause.txt",
        "cpp-linenoise-BSD-2-Clause.txt",
        "msgpack11-MIT.txt",
    ]
    for file_name in expected_license_files:
        assert (license_dir / file_name).exists()
        assert file_name in license_readme

    bundled_files = [path for path in license_dir.iterdir() if path.is_file() and path.name != "README.md"]
    assert bundled_files


def test_installer_stages_release_metadata_and_license_bundle():
    installer = (REPO_ROOT / "installer" / "HarkenSearch_Setup.iss").read_text(encoding="utf-8")

    assert 'Source: "{#SourceDir}\\HarkenSearchBootstrap.exe"' in installer
    assert 'DestDir: "{app}"' in installer

    for filename in (
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
        "ASSET_PROVENANCE.md",
        "BINARY_PROVENANCE.md",
        "PRIVACY.md",
        "AI_FEATURE_DISCLOSURE.md",
    ):
        assert f'{{#SourceDir}}\\{filename}' in installer

    assert 'Source: "{#SourceDir}\\LICENSES\\*"' in installer
    assert 'DestDir: "{app}\\LICENSES"' in installer
    assert "recursesubdirs" in installer


def test_private_research_globs_are_scanned_and_rejected(tmp_path):
    checker = load_checker_module()
    repo_root = build_repo(tmp_path)
    write_file(
        repo_root / "src" / "NuendoSkiProto.h",
        "reverse-engineered" + " from " + PRIVATE_DLL + "\n",
    )
    write_file(repo_root / "src" / (NUENDO_WDL_NAME + ".cpp"), GENERIC_VENDOR_PARITY + "\n")
    write_file(repo_root / "tests" / "test_nuendo_ski_tools_contract.py", PRIVATE_DISASSEMBLER_MARKER + "\n")

    violations = checker.find_violations(repo_root=repo_root)
    test_violations = checker.find_test_source_violations(repo_root=repo_root)

    paths = {path.as_posix() for path, _, _ in violations + test_violations}
    assert "src/NuendoSkiProto.h" in paths
    assert "src/" + NUENDO_WDL_NAME + ".cpp" in paths
    assert "tests/test_nuendo_ski_tools_contract.py" in paths


def test_release_bundle_mode_checks_metadata_exclusions_and_hashes(tmp_path, capsys):
    checker = load_checker_module()
    release_dir = build_release_dir(tmp_path)

    exit_code = checker.run_release_dir(release_dir)
    output = capsys.readouterr().out

    assert exit_code == 0
    assert "PASS" in output
    assert "hashes" in output.lower()


def test_release_bundle_mode_rejects_missing_metadata_forbidden_artifacts_and_hash_mismatch(tmp_path):
    checker = load_checker_module()
    release_dir = build_release_dir(tmp_path)
    (release_dir / "PTSLC_CPP.dll").write_bytes(b"forbidden")
    (release_dir / "BINARY_PROVENANCE.md").write_text(
        (release_dir / "BINARY_PROVENANCE.md").read_text(encoding="utf-8").replace(
            "`" + hashlib.sha256((release_dir / "rlottie.dll").read_bytes()).hexdigest() + "`",
            "`deadbeef`",
            1,
        ),
        encoding="utf-8",
    )
    (release_dir / "AI_FEATURE_DISCLOSURE.md").unlink()

    issues = checker.validate_release_dir(release_dir)

    assert any("PTSLC_CPP.dll" in issue for issue in issues)
    assert any("AI_FEATURE_DISCLOSURE.md" in issue for issue in issues)
    assert any("rlottie.dll" in issue and "SHA-256 mismatch" in issue for issue in issues)


def test_release_bundle_mode_rejects_static_library_artifacts(tmp_path):
    checker = load_checker_module()
    release_dir = build_release_dir(tmp_path)
    (release_dir / "ArisBootstrapCore.lib").write_bytes(b"stale-lib")

    issues = checker.validate_release_dir(release_dir)

    assert any("ArisBootstrapCore.lib" in issue for issue in issues)


def test_removed_web_experiment_and_dicebear_runtime_guards_exist():
    checker = load_checker_module()
    asset_doc = (REPO_ROOT / "ASSET_PROVENANCE.md").read_text(encoding="utf-8")

    assert not (REPO_ROOT / "tuizi").exists()
    assert checker.find_runtime_network_violations(REPO_ROOT) == []
    assert "must read local SVGs only" in asset_doc
    assert "must not call the public DiceBear API during startup" in asset_doc


def test_theme_settings_names_are_header_visible_constexpr_source():
    import json
    import sys

    theme_header = (REPO_ROOT / "src" / "Theme.h").read_text(encoding="utf-8")
    theme_cpp = (REPO_ROOT / "src" / "Theme.cpp").read_text(encoding="utf-8")
    localization_cpp = (REPO_ROOT / "src" / "Localization.cpp").read_text(encoding="utf-8")
    generator = (REPO_ROOT / "tools" / "generate_localization.py").read_text(encoding="utf-8")
    ledger_path = REPO_ROOT / "locale" / "harken.ui.strings.json"
    ledger = json.loads(ledger_path.read_text(encoding="utf-8"))
    theme_entries = [entry for entry in ledger["entries"] if entry.get("type") == "theme"]
    expected_ids = [
        "Set_ThemeDark",
        "Set_ThemeOcean",
        "Set_ThemeRoseCedar",
        "Set_ThemeCustom",
        "Set_ThemeLapisBlue",
        "Set_ThemeJadeMist",
    ]

    assert [entry["id"] for entry in theme_entries] == expected_ids
    assert all(entry.get("zh") and entry.get("en") for entry in theme_entries)
    assert "static inline constexpr const char* kSettingsThemeDisplayNamesZh[]" not in theme_header
    assert "static inline constexpr const char* kSettingsThemeDisplayNamesEn[]" not in theme_header
    assert "kArisSettingsThemeDisplayNamesZh" in theme_header
    assert "kArisSettingsThemeDisplayNamesEn" in theme_header
    assert "static constexpr const char* getSettingsThemeDisplayNameZh(int themeSlotIndex)" in theme_header
    assert "static constexpr const char* getSettingsThemeDisplayNameEn(int themeSlotIndex)" in theme_header
    assert "const char* ThemeColors::getSettingsThemeDisplayNameZh(int themeSlotIndex)" not in theme_cpp
    assert "const char* ThemeColors::getSettingsThemeDisplayNameEn(int themeSlotIndex)" not in theme_cpp
    assert "ThemeColors::getSettingsThemeDisplayNameZh(0)" not in localization_cpp
    assert "ThemeColors::getSettingsThemeDisplayNameEn(0)" not in localization_cpp
    assert "inline constexpr const char* kArisSettingsThemeDisplayNamesZh[]" in generator
    assert "inline constexpr const char* kArisSettingsThemeDisplayNamesEn[]" in generator
    assert 'u8"经典深色"' not in theme_header
    assert '"Classic Dark"' not in theme_header
    assert "static inline constexpr const char* kSettingsThemeDisplayNamesZh[]" not in localization_cpp

    sys.path.insert(0, str(REPO_ROOT / "tools"))
    import generate_localization

    generated = generate_localization.render_generated_header(ledger["entries"])
    for entry in theme_entries:
        assert generate_localization.cpp_u8(entry["zh"]) in generated
        assert generate_localization.cpp_u8(entry["en"]) in generated


def test_release_checker_ci_strings_exist():
    checker_source = (REPO_ROOT / "tools" / "check_release_compliance.py").read_text(
        encoding="utf-8"
    )
    assert "PUBLIC RELEASE COMPLIANCE CHECK PASSED" in checker_source
    assert "PUBLIC RELEASE COMPLIANCE CHECK FAILED" in checker_source
    assert "RELEASE BUNDLE COMPLIANCE CHECK PASSED" in checker_source
    assert "RELEASE BUNDLE COMPLIANCE CHECK FAILED" in checker_source


@pytest.mark.skipif(not (REPO_ROOT / "docs/process/ARIS_RELEASE_SCOPE.md").is_file(), reason="Internal release checklist is retained in the private repository")
def test_internal_release_scope_checklist():
    scope_doc = (REPO_ROOT / "docs/process/ARIS_RELEASE_SCOPE.md").read_text(encoding="utf-8")
    assert "Release candidate checklist" in scope_doc
    assert "AURO_BUNDLE_AVID_PTSL=OFF" in scope_doc
    assert "py tools/check_release_compliance.py --release-dir build_public/Release" in scope_doc
    assert "no private host-integration research artifact" in scope_doc
