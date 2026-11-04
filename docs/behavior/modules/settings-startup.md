# Settings / Startup Behavior Contract

## Header

**ID:** BDD-SETTINGS-STARTUP
**Module:** Settings / Startup
**Status:** Active
**Priority:** P1
**Owner:** Engineering
**Created:** 2026-06-03
**Last Updated:** 2026-11-04

## Product Behavior

**User promise:** Aris starts predictably, preserves safe settings, and does not perform surprising expensive or network-like work before the user can operate.
**Primary workflow:** User launches Aris, loads recent libraries/settings, and expects startup reconciliation and feature defaults to be safe.
**Non-goals:** This contract does not define full installer/update behavior.

## 中文语义

**用户承诺：** 无论首次启动、正常启动还是最近库丢失，Aris 都能进入可理解、可操作的安全状态，不会偷偷清理用户数据或启动昂贵后台工作。Mac 用户导出设置时可以自行选择备份位置；取消不改变设置。Mac 导入主题 JSON 时可以选取文件，取消不改当前主题。Mac 的系统字号与列表字号分别影响各自的文字。
**核心流程：** 用户启动 Aris；应用加载设置、edition flag 和最近库列表；缺失库以明确的受保护状态显示，用户再决定是否重连、移除或重新扫描。Mac 导出设置时先选目标，确认后保存并复制当前配置，取消直接返回。主题编辑器导入 JSON 后沿用现有预览或新建主题流程。Mac 首次加载字体时使用各自字号；设置变化后在下一帧开始前刷新字体，退出重开仍保留两项值。
**非目标：** 这份契约不定义完整安装器、自动更新流程或跨版本迁移之外的产品授权策略。

## BDD Scenarios

### Happy Path

- Given 已满足以下前置条件：a valid settings file and recent library
- When 执行以下操作：Aris starts
- Then 应得到以下结果：default settings, edition flags, and startup reconciliation produce a usable UI state
- Given 已满足以下前置条件：the Windows WebView2 startup shell is shown
- When 执行以下操作：Aris enters the startup gate on a normal launch
- Then 应得到以下结果：the legacy login form is replaced by the welcome surface, and continuing from that surface leads directly into loading
- Given Windows 启动欢迎页或后续加载页显示左上角品牌区
- When 官方横版 Logo 文件随程序存在
- Then 两页显示同一份 HARKEN search 字标，保留原有位置；图片缺失时仍显示 Harken Search 文字
- Given 已满足以下前置条件：Browser mode stores user workflow state such as recursive child-audio scope
- When 执行以下操作：settings are saved and restored on the next launch
- Then 应得到以下结果：Aris restores that scope truthfully instead of silently resetting to current-directory mode
- Given Mac 用户设置目录中有一份自定义词库
- When 设置页显示词库位置并执行“重置为默认”
- Then 显示实际 harken_thesaurus.json 路径，只重置该用户文件；重开后仍为内置规则，Windows 程序目录与旧词库迁移行为不变

- Given Mac 用户在设置中选择导出备份
- When 用户在系统保存面板确认一个可写目标
- Then 当前设置先保存到原设置文件，再复制到用户选定的 ini 文件，并显示实际位置；Windows 原有导出目标保持不变
- Given Mac 用户在系统保存面板取消
- When 面板返回空路径
- Then 不保存或复制文件，也不报告一次新的导出失败
- Given Mac 用户在主题编辑器选择导入 JSON 或导入并创建主题
- When 系统文件面板返回可读取的主题 JSON
- Then 沿用现有主题解析、预览或创建流程；取消时不改主题，读取失败显示原有失败提示；无有效主题字段的内容仍需另卡验证

- Given Mac 用户在主题编辑器导出当前草稿
- When 用户在系统保存面板确认一个 JSON 路径
- Then 剪贴板仍包含主题 JSON，文件保存相同主题；取消只保留剪贴板内容，写入失败不得报告文件保存成功
- Given Mac 用户在主题编辑器保存普通自定义主题
- When 正常退出并重新打开应用，或在设置中切回自定义主题
- Then `theme_custom.json` 保存在 Harken Search 用户设置目录，并从同一路径恢复配色；Windows 仍使用原应用目录文件

- Given Mac 用户将系统字号与列表字号设为不同值
- When 首次打开素材库、运行中改变系统字号，或退出后重开
- Then 阅读区与结果列表按各自字号绘制；改变系统字号不顺带改变列表文字，改变列表字号不顺带改变阅读区文字；重开后仍按保存值显示

### Edge Cases

