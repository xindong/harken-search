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
    machine_path = re.compile(r"(?i)(?:[a-z]:[/\\]|file://)")
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
            assert (source.parent / target).resolve().is_file(), (source, target)
