# CV-A17665E1 · Mac 拖入现有素材库文件并切换

## 来源与基线

2026-11-04 查正式 Mac 看板无同题卡或活跃负责人；远端 origin/qa/current 为 6573402de540c3d489b5a293d0cd7b6c45808be5。Windows 已合入 QA 的 WM_DROPFILES 对 .db 文件调用共享 BeginSwitchDatabase，其他窗口逻辑不在本卡改动。Windows 原 Cuevi 看板没有迁到 Mac，本卡按仓库 QA 源码记录来源，不称已读原卡或自动双向同步。

当前 Mac 外部拖放候选只接受文件夹，单个 .db 文件被拒绝。本卡从最新 QA 的独立 clone/分支出发，纳入上卡 dd8ec5b 待用户验收的 Mac 候选；候选不等于已合 QA。只支持**单个现存 .db 文件 URL**切换，沿用共享异步切库、错误提示、设置保存与失败回退。文件夹行为保持，音频文件与混合拖入仍拒绝；不修改 Windows WM_DROPFILES、数据库数据模型或日常库。

## 边界、失败与验证

1. Cocoa 拖入分类：单个本机 .db 文件可进入原生拖放接收；不存在的文件、目录伪装 .db、其他后缀、多个或混合输入拒绝。未打开素材库时也应允许选择已有 .db。
2. 只把路径交给共享 BeginSwitchDatabase，不直接打开 SQLite 或修改设置。切库忙、扫描忙、格式无效时沿用共享错误与回退；失败不能丢当前库。保留文件夹确认扫描原行为。
3. 在本卡编译期隔离根中用两个应用创建的测试库和生成 WAV 测试：拖入前后库路径、结果行、设置、正常退出重开；非法文件与忙状态保留旧库。Cocoa 实际 draggingEntered/performDragOperation 经私有 NSPasteboard 回放，不占用系统鼠标。真实 Finder 鼠标手势与用户界面验收仍单列未覆盖。
4. 定向 Mac 测试、实际 Debug 内部包构建解压复跑，必要时非诊断 Release 编译；包、截图、原始错误和绝对路径集中到 qa/CV-A17665E1/README.md。若触及共用代码，取得同 SHA Windows 回归；本卡预计只改 Mac 平台层与隔离 QA 测试。用户验收前不合入 QA/main、不发布。
