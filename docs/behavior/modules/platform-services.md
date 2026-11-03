# Platform Services Behavior Contract

## Header

**ID:** BDD-PLATFORM-SERVICES
**Module:** Platform Services
**Status:** Active
**Priority:** P0
**Owner:** Engineering / QA
**Created:** 2026-07-14
**Last Updated:** 2026-11-02

## Product Behavior

**User promise:** PAL 隔离操作系统、文件、图像和音频 I/O，但不改变 Aris 现有用户可见行为、错误语义、扫描生命周期、播放状态或宿主交接结果。
**Primary workflow:** 业务模块提交平台无关输入，能力接口在边界完成原生转换和 I/O，返回稳定结果；业务模块继续拥有产品状态、文案、重试、数据库事务和 UI。
**Non-goals:** 本阶段不新增平台功能，不重写 DSP/搜索/数据库，不承诺新增 macOS/Linux 产品能力，不提前完成阶段 7 公共 API 契约化或阶段 4 解耦。

## 中文语义

**用户承诺：** 用户不应感知 PAL 迁移；已有路径、播放、扫描、封面和宿主交接行为保持一致。
**核心流程：** `System`、`FileSystem`、`FileWatcher`、`ImageCodec`、`AudioFile`、`AudioDevice` 各自隐藏平台类型，业务侧只接收值对象、结果码、样本缓冲区和平台无关回调。
**非目标：** 静态门禁不替代人工语义审查；本契约不授权任何未确认的用户行为变化。

## BDD Scenarios

### Happy Path

- Given 业务模块需要操作系统、文件、图像或音频 I/O
- When 调用方执行对应平台能力
- Then 请求只经过对应 `Platform::*` 能力，业务侧只看到平台无关值、结果和回调

### Edge Cases

- Given 路径包含中文、空格、长路径或目标不存在
- When PAL 转换并执行请求
- Then 返回稳定的平台无关结果，产品层继续拥有文案、重试和状态语义

### Failure / Recovery

- Given 平台 API 返回取消、权限不足、忙碌、不支持或 I/O 失败
- When PAL 翻译错误
- Then 不吞错、不触碰数据库或 UI，调用方得到可判定结果且现有诊断语义不变

### Non-Functional

- Given PAL 运行在主线程、后台线程或实时音频回调
- When 执行对应能力
- Then 主线程不突破 16ms 预算，实时回调不分配、不阻塞、不访问数据库或文件系统

### macOS Data and Settings

The approved macOS port adds a native data backend without changing the Windows
layout or settings fields. Normal data resolves under the user's
`Library/Application Support/HarkenSearch`; resolving this path does not create
or migrate a library. The app must explicitly check `EnsureLayout().readOnly`
before startup writes. There is no implicit import of Windows or legacy data.

Preview builds require an explicit compiled isolation root. Relative paths,
parent escapes, sibling-prefix matches and symbolic-link ancestry are rejected.
Enabling isolation without a compiled root fails closed, with no fallback to
daily data. Editable settings and thesaurus files live outside the app bundle.

Mac settings retain the existing UTF-8 key/value format. Saving writes a private
same-directory temporary file, flushes it, atomically replaces the target, then
flushes the directory. Failures before replacement preserve the old settings. A
failure after replacement reports unconfirmed durability; it must not be reported
as a successful save or as proof the old file still exists. Failed temporary
files are retained for diagnosis. These I/O calls must not run in audio callbacks.

`TestMacUserDataLayout`, `TestMacUserDataRoots`, `TestMacDurableSettings`,
`TestPlatformDurableFileOpsCopy` and `TestPlatformDurableFileOpsReplaceDelete`
exercise the real Mac backends, including a real file-size-limit write failure.
They do not prove application initialization, UI restoration or user acceptance;
those require the real application workflow. Set `HARKEN_MAC_TEST_ROOT` and the
test process's `TMPDIR` to the card's isolated QA directory.

## Interface Binding

The macOS database picker now uses a single-file Cocoa open panel with the
existing localized title and optional initial directory. Cancel returns an empty
path. The shared UI still owns database mounting and error presentation. The
diagnostic Mac command bridge is available only in non-beta diagnostic preview
builds with a compiled isolation root; scan/open paths must remain under that
root. It is not a production automation or remote-control API.

| Interface / Entry | Kind | Source | Expected behavior |
| --- | --- | --- | --- |
| `Platform::*` | API | design + Graphify | 按能力隐藏系统、文件、图像和音频平台类型 |
| `FileSystemWatcher` | worker | Graphify/source | 原生句柄隐藏，事件顺序、去抖、取消和关闭语义不变 |
| `AudioEngine` | API | `src/AudioEngine.*` | 继续拥有播放/DSP 产品语义，不暴露设备/流后端类型 |
| `Database` | DB | `src/Database.*` | 继续拥有事务、扫描、代次和取消，不直接调用平台 I/O |
| `CoverModule` | API | `src/CoverModule.cpp` | 只接收图像像素值和失败结果，不管理 WIC/原生编解码器 |
| `main.cpp` | UI | `src/main.cpp` | 仅编排平台能力，不直接拥有平台句柄或 I/O |
| `WindowsMain.cpp` | UI | `src/WindowsMain.cpp` | 保持窗口/消息/初始化行为，平台实现经 PAL 收口 |

