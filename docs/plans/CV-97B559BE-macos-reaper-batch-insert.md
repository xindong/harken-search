# CV-97B559BE · Mac REAPER 批量插轨与失败回滚

## 已核对来源

- 2026-11-10 的 origin/qa/current 为 cfdd64eec1e1115612b3286c4c7591fed5c24c11。Mac 前置候选 CV-96F34299 / 958bb39 已通过双端技术验证，但尚未验收或合入 QA；本卡在独立 clone 叠加它。
- Windows 需求与实现：docs/behavior/modules/editor-handoff.md、reaper_extension/reaper_harken.cpp、tests/test_reaper_bridge_contract.cpp。Windows 插入在 REAPER timer 回调主线程执行，校正插入位置，失败时撤销本批新增素材和轨道并还原编辑指针。
- 共用协议与摆位：src/ReaperBridgeProtocol.h、src/ReaperBridgeClient.cpp。Mac 当前只返回“插轨未实现”；本机没有 REAPER 实机。
- timer 注册/注销用法参考 REAPER 扩展开源项目 SWS：reaper-oss/sws 的 cfillion/preview.cpp；实际函数布局来自仓库已有 REAPER SDK 头文件。

## 本卡范围

1. 在 Mac 扩展中加载已核对的必需宿主 API，只对显式隔离 QA socket 开放；完成握手时只有 timer 注册和服务线程均成功才报告可操作。
2. Unix socket 线程只解析请求并入队，不能调用 REAPER API；宿主 timer 回调在主线程处理一个批次。先验证所有文件存在且为普通文件；按编辑指针串行插入，使用共用 EvaluatePlacement 计算摆位及校正。
3. 任一文件插入、定位或校正失败时，删除本批新增素材/轨道并恢复原指针；返回失败，不报部分成功。卸载注销 timer、停服务、取消未处理请求，只清理自己创建的 socket。
4. 独立假宿主用真实 .dylib、隔离 WAV、真实 socket 和客户端验证：中文路径、两文件顺序、主线程调用、失败回滚、无 timer 时请求超时取消、卸载安全、错误协议与不可信 socket。保留原始失败和修正日志。

## 模块和验证

| 模块 | 输入/输出 | 失败处理 | 验证 |
| --- | --- | --- | --- |
| reaper_extension/reaper_harken_macos.cpp | REAPER 函数表 + socket 批次 → 主线程新增轨道素材与成功回执 | 缺 API/注册失败拒绝加载；超时前取消未开始请求；插入中失败回滚 | 新增假宿主集成测试 |
| tests/test_mac_reaper_host_handshake.cpp | 原隔离加载/协议门禁 → 兼容新握手语义 | 未实现插入的旧断言改为真实文件失败与安全退出 | 原测试继续通过 |
| tests/test_mac_reaper_batch_insert.cpp | 假宿主素材/轨道模型 + 隔离 WAV → 可重复成功与回滚 | 只写 QA 目录；无真实用户工程 | CTest 实际执行数与逐条断言 |
| Mac 产品 | 本卡提交的完整应用 → 启动/退出/设置重开 | 原始失败保留 | 完整构建、隔离运行、截图 |
| Windows | 同一提交 → 产品/REAPER 合约回归 | 未通过则不推进验收 | GitHub Actions 实际 checkout SHA 和日志 |

## 明确未覆盖

本卡不安装扩展，不启用默认生产端点，不修改产品 UI。没有 REAPER 实机，因此假宿主成功不能写成真实 REAPER 轨道插入已验证；产品直送、安装/升级与真实工程检查随后独立推进。Mac 与 Windows 核心素材库共用；本卡仅修改 Mac 宿主平台扩展及对应测试/构建登记。
