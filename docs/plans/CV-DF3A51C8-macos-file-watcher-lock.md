# CV-DF3A51C8 · Mac 文件监视回调死锁

## 基线与来源

- 2026-11-10，远端 qa/current 为 723d62f79c675dc915bd3b873f7c1698d50024ef；本卡独立 clone 与 task/CV-DF3A51C8 从该提交创建。正式 Mac Cuevi 卡和当前 Cindy 会话已登记。
- Windows 行为来源：仓库 docs/behavior/modules/platform-services.md 的增量文件监视约定，以及 Windows ReadDirectoryChangesW 对 FileAdded、FileModified、NeedRescan 的现有实现。此卡不改变 Windows 事件分类或数据库扫描、修剪逻辑。
- 源码现状：Mac FSEvents 的 processAppleEvents 持有 m_callbackMutex 后调用 coalesceAndEmit；后者立即发送或 flushCoalescedEvents 时再请求同一把非递归锁。收到文件事件时可能卡住监视线程。现有 Stage6 FileWatcherProbe 的重命名断言按 Windows 事件配对编写，不能直接作为 Mac 通过标准。
- 后续调用点核对：当前 src 中 startFileSystemWatcher 只有 Database 的声明与定义，没有产品调用方。这张卡只修平台组件，不声称应用已有实时增量入库；是否接通该功能须按 Windows 实际开放行为另卡确认。

## 范围和步骤

1. 在隔离目录构建小型 Mac 回调探针，合成一次 FSEvents Created/Modified/NeedRescan 事件，用独立进程超时保留修复前卡住的原始证据。探针不接触用户素材库。
2. 仅调整 Mac FSEvents 回调的锁边界；维持原有事件类型、路径和合并窗口语义。补充隔离回归，验证普通回调可完成、没有回调时安全返回、文件事件与 NeedRescan 均可送达。
3. 在同一提交构建完整 Mac 应用、运行组件级真实隔离目录变化和关闭测试，并对应用本身做启动/退出回归；记录事件数、超时与正常退出。共享文件改动需同提交 Windows 产品构建与相关回归，并核实流水线实际 SHA。
4. 把原始失败、修复后日志、运行截图如有、测试限制和路径集中到 qa/CV-DF3A51C8/README.md，回写正式 Cuevi。用户验收前不合并 QA。

## 模块边界与失败处理

| 模块 | 输入与输出 | 失败处理 | 独立验证 |
| --- | --- | --- | --- |
| Mac FSEvents 转换 | eventPaths/flags → FileEvent 回调 | 无回调直接返回；事件丢失标为 NeedRescan；不在回调里扫描数据库 | 合成事件顺序与 bounded timeout |
| 事件发送/合并 | FileEvent → 已登记的业务回调 | 不持有回调锁再次请求同锁；停止后不留挂起回调 | 实际目录创建与停止；重启另行验证 |
| 产品应用 | 当前未调用 startFileSystemWatcher | 不把组件测试称为自动入库；不在本卡擅自接线 | 应用启动/退出回归；自动增量入库未覆盖 |

未覆盖：产品自动增量入库、FSEvents 在每一种外接盘上的投递时间、用户日常素材库、大库性能、Windows 实机文件夹实时更新和用户界面主观验收。