- Given 已满足以下前置条件：no settings file exists because the user has never run Aris before
- When 执行以下操作：startup runs
- Then 应得到以下结果：Aris creates safe in-memory defaults, shows an empty/no-library state, and waits for explicit user library selection before scan/reconcile work
- Given 已满足以下前置条件：missing DB paths, disabled auto-rescan, or first-run defaults
- When 执行以下操作：startup runs
- Then 应得到以下结果：expensive reconcile paths remain opt-in and missing libraries are shown as unavailable/guarded entries instead of being silently removed
- Given 已满足以下前置条件：the user switches between built-in stock themes or saves a custom theme from the theme editor
- When 执行以下操作：settings are saved and Aris applies the theme on the next frame or next launch
- Then 应得到以下结果：stock themes remain stock selections instead of being marked customized, and the saved custom theme name remains available so `theme_custom.json` can be reloaded.
- Given 已满足以下前置条件：a built-in stock theme uses token chrome on toolbar/transport surfaces
- When 执行以下操作：the stock theme is applied without using the custom-theme flag
- Then 应得到以下结果：toolbar and transport token chrome remains selected by explicit stock-theme identity instead of depending on `ThemeColors::customized()`.
- Given Mac 用户设置目录只读、不安全或超出隔离测试根
- When 保存普通自定义主题
- Then 保存失败且不切换到新主题，不改写应用包或日常目录

### Failure / Recovery

- Given 已满足以下前置条件：settings are missing, encrypted values fail, or a DB path is unavailable
- When 执行以下操作：startup loads configuration
- Then 应得到以下结果：Aris falls back safely without silent destructive cleanup
- Given 已选中的自定义主题文件语法错误、没有受支持的顶层颜色字段，或颜色值格式错误
- When 启动或设置切换时读取该主题文件
- Then 解析失败且不改变传入的颜色或改写文件；启动使用安全回退，不把坏文件标成已加载的自定义主题。有效的旧版部分主题仍可加载。
- Given Mac 词库保存目录只读或不可用
- When 用户执行词库“重置为默认”
- Then 保留失败状态并显示与用户设置目录相关的提示，不把程序包目录当成词库位置

### Non-Functional

- Given 已满足以下前置条件：cold startup
- When 执行以下操作：initialization runs
- Then 应得到以下结果：startup does not start full scan, cover-wall warmup, waveform backfill, or network-like work before UI readiness unless the setting explicitly opts in, and it records diagnostics for those state decisions.

## Interface Binding

| Interface / Entry | Kind | Source | Expected behavior |
| --- | --- | --- | --- |
| `AppSettings` defaults | API | tests | Safe default values |
| `theme` / `customThemeName` | UI | source scan + tests | Stock theme identity, stock token chrome, and saved custom theme availability persist across save/load/startup |
| Browser scope settings | API | source scan + tests | Browser recursive/current-dir scope survives save/load without drift |
| startup welcome entry | API | source-contract tests + manual QA | the startup entry surface is the welcome page and continues directly into loading |
| startup brand wordmark | UI | source-contract test + manual QA | welcome and loading use the same bundled official logo, with text fallback when the file is absent |
| startup reconciliation | API | tests | Missing DB/library state handled safely |
| product edition flags | API | tests | Edition-gated defaults remain stable |
| settings encryption | API | tests | Sensitive settings are protected |
| startup diagnostics | diagnostics | QA protocol | Runtime state decisions are visible |
| Mac 词库位置与重置 | UI | SettingsThesaurusTools.inl, main.cpp | 页面、重置和持久化都指向 activePath；Windows 旧逻辑保留 |
| Mac settings backup export | UI | SettingsActions and Mac settings export dialog | Uses a user-selected UTF-8 path, keeps cancel side-effect free and preserves the Windows branch |
| Mac theme JSON export | UI | ThemeEditor and Mac theme save dialog | Saves the draft through the shared serializer to the selected UTF-8 path; cancel leaves the clipboard copy available |
| Mac custom theme file | UI | ThemeEditor, SettingsDialog and Cocoa startup | Resolves the same user settings file for save, switch and reopen; fails closed on an unsafe directory |
| `OpenThemeJsonFileDialogUtf8()` | API | src/platform/theme_json_dialogs_macos.mm | Mac returns a selected UTF-8 file path to the existing theme import actions; cancel returns empty |
| Mac independent font sizes | UI | src/platform/app_entry_macos.mm and shared UiFonts | Initial font atlas and later refresh use separate reading/list settings before the next ImGui frame |

## Graphify Binding

**Source graph:** `graphify-out/main/interface-call-graph.json` and `graphify-out/main/audit-call-graph.json`
**Impact target(s):** `AppSettings`, startup reconciliation helpers, product edition helpers
**Audit classification requirement:** startup and settings unknowns must be reviewed before completion because they affect every session.
**Dynamic calls:** review startup container/config paths.
**Unknown calls:** review settings/startup entries in `UNKNOWN_CALLS.md`.

