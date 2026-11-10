# CV-5AF44A02 · Mac REAPER 宿主扩展隔离握手

## 来源、基线和范围

2026-11-10 读取远端 `origin/qa/current` cfdd64eec1e1115612b3286c4c7591fed5c24c11、Mac 差异卡 CV-AB9FEDDA、通信卡 CV-5B213257/0d59e645、仓库 `docs/behavior/modules/editor-handoff.md` 和 Windows `reaper_extension/reaper_harken.cpp`。Windows 使用自有扩展接收命名管道消息并在 REAPER 主线程插入媒体；Mac 仅有经过隔离验证的显式 Unix socket 客户端。当前 Mac 常见应用目录未找到 REAPER，不能做真实轨道验收。[官方 REAPER SDK 的 `sdk/reaper_plugin.h`](https://github.com/justinfrankel/reaper-sdk/blob/main/sdk/reaper_plugin.h) 定义跨平台 `ReaperPluginEntry`、`reaper_plugin_info_t` 的字段顺序和 macOS 导出方式；本卡使用必要的最小 ABI 声明并保留来源与许可说明，不复制整份 SDK。

本卡独立 clone 为 `cards/CV-5AF44A02`，分支 `task/CV-5AF44A02`，从最新 QA 起步；为完整 Mac 产品验证，可合入已验证但尚未用户验收的 CV-5B213257 候选，依赖与合并 SHA 必须记录。Windows 正式 QA、main、用户日常数据均不改。

## 模块与可独立验证的行为

1. **Mac 扩展装载。** 新增 APPLE 专属扩展目标与 `ReaperPluginEntry` 必需 ABI。输入为宿主提供的 `reaper_plugin_info_t` 和仅用于本卡隔离测试的绝对 Unix socket 路径；无路径或 ABI 不合时返回加载失败。输出为 `reaper_harken.dylib` 和明确的加载/卸载结果。Windows DLL 源码、目标和现有行为不变。
2. **隔离宿主通信。** 扩展只在同一用户且受限权限的测试 endpoint 上接受单条有界消息，使用共用 REAPER 协议处理 `HELLO` 和 `PING`。协议、架构或 manifest 不匹配时返回对应错误；`INSERT_BATCH` 一律返回明确的未实现错误，不触发宿主 API，也不返回虚假成功。断开、超长消息、占用/非 socket 路径和卸载分别保留可判定失败。
3. **卸载与资源所有权。** 停止后台接收并关闭连接，只清理本扩展创建且仍由它持有的 endpoint。不得删除其它实例或用户创建的文件。真实 REAPER 插入与默认生产 endpoint 的安全安装、发现和开启另卡处理。
4. **验证。** 用本卡 QA 目录的隔离数据和短 socket 路径，`dlopen` 实际产出的扩展，假 REAPER 信息结构实测装载、握手、错误和卸载；再配置并构建完整 Mac 产品，运行定向 CTest 和启动退出。共用构建/协议若变更，推送合并 SHA 后取得同 SHA Windows 产品与 REAPER 合同回归，核对实际 checkout、测试数量及原始日志。没有真实 REAPER 宿主就明记未实测，不推进用户验收、QA 合入或对外发布。

## 结果记录

实际执行命令、退出码、失败日志、测试数量和包路径集中在仓库外 `qa/CV-5AF44A02/README.md`。本计划不预填通过。
