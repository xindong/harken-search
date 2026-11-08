from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


REQUIRED_FILES = (
    "HarkenSearch.exe",
    "HarkenSearchBootstrap.exe",
    "HarkenSearch.exe.sig",
    "HarkenSearchBootstrap.exe.sig",
    "harken-runtime-pair.json",
    "WebView2Loader.dll",
    "lucide.ttf",
    "splash_beta.png",
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "ASSET_PROVENANCE.md",
    "BINARY_PROVENANCE.md",
    "PRIVACY.md",
    "AI_FEATURE_DISCLOSURE.md",
)
REQUIRED_DIRECTORIES = (
    "assets/icons/brand",
    "assets/logos/brand",
    "assets/avatars",
    "LICENSES",
)

SIMILARITY_COMPONENT_FILES = {
    "ONNX Runtime": ("onnxruntime.dll",),
    "LAION CLAP HTSAT unfused audio encoder": (
        "models/clap/clap-audio.onnx",
        "models/clap/clap-mel-filters.f64",
        "models/clap/clap-window.f64",
    ),
}

PAIR_FIELDS = {
    "arisSha256",
    "arisSize",
    "bootstrapSha256",
    "bootstrapSize",
    "buildId",
    "channel",
    "config",
    "packageKeyId",
    "publicKeyDigest",
    "schemaVersion",
    "sourceCommit",
}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _integrity_sidecar(path: Path) -> bytes:
    return hashlib.sha512(path.read_bytes()).digest()[:32]


def configured_similarity_files(root: Path) -> tuple[str, ...]:
    metadata = root / "THIRD_PARTY_COMPONENTS.json"
    if not metadata.is_file():
        return ()
    payload = json.loads(metadata.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("THIRD_PARTY_COMPONENTS.json: root must be an object")
    components = payload.get("components")
    if not isinstance(components, list):
        raise ValueError("THIRD_PARTY_COMPONENTS.json: components must be a list")
    required = []
    for component in components:
        if not isinstance(component, dict):
            raise ValueError("THIRD_PARTY_COMPONENTS.json: component must be an object")
        name = component.get("name")
        if component.get("distributed") is True and isinstance(name, str):
            required.extend(SIMILARITY_COMPONENT_FILES.get(name, ()))
    return tuple(required)


def verify_startup_source(source_root: Path) -> list[str]:
    flags_path = source_root / "src" / "BetaFeatureFlags.h"
    entry_path = source_root / "src" / "WindowsMain.cpp"
    try:
        flags = flags_path.read_text(encoding="utf-8")
        entry = entry_path.read_text(encoding="utf-8")
    except OSError as error:
        return [f"startup source unavailable: {error}"]

    issues = []
    if not re.search(r"\bkShowStartupLogin\s*=\s*true\s*;", flags):
        issues.append("startup welcome disabled in release source")
    if not re.search(r"\bkUseBetaSplashLoading\s*=\s*false\s*;", flags):
        issues.append("startup web loading disabled in release source")
    if "else if (!kShowStartupLogin && kUseBetaSplash)" not in entry:
        issues.append("startup welcome bypass is not gated by beta splash")
    if "RunWebView2LoginScreen(hwnd" not in entry:
        issues.append("startup welcome WebView entry missing")
    if "RunWebView2LoadingScreen(hwnd" not in entry:
        issues.append("startup loading WebView entry missing")
    return issues


def verify_release_assembly(
    output_dir: str | Path,
    expected_source_commit: str | None = None,
    source_root: str | Path | None = None,
) -> list[str]:
    root = Path(output_dir)
    issues = verify_startup_source(Path(source_root or Path(__file__).resolve().parents[1]))
    for relative in REQUIRED_FILES:
        path = root / relative
        if not path.is_file() or path.stat().st_size == 0:
            issues.append(f"missing required release file: {relative}")
    for relative in REQUIRED_DIRECTORIES:
        path = root / relative
        if not path.is_dir() or not any(path.rglob("*")):
            issues.append(f"missing required release directory: {relative}")

    try:
        similarity_files = configured_similarity_files(root)
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        issues.append(f"invalid similarity metadata: {error}")
    else:
        for relative in similarity_files:
            path = root / relative
            if not path.is_file() or path.stat().st_size == 0:
                issues.append(f"missing configured similarity file: {relative}")

    receipt = root / "harken-runtime-pair.json"
    if receipt.is_file() and receipt.stat().st_size:
        try:
            payload = json.loads(receipt.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            issues.append(f"invalid harken-runtime-pair.json: {error}")
        else:
            if not isinstance(payload, dict) or set(payload) != PAIR_FIELDS:
                issues.append("invalid harken-runtime-pair.json: schema is not closed")
            else:
                aris = root / "HarkenSearch.exe"
                bootstrap = root / "HarkenSearchBootstrap.exe"
                if aris.is_file() and (
                    payload["arisSize"] != aris.stat().st_size
                    or payload["arisSha256"] != _sha256(aris)
                ):
                    issues.append("runtime-pair Aris identity mismatch")
                if bootstrap.is_file() and (
                    payload["bootstrapSize"] != bootstrap.stat().st_size
                    or payload["bootstrapSha256"] != _sha256(bootstrap)
                ):
                    issues.append("runtime-pair Bootstrap identity mismatch")
                if expected_source_commit is not None and (
                    not re.fullmatch(r"[0-9a-f]{40}", expected_source_commit)
                    or payload["sourceCommit"] != expected_source_commit
                ):
                    issues.append("runtime-pair sourceCommit mismatch")

                aris_sidecar = root / "HarkenSearch.exe.sig"
                if aris.is_file() and aris_sidecar.is_file() and (
                    aris_sidecar.read_bytes() != _integrity_sidecar(aris)
                ):
                    issues.append("integrity sidecar mismatch: HarkenSearch.exe.sig")
                bootstrap_sidecar = root / "HarkenSearchBootstrap.exe.sig"
                if bootstrap.is_file() and bootstrap_sidecar.is_file() and (
                    bootstrap_sidecar.read_bytes()
                    != (_sha256(bootstrap) + "\n").encode("ascii")
                ):
                    issues.append(
                        "integrity sidecar mismatch: HarkenSearchBootstrap.exe.sig"
                    )
    return issues


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail closed unless the clean Release output is complete."
    )
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--source-commit")
    args = parser.parse_args()
    issues = verify_release_assembly(args.output_dir, args.source_commit)
    if issues:
        for issue in issues:
            print(issue)
        return 1
    print(f"release assembly complete: {args.output_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
