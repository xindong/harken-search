# Harken 官方应用图标原件

2026-10-25 用户提供新的 Search 图标包。Search 目录保留 18 个透明橙色 PNG（16 至 1024 像素）；压缩包中的 HarkenSearch.ico 已更新到仓库根目录，其它 HARKEN、Studio、Hub 原件保持原样，校验值见 manifest.json。

用户确认：**Studio 是蓝色，Search 是橙色，Harken 总站是黑色。** 按产品文件夹取图，不依据展示图的排列位置判断。

Windows 程序和安装器使用根目录 HarkenSearch.ico；工具栏使用 assets/icons/brand/ 内现有名称；官网使用同源 favicon.ico 和 apple-touch-icon.png。横版品牌字标单独维护，不用方形应用图标替换。

macOS Finder/Dock 图标使用同一 Search 橙色 PNG 原件生成的 resources/HarkenSearch.icns，仅打入 Mac 应用包：

    python tools/generate_macos_app_icon.py
    python tools/generate_macos_app_icon.py --check

生成脚本需要 macOS 自带 iconutil；不会修改 Windows .ico、工具栏图标或官网资源。

在仓库根目录执行（需要 Pillow）：

    python tools/generate_harken_app_icons.py
    python tools/generate_harken_app_icons.py --check

同步或检查官网时添加 --website-root <HarkenSearch/frontend 的绝对路径>。脚本不修改原件，不上传网站，也不改已安装的程序。

原件目录不属于 CMake 的运行资源复制目录；程序只携带所需派生图标。\n
