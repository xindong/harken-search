"""Public-repo publication manifest contract for RQ-20260910-08."""

from __future__ import annotations

import re
import unittest
from fnmatch import fnmatch
from pathlib import Path

from tools import public_repo_manifest as manifest

ROOT = Path(__file__).resolve().parents[1]

CMAKE_SOURCE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".mm", ".m", ".rc", ".def"}
CMAKE_ADD_EXECUTABLE_KEYWORDS = {"WIN32", "MACOSX_BUNDLE", "EXCLUDE_FROM_ALL"}
CUEVI_INTERNAL_TEST_SOURCES = (
    "tests/test_cuevi_feedback_client.cpp",
    "tests/test_cuevi_feedback_live_client.cpp",
    "tests/test_cuevi_feedback_completion_workflow.cpp",
)
ROOT_LEVEL_PUBLIC_TEST_SOURCES = (
    "test_time_stretch.cpp",
    "test_channel_routing_settings.cpp",
)


def _normalize_cmake_path(arg: str) -> str:
    value = arg.strip().strip('"').replace("\\", "/")
    for prefix in (
        "${CMAKE_SOURCE_DIR}/",
        "${CMAKE_CURRENT_SOURCE_DIR}/",
        "${CMAKE_SOURCE_DIR}",
        "${CMAKE_CURRENT_SOURCE_DIR}",
    ):
        if value.startswith(prefix):
            value = value[len(prefix):]
            break
    if value.startswith("./"):
        value = value[2:]
    return value.lstrip("/")


def _tokenize_cmake_args(body: str) -> list[str]:
    tokens: list[str] = []
    buf: list[str] = []
    in_quote = False
    i = 0
    while i < len(body):
        ch = body[i]
        if in_quote:
            buf.append(ch)
            if ch == '"':
                in_quote = False
                tokens.append("".join(buf))
                buf = []
            i += 1
            continue
        if ch == '"':
            if buf:
                tokens.append("".join(buf))
                buf = []
            in_quote = True
            buf.append(ch)
            i += 1
            continue
        if ch.isspace():
            if buf:
                tokens.append("".join(buf))
                buf = []
            i += 1
            continue
        buf.append(ch)
        i += 1
    if buf:
        tokens.append("".join(buf))
    return tokens


def _iter_cmake_commands(text: str):
    i = 0
    n = len(text)
    while i < n:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        if text[i] == "#":
            while i < n and text[i] not in "\n\r":
                i += 1
            continue
        if not (text[i].isalpha() or text[i] == "_"):
            i += 1
            continue
        start = i
        i += 1
        while i < n and (text[i].isalnum() or text[i] == "_"):
            i += 1
        name = text[start:i]
        while i < n and text[i].isspace():
            i += 1
        if i >= n or text[i] != "(":
            continue
        i += 1
        depth = 1
        body: list[str] = []
        in_quote = False
        while i < n and depth:
            ch = text[i]
            if in_quote:
                body.append(ch)
                if ch == '"':
                    in_quote = False
                i += 1
                continue
            if ch == '"':
                in_quote = True
                body.append(ch)
                i += 1
                continue
            if ch == "#":
                while i < n and text[i] not in "\n\r":
                    i += 1
                continue
            if ch == "(":
                depth += 1
                body.append(ch)
            elif ch == ")":
                depth -= 1
                if depth:
                    body.append(ch)
            else:
                body.append(ch)
            i += 1
        yield name.lower(), "".join(body)


def _exists_guard_from_if_args(args: list[str]) -> str | None:
    if len(args) >= 2 and args[0].upper() == "EXISTS":
        path = _normalize_cmake_path(args[1])
        if path and "${" not in path and "$<" not in path:
            return path
    return None


def parse_cmake_add_executables(text: str) -> list[dict[str, object]]:
    stack: list[str | None] = []
    found: list[dict[str, object]] = []
    for name, body in _iter_cmake_commands(text):
        args = _tokenize_cmake_args(body)
        if name == "if":
            stack.append(_exists_guard_from_if_args(args))
        elif name == "elseif":
            if stack:
                stack.pop()
            stack.append(_exists_guard_from_if_args(args))
        elif name == "else":
            if stack:
                stack[-1] = None
        elif name == "endif":
            if stack:
                stack.pop()
        elif name == "add_executable":
            if not args:
                continue
            idx = 1
            while idx < len(args) and args[idx].upper() in CMAKE_ADD_EXECUTABLE_KEYWORDS:
                idx += 1
            sources: list[str] = []
            for arg in args[idx:]:
                if arg.upper() in CMAKE_ADD_EXECUTABLE_KEYWORDS:
                    continue
                if "${" in arg or "$<" in arg:
                    continue
                path = _normalize_cmake_path(arg)
                if not path:
                    continue
                suffix = Path(path).suffix.lower()
                if suffix in CMAKE_SOURCE_EXTS or "/" in path:
                    sources.append(path)
            found.append(
                {
                    "target": args[0],
                    "sources": sources,
                    "exists_guards": [item for item in stack if item],
                }
            )
    return found


def iter_repo_cmake_add_executables(root: Path) -> list[tuple[str, dict[str, object]]]:
    files = [root / "CMakeLists.txt"]
    cmake_dir = root / "cmake"
    if cmake_dir.is_dir():
        files.extend(sorted(cmake_dir.glob("*.cmake")))
    results: list[tuple[str, dict[str, object]]] = []
    for path in files:
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        for item in parse_cmake_add_executables(path.read_text(encoding="utf-8")):
            results.append((rel, item))
    return results


def required_add_executable_sources(
    executables: list[dict[str, object]],
    existing_files: set[str],
) -> list[str]:
    required: list[str] = []
    for item in executables:
        guards = list(item["exists_guards"])
        if any(guard not in existing_files for guard in guards):
            continue
        required.extend(list(item["sources"]))
    return required


def _cmake_var_ref(arg: str) -> str | None:
    if arg.startswith("${") and arg.endswith("}") and arg.count("${") == 1:
        return arg[2:-1]
    return None


def _is_source_like(path: str) -> bool:
    suffix = Path(path).suffix.lower()
    return suffix in CMAKE_SOURCE_EXTS or "/" in path


def _expand_known_dir_vars(path: str, dirs: dict[str, str]) -> str:
    changed = True
    while changed and "${" in path:
        changed = False
        for name, value in dirs.items():
            token = "${" + name + "}"
            if token in path and value and "${" not in value:
                path = path.replace(token, value)
                changed = True
    return path


def _record_source_assignment(assignments: dict[str, list[str]], dirs: dict[str, str], name: str, values: list[str], append: bool) -> None:
    sources: list[str] = []
    for value in values:
        expanded = _expand_known_dir_vars(value, dirs)
        if _is_source_like(expanded) and "${" not in expanded and "$<" not in expanded:
            sources.append(expanded)
        elif expanded and "${" not in expanded and "/" not in expanded and "." not in expanded:
            dirs[name] = expanded
    if name.endswith("_DIR") and len(values) == 1:
        dirs[name] = _expand_known_dir_vars(values[0], dirs)
    if append and name in assignments:
        assignments[name].extend(sources)
    elif sources or name.endswith(("_SOURCES", "_SOURCE", "_FILES")):
        assignments[name] = sources


