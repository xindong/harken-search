# CV-B80E0187 · Mac REAPER 默认本机端点

## 已核对来源

- 2026-11-10 的 origin/qa/current 为 cfdd64eec1e1115612b3286c4c7591fed5c24c11。Mac 候选 CV-97B559BE / b6589d5 已通过假宿主批量插入与同提交 Windows 回归，尚未验收或合入 QA；本卡在独立 clone 中叠加它。
- Windows 默认 REAPER 管道由 src/ReaperBridgeProtocol.h 声明；Windows 宿主扩展与客户端默认使用相同管道。Mac 的 src/platform/named_pipe_macos.mm 仅接受绝对路径，默认 Windows 管道名返回 Unsupported；Mac 扩展仅在 HARKEN_REAPER_BRIDGE_TEST_SOCKET 显式存在时启动。
- Mac 本机没有 REAPER.app。src/ReaperBridgeDoctor.cpp 仍使用 reaper.exe、.dll 和 PE 探针，故本卡完成后不能宣称产品诊断、安装或工作站直送已可用。

## 本卡范围与模块

1. 增加 Mac 端点路径小模块：从系统同用户私有临时目录推导固定短 socket 路径；只接受绝对、非软链接、当前用户拥有且拒绝其他用户访问的父目录，路径须符合 Unix socket 长度限制。不创建或清理目录，不接触日常素材库。
2. Mac 客户端把默认 Windows 管道名映射到上述 Mac 路径；显式绝对 QA 路径行为保持不变，其他相对路径仍拒绝。Mac 宿主扩展无 QA 覆盖时选择相同默认路径，有 QA 覆盖时仍只用隔离路径。已有 socket 不覆盖，卸载只清理本进程创建的端点。
3. 用隔离假宿主与测试根目录验证路径算法、危险目录拒绝、长度限制、默认/显式路径选择、原有批量插入与卸载；完整 Mac 产品构建、入口运行，Windows 同 SHA 构建与 REAPER 合约回归。

## 输出、失败和验证

| 模块 | 输入与输出 | 失败行为 | 验证 |
| --- | --- | --- | --- |
| Mac 端点路径 | 系统私有临时目录 → 固定 socket 路径 | 无效、过长、非私有目录返回空路径 | 新增纯路径单测与真实系统只读检查 |
| Mac 客户端 named_pipe | 默认名字/显式 QA 路径 → Unix socket 目标 | 不安全或不存在的端点仍失败，不误报成功 | 扩充本机 IPC 测试 |
| Mac REAPER 扩展 | QA 覆盖或默认路径 → 自有监听 socket | 不覆盖既有文件；加载失败后不留下 socket | 扩充真实 dylib 假宿主测试 |
| 双端产品 | 本卡最终提交 → 构建/运行/回归证据 | 失败保留原始日志，不推进验收 | Mac 隔离启动退出、Windows 同 SHA CI |

## 明确未覆盖

本卡不安装 REAPER、不改产品诊断或 UI、不放开安装/更新流程，也不声称真实轨道插入。默认连接的产品诊断与安装、真实 REAPER 工程另卡验证。界面和 Windows 共用需求没有新设计。
