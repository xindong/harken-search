"""A card clone can be removed only after its work is on the QA branch."""
from pathlib import Path
import contextlib
import io
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import remove_card_directory as cleanup


REPOSITORY = "xindong/harken-search"
REF = "qa/current"


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True).stdout.strip()


class CardDirectoryCleanupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.origin = self.root / "origin.git"
        subprocess.run(["git", "init", "--bare", "-b", REF, str(self.origin)], check=True, capture_output=True)
        seed = self.root / "seed"
        subprocess.run(["git", "clone", str(self.origin), str(seed)], check=True, capture_output=True)
        git(seed, "config", "user.name", "CI")
        git(seed, "config", "user.email", "ci@example.invalid")
        (seed / "tracked.txt").write_text("first\n", encoding="utf-8")
        (seed / ".gitignore").write_text("*.local\nbuild/\n", encoding="utf-8")
        git(seed, "add", "tracked.txt", ".gitignore")
        git(seed, "commit", "-m", "first")
        git(seed, "push", "origin", "HEAD:" + REF)
        self.card = self.root / "card-one"
        self.main_repo = self.root / "main-repository"
        for target in (self.card, self.main_repo):
            subprocess.run(["git", "clone", str(self.origin), str(target)], check=True, capture_output=True)
        git(self.card, "config", "user.name", "CI")
        git(self.card, "config", "user.email", "ci@example.invalid")
        git(self.card, "remote", "set-url", "origin", "https://github.com/" + REPOSITORY + ".git")
        actual_git = cleanup.git

        def local_fetch(repo, *args):
            if args and args[0] == "fetch":
                if repo.samefile(self.card) and args[:3] == ("fetch", "--no-tags", "origin"):
                    return actual_git(repo, "fetch", "--no-tags", str(self.origin), *args[3:])
                raise AssertionError("unexpected fetch outside the isolated bare repository")
            if args[:3] == ("ls-remote", "--heads", "origin") and repo.samefile(self.card):
                return actual_git(repo, "ls-remote", "--heads", str(self.origin), *args[3:])
            if args and args[0] == "push" and "origin" in args and repo.samefile(self.card):
                rewritten = tuple(str(self.origin) if part == "origin" else part for part in args)
                return actual_git(repo, *rewritten)
            if args and args[0] in {"ls-remote", "push"}:
                raise AssertionError("unexpected network git outside the isolated bare repository")
            return actual_git(repo, *args)

        self.local_fetch = local_fetch

        fetch_patch = patch.object(cleanup, "git", side_effect=local_fetch)
        fetch_patch.start()
        self.addCleanup(fetch_patch.stop)

    def args(self, *mode):
        return [
            "--workspace-root", str(self.root), "--card-name", self.card.name,
            "--card-prefix", "card-", "--main-repository", str(self.main_repo),
            "--repository", REPOSITORY, "--integration-ref", REF,
            "--feature-branch", "feature/one-menu", *mode,
        ]

    def refused(self, *mode):
        with self.assertRaises(cleanup.CleanupRefused):
            cleanup.main(self.args(*mode))
        self.assertTrue((self.card / ".git").exists())

    def test_clean_merged_clone_needs_dry_run_and_exact_head_to_delete(self):
        head = git(self.card, "rev-parse", "HEAD")
        self.assertEqual(cleanup.main(self.args("--dry-run")), 0)
        self.assertTrue(self.card.exists())
        self.refused("--execute")
        self.refused("--execute", "--expected-head", "0" * 40)
        self.assertEqual(cleanup.main(self.args("--execute", "--expected-head", head)), 0)
        self.assertFalse(self.card.exists())
        self.assertTrue(self.main_repo.exists())
        self.assertTrue(self.origin.exists())

    def test_dirty_file_and_stash_are_preserved(self):
        (self.card / "untracked.txt").write_text("work", encoding="utf-8")
        self.refused("--dry-run")
        git(self.card, "stash", "push", "--include-untracked", "-m", "saved")
        self.refused("--dry-run")

    def test_ignored_database_from_git_info_exclude_blocks_dry_run_and_execute(self):
        (self.card / ".git/info/exclude").write_text("*.db\n", encoding="utf-8")
        database = self.card / "private.db"
        database.write_text("private", encoding="utf-8")
        self.assertEqual(git(self.card, "status", "--porcelain=v1", "--untracked-files=all"), "")
        self.assertIn("private.db", git(self.card, "ls-files", "--others", "--ignored", "--exclude-standard"))
        head = git(self.card, "rev-parse", "HEAD")
        self.refused("--dry-run")
        self.refused("--execute", "--expected-head", head)
        self.assertTrue(database.exists())

    def test_ignored_local_file_from_gitignore_blocks_dry_run_and_execute(self):
        local_file = self.card / "settings.local"
        local_file.write_text("local", encoding="utf-8")
        build = self.card / "build"
        build.mkdir()
        (build / "artifact.bin").write_bytes(b"test output")
        self.assertEqual(git(self.card, "status", "--porcelain=v1", "--untracked-files=all"), "")
        self.assertIn("settings.local", git(self.card, "ls-files", "--others", "--ignored", "--exclude-standard"))
        self.assertIn("build/artifact.bin", git(self.card, "ls-files", "--others", "--ignored", "--exclude-standard"))
        head = git(self.card, "rev-parse", "HEAD")
        self.refused("--dry-run")
        self.refused("--execute", "--expected-head", head)
        self.assertTrue(local_file.exists())

    def test_ignored_file_added_after_dry_run_blocks_execute(self):
        head = git(self.card, "rev-parse", "HEAD")
        self.assertEqual(cleanup.main(self.args("--dry-run")), 0)
        database = self.card / "settings.local"
        database.write_text("new after preflight", encoding="utf-8")
        self.refused("--execute", "--expected-head", head)
        self.assertTrue(database.exists())

    def test_ignored_file_added_after_commit_check_blocks_delete(self):
        head = git(self.card, "rev-parse", "HEAD")
        original_assert_no_links = cleanup.assert_no_links
        checks = 0

        def add_ignored_file_at_second_link_check(root):
            nonlocal checks
            checks += 1
            if checks == 2:
                (root / "late.local").write_text("new before delete", encoding="utf-8")
            return original_assert_no_links(root)

        with patch.object(cleanup, "assert_no_links", side_effect=add_ignored_file_at_second_link_check):
            self.refused("--execute", "--expected-head", head)
        self.assertTrue((self.card / "late.local").exists())

    def test_assume_unchanged_modified_file_is_preserved(self):
        self._assert_hidden_index_change_preserved("--assume-unchanged")

    def test_skip_worktree_modified_file_is_preserved(self):
        self._assert_hidden_index_change_preserved("--skip-worktree")

    def _assert_hidden_index_change_preserved(self, flag):
        tracked = self.card / "tracked.txt"
        git(self.card, "update-index", flag, "tracked.txt")
        tracked.write_text("hidden work\n", encoding="utf-8")
        self.assertEqual(git(self.card, "status", "--porcelain=v1", "--untracked-files=all", "--ignored=matching"), "")
        head = git(self.card, "rev-parse", "HEAD")
        self.refused("--dry-run")
        self.refused("--execute", "--expected-head", head)
        self.assertEqual(tracked.read_text(encoding="utf-8"), "hidden work\n")

    def test_external_detached_worktree_with_unique_commit_is_preserved(self):
        external = self.root / "linked-external"
        git(self.card, "worktree", "add", "--detach", str(external), "HEAD")
        (external / "tracked.txt").write_text("external unique work\n", encoding="utf-8")
        git(external, "commit", "-am", "external unique commit")
        unique_head = git(external, "rev-parse", "HEAD")
        self.assertNotEqual(unique_head, git(self.card, "rev-parse", "HEAD"))
        self.assertEqual(git(self.card, "status", "--porcelain=v1", "--untracked-files=all", "--ignored=matching"), "")
        head = git(self.card, "rev-parse", "HEAD")
        self.refused("--dry-run")
        self.refused("--execute", "--expected-head", head)
        self.assertEqual(git(external, "rev-parse", "HEAD"), unique_head)
        self.assertEqual((external / "tracked.txt").read_text(encoding="utf-8"), "external unique work\n")

    def _add_merged_submodule(self):
        child_origin = self.root / "child-origin.git"
        subprocess.run(["git", "init", "--bare", "-b", "main", str(child_origin)], check=True, capture_output=True)
        seed = self.root / "child-seed"
        subprocess.run(["git", "clone", str(child_origin), str(seed)], check=True, capture_output=True)
        git(seed, "config", "user.name", "CI")
        git(seed, "config", "user.email", "ci@example.invalid")
        (seed / ".gitignore").write_text("*.db\n", encoding="utf-8")
        (seed / "child.txt").write_text("saved\n", encoding="utf-8")
        git(seed, "add", ".gitignore", "child.txt")
        git(seed, "commit", "-m", "child baseline")
        git(seed, "push", "origin", "HEAD:main")
        git(self.card, "-c", "protocol.file.allow=always", "submodule", "add", str(child_origin), "modules/child")
        git(self.card, "commit", "-am", "add child")
        git(self.card, "push", str(self.origin), "HEAD:" + REF)
        return self.card / "modules/child"

    def test_clean_parent_with_ignored_submodule_database_is_preserved(self):
        child = self._add_merged_submodule()
        database = child / "private.db"
        database.write_text("unpublished private data", encoding="utf-8")
        self.assertEqual(git(self.card, "status", "--porcelain=v1", "--untracked-files=all", "--ignored=matching"), "")
        head = git(self.card, "rev-parse", "HEAD")
        self.refused("--dry-run")
        self.refused("--execute", "--expected-head", head)
        self.assertEqual(database.read_text(encoding="utf-8"), "unpublished private data")

    def test_deinitialized_submodule_repository_with_unique_ref_is_preserved(self):
        child = self._add_merged_submodule()
        git(child, "config", "user.name", "CI")
        git(child, "config", "user.email", "ci@example.invalid")
        git(child, "switch", "-c", "unpublished")
        (child / "child.txt").write_text("unpublished commit\n", encoding="utf-8")
        git(child, "commit", "-am", "keep child commit")
        child_head = git(child, "rev-parse", "HEAD")
        git(self.card, "submodule", "deinit", "-f", "--", "modules/child")
        git(self.card, "rm", "--cached", "modules/child")
        git(self.card, "commit", "-m", "remove child gitlink")
        git(self.card, "push", str(self.origin), "HEAD:" + REF)
        stored = self.card / ".git/modules/modules/child"
        self.assertEqual(git(stored, "rev-parse", "refs/heads/unpublished"), child_head)
        self.assertNotIn("160000 ", git(self.card, "ls-files", "--stage"))
        self.assertEqual(git(self.card, "status", "--porcelain=v1", "--untracked-files=all", "--ignored=matching"), "")
        head = git(self.card, "rev-parse", "HEAD")
        self.refused("--dry-run")
        self.refused("--execute", "--expected-head", head)
        self.assertEqual(git(stored, "rev-parse", "refs/heads/unpublished"), child_head)

    def test_submodule_storage_added_after_commit_check_blocks_delete(self):
        head = git(self.card, "rev-parse", "HEAD")
        original_assert_no_links = cleanup.assert_no_links
        checks = 0
        stored = self.card / ".git/modules/orphan"

        def add_stored_repository_at_second_link_check(root):
            nonlocal checks
            checks += 1
            if checks == 2:
                subprocess.run(["git", "init", "--bare", str(stored)], check=True, capture_output=True)
            return original_assert_no_links(root)

        with patch.object(cleanup, "assert_no_links", side_effect=add_stored_repository_at_second_link_check):
            self.refused("--execute", "--expected-head", head)
        self.assertTrue((stored / "HEAD").is_file())

    def test_nested_protected_main_repository_is_preserved_without_submodule(self):
        protected = self.card / ".git/protected-main.git"
        subprocess.run(["git", "init", "--bare", str(protected)], check=True, capture_output=True)
        self.assertTrue((protected / "HEAD").is_file())
        self.assertEqual(git(self.card, "ls-files", "--stage").count("160000 "), 0)
        self.assertEqual(git(self.card, "status", "--porcelain=v1", "--untracked-files=all", "--ignored=matching"), "")
        head = git(self.card, "rev-parse", "HEAD")
        self.refused("--main-repository", str(protected), "--dry-run")
        self.refused("--main-repository", str(protected), "--execute", "--expected-head", head)
        self.assertTrue((protected / "HEAD").is_file())

    def test_hidden_branch_with_unique_commit_is_preserved(self):
        git(self.card, "switch", "-c", "backup/unfinished")
        (self.card / "tracked.txt").write_text("new\n", encoding="utf-8")
        git(self.card, "commit", "-am", "unfinished")
        git(self.card, "switch", REF)
        self.refused("--dry-run")

    def test_tag_with_unique_commit_is_preserved(self):
        git(self.card, "switch", "-c", "backup/tagged")
        (self.card / "tracked.txt").write_text("tagged only\n", encoding="utf-8")
        git(self.card, "commit", "-am", "tagged only")
        git(self.card, "tag", "saved-work")
        git(self.card, "switch", REF)
        git(self.card, "branch", "-D", "backup/tagged")
        head = git(self.card, "rev-parse", "HEAD")
        self.refused("--dry-run")
        self.refused("--execute", "--expected-head", head)
        self.assertEqual(git(self.card, "tag", "-l", "saved-work"), "saved-work")

    def test_non_commit_tag_is_preserved_when_commit_proof_is_impossible(self):
        blob = git(self.card, "hash-object", "tracked.txt")
        git(self.card, "tag", "saved-blob", blob)
        self.refused("--dry-run")
        self.assertEqual(git(self.card, "tag", "-l", "saved-blob"), "saved-blob")

    def test_fetch_fixture_matches_equivalent_path_without_network(self):
        alias = self.card / ".." / self.card.name
        self.assertNotEqual(alias, self.card)
        self.assertTrue(alias.samefile(self.card))
        result = self.local_fetch(alias, "fetch", "--no-tags", "origin", "+refs/heads/" + REF + ":refs/remotes/origin/" + REF)
        self.assertEqual(result.returncode, 0)
        with self.assertRaisesRegex(AssertionError, "unexpected fetch"):
            self.local_fetch(self.main_repo, "fetch", "--no-tags", "origin", "+refs/heads/" + REF + ":refs/remotes/origin/" + REF)

    def test_current_head_with_unique_commit_is_preserved(self):
        (self.card / "tracked.txt").write_text("not merged", encoding="utf-8")
        git(self.card, "commit", "-am", "not merged")
        self.refused("--dry-run")

    def test_dry_run_fetches_the_latest_integration_ref(self):
        old_ref = git(self.card, "rev-parse", "origin/" + REF)
        seed = self.root / "seed"
        (seed / "tracked.txt").write_text("accepted later", encoding="utf-8")
        git(seed, "commit", "-am", "accepted later")
        git(seed, "push", "origin", "HEAD:" + REF)
        self.assertEqual(cleanup.main(self.args("--dry-run")), 0)
        self.assertNotEqual(git(self.card, "rev-parse", "origin/" + REF), old_ref)
        self.assertEqual(git(self.card, "rev-parse", "origin/" + REF), git(seed, "rev-parse", "HEAD"))

    def test_wrong_remote_or_missing_qa_ref_is_preserved(self):
        git(self.card, "remote", "set-url", "origin", str(self.origin))
        self.refused("--dry-run")
        git(self.card, "remote", "set-url", "origin", "https://github.com/" + REPOSITORY + ".git")
        self.refused("--integration-ref", "qa/missing", "--dry-run")

    def test_url_rewrite_cannot_redirect_valid_looking_origin(self):
        git(self.card, "config", "url." + self.origin.as_uri() + ".insteadOf", "https://github.com/" + REPOSITORY + ".git")
        self.refused("--dry-run")

    def test_ssh_alias_must_resolve_to_github(self):
        git(self.card, "remote", "set-url", "origin", "git@github-test-alias:" + REPOSITORY + ".git")
        with patch.object(cleanup, "ssh_hostname", return_value="github.com"):
            self.assertEqual(cleanup.main(self.args("--dry-run")), 0)
        with patch.object(cleanup, "ssh_hostname", return_value="other.example"):
            self.refused("--dry-run")

    def test_path_escape_main_repo_and_existing_target_lock_are_refused(self):
        with self.assertRaises(cleanup.CleanupRefused):
            cleanup.main(self.args("--dry-run") + ["--card-name", "../seed"])
        with self.assertRaises(cleanup.CleanupRefused):
            cleanup.main(self.args("--dry-run") + ["--card-name", "card-missing"])
        with self.assertRaises(cleanup.CleanupRefused):
            cleanup.main(self.args("--dry-run") + ["--main-repository", str(self.card)])
        lock = self.root / ".card-one.cleanup.lock"
        lock.write_text("another owner", encoding="utf-8")
        self.refused("--dry-run")
        self.assertEqual(lock.read_text(encoding="utf-8"), "another owner")

    def test_linked_card_or_nested_link_is_refused(self):
        link = self.root / "card-linked"
        try:
            os.symlink(self.card, link, target_is_directory=True)
        except OSError:
            link.mkdir()
            real_check = cleanup.is_reparse
            with patch.object(cleanup, "is_reparse", side_effect=lambda path: Path(path) == link or real_check(path)):
                with self.assertRaisesRegex(cleanup.CleanupRefused, "链接|重解析"):
                    cleanup.main(self.args("--dry-run") + ["--card-name", link.name])
        else:
            with self.assertRaisesRegex(cleanup.CleanupRefused, "链接|重解析"):
                cleanup.main(self.args("--dry-run") + ["--card-name", link.name])
        nested = self.card / "linked-file"
        try:
            os.symlink(self.main_repo / "tracked.txt", nested)
        except OSError:
            nested.write_text("simulated link", encoding="utf-8")
            real_check = cleanup.is_reparse
            with patch.object(cleanup, "is_reparse", side_effect=lambda path: Path(path) == nested or real_check(path)):
                with self.assertRaisesRegex(cleanup.CleanupRefused, "链接|重解析"):
                    cleanup.main(self.args("--dry-run"))
        else:
            with self.assertRaisesRegex(cleanup.CleanupRefused, "链接|重解析"):
                cleanup.main(self.args("--dry-run"))

    def test_read_failure_and_unsafe_repository_argument_refuse_before_delete(self):
        git(self.card, "config", "--unset", "remote.origin.url")
        self.refused("--dry-run")
        with self.assertRaises(cleanup.CleanupRefused):
            cleanup.main(self.args("--dry-run") + ["--repository", "owner/repo;echo danger"])


    def ref_exists(self, repo, ref):
        return subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "--verify", "--quiet", ref],
            capture_output=True,
        ).returncode == 0

    def publish_merged_feature_branch(self):
        branch = "feature/one-menu"
        git(self.card, "switch", "-c", branch)
        (self.card / "tracked.txt").write_text("merged feature\n", encoding="utf-8")
        git(self.card, "commit", "-am", "merged feature")
        git(self.card, "push", str(self.origin), "HEAD:" + branch)
        git(self.card, "push", str(self.origin), "HEAD:" + REF)
        git(self.main_repo, "fetch", str(self.origin), "+" + branch + ":refs/heads/" + branch)
        return branch

    def test_dry_run_keeps_merged_feature_branch_until_execute(self):
        self.publish_merged_feature_branch()
        head = git(self.card, "rev-parse", "HEAD")
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(cleanup.main(self.args("--dry-run")), 0)
        text = output.getvalue()
        self.assertIn("feature/one-menu", text)
        self.assertIn("将删除", text)
        self.assertNotIn("已删除", text)
        self.assertTrue(self.card.exists())
        self.assertTrue(self.ref_exists(self.main_repo, "refs/heads/feature/one-menu"))
        self.assertTrue(self.ref_exists(self.origin, "refs/heads/feature/one-menu"))
        self.refused("--execute", "--expected-head", "0" * 40)
        self.assertTrue(self.ref_exists(self.origin, "refs/heads/feature/one-menu"))
        self.assertEqual(cleanup.main(self.args("--execute", "--expected-head", head)), 0)
        self.assertFalse(self.card.exists())
        self.assertFalse(self.ref_exists(self.main_repo, "refs/heads/feature/one-menu"))
        self.assertFalse(self.ref_exists(self.origin, "refs/heads/feature/one-menu"))
        self.assertTrue(self.main_repo.exists())
        self.assertTrue(self.ref_exists(self.main_repo, "refs/heads/" + REF))
        self.assertTrue(self.ref_exists(self.origin, "refs/heads/" + REF))

    def test_unmerged_feature_branch_is_preserved(self):
        branch = "feature/one-menu"
        git(self.card, "switch", "-c", branch)
        (self.card / "tracked.txt").write_text("not merged feature\n", encoding="utf-8")
        git(self.card, "commit", "-am", "not merged feature")
        git(self.card, "push", str(self.origin), "HEAD:" + branch)
        git(self.main_repo, "fetch", str(self.origin), "+" + branch + ":refs/heads/" + branch)
        self.refused("--dry-run")
        self.assertTrue(self.card.exists())
        self.assertTrue(self.ref_exists(self.main_repo, "refs/heads/" + branch))
        self.assertTrue(self.ref_exists(self.origin, "refs/heads/" + branch))

    def test_unique_commit_on_main_repository_branch_is_preserved(self):
        self.publish_merged_feature_branch()
        git(self.main_repo, "config", "user.name", "CI")
        git(self.main_repo, "config", "user.email", "ci@example.invalid")
        git(self.main_repo, "switch", "feature/one-menu")
        (self.main_repo / "tracked.txt").write_text("only in main\n", encoding="utf-8")
        git(self.main_repo, "commit", "-am", "only in main")
        unique = git(self.main_repo, "rev-parse", "HEAD")
        git(self.main_repo, "switch", REF)
        self.refused("--dry-run")
        self.assertEqual(git(self.main_repo, "rev-parse", "refs/heads/feature/one-menu"), unique)
        self.assertTrue(self.ref_exists(self.origin, "refs/heads/feature/one-menu"))

    def test_checked_out_feature_branch_in_main_repository_is_preserved(self):
        self.publish_merged_feature_branch()
        git(self.main_repo, "switch", "feature/one-menu")
        self.refused("--dry-run")
        self.assertEqual(git(self.main_repo, "branch", "--show-current"), "feature/one-menu")
        self.assertTrue(self.ref_exists(self.origin, "refs/heads/feature/one-menu"))

    def test_protected_or_unrelated_branch_names_are_refused(self):
        for branch in (REF, "main", "feature/other-card"):
            with self.assertRaises(cleanup.CleanupRefused):
                cleanup.main(self.args("--dry-run", "--feature-branch", branch))
            self.assertTrue((self.card / ".git").exists())

if __name__ == "__main__":
    unittest.main()
