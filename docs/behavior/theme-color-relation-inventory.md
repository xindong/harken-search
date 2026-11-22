# 高级主题编辑器色块关系盘点

盘点日期：2026-11-20

状态：源码关系盘点，不是颜色值验收，也不改变现有主题行为。

## 盘点口径

- **基础**：当前作为某个界面或语义轴的直接输入，没有被统一派生函数覆盖。
- **共享**：一个颜色被多个界面、组件或语义状态有意复用。
- **派生**：由其他颜色、透明度或混合公式生成，当前不应当作为独立基础色理解。
- **旧兼容**：为了兼容旧的 `ThemeColorScheme`、ImGui 样式或旧 JSON 字段而保留的平面字段。
- **意外耦合**：仅凭源码不能直接断言为错误；本表只把跨语义复用明显、且值得视觉确认的关系标成“疑似”。

当前证据主要来自：

- `src/Theme.h:40-182`：完整 `ThemeColorScheme` 字段。
- `src/ThemeEditorSchema.h:31-173`：高级编辑器字段清单与分组。
- `src/Theme.cpp:137-220`：语义默认值、派生色和 Wwise/Meter 关系。
- `src/Theme.cpp:740-791`：旧的简单自定义颜色批量覆盖。
- `src/Theme.cpp:1169-1286`：设计 Token 到兼容色表的映射。
- `src/Theme.cpp:1728-2072`：JSON 导出、导入和旧字段回填。
- `src/ThemeEditor.cpp:145-177,463-526`：编辑器草稿、预览和恢复边界。

## 关系总览

### 直接基础轴

当前最适合作为未来语义编辑入口的颜色轴：

`mainWindowBG`、`toolbarBG`、`interfaceText`、`resultsBG`、`resultsOutline`、`resultsSelection`、`metadataPanelBG`、`waveformBG`、`waveformColor`、`waveformPositionLine`、`filterBrowserBG`、`transferButtonBG`、`searchButtonBG`、`volumeColor`、`volumeClipColor`。

其中 `resultsSelection`、`transferButtonBG`、`volumeColor`、`volumeClipColor`、`waveformPositionLine` 和 `interfaceText` 的跨区域影响较多，不能直接当作普通单点颜色。

### 明确派生关系

- `frameBgBase <- toolbarBG`
- `chromeBorder <- resultsOutline`
- `popupBG <- toolbarBG`
- `menuPopupBG <- popupBG`
- `dialogBG <- popupBG + mainWindowBG`
- `dialogPanelBG <- dialogBG + frameBgBase`
- `toolbarAccent <- resultsSelection`
- `controlAccent <- toolbarAccent`
- `coverWallBG <- metadataPanelBG`
- `waveformPanelBorder / waveformSplitterBG / waveformSplitterLine / waveformFooterBG <- waveformBG、chromeBorder、controlAccent、resultsBG、mainWindowBG`
- `miniWaveColor <- waveformColor`
- `resultsActiveBorder <- controlAccent + resultsSelection`
- `filterBrowserSearchHitText / filterBrowserTagText <- filterBrowserText、filterBrowserTreeText、controlAccent`
- `meterBg / meterBase / meterMid / meterWarning / meterClip / meterScaleText <- waveformBG、chromeBorder、volumeColor、volumeClipColor、waveformPositionLine、resultsSecondaryText、interfaceText`
- `presetDial* <- mainWindowBG、toolbarBG、modeCapsuleActiveBG、interfaceText`
- Wwise 状态色主要由 `volumeColor`、`volumeClipColor`、`resultsSelection`、`interfaceText` 和固定青色轴派生。

## 逐字段盘点

### Main Window

| 字段 | 主分类 | 当前关系 | 编辑器 | 风险 / 结论 |
|---|---|---|---|---|
| `mainWindowBG` | 基础 | 主窗口画布根色，也是遮罩与部分表面混合基准 | 是 | 中：适合作为 `surface.canvas` |
| `menuBarBG` | 基础 | 菜单栏直接颜色；产品主题中可映射到画布色 | 是 | 中：与 `sidebarHeaderBG` 的关系需统一命名 |
| `menuText` | 共享 | 菜单文字、图标对比和部分 Wwise 青色派生的温度基准 | 是 | 中：基础文字轴，不宜随意批量替换 |
| `frameBgBase` | 派生（旧兼容） | 当前默认由 `toolbarBG` 回填，用作 ImGui 控件底色 | 是 | 中：保留 JSON 字段，但不应作为首要编辑入口 |
| `chromeBorder` | 共享（派生） | 当前默认来自 `resultsOutline`，并被波形、Info、弹层边界复用 | 是 | 高：边框语义被多个区域共享 |
| `popupBG` | 派生（旧兼容） | 由 `toolbarBG` 改透明度生成，服务 Tooltip/Popup | 是 | 中：应显示为“由工具栏表面派生” |
| `menuPopupBG` | 共享（派生别名） | 当前等同于 `popupBG`，用于菜单/Combo | 是 | 低：保留独立旧字段，默认跟随 |
| `dialogBG` | 派生（旧兼容） | 由 `popupBG` 与 `mainWindowBG` 混合 | 是 | 中：不应与基础表面并列展示 |
| `dialogPanelBG` | 派生（旧兼容） | 由 `dialogBG` 与 `frameBgBase` 混合 | 是 | 中：不应作为首要基础色 |
| `toolbarBG` | 基础 | 顶部工具栏外壳输入 | 是 | 中：适合作为 `surface.toolbar` 或 `surface.panel` 来源 |
| `historyCapsuleBG` | 派生 | 由工具栏和主窗口背景混合 | 是 | 低：可转为组件派生色 |
| `historyCapsuleActiveBG` | 派生 | 由控件底色与白色混合 | 是 | 中：与模式胶囊激活态存在共享公式 |
| `modeCapsuleBG` | 派生 | 由工具栏和主窗口背景混合 | 是 | 低：可转为组件派生色 |
| `modeCapsuleActiveBG` | 派生 | 由控件底色与白色混合 | 是 | 中：与历史胶囊激活态存在共享公式 |
| `toolbarAccent` | 共享（派生别名） | 当前默认等于 `resultsSelection`，服务工具栏和状态胶囊 | 是 | 高：选择色与工具栏强调色可能需要拆成两个语义角色 |
| `controlAccent` | 共享（派生别名） | 当前默认跟随 `toolbarAccent`，被大量通用控件使用 | 是 | 高：属于全局强调轴，不能当作普通局部颜色 |
| `transportBarBG` | 基础 | 播放控制栏外壳 | 是 | 中：适合作为独立组件表面 |
| `timecodeText` | 基础 | 时间码主文字 | 是 | 中：属于播放/测量语义轴 |
| `timecodeTextHover` | 基础 | 时间码悬停文字；产品主题中映射到焦点环色 | 是 | 中：当前名称与实际来源不完全一致 |
| `interfaceText` | 共享 | 通用文字、诊断文字、预设拨盘文字和 Wwise 中性色的来源 | 是 | 高：文字基础轴，不能只按某一个区域命名 |

### Results List

