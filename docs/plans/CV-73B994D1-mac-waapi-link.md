# CV-73B994D1：Mac WAAPI WebSocket 构建依赖接入

## 来源与范围

- 2026-11-10 核对远端 qa/current cfdd64eec1e1115612b3286c4c7591fed5c24c11。前卡 CV-ADCC313A 的 Mac 完整产品构建在 WaapiClient.cpp:21 因 ixwebsocket/IXNetSystem.h 不可见而停止。
- 本卡独立 clone 从最新 QA 建分支，快进到前卡 1086d90；上游 CV-80F84AE1、CV-5A3F46D2、CV-ADCC313A 尚未合 QA。整合时按最新 QA 去重、重验。
- 范围只处理 Mac 产品目标使用已有 ixwebsocket 库时的头文件传播与链接。Windows 原有链接保持不变；不修改 WAAPI 协议、对外网络策略或 UI，也不声称真实 Wwise 连接完成。

## 验证方法

1. 在未改产品代码的本卡 clone 复现 Mac 完整构建首错，保留原始日志。
2. Mac 应用目标链接已有 ixwebsocket 目标，利用其 PUBLIC include 传递头文件路径；重新完整构建，若遇独立后续错误则留日志另卡处理。
3. 跑现有 WAAPI 回环地址策略测试和可运行的 Mac 定向测试，只用隔离本机数据；不访问用户的 Wwise 工程或素材库。
4. 提交后核对同一 SHA 的 Windows 产品构建和定向回归。没有完整 Mac 应用运行、真实 Wwise 操作和用户验收时保持开发中。
