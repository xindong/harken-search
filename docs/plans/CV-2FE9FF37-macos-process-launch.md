# CV-2FE9FF37 · Mac 工作站进程启动基础能力

## 来源与版本

2026-11-09 读取最新 origin/qa/current cfdd64eec1e1115612b3286c4c7591fed5c24c11、Mac Cuevi 看板及共用工作站代码。Windows 的 Pro Tools 和 Cubase 适配器调用 Platform::LaunchProcess / RunProcessAndWait；Mac 对应实现直接返回 Unsupported。当前正式 QA 的 Mac 产品入口仍是 Demo，现有 Mac 产品候选 3e28710952caf55cd00945d16b2f430b74cff0b0 未验收。本卡以最新 QA 建独立 clone 和分支，完整产品构建时仅把候选作为开发依赖，不把它记成已合入 QA。

## 本卡范围

1. Mac 平台进程接口：把路径及命令行参数转换为 UTF-8 后直接启动目标程序，不经过 shell。无效路径、无效字符、启动失败返回明确状态和系统错误码；启动成功返回真实进程 ID，不留下僵尸进程。
2. 等待接口：能区分正常退出、非零退出、启动失败和超时；超时结束本卡启动的子进程，避免隔离测试遗留后台进程。保留 Windows 实现及调用方语义，不改变产品工作站能力提示。
3. 测试：隔离子进程覆盖路径含空格、参数引用、Unicode、无效路径、非零退出、超时和重复启动；构建 Mac 完整产品，在独立数据目录启动并正常退出。若修改共用构建配置，取得同提交 Windows 产品构建和相关回归。

## 2026-11-09 参数接口补测

同卡复核发现 src/platform/system_macos.mm 中另有 UTF-8 版 Platform::LaunchProcess，当前把参数按普通空格拆开，而 Windows 实现将已引用的参数交给 CreateProcessW。带空格的队列目录或文件名会在 Mac 被拆成多个参数。它目前只被 Windows 条件编译的 Pro Tools 代码调用，因此修复的是平台基础接口，不声称已经接通 Mac Pro Tools。

本次在同一独立 clone 和分支中，让 UTF-8 接口使用上面已验证的直接启动实现；保留调用方已引用参数的语义、拒绝无效 UTF-8，并避免复制另一套参数解析。增加真实子进程测试：带空格和中文的路径与参数、无效编码、异步运行及回收。再次构建完整 Mac 产品并运行隔离入口；CMake 选源变化后取得新提交的 Windows 产品构建和相关回归。旧提交 5cb9df4 的结果只作历史，不充当新提交的验证。

## 明确不在本卡范围

不启用 Mac 的 GetDawCapability，不把启动能力当作 REAPER、Pro Tools、Cubase 或 Wwise 直送已可用；这些还需要各自的宿主适配器、发现、IPC 和真实宿主测试。也不安装或启动用户的日常工作站，不改用户素材库、不修改 Windows 进程实现。未取得真实宿主证据时只报告基础能力，不推进工作站功能验收。