| 字段 | 主分类 | 当前关系 | 编辑器 | 风险 / 结论 |
|---|---|---|---|---|
| `resultsBG` | 基础 | 结果列表主表面 | 是 | 中：适合作为 `surface.results` |
| `resultsHeaderBG` | 基础 | 结果标题/筛选条表面 | 是 | 低：组件独立度较高 |
| `resultsText` | 基础 | 结果列表主文字；旧简单覆盖会跟随通用文字 | 是 | 中：基础色与旧批量覆盖并存 |
| `resultsOutline` | 基础 | 结果区边框，也是 `chromeBorder` 的默认来源 | 是 | 高：同时是局部边框和全局边框锚点 |
| `resultsSelection` | 共享 | 结果选中、工具栏强调、通用控件强调、Wwise Replaced 的共同来源 | 是 | 高：最明显的跨语义共享候选 |
| `resultsActiveBorder` | 派生 | 由 `controlAccent` 与 `resultsSelection` 混合并固定透明度 | 是 | 低：应从基础编辑区移到派生区 |
| `resultsScrollbar` | 基础 | 结果区滚动条，并参与部分悬停混合 | 是 | 中：仍是局部基础色 |
| `resultsTabBG` | 基础 | 结果页签普通背景 | 是 | 低：与全局 `tabBG` 不应混为一谈 |
| `resultsTabHover` | 基础 | 结果页签悬停背景 | 是 | 低：可保留组件状态色 |
| `resultsTabActive` | 基础 | 结果页签激活背景 | 是 | 低：可保留组件状态色 |
| `resultsSecondaryText` | 共享 | 结果弱文字，同时为电平表刻度和派生文字提供来源 | 是 | 中：跨列表与电平表共享 |
| `resultsPlayedText` | 基础 | 已播放文件文字 | 是 | 低：语义较明确 |
| `resultsGradientStart` | 派生 | 默认等于 `resultsBG` | 是 | 低：不应作为独立基础色 |
| `resultsGradientEnd` | 派生 | 由 `resultsBG` 与 `resultsSelection` 混合 | 是 | 低：应显示公式或继承状态 |
| `playlistGradientStart` | 共享（待确认） | 预设中通常与主强调/操作色同轴，但当前保留独立字段 | 是 | 中：需确认播放列表是否应跟随主强调色 |
| `playlistGradientEnd` | 共享（待确认） | 与播放列表渐变起点成对，通常属于操作色深阶 | 是 | 中：需确认是否独立保留 |
| `playlistGradientHighlight` | 派生 | 默认固定为白色高光 | 是 | 低：更像渲染参数而非主题基础色 |
| `playlistGradientShadow` | 派生 | 默认等于 `resultsBG` | 是 | 低：应跟随结果区表面 |

### Metadata Panel

| 字段 | 主分类 | 当前关系 | 编辑器 | 风险 / 结论 |
|---|---|---|---|---|
| `metadataPanelBG` | 基础 | Info/元数据面板主表面，也是封面墙默认来源 | 是 | 高：面板表面与封面墙有共享关系 |
| `metadataPanelText` | 基础 | Info 主文字，同时参与编辑器自身样式 | 是 | 中：产品语义和编辑器外壳共用 |
| `metadataPanelLabel` | 基础 | Info 标签文字，也参与编辑器边框/行样式 | 是 | 中：局部语义和编辑器 chrome 共用 |
| `metadataPanelScrollbar` | 基础 | Info 面板滚动条 | 是 | 低：局部字段 |
| `coverWallBG` | 派生 | 默认等于 `metadataPanelBG` | 是 | 中：如封面墙需要独立层次，应提供解除关联 |

### Waveform

| 字段 | 主分类 | 当前关系 | 编辑器 | 风险 / 结论 |
|---|---|---|---|---|
| `waveformBG` | 基础 | 波形主表面，也是 Footer/Meter 等派生基准 | 是 | 高：波形表面影响多个下游组件 |
| `waveformPanelBorder` | 派生 | 由波形背景、边框和强调色混合 | 是 | 中：当前直接编辑容易被重新派生覆盖 |
| `waveformSplitterBG` | 派生 | 由结果区背景和波形背景混合 | 是 | 低：组件结构色 |
| `waveformSplitterLine` | 派生 | 由 Splitter、边框和强调色混合 | 是 | 中：与 `sidebarSubpaneDivider` 有相似视觉职责但不是同一字段 |
| `waveformFooterBG` | 派生 | 由波形背景和主窗口背景混合 | 是 | 低：应跟随波形/画布层级 |
| `waveformColor` | 共享 | 主波形、Mini 波形和产品测量色来源 | 是 | 高：音频信号色与其他状态轴关联 |
| `waveformSecondaryColor` | 基础 | 波形辅助渐变色，当前没有统一派生公式 | 是 | 中：可作为波形组件局部覆盖 |
| `waveformPositionLine` | 共享（高风险） | 播放位置线，同时参与 Meter Warning 派生 | 是 | 高：播放进度和电平预警是不同语义，需重点确认 |
| `miniWaveColor` | 派生（共享别名） | 默认等于 `waveformColor` | 是 | 低：可提供独立覆盖，但默认应跟随 |
| `miniWaveBG` | 派生 | 默认透明 | 是 | 低：更像固定渲染策略 |
| `waveformSelectionDim` | 基础 | 波形未选区域遮罩 | 是 | 中：属于选择状态，不是普通波形主色 |
| `waveformSelectionHearthTop` | 基础 | 选择 Hearth 上沿颜色 | 是 | 中：专项视觉语义，当前未进入统一派生层 |
| `waveformSelectionHearthBot` | 基础 | 选择 Hearth 下沿颜色 | 是 | 中：与上沿成对管理 |
| `waveformSelectionEdge` | 基础 | 选择边缘颜色 | 是 | 中：与选择状态成组管理 |

### Filter Browser

| 字段 | 主分类 | 当前关系 | 编辑器 | 风险 / 结论 |
|---|---|---|---|---|
| `filterBrowserBG` | 基础 | 筛选器主表面，也是同级面板表面默认来源 | 是 | 高：侧栏、停靠面板和结果外壳有共享影响 |
| `filterBrowserText` | 基础 | 筛选器文字，旧简单覆盖会跟随通用文字 | 是 | 中：基础文字与局部文字共存 |
| `filterBrowserHeading` | 基础 | 筛选器标题，也是同级面板标题默认来源 | 是 | 中：区域标题和公共面板标题共享 |
| `filterBrowserOutline` | 共享 | 筛选器边框、同级面板边框和 Info 停靠边缘来源 | 是 | 高：跨侧栏、Info、相似面板共享 |
| `filterBrowserSelection` | 共享 | 筛选器选中、通用强调轴的一部分 | 是 | 高：与 `resultsSelection` 可能需要拆分或绑定 |
| `filterBrowserScrollbar` | 共享 | 筛选器滚动条，也是同级面板滚动条默认来源 | 是 | 中：共享合理，但应显示影响范围 |
| `filterBrowserSectionBG` | 基础 | 筛选分组/折叠头表面 | 是 | 中：与侧栏子面板可能存在层级相似关系 |
| `filterBrowserButtonBG` | 基础 | 筛选区按钮表面 | 是 | 低：局部按钮底色 |
| `filterBrowserTreeText` | 基础 | 文件夹树文字，同时参与搜索命中派生 | 是 | 中：树文字与命中状态需分开理解 |
| `filterBrowserTreeSelection` | 共享 | 文件夹树选中，旧简单覆盖会跟随强调色 | 是 | 高：与普通筛选选中存在共享关系 |
| `filterBrowserSearchHitText` | 派生 | 由强调色和树文字混合 | 是 | 低：应移到派生/状态区 |
| `filterBrowserLocateTint` | 意外耦合（疑似） | 当前等于 `transferButtonBG`，定位高亮和传输按钮共用一条颜色轴 | 是 | 高：建议确认后拆成 `status.locate` 或显式绑定 |
| `filterBrowserTagText` | 派生 | 由筛选文字和通用强调色混合 | 是 | 中：可保留“跟随”关系 |

