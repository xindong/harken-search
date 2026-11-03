# 筛选器三个输入框配色与列表交互：设计及验收记录

卡片：OPT-20261103-FILTER-INPUT-COLORS
状态：用户已在 v17 实际截图和统一验收页交付后明确要求走收尾流程；v22 已修复超长名称继续编辑时的截断并完成隔离实际包复验，正在核对远端检查与 QA 合入。
代码基线：2026-11-03 开工前将本卡分支快进到 origin/qa/current a4e7334。
独立目录：E:/tmp/harken-OPT-20261103-FILTER-INPUT-COLORS；分支 opt/OPT-20261103-FILTER-INPUT-COLORS。

## 收尾复核（2026-11-03）

- 用户在确认修订方向和实际截图后回复“走收尾流程”；记录的是这次界面确认，不代替代码审查或 QA 合入。
- 已整合 origin/qa/current d2d3be8145be5d3142f7cf426fca650bcc519f2e；整理提交签署前后的整合源码树一致。公开提交按仓库要求保留 Signed-off-by 和 Cindy 辅助开发说明。
- 整合后的隔离 Release 实际包位于 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v6/HarkenSearch.exe，SHA-256 为 b2a613fe873a5dc461136772a90921654339c86e61dc138e6a94cf5fd8d8dd62。
- D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence14/result.json 记录真实素材库副本上的 27 项界面操作，状态 pass、退出码 0；其中坏标签专辑仍准确筛出 1 个文件，输入框及结果标签显示清楚。实际截图在同目录。
- 整合后定向 Python 检查 7 项、C++ 名单模型测试通过；Debug 和隔离 Release 构建通过。仓库 Inner Source Gates 为 299 passed、1 skipped，发布合规检查通过。

## PR #83 独立审查后的修订（2026-11-03）

- 审查发现非空名单只在首次展开时载入，扫描新增的名称无法再从弹层选中。实际隔离包先复现失败，记录在 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence15-red/result.json；改成每次展开重新读取当前库名称后，同一测试通过。
- 原名单每帧重复清理全部名称并扫描匹配项。现将可见文字与匹配索引缓存在本卡名单模型中，仅在名单、查询词或语言提示改变时重算；C++ 测试先因缺少缓存模型而失败，再通过。
- 审查还指出狭窄窗口的弹层可能盖住输入框、首字母 O 会同时修改音频出点，以及右键点空白后旧查询词可能重新打开弹层。分别加入布局测试和隔离实际包操作，保留 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence18-red/result.json 与 evidence22-red/result.json 的失败事实；修正后均通过。
- 当前修订隔离包位于 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v13/HarkenSearch.exe，SHA-256 为 27574a7ba2fc855bf24afb2812858baeadf7011aacb4003058916fcaf2b83b50。D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence23/result.json：30 项实际操作通过、退出码 0；包含右键取消、新增艺术家重新展开可选、首字母 O 不改播放出点。QA 操作只作用于隔离素材库副本，测试后恢复临时修改的标签。
- 新增隔离 QA 按键通道仅在 ARIS_PREVIEW_ISOLATION 构建启用，普通产品仍保持非前台窗口不响应快捷键。完整修订提交与远端检查以 PR #83 最新 head 为准。

## 最新 QA 基线复验（2026-11-03）

- 已将 origin/qa/current 33c1b6c3dd391fa25c785f6463d8aa2d7d632a31 整合到本卡分支，整合提交为 16579ac60f3e51fd8866d47a38d58ee80e971a0e；原修正提交为 119bafc。两条新提交均保留 Signed-off-by 与 Cindy 辅助开发说明。
- 基于整合后的源码重新构建隔离 Release 实际包：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v14/HarkenSearch.exe，SHA-256 为 8d617c6be7b159fbd76f47478b8f3ce446e6740fc71bb24d486d292f0c336c62。
- D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence24/result.json 记录真实素材库副本上的 30 项操作，状态 pass、退出码 0；实际截图同目录。扫描新增名称后重开名单、右键点空白收起、首字母 O 不误改播放出点，均包含在内。
- QA 合入和看板完成以 PR #83 当前远端检查、独立审查回复及两边 QA 版本核对为准；本次隔离包仅供验收，不作为公开发布包。

