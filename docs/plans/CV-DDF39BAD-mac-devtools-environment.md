# CV-DDF39BAD：Mac 开发图库环境变量编译兼容

## 来源与范围

- 2026-11-10 核对远端 qa/current 为 cfdd64eec1e1115612b3286c4c7591fed5c24c11。上一卡 CV-9329752A/4852db3 的完整 Mac 产品构建在 DevToolsHarkenSearchSmoke.cpp 的 _dupenv_s 未声明处停止。
- 本卡独立 clone 从最新 QA 建立，快进到上一卡 4852db3；此前七张构建修复卡未入 QA，最终整合必须刷新 QA、去重并重验。
- 只处理诊断版开发图库的 AURO_UI_GALLERY_SECTION 读取：Windows 保留 _dupenv_s；Mac 使用已有 Platform::ReadEnvironmentVariable。参数缺失或无效时仍选第 0 节；只接受 0 至 8，不改变图库布局或正式功能。

## 验证

1. 未改产品代码前复现 Mac 完整构建首错，保留完整原始日志。
2. 最小修复后确认 DevToolsHarkenSearchSmoke.cpp 对象生成，继续完整产品构建；新的独立错误另卡处理。
3. 在本卡隔离环境运行 Mac 环境变量相关测试，核对 Windows 同提交产品构建和实际测试报告。完整应用可运行前，不把模块编译当成图库显示验收。
