# Harken Search

Professional sound manager.

Windows desktop application for audio library browsing, search, preview, metadata workflows, and DAW bridge utilities.

## 仓库去向

- 源码、问题反馈和后续开发：https://github.com/xindong/harken-search
- 公开仓从经过整理的源码开始，旧私有仓的历史记录单独保留。
- `main` 用于正式构建，`qa/current` 用于已验收改动的集成与 QA 包。

## 提交边界 / Contribution boundary

- 只提交当前任务验收所必需的最小改动：代码、测试、构建配置、以及必要文档。
- 与当前任务无关的迁移资料、历史卡片、本地流程状态、缓存、个人配置、AI/agent 运行产物不进入 PR。
- 共享规则以 [CONTRIBUTING.md](CONTRIBUTING.md) 为准；不确定某个文件该不该提交时，默认先不要提交。

## PR 与 QA 自动化

- 向 `main` 或 `qa/current` 提交 PR 时，运行公开检查与 Windows 构建测试。
- 完成审查和验收后，由维护者添加 `auto-merge-ok` 标签，才会排队自动合并；目标分支的远端保护规则缺失时会拒绝排队。
- 本地复验入口：`python tools/run_public_checks.py`。本地完整检查需要 Windows PowerShell、Git、Python 及 `requirements-ci.txt` 中的依赖。`qa/current` 的 QA EXE 使用触发该次运行的提交，并在产物名称中标明该提交号。

### 卡目录收尾

先由任务负责人核对任务已完成并归档，再对单张卡目录运行预检：

```powershell
python tools/remove_card_directory.py --workspace-root "<absolute-parent>" --card-name "<card-directory>" --card-prefix "<card-prefix>" --main-repository "<absolute-main-repository>" --repository xindong/harken-search --integration-ref qa/current --dry-run
```

预检会刷新远端集成分支，拒绝链接、错误仓库、未保存或被 Git 忽略的文件（包括构建与测试产物）、隐藏修改的索引标记、stash，以及未合入的本地分支或标签。卡目录是独立 clone 时，还会拒绝同一仓库关联的其他 Git worktree；卡目录是 Git worktree 时，则要求它唯一登记在 `--main-repository` 指定的主仓、停在本卡功能分支，并且只证明本卡 HEAD 已合入（同仓其他卡的分支和标签不参与判定），删除时用 `git worktree remove` 注销登记。核对输出的目标与完整 HEAD 后，使用同一组参数，将 `--dry-run` 换成 `--execute --expected-head <HEAD>` 才会删除。卡目录根部遗留的 `_cuevi-session.json` 这类已知运行期文件，只有在未跟踪或被忽略时才按名字放行，并随目录一起删除；已暂存、已跟踪但有改动的同名文件仍然拒绝，避免把真实改动一起删掉。其他未跟踪或被忽略的文件同样拒绝。脚本只处理指定目录；已有清理锁须人工核验，执行期间不要再向该目录写入文件。

## Build

Windows quick build / packaging guide:

- [BUILD_WINDOWS.md](BUILD_WINDOWS.md)
- [Optional local MCP connector](docs/manual/external-ai-mcp.md)

## Release compliance

Follow [BUILD_WINDOWS.md](BUILD_WINDOWS.md) for the ordinary Debug build or the
public release build with its required package key ID and verification public key.
Official packages are built from `main` by the `package-update.yml` workflow.
The signing private key is never part of the source tree or a CMake command.
Run the compliance checks before building and after assembling the release:

```powershell
py tools/check_release_compliance.py
py tools/check_release_compliance.py --release-dir build_public/Release
```

## Public-release notes

- Build public packages with `-DARIS_PUBLIC_RELEASE=ON`.
- Keep `-DAURO_BUNDLE_AVID_PTSL=OFF` by default unless Avid redistribution has been manually reviewed and approved for that package.
- The retired React/WebUI prototype is not part of this repository or release. Harken Search keeps its compiled native UI and the local WebView2 welcome surface implemented in `src/WindowsMain.cpp`.
- Release-facing AI/privacy expectations are documented in [PRIVACY.md](PRIVACY.md) and [AI_FEATURE_DISCLOSURE.md](AI_FEATURE_DISCLOSURE.md).
- 法律文件：[用户协议](docs/legal/EULA.md)、[隐私政策](docs/legal/PRIVACY_POLICY.md)、[开源发布总说明](docs/legal/OPEN_SOURCE_RELEASE_NOTICE.md)。
- English legal documents: [License Agreement](docs/legal/EULA.en.md), [Privacy Policy](docs/legal/PRIVACY_POLICY.en.md), [Open-Source Release Notice](docs/legal/OPEN_SOURCE_RELEASE_NOTICE.en.md).
- “Harken Search”, “Harken”, “XD”, and related logos are trademarks of XD Network Inc. The Apache License 2.0 does not grant trademark rights.