### Buttons and Status

| 字段 | 主分类 | 当前关系 | 编辑器 | 风险 / 结论 |
|---|---|---|---|---|
| `transferButtonBG` | 共享（高风险） | 传输按钮成功色、筛选定位色和 Wwise ConnectOk 的共同来源 | 是 | 高：成功、定位、连接成功未必应长期共用 |
| `searchButtonBG` | 疑似意外耦合 | 搜索按钮主操作色，部分旧路径会把它设为控件强调色，但内置主题允许两者不同 | 是 | 高：应显示为疑似关系，不应无条件宣称跟随 |
| `transferHistoryBG` | 基础 | 传输历史表面，旧简单覆盖会跟随子表面 | 是 | 中：区域基础色 |
| `transferHistoryText` | 基础 | 传输历史文字，旧简单覆盖会跟随通用文字 | 是 | 低：区域文字 |
| `spottingBG` | 基础 | Spotting 表面，旧简单覆盖会跟随子表面 | 是 | 中：区域基础色 |
| `spottingText` | 基础 | Spotting 文字，旧简单覆盖会跟随通用文字 | 是 | 低：区域文字 |
| `diagnosticOverlayBG` | 派生 | 由对话框面板色加透明度生成 | 是 | 低：应跟随弹窗层级 |
| `diagnosticOverlayText` | 共享（派生别名） | 默认等于 `interfaceText` | 是 | 中：诊断层可允许独立覆盖 |
| `diagnosticOverlayAccent` | 共享（派生别名） | 默认等于 `toolbarAccent` | 是 | 中：诊断强调是否跟随全局强调需确认 |
| `diagnosticOverlayMutedText` | 派生 | 由通用文字和诊断背景混合 | 是 | 低：应移到派生区 |

### Tab and Level Meter

| 字段 | 主分类 | 当前关系 | 编辑器 | 风险 / 结论 |
|---|---|---|---|---|
| `tabBG` | 旧兼容 | ImGui 全局 Tab 基线；Hover/Active 在应用样式时继续派生 | 是 | 中：与 `resultsTab*` 是两套层级，需避免用户混淆 |
| `volumeColor` | 共享（高风险） | 音量/电平成功色、Meter 基础段和 Wwise ConnectOk 来源 | 是 | 高：音频测量与连接状态语义共享 |
| `volumeClipColor` | 共享（高风险） | 削波色、Meter Clip 和 Wwise Fail/Unresolved 来源 | 是 | 高：错误、削波、未解决状态需要确认是否同轴 |
| `meterBg` | 派生 | 由波形背景与边框混合 | 是 | 低：应作为派生色 |
| `meterBase` | 派生 | 由 Meter 背景、弱文字和 `volumeColor` 混合 | 是 | 中：不应与 `volumeColor` 并列为同一语义 |
| `meterMid` | 派生 | 由基础段和预警段混合 | 是 | 低：典型派生色 |
| `meterWarning` | 派生（疑似意外耦合） | 由 `waveformPositionLine` 和 Meter 基础段混合 | 是 | 高：播放位置线与 Meter 预警跨语义复用 |
| `meterClip` | 派生 | 由 `volumeClipColor` 混合生成 | 是 | 低：应跟随削波语义轴 |
| `meterScaleText` | 派生 | 由 `resultsSecondaryText`、`timecodeText` 或 `interfaceText` 推导 | 是 | 中：结果弱文字和电平刻度文字跨区域共享 |

### Preset Dial

| 字段 | 主分类 | 当前关系 | 编辑器 | 风险 / 结论 |
|---|---|---|---|---|
| `presetDialBg` | 派生 | 由主窗口和工具栏背景混合 | 是 | 低：拨盘组件派生表面 |
| `presetDialCardActiveBg` | 派生 | 默认等于 `modeCapsuleActiveBG` | 是 | 中：拨盘卡片与模式胶囊共享激活层级 |
| `presetDialCardActiveText` | 派生 | 默认等于 `interfaceText` | 是 | 低：激活文字跟随主文字 |
| `presetDialCardInactiveText` | 派生 | 默认等于 `interfaceText`，由绘制距离再降阶 | 是 | 低：不要单独暴露为基础色 |
| `presetDialAccent` | 派生 | 由 `interfaceText` 与激活卡片背景混合 | 是 | 中：拨盘强调不是通用强调色 |

### Sidebar Activity Bar（当前未进入编辑器）

| 字段 | 主分类 | 当前关系 | 编辑器 | 风险 / 结论 |
|---|---|---|---|---|
| `sidebarRailBG` | 共享（隐含） | 产品主题中跟随画布；侧栏图标栏独立使用 | 否 | 中：应作为侧栏组件角色，而不是完全隐藏 |
| `sidebarRailDivider` | 基础 | 侧栏 Rail 分割线 | 否 | 低：局部结构色 |
| `sidebarIconInactive` | 共享（隐含） | 产品主题中跟随弱文字 | 否 | 中：应显示来源和影响范围 |
| `sidebarIconHover` | 共享（隐含） | 产品主题中跟随次要文字 | 否 | 中：状态文字轴共享 |
| `sidebarIconActive` | 共享（隐含） | 产品主题中跟随主强调色 | 否 | 高：与激活条共同变化 |
| `sidebarActiveStrip` | 共享（别名） | 默认等于 `sidebarIconActive` | 否 | 中：可保留跟随，允许解除关联 |
| `sidebarHeaderBG` | 共享（别名） | 产品主题中等于 Rail 背景 | 否 | 低：应显示“跟随侧栏 Rail” |
| `sidebarStatusBarBG` | 共享（隐含） | 产品主题中跟随面板表面 | 否 | 中：属于侧栏层级，不应与主窗口混为一色 |
| `sidebarSubpaneBG` | 共享（隐含） | 产品主题中跟随 Elevated；用于侧栏中间子面板 | 是 | 中：与筛选分组/波形分隔层有相似层级 |
| `sidebarSubpaneDivider` | 共享（隐含） | 产品主题中跟随强边框；用于子面板分割 | 是 | 中：可与波形结构线共享基础轴，但不应直接绑定字段 |
| `sidebarSubHeaderLine` | 基础 | 子标题延伸线的专用颜色 | 否 | 低：局部结构色 |

### Wwise Transfer（当前未进入编辑器）

