# CV-5A3F46D2：Mac Cubase 环境变量编译兼容

## 来源和边界

- 2026-11-10 读取远端 `qa/current`：`cfdd64eec1e1115612b3286c4c7591fed5c24c11`。上一卡 CV-80F84AE1 的完整产品构建在 `src/CubaseAdapter.cpp` 的 Windows 专用 `_dupenv_s` 处停止。
- 本卡独立 clone 从该 QA 建分支，并快进到上一卡已验证的 `a09f265`，作为尚未整合的前置依赖。整合时按最新 QA 核对和去重；不能把前置卡说成已验收。
- 范围只含 Cubase/Nuendo 适配器读取环境变量的 Mac 编译兼容、针对性验证和完整产品续编。Windows 路径维持原调用；不声称 Mac 工作站直送已实现。

## 实施和验证

1. 在未修改的本卡分支复现 Mac 完整构建错误，保留命令和原始日志。
2. Mac 调用已有的 `Platform::ReadEnvironmentVariable`；Windows 保持 `_dupenv_s`。缺失或读取失败仍返回空值，不让覆盖值伪装为有效直送配置。
3. 用隔离的进程环境变量验证缺失、空值与非空值；跑 Mac 定向测试和完整产品构建。若出现不同模块的新错误，留原日志并另卡处理。
4. 提交后取得同一 SHA 的 Windows 产品构建和相关测试证据。仅有本卡范围技术证据时保留开发中，不进入验收或 QA。

不使用用户的音频库、设置或真实 Cubase/Nuendo；Windows 功能对齐的其他缺口继续逐卡处理。