## 第二轮独立复审与整合包复验（2026-11-03）

- PR #83 对 v14 指出三处可复现问题：超过 1023 字节的分类原始值被截断；窄窗口中名单遮盖的输入框仍会误响应点击；在浮动筛选器中点击原输入框后，名单会被筛选器盖住。先用旧实际包复现并留存失败记录，再分别修正。
- 三类已应用筛选值改为完整字符串，显示仍经过清理，精确筛选仍使用数据库原值。重叠点击只认真正被点击的输入框；编辑浮动筛选器时把名单窗口提到前方且保留输入焦点。新增超长原始分类与浮动筛选器隔离桌面复验。
- 修正提交 7d4ed3a，接入 origin/qa/current 237374e97728093fdbd7baf987af8425ac8f9df1 后整合提交 50eb82c。整合后的隔离实际包：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v17/HarkenSearch.exe；SHA-256 为 62f2f8f5d35d810d77b2c231ee0fe0b90016a9e74e60fa355b0cd359d7e4d6e0。
- D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence29/result.json：真实素材库隔离副本上 31 项操作通过、退出码 0，包含超过 1023 字节的原始分类名称。D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/final-overlap/result.json 和 final-zorder/result.json：浮动筛选器遮挡误点与名单前后顺序均通过、退出码均为 0；final-zorder/floating-query-after-arrow.png 为实际运行截图。
- 定向 Python 检查 9 项通过、46 项未选择；C++ 名单模型测试通过；Debug 与隔离 Release 构建通过；仓库 Inner Source Gates 为 299 passed、1 skipped，发布合规检查通过。统一验收页 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/验收入口.html 内嵌 12 张来自该整合包的真实截图。
- 用户此前的“走收尾流程”“归档这张 Harken 任务卡”发生在第二轮修正和 v17 实际包之前。按本项目界面验收规则，仍需用户看过当前版本并明确确认；此前不写验收通过、不合入或归档。

## v17 确认后的第三轮复审（2026-11-03）

- 用户在 v17 的实际截图和统一验收页交付后回复原句“走收尾流程”，确认当前界面及合入收尾方向；该回执已写进本卡 Cuevi 阶段信息。v17 的产品源代码和实际包没有在这轮复审中修改。
- 当前 head 的独立复审指出两处真实缺口：新增的 C++ 名单模型及 Python 契约测试未进入远端必需检查；浮动面板隔离测试在建立私有桌面前若初始化失败，可能留下临时设置。两个回归先失败、修正后通过。
- 远端必需检查已加入 C++ 测试目标、CTest 选择和 Python 契约测试；隔离测试把设置写入和资源建立全部纳入恢复保护。新的两种初始化失败回归均通过，浮动面板两项实际包操作再次通过，使用的仍是 v17 原包且 SHA-256 为 62f2f8f5d35d810d77b2c231ee0fe0b90016a9e74e60fa355b0cd359d7e4d6e0。
- 本机等同远端的公开检查为 308 passed、1 skipped，CTest 名单模型通过。仍须以 PR #83 新 head 的远端回执和独立复审为准，再核对最新 QA 并合入；合入前不写已完成或归档。
- 新 head 的复审摘要随后指出浮动面板测试可能在程序关闭超时或异常退出时仍报成功。现只在两项界面检查通过、程序正常退出且退出码为 0 时写 pass；超时强制退出或异常码写 incomplete 并让命令失败。v17 原包重新完成遮挡误点与层级两项操作，程序退出码均为 0、无超时，结果已写在 final-v3-overlap/result.json 和 final-v3-zorder/result.json。
- 随后复审摘要再次指出主验收脚本遇到中断时可能漏记失败：原脚本只捕获 Exception，KeyboardInterrupt 等中断不在其中，虽停止执行剩余检查，最终仍可能凭正常退出码写 pass。现把中断也记录为失败后原样抛出；同一 v17 实际包再次完成 31 项操作、退出码 0，记录在 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence30/result.json。产品代码和程序包字节没有变化。