| 字段 | 主分类 | 当前关系 | 编辑器 | 风险 / 结论 |
|---|---|---|---|---|
| `wwiseConnectOk` | 派生 | 由 `volumeColor` 的绿色轴、文字色混合 | 否 | 高：连接成功与音量成功色存在跨域共享 |
| `wwiseConnectFail` | 派生 | 由 `volumeClipColor` 与文字色混合 | 否 | 中：连接失败与削波错误可能同轴，但需确认 |
| `wwiseDiffNeutral` | 派生 | 由 `interfaceText` 与 `chromeBorder` 混合 | 否 | 中：中性色来源跨通用文字和边框 |
| `wwiseDiffNew` | 派生 | 固定青色轴再向 `menuText` 靠拢 | 否 | 中：当前不是单纯继承某个基础 Token |
| `wwiseDiffReplaced` | 派生（疑似意外耦合） | 由 `resultsSelection` 的暖色轴与文字混合 | 否 | 高：列表选中与 Wwise 替换状态是两个语义 |
| `wwiseDiffUnresolved` | 派生 | 由 `volumeClipColor` 与文字混合 | 否 | 中：未解决与削波共用错误轴，需确认 |
| `wwiseLeafBadge` | 派生 | 由绿色轴和青色轴混合 | 否 | 低：Wwise 内部层级专用派生色 |
| `wwiseParentBadge` | 派生 | 由青色轴和文字色混合 | 否 | 低：Wwise 内部层级专用派生色 |

## 当前最值得先确认的耦合

以下关系不能仅靠字段名称决定，应在真实界面中逐项确认：

1. `resultsSelection -> toolbarAccent -> controlAccent`：结果选中、工具栏状态和全局控件是否真的应该同色。
2. `transferButtonBG -> filterBrowserLocateTint -> wwiseConnectOk`：传输成功、目录定位、Wwise 连接成功是否共用成功色。
3. `waveformPositionLine -> meterWarning`：播放位置线和电平预警是否共用暖色来源。
4. `volumeColor / volumeClipColor -> Wwise 状态色`：音频电平状态和连接/差异状态是否应继续共用颜色轴。
5. `interfaceText / resultsSecondaryText -> diagnostic、preset dial、meter`：通用文字层级是否足够覆盖这些不同组件。
6. `metadataPanelBG -> coverWallBG`、`waveformColor -> miniWaveColor`、`sidebarRailBG -> sidebarHeaderBG`：这些更像合理的默认跟随，但仍应支持“解除关联”。

## 盘点结论

- 现有问题不是所有字段都需要拆开，而是“共享、派生、兼容”没有在编辑器中被表达出来。
- `ThemeColorScheme` 仍应保留为稳定渲染和旧 JSON 兼容接口，不能直接改名或删除字段。
- `ThemeEditorSchema.h` 后续需要从“成员指针清单”升级成“字段关系描述”，至少补充来源、派生公式、影响范围、是否允许解除关联和旧字段状态。
- 当前不建议直接改已有主题数值，也不建议先把所有字段都改成新的语义 Token。
- 下一步应先对“当前最值得先确认的耦合”做实机对照，再决定哪些共享关系保留、哪些拆开。

## 建议决策清单

以下是基于当前源码关系的建议，不等于用户已经确认的最终设计。这里的“保留共享”统一指“新语义主题中默认跟随，但允许组件解除关联”，不建议继续使用一个简单输入直接把多个平面字段永久写成同一个值。

### 建议保留为共享默认

| 关系 | 建议 | 原因 |
|---|---|---|
| `popupBG -> menuPopupBG` | 保留共享 | 都属于弹层/菜单表面，默认应保持同一层级 |
| `metadataPanelBG -> coverWallBG` | 保留共享，可解除 | 封面墙通常属于 Info 面板的同层级表面，但特殊主题可能需要独立层次 |
| `waveformColor -> miniWaveColor` | 保留共享，可解除 | Mini 波形是同一音频信号的缩略表达，默认同色最容易理解 |
| `filterBrowserSelection <-> filterBrowserTreeSelection` | 保留共享，可解除 | 都是左侧筛选区的选中状态，属于同一选择语义 |
| `sidebarRailBG -> sidebarHeaderBG` | 保留共享，可解除 | Rail 和侧栏标题属于同一侧栏外壳层级 |
| `sidebarIconActive -> sidebarActiveStrip` | 保留共享，可解除 | 激活图标与激活条应共同表达当前侧栏位置 |
| `filterBrowserOutline -> peerPanelOutline` | 保留共享，可解除 | 筛选器、Info 和相似面板当前使用同一套外壳边界语言 |
| `filterBrowserScrollbar -> peerPanelScrollbar` | 保留共享，可解除 | 同级面板滚动条无需默认产生不同色块 |
| `interfaceText -> diagnosticOverlayText` | 保留共享，可解除 | 诊断浮窗普通文字默认应属于同一主文字层级 |
| `interfaceText -> presetDialCardActiveText` | 保留共享，可解除 | 拨盘激活文字默认跟随主界面文字，避免出现孤立文字色 |
| `volumeColor -> meterBase` | 保留共享为派生来源 | 都表达正常音频能量轴；Meter 仍通过明暗/混合生成自己的显示色 |
| `volumeClipColor -> meterClip` | 保留共享为派生来源 | 削波语义需要在 Meter 中保持同一危险色轴 |
| `resultsBG -> resultsGradientStart / playlistGradientShadow` | 保留共享为派生来源 | 渐变的起始/回收端应默认回到结果区表面 |
| `modeCapsuleActiveBG -> presetDialCardActiveBg` | 保留共享为派生来源 | 两者都表达顶部导航/拨盘的激活层级 |

### 建议拆成独立语义

这些关系可以保留当前颜色作为初始值，但不应继续使用同一个语义 Token 或同一个编辑输入控制它们：

| 当前关系 | 建议拆分为 | 原因 |
|---|---|---|
| `resultsSelection -> toolbarAccent` | `selection.results` / `accent.toolbar` | 结果选中是内容状态，工具栏强调是操作状态 |
| `toolbarAccent -> controlAccent` | `accent.toolbar` / `accent.control` | 顶部状态胶囊和全局控件未来可能需要不同层级 |
| `resultsSelection -> WwiseDiffReplaced` | `selection.results` / `status.replaced` | 列表选中不应自动决定 Wwise 差异状态 |
| `transferButtonBG -> filterBrowserLocateTint` | `action.transfer` / `status.locate` | 传输操作与目录定位不是同一种反馈 |
| `transferButtonBG -> wwiseConnectOk` | `action.transfer` / `status.connected` | 传输按钮和连接成功状态的交互语义不同 |
| `volumeColor -> wwiseConnectOk` | `signal.level` / `status.success` | 音频电平色不能直接代表连接成功 |
| `volumeClipColor -> wwiseConnectFail / WwiseDiffUnresolved` | `signal.clip` / `status.error` / `status.unresolved` | 削波、连接失败和未解决节点虽然都偏危险色，但用户含义不同 |
| `waveformPositionLine -> meterWarning` | `signal.playhead` / `status.warning` | 播放位置和电平预警是两个完全不同的阅读任务 |
| `resultsSecondaryText -> meterScaleText` | `text.secondary` / `text.meterScale` | 列表弱信息与电平刻度的对比需求不同 |
| `resultsOutline -> chromeBorder` | `border.results` / `border.strong` | 结果区边界与全局窗口边界不应永久互相覆盖 |
| `playlistGradientStart / End -> resultsSelection` | `gradient.playlistStart / End` | 播放列表渐变是视觉装饰，不应由结果选中状态直接驱动 |
| `timecodeTextHover -> focus ring` | `text.timecode` / `focus.ring` | 悬停文字和键盘/控件焦点是不同状态 |
| `diagnosticOverlayAccent -> toolbarAccent` | `accent.diagnostic` / `accent.toolbar` | 诊断浮窗可以保留默认跟随，但应允许独立表达诊断状态 |

