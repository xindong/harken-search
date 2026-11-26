---
name: harken-card-closeout
description: Harken Search 仓库专用的卡片收尾细则：合入 qa/current、CI 与审查线程、清理卡目录与分支、证据保留期、结卡归档顺序。配合 Cuevi 的验收收尾技能使用，只在本仓库任务里生效。
---

# Harken Search 卡片收尾

本页属于 Harken Search 仓库，只在本仓库的任务里生效。它补充 Cuevi 收尾技能里的「项目专属」部分——分支、路径、命令、保留期只对 Harken Search 有效，不得套用到其他项目。收尾门槛、校验与归档入口一律以 Cuevi 当前规则和收尾校验为准，本页不替代。

用户本人主动提交 `/cuevi-acceptance-closeout` 时，按 Cuevi 收尾技能核对当轮候选并登记人工验收，不要求再确认；仅说「走收尾流程」授权当前已验收卡的合入、审查、最终复测、清理、可信完成与归档，不单独证明验收事实。两种方式都不授权正式发布。顺序沿用 2026-11-24 22:01 确认的清理先于 `done`，并以当前 Cuevi 校验为准。

## 1. 定位

卡号取自会话标题「工程名｜卡号｜描述」，再读 Cuevi 当前卡的 revision、时间线、活跃会话和 PR。确认卡目录、分支、候选 SHA 和用户最后确认的是不是同一个候选；修复后行为变了而用户没看过新包，只问一句「请看新包」，先不收尾。

## 2. 记下人工确认

只有已验收补丁还未提交时，按 Cuevi 收尾技能先核对送验包和补丁身份，在本卡分支原样提交并回读；已有对应提交就复用。收尾授权包含本卡必要的原样提交、推任务分支和向 `qa/current` 建并合入 PR，不另问是否继续，也不直接推 `qa/current`。

只有确认能唯一对应当前卡、当前 QA 轮次和已交给用户试用的当前候选，才记录人工验收。用户主动提交统一入口的命令就是确认原话；无通过记录时先登记，不再要求口头确认。单说「收尾」「done」不证明已验收；「通过了，合并」也须能唯一定位本轮候选。把真实原话、Cindy 会话和设备记入当前部署提供的正式人工验收入口（`method=human`，必须带 `confirmationQuote`/`confirmationRef`；`packageSha256` 用小写十六进制；候选、会话、`confirmedAt` 与证据附件逐字段一致）。用户是委托 AI 代验，则由 Cuevi 验收技能里的 AI 验评分册先登记，不替用户通过，也不提前上报 `passed:true`。每次写卡前重读 revision。

## 3. 合入 qa/current

1. 把最新 `origin/qa/current` 合进本卡分支，保留验收候选为祖先。`git merge-tree --write-tree origin/qa/current <候选>` 可不切目录预判冲突。
2. 界面或布局改动：开 PR 前对照 CI 精确执行额外步骤（工作区样式边界、筛选器证据）。逐个 `cmake --build --target <受影响测试目标>`，因为 CI 不构建 `EXCLUDE_FROM_ALL` 的行为测试目标。
3. 向 `qa/current` 开 PR，不直推、不 force。两项必检（Harken Search / checks、Harken Search / build and test）通过；审查线程（含 Greptile）逐条回复并 resolve，未 resolve 时 PR 一直是 BLOCKED。用保留祖先的普通 merge；仅在已完成最终集成复测且清理条件满足时删除任务分支，不用 `--delete-branch` 抢在复测前删分支。
4. 合入后 `git fetch`，回读远端与本机 `qa/current` 都包含候选和合并提交；再把本机主仓的 `qa/current` 快进到远端（`--ff-only`）。相邻卡可能在这期间再次快进 QA：记录本卡的 PR 合并 SHA 与验收源码 SHA，以两者都是祖先为准，不回退别人的卡。

## 4. 清理（先于 done）

按 Cuevi 收尾顺序先复测最终集成版；修复改变已验收行为时重送 QA。再确认工作树归属、未提交文件、未推送提交、占用与清理授权。先保存证据：验收材料、截图、报告、日志放到 `D:/Harken-QA/<卡号>-<用途>/`（保留 7 天），确认不在卡目录里；把属于本卡的忽略生成目录（`build_debug/`、`build_qa/`、`tools/__pycache__/` 等）移出，不手写递归删除。

