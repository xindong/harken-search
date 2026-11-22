# 游戏音效 Playlist 写入 · 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: 用 superpowers:subagent-driven-development 或 superpowers:executing-plans 逐任务执行。步骤用 `- [ ]` 勾选跟踪。

**Goal:** 让外部 AI 客户端在用户授权下，把游戏音效真正写进 Harken 的 Playlist（主 + 子），幂等、可撤权、失败整体回滚。

**Architecture:** 连接器不碰数据库；它通过本机命名管道把写入请求交给 Harken，Harken 用自己的 Database Playlist 接口在单个事务里执行并返回回执。授权文档新增 `playlistRead`/`playlistEdit` 两项能力和 `bridgeToken` 作为管道鉴权。

**Tech Stack:** C++17（ImGui 设置页、Win32 命名管道、SQLite via Harken Database）、Python（MCP 端到端测试）。

**前置事实（已核实）**
- 连接器可执行文件：`HarkenExternalAiMcp.exe`，随 `HarkenSearch.exe` 同目录分发。
- 连接器已有 `harken_preview_playlist_pack` 与 `harken_apply_playlist_pack`；apply 目前在校验齐全后返回 `playlist_write_unavailable`。
- 授权文档：`<settings>/external-ai/<libraryId>.json`，`roots` 已是 `{path, enabled}` 对象数组。
- Harken 侧写入口存在：`Database::createPlaylist`、`PlaylistBulkAddResult`（批量加入，含已存在计数）。

---

## Task 1: 授权文档支持 8 项能力

**Files:**
- Modify: `src/ExternalAiAuthorization.h`, `src/ExternalAiAuthorization.cpp`
- Modify: `src/ExternalAiAuthorizationStore.h`, `src/ExternalAiAuthorizationStore.cpp`
- Test: `tests/test_external_ai_authorization.cpp`

- [ ] Step 1: 在 `ExternalAiAuthorization.h` 的能力枚举里加 `ReadPlaylist`、`EditPlaylist`（放在 `EditMetadata` 之后，保持既有键序不变）
- [ ] Step 2: `ExternalAiAuthorizationStore.h` 把 `ExternalAiCapabilityCount` 改为 8，并在 `ExternalAiCapabilities` 追加两项
- [ ] Step 3: 能力键名映射加 `playlistRead` / `playlistEdit`（两侧：解析与序列化用同一张表）
- [ ] Step 4: 写失败测试——旧文档（只有 6 项能力）解析后 `playlistRead/playlistEdit` 为 `Denied`；未知键仍然报错
- [ ] Step 5: 跑 `TestExternalAiAuthorization`，确认新用例失败
- [ ] Step 6: 实现解析/序列化，使其通过
- [ ] Step 7: 跑全套 `ctest -R "TestExternalAi"` 与 `python -m pytest tests/test_external_ai_mcp.py <exe>`，全绿后提交

## Task 2: 文档里的 bridgeToken

**Files:**
- Modify: `src/ExternalAiAuthorization.cpp`（解析可选字段 `bridgeToken`）
- Modify: `src/ExternalAiAuthorizationStore.cpp`（保存时若无 token 则生成 32 字节 hex 并保留）
- Test: `tests/test_external_ai_authorization.cpp`

- [ ] Step 1: 失败测试：新保存的文档含 64 位十六进制 `bridgeToken`；重复保存不变；旧文档无该字段时解析成功且 token 为空
- [ ] Step 2: 实现生成/保留逻辑（只用 `std::random_device` + hex 编码，不引新依赖）
- [ ] Step 3: 跑测试，全绿提交

## Task 3: 设置页显示 8 项能力（三组）

**Files:**
- Modify: `src/ExternalAiSettingsPage.inl`
- Modify: `locale/harken.ui.strings.json`（新增 `ExtAi_GroupPlaylist`、`ExtAi_CapPlaylistRead`、`ExtAi_CapPlaylistEdit`）
- Test: 隔离构建截图 + `tools/qa_external_ai_one_click.py` 增补断言

- [ ] Step 1: 分组改为三组：查找（3）、文件名与元数据（3）、Playlist（2）
- [ ] Step 2: 标签数组按 8 项补齐；确认没有越界（用 `std::size(ExternalAiCapabilities)` 驱动循环）
- [ ] Step 3: 加三组键的中英日文案（ja 必须真日语，否则纯净度门禁红）
- [ ] Step 4: 构建 + 隔离构建截图，确认三组都在、下拉可改
- [ ] Step 5: `python -m pytest tests/test_localization_ledger.py tests/test_localization_purity.py tests/test_core_workspace_style_boundary.py -q` 全绿后提交

## Task 4: Harken 侧写入服务端（纯逻辑，先不接管道）

