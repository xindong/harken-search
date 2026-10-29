# REQ-20261029-ABOUT-LEGAL-CREDITS 关于窗口双页设计

## 用户确认的范围（2026-10-29）

- 窗口整体加大、加高，文字和界面元素对齐。
- 头部并排使用 Harken Search 官方图形标志与用户指定的 HARKEN search 官方文字标志，两者垂直居中。
- 第一页“关于”：Apache License 2.0、Copyright 2026 xindong、Cindy 女头和致谢，以及 Harken Search / Cindy 各自的仓库与下载明文地址。
- 第二页“法律文档”：在窗口内直接阅读安装与使用协议、隐私政策、开源发布说明和 Apache 2.0 全文；长文可滚动。两页可用页签及底部按钮往返。
- 用户已先看过布局预览，随后授权直接修改产品代码；实际界面仍需用户验收。

## 模块设计

### 1. 关于窗口与导航

src/main.cpp 在既有“帮助 → 关于”弹窗内维护当前页和当前法律文档。窗口按可用桌面区域限制为最大 980 × 780；页签和底部按钮切换两页，右上角与 Esc 均可关闭。头部用已随包的官方图形标志与 harken-search-menu-wordmark 图片；根据主题背景选择黑白文字图。版本号靠右，与图标行垂直居中。字号、头像、选项按钮和留白均随这张卡放大。

### 2. 关于内容与链接

第一页把版权、许可、Cindy 致谢和两列地址分区排齐。Cindy 女头取自用户提供的头像，随包放在 assets/icons/contributors/cindy.png。四个地址以完整文字显示，点击地址后由原有 Platform::OpenUrl 打开：

| 对象 | 仓库 | 下载 |
| --- | --- | --- |
| Harken Search | https://github.com/xindong/harken-search | https://harkensearch.xd.com |
| Cindy | https://github.com/makecindy/cindy | https://cindy.app |

### 3. 法律文档阅读

第二页有四个选择按钮，选中文档后把随安装包提供的正文载入窗口内的独立滚动区。切换文档时滚动位置回到顶部；文件缺失时显示明确提示。中文界面读中文文档，英文及日文界面读英文文档；Apache 2.0 使用同一份 LICENSE。文档只读取现有安装资源，修改范围不涉及协议正文。

| 入口 | 中文文件 | 英文/日文文件 |
| --- | --- | --- |
| 安装与使用协议 | docs/legal/EULA.md | docs/legal/EULA.en.md |
| 隐私政策 | docs/legal/PRIVACY_POLICY.md | docs/legal/PRIVACY_POLICY.en.md |
| 开源发布说明 | docs/legal/OPEN_SOURCE_RELEASE_NOTICE.md | docs/legal/OPEN_SOURCE_RELEASE_NOTICE.en.md |
| Apache 2.0 | LICENSE | LICENSE |

### 4. 文案、资源与隔离验收

locale/harken.ui.strings.json 提供三种语言的页签、按钮、标题、地址标签和文件缺失提示。tools/qa_about_window.py 只在隔离 QA 启动时用现有诊断命令打开指定页面并抓取独立弹窗，不占用用户鼠标；产品正常入口仍为“帮助 → 关于”。

## 验证与交付

- tests/test_about_window_legal_credits.py：10 项针对性检查，覆盖官方资源、双页、法律文件映射、文案和链接。
- tools/generate_localization.py 的 generate 与 check 均通过；HarkenSearch Debug 目标构建通过。
- 本卡实际测试包：D:/Harken-QA/REQ-20261029-ABOUT-LEGAL-CREDITS/preview-v3/package/HarkenSearch.exe。包内含法律文档和品牌资源。
- 同一测试包的实际运行截图：preview-v3/runtime-about-final/about-window.png 与 preview-v3/runtime-legal-final/about-window.png。截图分别显示关于页与可滚动的协议正文页。
- 当前仍是本卡分支；等待用户对实际运行界面的验收，不在此阶段合入 qa/current。