def _repo_source_path(path: str, directory: str) -> str:
    if (
        not directory
        or directory == "cmake"
        or path.startswith("/")
        or path.startswith(directory + "/")
        or path.startswith(("src/", "tests/", "thirdparty/", "resources/"))
    ):
        return path
    return f"{directory}/{path}"


def parse_cmake_closure(text: str, directory: str = "") -> dict[str, object]:
    stack: list[str | None] = []
    assignments: dict[str, list[str]] = {}
    dirs: dict[str, str] = {}
    targets: list[dict[str, object]] = []
    subdirectories: list[dict[str, object]] = []
    includes: list[dict[str, object]] = []
    prefix = f"{directory.strip('/')}/" if directory.strip("/") else ""
    for name, body in _iter_cmake_commands(text):
        args = _tokenize_cmake_args(body)
        if name == "if":
            stack.append(_exists_guard_from_if_args(args))
        elif name == "elseif":
            if stack:
                stack.pop()
            stack.append(_exists_guard_from_if_args(args))
        elif name == "else":
            if stack:
                stack[-1] = None
        elif name == "endif":
            if stack:
                stack.pop()
            continue
        guards = [item for item in stack if item]
        if name == "set" and args:
            _record_source_assignment(assignments, dirs, args[0], [_normalize_cmake_path(arg) for arg in args[1:]], False)
        elif name == "list" and len(args) >= 3 and args[0].upper() == "APPEND":
            _record_source_assignment(assignments, dirs, args[1], [_normalize_cmake_path(arg) for arg in args[2:]], True)
        elif name in {"add_executable", "add_library"} and args:
            idx = 1
            keywords = CMAKE_ADD_EXECUTABLE_KEYWORDS | {"STATIC", "SHARED", "MODULE", "OBJECT", "INTERFACE"}
            while idx < len(args) and args[idx].upper() in keywords:
                idx += 1
            sources: list[str] = []
            for arg in args[idx:]:
                if arg.upper() in keywords:
                    continue
                variable = _cmake_var_ref(arg)
                if variable is not None:
                    sources.extend(_repo_source_path(item, directory) for item in assignments.get(variable, []))
                    continue
                if "${" in arg or "$<" in arg:
                    continue
                path = _normalize_cmake_path(arg)
                if path and _is_source_like(path):
                    sources.append(_repo_source_path(path, directory))
            targets.append({"target": args[0], "sources": sources, "exists_guards": guards})
        elif name == "add_subdirectory" and args:
            subdir = _normalize_cmake_path(args[0])
            if subdir and "${" not in subdir:
                subdirectories.append({"path": f"{prefix}{subdir}", "exists_guards": guards})
        elif name == "include" and args and args[0].upper() != "OPTIONAL":
            included = _normalize_cmake_path(args[0])
            if included and "${" not in included and "$<" not in included:
                includes.append({"path": f"{prefix}{included}" if prefix and not included.startswith(prefix) else included, "exists_guards": guards})
    return {"assignments": assignments, "targets": targets, "subdirectories": subdirectories, "includes": includes}


def required_cmake_closure_sources(
    files: dict[str, str],
    existing_files: set[str],
    entry: str = "CMakeLists.txt",
) -> list[str]:
    required: list[str] = []
    seen: set[str] = set()

    def walk(rel: str, directory: str) -> None:
        if rel in seen or rel not in files:
            return
        seen.add(rel)
        parsed = parse_cmake_closure(files[rel], directory)
        for item in list(parsed["targets"]):
            guards = list(item["exists_guards"])
            if any(guard not in existing_files for guard in guards):
                continue
            required.extend(list(item["sources"]))
        for item in list(parsed["subdirectories"]):
            guards = list(item["exists_guards"])
            if any(guard not in existing_files for guard in guards):
                continue
            subdir = str(item["path"])
            walk(f"{subdir}/CMakeLists.txt", subdir)
        for item in list(parsed["includes"]):
            guards = list(item["exists_guards"])
            if any(guard not in existing_files for guard in guards):
                continue
            included = str(item["path"])
            walk(included, str(Path(included).parent.as_posix()))

    entry_dir = str(Path(entry).parent.as_posix())
    walk(entry, "" if entry_dir == "." else entry_dir)
    return required


REQUIRED_INTERNAL_PREFIXES = (
    "docs/superpowers/",
    "docs/plans/",
    "docs/product/",
    "docs/context/",
    "docs/governance/",
    "docs/process/",
    "docs/templates/",
    "docs/architecture/",
    ".openclaw/",
    ".agents/",
    ".cuevi/",
    ".vscode/",
    "design-system/",
    "tests/website/",
)

# Audit FAIL list (RQ-20260910-08): these internals must stay in the exclusion set.
AUDIT_INTERNAL_GLOBS = (
    "tools/cuevi_*",
    "tools/daily_self_review*",
    "tools/*daily_self_review*",
    "tools/qa_provenance_manifest.py",
    "scripts/slack_notify*",
    "scripts/tests/test_slack_notify.py",
    "tests/test_cuevi_*",
    "tests/website/*",
)

# 新增排除必须写进本表并附理由，禁止只改 tools/public_repo_manifest.EXCLUDED_GLOBS。
EXPLICIT_SAME_FAMILY_EXCLUSION_GLOBS: tuple[tuple[str, str], ...] = (
    ("tests/test_local_agent_docs.py", "Development-method contracts depend on repository-local documents outside the product export."),
    ("tools/aris_design_review_server.py", "Private design review needs unpublished design data."),
    ("tools/start_aris_design_review.ps1", "Launcher for the private design review server."),
    ("tests/test_aris_design_review_server.py", "Tests require the private design review data."),
    ("tests/test_aris_skill_runtime.py", "Internal Aris skill-runtime isolation test; not part of the public product tree"),
    ("tests/test_aris_speedup_tools.py", "Internal Aris speedup tooling test; not part of the public product tree"),
    ("tests/test_stage3_style_gate.py", "Internal Stage 3 style gate test; not part of the public product tree"),
    ("tools/aris_card_timer.py", "Internal Aris card timer; not part of the public product tree"),
    ("tools/aris_skill_runtime.py", "Internal Aris skill runtime; not part of the public product tree"),
    ("tools/aris_startup_pack.py", "Internal Aris startup pack; not part of the public product tree"),
    ("tools/qa_dock_gestures.py", "Internal QA dock gesture probe; not part of the public product tree"),
    ("tools/qa_retired_speed_runtime.py", "Retired internal QA speed runtime probe; not part of the public product tree"),
    ("tools/qa_send_target_menu.py", "隔离菜单验收探针依赖不公开的内部驱动，不进公开仓"),
    ("tools/database_switch_acceptance.py", "数据库切换验收依赖内部驱动，保留内部仓库复验"),
    ("tools/database_switch_busy_acceptance.py", "数据库忙碌验收依赖内部驱动，保留内部仓库复验"),
    ("tools/verify_cuevi_feedback_e2e.py", "Cuevi 反馈 e2e 属内部流程，不进公开仓"),
    ("tests/test_daily_self_review*", "每日自审测试同族"),
    ("tests/test_qa_provenance_manifest.py", "QA provenance 清单测试同族"),
    ("tests/test_start_cuevi_airs_bat.py", "Cuevi 启动脚本测试同族"),
    ("tests/test_aris_intake.py", "内部 intake 测试同族"),
    ("tests/test_aris_merge_closer.py", "合并闭环内部工具测试同族"),
    ("tests/test_aris_pr_create.py", "PR 创建门禁测试同族"),
    ("tests/test_auro_gate.py", "Auro 门禁测试同族"),
    ("tests/test_auro_process_gate.py", "Auro 流程门禁测试同族"),
    ("tests/test_auro_workflow_cuevi_contract.py", "Cuevi 工作流合同测试同族"),
    ("tests/test_qa_exe_packaging_contract.py", "QA EXE 打包合同测试同族"),
    ("tools/aris_bug_intake.py", "内部缺陷入口，不进公开仓"),
    ("tools/aris_merge_closer.py", "内部合并闭环工具"),
    ("tools/aris_pr_create.py", "内部 PR 创建门禁"),
    ("tools/aris_requirement_intake.py", "内部需求入口"),
    ("tools/auro_gate.py", "Auro 门禁工具"),
    ("tools/auro_process_gate.py", "Auro 流程门禁工具"),
    ("tools/auro_requirement_intake.py", "Auro 需求入口"),
    ("tools/aris_gate.py", "Aris 门禁工具"),
    ("tools/aris_process_gate.py", "Aris 流程门禁工具"),
)

