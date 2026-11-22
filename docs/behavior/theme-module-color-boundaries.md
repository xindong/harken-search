# 模块色块语义边界

## 目标

高级编辑器继续保存旧的 `ThemeColorScheme` 平面字段和旧 JSON；运行时模块通过短生命周期的 `ThemeModuleColors` 读取语义角色。这个投影层不持久化关系，也不重写已有主题数值。

## 已迁移入口

| 模块 | 语义入口 | 保持的旧行为 |
|---|---|---|
| 波形管理器 | `waveform.primary / background / secondary / playhead`、`results.outline` | 波形颜色、边框和播放头颜色不变 |
| 日夜主题选择器 | `controls.frame / border` | 选择器控件底色和边框不变 |
| 相似分析进度与结果弹窗 | `surface.toolbar / elevated / border`、`metadata.text`、`topBar.text` | 进度按钮与弹窗外观不变 |
| Wwise 传输面板 | `wwise.*`、`results.selection` | 状态色与选中态不变；标签文字、正文文字可独立，未设置时继续使用旧文字来源 |
| 侧栏活动条 | `sidebar.rail / railDivider / inactive / hover / active / activeStrip` | 图标悬停、激活和分组分隔线不变 |
| 诊断浮窗 | `diagnostic.*`、`meter.*`、`signal.level` | 性能曲线、缓存进度条和日志入口不变 |
| 更新提示 | `results.*`、`overlay.*`、`topBar.text`、`wwise.failed` | 版本卡片、错误详情、下载进度和历史内容不变 |
| 主窗口筛选器入口 | `filterBrowser.*`、`controls.searchAction`、`surface.canvas` | 侧栏元数据输入、弹出层、文件树工具条和列表容器不变 |
| 结果列表内联绘制 | `results.frameOutline / scrollbar / tab* / gradient* / playedText`、`surface.canvas` | 结果列表外框、页签、滚动条、渐变和已播放文字不变 |
| 顶部播放控制 | `transport.*`、`controls.frame`、`topBar.accent`、`surface.border` | 播放控制、Key/BPM、音量和频谱控件的颜色计算不变 |
| 波形底栏与状态壳 | `waveform.footer / splitter / splitterLine`、`transport.text`、`controls.accent`、`surface.border` | 波形底栏、选区胶囊、迷你播放控制和合并状态栏不变 |
| 波形内部绘制 | `waveform.background / primary / secondary / playhead / border / splitter`、`controls.accent`、`overlay.background` | 主波形、迷你波形、加载占位、分屏播放头、缩放滑条和浅色壳层不变 |
| 筛选与 Dock 壳层细节 | `filterBrowser.*`、`metadata.*`、`results.*`、`transfer.history*`、`controls.accent` | 系统目录定位、高亮标签、元数据筛选、封面墙和 Dock/状态栏外壳不变 |
| 导航胶囊与时间码 | `navigation.history* / mode*`、`transport.timecode*`、`results.tabHover` | 历史前进后退胶囊、模式胶囊和时间码悬停文字不变 |

## 兼容规则

1. `ThemeColorScheme` 仍是旧主题、日夜主题和 JSON 的兼容载体。
2. `ResolveThemeModuleColors()` 映射字段和可选独立色的来源，不在模块入口重算混色或重写主题数值。
3. 颜色语义拆分先体现在模块读取路径；旧字段只有在明确建立迁移规则后才改变默认关系。
4. 新模块不得直接把 `resultsSelection`、`toolbarAccent`、`controlAccent`、`transferButtonBG`、`filterBrowserLocateTint` 或 Wwise 状态字段互相当作别名。

## 下一批迁移

- 筛选器的定位、选中和结果高亮入口。
- `main.cpp` 中剩余的结果列表细节和波形内部刻度/拖拽壳层入口。
- 在高级编辑器中显示模块影响范围，再决定哪些关系可以成为新语义主题的默认绑定。
