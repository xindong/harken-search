#!/usr/bin/env python3
"""Check identity fields in an actual built Harken Search macOS bundle."""

import argparse
import plistlib
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()

    contents = args.app / "Contents"
    with (contents / "Info.plist").open("rb") as source:
        info = plistlib.load(source)

    expected = {
        "CFBundleExecutable": "HarkenSearch",
        "CFBundleIdentifier": "com.xd.harkensearch",
        "CFBundleName": "Harken Search",
        "CFBundleShortVersionString": args.version,
        "CFBundleVersion": args.version,
        "CFBundleIconFile": "HarkenSearch.icns",
    }
    failures = []
    for key, value in expected.items():
        actual = info.get(key)
        print(f"{key}: {actual!r}")
        if actual != value:
            failures.append(f"{key}: expected {value!r}, got {actual!r}")

    executable = contents / "MacOS" / expected["CFBundleExecutable"]
    icon = contents / "Resources" / expected["CFBundleIconFile"]
    for path in (executable, icon):
        if not path.is_file():
            failures.append(f"missing bundle file: {path}")

    if failures:
        print("FAIL: " + "; ".join(failures))
        return 1
    print("PASS: built macOS bundle identity and files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