### 建议继续派生，但不作为基础编辑项

以下字段不建议消除关系，而是改成“显示公式/来源”的只读派生项：

- `frameBgBase`、`popupBG`、`dialogBG`、`dialogPanelBG`。
- `historyCapsuleBG`、`historyCapsuleActiveBG`、`modeCapsuleBG`、`modeCapsuleActiveBG`。
- `resultsActiveBorder`、`resultsGradientStart`、`resultsGradientEnd`、`playlistGradientHighlight`、`playlistGradientShadow`。
- `waveformPanelBorder`、`waveformSplitterBG`、`waveformSplitterLine`、`waveformFooterBG`、`miniWaveBG`。
- `filterBrowserSearchHitText`、`filterBrowserTagText`、`diagnosticOverlayBG`、`diagnosticOverlayMutedText`。
- `meterBg`、`meterBase`、`meterMid`、`meterWarning`、`meterClip`、`meterScaleText`。
- `presetDialBg`、`presetDialCardActiveBg`、`presetDialCardActiveText`、`presetDialCardInactiveText`、`presetDialAccent`。
- Wwise 的 `wwise*` 字段，除非后续确认某个状态需要用户独立调色，否则先作为状态语义的派生输出。

### 建议保留为旧兼容，不进入新的基础语义层

- `frameBgBase`、`popupBG`、`dialogBG`、`dialogPanelBG`：保留字段和 JSON 读写，但不再作为新编辑器的第一层颜色。
- `tabBG`：继续作为 ImGui 全局 Tab 兼容输入；它与 `resultsTabBG / resultsTabHover / resultsTabActive` 分开管理。
- 旧 JSON 中未带版本的主题：保持当前平面值，不自动重新建立上述共享关系。

## 建议采用的总规则

1. **同一视觉含义才共享**：同层级表面、同一选择状态、同一音频信号轴可以默认共享。
2. **不同用户含义必须拆开**：内容选中、操作强调、连接成功、削波、播放位置和诊断状态不能因为颜色相近就共用一个 Token。
3. **共享只做默认绑定**：不再用一次编辑直接覆盖多个平面字段；解除关联后只影响当前组件。
4. **旧主题不自动重连**：旧主题继续按原值运行，新语义绑定只应用于新建或明确转换的主题。
5. **派生色不伪装成基础色**：保留实时预览，但在数据层记录来源和公式。

## 高级编辑器优化方案（提案）

目标：优化编辑器的理解和管理方式，但不改变现有内置主题、已有自定义主题、日夜主题文件或渲染输出。

### 第一阶段：只增加关系层，不改变编辑行为

- 保留 `ThemeColorScheme`、`ThemeEditorItem` 的旧字段和现有 JSON 读写。
- 新增独立的编辑器关系描述，不把关系元数据写进运行时颜色结构。
- 现有主题全部按“旧平面主题”打开；每个颜色仍只修改自己的草稿字段。
- 编辑器新增“来源/跟随/派生/兼容”标记和影响范围查看，但第一阶段不自动批量改色。
- 继续复用当前 `draft -> ApplyDraftToLive -> cancel restore` 流程，实时预览只接收解析后的平面 `ThemeColorScheme`。
- 日夜配色继续沿用现有两份文件和现有保存路径，本阶段不引入新的日夜主题格式。

这一阶段可以先改善“看得懂、找得到、知道谁影响谁”，但不会因为打开新版编辑器就改变任何颜色。

### 第二阶段：只对新主题开放语义编辑

- 增加明确的“另存为语义主题”动作，不把已有主题自动转换。
- 语义主题保存基础色、绑定关系、独立覆盖和最终解析后的平面颜色。
- 旧的 `theme_custom.json` 继续保留为可直接读取的平面结果；新关系数据使用版本化元数据或旁车文件保存。
- 关系编辑只作用于当前草稿：修改共享基础色时更新关联目标；点击解除关联后，当前目标变为独立覆盖。
- 任何语义转换都需要预览差异，并允许取消；旧主题不因读取或升级而重连。

### 第三阶段：验证后再扩大范围

- 对五套内置主题逐项比较解析前后的 113 个颜色字段，要求完全不变。
- 对已有自定义主题做旧 JSON 导入、保存、重启和取消回滚，要求颜色值不变。
- 对白天/黑夜主题继续跑原有保存、切换、重启和手动主题恢复检查，不把语义元数据混入其现有文件。
- 对“解除关联”做单项 Undo/Redo 和取消编辑检查，确认不会污染打开前快照。
- 只有上述证据通过后，才考虑把部分隐藏 Sidebar/Wwise 字段纳入语义编辑。

### 编辑器界面分层建议

第一阶段的界面可以先按三层呈现，不改变字段值：

1. **基础颜色**：只放基础语义轴，例如画布、面板、文字、边框、操作强调、选择、波形和状态色。
2. **区域颜色**：显示最终色块、当前来源和“跟随/独立”状态；默认关系先只读观察。
3. **高级与兼容**：显示派生公式、旧字段和当前影响范围；旧字段继续可编辑，但明确标为兼容项。

第一阶段不增加长说明，只保留必要状态文字：`跟随`、`独立`、`由公式生成`、`兼容字段`、`影响范围`。

### 不采用的做法

- 不直接改写 `ThemeColorScheme` 字段含义。
- 不把五套内置主题转换成共享 Token 存储。
- 不打开编辑器就自动把多个旧字段合并成一个颜色。
- 不让一个基础色编辑动作绕过草稿和 Undo，直接写入多个运行时全局。
- 不先改日夜主题文件格式。

## 2026-11-21 首批落地（只改关系层，颜色值全部未变）

本批只调整 `src/ThemeEditorRelations.h` 的关系声明和编辑器标记，没有改动 `ThemeColorScheme` 字段、内置主题数值、JSON 读写或渲染公式。

### 已拆成独立轴

| 字段 | 原关系 | 现关系 | 原因 |
|---|---|---|---|
| `toolbarAccent` | 共享（跟随 `resultsSelection`） | 独立 | 内容选中与工具栏状态是两个阅读任务 |
| `controlAccent` | 共享（跟随 `toolbarAccent`） | 独立 | 全局控件强调不随工具栏状态联动 |
| `chromeBorder` | 共享（跟随 `resultsOutline`） | 独立 | 列表边框不该充当全窗口强边框的来源 |
| `filterBrowserLocateTint` | 疑似共用（`transferButtonBG`） | 独立 | 目录定位是导航反馈，不是传输入口 |
| `searchButtonBG` | 疑似共用（`controlAccent`） | 独立 | 主操作按钮与通用强调轴分开 |
| `wwiseDiffReplaced` | 疑似共用（`resultsSelection`） | 独立 | 列表选中不代表传输结果「已替换」 |
| `meterWarning` | 疑似共用（`waveformPositionLine`） | 派生（来源改为 `meterBase`） | 电平预警不再挂在播放位置色上 |

