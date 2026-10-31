"""Verify the effective rules protecting a public integration branch."""
import argparse
import json
import re
import subprocess
from urllib.parse import quote

REQUIRED_CHECKS = ("Harken Search / checks", "Harken Search / build and test")
REPOSITORY = "xindong/harken-search"
ALLOWED_BRANCHES = ("main", "qa/current")


def evaluate_rules(rules, branch="main"):
    if not isinstance(rules, list) or not all(isinstance(rule, dict) for rule in rules):
        return ["effective branch rules are malformed"]
    issues = []
    types = {rule.get("type") for rule in rules}
    for kind in ("deletion", "non_fast_forward"):
        if kind not in types:
            issues.append(f"{branch} does not enforce {kind}")
    reviews = [rule.get("parameters", {}) for rule in rules if rule.get("type") == "pull_request"]
    if not any(p.get("required_review_thread_resolution") is True for p in reviews):
        issues.append(f"{branch} must require pull requests and resolved review threads")
    checks = set()
    for rule in rules:
        if rule.get("type") != "required_status_checks":
            continue
        parameters = rule.get("parameters", {})
        checks.update(item.get("context") for item in parameters.get("required_status_checks", []) if isinstance(item, dict))
    for name in REQUIRED_CHECKS:
        if name not in checks:
            issues.append(f"{branch} does not require current checks: {name}")
    return issues


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--branch", required=True, choices=ALLOWED_BRANCHES)
    args = parser.parse_args()
    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository) is None:
        parser.error("repository must be owner/name")
    if args.repository != REPOSITORY:
        parser.error(f"repository must be {REPOSITORY}")
    response = subprocess.run(
        ["gh", "api", f"repos/{args.repository}/rules/branches/{quote(args.branch, safe='')}"],
        capture_output=True, text=True, check=False,
    )
    if response.returncode:
        print(f"Public {args.branch} rules could not be verified: " + response.stderr.strip())
        return 1
    try:
        issues = evaluate_rules(json.loads(response.stdout), args.branch)
    except (ValueError, TypeError, AttributeError):
        issues = ["effective branch rules are malformed"]
    if issues:
        print("\n".join(issues))
        return 1
    print(f"Public {args.branch} requires source checks, product tests and resolved review threads")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