## 最新 QA 整合与验收继承（2026-11-03）

- 合入前发现 origin/qa/current 已更新到 7b53610fc77123aa6f97352a354dfb128422a930；这是另一张更新器卡的改动。本卡在独立目录接入后，仅 tools/run_public_checks.py 的测试清单发生文本冲突，保留两张卡各自的测试项；未改本卡筛选产品代码。
- 基于整合源码重建隔离 Release 实际包 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v18/HarkenSearch.exe，SHA-256 为 7d5a532990c86df813da1b17aaa93b2cb030c8d595398bb02b40560580ebb92e。D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence31/result.json 记录 31 项实际包操作通过、退出码 0；final-v18-overlap/result.json 与 final-v18-zorder/result.json 均为 pass、程序退出码 0、无超时。
- 将 v17 与 v18 的艺术家、专辑、分类展开和已选坏标签专辑四张真实截图在筛选区域 (0,40)-(850,650) 逐像素比较，尺寸相同且该区域完全一致。用户已确认的本卡界面没有改变，故按 Cuevi 简化流程保留 v17 的前端确认；v18 的整合结果另行核验通过。公开检查 310 passed、1 skipped，C++ 名单模型 CTest 与定向 Python 11 项通过。

## 超长名称输入显示复核（2026-11-03）

- PR #83 新一轮复审指出：虽然已应用筛选保存完整原始名称，通用输入控件仍把显示文字截到 1023 字节。隔离实际包先复现：超过 1100 字节并以“界”结尾的分类名可准确筛出素材，但输入控件内部只收到前 1023 字节；失败记录为 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence34-red/result.json。
- 输入控件保留普通名称的栈内缓冲区；遇到超长值时才按实际字节数扩容，避免截断多字节汉字。仅隔离 QA 包暴露当前三个筛选输入框实际收到的文字，用于检查显示路径，不改正式包的数据接口。
- 修复包 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v19/HarkenSearch.exe，SHA-256 为 b5b4443db5a3f85b59c0e860e7a66f8489c801df22292d2efe05ad75c8337134。evidence35/result.json：32 项实际包操作通过、退出码 0，超长分类名 1120 个 UTF-8 字节完整进入输入框；final-v19-overlap/result.json 和 final-v19-zorder/result.json 均通过、退出码 0、无关闭超时。
- v18 与 v19 的艺术家、专辑、分类展开及坏标签专辑四张实际截图，在筛选区域 (0,40)-(850,650) 尺寸一致、逐像素相同。用户已确认的普通界面保持原样；超长名称末尾字符的显示完整性由实际输入控件检查覆盖。定向 Python 11 项、C++ 名单模型 CTest 及公开检查 310 passed、1 skipped。
- 合入前 origin/qa/current 又更新到 631d5ef84597e95f7ddf1547f2637f410862519e，变更只涉及另一张卡的白天配色设置页及其检查，本卡已整合。整合包 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v20/HarkenSearch.exe，SHA-256 为 161a9d7f13294a99a8b22ac16c33f273f75871231667115c491dd559a937a5d4。evidence36/result.json：32 项实际操作通过、退出码 0、长名称 1120 字节完整；final-v20-overlap/result.json 和 final-v20-zorder/result.json 通过、正常退出。v19 与 v20 的上述四张筛选截图逐像素相同；定向 Python 11 项和 C++ 名单模型 CTest 通过。远端必需检查以 PR #83 最新 head 为准。
- origin/qa/current 随后更新到 5cd13a79c3f123eabab14df55cce7add7530a3df，变更是另一张卡的夜间配色编辑入口与其独立 QA 命令；本卡无冲突整合。v21 实际包 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v21/HarkenSearch.exe，SHA-256 136106bcd6be4871670d8da9c9039439fce19acd80f49b92f9e2b324d136a960。evidence37/result.json：32 项操作通过、退出码 0、长名称 1120 字节完整；final-v21-overlap/result.json 与 final-v21-zorder/result.json 均通过、退出码 0。v17 与 v21 四张关键截图筛选区域逐像素相同。等待此 head 的远端检查和复审后再合入。
- 对 v21 的独立复审又指出超长已选名称没有继续编辑的余量：在 1120 字节分类名前插入一个字母后，外层查询缓冲区会截坏末尾汉字。隔离实际包先用新增操作复现，evidence38-red/result.json 记录无效 UTF-8 查询导致测试失败。现把三个字段的外层缓冲空间与共用输入控件的 1024 字节编辑余量对齐；仅隔离 QA 包回读实际草稿查询。
- 修复包 D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v22/HarkenSearch.exe，SHA-256 6d6f98064de9456ec65ba4e33f8945bb29f54f8855f7583c9361ce86f4e34f45。evidence40/result.json：33 项实际操作通过、退出码 0，长分类名 1120 字节完整，前面插入字母后的查询 1121 字节仍完整；final-v22-overlap/result.json 与 final-v22-zorder/result.json 均通过、退出码 0。v17 与 v22 四张关键截图筛选区域逐像素相同。