`ThemeEditorRelationKind::Independent` 是本批新增的标记，编辑器里显示「独立」，与「共享（可关联/解除）」区分，并且不能通过解除/关联按钮被重新绑回去。

### 已合并为一条轴

| 字段 | 原关系 | 现关系 | 原因 |
|---|---|---|---|
| `presetDialCardActiveText` | 派生（不可关联） | 共享，来源 `interfaceText` | 与其他主文字确属同一层级，五套内置主题实测同值 |

### 实测后否决的合并（不能做，会改颜色或没有效果）

先核对五套内置主题的实际取值，只有值本来相同的对子才适合合并；否则加载时会被判为「独立」，合并只是标注，不会生效。

| 原建议 | 实测结果 | 结论 |
|---|---|---|
| `metadataPanelBG → coverWallBG` | 5/5 主题不同 | 保留现有共享声明，但实际每套都是独立值 |
| `modeCapsuleActiveBG → presetDialCardActiveBg` | 5/5 不同（卡片激活底固定为浅色） | 不合并 |
| `interfaceText → presetDialCardInactiveText` | 5/5 不同（刻意为降阶文字） | 不合并，属于有意区分的层级 |
| `historyCapsuleActiveBG + modeCapsuleActiveBG` | Dark 明显不同（ff4a5668 / ff1e3a70） | 不合并 |
| `historyCapsuleBG + modeCapsuleBG` | Ocean、JadeMist 差 1 个色阶 | 不合并（合并需改公式，会改颜色） |
| `waveformColor → miniWaveColor` | Ocean、LapisBlue 不同（LapisBlue 迷你波形是另一色） | 不合并 |
| `popupBG → menuPopupBG` | LapisBlue 不同 | 保持现状 |
| `filterBrowserSelection → filterBrowserTreeSelection` | LapisBlue、JadeMist 不同 | 保持现状 |
| `sidebarRailBG → sidebarHeaderBG` | LapisBlue 不同 | 保持现状 |
| `resultsBG → playlistGradientShadow` | RoseCedar、LapisBlue、JadeMist 不同 | 不合并 |
| `filterBrowserSectionBG → sidebarSubpaneBG`、`filterBrowserOutline → sidebarSubpaneDivider` | 仅 LapisBlue 相同 | 不合并 |

### 仍然只标派生、暂不拆的

`volumeColor → wwiseConnectOk`、`volumeClipColor → wwiseConnectFail/wwiseDiffUnresolved`、`meterBase/meterClip/meterScaleText` 等仍是派生关系：这些值由 `ApplySemanticThemeDefaults` 的公式算出，拆成独立轴必须改公式，会改变现有主题颜色，留到确认公式方案后再做。

### 验证

- `TestThemeColors`：433 项通过，1 项失败为既有问题（`sidebar keeps visible 1px input framing…` 检查 `src/main.cpp` 的旧字面量，与本批无关）。
- 新增断言：五套内置主题经「读取 → 关系层初始化 → 传播」后，`exportToJSON` 输出逐字节相同，且每个可编辑色的解析值不变。
- 新增断言：`toolbarAccent` / `controlAccent` / `chromeBorder` / `wwiseDiffReplaced` / `filterBrowserLocateTint` / `searchButtonBG` 不再能被「关联」按钮绑回旧来源。
- 主程序 `HarkenSearch` 增量编译通过。

### 实机截图对照（同库、同隔离包、只换 EXE）

- 方法：`tools/qa_theme_color_relations.py`，隔离包私有桌面，不占用用户鼠标，库为 4,260 个文件的测试库；配色改动一律「取消修改」，不落盘。
- 五套内置主题的主窗口（经典深色、极光青、深海军蓝、海玻璃、柔雾粉）逐像素比对：**全部 0 像素差异**。
- 高级主题编辑器每套差 3,282~3,286 像素，且全部落在颜色行文字区 `(108,359)-(732,659)`，即「共享/疑似共用/派生 → 独立」的标记变化，色块本身未变。
- 解耦实测（空白库，白天配色）：把「工具栏点缀」改成绿色，改动前带动 87,688 个像素（编辑器按钮、右侧工具栏、侧栏图标、播放栏），改动后 38,126 个像素，只影响其自身负责区域。
- 证据与对照报告：`D:/Harken-QA/THEME-COLOR-RELATION-20261121/report.html`（自包含单文件），截图目录同路径下 `library-before/`、`library-after/`、`capture-before/`、`capture-after/`。

## 2026-11-21 第二批：把写死的主题色接进 JSON

### 已修：14 个字段此前既不导出也不导入

用户指出「写死的颜色导入 JSON 永远替换不了」。按字段逐个核对导出/导入两张表，确认以下字段只在主题构造里赋值，从未进过 `theme_*.json`：

| 分组 | 字段 |
|---|---|
| 侧栏 | `sidebarRailBG`、`sidebarRailDivider`、`sidebarIconInactive`、`sidebarIconHover`、`sidebarIconActive`、`sidebarActiveStrip`、`sidebarHeaderBG`、`sidebarStatusBarBG`、`sidebarSubHeaderLine` |
| 预设拨盘 | `presetDialBg`、`presetDialCardActiveBg`、`presetDialCardActiveText`、`presetDialCardInactiveText`、`presetDialAccent` |

现在 `exportToJSON` 与 `importFromJSON` 都覆盖这 14 项：

- 旧 JSON 不含这些键时保持主题原有取值，颜色不变（已加断言）。
- 新 JSON 写入这些键即可替换，颜色可被完全接管。
- 它们目前仍未进入高级编辑器的可编辑列表，界面上仍不可调。

### 仍待处理：派生公式里的固定色锚点

这些锚点参与默认值计算，不从主题读取，因此在主题之间不会"统一改变"（导出后的输出字段本身可被 JSON 覆盖）：

| 位置 | 固定值 | 影响字段 |
|---|---|---|
| `DeriveMeterWarning` | `(0.96, 0.88, 0.72)` 暖白 | `meterWarning` |
| `DeriveMeterClip` | `(0.84, 0.67, 0.64)` + 灰 0.70 | `meterClip` |
| `DeriveMeterBase` | 灰 0.62 | `meterBase` |
| 胶囊激活底 | 纯白 72% | `historyCapsuleActiveBG`、`modeCapsuleActiveBG` |
| 播放列表高光 | 纯白 | `playlistGradientHighlight` |
| Wwise 绿/青轴 | `(0.42, 0.88, 0.48)`、`(0.38, 0.76, 0.90)` | `wwiseConnectOk`、`wwiseDiffNew`、`wwiseLeafBadge`、`wwiseParentBadge` |
| 菜单文字对比兜底 | 纯白/纯黑 | `MenuTextFor` 返回色 |

处理方式二选一，均需用户确认：

1. **提成字段**：新增锚点色字段，默认值等于当前固定值，颜色不变，JSON 和编辑器都能改。
2. **换成主题色锚点**：更"统一"，但会改变现有五套主题的实际观感，需逐项比对差异。

## 2026-11-21 第三批：派生锚点已提为字段（颜色未变）

按「同类合并、不同类分出、不改现有颜色」处理第二批列出的固定锚点。

### 合并（原本是同一个东西）

