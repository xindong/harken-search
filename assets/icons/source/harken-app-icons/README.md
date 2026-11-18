# Harken 官方应用图标原件

2026-10-25 用户提供新的 Search 图标包。Search 目录保留 18 个透明橙色 PNG（16 至 1024 像素）；其它 HARKEN、Studio、Hub 原件保持原样，校验值见 manifest.json。2026-11-18 起根目录 HarkenSearch.ico 改用 Search 机器人图标包，本目录只派生小尺寸品牌 PNG。

用户确认：**Studio 是蓝色，Search 是橙色，Harken 总站是黑色。** 按产品文件夹取图，不依据展示图的排列位置判断。

本目录现在只提供 assets/icons/brand/ 内的 auro*.png 和 harken*.png（工具与界面用的品牌小图）；根目录 HarkenSearch.ico 已改用 2026-11-18 的 Search 机器人图标包，见 assets/icons/source/harken-bot-icons/README.md。横版品牌字标单独维护，不用方形应用图标替换。

在仓库根目录执行（需要 Pillow）：

    python tools/generate_harken_app_icons.py
    python tools/generate_harken_app_icons.py --check

同步或检查官网时添加 --website-root <HarkenSearch/frontend 的绝对路径>。脚本不修改原件，不上传网站，也不改已安装的程序。

原件目录不属于 CMake 的运行资源复制目录；程序只携带所需派生图标。\n