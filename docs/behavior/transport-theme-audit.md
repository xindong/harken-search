# 播放栏五主题巡检

## 目标与范围

在隔离的 Windows 测试包中自动切换五套内置主题，检查截图所示顶部整条播放栏可见文字和必要图形的对比度，保存真实截图和逐项异常。按异常逐项修复，再运行同一批场景。音频处理、数据库和其他界面不变。主责模块：UI。

## 详细设计

- 颜色检查：使用 WCAG sRGB 相对亮度，先合成透明颜色；普通文字下限 4.5，必要图形下限 3。使用未四舍五入的数值判断。装饰线与真正不可操作状态单独记录，不冒充检查通过。
- 运行时证据：在实际绘制处登记颜色、实际背景、控件区域、主题、帧与状态。仅隔离测试入口可读取；平时不持续采集。缺失、裁切或过期的区域不能判为通过。
- 批量执行：指定 EXE、隔离数据根目录和输出目录；核对进程身份后驱动真实程序，等待状态与渲染帧确认，再截取客户端和播放栏。使用合成音频，不访问用户媒体。
- 输出：逐主题/状态 JSON、PNG、中文异常汇总与五主题对照图，保留程序哈希和代码版本。失败、未覆盖、禁用项分开显示。
- 修复：先 EQ，再按巡检结果逐项处理其余播放栏控件。保持主题色相和布局，优先按实际背景选择可读颜色。

## 执行顺序

1. 写可重复测试，验证透明叠加、阈值边界和海玻璃低对比度能被抓住。
2. 接实际绘制采集与截图，运行五主题基线，保存修改前异常。
3. 一项一项修复并补充回归验证，保存修改后结果。
4. 运行对应测试与 Debug 构建，给用户真实对照截图和剩余异常。界面确认前不记视觉验收通过。

## 运行入口

需要 Windows、Visual Studio 2022、Python 和已有 tools/requirements-manual.txt 中的 Pillow。实际截图要求交互式桌面；数据和截图均留在本机。

```powershell
cmake -S . -B build_audit -G "Visual Studio 17 2022" -A x64 -DARIS_PREVIEW_ISOLATION=ON -DARIS_PREVIEW_ISOLATION_ROOT=C:/temp/harken-transport-profile -DAURO_BUNDLE_AVID_PTSL=OFF
cmake --build build_audit --config Debug --target HarkenSearch TestTransportContrast
python tools/qa_transport_themes.py --exe build_audit/Debug/HarkenSearch.exe --isolation-root C:/temp/harken-transport-profile --output C:/temp/harken-transport-report --code-revision candidate-label
```

隔离根必须与构建固定值一致，输出目录必须为新目录。只启动、控制和关闭本轮创建的进程。返回值：0 表示已测场景通过；1 表示对比度、裁切或缺失异常；2 表示运行/截图未完成，已有结果写入 capture-error.json。报告包含实际窗口大小，启动时请求的宽度可能被程序恢复布局覆盖。

固定十种状态：直通、旁通、挖槽、处理开启（含反向/声像/BM）、单声道、频谱展开、音量 107%、音量填充交界 55%、选区、宽 EQ。报告记录原始绘制颜色、透明叠加、区域、帧、截图 SHA-256、EXE SHA-256、Git 版本和源文件哈希。缺失项按条件检查，频谱收起到更多按钮时不把隐藏控件记为通过。

检查范围是音效模式的顶部栏。未覆盖音乐模式、弹窗、悬停/拖动、波形底栏、变调轮、扫描中状态和其他窗口尺寸。文字实际大小、抗锯齿和控件间关系仍需查看真实截图；数字通过不代表全部视觉验收通过。

音量百分比与频谱混合比不添加文字底板、描边或阴影，按实际滑块填充颜色逐列选择字色；保留控件原有边线。颜色检查覆盖静止截图中的底色，不代表所有播放动画帧。频谱混合比的键盘输入另由 `TestTransportBlendInput` 验证：真实 ImGui 接收 Ctrl+单击后进入数字编辑，输入 75 并回车应保存为 75%，退出编辑后仍无文字底板；同时检查原有纹理抗锯齿边框不被误删。运行：`cmake --build build --config Debug --target TestTransportBlendInput`，再执行 `ctest --test-dir build -C Debug -R TestTransportBlendInput --output-on-failure`。此交互测试不替代上述真实包截图。

裁切范围以 schema 2 报告为准：所有检测点保存采集时的有效裁切区；EQ 标记、斜率、频率、模式和刻度文字使用实际字体、字号和未裁切字形边界。图形替代文字时按图形采集。其它文字控件仍只登记控件边界，其内部文字裁切明确记为未测；报告中的“已测区域裁切为 0”不代表全部文字无裁切。字形采集由真实 ImGui 字体与嵌套裁切区的回归验证。