FACADE_DOCS = (
    "README.md",
    "CONTRIBUTING.md",
    "PRIVACY.md",
    "SECURITY.md",
    "THIRD_PARTY_NOTICES.md",
    "AI_FEATURE_DISCLOSURE.md",
    "ASSET_PROVENANCE.md",
    "BUILD_WINDOWS.md",
    "NOTICE",
    "CODE_OF_CONDUCT.md",
)

TOKEN_RE = re.compile("ATTA" + r"[0-9A-Za-z]{20,}")
KEY_RE = re.compile("669aeddd" + "ef2efa19")
HOST_RE = re.compile("trello" + r".com", re.IGNORECASE)
MACHINE_PATH_RE = re.compile(r"C:\\Users|E:\\XDGit", re.IGNORECASE)
INTERNAL_WORD_RE = re.compile(
    r"(?i)(?<![A-Za-z0-9_])(cuevi|slack|trello|xdmaker|harness|governance|self_review|provenance|openclaw)(?![A-Za-z0-9_])"
)
INTERNAL_HOST_RE = re.compile(
    r"127\.0\.0\.1|10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|172\.(1[6-9]|2\d|3[0-1])\.\d{1,3}\.\d{1,3}"
)
INTERNAL_MACHINE_RE = re.compile(r"(?i)E:\\XDGit|E:/XDGit|C:\\Users\\|C:/Users/")
SKIP_SUFFIXES = {
    ".dll",
    ".exe",
    ".gif",
    ".ico",
    ".jpeg",
    ".jpg",
    ".lib",
    ".obj",
    ".pdb",
    ".png",
    ".svg",
    ".ttf",
    ".wav",
    ".webp",
    ".zip",
}

SQLITE3_ATTA_FALSE_POSITIVE = {
    "path": "thirdparty/sqlite/sqlite3.c",
    "line": 176523,
    "match": "ATTACHBETWEENOTHING" "ROUPSCASCADEFAULT",
    "reason": "SQLite 关键字粘连，不是 Trello token",
}