## TDD / QA Proof

| Proof ID | Kind | Artifact / command / event | Pass signal | Status |
| --- | --- | --- | --- | --- |
| TEST-APP-SETTINGS | unit | `TestAppSettingsDefaults` | defaults and cached matching pass | active |
| TEST-STARTUP-RECONCILE | integration | `TestStartupReconciliation` | startup/missing DB behavior passes | active |
| TEST-PRODUCT-EDITION | unit | `TestProductEdition` | edition gating passes | active |
| TEST-SETTINGS-ENCRYPTION | unit | `TestSettingsEncryption` | encryption behavior passes | active |
| TEST-OUTPUT-DEVICE-COMBO | unit | `TestOutputDeviceComboItem` | identical-name output devices share one display label but keep distinct ImGui `##` Selectable IDs; combo strings stay stable after `c_str()` | active |
| DIAG-STARTUP | diagnostics | `TestStartupReconciliation` startup source-contract assertions | startup branch diagnostics markers present | active |
| TEST-STARTUP-WELCOME | source-contract | `TestStartupReconciliation` | welcome entry replaces the legacy login form and continues into loading | active |
| TEST-THEME-PERSISTENCE | source-contract | source scan + `TestThemeColors` | stock theme application does not set `ThemeColors::customized()`, stock token chrome is keyed by stock theme identity, and custom theme name is written/read by settings IO | active |
| TEST-MAC-THEME-EXPORT | platform-contract | `TestThemeJsonExportMacBoundary` + `TestThemeColors` | Mac save panel boundary and shared JSON serializer are checked separately | active |
| TEST-MAC-CUSTOM-THEME-PATH | integration | `TestMacUserDataLayout` + isolated Cocoa product restart | path, unsafe/read-only failure, loaded colors and normal exit are checked in a separate QA profile | active |
| TEST-MAC-THEME-IMPORT | source-contract / runtime | `tests/test_theme_json_import_macos_boundary.py` and isolated Mac chooser probe | Both import buttons reach the native chooser; cancel and selected UTF-8 path behave correctly | active |
| TEST-MAC-INDEPENDENT-FONTS | runtime | Isolated Cocoa product with a three-row WAV library, two opposite font-size settings, screenshots, normal exit and reopen | List glyph size follows listFontSize while reading surfaces follow readingFontSize; live reading change refreshes the atlas before the next frame | active |

## Missing Coverage

- Mac 词库重置的源码绑定由 test_thesaurus_json_dialog_boundary.py 检查；隔离实际应用由 macos_thesaurus_reset_smoke.py 验证重置文件与重开。正向文件面板点选和用户界面确认仍需单独验收。

- Mac 保存面板的正向人工点选与导出文件回读仍需实际界面验收；原生面板取消、应用重开和源码边界测试不能代替它。
- Mac 主题编辑器导出的正向界面点选和文件回读也需使用本卡实际包确认；面板探针不能代替用户验收。
- Mac 自定义主题在编辑器中的真实点选与设置切换仍需实际界面验证；隔离配置注入的重启测试只证明产品读取和绘制，不证明点选流程。
- Mac 两项字号的隔离配置与诊断指令能证明真实绘制、运行中刷新和重开；设置界面的鼠标点选与用户视觉确认仍需单独验收。
- Source-contract coverage now asserts the first-run no-database startup branch and CTA hook; full GUI smoke for perceived onboarding clarity remains human-only.
- Welcome-entry visual polish and screenshot proof remain human-only even when source-contract coverage is green.
- Residual gap: migration from older settings payloads is not yet asserted with fixture upgrades.
- Human-only verification: perceived startup clarity and button prominence.

## Change Gate

- [x] Refresh Graphify main and audit graphs with `py tools/graphify_auto_refresh.py --once`.
- [x] Re-run startup/settings tests.
- [x] Review settings/startup `UNKNOWN_BLOCKING` entries.
- [x] Update this contract if startup defaults or reconciliation semantics change.

## Decision Log

| Date | Decision | Reason | Decider |
| --- | --- | --- | --- |
| 2026-06-03 | Added first-run and guarded missing-library startup semantics. | Review found first startup and visible missing-library behavior were not concrete enough. | Engineering |
| 2026-06-10 | Replaced implicit default-database startup fallback with an explicit no-database empty state and CTA reuse contract. | Product requirement now requires first-run users to consciously create or open a database before library work begins. | Engineering |
| 2026-06-24 | Added theme identity persistence semantics. | User reported stock theme single/double-click token drift and custom token colors not persisting; settings must preserve custom theme name while stock application must not masquerade as customized. | Engineering |
| 2026-06-25 | Kept stock token chrome explicit after clearing the custom flag. | Dark/LapisBlue previously reached toolbar/transport token chrome through the erroneous customized flag; the state fix must not silently switch them to fallback chrome. | Engineering |

