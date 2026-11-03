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
    def test_enterprise_slack_member_ids_are_preserved(self) -> None:
        mapping = MODULE.load_slack_map(json.dumps({"member": "W000000001", "bot": "B000000001"}))
        self.assertEqual(mapping, {"member": "W000000001"})
        self.assertEqual(MODULE.mention_for("member", mapping), "<@W000000001>")

    def test_digest_prompt_asks_for_reader_facing_change(self) -> None:
        prompt = MODULE.digest_system("已合并的 PR")
        self.assertIn("读者会碰到什么变化", prompt)
        self.assertIn("Harken Search 频道", prompt)
        self.assertIn("第一句直接写这条改动的场景", prompt)
        self.assertIn("禁止使用系统提示里的任何示例事实", prompt)
        self.assertIn("不要写成搜索或播放修复", prompt)
        self.assertNotIn("Venus", prompt)
        self.assertNotIn("2 到 3 句", prompt)

    def test_current_pr_template_module_field_is_supported(self) -> None:
        template = (Path(__file__).resolve().parents[2] / ".github/pull_request_template.md").read_text(encoding="utf-8")
        body = template.replace("Primary module domain and secondary reviewers:", "Primary module domain and secondary reviewers: Search / UI")
        self.assertEqual(MODULE.parse_scope_labels(body), ["搜索", "界面"])

    def test_complete_private_key_blocks_are_fully_redacted(self) -> None:
        for kind in ("PRIVATE KEY", "EC PRIVATE KEY", "RSA PRIVATE KEY", "ENCRYPTED PRIVATE KEY", "OPENSSH PRIVATE KEY"):
            with self.subTest(kind=kind):
                body = "TEST_PRIVATE_MATERIAL"
                block = f"prefix\n-----BEGIN {kind}-----\n{body}\n-----END {kind}-----\nsuffix"
                clean = MODULE.sanitize_plain(block, 400)
                self.assertNotIn(body, clean)
                self.assertNotIn("PRIVATE KEY", clean)
                self.assertIn("[redacted-secret]", clean)
                self.assertIn("prefix", clean)
                self.assertIn("suffix", clean)

    def test_unterminated_private_key_is_redacted_to_end(self) -> None:
        kind = "EC PRIVATE KEY"
        block = f"prefix\n-----BEGIN {kind}-----\nTEST_PRIVATE_MATERIAL"
        clean = MODULE.sanitize_plain(block, 400)
        self.assertNotIn("TEST_PRIVATE_MATERIAL", clean)
        self.assertIn("[redacted-secret]", clean)
        self.assertIn("prefix", clean)

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