## 需求与范围

用户先要求左侧筛选器的「艺术家」「专辑」「分类」三个输入框与相邻控件协调；随后追加三个字段的列表交互。三个框的位置、尺寸、间距、标签、主动刷新重置和已保存主题设置保持原样。配色只调整背景、边框、输入文字、提示文字及已有悬停、聚焦状态。

追加交互的用户选择：点输入框或箭头均展开完整可滚动名单；艺术家逐个显示，不再把相近名称合为一行；在原输入框输入时按名称包含匹配名单，选择名称后才应用筛选；列表获得焦点后按字母跳到该字母的首项，完整名单仍保留；点空白处收起，未选则不改变已应用筛选。专辑与分类使用同一规则。名称搜索不含拼音匹配或错字容忍。用户进一步要求侧面弹层尽量加宽、加高，打开时直接显示更多艺术家，并在第一轮加高预览后要求“再长，尽量长一些”。已确认的最新预览保留约 560 像素宽，弹层高度优先用满窗口可用空间，仅留少量边距；高窗口示意首屏约 26 位，矮窗口按可用高度收缩，名单继续可滚动。首屏数量取决于实际窗口高度，不固定为 26。

原始证据是用户截图 cindy-media://blobs/03ea590414caafffeca173600f6346908ddae2085d6cfe5fddd2ce4ca2569240.png。截图里三个框是深蓝底，旁边下拉框是浅色；截图所用主题尚未通过实际运行包核实，不能仅凭截图宣称所有主题现状相同。

## 模块设计

| 模块 | 当前实现 | 提案与边界 | 独立核验 |
| --- | --- | --- | --- |
| 左侧元数据输入 | src/main.cpp 的三个过滤输入共用 DrawUnifiedSearchInput | 三个调用处使用同一组侧栏主题色；不改全局输入控件默认色 | 三框普通、悬停、聚焦逐一比对 |
| 配色来源 | src/HarkenSearchWidgets.cpp 的 TextInput 在继承上下文颜色时使用窗口通用色；海军蓝主题的侧栏为浅色，通用输入底色仍深 | 仅在该侧栏局部使用已有 filterBrowser 系列配色：普通态接近相邻下拉框，悬停略提亮，聚焦用清晰边线；文字随主题选色，避免固定浅底或深底 | 五个内置主题分别检查提示文字和输入文字可读性、边框及状态区分 |
| 列表打开与收起 | 目前只有箭头按钮打开现有弹层；弹层由 ImGui 管理关闭 | 输入框和箭头均可打开同一弹层；点空白处关闭且不提交临时查询 | 三个字段逐项检查打开、切换、空白关闭、已应用值恢复 |
| 名单查询与选择 | 现有弹层另有搜索框，超过 10 项才显示；艺术家把相近名称分组 | 原输入框承担名单包含搜索；查询词与已应用筛选分开；艺术家逐个显示，点选后才提交 | 空词、无结果、名称中段匹配、同名与大量名单逐项检查 |
| 首字母跳转 | 现有弹层无此功能 | 列表获得焦点后按字母滚到首个同字母项；在输入框编辑时仍正常输入查询词 | A/Z、无对应项、大小写、中文名称和滚动位置检查 |
| 弹层尺寸 | 现有弹层偏窄，列表首屏数量少 | 侧面弹层宽约 560 像素，高度优先用满窗口可用空间并留少量边距；高窗口示意约 26 位，较矮窗口自动缩短，始终保持可见和可滚动 | 大小窗口分别检查边界、首屏数量和滚动 |
| 提案预览 | 用户原截图 287×559 | 颜色图与交互图均为静态提案，不是实际运行截图；用户已同意按最新版进入实现 | 实际包运行后重新截图验收 |

