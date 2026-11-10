# CV-96F34299 · Mac REAPER 宿主必需 API 就绪门禁

## 来源与当前差异

- 基线：2026-11-10 核对 origin/qa/current cfdd64eec1e1115612b3286c4c7591fed5c24c11。
- Windows 行为来源：reaper_extension/reaper_harken.cpp 的 LoadReaperAPI；它在建立通信服务前检查插轨、光标、素材和轨道相关函数。
- Mac 前置候选：CV-5AF44A02 / d6cc189，已有独立扩展与 QA socket，但仅检查 REAPER ABI 版本和 GetFunc 非空，没有检查必需函数是否存在。该候选尚未验收或合入 QA。
- 共同产品行为：docs/behavior/modules/editor-handoff.md 要求工作站路径不能虚报可用。本卡只修宿主扩展加载门禁，不接通产品直送。

## 本卡范围

1. 在本卡独立 clone 的 task/CV-96F34299 分支叠加前置候选；不改 source 主目录或 Windows 扩展实现。
2. Mac 扩展加载时，通过 GetFunc 核对与 Windows 相同的必需函数集合。任意必需函数缺失时 ReaperPluginEntry 返回失败，不创建 socket；存在全部函数时保持上一卡的显式 QA socket 握手及“插轨未实现”错误。可选的 UI/Undo 函数不作为加载门禁。
3. 假 REAPER ABI 测试逐项覆盖缺失函数、错误版本、无端点、安全 socket、客户端未误报 Ready、卸载后端点移除；测试不调用 REAPER API，不改工程或素材。

## 模块、失败处理和验证

| 模块 | 输入与输出 | 失败处理 | 验证 |
| --- | --- | --- | --- |
| reaper_extension/reaper_harken_macos.cpp | host->GetFunc 函数查询 → 扩展可加载状态 | 必需函数缺失立即失败，不启动 socket | 真实 dylib + 假宿主加载测试 |
| tests/test_mac_reaper_host_handshake.cpp | 可控函数表、显式 QA socket → 断言 | 每个缺项独立断言；已有握手、卸载、安全性不断言为空 | Mac CTest、直接断言数 |
| Mac 完整应用 | 本卡提交的隔离构建 → 正常启动与退出 | 原始失败日志保留 | 完整产品构建与 QA 数据目录运行 |
| Windows 共用构建 | 同提交 Windows CI | 构建/测试失败不推进验收 | 产品构建及 REAPER 合约相关测试，核对 checkout SHA |

## 明确未完成

本卡不注册生产端点、不调用 InsertMedia、不安装扩展、不宣称在真实 REAPER 中插轨。Mac 本机尚未找到 REAPER；真实宿主测试、插轨算法和应用接线另卡。用户验收与 QA 整合遵守当前工作流。