| 合并后的字段 | 默认值 | 原来的写死值 | 合并理由 |
|---|---|---|---|
| `themeHighlight` | 纯白 | 胶囊激活底用纯白 72%、`playlistGradientHighlight` 用纯白 | 都是"向纯白提亮"，两处值完全相同 |
| `meterNeutralAnchor` | 灰 0.62 | `meterBase` 用灰 0.62、`meterClip` 用灰 0.70 | 都是中性去饱和锚点；0.70 = 0.62 向白 21.05% 混合，公开的仍是一个灰，默认值精确等于原值 |

### 分出（用途不同，必须各自独立）

| 字段 | 默认值 | 影响 |
|---|---|---|
| `meterWarnAnchor` | 暖白 (0.96,0.88,0.72) | `meterWarning` |
| `meterClipAnchor` | 珊瑚 (0.84,0.67,0.64) | `meterClip` |
| `wwiseOkAnchor` | 绿 (0.42,0.88,0.48) | `wwiseConnectOk`、`wwiseLeafBadge` |
| `wwiseNewAnchor` | 青 (0.38,0.76,0.90) | `wwiseDiffNew`、`wwiseLeafBadge`、`wwiseParentBadge` |

### 保持写死（说明）

`MenuTextFor()` 在菜单底色上按对比度返回纯白或纯黑，属于可读性兜底，不是主题色，未提为字段。

### 验证

- 六套锚点默认值与旧写死值逐项相同。
- 新增金标测试：五套内置主题导出 JSON 在剔除六个新锚点键后，FNV-1a 指纹与改动前完全一致（0 个颜色变化）。
- `TestThemeColors`：457 项通过，1 项失败为既有问题（`src/main.cpp` 旧字面量，与本批无关）。
- 六个锚点已进入 `exportToJSON` / `importFromJSON`，JSON 可覆盖。

## 2026-11-21 第四批：同类文字/表面合并为一条轴

做法：先导出五套内置主题全部 119 个色值，穷举"在所有主题里取值完全相同"的字段对（共 11 对），再按语义筛选，只合并用途相同的一类。

### 已合并（合并后颜色零变化）

| 合并后的轴 | 跟随字段 | 依据 |
|---|---|---|
| 正文文字 `menuText` | `resultsText`、`metadataPanelText`、`spottingText` | 五套主题里四个值完全相同；都是"区域正文" |
| 侧栏文字 `filterBrowserText` | `filterBrowserTreeText` | 五套完全相同；同属侧栏文字 |
| 次要文字 `resultsSecondaryText` | `presetDialCardInactiveText` | 五套完全相同；都属次要层级 |
| 次级面板底 `transferHistoryBG` | `spottingBG` | 五套完全相同；同层级面板 |

`sidebarIconActive → sidebarActiveStrip`、`interfaceText → presetDialCardActiveText` 已在第三批合并。

### 同性质但实测值不同（未合并，需决定是否接受色差）

| 候选 | 各主题差异 | 说明 |
|---|---|---|
| 三处滚动条统一 | LapisBlue 差 3、JadeMist 差 13（其余三套完全相同） | 同属滚动条，可统一但会轻微改色 |
| `resultsOutline` 与 `filterBrowserOutline` | LapisBlue 差 20、RoseCedar 差 31（其余三套相同） | 同属区域边框 |
| `tabBG` 与 `resultsTabBG` | RGB 全同，仅 alpha `ff` 与 `f4` | 两套页签底 |
| `filterBrowserButtonBG` 与 `filterBrowserSectionBG` | 各主题差 7~35 | 筛选按钮底与分组底 |
| `sidebarStatusBarBG` 并入 `sidebarRailBG` | 各主题差 5~12 | 侧栏状态栏与侧栏底 |
| `resultsSecondaryText` 与 `metadataPanelLabel` | 差 13~89（JadeMist 最大） | 列表弱文字与信息区标签 |

### 不能合并（有真实设计差异）

- 主文字：LapisBlue 侧栏文字是深色 `ff203b55`，其他区域是浅色，合并会看不清。
- 区域表面：JadeMist 侧栏底是纯白，与画布/列表底不同层级。
- 侧栏分隔线：JadeMist、RoseCedar 用黑色半透明，其余用主题青色。

### 验证

- 五套内置主题金标指纹仍与基准一致：合并未改动任何现有颜色。
- `TestThemeColors`：466 项通过，1 项失败为既有问题（`src/main.cpp` 旧字面量，与本批无关）。
- 新增断言：四个合并轴加载即联动、可单项解除关联、解除后不影响其他区域。

## 2026-11-21 第五批：滚动条、区域边框、页签底合并（已落地）

用户看过前后对照后确认合并。做法：`MakeBuiltinScheme` 末尾统一取值，主题里不再各写一份。

| 合并后的轴 | 跟随字段 | 说明 |
|---|---|---|
| `resultsScrollbar` | `metadataPanelScrollbar`、`filterBrowserScrollbar` | 五套主题屏幕无可见变化（滚动条自动隐藏，列表滚动条本就是基准） |
| `resultsOutline` | `filterBrowserOutline` | 深海军蓝边线变深（20/255）、柔雾粉变浅（31/255），其余三套本来相同 |
| `tabBG` | `resultsTabBG` | 深海军蓝、柔雾粉略变；其余三套 RGB 相同，仅 alpha 由 f4 变 ff |

编辑器关系表同步标记为可跟随/可解除。测试基线随之更新（金标指纹重设），并新增断言：四组合并轴在五套内置主题里取值一致。

### 其余候选的实测结论（未合并）

| 候选 | 变化量 | 结论 |
|---|---|---|
| `metadataPanelLabel` 并入 `resultsSecondaryText` | 编辑器 12~15 万像素、Info 面板 2 万像素 | 不合并：语义不同（信息区标题 vs 列表次要信息），且牵动整个编辑器外壳 |
| `sidebarStatusBarBG` 并入 `sidebarRailBG` | 约 3 千像素（侧栏底部） | 语义最接近，变化最小，可后续单独确认 |
| `filterBrowserButtonBG` 并入 `filterBrowserSectionBG` | 数千像素 | 不合并：按钮需要浮在分组底之上，同色失去层次 |

对照报告：`D:/Harken-QA/THEME-COLOR-RELATION-20261121/round2-report.html`。

## 2026-11-21 第六批：删除三个从未绘制的侧栏色字段

用户确认「未使用的就拿掉」。实机验色（临时染成红/绿/蓝后截图）与代码核对都证明以下三个字段不参与绘制：

| 字段 | 原用途 | 证据 |
|---|---|---|
| `sidebarRailBG` | 图标栏底色 | 图标栏已改为无背景填充，图标直接浮在外层渐变上；字段只在模块映射里赋值，无读取方 |
| `sidebarHeaderBG` | 侧栏标题栏底色 | 无任何绘制代码读取；标题栏实际使用其他颜色 |
| `sidebarStatusBarBG` | 底部 22px 状态条 | `DrawSidebarStatusBar()` 长期未被调用，文件数已由顶部搜索栏展示 |

处理：

