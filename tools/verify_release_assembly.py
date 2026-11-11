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
    "concrt140.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll",
    "msvcp140_codecvt_ids.dll",
    "vccorlib140.dll",
    "vcomp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "vcruntime140_threads.dll",
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
PINNED_SIMILARITY_SHA256 = {
    "onnxruntime.dll": "4cb41e89b8bf30578e1dd95e9c40292d61974a4bfcd666409302c4f0c5aa8ce0",
    "models/clap/clap-audio.onnx": "50d03fb781f7bf520a5d424a3479871cfca4af7a31026892ac1e935d5303f677",
    "models/clap/clap-mel-filters.f64": "256b8c586fb0f0ea44c6b2cafa44caa9bf288c1cfe1a05ff2527b73ae123d37a",
    "models/clap/clap-window.f64": "0d9c0e8dfb7e477785eaaac6e2082e19a030bc113178a207cb4d1ab8d536a7a9",
}
REQUIRED_SIMILARITY_LICENSES = (
    "LICENSES/ONNX-Runtime-MIT.txt",
    "LICENSES/ONNX-Runtime-ThirdPartyNotices.txt",
    "LICENSES/CLAP-Apache-2.0.txt",
)

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
    require_similarity: bool = False,
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

    if require_similarity:
        try:
            payload = json.loads((root / "THIRD_PARTY_COMPONENTS.json").read_text(encoding="utf-8"))
            components = {
                component["name"]: component
                for component in payload["components"]
                if isinstance(component, dict) and isinstance(component.get("name"), str)
            }
        except (OSError, UnicodeError, json.JSONDecodeError, KeyError, TypeError) as error:
            issues.append(f"required similarity metadata invalid: {error}")
        else:
            for name in SIMILARITY_COMPONENT_FILES:
                if components.get(name, {}).get("distributed") is not True:
                    issues.append(f"required similarity component not distributed: {name}")
        for relative, expected_hash in PINNED_SIMILARITY_SHA256.items():
            path = root / relative
            if not path.is_file():
                issues.append(f"missing required similarity file: {relative}")
            elif _sha256(path) != expected_hash:
                issues.append(f"similarity resource SHA256 mismatch: {relative}")
        for relative in REQUIRED_SIMILARITY_LICENSES:
            path = root / relative
            if not path.is_file() or path.stat().st_size == 0:
                issues.append(f"missing required similarity license: {relative}")

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
    parser.add_argument("--require-similarity", action="store_true")
    args = parser.parse_args()
    issues = verify_release_assembly(
        args.output_dir, args.source_commit, require_similarity=args.require_similarity
    )
    if issues:
        for issue in issues:
            print(issue)
        return 1
    print(f"release assembly complete: {args.output_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