## Graphify Binding

**Source graph:** `graphify-out/main/interface-call-graph.json`, `graphify-out/main/audit-call-graph.json`
**Impact target(s):** `Platform`, `FileSystemWatcher`, `Database`, `AudioEngine`, `CoverModule`, `main.cpp`, `WindowsMain.cpp`
**Audit classification requirement:** S6-01 首次刷新用于建立阶段 6 图谱基线；主 `GRAPH_REPORT.md` 在入口树缺失，不能假设已有报告可比。`UNKNOWN_BLOCKING` 需在阶段收口前分类或解决。
**Dynamic calls:** S6-01 建立基线后复核 `DYNAMIC_CALLS.md`。
**Unknown calls:** S6-01 建立基线后复核 `UNKNOWN_CALLS.md`。

## TDD / QA Proof

| Proof ID | Kind | Artifact / command / event | Pass signal | Status |
| --- | --- | --- | --- | --- |
| TEST-PLATFORM-BOUNDARY | contract | `tests/test_stage6_platform_boundary.py` / `TestStage6PlatformBoundary` | 账本与扫描结果一致，无新增发现 | missing |
| TEST-CONTRACT-REGISTRY | unit / gate | `TestBehaviorContractCheck` | 契约目录、证明和模块索引集合一致 | missing |
| TEST-PLATFORM-PERF | integration / performance | `TestStage6PerformanceBaseline` | 五轮入口快照、总体噪声与产品/回归门限均通过纯源验证 | active |
| DIAG-PLATFORM-GRAPH | diagnostics | `tools/graphify_auto_refresh.py --once` + impact packets | 主/审计图刷新成功，变更边可解释 | missing |

## Missing Coverage

- Remaining proof: independent final review and the first Stage 6 Graphify baseline remain separate gates.
- Suspected stale proof: existing `platform.h` callers and `ma_*` ownership require ledger review; static matches do not prove semantic I/O absence.
- Human-only verification: two-person PAL review; later UI/audio subjective checks and any real device/host workflow.
- 待产品确认: any behavior change or improved error wording stays in the private product-review queue; no semantic change is authorized by this contract.

## Change Gate

- [ ] Refresh Graphify main and audit graphs with `py -3 tools/graphify_auto_refresh.py --once`.
- [ ] Run `py -3 tools/behavior_contract_check.py`.
- [ ] Run `py -3 tools/platform_boundary_check.py --check-ledger` against the frozen Stage 6 platform migration ledger.
- [ ] Run the platform-services TDD/QA proof and the frozen performance matrix.
- [ ] Review module-relevant `UNKNOWN_BLOCKING` entries.
- [ ] Obtain two independent review records before S6-02.

## Decision Log

| Date | Decision | Reason | Decider |
| --- | --- | --- | --- |
| 2026-07-15 | Freeze approved five-probe r3 with population sigma | Keep the 16ms product promise distinct from the 3.802742ms empirical regression limit | User / Engineering |
| 2026-07-14 | 采用隔离优先、能力切片 PAL | 保持现有 Windows 行为并控制迁移爆炸半径 | 用户 / Engineering |
| 2026-07-14 | ImageCodec 独立为 PAL 能力 | CoverModule 的 WIC 解码/编码不能遗漏 | Engineering review |
| 2026-07-14 | `platform.h` 在 S6-08 删除 | 让阶段出口拥有唯一、可验证的能力头终态 | 用户批准设计 |

## macOS 原生入口与共享运行服务（CV-521D03CB）

- Cocoa/OpenGL 入口调用真实的 RenderUI；不再编译用于替代搜索刷新和主界面的 main_stubs_macos.cpp。
- AppRuntime 共用 Windows 的音频/设置初始化、异步切库与音频加载轮询、后台任务等待和资源关闭顺序。字体和点击节流也由两端共同使用。
- 首启没有素材库时呈现原有建库界面。默认建库目录来自 HarkenUserData，失败不能改到程序目录或当前工作目录。读取设置后仅选择实际存在、可用的已知素材库。
- 窗口服务只接受本进程当前存活窗口，关闭通过原生窗口事件执行。退出先停止计时器，再等待工作线程、关闭数据库和音频，保存设置后释放界面及图形资源；设置保存失败须记录并返回非零退出码。
- Mac 外部文件拖放、全局唤醒快捷键和音频工作站连接尚未接入，必须明确显示不支持；不能用伪成功替代。Windows 对应实现保留。
- 仅诊断且编译期隔离的版本接受 --mac-qa-smoke：运行真实主界面，读取实际帧缓冲保存截图，随后走正常关闭。正式配置不接受该自动退出参数。
- tools/macos_runtime_smoke.py --app <绝对可执行文件路径> --qa-root <编译期隔离目录> --out <新的证据目录> 自动验证启动、字号跨进程恢复、真实只读目录导致的保存失败及恢复。它不移动系统鼠标，保存每次退出码、原始输出、截图及生命周期事件。
- 这项入口验证不等于扫描、组合筛选、试听和波形全链路已通过；那些结果须由后续功能验证单独给出。
