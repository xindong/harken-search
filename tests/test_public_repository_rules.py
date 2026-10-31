import json
import subprocess
import sys
import unittest
from unittest.mock import patch

from tools.verify_public_repository import REQUIRED_CHECKS, evaluate_rules, main


def rules():
    return [
        {"type": "deletion"}, {"type": "non_fast_forward"},
        {"type": "pull_request", "parameters": {
            "required_review_thread_resolution": True,
            "required_approving_review_count": 0,
        }},
        {"type": "required_status_checks", "parameters": {
            "strict_required_status_checks_policy": True,
            "required_status_checks": [{"context": name} for name in REQUIRED_CHECKS],
        }},
    ]


class PublicRepositoryRulesTests(unittest.TestCase):
    def test_actual_checks_and_thread_resolution_allow_the_current_workflow(self):
        self.assertEqual(evaluate_rules(rules()), [])

    def test_a_missing_product_build_blocks_auto_merge(self):
        value = rules()
        value[-1]["parameters"]["required_status_checks"].pop()
        self.assertTrue(evaluate_rules(value))

    def test_missing_branch_or_review_protection_is_rejected(self):
        for index in (0, 1, 2):
            with self.subTest(index=index):
                value = rules()
                value.pop(index)
                self.assertTrue(evaluate_rules(value))

    def test_passed_checks_do_not_need_to_match_the_latest_code(self):
        value = rules()
        value[-1]["parameters"]["strict_required_status_checks_policy"] = False
        self.assertEqual(evaluate_rules(value), [])

    def test_missing_checks_still_block_when_latest_code_is_not_required(self):
        value = rules()
        value[-1]["parameters"]["strict_required_status_checks_policy"] = False
        value[-1]["parameters"]["required_status_checks"].pop()
        self.assertTrue(evaluate_rules(value))

    def test_qa_current_requires_the_same_effective_rules_as_main(self):
        self.assertEqual(evaluate_rules(rules(), "qa/current"), [])
        value = rules()
        value[-1]["parameters"]["required_status_checks"].pop()
        self.assertIn("qa/current", " ".join(evaluate_rules(value, "qa/current")))

    def test_cli_queries_actual_qa_branch_and_rejects_missing_rules(self):
        with patch.object(sys, "argv", ["verify", "--repository", "xindong/harken-search", "--branch", "qa/current"]), patch("tools.verify_public_repository.subprocess.run") as run:
            run.return_value = subprocess.CompletedProcess([], 0, json.dumps(rules()), "")
            self.assertEqual(main(), 0)
            self.assertEqual(run.call_args.args[0], ["gh", "api", "repos/xindong/harken-search/rules/branches/qa%2Fcurrent"])
            run.return_value = subprocess.CompletedProcess([], 0, "[]", "")
            self.assertEqual(main(), 1)

    def test_cli_preserves_main_gate_and_fails_when_remote_rules_cannot_be_read(self):
        with patch.object(sys, "argv", ["verify", "--repository", "xindong/harken-search", "--branch", "main"]), patch("tools.verify_public_repository.subprocess.run") as run:
            run.return_value = subprocess.CompletedProcess([], 0, json.dumps(rules()), "")
            self.assertEqual(main(), 0)
            self.assertEqual(run.call_args.args[0], ["gh", "api", "repos/xindong/harken-search/rules/branches/main"])
            run.return_value = subprocess.CompletedProcess([], 1, "", "rules unavailable")
            self.assertEqual(main(), 1)

    def test_cli_rejects_wrong_repository_and_target_before_remote_call(self):
        for repository, branch in (("xindong/aris", "main"), ("other/harken-search", "qa/current"), ("xindong/harken-search", "feature/topic")):
            with self.subTest(repository=repository, branch=branch), patch.object(sys, "argv", ["verify", "--repository", repository, "--branch", branch]), patch("tools.verify_public_repository.subprocess.run") as run:
                with self.assertRaises(SystemExit):
                    main()
                run.assert_not_called()
