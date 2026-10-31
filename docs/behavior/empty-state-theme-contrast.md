# 无数据库空态五主题配色

## 场景与目标

首次启动或原数据库不可用时，空态标题、正文、底部说明及“新建数据库”“打开数据库”文字在五套内置主题中均达到至少4.5:1。按钮默认、悬停、按下使用同一个可读字色，不因状态切换变淡。建库、开库行为及布局不变。

## 实现

`EmptyStatePalette` 只负责此模块的配色，复用 `TransportContrast` 的sRGB亮度、透明合成及8位量化。面板沿用原来的渐变，底层颜色由调用处传入实际 ChildBg；三个文字颜色按四角背景校验，两个按钮按四角及三种填充状态共同校验。优先保留原色，必要时提高不透明度或向深/浅色调整，目标4.8留出量化余量，硬性下限4.5。

当主按钮底色与卡片过于接近时，使用本主题强调色混合卡片底色，让浅色主题的主操作可以识别。装饰边框和分隔线不冒充文字对比度检查。

根据五主题截图反馈，右侧“打开数据库”还必须呈现清楚的按钮区域：使用柔和主题色填充，与卡片四角背景在默认、悬停、按下状态均至少3:1（目标3.2），不增加文字小底板。文字仍按三个状态共同选色，不改按钮位置、尺寸或开库行为。增加60项按钮底色检查，原实现60项失败。真实截图报告也检查默认按钮底色分离度。

`EmptyStateVisualAudit` 只在隔离QA命令请求后的短时间内采集实际绘制文字色、背景角点、控件边界、主题与帧号。普通版本无法通过该命令打开采集。区域边界用于截图取样，不宣称已测所有内部文字裁切。

## 可重复检查

构建隔离包时使用本机指定的独立数据根；编译目录使用 `build`，截图、日志与数据放在指定QA产物目录。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DARIS_PREVIEW_ISOLATION=ON -DARIS_PREVIEW_ISOLATION_ROOT=D:/Harken-QA/empty-state/profile -DAURO_BUNDLE_AVID_PTSL=OFF
cmake --build build --config Debug --target HarkenSearch TestEmptyStateContrast TestTransportContrast
ctest --test-dir build -C Debug --output-on-failure -R "^(TestEmptyStateContrast|TestTransportContrast)$"
python tools/qa_empty_state_themes.py --exe build/Debug/HarkenSearch.exe --isolation-root D:/Harken-QA/empty-state/profile --output D:/Harken-QA/empty-state/screenshots
```

输出目录必须尚不存在。脚本只启动自己的进程，核对EXE、PID与固定隔离根；已有数据库或未消费命令时拒绝继续。等待正确主题的新帧及五项文字齐全后截取真实窗口，不移动桌面鼠标或发送全局点击。保存完整图、提示区图、对照图、JSON报告和程序/截图摘要。报告同时列绘制颜色与截图最强文字像素估算，任一低于4.5都失败。退出码0通过、1低对比度、2截图或身份错误。不会建立或打开数据库。

本次回归先在原配色中检出56次失败。修复后需验证五主题、四角、三段文字与两按钮三态共180项通过，实际包另核对默认状态五主题25处文字。悬停/按下配色由同一生产函数的自动测试覆盖，截图不宣称覆盖这些交互；不同窗口尺寸、DPI、弹窗和自定义主题不在此轮验收范围。视觉体验仍以用户确认的真实截图为准。
