---
name: harken-record
description: Harken Search 的 Bug / 需求记录入口：把用户口述整理成一张能直接开工的卡。追问、计划确认、写入与回读全部按 Cuevi 的 cuevi-recording-feedback 执行，本页只补本项目的固定目标与证据要求。
---

# Harken Search 记录 Bug / 需求

**规则不在这里。** 追问方式、计划确认、`submissionKey`、查重、回读一律按 Cuevi 的 `cuevi-recording-feedback`（`/cuevi-recording-feedback`）执行。本页只写它不知道的本项目参数。

## 固定目标

- 项目 `project-repository-6c277090039d015acb72df2ca87b3c0666f5be97254b3104cf26bebd206bc404`（Harken Search）
- 看板 `board-1ee8793a69ad-harken-search`，里程碑 `milestone-1ee8793a69ad-harken-search`
- 不写老项目 `project-airs` / `board-airs`，那是历史

## 本项目的记录要求

- **Bug 必带四样**：复现入口（点哪里、从哪进）、实际表现、预期表现、已有证据（截图、日志、库里那几条素材）。缺了就在卡上写「暂无」，不编。
- **前端/界面问题**：必须有真实截图或录屏，说明用的是哪份素材库（真实库还是副本）。听感问题写明用哪条素材试听。
- **分类**：既有行为不对 = Bug；希望新增或改进 = 需求。更新公告类按需求记，写清触发时机和受众。
- **标题**：中文、能脱离对话检索（写明界面位置和具体行为），不要写成「有个问题」。
- **编码**：正文先落成 UTF-8 无 BOM 的文件再提交，写完回读标题，出现问号就停。

## 证据放哪

`D:/Harken-QA/<卡号或日期>-<用途>/`，不进仓库、不放桌面和下载目录；临时材料按 7 天留存。

## 交付条件

只有写入返回 `recorded:true` 且回读正确（含难度与计划模型）才算记录完成。建卡不等于开工，也不套用执行模型。
