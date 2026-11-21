# 游戏音效 Playlist 写入 · 规格（2026-11-21）

卡号：`REQ-20261121-GAME-SFX-PLAYLIST-WRITE`
分支：`feature/REQ-20261121-GAME-SFX-PLAYLIST-WRITE`（叠在外部 AI 授权版 + 一键连接之上）

## 1. 要解决的问题

外部 AI 客户端（Cindy / Codex）已经能**读**Harken 库：找音效、找同类、找目录。
现在要做的是让它能**按用户授权，把游戏音效整理进 Harken 的 Playlist**——不是生成一份报告，而是真的在 Harken 里出现「游戏音效」主 Playlist 和它的子 Playlist。

不做的事：不移动/复制/改名/删除实体文件，不写音频标签，不碰音乐域，不上传任何数据。

## 2. 用户看到的完整流程

1. 用户在 Harken 设置 → 外部 AI 里：打开开关、勾一个或多个目录、把 `Playlist 读取` 与 `Playlist 写入` 设为可读/可编辑、保存。
2. 用户在设置页点「连接」把 Cindy 或 Codex 接上。
3. 用户在客户端说「把我这个库的游戏音效整理成 Playlist」。
4. 客户端调用 `harken_preview_playlist_pack`：返回清单（每条含目标 Playlist 路径、置信度、理由、证据来源、是否需要复看），**这一步不写任何东西**。
5. 用户看过清单说「可以」，客户端调用 `harken_apply_playlist_pack`（`proposal_id` + `confirm: true`）。
6. Harken 收到写入请求 → 校验授权与数据新鲜度 → 在一个数据库事务里建/找 Playlist 并加入文件 → 返回回执（新增 N、已有跳过 M、失败 0、回执号）。
7. 设置页「最近一次外部 AI 写入」显示这条回执，用户随时可以把 `Playlist 写入` 改回不授权，后续请求立即被拒。

## 3. 权限模型

授权文档（`external-ai/<libraryId>.json`）扩展两项能力：

| 文档键 | 界面文案 | 含义 |
| --- | --- | --- |
| `playlistRead` | Playlist 读取 | 允许读取现有 Playlist 结构（用于避免重复、选父级） |
| `playlistEdit` | Playlist 写入 | 允许把文件加入 Playlist |

规则：

- 两项都必须**非 none** 才允许 apply；写入额外要求 `playlistEdit == edit`。
- 每个待写文件都必须落在**已勾选**的目录里；未勾选目录一律拒绝。
- 写入只影响 Playlist 成员关系，不改变文件本身。
- 撤回授权后，下一次 apply 立即失败（连接器每次请求都重读授权文档）。

## 4. 写入通道（关键取舍）

连接器是独立进程，**不允许**自己打开数据库写 Playlist：会绕过 Harken 的去重与排序规则，并与 Harken 的 SQLite 连接、WAL 状态冲突。

因此：**连接器把写入请求交给 Harken 执行。**

- 传输：本机命名管道 `\\.\pipe\harken-external-ai-<libraryIdHash>`，只允许当前用户。
- 鉴权：授权文档新增 `bridgeToken`（32 字节随机十六进制，由 Harken 在保存授权时生成）。连接器启动时已拿到 `--settings` 文档路径，因此能读到 token；请求必须带 `token`，不匹配即拒绝。
  - 效果：只有能读到该用户授权文档的进程才能写入；单纯连上管道不足以写。
- 请求：`{token, requestId, libraryId, playlistPath, fileIds[], proposalDigest}`
- 回执：`{status, added, skippedExisting, failed, receiptId}`
- 幂等：`requestId` 与 `proposalDigest` 记录在设置目录的 `external-ai/receipts.json`（最多 64 条）。同一 `requestId` 再次到达直接返回原回执，不重复添加。
- 原子性：全部加入在**一个**数据库事务内；任何一步失败整体回滚，回执 `status=failed` 并带原因。
- 超时：连接器等待 15 秒；Harken 未运行或超时 → `playlist_write_unavailable`（fail-closed，不重试乱写）。

## 5. 数据与文件

新增/修改：

| 文件 | 变化 |
| --- | --- |
| `src/ExternalAiAuthorization.h/.cpp` | 能力枚举加 `ReadPlaylist`/`EditPlaylist`；文档新增 `bridgeToken`（可选字段，缺失时写入不可用） |
| `src/ExternalAiAuthorizationStore.h/.cpp` | 8 项能力序列化；保存时生成/保留 bridgeToken |
| `src/ExternalAiSettingsPage.inl` | 分项权限分三组（查找 / 文件名与元数据 / Playlist）；显示最近一次写入回执 |
| `src/ExternalAiPlaylistBridge.h/.cpp`（新） | Harken 侧管道服务端 + 请求校验 + 调用 Database + 回执落盘 |
| `src/ExternalAiMcp.cpp` | apply 工具改为发请求到管道；错误码映射 |
| `tests/test_external_ai_playlist_bridge.cpp`（新） | 服务端逻辑：鉴权、幂等、回滚、越权路径 |
| `tests/test_external_ai_mcp.py` | 端到端：preview → apply → 重复 apply → 撤权 |

## 6. 验收标准

AI 自验（隔离库，合成 WAV）：

1. preview 不产生任何写入（库文件、Playlist 均不变）。
2. 授权 `playlistEdit=edit` 时 apply 成功：Harken 出现主 Playlist + 子 Playlist，成员数量与清单一致。
3. 同一 `requestId` 再 apply：回执与首次一致，Playlist 成员不增加。
4. 把 `playlistEdit` 改成 none 后 apply：`not_authorized`，Playlist 不变。
5. 清单里混入未勾选目录的文件：`not_authorized`，**整体不写**。
6. 写入过程中模拟失败：事务回滚，Playlist 不存在或不含半批成员。
7. Harken 未运行：连接器返回 `playlist_write_unavailable`，不阻塞、不无限重试。

用户验收：在 Harken 里真的看到整理好的 Playlist，并能点「断开/取消写入」撤回。

## 7. 阶段

- **P1 授权面**：8 项能力 + 三组显示 + 文档键 + 保存生成 bridgeToken（2 组测试）
- **P2 桥**：管道服务端、请求校验、事务写入、回执落盘（单测）
- **P3 连接器**：apply 改走管道、错误码、超时（单测 + Python 端到端）
- **P4 界面**：最近写入回执显示
- **P5 验收收尾**：隔离构建实机、证据、文档、合并

范围外（本卡不做）：Harken 内二次确认弹窗（当前以「授权即同意 + 回执可查」为准，如需要再开卡）、音乐域、跨库批量、Playlist 重命名/删除。
