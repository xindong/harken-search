# Harken 官方机器人图标原件

2026-11-18 用户提供 `Harken bot icon.zip`（Harken 上线群发布附件）。这里只保留 Search 产品的 5 个官方 PNG（64 至 1024 像素），字节与压缩包内一致，校验值见 manifest.json；Studio 及其它产品原件不入库。

用途：仓库根目录的 `HarkenSearch.ico` 现在由这套原件生成，作为唯一的应用图标，供 exe 资源、托盘、运行中的任务栏按钮、Alt-Tab、资源管理器文件图标和安装器共用；四角按 22% 圆角裁成透明头像瓦片。

在仓库根目录执行（需要 Pillow）：

    python tools/generate_harken_app_icons.py
    python tools/generate_harken_app_icons.py --check

脚本按 16 至 256 像素生成多尺寸 ICO，小尺寸由最近的更大原件等比例缩放，再套 22% 圆角遮罩（8 倍超采样抗锯齿），不裁切构图、不改色。原件目录不属于 CMake 的运行资源复制目录；程序只携带派生出的 ICO。