# (posix path, normalized token) -> reason. Systematic full-scan false positives only.
PUBLIC_INTERNAL_FEATURE_ALLOWLIST = {
    ("docs/legal/EULA.en.md", "governance"): "Legal text uses governance as a general legal term",
    ("docs/behavior/audition-plugin-chain-module-design.md", "cuevi"): "设计文档关联位置引用 Cuevi 卡片号",
    ("tests/test_preview_isolation_guard.cpp", "e:/xdgit"): "预览隔离守卫测试用 E:/XDGit 本机路径作为越界负例夹具",
    ("tests/test_preview_nas_probe.cpp", "c:\\users\\"): "NAS 探针测试用 C 盘 Users 路径作为本地非网络夹具",
    ("tests/test_preview_nas_probe.cpp", "c:/users/"): "NAS 探针测试用 C 盘 Users 路径作为本地非网络夹具",
    (".github/pull_request_template.md", "provenance"): "公开 PR 模板要求填写 provenance 证据栏",
    (".github/workflows/inner-source-gates.yml", "cuevi"): "私有仓 CI 步骤名仍引用 Cuevi 工具；卡 09 再裁工作流",
    (".github/workflows/inner-source-gates.yml", "governance"): "公开 gates 作业名 governance-and-release",
    (".github/workflows/inner-source-gates.yml", "provenance"): "Stage 1 / QA provenance 门禁步骤名",
    (".gitignore", "harness"): "忽略内部 harness 构建目录，目录本身已排除",
    (".gitignore", "openclaw"): "忽略 .openclaw 运行时，目录本身已排除",
    ("ASSET_PROVENANCE.md", "provenance"): "公开资产出处声明文件",
    ("CMakeLists.txt", "127.0.0.1"): "CTest 本机回环占位，不是内网主机",
    ("CMakeLists.txt", "cuevi"): "产品 CueviFeedback 与守卫测试目标名",
    ("CMakeLists.txt", "harness"): "CTest harness 目标名",
    ("CMakeLists.txt", "openclaw"): "注释说明 .openclaw 不进发布树",
    ("LICENSES/README.md", "provenance"): "第三方许可证出处说明",
    ("LICENSES/TweetNaCl-PUBLIC-DOMAIN.txt", "provenance"): "上游许可证 provenance 用词",
    ("docs/behavior/README.md", "harness"): "行为文档提到测试 harness 入口",
    ("docs/behavior/acceptance-checklist.md", "openclaw"): "验收表路径列到已排除的 .openclaw 测试，便于对照",
    ("docs/behavior/modules/cover-replacement-browse-surface.md", "openclaw"): "行为文档引用已排除的 harness 命令",
    ("docs/behavior/modules/cover-replacement-retirement.md", "openclaw"): "行为文档引用已排除的 harness 命令",
    ("docs/behavior/modules/editor-handoff.md", "provenance"): "编辑器交接行为里的 provenance 字段名",
    ("docs/behavior/modules/scan-indexing.md", "harness"): "扫描行为文档引用测试 harness",
    ("docs/behavior/modules/scan-indexing.md", "openclaw"): "扫描行为文档引用已排除的 harness 路径",
    ("docs/behavior/regression-suite.md", "openclaw"): "回归套件文档引用已排除的 harness 路径",
    ("installer/HarkenSearch_Setup.iss", "provenance"): "安装器第三方 provenance 文案",
    ("installer/aris_release_history.json", "cuevi"): "历史发行说明里的旧反馈通道名",
    ("installer/harken_release_history.json", "cuevi"): "qa/current 已把同一份历史说明改名为 harken_*，合并扫描仍会命中旧反馈通道名",
    ("locale/harken.ui.strings.json", "cuevi"): "产品反馈提交成功文案，对应 src/CueviFeedback*",
    ("scripts/package_local_update_demo.ps1", "127.0.0.1"): "本地更新演示绑定本机回环",
    ("scripts/package_local_update_demo.ps1", "provenance"): "演示包 provenance 校验",
    ("scripts/qa_probe_cover_browse_surface.py", "harness"): "QA 探针注释中的测试 harness 词",
    ("tools/qa_update_notice_panel.py", "harness"): "更新面板 QA 工具注释中的隔离驱动接缝描述",
    ("scripts/qa_probe_cover_browse_surface.py", "openclaw"): "QA 探针注释中的已退役路径名",
    ("scripts/qa_probe_cover_replacement_integration.py", "harness"): "QA 探针注释中的测试 harness 词",
    ("scripts/qa_probe_cover_replacement_integration.py", "openclaw"): "QA 探针注释中的已退役路径名",
    ("scripts/qa_probe_cover_replacement_retirement.py", "harness"): "QA 探针注释中的测试 harness 词",
    ("scripts/qa_probe_cover_replacement_retirement.py", "openclaw"): "QA 探针注释中的已退役路径名",
    ("scripts/qa_probe_cover_wall_perf.py", "harness"): "QA 探针注释中的测试 harness 词",
    ("scripts/qa_probe_cover_wall_perf.py", "openclaw"): "QA 探针注释中的已退役路径名",
    ("src/AppState.h", "harness"): "测试 seam / harness 钩子注释",
    ("src/AudioEngine.h", "slack"): "英文 slack window（余量窗口），不是 Slack 产品",
    ("src/CueviFeedbackClient.cpp", "127.0.0.1"): "产品反馈客户端本机开发回环默认值",
    ("src/CueviFeedbackClient.cpp", "cuevi"): "产品反馈客户端类型名",
    ("src/WaapiClient.h", "127.0.0.1"): "Wwise WAAPI 默认本机回环，产品协议不是内网主机",
    ("src/WindowsMain.cpp", "127.0.0.1"): "WAAPI/本机回环默认值",
    ("src/WindowsMain.cpp", "harness"): "调试/测试 harness 分支注释",
    ("src/WwiseTransferConfig.h", "127.0.0.1"): "WAAPI 默认本机回环",
    ("src/WwiseTransferExecute.cpp", "provenance"): "Wwise 传输 provenance 字段",
    ("src/bootstrap/WindowsBootstrapFilesystemWin32.cpp", "provenance"): "bootstrap 文件 provenance 记录",
    ("src/bootstrap/WindowsBootstrapJournal.cpp", "provenance"): "bootstrap journal provenance 记录",
    ("src/bootstrap/WindowsBootstrapRuntimeWin32.cpp", "harness"): "测试 harness 运行时钩子",
    ("src/main.cpp", "harness"): "测试 harness 入口注释",
    ("src/update/UpdateCheckService.cpp", "xdmaker"): "注释说明与 XDMaker 手动更新语义兼容，不含内网地址",
    ("src/webui/src/components/desktop-annotation/DesktopAnnotationPage.tsx", "127.0.0.1"): "标注页本机开发服务器默认值",
    ("tests/bootstrap/bootstrap_handoff_fixture.cpp", "harness"): "bootstrap 测试 harness 夹具",
    ("tests/bootstrap/test_aris_bootstrap_blackbox.py", "harness"): "bootstrap 黑盒测试 harness 词",
    ("tests/bootstrap/test_aris_bootstrap_filesystem.cpp", "harness"): "bootstrap 文件系统测试 harness 词",
    ("tests/bootstrap/test_aris_bootstrap_filesystem.cpp", "provenance"): "bootstrap provenance 行为测试",
    ("tests/test_annotation_launcher_contract.py", "127.0.0.1"): "标注启动契约的本机回环",
    ("tests/test_aris_annotation_server.py", "127.0.0.1"): "标注服务本机绑定",
    ("tests/test_aris_annotation_server.py", "openclaw"): "测试夹具路径名，不发布 .openclaw 目录",
    ("tests/test_aris_card_worktree_gate.py", "cuevi"): "卡片 worktree 门禁测试提到任务卡身份",
    ("tests/test_aris_design_review_server.py", "127.0.0.1"): "设计评审服务本机绑定",
    ("tests/test_aris_figma_handoff.py", "governance"): "Figma 交接治理检查用词",
    ("tests/test_backfill_recovery_worker.cpp", "harness"): "C++ 测试 harness 助手",
    ("tests/test_background_io_policy.cpp", "harness"): "C++ 测试 harness 助手",
    ("tests/test_cover_replacement_integration.cpp", "harness"): "C++ 测试 harness 助手",
    ("tests/test_cover_replacement_integration.cpp", "openclaw"): "测试命令行引用已排除路径",
    ("tests/test_export_region_preserve.cpp", "slack"): "tail slack（余量）测试文案，不是 Slack",
    ("tests/test_feedback_transport_boundary.py", "trello"): "守卫测试确认 Trello 传输已删除",
    ("tests/test_github_main_protection.py", "governance"): "main 保护治理检查",
    ("tests/test_harken_search_update_endpoints_scan.py", "127.0.0.1"): "本地更新演示环回地址，非内部信息",
    ("tests/harken_search_visible_copy_baseline.json", "provenance"): "可见文案账本说明为何锁定历史测量记录，不是内部工具",
    ("tests/test_import_precompute_waveform.cpp", "harness"): "C++ 测试 harness 助手",
    ("tests/test_imgui_docking_viewport_contract.py", "harness"): "Dock 视口合同测试在临时目录生成的 C++ 测试助手",
    ("tests/test_inner_source_governance.py", "cuevi"): "内源 gates 仍编译/运行 Cuevi 工具步骤",
    ("tests/test_inner_source_governance.py", "governance"): "内源治理门禁测试名与步骤名",
    ("tests/test_inner_source_governance.py", "provenance"): "内源 gates 的 provenance 步骤名",
    ("tests/test_localization_purity.py", "cuevi"): "纯度豁免反例故意写一个不是任务卡号的 owner，用来证明会被拒绝",
    ("tests/test_mini_waveform_decode_perf.cpp", "openclaw"): "性能探针注释中的已退役路径",
    ("tests/test_public_repo_manifest.py", "cuevi"): "本测试必须写出要排除的 cuevi 路径模式",
    ("tests/test_public_repo_manifest.py", "governance"): "本测试必须写出排除集中的 governance 目录名",
    ("tests/test_public_repo_manifest.py", "openclaw"): "本测试必须写出排除集中的 openclaw 目录名",
    ("tests/test_public_repo_manifest.py", "trello"): "本测试用拆开的正则扫描 trello host 残留",
    ("tests/test_public_repo_manifest.py", "127.0.0.1"): "allowlist keys document loopback addresses",
    ("tests/test_public_repo_manifest.py", "192.168.1.5"): "allowlist keys document private-ip negative fixtures",
    ("tests/test_public_repo_manifest.py", "192.168.1.10"): "allowlist keys document WAAPI private-ip negative fixtures",
    ("tests/test_public_repo_manifest.py", "e:/xdgit"): "allowlist keys document banned machine-path markers",
    ("tests/test_public_repo_manifest.py", "c:/users/"): "allowlist keys document banned machine-path markers",
    ("tests/test_public_repo_manifest.py", "harness"): "allowlist covers product test-harness wording",
    ("tests/test_public_repo_manifest.py", "provenance"): "allowlist covers public provenance wording",
    ("tests/test_public_repo_manifest.py", "slack"): "allowlist covers English slack-window false positives",
    ("tests/test_public_repo_manifest.py", "xdmaker"): "allowlist covers XDMaker-compatible comment wording",
    ("tests/test_public_repo_manifest.py", "self_review"): "audit exclusion globs must name daily_self_review tools",
    ("tests/test_reaper_bridge_stub.cpp", "harness"): "C++ 测试 harness 助手",
    ("tests/test_reaper_send_productization.py", "harness"): "REAPER 发送测试 harness 词",
    ("tests/test_release_notes.cpp", "provenance"): "发行说明 provenance 字段测试",
    ("tests/test_review_feedback_collector.py", "governance"): "评审反馈收集器治理合同测试",
    ("tests/test_scan_perf_boom.cpp", "openclaw"): "性能探针注释中的已退役路径",
    ("tests/test_scan_waveform_backfill_perf.cpp", "openclaw"): "性能探针注释中的已退役路径",
    ("tests/test_search_hit_column_identity.py", "harness"): "测试读已排除的 harness 配置，目录本身不进公开树",
    ("tests/test_search_hit_column_identity.py", "openclaw"): "测试读已排除的 harness 配置，目录本身不进公开树",
    ("tests/test_stage1_provenance_gate.py", "provenance"): "Stage1 资产 provenance 门禁测试",
    ("tests/test_stage3_human_style_audit.py", "harness"): "风格审计测试 harness 词",
    ("tests/test_stage3_style_gate.py", "provenance"): "风格门禁对 provenance 注释的检查",
    ("tests/test_tab_presets.cpp", "harness"): "C++ 测试 harness 助手",
    ("tests/test_trello_deadcode_scan.py", "trello"): "守卫测试确认 Trello 死代码已删除",
    ("tests/test_update_local_demo_contract.py", "127.0.0.1"): "本地更新演示允许的本机回环 URL",
    ("tests/test_update_local_demo_contract.py", "192.168.1.5"): "合同测试用私网地址作反例，断言必须拒绝",
    ("tests/test_update_local_demo_contract.py", "cuevi"): "本地演示合同提到反馈通道名",
    ("tests/test_waapi_plaintext_loopback_contract.py", "127.0.0.1"): "WAAPI 只允许本机回环的正例",
    ("tests/test_waapi_plaintext_loopback_contract.py", "192.168.1.10"): "WAAPI 合同测试用私网地址作反例，断言必须拒绝",
    ("tests/test_waapi_plaintext_loopback_contract.py", "harness"): "WAAPI 合同测试 harness 词",
    ("tests/tool_similarity_bench.cpp", "slack"): "vector slack（容量余量），不是 Slack",
    ("tests/tool_similarity_bench.cpp", "trello"): "历史票号注释 Trello #372，无 host/token",
    ("thirdparty/imgui/imgui_widgets.cpp", "slack"): "Dear ImGui 英文 slack 余量注释",
    ("thirdparty/ixwebsocket/ixwebsocket/IXGetFreePort.cpp", "127.0.0.1"): "上游库本机端口探测",
    ("thirdparty/ixwebsocket/ixwebsocket/IXSocketServer.cpp", "127.0.0.1"): "上游库本机监听默认值",
    ("thirdparty/miniaudio.h", ".".join(("10", "3", "1", "1"))): "miniaudio 文档章节号，不是 IPv4 主机",
    ("thirdparty/sqlite/sqlite3.c", "harness"): "SQLite 测试 harness 注释，不是内部工具",
    ("tools/aris_annotation_server.py", "127.0.0.1"): "标注服务本机绑定",
    ("tools/aris_design_review_server.py", "127.0.0.1"): "设计评审服务本机绑定",
    ("tools/aris_design_review_server.py", "governance"): "设计评审服务治理提示文案",
    ("tools/check_release_compliance.py", "c:/users/"): "禁词清单本身，用来拦截本机路径泄漏",
    ("tools/check_release_compliance.py", "e:/xdgit"): "禁词清单本身，用来拦截本机路径泄漏",
    ("tools/check_release_compliance.py", "provenance"): "合规扫描对 provenance 声明的检查",
    ("tools/prepare_local_update_demo.py", "127.0.0.1"): "本地更新演示本机镜像",
    ("tools/prepare_local_update_demo.py", "cuevi"): "演示脚本注释中的任务卡身份",
    ("tools/prepare_local_update_demo.py", "provenance"): "演示包 provenance 校验",
    ("scripts/package_qa_exe.ps1", "cuevi"): "Optional feedback build parameters contain no endpoint or secret values",
    ("tools/public_repo_manifest.py", "cuevi"): "排除规则必须写出 .cuevi/ 与 cuevi_* 文件名",
    ("tools/localization_purity.py", "cuevi"): "公开 CI 门禁源码含看板名，不可中性化",
    ("tools/public_repo_manifest.py", "governance"): "排除规则必须写出 docs/governance/",
    ("tools/public_repo_manifest.py", "openclaw"): "排除规则必须写出 .openclaw/",
    ("tools/database_switch_acceptance.py", "openclaw"): "隔离数据库切换验收脚本引用未发布的本机 QA 驱动目录",
    ("tools/database_switch_acceptance.py", "harness"): "隔离数据库切换验收脚本引用未发布的本机 QA 驱动目录",
    ("tools/database_switch_busy_acceptance.py", "openclaw"): "扫描中切库拒绝验收脚本引用未发布的本机 QA 驱动目录",
    ("tools/database_switch_busy_acceptance.py", "harness"): "扫描中切库拒绝验收脚本引用未发布的本机 QA 驱动目录",
    ("tools/reaper_real_validation.ps1", "harness"): "REAPER 验证脚本测试 harness 词",
    ("tools/stage6_main_thread_frame_budget_probe.py", "harness"): "性能探针测试 harness 词",
    ("tools/stage6_main_thread_frame_budget_probe.py", "openclaw"): "探针注释中的已退役路径",
    ("tools/start_aris_annotation.ps1", "127.0.0.1"): "标注服务本机启动地址",
    ("tools/start_aris_design_review.ps1", "127.0.0.1"): "设计评审服务本机启动地址",
    ("tools/verify_github_main_protection.py", "governance"): "main 保护治理检查工具",
    ("tools/website_release_catalog_preview.py", "127.0.0.1"): "发行页预览本机绑定；website/ 本体已排除",
    ("tools/website_release_page_preview.py", "127.0.0.1"): "发行页预览本机绑定；website/ 本体已排除",
}


