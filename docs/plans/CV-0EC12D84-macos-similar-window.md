# CV-0EC12D84 · Mac 相似音效窗口实测

2026-11-09 接续本卡。Mac 看板卡 CV-0EC12D84、独立 clone 和 task/CV-0EC12D84 分支已核对，当前负责 Session 已登记。Windows 需求来源为 REQ-20261101-SIMILAR-DEEP、docs/plans/2026-11-01-similar-deep.md 及 docs/behavior/modules/similar-sounds.md；这里只按已确认的固定“同类替代”、显式查找、窗口内结果与试听行为核对，不恢复历史双模式界面。Windows 原 Cuevi 卡本机未直连，以仓库行为说明及已合入 QA 的代码为准，未声称实时同步。

## 范围与顺序

1. 将 2026-11-09 实际刷新到的 qa/current cfdd64e 与现有 Mac 累积候选 53b7fee 合入本卡分支，保留本卡已有六项声学测试和失败日志。相对先前 b3779fd，新增部分只涉及 Windows Release 的 MSVC 运行库打包，但仍需检查 CMake 整合；同时核对相似音效新增的能量修正和模型包要求。
2. 只在隔离 Mac QA 控制内补可观察的相似窗口状态；用原有指针命令点选真实 Info 入口、分析、Find、结果行和试听控件。若默认无模型构建明确拒绝同类查找，如实登记模型缺口，不伪造结果或增加空实现。
3. 在本卡独立库生成可控 WAV，扫描并重开；记录打开窗口、分析进度与完成、索引数、查找结果或明确失败、主列表选择是否保持、正常退出。保存实际窗口截图与逐条命令。
4. 构建完整 Mac 产品，运行六项定向声学测试与实际隔离应用。若改动共用代码，同一最终提交取得 Windows 产品构建和相关回归；只改 Mac QA 桥也需核实 Windows 产品构建不受影响。测试包、已知限制和完整日志统一写入 qa/CV-0EC12D84/README.md。界面表现等待用户明确验收，合 QA 更需按最新基线重验。

## 边界

输入只来自 qa/CV-0EC12D84/data/ 的生成音频、数据库和设置；输出为该目录的日志、状态和截图。失败时保留原日志，按实际原因修正测试或本卡范围内产品行为。CLAP/ONNX 模型、深度相似听感、外部 DAW 和对外发布另行评估，不把无模型声学测试称为 Windows 模型版功能齐全。

## 2026-11-09 实测后的下一步

隔离界面已由 Info 按钮真实打开，六条合成音频分析成功；点击“查找相似音效”返回 ModelUnavailable，分析回执为 model_runtime_unavailable。现有 Mac CMake 未配置 HARKEN_ONNXRUNTIME_ROOT 或 HARKEN_SIMILARITY_MODEL_DIR，包内没有 CLAP 资源。固定“同类替代”会主动要求模型，不能以声学索引通过替代查询成功。

先按 docs/similarity-model-build.md 的固定版本、哈希与授权核对 Mac Intel ONNX Runtime SDK 和三份 CLAP 资源来源，再在隔离构建中接入并验证库加载、索引、查找、结果试听；若模型无法取得或核验失败，保留当前明确错误，不改为静默声学降级。正式包和用户验收前还要取得同一最终提交的 Windows 构建与回归。

SDK 检查结果：官方 Release 直链返回 HTTP 504，GitHub 官方 Release API 的同一 asset 成功取得 8,940,679 字节 x86_64 包，SHA-256 为 0f73006813af2a1a5d1723ed7dfb694fc629d15037124081bb61b7bf7d99fc78。动态库标注最低 macOS 13.3，而本机为 12.4；实机仍可启动，并通过 lsof 确认应用从包内 MacOS 目录加载该库。这只证明加载，不能推断模型推理在 12.4 可用。Mac 构建已补包内动态库与 @executable_path 加载路径；三份固定哈希 CLAP 资源仍缺，真实查找仍返回 ModelUnavailable。