- **卡目录在 `E:/tmp/harken-<卡号>/`（P18 为 `D:/tmp/...`）：** 在卡目录外运行固定工具，先 `--dry-run` 看 HEAD，再 `--execute --expected-head <完整 HEAD>`：
  `py tools/remove_card_directory.py --workspace-root <卡目录的直属父目录> --card-name harken-<卡号> --card-prefix harken- --main-repository <本机主仓> --repository xindong/harken-search --feature-branch <分支> --dry-run`
  `--card-prefix` 只去掉 `harken-`，剩下的必须与当前 Cuevi 卡号和功能分支身份逐字对应（包括现有 `BUG-`、`REQ-`、`TASK-` 等卡号）；`--repository` 用 owner/repo，集成分支不带 `origin/`。工具会移除 worktree、删本地和远端功能分支并 prune。不用 `rm -rf`，不用 `git branch -D`；`-d` 删不掉说明没合干净，停下查。
- **会话自带的 `.cindy-worktrees/<名>/`：** 不在会话内删自己所在的目录。在安全退出该目录后，从主仓或有权限的独立会话处理；通过 Cindy 的正式会话回收入口处理其管理的目录（会话归档与 Cuevi 卡片归档是两件事），本地分支在主仓侧用 `git branch -d`。回收未授权、不可用或仍被占用时，保留路径并停在 `done` 前，不强删、不手改 `.git/worktrees`。
- 回读：目录、worktree 登记、本地与远端分支确实不存在。已被合法清理过的，用实查「不存在」证明，不重复删。

## 5. 补一张 Mac 卡

结卡前先看本次改动要不要 Mac 跟着做。动了用户看得见的功能或行为（界面、交互、搜索、播放、导入导出、设置项）就要开一张 Mac 对应卡。

**Mac 卡要落在 Cuevi 的 mac 版本工作区，也就是独立的 `Harken Search macOS` 项目（`project-5f21fc53-1ef4-48f5-9a09-ed34f72d9fbf`，看板 `board-702be56d-e9fb-4628-a755-27c582134568`），不是 Harken Search 项目。** Mac 是独立开发线，那边没有绑本仓库。

建卡落点很容易搞错：在本仓库会话里直接建卡，项目会默认成 Harken Search。要放到 mac 工作区，就得按那个 projectId 建，或在 mac 工作区里建；建完回读卡片的归属，发现落到 Harken Search 就停下，别顺手当完成。

- 标题写清来源：`[Mac] <本卡功能>`；正文写来源卡号、这次改了哪些行为、验收要看什么、参考的本卡候选 SHA。
- 纯 Windows 侧的（安装器、文件关联、Windows 凭据、系统集成、只在本机复现的性能问题）不开卡，在本卡备注里写一句「Mac 无需跟进」及原因。
- 只开卡：不替它开工、不建目录、不往本卡分支里塞东西；Mac 卡走它自己的流程。
- 卡号回写本卡的备注和汇报，否则下一轮没人知道 Mac 侧还欠什么。

## 6. 结卡与归档

按 Cuevi 收尾技能及其收尾校验执行；入口名以当前部署为准。人工验收卡在 `awaiting_qa` 时：附件 → 正式人工验收记录 → 合入、审查与最终复测、清理并回读 → 收尾校验 → 仅在校验返回该动作时提交完成（带 `acceptance` 与 `qaMerge`，两处 `acceptedCandidateSha` 一致，`verifiedAt` 晚于 `confirmedAt`）→ 回读可信完成事件及 `done` → 重新采集证据、校验归档 → 用新 revision 加幂等键提交归档 → 回读 `archived=true`。

- 不要先试 `awaiting_merge`（`awaiting_qa` 到它会 422）；不提前调用会直接写 `done` 的 AI 验收报告入口；不能把委托 AI 实测当作人工确认。
- Cuevi 自动刷新 PR 状态会推进 revision：先看时间线，同一候选且无新限制时，按当前 revision 刷新回执，不重做 Git 合并。归档前确认 PR 状态已回填 `merged`。
- 历史手改 `done` 或最新完成事件不可信时，不能补附件直接归档，也不能借旧入口、拖卡、取消或隐藏绕开；仅可使用已部署的正式恢复入口，缺失则保留真实状态并报告。

## 汇报（固定）

```text
完成卡片：卡号＋标题；已完成／未完成
完成功能：实际实现并验证的功能
Cuevi 阶段：done / archived（revision）
清理：本地目录（已删／保留及原因）；本地分支；远端分支；QA 材料位置与到期日（7 天）
Mac 跟进：新开的 Mac 卡号，或「Mac 无需跟进」＋原因
```

「Git 状态干净」不等于「已清理」，清理状态必须单独写。

## 停止条件（保留现场，写清卡点，不绕过）

必检未过、审查线程未解决、有冲突；用户尚未确认当前候选；工作树与已验收内容不符、有他人或不明改动；他人或另一台电脑占用；清理前仍有未提交、未推送的内容，或清理预检拒绝；Cuevi 返回非预期回执。不 force、不改保护规则、不替用户通过。

## 不做

不合 `main`，不发布、补发或撤回；不清理别的卡或他人目录；不为「收尾」改验收结论。