**Files:**
- Create: `src/ExternalAiPlaylistBridge.h`, `src/ExternalAiPlaylistBridge.cpp`
- Test: `tests/test_external_ai_playlist_bridge.cpp`（新目标，加入 `cmake/harken_external_ai_mcp.cmake`）

- [ ] Step 1: 定义请求/回执结构：`{token, requestId, libraryId, playlistPath, fileIds[], proposalDigest}` → `{status, added, skippedExisting, failed, receiptId, reason}`
- [ ] Step 2: 失败测试：token 不匹配 → `denied`；libraryId 不匹配 → `stale`；未知 playlistPath 层级 → `invalid`
- [ ] Step 3: 失败测试：同一 requestId 第二次调用返回同一 receiptId 且 `added == 0`
- [ ] Step 4: 实现纯逻辑部分：校验 + 幂等表（内存版），暂时用注入的假写入函数
- [ ] Step 5: 测试全绿提交

## Task 5: 接真实 Database 写入（单事务）

**Files:**
- Modify: `src/ExternalAiPlaylistBridge.cpp`
- Modify: `src/Database.cpp`（仅在需要时补一个批量加入的封装；优先复用现有 `PlaylistBulkAddResult`）
- Test: `tests/test_external_ai_playlist_bridge.cpp`（用临时库文件）

- [ ] Step 1: 失败测试：给定 3 个文件加入 `Game SFX` / `Combat`，主与子 Playlist 都存在且成员正确
- [ ] Step 2: 失败测试：加入过程注入失败 → 事务回滚，Playlist 不存在或成员为 0
- [ ] Step 3: 实现：`BEGIN` → 找/建主 Playlist → 找/建子 Playlist → 批量加入 → `COMMIT`；失败 `ROLLBACK`
- [ ] Step 4: 跑测试，全绿提交

## Task 6: 命名管道服务端

**Files:**
- Modify: `src/ExternalAiPlaylistBridge.cpp`（Win32 管道线程）
- Modify: `src/WindowsMain.cpp`（按当前库/授权状态启停服务端）
- Test: `tests/test_external_ai_playlist_bridge.cpp`（用匿名/命名管道客户端直连）

- [ ] Step 1: 失败测试：管道名由 libraryId 哈希决定；连接后发合法请求得到回执
- [ ] Step 2: 实现：`CreateNamedPipe` + 单请求单响应 + 5 秒超时 + 仅当前用户 ACL
- [ ] Step 3: 服务端在「当前库 + 授权文件存在 + playlistEdit=edit」时才启动，否则不建管道
- [ ] Step 4: 测试全绿提交

## Task 7: 连接器 apply 改走管道

**Files:**
- Modify: `src/ExternalAiMcp.cpp`
- Test: `tests/test_external_ai_mcp.py`

- [ ] Step 1: 失败测试：Harken 未运行时 apply 返回 `playlist_write_unavailable`
- [ ] Step 2: 实现客户端：读 token → 连管道 → 发请求 → 15 秒超时 → 映射回执
- [ ] Step 3: 端到端测试：伪造服务端（Python 起一个管道）验证连接器请求内容正确
- [ ] Step 4: 全绿提交

## Task 8: 设置页显示最近一次写入

**Files:**
- Modify: `src/ExternalAiSettingsPage.inl`（读取 `external-ai/receipts.json` 最后一条）
- Modify: `locale/harken.ui.strings.json`

- [ ] Step 1: 文案：`最近一次外部 AI 写入：新增 N，已有 M，回执 R`
- [ ] Step 2: 无回执时整块不显示
- [ ] Step 3: 构建 + 截图 + 账本/纯净度测试通过后提交

## Task 9: 端到端验收（隔离构建）

**Files:**
- Modify: `tools/qa_external_ai_one_click.py` 或新增 `tools/qa_game_sfx_playlist_write.py`

- [ ] Step 1: 隔离库 + 合成 WAV，勾选目录、开启 playlistRead/Edit、保存、连接
- [ ] Step 2: preview → 断言库无变化
- [ ] Step 3: apply → 断言 Playlist 结构与成员
- [ ] Step 4: 重复 apply → 断言不增加
- [ ] Step 5: 撤销 playlistEdit → 断言 `not_authorized`
- [ ] Step 6: 出证据 JSON + 截图，写 HTML 验收报告

## Task 10: 收尾

- [ ] Step 1: 更新 `docs/manual/external-ai-mcp.md` 与 `game-sfx-profile.md`
- [ ] Step 2: PR（base 为授权版合入后的 qa/current）→ CI → 合入
- [ ] Step 3: Cuevi 进度/completed/archived，清理卡目录与分支
