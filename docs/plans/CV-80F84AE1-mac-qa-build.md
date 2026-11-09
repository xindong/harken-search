# CV-80F84AE1 · Mac 最新 QA 的字符串复制编译恢复

## 现象与范围

2026-11-10 读取 GitHub 的 qa/current：cfdd64eec1e1115612b3286c4c7591fed5c24c11。未改代码的本卡首次 Mac 配置先因 Windows 专属 TestReaperBridgeContract 依赖 Mac 上不存在的 reaper_harken 而失败；前卡 CV-5B213257 已有相同的 WIN32 条件保护，但尚未合入 QA。本卡仅复用该保护作为可构建前置，后续整合时按实际 QA 去重。其后的已知编译阻塞是 src/AppState.cpp 的 _TRUNCATE 未定义；Windows 版本使用截断复制，目标缓冲区保留结尾 NUL。除配置前置外，本卡只处理共享 AppState 的固定缓冲区复制及为了验证它所需的定向测试；不改搜索/素材库的产品规则或界面，不借此宣称工作站直送完成。

独立 clone：cards/CV-80F84AE1；分支 fix/CV-80F84AE1。正式 Mac Cuevi 卡与本 Cindy 会话的负责 Session 已登记。Windows 需求来源为当前 qa/current 的 AppState 行为；旧 Mac 构建基线卡只提供历史错误证据，不接管其分支。

## 验证顺序

1. 在本卡目录用固定 CMake、Python 3.13 配置未改代码，保存原始错误及退出码。只给已知 Windows 专属 REAPER 测试加 WIN32 条件，再重配并运行未改 AppState 的产品构建，保存编译错误；核对 AppState 中全部三处固定缓冲区复制。
2. 将 AppState 的复制统一为跨平台、最多写入 N-1 字节并总是写结尾 NUL 的实现；用实际状态恢复/预设测试覆盖正常文本、过长文本和末尾边界，先红再绿。Windows 保持相同可见截断结果。
3. 重新构建完整 Mac 产品、运行相关测试与平台边界检查。若出现下一处独立模块错误，保留原始日志并明确标为下一卡，不用空实现掩盖。
4. 提交后在同一 SHA 的 Windows CI 构建产品并运行相关 AppState/搜索工作区测试，核对实际测试数量。结果、错误和绝对路径汇总在 qa/CV-80F84AE1/README.md；没有可运行的本卡产品就不生成验收包或称为完成。

本卡不动用户的日常素材、数据库和设置。所有构建及合成测试数据留在本卡独立目录；没有界面改版。用户验收和 QA 整合仍按现行流程另行处理。

## 本轮实际执行

- 未改代码的 CMake 配置失败于 Windows 专属 REAPER 合同测试依赖；使用与 CV-5B213257 相同的 WIN32 条件后配置通过。两次原始记录在本卡 QA 的 logs/001 与 logs/002。
- 未改 AppState 的完整产品编译确实停在两处 _TRUNCATE 未定义；logs/003 保留退出码和完整诊断。新增固定缓冲区复制函数后，六项行为边界检查与两项 Mac CTest 通过；平台边界 439 项通过，见 logs/005、006、008、009。
- 再次完整产品构建已越过 AppState.cpp，约 60% 时在独立的 CubaseAdapter.cpp 的 _dupenv_s 未定义处停止，见 logs/007。本卡不扩到 Cubase 适配，当前没有可运行产品包或截图。
- 本机完整公开检查 309 通过、19 失败、1 跳过；其中两项因新测试文件在运行时尚未暂存，暂存后该模块 30/30 通过。其余失败涉及本机缺 PowerShell/PATH 的 python 和已有清理测试对路径重解析的处理，保留 logs/010；不把本机完整公开检查写成通过。Windows 同提交 CI 仍待执行。
