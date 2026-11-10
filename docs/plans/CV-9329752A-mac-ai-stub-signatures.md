# CV-9329752A：Mac AI 替代实现接口签名编译兼容

## 来源和范围

- 2026-11-10 核对远端 qa/current 为 cfdd64eec1e1115612b3286c4c7591fed5c24c11。上一卡 CV-9DEEF7F0/8556397 的完整 Mac 构建在 AIEngineStub.cpp 四个转写函数的定义与 AIEngine.h 声明不匹配处停止。
- 本卡独立 clone 从最新 QA 建立，快进到上一卡 8556397；此前六张构建修复卡尚未入 QA，最终整合必须刷新 QA、去重并重验。
- 只使现有 AI 未启用替代实现的 Transcribe、TranscribeAsync、BatchTranscribe、TranscribeFromPCM 函数签名与公共头文件一致。返回仍明确失败或空结果，不接入模型、不修改 Windows 真实 AI 实现，也不把 AI 分析列为 Mac 已支持功能。

## 验证

1. 在本卡未改产品代码时复现四处 Mac 编译错误，保留完整原始日志。
2. 对齐签名后继续完整 Mac 产品构建，确认 AI 替代模块生成；若出现独立后续错误则留日志另卡处理。
3. 用隔离数据确认未启用 AI 的调用仍返回失败或空结果；核对同提交 Windows 产品构建和实际测试报告。完整应用可链接运行前，不以模块编译当作功能验收。
