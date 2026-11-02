from pathlib import Path
import hashlib
import json
import re
import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_product_and_release_versions_are_one_newer_stable_build() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    version_header = (ROOT / "src/Version.h").read_text(encoding="utf-8")
    installer = (ROOT / "installer/HarkenSearch_Setup.iss").read_text(encoding="utf-8")
    resources = (ROOT / "resources.rc").read_text(encoding="utf-8")

    project = re.search(r"project\(HarkenSearch VERSION ([0-9]+\.[0-9]+\.[0-9]+)", cmake)
    compiled = re.search(
        r'SOUNDMANAGER_VERSION_STRING\s+"([0-9]+\.[0-9]+\.[0-9]+)"',
        version_header,
    )
    version_parts = {
        name: re.search(
            rf"SOUNDMANAGER_VERSION_{name}\s+([0-9]+)",
            version_header,
        )
        for name in ("MAJOR", "MINOR", "PATCH")
    }
    setup = re.search(r'#define MyAppVersion "([0-9]+\.[0-9]+\.[0-9]+)"', installer)
    fileversion = re.search(r"FILEVERSION\s+([0-9]+),([0-9]+),([0-9]+),([0-9]+)", resources)
    productversion = re.search(
        r"PRODUCTVERSION\s+([0-9]+),([0-9]+),([0-9]+),([0-9]+)", resources
    )
    assert project and compiled and setup and fileversion and productversion
    assert all(version_parts.values())

    composed = ".".join(
        version_parts[name].group(1) for name in ("MAJOR", "MINOR", "PATCH")
    )
    rc_file = ".".join(fileversion.group(i) for i in range(1, 4))
    rc_product = ".".join(productversion.group(i) for i in range(1, 4))
    assert (
        project.group(1)
        == compiled.group(1)
        == composed
        == setup.group(1)
        == rc_file
        == rc_product
        == json.loads((ROOT / "installer/harken_release_history.json").read_text(encoding="utf-8"))["releases"][0]["version"]
    )
    assert fileversion.group(4) == "0" and productversion.group(4) == "0"


def test_product_target_contains_update_application_and_https_sources() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "src/update/UpdateApplicationService.cpp" in cmake
    assert "src/update/UpdateCheckHttpsTransport.cpp" in cmake
    assert "src/update/HarkenSearchUpdateComposition.cpp" in cmake
    assert "add_dependencies(HarkenSearch HarkenSearchBootstrap)" in cmake
    assert 'TARGET_FILE:HarkenSearchBootstrap' in cmake


def test_real_release_update_cli_is_opt_in_and_disabled_by_default() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    windows_main = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")

    assert 'option(ARIS_UPDATE_QA_CLI' in cmake
    assert '"Build the CLI-only real-release update QA entry" OFF)' in cmake
    assert 'ARIS_UPDATE_QA_CLI=1' in cmake
    assert 'defined(ARIS_UPDATE_QA_CLI)' in windows_main
    assert '--aris-update-qa-cli' in windows_main


def test_qa_cli_runs_after_health_and_integrity_without_runtime_consent() -> None:
    source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    health = source.index("CaptureHarkenSearchBootstrapHealthChallenge();")
    integrity = source.index("g_integrityCheckFailed = false;")
    early_cli = source.index("earlyCliResult = RunHarkenSearchDemoCli(g_integrityCheckFailed);")
    assert health < integrity < early_cli
    assert "EnsureRequiredConsent" not in source
    assert "defined(ARIS_UPDATE_QA_CLI)" in source


def test_qa_cli_keeps_exact_flag_and_fail_closed_integrity() -> None:
    source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    assert 'expectedFlag = "--aris-update-qa-cli"' in source
    assert "WideToUtf8(flag.value) != expectedFlag" in source
    assert "integrity_rejected" in source
    assert "return 3;" in source


def test_windows_product_wires_update_composition_and_health_ack() -> None:
    source = (ROOT / "src/WindowsMain.cpp").read_text(encoding="utf-8")
    assert '#include "update/HarkenSearchUpdateComposition.h"' in source
    assert "aris::update::AcknowledgeHarkenSearchBootstrapHealthReady();" in source
    assert "StartHarkenSearchAutomaticUpdate" in source


def _ledger_zh(entry_id: str) -> str:
    payload = json.loads((ROOT / "locale/harken.ui.strings.json").read_text(encoding="utf-8"))
    matches = [entry for entry in payload["entries"] if entry.get("id") == entry_id]
    assert len(matches) == 1, entry_id
    return matches[0]["zh"]


