# CV-9DEEF7F0：Mac Wwise 面板字体编译兼容

## 来源和边界

- 2026-11-10 核对远端 qa/current 为 cfdd64eec1e1115612b3286c4c7591fed5c24c11。上一卡 CV-9B6DA6A3/838f435 的完整 Mac 构建在 WwiseTransferPanel.cpp 四处 GetUIFontEmphasis 未声明处停止。
- 本卡独立 clone 从最新 QA 建立，快进到上一卡 838f435；此前五张构建修复卡均未入 QA，最终整合必须刷新 QA、去重并重验。
- WindowsMain.h 将 GetUIFontEmphasis 声明包在 _WIN32 内。只处理 WwiseTransferPanel.cpp 的字体选择：Windows 继续使用原强调字体，Mac 使用当前 ImGui 字体绘制，保留面板内容和交互。Mac 强调字重与 Windows 的视觉对照留给可运行产品验收，不在本卡声称已经一致。

## 验证

1. 在本卡未改产品代码时复现 WwiseTransferPanel.cpp 的 Mac 编译首错并留原始日志。
2. 最小修改字体调用后，确认该模块编译，并继续完整 Mac 产品构建；出现独立新错误时保留日志，另卡处理。
3. Windows 共用源码同提交构建产品并读取实际测试数量与失败数。Mac 运行可用的 Wwise 定向测试。完整产品可启动时才做隔离窗口和真实字体截图；构建通过不代替功能及视觉验收。
