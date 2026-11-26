---
name: harken-dev
description: Harken Search 仓库的开发约定：构建目录与命令、测试怎么跑、测试包与证据交付、复用优先的代码规则、仓库卫生和已知坑。在卡目录里动手时用；开工占卡与收尾归档另有技能。
---

# Harken Search 开发约定

本页只写本仓库的落地做法。占卡、建目录、合入与清理走 `harken-card-start` / `harken-card-closeout`；卡面、阶段与授权以 Cuevi 当前规则为准。

## 1. 在哪改

产品文件只在卡目录里改：`E:/tmp/harken-<卡号>/`（P18 为 `D:/tmp/harken-<卡号>/`）。主目录 `E:/xdgit/xindong/harken-search` 只用来查看、建卡目录和跑集成；主目录停在 `qa/current` 时 `pre-commit` 会拦住普通提交。

## 2. 构建

日常开发用 Debug 构建，别动发布构建：

```bash
cmake -S . -B build_debug -G "Visual Studio 17 2022" -A x64
cmake --build build_debug --config Debug --target HarkenSearch -- /m
build_debug/Debug/HarkenSearch.exe
```

只有要出公开发布包时才是 `build_public`（`-DARIS_PUBLIC_RELEASE=ON -DAURO_BUNDLE_AVID_PTSL=OFF`，还要 bootstrap key），并跑 `py tools/check_release_compliance.py --release-dir build_public/Release`。发布流程本身在 `harken-release`。

## 3. 测试

```bash
ctest --test-dir build_debug -C Debug --output-on-failure
```

- 行为测试目标大多是 `EXCLUDE_FROM_ALL`，整套构建不会带上它们。要跑某个先用 `cmake --build build_debug --config Debug --target Test<名字>`，再 `ctest --test-dir build_debug -C Debug -R <名字>`。改动界面或布局时这一步不能省。
- 普通一张卡只跑相关测试；**全量测试只在正式发布前，或动了数据库、读音频、更新、构建这些共用底子之后**才重跑。
- 全量 Python 不全绿时写清差异来源，不写「全绿」。

## 4. 交包与证据

- 给用户试用的包 = **整个 `build_debug/Debug/` 目录**，不是单个 exe。
- 前端表现类改动必须真跑实际包、给真实截图，并写清打开的是哪份素材库；不能只说「已实现」。
- 截图、报告、日志放 `D:/Harken-QA/<卡号或日期>-<用途>/`，不进仓库、不放桌面和下载目录；临时产物按 7 天留存。能反复运行的测试放仓库 `tests/`。
- 实机验证用固定隔离构建，启动时传 `AURA_RUNTIME_ROOT`；不要用普通构建跑私有桌面，会打开用户真实库并留下收不掉的实例。

## 5. 写代码前的规矩

**先找有没有已经做过的那一份。** 同一功能换个窗口、换个入口、换个面板，都不算新功能的理由。只有三种做法：改已有实现、把共用部分抽到现有层、确实独立设计。前两种直接做；第三种先报维护成本，固定四栏一句话：以后同一处改动要改几处、要回归哪些已有界面、用户能看见哪些不一致、以后合并要迁哪些配置或数据。

- 取色只从 `ThemeColorScheme` 的语义色来，不新增第二套取色；文案走现有本地化键；设置项进现有设置存储。同一状态出现第二套颜色、默认值或文案，就按独立设计算。
- 同一状态两套外观、两套逻辑，是明确不接受的结果。

## 6. 仓库卫生

- 根目录不堆一次性文件夹；编译结果只进 `build*`；临时笔记只进 `.local`；不要新建 `tmp` 这类目录。
- 中文正文和含中文的脚本一律 UTF-8 落盘再提交。用 ascii 存过含中文的脚本，汉字会变成问号，卡标题和正文都坏过。
- 删编译缓存没有固定脚本；脚本没交付前不要自己编删除命令，把现场留着。