def test_ready_state_opens_the_same_native_update_window() -> None:
    source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    state = (ROOT / "src/AppState.h").read_text(encoding="utf-8")

    assert '#include "update/HarkenSearchUpdateComposition.h"' in source
    assert "GetHarkenSearchUpdateUiState" in source
    assert "RequestHarkenSearchUpdateCheck" in source
    assert "ConfirmHarkenSearchUpdateRestart" in source
    assert "readyChanged && !state.updateNoticeSeenThisSession" in source
    assert "state.showUpdateCenter = true" in source
    assert "BuildHarkenSearchUpdateReadyCopy" not in source
    assert 'u8"已更新到 v%s"' not in source
    assert "kUpdateNoticePopupId" not in source
    assert "updateNoticeSeenThisSession" in state


def test_update_notice_uses_shared_parser_and_verified_package_content() -> None:
    source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    composition = (ROOT / "src/update/HarkenSearchUpdateComposition.cpp").read_text(encoding="utf-8")
    model = (ROOT / "src/update/HarkenSearchUpdateNoticeModel.cpp").read_text(encoding="utf-8")
    assert "ParseHarkenSearchReleaseNotes" in source
    assert "SyncHarkenSearchCandidateReleaseNotes" in source
    assert "release_notes::ParseDocument" in model
    assert "UpdateDownloadManager::BuildStagingPaths(notesRequest)" in composition
    assert "ReadReadyReleaseNotes" in composition


def test_update_center_keeps_progress_errors_and_actions_with_full_notice() -> None:
    source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    state = (ROOT / "src/AppState.h").read_text(encoding="utf-8")
    ui = (ROOT / "src/update/HarkenSearchUpdateNoticeUi.cpp").read_text(encoding="utf-8")

    assert "showUpdateCenter" in state
    assert "showUpdateCenter = true" in source
    assert "##HarkenSearchUpdateCenter" in source
    assert 'u8"更新中心"' in source
    assert 'u8"检查更新"' in source
    assert 'u8"重试"' in source
    assert 'u8"立即重启更新"' in source
    assert 'u8"稍后"' in source
    assert "RenderHarkenSearchUpdateNoticeUi" in source
    assert "RenderReleaseContents" in ui
    assert "BuildHarkenSearchReleaseNotesHistory" in ui
    assert "Update_InlineBackToCheck" in ui
    for name in ("downloadedBytes", "totalBytes", "lastError"):
        assert "updateState." + name in source


def test_manual_check_cannot_replace_an_update_waiting_for_restart() -> None:
    source = (ROOT / "src/update/HarkenSearchUpdateComposition.cpp").read_text(
        encoding="utf-8"
    )
    assert "g_updateUiState.phase == HarkenSearchUpdateUiPhase::Ready" in source