class AutoMergeNoticeTest(unittest.TestCase):
    def payload(self):
        return {"repository": {"full_name": "xindong/harken-search"}, "workflow_run": {
            "name": "Enable PR Auto-merge", "event": "pull_request_target", "conclusion": "success",
            "path": ".github/workflows/auto-merge.yml", "run_attempt": 1, "id": 999,
            "run_started_at": "2026-11-03T02:00:00Z", "updated_at": "2026-11-03T02:01:00Z",
            "head_repository": {"full_name": "xindong/harken-search"}, "pull_requests": [{"number": 72}]}}

    def pull(self):
        return {"number": 72, "merged": True, "merged_at": "2026-11-03T02:00:30Z",
                "merged_by": {"login": "github-actions[bot]"}, "title": "迁入通知",
                "html_url": "https://github.com/xindong/harken-search/pull/72", "body": "合并后显示通知。",
                "base": {"repo": {"full_name": "xindong/harken-search"}}, "user": {"login": "example-author"}}

    def run_event(self, payload, pull, jobs=None):
        class Response:
            def __init__(self, body):
                self.body = body
            def read(self):
                return json.dumps(self.body).encode("utf-8")
            def __enter__(self):
                return self
            def __exit__(self, *_args):
                return False
        def respond(request, **_kwargs):
            if "/actions/runs/" in request.full_url:
                return Response(jobs or {"total_count": 0, "jobs": []})
            return Response(pull)
        env = {"EVENT_NAME": "workflow_run", "GITHUB_REPOSITORY": "xindong/harken-search",
               "GITHUB_TOKEN": "test-github-token", "SLACK_BOT_TOKEN": "test-slack-token", "SLACK_CHANNEL": "C000000001"}
        output = io.StringIO()
        with mock.patch.object(MODULE, "load_event_payload", return_value=payload), \
             mock.patch.object(MODULE.urllib.request, "urlopen", side_effect=respond) as fetch, \
             mock.patch.object(MODULE, "post_slack") as posted, mock.patch("sys.stdout", output):
            code = MODULE.main([], env=env)
        return code, posted, fetch, output.getvalue()

    def test_successful_instant_bot_merge_posts_the_real_pr_once(self):
        code, posted, fetch, _ = self.run_event(self.payload(), self.pull())
        self.assertEqual(code, 0)
        posted.assert_called_once()
        self.assertIn("PR #72 已合并", posted.call_args.args[2])
        self.assertIn("xindong/harken-search/pull/72", posted.call_args.args[2])
        self.assertEqual(fetch.call_args.args[0].full_url, "https://api.github.com/repos/xindong/harken-search/pulls/72")

    def test_invalid_failed_or_unrelated_workflows_never_fetch_or_send(self):
        for field, value in (("run_attempt", 0), ("conclusion", "failure"), ("name", "Unrelated"),
                             ("event", "pull_request"), ("head_repository", {"full_name": "outside/fork"})):
            with self.subTest(field=field):
                payload = self.payload()
                payload["workflow_run"][field] = value
                code, posted, fetch, _ = self.run_event(payload, self.pull())
                self.assertEqual(code, 0)
                posted.assert_not_called()
                fetch.assert_not_called()

    def test_upstream_rerun_announces_only_a_merge_inside_the_new_merge_step(self):
        jobs = {"total_count": 1, "jobs": [{"name": "enable-auto-merge", "steps": [{
            "name": "Queue merge after required checks", "conclusion": "success",
            "started_at": "2026-11-03T02:10:00Z", "completed_at": "2026-11-03T02:11:00Z"}]}]}
        for merged_at, expected in (("2026-11-03T02:10:30Z", 1), ("2026-11-03T02:00:30Z", 0)):
            with self.subTest(merged_at=merged_at):
                payload, pull = self.payload(), self.pull()
                payload["workflow_run"].update(run_attempt=2, updated_at="2026-11-03T02:11:00Z")
                pull["merged_at"] = merged_at
                code, posted, fetch, _ = self.run_event(payload, pull, jobs)
                self.assertEqual(code, 0)
                self.assertEqual(posted.call_count, expected)
                self.assertIn("/actions/runs/999/attempts/2/jobs", fetch.call_args_list[0].args[0].full_url)

    def test_manual_unmerged_or_old_merges_are_not_reannounced(self):
        for field, value in (("merged_by", {"login": "example-maintainer"}), ("merged", False),
                             ("merged_at", "2026-11-03T01:59:59Z"), ("merged_at", "2026-11-03T02:01:01Z")):
            with self.subTest(field=field):
                pull = self.pull()
                pull[field] = value
                code, posted, _fetch, _ = self.run_event(self.payload(), pull)
                self.assertEqual(code, 0)
                posted.assert_not_called()


