# CV-5C9E7A21 · Mac Finder 与 Dock 显示官方 Search 图标

## 来源与范围

- 来源：差异盘点 CV-AB9FEDDA 确认 Mac 应用包的 CFBundleIconFile 为空；Windows 已使用仓库根目录 HarkenSearch.ico。仓库 assets/icons/source/harken-app-icons/README.md 记录了用户确认的 Search 橙色官方图标及 16–1024 像素原件。
- 开工基线为 origin/qa/current 5bb7c63aa95f89214b314125ee818d9873cdacaa；独立 clone 与分支 task/CV-5C9E7A21。先合入尚待用户验收的累计 Mac 候选 46a59402，才能测试真实产品应用；不把该候选当作已进入 QA。
- 图标包首次验证后，qa/current 又合入侧栏筛选 PR #83 和波形模式 PR #98，前进到 6573402。两项修改共用界面与设置代码；本卡需合入后重新构建、启动并针对筛选及波形补测，旧图标包只保留过程证据。
- 只给 Mac 应用包加原生图标：用现有 Search 透明橙色各尺寸原件生成 .icns，配置 CMake 复制到 Contents/Resources 并在 Info.plist 引用。保留 Windows HarkenSearch.ico、应用内字标和原始 PNG，不调整配色、形状或共用界面。

## 实施与验证

1. 在本卡分支加入可重复的图标生成入口和生成物；检查源尺寸与输出，避免放大 20/40 像素工具栏图标。
2. 只在 APPLE 构建目标接入图标资源与包元数据。检查产品包的 CFBundleIconFile 指向真实 .icns，文件可由 macOS 系统解码；从不同工作目录启动真实 Cocoa 产品并正常退出。
3. 在隔离 QA 数据根构建 Intel Mac 包、解压核对程序与图标、保存实际应用截图；取得同提交 Windows 产品构建和相关回归证据。测试包、截图、差异、失败日志统一放在 qa/CV-5C9E7A21/README.md。

## 验收边界

- 这是复用现有官方图标，不设计新视觉方案。Finder/Dock 的用户观感仍须用户确认，不能把包内文件存在当成视觉验收。
- 不修改日常素材库或设置；不签名、公证、对外发布；用户确认前不合入 QA。
