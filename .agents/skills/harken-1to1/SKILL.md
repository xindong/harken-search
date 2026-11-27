---
name: harken-1to1
description: Harken Search 一消一流程的人读版：一轮怎么选卡、在哪建目录、用什么基线、怎么构建自验、证据放哪、什么情况才停。给人和新会话对齐用；实际执行以对应电脑上的一消一调度提示词为准。
---

# Harken Search 一消一（人读版）

**这页是给人看的对齐材料，不是执行入口。** 真正跑的是两台电脑上的「一消一」自动化提示词（A 机 30 分钟、P18 60 分钟）。提示词与该页冲突时以提示词为准；要改流程，改提示词，顺便把这里同步。

规则层面（选卡原则、占卡语义、报告格式）按 Cuevi 的 `cuevi-one-in-one-out` 与当轮 Service 规则；本页只写本项目的落地参数。

## 一轮做什么

1. **核对环境**：工作目录、`git remote` 指向 `xindong/harken-search`、`.cuevi/project.json` 的 project_id 与本机服务地址一致，不一致就停。
2. **读规则**：从 Service 取当轮一消一正文与 revision（HTTP 直读优先，插件 `INTERNAL` 时可重试一次），不用旧正文。
3. **选一张卡**：未归档、无活跃会话、无阻塞，优先 `in_progress`，其次 `backlog`；`currentStep/nextAction` 写着等验收或等选择的让位。一轮只锁一张。
4. **占卡**：写 UTF-8 无 BOM 的 session JSON，拿回真实 session id 才算占上；拿不到就不改代码。
5. **开发**：只做本卡范围。
6. **自验**：真跑、真测、真截图，按下面的命令。
7. **写回**：进度、`currentStep`、`nextAction`、证据位置写回卡并回读。
8. **释放**：`session end` 并回读 `activeSessions` 为空；下一轮才能重新占。

## 本项目的固定参数

- 卡目录：A 机 `E:/tmp/harken-<卡号>`，P18 `D:/tmp/harken-<卡号>`；基线一律 `origin/qa/current`
- 不在主目录改产品文件；合并和清理不在卡目录里做
- 两台机器并行：开 session 前 fresh 回读 `activeSessions`，撞上 `409` 就换下一张，不抢不接管

## 构建与测试

```powershell
cmake -S . -B build_debug -G "Visual Studio 17 2022" -A x64
cmake --build build_debug --config Debug --target HarkenSearch -- /m
ctest --test-dir build_debug -C Debug --output-on-failure
```

- 行为测试目标多为 `EXCLUDE_FROM_ALL`，整套构建不会带上：先 `--target Test<名字>` 再 `-R <名字>`
- 改界面或布局时这一步不能省
- **省 token**：构建/测试输出先重定向到 `D:/Harken-QA/<卡号>/<用途>-<时间>.log`，再用 `Select-String` 只取失败行和末尾汇总；不要整段贴回上下文，同一基线不重复全量构建

## 占卡命令的写法（踩过的坑）

必须单行、无反引号包裹、无 `&` 调用运算符、无嵌套引号，可执行文件绝对路径写在句首：

```text
C:/Users/<本机用户>/AppData/Local/<node路径>/node.exe <cuevi 仓库>/apps/cli/dist/main.js --server http://<本机服务地址>:4319 session start --file <卡目录>/_cuevi-session.json --json
```

把整条命令再包一层 `powershell -Command`、或用 `& '…'` 调用 node、或在命令行里加重定向，都会被策略层判成无法解析而拒掉。

## 什么时候停

只有四种：

1. 卡达到可交付并已送验（`awaiting_qa`）
2. 必须用户看界面/听感才能继续
3. 真实阻塞（缺环境、权限、数据、决策）且已写进卡
4. 同一问题连续两次失败

## 交付形态

- 整卡做完、实测齐了：出正式 HTML 验收报告（真实截图 + 试用入口），推 `awaiting_qa`，再请用户确认
- 没做完但已有可运行包、改动看得见：可给「预览版 HTML」，首行必须写「未完成，仅预览，不算验收」，不推 `awaiting_qa`、不要求确认通过
- 收尾与归档交给 `harken-card-closeout`

## 证据

`D:/Harken-QA/<卡号>/`：日志、截图、报告、session 回执。临时材料 7 天。