现有通用 TextInput 还服务其他页面，实施时不修改全局默认色。若局部复用现有样式无法覆盖三态，则仅为这三个调用建立小范围颜色作用域；不新增控件体系。

## 颜色提案（方向已确认）

- 普通态：浅蓝灰背景与细边线，参考截图中相邻「格式」下拉框；提示文字改为清晰的深蓝灰。
- 悬停：保留浅色背景，边线稍明显，不移动或放大控件。
- 聚焦：浅色背景、清晰蓝色边线和深色输入文字；不增加文字背板。
- 预览只覆盖用户截图所示的浅色侧栏。五主题的实际颜色必须从各自内置主题读取，不能把这张图的像素值写死到全部主题。

预览文件：

- 原图副本：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS-preview/filter-inputs-before-user.png
- 普通态局部改色：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS-preview/filter-inputs-proposal-normal.png
- 三态颜色草案：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS-preview/filter-inputs-proposal-states.png
- 展开位置提案：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS-preview/filter-inputs-interaction-placement.png
- 四步交互分镜：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS-preview/filter-inputs-interaction-steps.png
- 加宽加高后的位置提案：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS-preview/filter-inputs-interaction-placement-v2.png
- 加宽加高后的名单近景：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS-preview/filter-inputs-interaction-list-v2.png
- 尽量用满高窗口的位置提案：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS-preview/filter-inputs-interaction-tall-v3.png
- 高窗口约 26 位名单近景：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS-preview/filter-inputs-interaction-tall-list-v3.png

## 实施与验收顺序

1. 已先运行失败检查，再实现三个输入框的局部主题配色、独立的临时搜索词、完整名单、字母跳转和尽量用满可用高度的侧面弹层。三个字段共用同一组件逻辑，列表只绘制可见行。
2. 已用本卡独立包和实际素材库，在私有桌面完成艺术家、专辑、分类的展开与选中、正反例搜索、点空白恢复、K 跳转、五主题、矮窗口、清除筛选、长分类名点选及结果栏标签限宽。输入由程序内隔离测试通道发送，不占用用户鼠标。
3. 已交付真实运行截图和隔离测试包。用户对实际包明确确认前，不记前端验收通过，不合并 QA，也不完成或归档卡片。

## 本轮实现和证据（2026-11-03）