def _posix(path: str) -> str:
    return Path(path).as_posix()


def _iter_audit_internal_tracked_paths() -> list[str]:
    leaked_candidates = []
    for path in manifest._git_tracked_files(ROOT):
        posix = _posix(path)
        if posix == "tests/website" or posix.startswith("tests/website/"):
            leaked_candidates.append(posix)
            continue
        if any(fnmatch(posix, glob) for glob in AUDIT_INTERNAL_GLOBS):
            leaked_candidates.append(posix)
    return leaked_candidates


def _scan_public_internal_features() -> list[str]:
    hits: list[str] = []
    for rel in manifest.iter_public_paths(ROOT):
        posix = _posix(rel)
        path = ROOT / rel
        if not path.is_file() or path.suffix.lower() in SKIP_SUFFIXES or path.suffix.lower() == ".pyc":
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            text = path.read_bytes().decode("utf-8", errors="replace")
        for rx in (INTERNAL_WORD_RE, INTERNAL_HOST_RE, INTERNAL_MACHINE_RE):
            for match in rx.finditer(text):
                token = match.group(0)
                key = (posix, token.casefold())
                if key in PUBLIC_INTERNAL_FEATURE_ALLOWLIST:
                    continue
                line = text.count("\n", 0, match.start()) + 1
                hits.append(f"{posix}:{line}:{token}")
    return hits