## 第8批异常退出启动弹窗关闭

上次没正常关掉再打开，直接进入工作界面，不先弹异常退出报告窗。崩溃检测和既有反馈能力保留。这不是崩溃系统重写，也不增加新提示。

### 五项所有权

- identity：隔离 settingsDir 下的 aris_sentinel.lock、crashDetectedOnStartup，以及既有 showCrashReportDialog 字段。身份不是生产崩溃文件，也不是进程号。
- generation：WindowsMain 仍检测哨兵并记下标志，同时为本次会话写哨兵。界面当帧只把 crashDetectedOnStartup 清成 false。正常退出仍删除哨兵，不再打开启动报告窗。
- cancel：没有新的异步任务。清掉启动标志只取消启动弹窗，不取消崩溃检测、哨兵写入或清理、MiniDump 或 SEH。
- stale-result：标志只消费一次。没有 future、ports 或跨帧 this。启动路径不能再凭 showCrashReportDialog 或旧崩溃文件把窗打开。
- 生命周期：WindowsMain 原有哨兵、MiniDump 和 SEH 寿命不变。标志在界面线程消费。帮助菜单里的反馈提交能力保留，启动路径不再启动那次发送。能力还在，不等于反馈凭据已经启用，也不等于网络发送已经证明。

### 七个保护键

Crash_DlgTitle、Main_CrashTitleText、Main_CrashReportNote、Crash_SendReport、Fb_CrashReportTitle、Main_AttachDiagLog500、Main_DismissBtn 仍留在账本里，并列入生成器保护名单。不删除文案，也不新增可见文案或语言键。

### 验证分类

源码合同证明启动窗不再 OpenPopup、不再模态、不再把 showCrashReportDialog 设为 true，并仍把 crashDetectedOnStartup 清成 false。保护键测试和旧反馈门只证明源码约束。产品编译只证明这批源码能编进隔离预览。这些都不是界面体验、真实异常退出后再开、人眼、听感、真实网络库或性能证明。

### 回退

只撤回本批启动窗、新增测试、CMake 注册、生成器里这七个保护键、本附录和台账两行。恢复到本批允许基线。不回退前面已经核收的批次，不改用户配置、数据库结构或迁移。
## 默认播放器关联（第12批）

本批把已交付的默认播放器关联修复搬回 745 旧布局。普通非隔离程序里，用户点「设为默认」会打开系统默认应用设置，让用户自己选格式；不再假写受保护 UserChoice。隔离预览里 Set/Remove 都不能打开系统设置。ProgId 仍是 Auro.AudioFile。这不是新播放器或新文件格式。

### 五项所有权

1. identity：兼容 ProgId、实际可执行路径、系统当次有效关联与用户主动点击。Capabilities 不是默认授权。
2. generation：一次同步登记/查询/点击，无新 worker。系统默认以当次 QueryEffectiveFileAssociation 为准。
3. cancel：745 取消清本 ProgId 的 UserChoice/Latest 路径保留。隔离时 Set/Remove 都不开系统设置。
4. stale-result：不把旧 UserChoice 冒充系统默认。源码迁移不等于用户电脑已关联。
5. 生命周期：Launch 在任何 COM/UI/OpenUrl 之前因 PreviewIsolationActive 返回 false。测试不执行真 HKCU。

### 危险测试不 RUN

TestDefaultAudioPlayerAssociation 直接改真 HKCU，可 BUILD 不可 RUN、不可 CTest。

### 回退

撤回本批 14 文件到基线，删除本批新建 Python 只读测试。

### 证据局限

已证源码合同与完整产品编译。未证 GUI、系统默认应用设置、注册表、E2E。
## 设置搜索高亮闪动可测性（第13批）

设置搜索、关键词高亮、切页和滚动定位已在 745。本批只把已交付的闪动公式抽成可传 elapsed 的纯函数，不改搜索产品。

### 五项所有权

1. identity：命中条目仍是已有 SettingsSearchEntry/cursor。纯公式不持身份、不写设置值。
2. generation：仍由查询/cursor 刷新 g_settingsSearchPulseT0。测试只传 elapsed，不注入全局时钟。
3. cancel：清空搜索仍是 hasSettingsSearch=false，无新任务。
4. stale-result：旧 T0 只影响闪动相位，不写业务值。
5. 生命周期：inline 纯公式无堆分配；OverlayLabelHighlight 仍在 UI 线程绘制。负 elapsed 与 >=1 为 0.38。

### 未证

不证 GUI/人眼/真机闪动感。测试可推进 elapsed。
