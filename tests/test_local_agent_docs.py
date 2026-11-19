from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "docs" / "agent-methods" / "project-rules.md"
METHODS = ROOT / "docs" / "agent-methods"


def test_local_agent_entry_and_methods_are_utf8_and_complete() -> None:
    expected = [
        ENTRY,
        METHODS / "README.md",
        METHODS / "systematic-debugging.md",
        METHODS / "tdd.md",
        METHODS / "verification-before-completion.md",
        METHODS / "ui-acceptance.md",
        METHODS / "workflow.md",
    ]
    for path in expected:
        text = path.read_text(encoding="utf-8")
        assert chr(0xFFFD) not in text
        assert "?" * 2 not in text


def test_local_agent_docs_do_not_depend_on_old_external_repository_paths() -> None:
    machine_path = re.compile(
        r"(?i)(?:[a-z]:[/\\]|file://|(?<![\w./:])(?:/|~/)[\w.-]+/)"
    )
    for path in [ENTRY, *METHODS.glob("*.md")]:
        text = path.read_text(encoding="utf-8")
        assert not machine_path.search(text), path


def test_local_agent_links_resolve_inside_this_repository() -> None:
    sources = [ENTRY, METHODS / "README.md"]
    for source in sources:
        text = source.read_text(encoding="utf-8")
        for target in re.findall(r"\[[^]]+\]\(([^)]+)\)", text):
            if "://" in target or target.startswith("#"):
                continue
            resolved = (source.parent / target).resolve()
            assert resolved.is_relative_to(ROOT), (source, target)
            assert resolved.is_file(), (source, target)


def test_local_agent_rules_default_to_git_worktree() -> None:
    """每张卡默认用 Git worktree；独立 clone 只作为同分支被占用时的例外。"""
    entry = ENTRY.read_text(encoding="utf-8")
    workflow = (METHODS / "workflow.md").read_text(encoding="utf-8")

    assert "git worktree add" in entry
    assert "不新建 Git worktree" not in entry
    assert "clone" in entry
    assert "独立 clone" not in workflow
    assert "本卡目录" in workflow

def test_runtime_files_stay_outside_the_card_directory() -> None:
    """运行期文件放在卡目录外，避免混进产品改动或挡住收尾清理。"""
    entry = ENTRY.read_text(encoding="utf-8")
    workflow = (METHODS / "workflow.md").read_text(encoding="utf-8")

    assert "运行期文件" in entry
    assert "不写进卡目录" in entry
    assert "运行期文件" in workflow