- 从 `ThemeColorScheme`、模块颜色投影、`MakeBuiltinScheme`、JSON 导出/导入、配色编解码、编辑器关系表中删除。
- 删除未使用的 `DrawSidebarStatusBar()`。
- 关系条目由 115 降至 112；金标指纹随删除重新设基线。
- 保留并继续使用：`sidebarRailDivider`、`sidebarIconInactive/Hover/Active`、`sidebarActiveStrip`、`sidebarSubpaneBG/Divider`、`sidebarSubHeaderLine`。

验证：

- 删除前后 11 张实机截图（五套主题主界面 + 五套编辑器 + 结束帧）逐像素比对：**全部 0 差异**，确认这三个字段确实不影响画面。
- `TestThemeColors` 487 项通过；`TestMistBlueTheme`、`TestThemeSchedulePaletteCodec` 均通过；唯一失败仍是既有 `src/main.cpp` 旧字面量。

## 2026-11-21：会影响成品色彩的算法清单

导入 JSON 后画面与键值不一致，来源分三层。

### 一、绘制期算法（每次绘制都生效，与 JSON 是否写全无关）

| 算法 | 输入字段 | 影响 | JSON 能否压住 |
|---|---|---|---|
| 结果列表横向渐变 | `resultsGradientStart` → `resultsGradientEnd`（`main.cpp` 41719 附近） | 结果区从左到右整片变浅；石板预设因此偏棕 | 能：两个键都设成 `resultsBG` |
| 浅色主题列表底二次混合 | 上述两键各 50% | 浅色主题列表底 | 能（同上） |
| 播放列表渐变 | `playlistGradientStart/End/Highlight/Shadow` | 播放列表卡片 | 能 |
| 弹层半透明 | `popupBG` α0.98、`dialogPanelBG`、`diagnosticOverlayBG` α0.92 | 菜单、Tooltip、对话框底色比键值浅 | 能（写入带 alpha 的值） |
| 结构线半透明 | `sidebarSubpaneDivider` α0.22/0.30、`resultsActiveBorder` α0.85 | 边框偏淡 | 能 |
| 悬停/激活插值 | `sidebarIconInactive/Hover/Active`、按钮混 `controlAccent` 14%/22% | 悬停与按下时变色 | 部分（三色可改，动画不可关） |
| 菜单文字对比兜底 | 菜单底色亮度 → 纯白或纯黑 | 菜单文字可能不是 `interfaceText` | 不能 |
| 选区遮罩 | `waveformSelectionDim` 的 alpha | 波形未选区域压暗 | 能 |

实测：把 `resultsGradientStart/End` 都设成 `resultsBG`，结果区由 #11151a→#25221e 的棕色渐变变成整片 #11151a。

### 二、构建主题时的派生（写入 JSON 字段）

`ApplySemanticThemeDefaults` + `ApplyMeterThemeDefaults` 共 20 余条公式，典型：

- `resultsGradientEnd ← resultsBG` 与 `resultsSelection` 按 9:1 混合
- `resultsActiveBorder ← controlAccent` 与 `resultsSelection` 各半，α0.85
- `waveformPanelBorder / waveformSplitterLine ← waveformBG + chromeBorder + controlAccent`
- `historyCapsuleActiveBG / modeCapsuleActiveBG ← frameBgBase + themeHighlight ×0.72`
- `wwise*` 八色 ← `volumeColor` / `volumeClipColor` / 固定绿青锚点
- `meter*` 六色 ← 波形底 / 音量色 / 削波色 / 中性灰锚点
- `popupBG ← toolbarBG`（α0.98），`dialogBG/dialogPanelBG ← popupBG` 混画布
- `miniWaveColor ← waveformColor`、`coverWallBG ← metadataPanelBG`、`filterBrowserLocateTint ← transferButtonBG`

关键：**只在 JSON 缺该键时生效**。键写全则不被覆盖（13 套导出→导入→再导出逐字节一致已证）。

### 三、派生锚点

`themeHighlight`、`meterWarnAnchor`、`meterClipAnchor`、`meterNeutralAnchor`、`wwiseOkAnchor`、`wwiseNewAnchor` 参与上述派生，可在 JSON 内覆盖，改一处即带动对应派生色。

## 2026-11-21 第七批：颜色混合效果开关

用户要求“取消非必要的混合效果，混合应可开关，只保留渐变”。

实现：

- `ThemeColorScheme::colorMixing`（默认 `true`，旧主题与内置主题外观不变）。
- JSON 键 `"colorMixing": "1"/"0"`；缺省视为开启。
- 高级主题编辑器「颜色混合效果」勾选框（`ThemeEditor_ColorMixing`），切换即重算派生色并重置关联状态。
- `MixOrA(scheme, a, b, t)`：关闭混合时返回主色 `a`，`ApplySemanticThemeDefaults`、`DeriveMeter*`、Wwise 与拨盘派生全部走它。
- `ThemeColors::resolveDerivedColors()`：关闭混合时重算派生色，但保留 `resultsGradientStart/End`，保证渐变仍然按两端插值。
- `loadAndApplyThemeFile()` 在 `colorMixing == false` 时自动调用它。

覆盖范围：全部构建期派生（20 余条公式）。渐变两端始终保留。

未覆盖（需后续把表面提升为字段，直接取消会破坏结构）：面包屑条、结果页签台、播放栏底条、按钮悬停混色、菜单文字对比兜底。

验证：`TestThemeColors` 523 项通过（新增 9 项锁定关闭混合的语义）；实机同一石板主题只改开关，差异 43,210 像素，结果区渐变保持不变。
对照报告：`D:/Harken-QA/THEME-COLOR-RELATION-20261121/mixing-switch.html`。

## 2026-11-22 修复：极光画布色（三条面板缝隙）

用户实机反馈「三处缝隙颜色不统一」。量过之后确认：三处缝（侧栏与结果表之间 12px、结果表与右侧 Info 之间 16px、结果表与播放条之间上下两条）画的都是 `mainWindowBG`；极光其他表面全是蓝调（`#0b131f` / `#0e1725` / `#0c1622`），只有这个画布色是默认深色主题遗留的中性灰 `#141516`，所以三条缝发灰。

改动：`src/BuiltinThemeScheme.cpp` 极光 `mainWindowBG` `0xff141516` → `0xff0c1522`（取工具栏与结果区之间的蓝调）。只动极光，其他四套内置主题与自定义主题不动。

影响面（同一构建同库，改前/改后逐像素）：

- 极光差异 192,019 像素，全部落在画布条带：侧栏缝 12px、右侧缝 16px、底部两条、工具栏下沿一条。
- 玄夜 / 琉璃蓝 / 霁翠 / 玫瑰雪松 / 任一自定义 JSON 主题：0 差异。

因此这条改动有意破坏了「五主题 0 像素差异」的基线，需要同步的只有：

- `tests/test_theme_colors.cpp` 极光指纹 `17086517842336242072` → `17444617343897474163`。
- 任何以五主题 0 差异为准的对照报告，极光一项要按「有意变更」标注，不能当成回归。

未处理：三条缝的**宽度**仍是 12 / 16 / 上下两条，本次只统一颜色；要统一宽度得动停靠布局绘制，属于另一件事。

证据：`D:/Harken-QA/SEAM-NAV-20261122/`（`evidence/` 原始 PNG + `report.json`，`Harken SEAM-NAV QA/截图/` 前后对照）。采集脚本：`tmp/theme-editor-v2-validation/capture_seam_nav.py`、`seed_seam_nav_library.py`。
