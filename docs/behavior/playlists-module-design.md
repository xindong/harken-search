# 播放列表范围模块详细设计（第3批）

本批只把播放列表侧栏目录、标题数量和封面分组从“已加载页”改成“完整成员 / 完整筛选结果”。不改 9/22 旧布局、不引入新 Dock、不动主线配置和库。

## 身份

- 树：未筛选的完整成员。普通列表走 getPlaylistFiles，聚合文件夹走 getAggregatedPlaylistFiles。身份键是 playlistFolderHierarchyPlaylistId。
- 计数 / 封面：同一次 Refresh 的完整筛选结果。身份键是 requestId + Playlist/playlistId/aggregated/filter。
- 标题数量只用 ResolvePlaylistResultTitleCount(matchCount, loadedPage)，格式用已有 L(Main_LldFmt)。

## 接口

- RefreshPlaylistFolderHierarchy：UI 线程用轻量 folder_path 计数刷新播放列表范围目录树，按 playlist id 与数据版本缓存，不再加载完整 AudioRecord。
- searchPlaylistCoverKeys / searchAggregatedPlaylistCoverKeys：SearchWorker 自有只读连接上的只读 SELECT，紧邻对应 FileCount。
- BuildPlaylistCoverGroupsFromCompleteMembers：用完整筛选键生成分组，不用当前页。
- AsyncSearchRefreshFacetsInput.playlistView / AsyncSearchFacetResultInput.playlistView：默认 false。

## 代 / 取消 / 过期 / 生命周期

- generation：filterQueryGeneration、pendingSearchRequestId、pendingFacetRequestId 与同请求 Refresh 主包绑定。
- cancel：latestRequestId + sqlite3_interrupt。封面查询前后都看 isRequestStale；过期不入队、不填 facetsComputed。
- stale-result：只接当前 pending 请求。过期封面不覆盖新状态、不清新 pending、不进持久缓存。
- 生命周期：Playlist 不发 FacetsOnly。成功 Refresh 在既有 RefreshFacetsStateAdapter 的 facetsChanged 路径里，仅当 viewMode==Playlist 且 kind==Refresh 且 pendingFacetRequestId==result.requestId 时清 0。较新请求不清。空结果 / 空封面 / 200 万上限不发后续包也必须结束等待。
- 播放列表不调 persistCoverFacetCache / persistPathFacetCache。

## 线程所有权

- SearchWorker 独占只读连接计算 count/page/cover keys。
- UI 线程 apply 发布 facets/数量/树和完成状态；树仍是来源的 UI 同步查询。
- 本批不启动拼音回填、不写主库、不改源音频。
- 树读完整成员可能卡 UI、cover 查询开销是来源残余；保留 200 万阈值和 interrupt，不借机重构。

## 独立测试

- TestPlaylistFolderTree / ScopeCounts / CoverScope：纯计算。
- 两个 workflow 与 adapter：刷新替换 / 空结果 / 不持久化。
- TestPreviewPlaylistDatabase：合成库 4 成员 page=2，普通与聚合查询。
- test_preview_playlist_contract.py：产品源码合同。
- 不冒充 GUI/E2E。

这是 745 页面结果限制所需的最小适配，不冒称来源整树原样，也不把整卡标为完成。

## r2 测试组织（待独立复核）

产品行为不重改。r2 只纠正树测试的跨文件断言位置与链接闭包。

- 树测试只链 PathUtils.cpp 与构建时已有的公共测试支持（filesystem/text_encoding 等）。不能链 Utils.cpp、harken_user_data_layout.cpp、harken_legacy_data_migrator.cpp。
- 原因：本隔离 overlay 的 Utils.cpp 会调用 PreviewIsolationActive/布局路径，再链 Utils 就要把真实迁移器拆进纯计算树测试。
- 路径编码只用测试桩 Utf8ToPath/PathToUtf8。这是转换边界证据，不是产品 Utils 转换函数。
- 展开清空与批量刷新断言读 main.cpp + PlaylistQuickBar.cpp 真实调用链：main 三角已展开分支清 -1 恰 1 处；QuickBar 删除二次确认且删的是当前列表时清 -1 恰 1 处；QuickBar 确认批量添加且目标为当前 Playlist 时 LoadPlaylistFiltered(..., false)。
- TestPreviewPlaylistDatabase 仍可保留现有 overlay layout/migrator 与 /FI 合成根，不扩大平台依赖。
- 证据分类：产品源码合同 / 转换桩 / 未 GUI。不冒充 E2E。
