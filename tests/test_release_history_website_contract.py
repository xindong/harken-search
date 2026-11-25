"""Source-side row checks for harken-website's importReleaseHistory.

Reference: HarkenSearch/frontend/src/lib/updates.mjs at
863915436426806224ad4a13cf58a991135701cb. The source's highest stable
version stands in for the manifest target; publication identity is not checked.
"""

from datetime import date
import json
from pathlib import Path
import re

import pytest


HISTORY = Path(__file__).resolve().parents[1] / "installer/harken_release_history.json"
VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)")
JS_WHITESPACE = (
    " \t\n\r\v\f\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006"
    "\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000\ufeff"
)


def _text(value):
    return value.strip(JS_WHITESPACE) if isinstance(value, str) else ""


def _array(value, field):
    if value is None:
        return []
    if not isinstance(value, list):
        raise ValueError(f"{field} must be an array")
    return value


def _public_rows(record):
    count = sum(
        bool(_text(item.get("text")))
        for item in _array(record.get("items"), "release items")
        if isinstance(item, dict) and item.get("category") in ("add", "fix", "improve")
    )
    for section in _array(record.get("sections"), "release sections"):
        title = _text(section.get("title")) if isinstance(section, dict) else ""
        if not title or title == "内部验证":
            continue
        for group in _array(section.get("items"), "section items"):
            if not isinstance(group, dict):
                continue
            for item in _array(group.get("list"), "group list"):
                count += bool(_text(item.get("description") if isinstance(item, dict) else item))
    return count


def _latest_public_release(history):
    if not isinstance(history, dict):
        raise ValueError("website release history must be an object")
    schema = history.get("schema_version")
    releases = history.get("releases")
    if isinstance(schema, bool) or schema not in (1, 2, 3) or not isinstance(releases, list) or not releases:
        raise ValueError("website release history needs a supported schema and nonempty releases")
    by_version = {}
    for record in releases:
        version = record.get("version") if isinstance(record, dict) else None
        if not isinstance(version, str) or not VERSION.fullmatch(version):
            raise ValueError(f"invalid stable release version: {version!r}")
        if version in by_version:
            raise ValueError(f"duplicate release version: {version}")
        by_version[version] = record

    version = max(by_version, key=lambda label: tuple(map(int, label.split("."))))
    record = by_version[version]
    release_date = _text(record.get("release_date"))
    try:
        if date.fromisoformat(release_date).isoformat() != release_date:
            raise ValueError
    except ValueError as error:
        raise ValueError(f"release {version} has an invalid release_date: {release_date!r}") from error
    count = _public_rows(record)
    if not count:
        raise ValueError(f"release {version} has no public website notes")
    return version, count


def _release(version="1.0.10", **fields):
    return {"version": version, "release_date": "2026-11-25", **fields}


def test_current_release_history_has_public_website_notes():
    history = json.loads(HISTORY.read_text(encoding="utf-8"))
    version, count = _latest_public_release(history)
    print(f"Website release {version}: {count} public note rows")


@pytest.mark.parametrize(
    ("fields", "count"),
    [
        (
            {"sections": [{"title": "Search", "items": [{"list": ["Text", {"description": "Object"}]}]}]},
            2,
        ),
        ({"sections": [{"title": "性能优化", "items": [{"list": [{"description": "Legacy"}]}]}]}, 1),
        ({"items": [{"category": "fix", "text": "Flat legacy"}], "sections": []}, 1),
    ],
    ids=["module", "legacy-section", "legacy-flat"],
)
def test_website_accepts_module_and_legacy_layouts(fields, count):
    older = _release("1.0.9", items=[{"category": "add", "text": "Older release"}])
    history = {"schema_version": 2, "releases": [older, _release(**fields)]}
    assert _latest_public_release(history) == ("1.0.10", count)


@pytest.mark.parametrize(
    "fields",
    [
        {"sections": []},
        {"sections": [{"title": " 内部验证 ", "items": [{"list": ["Not for the website"]}]}]},
        {"sections": [{"title": "Search", "items": [{"list": [{"title": "Title only"}, " \t", 42]}]}]},
        {"sections": [{"title": "Search", "items": [{"list": ["\ufeff"]}]}]},
        {"items": [{"category": "other", "text": "Unknown legacy category"}]},
    ],
    ids=["empty-sections", "private-only", "title-only", "zero-width-bom", "unknown-category"],
)
def test_latest_release_cannot_fall_back_to_older_public_notes(fields):
    older = _release("1.0.9", items=[{"category": "add", "text": "Older release"}])
    history = {"schema_version": 2, "releases": [older, _release(**fields)]}
    with pytest.raises(ValueError, match=r"release 1\.0\.10 has no public website notes"):
        _latest_public_release(history)


def test_website_rejects_invalid_latest_release_date():
    latest = _release(release_date="2026-02-30", items=[{"category": "fix", "text": "Valid detail"}])
    with pytest.raises(ValueError, match="invalid release_date"):
        _latest_public_release({"schema_version": 2, "releases": [latest]})
