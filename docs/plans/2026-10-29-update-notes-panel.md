# REQ-20261029-UPDATE-NOTES-PANEL 更新弹窗本机版本区显示最新更新内容与贡献者

## 现象

用户截图：v1.0.41 已安装且为最新，"检查更新"面板的本机版本区只显示
"该版本暂未提供更新说明。"。原因有两个：

1. QA 体验包通道（scripts/package_qa_exe.ps1）的运行时白名单不带
   `aris_release_notes.json`，exe 同目录没有文件可读；
   正式安装器（installer/HarkenSearch_Setup.iss）一直带。
2. 界面在"已是最新版本"状态只按本机版本号精确查记录；本机版本在历史中
   没有记录时直接留白，不显示任何更新内容与贡献者。

## 改动

- `src/update/HarkenSearchUpdateNoticeModel.h/.cpp`：新增
  `SelectHarkenSearchLocalDisplayReleaseNotes`。优先精确匹配本机版本；
  找不到时回退到已加载历史的第一条（解析器保证最新在前），让标题、条目和
  贡献者保持可见；历史为空仍返回空指针，界面保留"暂未提供更新说明"。
- `src/update/HarkenSearchUpdateNoticeUi.cpp`："本机版本"状态改用上述选择；
  候选版本（本次更新/下载/重启）路径不变，仍不允许陈旧本地记录冒充新版本。
- `scripts/package_qa_exe.ps1`：运行时白名单加入 `aris_release_notes.json`，
  发布前用 `tools/pack_engine.py --prepare-release-notes` 按当前版本从
  `installer/harken_release_history.json` 生成并校验（与正式通道同一道闸门）。
- `tests/test_update_notice_content.cpp`：回退选择、精确优先、空历史三个用例。
- `tests/test_release_notes_package_gate.py`：QA 打包必须附带更新说明的契约测试。
- `tools/qa_update_notice_panel.py`：隔离启动、等待主界面就绪、经功能
  harness 打开更新中心、校验 rendered_version/items 并整窗截图；
  不移动桌面鼠标、不注入按键。

## 验证

- `TestUpdateNoticeContent`、`TestUpdateNoticePresentation` 通过。
- release notes / update notice 相关 51 项 Python 契约测试通过。
- 本卡体验包 `D:/Harken-QA/REQ-20261029-UPDATE-NOTES-PANEL/preview-v1/package`
  实际运行，本机版本区显示 v1.0.41 标题、5 组条目与贡献者
  （runtime-1/update-notice-local-version.png，
  rendered_version=1.0.41、items=7）。
