# Pro Tools (PTSL) 交付链路 · 环境与验收说明

> **本文件的内容由需求侦察官整理，待交付方/开发官提交到仓库 `docs/operations/ptsl-environment.md`。**
> 在正式落库前，本副本只作草稿；卡面引用应以仓库路径为准。
> 最后核实：2026-11-28（Windows 本机，Pro Tools 未安装/未激活）

## 1. 这份文件解决什么

说明 Harken Search 的 Pro Tools 落位（spot）能力**当前处于什么状态**、要满足什么条件才能验证、以及验证时用什么证据判定通过。
目的是避免有人把它当成"从零开发"来估工作量，也避免用"编译通过"冒充"落位可用"。

## 2. 已核实的实现现状

| 项 | 事实 |
|---|---|
| 客户端实现 | `src/HarkenSearchProToolsBridgeMain.cpp` 使用官方 `PTSLC_CPP` 客户端，含 staging → import、`SpotMode`、typed response 校验 |
| 进程结构 | 主程序 **spawn 子进程** `HarkenSearchProToolsBridge.exe`（见 `src/ProToolsBridge.cpp`），能否落位取决于该 bridge 的构建配置 |
| 功能开关 | 不是普通 CMake 选项：只有解析到 SDK 时才会定义 `AURO_PROTOOLS_PTSL_ENABLED`——需传入 `-DAURO_PROTOOLS_PTSL_SDK_ROOT=<路径>`，或 drop 目录同时存在 `PTSLC_CPP.lib` 与 `include/PTSLC_CPP/CppPTSLClient.h` |
| 打包开关 | `AURO_BUNDLE_AVID_PTSL` **只控制是否把 `PTSLC_CPP.dll` 拷进输出/包**，不控制功能开关；发布配置默认 OFF |
| 链接方式 | **链接期导入**（链接 `.lib`），不是 `LoadLibrary` 延迟加载 → 把 DLL 放到 exe 旁边**不会**激活功能 |
| 现有产物 | 仓库内各个构建目录的 `HarkenSearchProToolsBridge.exe` 均为**未启用 PTSL** 的构建（`AURO_PROTOOLS_PTSL_SDK_ROOT` 为空） |
| 文档中的字符串 | `src/ProToolsBridge.h` 的 `kPtslClientDllName` 常量会把 `PTSLC_CPP.dll` 字样编进 exe；**不能据此推断运行时加载** |

## 3. 缺什么

- 本机存在 `C:\Program Files\Avid\Pro Tools\PTSL.dll`——这是 **Pro Tools 侧服务端**运行库，**不是**客户端 SDK。
- 客户端所需的 `PTSLC_CPP.dll` / `PTSLC_CPP.lib` / `include/PTSLC_CPP/CppPTSLClient.h` **在仓库与本机均不存在**。
- 获取途径：`https://developer.avid.com/scripting/` → Download Toolkit → 接受 click-through 许可（**免费**，只需 Avid 账号；不需要伙伴计划或 NDA）→ 下载 SDK 包。
- **阻塞项**：Pro Tools 本机尚未安装/激活，因此 preflight 与落位误差**无法实测**。

## 4. 验证分三段（前置条件到哪段就做到哪段）

| 段 | 前置 | 交付证据 |
|---|---|---|
| ① 编译 | 拿到 SDK，`-DAURO_PROTOOLS_PTSL_SDK_ROOT=<路径>` | 必须贴 `dumpbin /dependents` **原始输出**证明 `PTSLC_CPP.dll` 出现在依赖里；只写"编译通过"不算 |
| ② preflight | Pro Tools 已安装、可启动、许可到位 | bridge 自身返回的 `insertedCount` / `resolvedTrackId/Name` / `perClipSampleOffsets` / `ptslCommandTrace` / `pathTaken`；"看起来落上了"不算 |
| ③ 回退 | 同 ②，另加断开 runtime 与关库两种情形 | 失败时有可读原因（不静默失败），回退路径按现有 AAF fallback 实测，不假设"写目录 + 导入" |

## 5. 判定口径

- 落点误差：同一素材多次落位，剪辑起点与光标偏差 ≤ 1 帧（48kHz），且必须落在目标轨道；出现错轨即判失败。
- handles：非零起点素材头尾余量保留（按素材实际余量，至少各 0.5s），落位后可回溯到原素材标识。
- 版本：实测到的 Pro Tools 版本要写清；文档支持的版本区间若未实测，一律写"未验证"，不得写成"支持"。
- 发布：`AURO_BUNDLE_AVID_PTSL` 保持 OFF；`PTSLC_CPP.dll` 不进任何发布包或安装器。

## 6. 已知边界与风险

- PTSL 是免费 SDK，但**把 DLL 打进发布包**属许可与分发范畴，需单独确认；这也是默认 OFF 的原因。
- 该链路依赖用户环境中的 Pro Tools 与 PTSL runtime，对外表述不宜写"开箱即用"。
- 本文件只记录到 2026-11-28；后续每次实测请更新本文件而非另起新文档。
