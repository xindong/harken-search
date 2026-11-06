"""Public notification delivery and offline preview contracts."""
from pathlib import Path
import os
import subprocess
import shutil
import tempfile
import sys
import unittest

import yaml
from tools.public_repo_manifest import is_public_path

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ".github/workflows/pr-merge-notify.yml"


class SlackNotificationWorkflowTests(unittest.TestCase):
    def workflow(self):
        path = ROOT / WORKFLOW
        self.assertTrue(path.is_file(), "The migrated notification workflow is missing")
        return yaml.load(path.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)

    def test_notifications_and_dependencies_survive_export(self):
        for path in (WORKFLOW, "scripts/slack_notify.py", "scripts/tests/test_slack_notify.py"):
            with self.subTest(path=path):
                self.assertTrue(is_public_path(path), path)
                self.assertTrue((ROOT / path).is_file(), path)

    def test_original_events_only_run_trusted_default_branch_code(self):
        workflow = self.workflow()
        self.assertEqual(workflow["on"]["pull_request_target"]["types"], ["closed"])
        self.assertNotIn("branches", workflow["on"]["pull_request_target"])
        self.assertNotIn("branches-ignore", workflow["on"]["pull_request_target"])
        self.assertEqual(workflow["on"]["issues"]["types"], ["opened"])
        self.assertNotIn("pull_request", workflow["on"])
        notify = workflow["jobs"]["notify"]
        self.assertIn("github.event.pull_request.merged == true", notify["if"])
        checkout = notify["steps"][0]
        self.assertEqual(checkout["with"]["ref"], "${{ github.event.repository.default_branch }}")
        self.assertEqual(checkout["with"]["persist-credentials"], "false")
        self.assertEqual(set(workflow["permissions"].values()), {"read"})
        self.assertEqual(workflow["permissions"].get("actions"), "read")

    def test_auto_merge_workflow_completion_has_a_notification_entry(self):
        workflow = self.workflow()
        self.assertEqual(workflow["on"]["workflow_run"], {"workflows": ["Enable PR Auto-merge"], "types": ["completed"]})
        self.assertIn("github.event_name == 'workflow_run'", workflow["jobs"]["notify"]["if"])
        self.assertIn("github.event.workflow_run.conclusion == 'success'", workflow["jobs"]["notify"]["if"])

    def test_missing_configuration_skips_post_with_visible_summary(self):
        notify = self.workflow()["jobs"]["notify"]
        config = next(step for step in notify["steps"] if step.get("id") == "configuration")
        self.assertEqual(config["env"]["SLACK_CHANNEL"], "${{ vars.SLACK_CHANNEL }}")
        self.assertEqual(config["env"]["SLACK_BOT_TOKEN"], "${{ secrets.SLACK_BOT_TOKEN }}")
        self.assertIn("configured=false", config["run"])
        self.assertIn("GITHUB_STEP_SUMMARY", config["run"])
        post = next(step for step in notify["steps"] if step.get("run") == "python scripts/slack_notify.py")
        self.assertEqual(post["if"], "steps.configuration.outputs.configured == 'true'")
        self.assertEqual(post["env"]["SLACK_CHANNEL"], "${{ vars.SLACK_CHANNEL }}")

    def test_manual_preview_does_not_receive_credentials_or_post(self):
        workflow = self.workflow()
        self.assertIn("workflow_dispatch", workflow["on"])
        preview = workflow["jobs"]["preview"]
        self.assertEqual(preview["if"], "github.event_name == 'workflow_dispatch'")
        text = str(preview)
        self.assertNotIn("secrets.", text)
        self.assertNotIn("GH_SLACK_MAP", text)
        run_steps = [step["run"] for step in preview["steps"] if "run" in step]
        self.assertIn("python scripts/slack_notify.py --dry-run", run_steps)
        self.assertNotIn("python scripts/slack_notify.py", run_steps)

    def test_configuration_shell_does_not_post_or_reveal_credentials(self):
        config = next(step for step in self.workflow()["jobs"]["notify"]["steps"] if step.get("id") == "configuration")
        bash = shutil.which("bash")
        if os.name == "nt":
            candidate = Path(os.environ.get("ProgramFiles", "")) / "Git/bin/bash.exe"
            bash = str(candidate) if candidate.is_file() else None
        if not bash:
            self.skipTest("Bash is required to execute the workflow configuration step")
        for token, channel, configured in (("", "", False), ("", "C000000001", False),
                                           ("dummy-test-token", "", False), ("dummy-test-token", "C000000001", True)):
            with self.subTest(token_present=bool(token), channel_present=bool(channel)):
                with tempfile.TemporaryDirectory() as directory:
                    output = Path(directory) / "output"
                    summary = Path(directory) / "summary"
                    env = dict(os.environ, SLACK_BOT_TOKEN=token, SLACK_CHANNEL=channel,
                               GITHUB_OUTPUT=output.as_posix(), GITHUB_STEP_SUMMARY=summary.as_posix())
                    result = subprocess.run([bash, "-e", "-c", config["run"]], env=env,
                                            capture_output=True, text=True, encoding="utf-8", timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(output.read_text(encoding="utf-8").strip(), f"configured={str(configured).lower()}")
                    summary_text = summary.read_text(encoding="utf-8") if summary.exists() else ""
                    if not configured:
                        self.assertIn("本次未发送通知", summary_text)
                    self.assertNotIn("dummy-test-token", result.stdout + result.stderr + summary_text)

    def test_real_cli_renders_without_any_service_credentials(self):
        env = {key: value for key, value in os.environ.items() if key in ("SYSTEMROOT", "PATH", "TEMP", "TMP")}
        env.update(PYTHONIOENCODING="utf-8", EVENT_NAME="pull_request_target", PR_NUMBER="42",
                   PR_TITLE="通知预览", PR_URL="https://github.com/xindong/harken-search/pull/42",
                   PR_AUTHOR="example-author", PR_MERGED_BY="example-maintainer",
                   PR_BODY="合并后在原频道看到通知。")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/slack_notify.py"), "--dry-run"],
                                env=env, capture_output=True, text=True, encoding="utf-8", timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PR #42 已合并", result.stdout)
        self.assertIn("合并后在原频道看到通知。", result.stdout)
        self.assertIn("xindong/harken-search/pull/42", result.stdout)


if __name__ == "__main__":
    unittest.main()