class PublicRepoManifestTests(unittest.TestCase):
    def test_exclusion_set_covers_internal_directories(self):
        excluded = set(manifest.excluded_prefixes())
        missing = [prefix for prefix in REQUIRED_INTERNAL_PREFIXES if prefix not in excluded]
        self.assertEqual(missing, [], "internal directories missing from exclusion set")

    def test_audit_internal_globs_are_excluded_from_public_set(self):
        public = set(_posix(path) for path in manifest.iter_public_paths(ROOT))
        leaked = [path for path in _iter_audit_internal_tracked_paths() if path in public or manifest.is_public_path(path)]
        self.assertEqual(leaked, [], "audit FAIL internals remain public:\n" + "\n".join(leaked))

    def test_excluded_globs_require_explicit_same_family_list_with_reasons(self):
        actual = list(manifest.excluded_globs())
        missing_audit = [glob for glob in AUDIT_INTERNAL_GLOBS if glob not in actual]
        self.assertEqual(missing_audit, [], "AUDIT_INTERNAL_GLOBS dropped from publication exclusions")
        extras = [glob for glob in actual if glob not in AUDIT_INTERNAL_GLOBS]
        named = [glob for glob, _reason in EXPLICIT_SAME_FAMILY_EXCLUSION_GLOBS]
        self.assertEqual(extras, named, "new public-set exclusions must be named in EXPLICIT_SAME_FAMILY_EXCLUSION_GLOBS with a reason")
        missing_reasons = [glob for glob, reason in EXPLICIT_SAME_FAMILY_EXCLUSION_GLOBS if not str(reason).strip()]
        self.assertEqual(missing_reasons, [], "same-family exclusions missing reasons: " + ", ".join(missing_reasons))

    def test_internal_host_regex_hits_known_private_ip_samples(self):
        samples = (
            ".".join(("10", "0", "0", "1")),
            ".".join(("192", "168", "1", "5")),
            ".".join(("172", "16", "0", "1")),
            ".".join(("10", "3", "1", "1")),
        )
        missed = [sample for sample in samples if INTERNAL_HOST_RE.search(sample) is None]
        self.assertEqual(missed, [], "INTERNAL_HOST_RE missed known private-ip samples: " + ", ".join(missed))

    def test_synthetic_internal_scan_must_report_known_private_ip_samples(self):
        samples = (
            ".".join(("10", "0", "0", "1")),
            ".".join(("192", "168", "1", "5")),
            ".".join(("172", "16", "0", "1")),
            ".".join(("10", "3", "1", "1")),
        )
        blob = "marker " + " ".join(samples) + " end"
        hits = [match.group(0) for match in INTERNAL_HOST_RE.finditer(blob)]
        self.assertEqual(hits, list(samples), "scanner did not report implanted private-ip samples")

    def test_this_file_does_not_embed_miniaudio_chapter_ip_as_contiguous_token(self):
        banned = ".".join(("10", "3", "1", "1"))
        source = Path(__file__).read_text(encoding="utf-8")
        self.assertNotIn(banned, source, "test fixture still embeds a contiguous private-ip token")

    def test_public_github_subset_and_dotfiles_are_included(self):
        public = set(manifest.iter_public_paths(ROOT))
        for path in (
            ".github/pull_request_template.md",
            ".github/workflows/public-checks.yml",
            ".gitignore",
        ):
            self.assertIn(path, public)
        self.assertIn(".github/workflows/auto-merge.yml", public)
        self.assertNotIn(".github/workflows/pr-merge-notify.yml", public)

    def test_legal_facade_notice_security_and_code_of_conduct_ship_in_public_tree(self):
        # RQ-20260911-09: root NOTICE, public GitHub SECURITY.md, and
        # Contributor Covenant must be in the publication set.
        self.assertIn("NOTICE", manifest.PUBLIC_TOP_LEVEL_FILES)
        self.assertIn("CODE_OF_CONDUCT.md", manifest.PUBLIC_TOP_LEVEL_FILES)
        self.assertIn(".github/SECURITY.md", manifest.PUBLIC_GITHUB_PATHS)
        for name in ("NOTICE", "CODE_OF_CONDUCT.md"):
            self.assertIn(name, FACADE_DOCS)
        public = set(manifest.iter_public_paths(ROOT))
        for path in ("NOTICE", "CODE_OF_CONDUCT.md", ".github/SECURITY.md"):
            self.assertTrue((ROOT / path).is_file(), f"missing legal facade file {path}")
            self.assertIn(path, public)

    def test_github_security_policy_matches_root_policy_body(self):
        root = (ROOT / "SECURITY.md").read_text(encoding="utf-8")
        github = (ROOT / ".github" / "SECURITY.md").read_text(encoding="utf-8")
        root_body = root.split("This policy covers Harken Search.", 1)[1]
        github_body = github.split("This policy covers Harken Search.", 1)[1]
        self.assertEqual(root_body, github_body)
        self.assertIn("harken-support@xd.com", root)
        self.assertIn("harken-support@xd.com", github)

    def test_public_docs_keep_product_documents_and_required_dependency_rules(self):
        docs = [path for path in manifest.iter_public_paths(ROOT) if path.replace("\\", "/").startswith("docs/")]
        illegal = [
            path
            for path in docs
            if not (
                path.replace("\\", "/").startswith("docs/behavior/")
                or path.replace("\\", "/").startswith("docs/manual/")
                or path.replace("\\", "/").startswith("docs/legal/")
                or path == "docs/architecture/stage-4-dependency-rules.json"
            )
        ]
        self.assertEqual(illegal, [], "public docs must be product documents or the exact dependency-rule fixture")

    def test_license_exists_and_is_apache_2_0(self):
        license_path = ROOT / "LICENSE"
        self.assertTrue(license_path.is_file(), "root LICENSE is required")
        text = license_path.read_text(encoding="utf-8")
        self.assertIn("Apache License", text)
        self.assertIn("Version 2.0", text)
        self.assertRegex(text, r"Copyright 2026 xindong")

    def test_public_tree_has_no_trello_secrets_or_host(self):
        hits = []
        allowed = (
            SQLITE3_ATTA_FALSE_POSITIVE["path"],
            SQLITE3_ATTA_FALSE_POSITIVE["line"],
            SQLITE3_ATTA_FALSE_POSITIVE["match"],
        )
        for rel in manifest.iter_public_paths(ROOT):
            path = ROOT / rel
            posix = _posix(rel)
            if not path.is_file() or path.suffix.lower() in SKIP_SUFFIXES or path.suffix.lower() == ".pyc":
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                text = path.read_bytes().decode("utf-8", errors="replace")
            for label, rx in (("token", TOKEN_RE), ("key", KEY_RE), ("host", HOST_RE)):
                for match in rx.finditer(text):
                    line = text.count("\n", 0, match.start()) + 1
                    if (
                        label == "token"
                        and posix == allowed[0]
                        and line == allowed[1]
                        and match.group(0) == allowed[2]
                    ):
                        continue
                    hits.append(f"{posix}:{label}:line={line}:offset={match.start()}:len={len(match.group(0))}")
        self.assertEqual(hits, [], "forbidden leftovers remain in public tree:\n" + "\n".join(hits))

    def test_public_tree_has_no_machine_absolute_paths(self):
        hits = []
        for rel in manifest.iter_public_paths(ROOT):
            path = ROOT / rel
            if not path.is_file() or path.suffix.lower() in SKIP_SUFFIXES:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                text = path.read_bytes().decode("utf-8", errors="replace")
            posix = _posix(rel)
            for match in MACHINE_PATH_RE.finditer(text):
                token = match.group(0)
                if (posix, token.casefold()) in PUBLIC_INTERNAL_FEATURE_ALLOWLIST:
                    continue
                if (posix, "e:/xdgit") in PUBLIC_INTERNAL_FEATURE_ALLOWLIST and "XDGit" in token:
                    continue
                if (posix, "c:/users/") in PUBLIC_INTERNAL_FEATURE_ALLOWLIST and "Users" in token:
                    continue
                hits.append(f"{posix}:offset={match.start()}:{match.group(0)}")
        self.assertEqual(hits, [], "machine absolute paths remain in public tree:\n" + "\n".join(hits))

    def test_release_history_cuevi_token_covers_aris_and_harken_filenames(self):
        for path in (
            "installer/aris_release_history.json",
            "installer/harken_release_history.json",
        ):
            self.assertIn((path, "cuevi"), PUBLIC_INTERNAL_FEATURE_ALLOWLIST)

    def test_search_hit_column_identity_openclaw_harness_hits_are_allowlisted(self):
        path = "tests/test_search_hit_column_identity.py"
        text = (ROOT / path).read_text(encoding="utf-8")
        tokens = {
            match.group(0).casefold()
            for rx in (INTERNAL_WORD_RE, INTERNAL_HOST_RE, INTERNAL_MACHINE_RE)
            for match in rx.finditer(text)
        }
        self.assertEqual(
            tokens,
            {"openclaw", "harness"},
            "target file hits drifted from the registered exemptions; update the allowlist accordingly",
        )
        for token in sorted(tokens):
            key = (path, token)
            self.assertIn(key, PUBLIC_INTERNAL_FEATURE_ALLOWLIST)
            self.assertTrue(
                str(PUBLIC_INTERNAL_FEATURE_ALLOWLIST[key]).strip(),
                f"allowlist {key} missing a registration reason",
            )

    def test_magic_eq_public_contract_has_no_private_driver_dependency(self):
        source = (ROOT / "tests/test_magic_eq_ui_contract.py").read_text(encoding="utf-8")
        self.assertEqual(INTERNAL_WORD_RE.findall(source), [])

    def test_public_tree_internal_feature_scan_is_fully_allowlisted(self):
        hits = _scan_public_internal_features()
        self.assertEqual(hits, [], "unallowlisted internal-feature hits in public tree:\n" + "\n".join(hits))

    def test_publication_tool_has_no_machine_or_host_literals(self):
        text = (ROOT / "tools/public_repo_manifest.py").read_text(encoding="utf-8")
        self.assertIsNone(INTERNAL_HOST_RE.search(text), "publication tool embeds a loopback/intranet host")
        machine = [m.group(0) for m in INTERNAL_MACHINE_RE.finditer(text)]
        self.assertEqual(machine, [], "publication tool embeds machine absolute paths: " + ", ".join(machine))

    def test_facade_docs_use_harken_search_brand(self):
        missing = []
        stale = []
        for name in FACADE_DOCS:
            text = (ROOT / name).read_text(encoding="utf-8")
            if "Harken Search" not in text and "HarkenSearch" not in text:
                missing.append(name)
            if re.search(r"(?m)^# Aris\b", text):
                stale.append(name)
        self.assertEqual(missing, [], "facade docs missing Harken Search brand")
        self.assertEqual(stale, [], "facade docs still use Aris as the top-level title")

    def test_contributing_is_public_and_omits_internal_governance_wording(self):
        text = (ROOT / "CONTRIBUTING.md").read_text(encoding="utf-8")
        banned = (
            "qa/current",
            "shared `main` workspace",
            "共享 main 工作区",
            "中文入口规则",
            "card-specific branch",
            "independent worktree",
            "卡片专属分支",
            "独立 worktree",
        )
        hits = [phrase for phrase in banned if phrase in text]
        self.assertEqual(hits, [], "CONTRIBUTING still names internal governance wording")
        self.assertIn("CODE_OF_CONDUCT.md", text)
        self.assertTrue("SECURITY.md" in text or ".github/SECURITY.md" in text)
        self.assertRegex(text, r"(?i)\bDCO\b")

    def test_readme_uses_welcome_surface_and_states_trademark_limit(self):
        text = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertNotIn("login surfaces", text)
        self.assertNotIn("startup/login", text)
        self.assertTrue("welcome" in text.lower())
        self.assertIn("local WebView2 welcome surface", text)
        self.assertIn("trademark", text.lower())
        self.assertIn("The Apache License 2.0 does not grant trademark rights.", text)
        self.assertIn("Release compliance", text)
        self.assertIn("py tools/check_release_compliance.py", text)

    def test_public_markdown_links_stay_inside_the_public_tree(self):
        link_re = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
        public = {Path(path).as_posix() for path in manifest.iter_public_paths(ROOT)}
        broken = []
        for rel in sorted(public):
            path = ROOT / rel
            if path.suffix.lower() != ".md" or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8")
            for match in link_re.finditer(text):
                raw = match.group(1).strip().split()[0].strip("<>")
                target, _, _ = raw.partition("#")
                if not target or re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I):
                    continue
                resolved = (path.parent / target).resolve()
                try:
                    posix = resolved.relative_to(ROOT.resolve()).as_posix()
                except ValueError:
                    continue
                if Path(rel).as_posix().startswith("thirdparty/"):
                    continue
                if posix not in public and not any(
                    posix == item or posix.startswith(item.rstrip("/") + "/")
                    for item in public
                ):
                    broken.append(f"{Path(rel).as_posix()} -> {posix}")
        excluded_mentions = []
        excluded_prefixes = (
            "docs/process/",
            "docs/templates/",
            "docs/plans/",
            "docs/governance/",
            "docs/product/",
            "docs/context/",
            "docs/architecture/",
            "docs/superpowers/",
        )
        for rel in sorted(public):
            path = ROOT / rel
            posix = Path(rel).as_posix()
            if path.suffix.lower() != ".md" or not path.is_file() or posix.startswith("thirdparty/"):
                continue
            text = path.read_text(encoding="utf-8")
            for prefix in excluded_prefixes:
                if prefix in text.replace("\\", "/"):
                    excluded_mentions.append(f"{posix}: {prefix}")
        self.assertEqual(broken, [], "public markdown links escape the publication set:\n" + "\n".join(broken))
        self.assertEqual(excluded_mentions, [], "public docs still name excluded internal paths:\n" + "\n".join(excluded_mentions))


    def test_exists_guard_skips_missing_sources_in_memory_tree(self):
        cmake = """
add_executable(Unguarded missing.cpp)
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/private.cpp)
  add_executable(Guarded tests/private.cpp src/ok.cpp)
endif()
add_executable(PublicTarget tests/ok.cpp)
"""
        executables = parse_cmake_add_executables(cmake)
        existing = {"tests/ok.cpp", "src/ok.cpp"}
        required = required_add_executable_sources(executables, existing)
        self.assertIn("missing.cpp", required)
        self.assertNotIn("tests/private.cpp", required)
        self.assertIn("tests/ok.cpp", required)

    def test_variable_source_lists_and_subdirectories_join_the_closure(self):
        files = {
            "CMakeLists.txt": """
set(APP_SOURCES src/public.cpp src/secret.cpp)
list(APPEND APP_SOURCES src/also_secret.cpp)
add_executable(App ${APP_SOURCES})
if(EXISTS ${CMAKE_SOURCE_DIR}/src/optional.cpp)
  set(OPTIONAL_SOURCES src/optional.cpp)
  add_executable(Optional ${OPTIONAL_SOURCES})
endif()
add_subdirectory(plugin)
include(cmake/extra_sources.cmake)
""",
            "plugin/CMakeLists.txt": """
set(PLUGIN_SOURCES hidden.cpp plugin/visible.cpp)
add_library(Plugin ${PLUGIN_SOURCES})
""",
            "cmake/extra_sources.cmake": """
set(EXTRA_SOURCES src/from_include.cpp)
add_executable(Extra ${EXTRA_SOURCES})
""",
        }
        existing = {
            "CMakeLists.txt",
            "plugin/CMakeLists.txt",
            "src/public.cpp",
            "src/secret.cpp",
            "src/also_secret.cpp",
            "plugin/hidden.cpp",
            "plugin/visible.cpp",
            "src/from_include.cpp",
            "cmake/extra_sources.cmake",
        }
        required = set(required_cmake_closure_sources(files, existing))
        self.assertEqual(
            required,
            {
                "src/public.cpp",
                "src/secret.cpp",
                "src/also_secret.cpp",
                "plugin/hidden.cpp",
                "plugin/visible.cpp",
                "src/from_include.cpp",
            },
        )

    def test_root_level_test_sources_are_in_public_top_level_files(self):
        missing = [
            path
            for path in ROOT_LEVEL_PUBLIC_TEST_SOURCES
            if path not in manifest.PUBLIC_TOP_LEVEL_FILES
        ]
        self.assertEqual(missing, [], "root-level test sources must ship in the public tree")


    def test_harken_search_icon_is_public_and_old_brand_icons_are_not(self):
        self.assertIn("HarkenSearch.ico", manifest.PUBLIC_TOP_LEVEL_FILES)
        self.assertNotIn("Aris.ico", manifest.PUBLIC_TOP_LEVEL_FILES)
        self.assertNotIn("Airs.ico", manifest.PUBLIC_TOP_LEVEL_FILES)
        public = set(manifest.iter_public_paths(ROOT))
        self.assertIn("HarkenSearch.ico", public)
        self.assertNotIn("Aris.ico", public)
        self.assertNotIn("Airs.ico", public)

    def test_existing_add_executable_sources_are_public_or_exists_guarded(self):
        public = set(manifest.iter_public_paths(ROOT))
        leaks = []
        for rel, item in iter_repo_cmake_add_executables(ROOT):
            for source in list(item["sources"]):
                disk = ROOT / source
                if not disk.is_file():
                    continue
                posix = Path(source).as_posix()
                if posix in public or manifest.is_public_path(posix):
                    continue
                guards = [Path(guard).as_posix() for guard in list(item["exists_guards"])]
                if any(guard not in public and not manifest.is_public_path(guard) for guard in guards):
                    continue
                leaks.append(f"{rel}: {item['target']} -> {posix} guards={guards}")
        self.assertEqual(leaks, [], "cmake add_executable sources leak out of the public set without EXISTS guards:\n" + "\n".join(leaks))

    def test_filtered_tree_add_executable_sources_are_present_or_guarded(self):
        public = {Path(path).as_posix() for path in manifest.iter_public_paths(ROOT)}
        executables = [item for _, item in iter_repo_cmake_add_executables(ROOT)]
        required = required_add_executable_sources(executables, public)
        missing = []
        for source in required:
            posix = Path(source).as_posix()
            disk = ROOT / posix
            if disk.is_file() and posix not in public:
                missing.append(posix)
        self.assertEqual(missing, [], "filtered public tree would still require excluded add_executable sources:\n" + "\n".join(missing))

    def test_filtered_tree_variable_and_subdirectory_sources_are_present_or_guarded(self):
        public = {Path(path).as_posix() for path in manifest.iter_public_paths(ROOT)}
        files = {
            path.relative_to(ROOT).as_posix(): path.read_text(encoding="utf-8")
            for path in [ROOT / "CMakeLists.txt", *ROOT.rglob("CMakeLists.txt"), * (ROOT / "cmake").glob("*.cmake")]
            if path.is_file()
        }
        required = required_cmake_closure_sources(files, public | set(files))
        missing = sorted({
            Path(source).as_posix()
            for source in required
            if (ROOT / source).is_file() and Path(source).as_posix() not in public
        })
        self.assertEqual(
            missing,
            [],
            "filtered public tree would still require excluded variable or subdirectory sources:\n" + "\n".join(missing),
        )

    def test_cuevi_internal_targets_are_exists_guarded(self):
        by_source = {}
        for rel, item in iter_repo_cmake_add_executables(ROOT):
            for source in list(item["sources"]):
                by_source.setdefault(Path(source).as_posix(), []).append((rel, item))
        missing_guards = []
        for source in CUEVI_INTERNAL_TEST_SOURCES:
            entries = by_source.get(source, [])
            if not entries:
                missing_guards.append(f"{source}: add_executable not found")
                continue
            for rel, item in entries:
                guards = [Path(guard).as_posix() for guard in list(item["exists_guards"])]
                if source not in guards:
                    missing_guards.append(f"{rel}: {item['target']} missing EXISTS {source}; guards={guards}")
        self.assertEqual(missing_guards, [], "cuevi internal tests must be wrapped in if(EXISTS <file>):\n" + "\n".join(missing_guards))


if __name__ == "__main__":
    unittest.main()
