from __future__ import annotations

import io
import json
import tempfile
import unittest
import unittest.mock as mock
import urllib.error
from pathlib import Path



import importlib.util

SCRIPT = Path(__file__).resolve().parents[1] / "slack_notify.py"
SPEC = importlib.util.spec_from_file_location("slack_notify", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
SPEC.loader.exec_module(MODULE)


PR_BODY = """# Pull request

## Scope

- Problem and user impact: 搜索结果里点封面，预览不再卡住。
- Included changes: 预览线程在封面替换后立即取消旧任务。
- Primary module domain: Search / UI

## Risk

- Affected behavior and data: 仅封面预览。
"""


class SlackNotifyTest(unittest.TestCase):
    def test_digest_prompt_asks_for_reader_facing_change(self) -> None:
        prompt = MODULE.digest_system("已合并的 PR")
        self.assertIn("读者会碰到什么变化", prompt)
        self.assertIn("Harken Search 频道", prompt)
        self.assertIn("第一句直接写这条改动的场景", prompt)
        self.assertIn("禁止使用系统提示里的任何示例事实", prompt)
        self.assertIn("不要写成搜索或播放修复", prompt)
        self.assertNotIn("Venus", prompt)
        self.assertNotIn("2 到 3 句", prompt)

    def test_parse_checked_product_scopes_only(self) -> None:
        labels = MODULE.parse_scope_labels(PR_BODY)
        self.assertEqual(labels, ["搜索", "界面"])
        self.assertEqual(MODULE.project_prefix(labels), "【搜索 / 界面】")
        self.assertEqual(MODULE.parse_scope_labels(""), [])
        self.assertEqual(MODULE.parse_scope_labels("Primary module domain: unknown"), [])

    def test_issue_body_does_not_invent_scope(self) -> None:
        body = "## 预期行为\n搜索页封面还在转圈。\nSearch 结果里能听到预览。"
        self.assertEqual(MODULE.parse_scope_labels(body), [])

    def test_fallback_digest_keeps_task_list_text(self) -> None:
        body = "- [ ] 搜索结果里点封面，预览不再卡住。\n- [x] 旧任务"
        self.assertEqual(
            MODULE.fallback_digest(body),
            "搜索结果里点封面，预览不再卡住。",
        )

    def test_extract_summary_drops_comments_and_later_sections(self) -> None:
        summary = MODULE.extract_summary_section(PR_BODY)
        self.assertIn("搜索结果里点封面", summary)
        self.assertNotIn("Affected behavior", summary)

    def test_sanitize_strips_mentions_and_secrets(self) -> None:
        # Split the sample so the compliance scanner does not treat this
        # fixture as a live token.
        sample = "".join(("sk", "-", "abcdefghijklmnopqrstuvwxyz"))  # REPLACE_WITH
        dirty = (
            "请 @channel 看一下 <@U000000004> 和 ＠example-author\n"
            f"key={sample}\n"
            "<!here>"
        )
        cleaned = MODULE.sanitize_plain(dirty, 400)
        self.assertNotIn("@channel", cleaned)
        self.assertNotIn("<@U000000004>", cleaned)
        self.assertNotIn("example-author", cleaned)
        self.assertNotIn(sample, cleaned)
        self.assertIn("[redacted-secret]", cleaned)
        self.assertNotIn("<!here>", cleaned)

    def test_mention_uses_map_and_falls_back_to_login(self) -> None:
        mapping = {"example-author": "U000000001"}
        self.assertEqual(MODULE.mention_for("example-author", mapping), "<@U000000001>")
        self.assertEqual(MODULE.mention_for("unknown-dev", mapping), "unknown-dev")
        self.assertEqual(MODULE.mention_for("bad<script>", mapping), "bad&lt;script&gt;")
        self.assertEqual(MODULE.mention_for("x", {"x": "not-an-id"}), "x")

    def test_format_title_keeps_chinese_and_adds_translation(self) -> None:
        self.assertEqual(
            MODULE.format_title("支持 Hub 记忆新建工程路径", "ignored"),
            "支持 Hub 记忆新建工程路径",
        )
        self.assertEqual(
            MODULE.format_title(
                "fix(studio): preserve rapid tree chevron toggles",
                "修复 Studio：保留快速树形展开箭头切换",
            ),
            "修复 Studio：保留快速树形展开箭头切换（fix(studio): preserve rapid tree chevron toggles）",
        )

    def test_build_pr_and_issue_cards(self) -> None:
        pr = MODULE.build_card(
            {
                "kind": "pr",
                "number": "77",
                "url": "https://github.com/xindong/harken-search/pull/77",
                "title": "更新中心对齐已通过的主题（feat(update): align update center with approved theme）",
                "author": "<@U000000001>",
                "merged_by": "<@U000000001>",
                "digest": "更新中心的按钮和颜色跟现在的主题一致。",
                "prefix": "【界面】",
            }
        )
        self.assertIn("✅ *【界面】PR #77 已合并：", pr)
        self.assertIn("作者 <@U000000001>", pr)
        self.assertIn("合入管理员 <@U000000001>", pr)
        self.assertIn("https://github.com/xindong/harken-search/pull/77", pr)
        self.assertNotIn("内容：", pr)

        issue = MODULE.build_card(
            {
                "kind": "issue",
                "number": "40",
                "url": "https://github.com/xindong/harken-search/issues/40",
                "title": "搜索结果点封面后预览一直转圈",
                "author": "<@U000000002>",
                "digest": "点封面后预览转圈停不下来。",
                "prefix": "",
            }
        )
        self.assertTrue(issue.startswith("📌 *新 issue #40："))
        self.assertIn("<@U000000002> 提出", issue)
        self.assertNotIn("合入管理员", issue)

    def test_empty_digest_still_renders_skeleton(self) -> None:
        card = MODULE.build_card(
            {
                "kind": "pr",
                "number": "78",
                "url": "https://github.com/xindong/harken-search/pull/78",
                "title": "明文 WAAPI 只允许回环地址",
                "author": "<@U000000003>",
                "merged_by": "<@U000000003>",
                "digest": "",
                "prefix": "【音频】",
            }
        )
        self.assertIn("作者 <@U000000003>", card)
        self.assertNotIn("\n\n\n", card)
        self.assertTrue(card.endswith("https://github.com/xindong/harken-search/pull/78"))

    def test_collect_event_does_not_post_when_model_fails(self) -> None:
        env = {
            "EVENT_NAME": "pull_request_target",
            "PR_NUMBER": "77",
            "PR_TITLE": "feat(update): align update center with approved theme",
            "PR_URL": "https://github.com/xindong/harken-search/pull/77",
            "PR_AUTHOR": "example-maintainer",
            "PR_MERGED_BY": "example-maintainer",
            "PR_BODY": PR_BODY,
            "GH_SLACK_MAP": json.dumps({"example-maintainer": "U000000003"}),
        }
        with mock.patch.object(MODULE, "call_model", return_value=""):
            event = MODULE.collect_event(env)
        self.assertEqual(event["prefix"], "【搜索 / 界面】")
        self.assertEqual(event["digest"], "搜索结果里点封面，预览不再卡住。")
        self.assertEqual(event["author"], "<@U000000003>")
        card = MODULE.build_card(event)
        self.assertIn("已合并", card)
        self.assertIn("搜索结果里点封面，预览不再卡住。", card)
        self.assertIn("https://github.com/xindong/harken-search/pull/77", card)

    def test_post_slack_is_not_retried_on_http_error(self) -> None:
        calls = {"n": 0}

        def boom(*_args, **_kwargs):
            calls["n"] += 1
            raise urllib.error.URLError("down")

        with mock.patch.object(MODULE.urllib.request, "urlopen", side_effect=boom):
            with self.assertRaises(SystemExit):
                MODULE.post_slack("token", "C000000001", "hello")
        self.assertEqual(calls["n"], 1)

    def test_post_slack_rejects_non_object_json(self) -> None:
        class FakeResponse:
            def read(self) -> bytes:
                return b'["not", "ok"]'

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

        with mock.patch.object(MODULE.urllib.request, "urlopen", return_value=FakeResponse()):
            with self.assertRaises(SystemExit) as raised:
                MODULE.post_slack("token", "C000000001", "hello")
        self.assertIn("invalid-response", str(raised.exception))

    def test_call_model_treats_unexpected_json_as_empty(self) -> None:
        class FakeResponse:
            def read(self) -> bytes:
                return b'["not", "an", "object"]'

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

        with mock.patch.object(MODULE.urllib.request, "urlopen", return_value=FakeResponse()):
            text = MODULE.call_model(
                base_url="https://example.invalid",
                api_key="test-key",
                system="sys",
                user="hi",
                max_tokens=20,
                attempts=1,
            )
        self.assertEqual(text, "")

    def test_collect_event_reads_github_event_path(self) -> None:
        payload = {
            "pull_request": {
                "number": 77,
                "title": "feat(update): align update center with approved theme",
                "html_url": "https://github.com/xindong/harken-search/pull/77",
                "user": {"login": "example-author"},
                "body": PR_BODY,
                "merged_by": {"login": "example-author"},
            }
        }
        handle = tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".json", delete=False)
        self.addCleanup(lambda: Path(handle.name).unlink(missing_ok=True))
        handle.write(json.dumps(payload))
        handle.close()
        event_path = Path(handle.name)
        env = {
            "EVENT_NAME": "pull_request_target",
            "GITHUB_EVENT_PATH": str(event_path),
            "GH_SLACK_MAP": json.dumps({"example-author": "U000000001"}),
        }
        with mock.patch.object(
            MODULE,
            "call_model",
            side_effect=["更新中心对齐已通过的主题", "更新中心的按钮和颜色跟现在的主题一致。"],
        ):
            event = MODULE.collect_event(env)
        self.assertEqual(event["number"], "77")
        self.assertEqual(event["prefix"], "【搜索 / 界面】")
        self.assertIn("主题一致", event["digest"])
        self.assertEqual(event["author"], "<@U000000001>")

    def test_main_dry_run_prints_card_without_slack(self) -> None:
        env = {
            "EVENT_NAME": "issues",
            "ISSUE_NUMBER": "40",
            "ISSUE_TITLE": "搜索结果点封面后预览一直转圈",
            "ISSUE_URL": "https://github.com/xindong/harken-search/issues/40",
            "ISSUE_AUTHOR": "example-reporter",
            "ISSUE_BODY": "## 预期行为\n点封面后应马上出预览。",
            "GH_SLACK_MAP": json.dumps({"example-reporter": "U000000002"}),
        }
        buf = io.StringIO()
        with mock.patch.object(MODULE, "call_model", return_value="点封面后预览转圈停不下来。"):
            with mock.patch.object(MODULE, "post_slack") as posted:
                with mock.patch("sys.stdout", buf):
                    code = MODULE.main(["--dry-run"], env=env)
        self.assertEqual(code, 0)
        posted.assert_not_called()
        self.assertIn("📌 *新 issue #40：", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