- 组件及名单规则：src/SidebarMetadataPicker.h、src/main.cpp；三个输入框的临时搜索词与已应用筛选分开，点击输入或箭头打开，点击空白或 Escape 收起。
- 长名称与结果栏：src/AppState.h 将三个字段的名称缓冲区扩到 1024 字节；结果栏仅缩短过长的显示文字，完整名称仍用于筛选，悬停可看完整文字。
- 独立检查：tests/test_sidebar_metadata_picker.cpp、tests/test_sidebar_metadata_picker_contract.py；实际包回放：tools/qa_sidebar_metadata_picker.py。当前测试库的非空艺术家 124、专辑 215、分类 319 个；最长分类名超过旧输入框 127 字节上限，已在实际包中点选并筛出 1 条文件。
- 本卡测试包：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package/HarkenSearch.exe；固定隔离数据：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/data/。包 SHA-256：8d9bf7546b3cb7dd17a89c4a06bfd96642beaa545e43cc9a3a3419a50b8f0e6d。
- 最终运行记录：D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence9/result.json；退出码 0，24 项检查通过。真实截图同目录下的 artist-expanded.png、album-expanded.png、category-expanded.png、artist-search-matches.png、artist-K-jump.png、artist-short-window.png、long-category-selected.png 和 theme-*-artist-expanded.png。
- 定向测试 64 项通过，C++ 名单模型测试通过；独立 Release 构建及隔离 Release 构建通过。整份旧的 tests/test_core_workspace_style_boundary.py 尚有 3 项不相关的基线断言失败：两项要求仓库基线本来就没有的固定海军蓝色值，一项要求不存在 ImGui::Begin，但基线已有 ScanDropNotice。没有为本卡改写这些断言。
- 未覆盖：用户亲自查看及操作本卡包、浮动筛选面板跨显示器的人工体验。验收与 QA 合入仍待后续。

没有数据库结构、音频读取、素材查询算法或已保存主题设置变更。回退方式是撤销本卡局部样式和列表交互提交并重建测试包；预览文件本身不会进入程序包。

## 用户截图反馈修订（2026-11-03）

用户在实际包截图中指出名单里的乱码和弹层内外两层边框。核对隔离测试库和原始 WAV/OGG 文件后，乱码对应素材标签中已有的控制字节及损坏字符；例如艺术家字段可见连续 `0x1A`，原始 WAV 标签亦含同样字节。当前数据库的原值不能据此猜出完整人名。

- 名单只整理显示文字：遇到控制字节截去后面的错误尾巴，连续损坏字符以省略号标明，完全无法识别的名称显示本地化提示。点选仍使用数据库原值，避免改变筛选结果；未修改素材文件或数据库。无法从现有标签确定的字符没有猜填。
- 弹层保留一个外边框；滚动名单的内部边框和分隔线已去掉。顶部“全部”固定可点，名单独立滚动；展开时不再叠加箭头提示。一次完全单层滚动试验使“全部”滚出短窗口，实际包检查发现后已修正。
- 新增显示清理和单层视觉检查；最终隔离运行记录在 `D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/evidence11/result.json`，退出码 0，26 项检查通过，包括坏标签名称的真实运行截图。
- 修订包为 `D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v3/HarkenSearch.exe`，SHA-256 `69dcb82f697fc20e83c17b3e4b6d94741d09d51ca23bce511d44117c70e652e2`。原包保留作版本对照。仍待用户确认本次实际包，Cuevi 不记通过、不合入 QA。

## 已选名称的显示修订（2026-11-03）

用户继续指出，选中 `Robot Monster Vocalizations Vol. 3` 后，专辑输入框还显示了原始标签的坏尾字节。追查发现名单使用了清理后的显示文字，但输入框和结果栏的筛选标签仍直接显示数据库原值。

- 三个字段的输入框在非编辑状态显示整理后的名称；输入查询时仍显示用户正在输入的内容，清空查询不会变成“原始名称无法识别”。空白处取消后恢复整理后的已选名称。
- 三个字段在结果栏的筛选标签及其完整文字提示也使用整理后的显示名称；筛选运算、清除操作仍使用原始数据库值，不改数据。
- 对用户截图中的专辑原值（尾部含 `0x1A`）用实际包点选，输入框和结果标签均显示 `Robot Monster Vocalizations Vol. 3`，仍准确筛出 1 个 OGG 文件。`evidence13/result.json` 共 27 项实际操作检查通过，退出码 0；对应真实截图为 `evidence13/album-damaged-selected-input.png`。
- 最终修订包：`D:/Harken-QA/OPT-20261103-FILTER-INPUT-COLORS/package-v5/HarkenSearch.exe`，SHA-256 `7bf768d4496971b9cdee494d21b2e0e51777aef3cee6f03b7172e77c4cbc8c20`。相关定向测试 67 项与 C++ 名单模型测试通过，普通及隔离 Release 构建通过。待用户再次确认实际包，不合入 QA。
