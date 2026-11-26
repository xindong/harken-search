---
name: harken-card-start
description: Harken Search 仓库专用的开工参数：卡目录位置、分支基线、开工授权的键格式、占用冲突的处理。配合 Cuevi 的「挑最久没动的卡」和 1消1 技能使用，只在本仓库任务里生效。
---

# Harken Search 开工参数

本页属于 Harken Search 仓库，只在本仓库的任务里生效。Cuevi 侧的「挑卡」「排除」「授权门槛」流程不在这里重写，以 Cuevi 当前规则为准；这里只补本仓库的落地参数。

## 卡目录与分支

- 同机默认目录：`E:/tmp/harken-<卡号>`；P18 机器用 `D:/tmp/harken-<卡号>`。已有目录先核归属并复用，不重复 `worktree add`。
- 分支基线：`origin/qa/current`。创建前先 `git fetch`，用最新远端基线：`git worktree add <目录> -b <分支> origin/qa/current`。
- 集成分支名为 `qa/current`，不带 `origin/`；其他仓库不适用这套命名。

## 授权键

- 开工授权按 Service 规则记录，本仓库惯用的键前缀是 `harken.dev-start.`，后接日期（例如 `harken.dev-start.20261126`）。
- 键前缀只是本仓库的命名习惯，格式与是否允许自建键以 Service 当前规则为准；规则与习惯冲突时以规则为准。
- `authorKind: ai`，summary 里保留用户原话与提问来源，不冒充人工点击。

## 占用与冲突

- 卡被别的会话占用时，Service 会返回冲突；按返回的码与消息处理，先查清占用者（Cuevi 活跃会话 + Cindy 会话标题/目录），再决定等还是问用户。
- 旧 Cuevi 会话心跳满 24 小时只是「待核查」，不代表原会话已停；具体停止状态、消息队列与目录锁以本轮从 Service 读取的规则为准。
- 不自己结束别人的会话，不硬占；拿不到 session id 就不动代码。

## 开工后

- `session start` 带绝对路径、分支、刚回读的 revision、真实 Cindy 会话 id。
- 只做卡面已确认范围；改完真跑、真截图、留证据，再推进到 `awaiting_qa`。
- 收尾走 `harken-card-closeout`，不在这里另写一套。
