"""Remove exactly one clean, merged card clone and its merged feature branch after task closeout."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import uuid


REPARSE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
REPOSITORY_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$")
CARD_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*$")
BRANCH_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/-]*$")
COMMIT_ID = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})$")


class CleanupRefused(RuntimeError):
    """A precondition for deletion was not proved."""


def refuse(message: str) -> None:
    raise CleanupRefused(message)


def git(repo: Path, *args: str) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            ["git", "-C", str(repo), *args], capture_output=True, text=True,
            encoding="utf-8", errors="replace", check=False, timeout=60,
            env={**os.environ, "GIT_TERMINAL_PROMPT": "0"},
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        refuse("无法读取或刷新 Git 状态：" + str(error))


def git_output(repo: Path, *args: str) -> str:
    result = git(repo, *args)
    if result.returncode:
        refuse("Git 核验失败：" + " ".join(args))
    return result.stdout.strip()


def is_reparse(path: Path) -> bool:
    if path.is_symlink():
        return True
    try:
        return bool(path.lstat().st_file_attributes & REPARSE)
    except (AttributeError, FileNotFoundError):
        return False


def absolute_directory(raw: str, label: str) -> Path:
    path = Path(raw)
    if not path.is_absolute() or not path.is_dir():
        refuse(label + "必须是已存在的绝对目录")
    for part in (path, *path.parents):
        if is_reparse(part):
            refuse(label + "经过链接或重解析点")
    return path.resolve(strict=True)


def validate_names(repository: str, integration_ref: str, card_name: str, card_prefix: str) -> None:
    if not REPOSITORY_NAME.fullmatch(repository) or any(part in {".", ".."} for part in repository.split("/")):
        refuse("仓库名称必须是安全的 owner/repo 格式")
    if not BRANCH_NAME.fullmatch(integration_ref) or git(Path.cwd(), "check-ref-format", "refs/heads/" + integration_ref).returncode:
        refuse("集成分支名称无效")
    if not CARD_NAME.fullmatch(card_name) or not CARD_NAME.fullmatch(card_prefix) or not card_name.startswith(card_prefix) or len(card_name) <= len(card_prefix):
        refuse("卡目录名必须是指定前缀下的单个目录名")


def assert_no_links(root: Path) -> None:
    if is_reparse(root):
        refuse("卡目录是链接或重解析点")
    for current, dirnames, filenames in os.walk(root, followlinks=False):
        for name in (*dirnames, *filenames):
            if is_reparse(Path(current) / name):
                refuse("卡目录包含链接或重解析点")


def ssh_hostname(alias: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]*", alias):
        refuse("SSH 主机别名无效")
    try:
        result = subprocess.run(
            ["ssh", "-G", alias], capture_output=True, text=True,
            encoding="utf-8", errors="replace", check=False, timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        refuse("无法核验 SSH 主机别名")
    if result.returncode:
        refuse("无法核验 SSH 主机别名")
    names = [line.split(None, 1)[1].strip().lower() for line in result.stdout.splitlines()
             if line.lower().startswith("hostname ")]
    if len(names) != 1:
        refuse("SSH 主机别名没有唯一的目标主机")
    return names[0]


def assert_origin(root: Path, repository: str) -> None:
    configured = git_output(root, "config", "--get-all", "remote.origin.url").splitlines()
    effective = git_output(root, "remote", "get-url", "--all", "origin").splitlines()
    if len(configured) != 1 or len(effective) != 1:
        refuse("origin 必须只有一个远端地址")

    def matches(url: str) -> bool:
        for suffix in (repository, repository + ".git"):
            if url == "https://github.com/" + suffix:
                return True
            for pattern in (r"git@([A-Za-z0-9][A-Za-z0-9.-]*):" + re.escape(suffix),
                            r"ssh://git@([A-Za-z0-9][A-Za-z0-9.-]*)/" + re.escape(suffix)):
                match = re.fullmatch(pattern, url)
                if match and (match.group(1) == "github.com" or ssh_hostname(match.group(1)) == "github.com"):
                    return True
        return False

    if not matches(configured[0]) or not matches(effective[0]):
        refuse("origin 不是指定的 GitHub 仓库")


def assert_no_local_content(root: Path) -> None:
    indexed = git_output(root, "ls-files", "-v", "-z")
    if any(entry and (entry[0] == "S" or entry[0].islower()) for entry in indexed.split("\0")):
        refuse("索引含 assume-unchanged 或 skip-worktree 标记；无法证明文件未修改")
    if git_output(root, "status", "--porcelain=v1", "--untracked-files=all", "--ignored=matching"):
        refuse("目录还有未保存或被 Git 忽略的文件")


def assert_no_linked_worktrees(root: Path) -> None:
    worktrees = [line.removeprefix("worktree ") for line in git_output(root, "worktree", "list", "--porcelain").splitlines()
                 if line.startswith("worktree ")]
    if len(worktrees) != 1 or Path(worktrees[0]).resolve(strict=True) != root:
        refuse("目录关联其他 Git worktree；请先单独核验并保留其工作和提交")


def assert_no_submodules(root: Path) -> None:
    if (root / ".git/modules").exists():
        refuse("目录保留子模块仓库数据；拒绝删除")
    indexed = git_output(root, "ls-files", "--stage", "-z")
    if any(entry.startswith("160000 ") for entry in indexed.split("\0")):
        refuse("目录含 Git 子模块；拒绝删除")


def assert_merged_and_clean(root: Path, repository: str, integration_ref: str) -> str:
    if not (root / ".git").is_dir() or Path(git_output(root, "rev-parse", "--show-toplevel")).resolve(strict=True) != root:
        refuse("目标不是独立的卡目录仓库")
    assert_origin(root, repository)
    assert_no_linked_worktrees(root)
    assert_no_submodules(root)
    assert_no_local_content(root)
    if git_output(root, "stash", "list", "--format=%gd"):
        refuse("目录还有 stash")
    remote_ref = "refs/remotes/origin/" + integration_ref
    source_ref = "refs/heads/" + integration_ref
    git_output(root, "fetch", "--no-tags", "origin", "+" + source_ref + ":" + remote_ref)
    qa_head = git_output(root, "rev-parse", "--verify", remote_ref + "^{commit}")
    if not COMMIT_ID.fullmatch(qa_head):
        refuse("无法核验集成分支提交")
    head = git_output(root, "rev-parse", "--verify", "HEAD^{commit}")
    saved_refs = git_output(root, "for-each-ref", "--format=%(refname)", "refs/heads", "refs/tags")
    for ref in ("HEAD", *saved_refs.splitlines()):
        if git(root, "merge-base", "--is-ancestor", ref + "^{commit}", qa_head).returncode:
            refuse("HEAD、本地分支或标签无法证明已进入最新集成分支：" + ref)
    return head


@contextmanager
def target_lock(workspace_root: Path, card_name: str):
    lock = workspace_root / ("." + card_name + ".cleanup.lock")
    token = uuid.uuid4().hex
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        refuse("这张卡已有清理锁；请先人工核验占用者")
    except OSError as error:
        refuse("无法锁定这张卡：" + str(error))
    try:
        with os.fdopen(fd, "w", encoding="ascii") as handle:
            handle.write(token)
        yield
    finally:
        try:
            if lock.read_text(encoding="ascii") == token:
                lock.unlink()
        except FileNotFoundError:
            # The lock is already absent, so there is nothing left to release.
            pass


def delete_tree(root: Path) -> None:
    assert_no_links(root)
    assert_no_linked_worktrees(root)
    assert_no_submodules(root)
    assert_no_local_content(root)
    for current, dirnames, filenames in os.walk(root, topdown=False, followlinks=False):
        for name in filenames:
            path = Path(current) / name
            if is_reparse(path) or not path.resolve(strict=True).is_relative_to(root):
                refuse("删除时发现不安全的文件")
            os.chmod(path, stat.S_IWRITE)
            path.unlink()
        for name in dirnames:
            path = Path(current) / name
            if is_reparse(path) or not path.resolve(strict=True).is_relative_to(root):
                refuse("删除时发现不安全的目录")
            path.rmdir()
    root.rmdir()



def assert_feature_branch_name(branch: str, integration_ref: str, card_name: str, card_prefix: str) -> None:
    if branch in {integration_ref, "main", "master", "HEAD"}:
        refuse("拒绝删除当前可用版本或正式发布分支")
    if not BRANCH_NAME.fullmatch(branch) or git(Path.cwd(), "check-ref-format", "refs/heads/" + branch).returncode:
        refuse("功能分支名称无效")
    identity = card_name[len(card_prefix):]
    if not any(part == identity or part.startswith(identity + "-") for part in branch.split("/")):
        refuse("功能分支不属于这一张卡")


def branch_sha(repo: Path, branch: str) -> str | None:
    result = git(repo, "show-ref", "--verify", "refs/heads/" + branch)
    if result.returncode:
        return None
    parts = result.stdout.split()
    if len(parts) != 2 or parts[1] != "refs/heads/" + branch or not COMMIT_ID.fullmatch(parts[0]):
        refuse("功能分支指针无法核验")
    return parts[0]


def remote_branch_sha(repo: Path, branch: str) -> str | None:
    result = git(repo, "ls-remote", "--heads", "origin", "refs/heads/" + branch)
    if result.returncode:
        refuse("无法核验远端功能分支")
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        return None
    if len(lines) != 1:
        refuse("远端功能分支不唯一")
    parts = lines[0].split()
    if len(parts) != 2 or parts[1] != "refs/heads/" + branch or not COMMIT_ID.fullmatch(parts[0]):
        refuse("远端功能分支结果无法核验")
    return parts[0]


def assert_commit_merged(card: Path, sha: str, qa_head: str, label: str) -> None:
    if git(card, "cat-file", "-e", sha + "^{commit}").returncode:
        refuse(label + "的提交无法在本卡目录中核验")
    if git(card, "merge-base", "--is-ancestor", sha, qa_head).returncode:
        refuse(label + "还没有进入最新集成分支")


def checked_out(repo: Path, branch: str) -> bool:
    current = git(repo, "symbolic-ref", "--quiet", "HEAD")
    return current.returncode == 0 and current.stdout.strip() == "refs/heads/" + branch


def prove_feature_branches(card: Path, main_repo: Path, branch: str, integration_ref: str) -> tuple[str | None, str | None]:
    qa_head = git_output(card, "rev-parse", "--verify", "refs/remotes/origin/" + integration_ref + "^{commit}")
    remote_sha = remote_branch_sha(card, branch)
    if remote_sha:
        git_output(card, "fetch", "--no-tags", "origin", "+refs/heads/" + branch + ":refs/remotes/origin/" + branch)
        assert_commit_merged(card, remote_sha, qa_head, "远端功能分支")
    main_sha = branch_sha(main_repo, branch)
    if main_sha:
        if checked_out(main_repo, branch):
            refuse("主仓正停在这条功能分支上")
        assert_commit_merged(card, main_sha, qa_head, "主仓功能分支")
    return remote_sha, main_sha


def delete_feature_branches(card: Path, main_repo: Path, branch: str, remote_sha: str | None, main_sha: str | None) -> None:
    if main_sha:
        current_sha = branch_sha(main_repo, branch)
        if current_sha and current_sha != main_sha:
            refuse("主仓功能分支已变化，与预检结果不符")
        if current_sha and checked_out(main_repo, branch):
            refuse("主仓正停在这条功能分支上")
    if remote_sha:
        lease = "--force-with-lease=refs/heads/" + branch + ":" + remote_sha
        if git(card, "push", "origin", lease, ":refs/heads/" + branch).returncode:
            refuse("删除远端功能分支失败")
    if main_sha:
        current_sha = branch_sha(main_repo, branch)
        if current_sha and current_sha != main_sha:
            refuse("主仓功能分支已变化，与预检结果不符")
        if not current_sha:
            return
        if checked_out(main_repo, branch):
            refuse("主仓正停在这条功能分支上")
        if git(main_repo, "update-ref", "-d", "refs/heads/" + branch).returncode:
            refuse("删除主仓功能分支失败")


def print_cleanup_plan(target: Path, head: str, branch: str, integration_ref: str, remote_sha: str | None, main_sha: str | None) -> None:
    print("预检通过；目标：" + str(target))
    print("HEAD：" + head)
    print("功能分支：" + branch)
    print("主仓本地分支：" + ("将删除" if main_sha else "已不在"))
    print("远端分支：" + ("将删除" if remote_sha else "已不在"))
    print("不删除集成分支：" + integration_ref)
    print("任务完成与归档状态须由调用者另行核验；删除时传入 --execute --expected-head " + head)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace-root", required=True, help="卡目录的直属父目录，须为绝对路径")
    parser.add_argument("--card-name", required=True, help="只删除这一张卡的目录名")
    parser.add_argument("--card-prefix", required=True, help="卡目录名必须以此前缀开头")
    parser.add_argument("--main-repository", required=True, help="不可删除的主仓目录绝对路径")
    parser.add_argument("--repository", required=True, help="期望的 GitHub owner/repo")
    parser.add_argument("--integration-ref", default="qa/current", help="已验收集成分支，默认 qa/current")
    parser.add_argument("--feature-branch", required=True, help="只删除这一张卡已合并的功能分支")
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--dry-run", action="store_true", help="只核验，输出当前 HEAD")
    modes.add_argument("--execute", action="store_true", help="明确删除，须同时给出预检 HEAD")
    parser.add_argument("--expected-head", help="预检输出的完整 HEAD SHA")
    args = parser.parse_args(argv)

    validate_names(args.repository, args.integration_ref, args.card_name, args.card_prefix)
    workspace_root = absolute_directory(args.workspace_root, "工作目录")
    main_repo = absolute_directory(args.main_repository, "主仓目录")
    target = workspace_root / args.card_name
    if is_reparse(target):
        refuse("卡目录是链接或重解析点")
    if not target.is_dir() or target.resolve(strict=True).parent != workspace_root:
        refuse("目标必须是工作目录内直属的真实卡目录")
    if main_repo.is_relative_to(target.resolve(strict=True)):
        refuse("拒绝删除主仓库或包含主仓库的目录")
    assert_feature_branch_name(args.feature_branch, args.integration_ref, args.card_name, args.card_prefix)
    if args.execute and (not args.expected_head or not COMMIT_ID.fullmatch(args.expected_head)):
        refuse("执行删除必须给出预检得到的完整 HEAD SHA")
    with target_lock(workspace_root, args.card_name):
        assert_no_links(target)
        head = assert_merged_and_clean(target, args.repository, args.integration_ref)
        remote_sha, main_sha = prove_feature_branches(target, main_repo, args.feature_branch, args.integration_ref)
        if args.dry_run:
            print_cleanup_plan(target, head, args.feature_branch, args.integration_ref, remote_sha, main_sha)
            return 0
        if head != args.expected_head:
            refuse("HEAD 已变化，与预检结果不符")
        assert_no_links(target)
        remote_sha, main_sha = prove_feature_branches(target, main_repo, args.feature_branch, args.integration_ref)
        delete_feature_branches(target, main_repo, args.feature_branch, remote_sha, main_sha)
        delete_tree(target)
        print("已删除这一张卡目录：" + str(target))
        if remote_sha or main_sha:
            print("已删除功能分支：" + args.feature_branch)
        else:
            print("功能分支已不在：" + args.feature_branch)
        return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CleanupRefused as error:
        print("拒绝清理：" + str(error), file=sys.stderr)
        raise SystemExit(1) from None
