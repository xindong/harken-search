"""Fail when an enabled CTest entry points at a missing Python test."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess


def missing_inputs(test: dict, default_directory: Path) -> list[str]:
    command = test.get("command", [])
    if not command or "python" not in Path(command[0]).name.lower():
        return []
    properties = {item["name"]: item["value"] for item in test.get("properties", [])}
    cwd = Path(properties.get("WORKING_DIRECTORY", default_directory))
    required = [Path(arg) for index, arg in enumerate(command[1:], start=1)
                if arg.endswith(".py") and "*" not in arg and command[index - 1] != "-p"]
    if "unittest" in command:
        for arg in command[command.index("unittest") + 1:]:
            if arg.startswith("tests."):
                required.append(Path(*arg.split(".")).with_suffix(".py"))
        if "-s" in command:
            required.append(Path(command[command.index("-s") + 1]))
    return [str(path) for path in required if not (cwd / path).exists()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="RelWithDebInfo")
    args = parser.parse_args()
    result = subprocess.run(
        ["ctest", "--test-dir", str(args.build_dir), "-C", args.config, "--show-only=json-v1"],
        capture_output=True, text=True, encoding="utf-8", check=True,
    )
    tests = json.loads(result.stdout)["tests"]
    missing = {test["name"]: paths for test in tests
               if (paths := missing_inputs(test, args.build_dir))}
    print(json.dumps({"registeredTests": len(tests), "missingInputs": missing}, indent=2))
    return int(bool(missing))


if __name__ == "__main__":
    raise SystemExit(main())
