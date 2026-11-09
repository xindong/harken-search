# CV-5B213257 · Mac REAPER 本机通信与隔离握手

## 来源和当前事实

2026-11-10 读取 GitHub 最新 qa/current cfdd64eec1e1115612b3286c4c7591fed5c24c11、Mac Cuevi 差异盘点 CV-AB9FEDDA 和 Windows REAPER 扩展源码。Windows 的 REAPER 批量直送使用项目自有扩展和命名管道；Mac 的 Platform::NamedPipe 当前返回 Unsupported，现有扩展也只构建为 Windows DLL。Mac 的 GetDawCapability 和普通发送入口继续明确报告不可用。

独立 clone：cards/CV-5B213257；分支：task/CV-5B213257。正式 Cuevi 卡和负责会话已登记。本卡不接管 FileList 导入卡 CV-A9D5D187，也不把它的测试当作 REAPER 轨道直送证据。

## 本卡范围

只给 Mac 的现有消息交换接口加入明确指定绝对路径的 Unix domain socket 客户端；保持 Windows 实现和 REAPER 消息格式不变。该路径用于隔离假宿主测试，不能把 Windows 默认管道名映射到生产 socket，也不放开当前 Mac 工作站能力标签或发送按钮。与 Mac 无关的 ExchangeMessageWithOptions 和服务端会话继续返回 Unsupported。

输入：绝对 socket 路径、长度受限的 HELLO/INSERT 消息、超时。输出：现有平台状态、系统错误码和完整响应。拒绝相对路径、符号链接、非 socket 文件、非当前用户 socket 与不安全权限；连接、写入、等待和读取分别保留可判定错误。单次交换有总时限与消息大小上限，返回的成功只代表假宿主回执格式正确，不代表 REAPER 实际轨道已插入。

## 验证和后续边界

先用卡片专用临时目录内的真实 Unix socket 假宿主做红绿测试：握手、批量插入回执、拒绝不安全路径、连接失败、超时和坏响应。构建 Mac 定向测试与完整产品，运行平台边界检查；记录命令、退出码和原始日志。改动只在 APPLE 源码和 APPLE 条件测试目标，Windows 产品代码不变；若后续修改共用代码，另取同提交 Windows 回归。

本卡不实现 REAPER macOS 扩展、安装器、默认端点映射或实际轨道插入。那些须另卡复用现有协议与插入算法，在隔离 REAPER 宿主中测真实发送；在此之前 Mac 工作站 UI 必须继续诚实显示不可用。本卡没有界面变化，不生成伪造的宿主截图，也不进入 QA 验收队列。

## 本轮执行记录（2026-11-10）

- 先写 10 项独立 socket 假宿主检查；原 Mac 占位实现实际结果为 2 通过、8 失败，见 qa/CV-5B213257/logs/001-red-mac-ipc/report.json。
- Mac 平台实现仅接受显式绝对 socket 路径；验证 socket 类型、当前用户与权限，连接后复核对端用户，以一个总截止时间完成非阻塞连接、写入和按行读取。Windows 默认管道名、兼容选项及服务端仍返回 Unsupported。手工编译后 10/10 检查通过；CMake 定向测试 1/1、再加现有 REAPER 平台合同为 2/2，Stage6 平台边界 439/439。
- 首次 CMake 配置因 Windows 专属 TestReaperBridgeContract 未受 WIN32 条件保护而失败；已仅给该 Windows 目标补平台条件，第二次配置通过。第一次目标构建被系统 Python 3.9 的本地化生成器兼容问题拦住；显式使用既有 Python 3.13 后重新配置、构建和 CTest 通过。原始失败日志均保留。
- 基于最新 QA 的 Mac 完整产品构建在既有 AppState.cpp 的 _TRUNCATE 未定义处失败，本卡没有扩到应用状态模块。故本轮无完整应用包、实际 REAPER 插入或用户验收。Windows 同提交回归仍待提交后执行。
- 仓库要求的 Graphify --once 已运行；它没有解析新 Objective-C++ 文件的调用点，C++ 客户端仍有 7 个 UNKNOWN_BLOCKING 与 3 个 POSSIBLE_DYNAMIC 静态分类。本卡通过源码追踪和真实 Unix socket 测试限定结论，不声称已取得全项目调用图覆盖；小型复核回执见 qa/CV-5B213257/graphify-reaper-review.json。
- Windows 相关回归：公共检查流水线加入原有 TestReaperBridgeContract 的构建和执行，在本卡提交上验证 WIN32 条件仍保留 Windows REAPER 扩展与合同测试。实际 CI 结果必须回读 checkout SHA、产品构建和该测试数量后填写；Mac 的成功不能代替此项。
