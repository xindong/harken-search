# REQ-20261029-ABOUT-LEGAL-CREDITS 关于窗口：法务、品牌与 Cindy 标识、明文链接

## 需求（用户 2026-10-29）

1. 关于窗口不再放“打开更新中心”迁移提示（更新中心已有独立入口）。
2. 显示法务信息：Apache License 2.0、Copyright 2026 xindong、随包 LICENSE /
   PRIVACY.md / docs/legal 指引。
3. 标注“感谢 Cindy 对本软件的开发支持”，并带 Cindy 女头 logo。
4. 带 Harken Search 图标与文字 logo（官方 wordmark 图，深浅底自动选黑白）。
5. 明文标出四个地址（文字链接，点击打开浏览器）：
   - 仓库 https://github.com/xindong/harken-search
   - 下载 https://harkensearch.xd.com
   - Cindy 仓库 https://github.com/makecindy/cindy
   - Cindy 下载 https://cindy.app

## 改动

- `src/HarkenSearchIcons.h/.cpp`：新增 `LoadPackagedAsset`（按 exe 相对路径
  加载并缓存 PNG 贴图，缺失静默返回空，调用方保留文字回退）。
- `src/main.cpp`：关于窗口重排——品牌图标 + 官方 wordmark + 版本、法务两行、
  Cindy 头像 + 致谢行、四行明文 URL（点击 `Platform::OpenUrl`）。
- `src/WindowsMain.cpp` + main.cpp：新增受固定隔离与主界面就绪门控的
  `about_window` QA 命令（仅 open/state），用于真实截图。
- `locale/harken.ui.strings.json`：移除两条更新中心迁移文案；新增版权、
  法务指引、Cindy 致谢与四行链接标签（zh/en/ja 齐全）。
- `assets/icons/contributors/cindy.png`：Cindy 女头 logo 随包资源；
  `CMakeLists.txt` 拷贝 contributors 目录到 exe 旁。
- `tests/test_about_window_legal_credits.py`：8 项契约测试。
- `tools/qa_about_window.py`：隔离启动、打开关于窗口、截独立弹窗 OS 窗口
  （多视口下弹窗是独立窗口）；不占桌面鼠标。

## 验证

- 8 项契约测试通过；语言账本生成器 generate+check 通过。
- 本卡包实际运行截图：runtime-4/about-window.png（品牌图标、wordmark、
  法务、Cindy 头像致谢、四行完整不折行 URL）。