def test_bootstrap_product_failure_prompts_reinstall_without_touching_user_data() -> None:
    source = (ROOT / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    explanation = (ROOT / "src/bootstrap/HarkenSearchBootstrapExitExplanation.cpp").read_text(
        encoding="utf-8"
    )
    assert "MessageBoxW" in source
    assert "ExplainBootstrapExit(result)" in source
    assert "if (explanation.showDialog)" in source
    assert 'explanation.requiresReinstall ? L"Harken Search 需要重新安装"' in source
    assert "explanation.requiresReinstall ? MB_ICONERROR" in source
    assert "重新安装" in explanation
    assert "数据库、设置和用户数据不会被删除" in explanation
    assert "launchedFromHandoff" in source


def test_bootstrap_handoff_gate_is_exact() -> None:
    source = (ROOT / "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp").read_text(
        encoding="utf-8"
    )
    assert re.search(
        r"const bool launchedFromHandoff\s*=.*?"
        r"handoff != nullptr && handoff != INVALID_HANDLE_VALUE &&\s*"
        r"GetFileType\(handoff\) == FILE_TYPE_PIPE;",
        source,
        re.DOTALL,
    )
    assert re.search(
        r"if \(mode == BootstrapNativeMode::Normal && result != 0 &&\s*"
        r"launchedFromHandoff && !progressPresented\)\s*\{.*?"
        r"const auto explanation = aris::bootstrap::ExplainBootstrapExit\(result\);"
        r".*?if \(explanation\.showDialog\).*?"
        r"explanation\.requiresReinstall \? L\"Harken Search 需要重新安装\" : "
        r"L\"Harken Search 更新未完成\";.*?MessageBoxW\(",
        source,
        re.DOTALL,
    )
def test_internal_bootstrap_runtime_audit_entries_are_exact() -> None:
    register = json.loads(
        (
            ROOT
            / "docs/plans/refactor/stage-6-platform-audited-exceptions.json"
        ).read_text(encoding="utf-8")
    )
    expected = {
        "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|bc7ccdeef83b4b32|1",
        "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|MessageBoxW|767ba0a566e946d8|1",
    }
    entries = {
        entry["fingerprint"]: entry
        for entry in register["findings"]
        if entry.get("fingerprint") in expected
    }
    assert set(entries) == expected
    for fingerprint, entry in entries.items():
        assert entry["path"] == "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp"
        source_lines = (ROOT / entry["path"]).read_text(encoding="utf-8").splitlines()
        assert 0 < entry["line"] <= len(source_lines)
        source_line = " ".join(source_lines[entry["line"] - 1].strip().split())
        assert hashlib.sha256(source_line.encode("utf-8")).hexdigest()[:16] == fingerprint.split("|")[3]
        assert entry["rule"] == "native-symbol"
        assert entry["owner_slice"] == "BOOTSTRAP-RUNTIME"
        assert entry["disposition"] == "platform-adapter"
        assert entry["evidence_document"] == (
            "docs/plans/refactor/stage-6-bootstrap-runtime-adapter-audit.md"
        )


def test_release_payload_contains_signed_bootstrap_package_entries() -> None:
    source = (ROOT / "tools/release_update.py").read_text(encoding="utf-8")
    assert 'PACKAGE_PLAN_NAME = "ArisBootstrap.package.json"' in source
    assert 'PACKAGE_SIGNATURE_NAME = "ArisBootstrap.package.json.sig"' in source
    assert "build_package_plan" in source


def test_bootstrap_package_request_matches_published_sidecars() -> None:
    authority = (ROOT / "src/platform/update_bootstrap_authority_win32.cpp").read_text(
        encoding="utf-8"
    )
    transport = (ROOT / "src/update/UpdateCheckHttpsTransport.cpp").read_text(
        encoding="utf-8"
    )
    for channel in ("", ".stable"):
        plan = f"ArisBootstrap.package{channel}.json"
        assert f'"{plan}"' in authority
        assert f'"{plan}"' in transport
        assert f'"{plan}.sig"' in transport
    assert 'const std::string signatureUrl = planUrl + ".sig";' in authority
    assert "transport.port->Fetch({planUrl" in authority
    assert "transport.port->Fetch({signatureUrl" in authority
    assert "HarkenSearchBootstrap.package" not in authority
    assert "HarkenSearchBootstrap.package" not in transport


def test_installer_preserves_the_provisioned_runtime_pair() -> None:
    installer = (ROOT / "installer/HarkenSearch_Setup.iss").read_text(encoding="utf-8")

    assert 'Source: "{#SourceDir}\\HarkenSearchBootstrap.exe.sig"' in installer
    assert 'Source: "{#SourceDir}\\harken-runtime-pair.json"' in installer


def test_product_composition_contains_real_bootstrap_handoff() -> None:
    composition = (ROOT / "src/update/HarkenSearchUpdateComposition.cpp").read_text(
        encoding="utf-8"
    )
    authority = (
        ROOT / "src/platform/update_bootstrap_authority_win32.cpp"
    ).read_text(encoding="utf-8")
    assert "Bootstrap launch broker is not yet provisioned" not in authority
    assert "RunWindowsBootstrapLaunchBroker" not in authority
    assert "CreateNamedPipeW" in authority
    assert "PROC_THREAD_ATTRIBUTE_HANDLE_LIST" in authority
    assert "CreateProcessW" in authority
    assert '"existing_inventory", nlohmann::json::array()' not in authority
    assert '{"channel_handle", 0U}' not in authority
    assert '"launch_broker"' in authority
    assert '"process_identity", IdentityJson(brokerIdentity)' in authority
    assert "recursive_directory_iterator" in authority
    assert "ExistingInventory(ExecutableDirectory())" in authority
    assert "ExistingInventory(*binding.binding)" not in authority
    assert "SetSecurityInfo" in authority
    assert "SetNamedSecurityInfoW" not in authority
    assert "PROTECTED_DACL_SECURITY_INFORMATION" in authority
    assert "binding.binding->channel() != ArchiveChannel(archive.channel)" in authority
    assert 'ArisBootstrap.package.stable.json' in authority
    assert (
        "Platform::UpdateBootstrapAuthority::CreateUpdateBootstrapAuthorityPort()"
        in composition
    )
    assert "*authority.port, config" in composition


def test_github_release_repository_has_one_compile_time_configuration() -> None:
    config = ROOT / "src/update/UpdateReleaseConfig.h"
    assert config.exists()
    config_source = config.read_text(encoding="utf-8")
    assert (
        'ARIS_UPDATE_GITHUB_REPOSITORY "xindong/harken-search"'
        in config_source
    )
    assert "ARIS_UPDATE_GITHUB_RELEASE_DOWNLOAD_PREFIX" in config_source
    assert "ARIS_UPDATE_MIRROR_BASE_URL" in config_source
    assert "ResolveUpdateDownloadUrl" in config_source


    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert (
        'set(ARIS_UPDATE_GITHUB_REPOSITORY "xindong/harken-search" CACHE STRING'
        in cmake
    )
    assert "GitHub repository owner/name used for Aris update discovery" in cmake
    assert 'MATCHES "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$"' in cmake
    assert "ARIS_UPDATE_GITHUB_REPOSITORY must be owner/name" in cmake

    assert (
        'set(ARIS_UPDATE_MIRROR_BASE_URL "https://harkensearch.xd.com/aris/" CACHE STRING'
        in cmake
    )
    assert "ARIS_UPDATE_MIRROR_BASE_URL must be an exact HTTP(S) base URL" in cmake
    assert 'ARIS_UPDATE_GITHUB_REPOSITORY=\\"${ARIS_UPDATE_GITHUB_REPOSITORY}\\"' in cmake

    source_files = [
        ROOT / "src/update/HarkenSearchUpdateComposition.cpp",
        ROOT / "src/update/UpdateCheckHttpsTransport.cpp",
        ROOT / "src/update/UpdateHttpTransport.cpp",
        ROOT / "src/update/UpdateStateStore.cpp",
    ]
    for source_file in source_files:
        source = source_file.read_text(encoding="utf-8")
        assert "github.com/xindong/aris" not in source


def test_public_release_rejects_non_public_update_repository_coordinate() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "ARIS_PUBLIC_UPDATE_GITHUB_REPOSITORY" in cmake
    assert "ARIS_PUBLIC_RELEASE requires ARIS_UPDATE_GITHUB_REPOSITORY" in cmake


def test_production_packaging_and_update_discovery_are_stable_only() -> None:
    workflow = (ROOT / ".github/workflows/package-update.yml").read_text(
        encoding="utf-8"
    )
    composition = (ROOT / "src/update/HarkenSearchUpdateComposition.cpp").read_text(
        encoding="utf-8"
    )

    assert "-DAIRS_RELEASE_CHANNEL=stable" in workflow
    assert "testing" not in workflow.lower()
    assert "config.check.manifestUrl = CompanyCloudStableManifestUrl()" in composition
    assert "releases/latest/download/manifest-harken-search-win-x64-stable.json" in (
        ROOT / "src/update/UpdateReleaseConfig.h"
    ).read_text(encoding="utf-8")
    assert "config.check.policy.selectedChannel = UpdateChannel::Stable" in composition
    assert "config.runtime.channel = UpdateChannel::Stable" in composition


def test_public_release_rejects_qa_cli_at_configure_time() -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "if(ARIS_PUBLIC_RELEASE AND ARIS_UPDATE_QA_CLI)" in cmake
    assert "ARIS_PUBLIC_RELEASE rejects ARIS_UPDATE_QA_CLI" in cmake


def test_update_mirror_is_transport_only_and_cannot_replace_signed_identity() -> None:
    config = (ROOT / "src/update/UpdateReleaseConfig.h").read_text(encoding="utf-8")
    manifest = (ROOT / "src/update/UpdateManifest.cpp").read_text(encoding="utf-8")
    state_store = (ROOT / "src/update/UpdateStateStore.cpp").read_text(encoding="utf-8")
    check_transport = (
        ROOT / "src/platform/update_check_https_transport_win32.cpp"
    ).read_text(encoding="utf-8")
    download_transport = (ROOT / "src/platform/update_http_transport.cpp").read_text(
        encoding="utf-8"
    )

    assert "ARIS_UPDATE_MIRROR_BASE_URL" in config
    assert "ARIS_UPDATE_GITHUB_RELEASE_DOWNLOAD_PREFIX" in config
    assert "if (trustedUrl.rfind(release, 0) != 0) return std::string(trustedUrl);" in config
    assert "ResolveUpdateDownloadUrl(std::string_view trustedUrl)" in config
    assert "ResolveUpdateDownloadUrl" in config
    assert "ARIS_UPDATE_MIRROR_BASE_URL" not in manifest
    assert "ARIS_UPDATE_MIRROR_BASE_URL" not in state_store
    assert "UpdateTransportCandidates(request.url)" not in check_transport
    assert "ResolveUpdateDownloadUrl(plan.url)" in download_transport
    assert "https://harkensearch.xd.com/aris/" in config
    assert 'ARIS_UPDATE_MIRROR_BASE_URL "https://harkensearch.xd.com/aris/"' in config
    assert 'set(ARIS_UPDATE_MIRROR_BASE_URL "https://harkensearch.xd.com/aris/" CACHE STRING' in (
        ROOT / "CMakeLists.txt"
    ).read_text(encoding="utf-8")