class NotificationRetryTest(unittest.TestCase):
    def env(self, **values):
        return {
            "EVENT_NAME": "pull_request_target", "GITHUB_REPOSITORY": "xindong/harken-search",
            "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "2", "GITHUB_TOKEN": "test-github-token",
            "SLACK_BOT_TOKEN": "test-slack-token", "SLACK_CHANNEL": "C000000001",
            "PR_NUMBER": "77", "PR_TITLE": "通知测试", "PR_AUTHOR": "example-author",
            "PR_MERGED_BY": "example-maintainer", "PR_URL": "https://github.com/xindong/harken-search/pull/77",
            **values,
        }

    def response(self, conclusion, *, job_name="notify", total_count=1):
        payload = {"total_count": total_count, "jobs": [{"name": job_name, "steps": [
            {"name": "Post notice to Slack", "conclusion": conclusion}]}]}
        return self.json_response(payload)

    def json_response(self, payload):
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.read.return_value = json.dumps(payload).encode("utf-8")
        return response

    def test_successful_notification_rerun_does_not_post_again(self):
        for event in ("pull_request_target", "issues", "workflow_run"):
            with self.subTest(event=event), \
                 mock.patch.object(MODULE.urllib.request, "urlopen", return_value=self.response("success")) as fetch, \
                 mock.patch.object(MODULE, "post_slack") as post, mock.patch("sys.stdout", io.StringIO()):
                self.assertEqual(MODULE.main([], self.env(EVENT_NAME=event)), 0)
                post.assert_not_called()
                self.assertIn("/actions/runs/123/attempts/1/jobs", fetch.call_args.args[0].full_url)

    def test_failed_or_unreached_send_can_be_retried(self):
        for conclusion in ("failure", "cancelled", "skipped", None):
            with self.subTest(conclusion=conclusion), \
                 mock.patch.object(MODULE.urllib.request, "urlopen", return_value=self.response(conclusion)) as fetch, \
                 mock.patch.object(MODULE, "post_slack") as post:
                self.assertEqual(MODULE.main([], self.env()), 0)
                fetch.assert_called_once()
                post.assert_called_once()

    def test_prior_success_is_found_on_later_pages_and_attempts(self):
        empty = self.json_response({"total_count": 0, "jobs": []})
        other = self.response("success", job_name="preview", total_count=2)
        sent = self.response("success", total_count=2)
        with mock.patch.object(MODULE.urllib.request, "urlopen", side_effect=[empty, other, sent]) as fetch, \
             mock.patch.object(MODULE, "post_slack") as post, mock.patch("sys.stdout", io.StringIO()):
            self.assertEqual(MODULE.main([], self.env(GITHUB_RUN_ATTEMPT="3")), 0)
        post.assert_not_called()
        self.assertEqual(fetch.call_count, 3)
        self.assertIn("/attempts/2/jobs?per_page=100&page=2", fetch.call_args.args[0].full_url)

    def test_rerun_fails_without_sending_if_history_cannot_be_checked(self):
        with mock.patch.object(MODULE.urllib.request, "urlopen", side_effect=urllib.error.URLError("offline")), \
             mock.patch.object(MODULE, "post_slack") as post:
            with self.assertRaisesRegex(SystemExit, "notification history lookup failed"):
                MODULE.main([], self.env())
        post.assert_not_called()

    def test_first_attempt_for_qa_and_main_posts_without_history_lookup(self):
        for branch in ("qa/current", "main"):
            payload = {"action": "closed", "pull_request": {"merged": True, "base": {"ref": branch}}}
            with self.subTest(branch=branch), mock.patch.object(MODULE, "load_event_payload", return_value=payload), \
                 mock.patch.object(MODULE.urllib.request, "urlopen") as fetch, \
                 mock.patch.object(MODULE, "post_slack") as post:
                self.assertEqual(MODULE.main([], self.env(GITHUB_RUN_ATTEMPT="1")), 0)
                fetch.assert_not_called()
                post.assert_called_once()

    def test_dry_run_never_reads_history_or_posts(self):
        with mock.patch.object(MODULE.urllib.request, "urlopen") as fetch, \
             mock.patch.object(MODULE, "post_slack") as post, mock.patch("sys.stdout", io.StringIO()):
            self.assertEqual(MODULE.main(["--dry-run"], self.env()), 0)
        fetch.assert_not_called()
        post.assert_not_called()


if __name__ == "__main__":
    unittest.main()
