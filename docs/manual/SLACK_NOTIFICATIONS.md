# Slack 合并通知

本仓库的 Slack Notify 工作流沿用 PR **合并后**和新 issue 创建时的通知。
普通 push、打开 PR、关闭但未合并的 PR 不发送通知。
机器人凭据和频道未配置时，工作流明确提示未配置并跳过发送，不影响产品检查。

## 需要填写的配置

打开本仓库 **Settings → Secrets and variables → Actions**。

| 位置 | 名称 | 内容 |
| --- | --- | --- |
| Secrets → New repository secret | SLACK_BOT_TOKEN | 原机器人以 xoxb- 开头的 Bot User OAuth Token |
| Variables → New repository variable | SLACK_CHANNEL | 原频道 ID，例如 C0123456789，不是频道名称 |
| Variables → New repository variable | GH_SLACK_MAP | 可选，GitHub 用户名到 Slack 成员 ID 的 JSON 映射 |

人员映射格式示例（替换成自己的用户名和成员 ID）：

~~~json
{"github-user": "U0123456789", "another-user": "U0987654321"}
~~~

不填人员映射也能发送，作者和合入人员显示 GitHub 用户名。已有旧映射时复制其值即可。
频道 ID 可以从频道详情中复制；成员 ID 可以从成员个人资料的更多菜单中复制。
原机器人应留在原频道，并具有 chat:write 权限。沿用原机器人和频道时通常无需重新邀请。

令牌请在 Slack 应用的 **OAuth & Permissions → Bot User OAuth Token** 页面取得。
GitHub 已保存的 Secret 不能显示原值；请从原 Slack 应用或自己的凭据存放位置取值，直接填到新仓库的 Secret，不放进代码或聊天。

## 先预览，再开启自动通知

1. 通知工作流合入默认分支 main 后，打开 **Actions → Slack Notify → Run workflow**。
2. 手动运行只做测试和示例预览，不接收 Secret，也不向频道发送消息；预览中的 PR #0 是示例。
3. 填好 SLACK_BOT_TOKEN 和 SLACK_CHANNEL 后，后续 PR 合并或新 issue 才会实际发送。此前跳过的通知不会自动补发。
4. 到 Actions 查看相应运行：缺配置会在 Summary 提示；Slack 拒绝发送会使发送步骤失败并显示错误类型。

## 可选的中文摘要

原来的模型摘要功能也已保留，可另外配置两个仓库 Secret：
ANTHROPIC_API_KEY 和 ANTHROPIC_BASE_URL。基础地址不包含 /v1/messages。
未配置时保留原始标题，并提取正文的第一条有效摘要，不影响基础通知。
配置后，标题和最多 4000 字符的摘要素材会发送到所配置的模型服务。

## 本地验证

~~~text
python -m pytest scripts/tests/test_slack_notify.py tests/test_slack_notification_workflow.py -q
~~~

发送流程只运行默认分支代码，不检出或执行外部 PR 的代码。手动预览可以选分支，但不会取得通知凭据。
