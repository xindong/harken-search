from __future__ import annotations

"""
pack_engine.py — Core packaging logic for Aris distribution ZIPs.

Reads pack_manifest.json, generates harken_features.json with an HMAC-SHA256
signature, copies files, and creates a signed ZIP archive.

Usage (CLI):
    python pack_engine.py --release-dir build_user/Release \
                          --output-dir dist \
                          --edition pro \
                          --version 2.3.0 \
                          [--preset internal_test | all | none]
"""

import argparse
import hashlib
import hmac
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

try:
    from .build_offline_manual import RELATIVE_PDF, stage_manual
except ImportError:  # 兼容直接运行 tools/pack_engine.py
    from build_offline_manual import RELATIVE_PDF, stage_manual

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

EDITION_MAP = {"base": 0, "pro": 1, "ultimate": 2}

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = Path(__file__).parent / "pack_manifest.json"
SECRET_PATH = Path(__file__).parent / ".pack_secret"
VERSION_HEADER_PATH = REPO_ROOT / "src" / "Version.h"
RELEASE_HISTORY_JSON_PATH = REPO_ROOT / "installer" / "harken_release_history.json"
RELEASE_HISTORY_MD_PATH = REPO_ROOT / "installer" / "RELEASE_HISTORY.md"
INSTALLER_TEMPLATE_PATH = REPO_ROOT / "installer" / "HarkenSearch_Setup.iss"
INSTALLER_OUTPUT_DIR = REPO_ROOT / "installer" / "output"
GENERATED_INSTALLER_PATH = REPO_ROOT / "installer" / "HarkenSearch_Setup_generated.iss"
RELEASE_NOTES_FILENAME = "aris_release_notes.json"

# Files that are always included regardless of feature selection
ALWAYS_INCLUDE = ["HarkenSearch.exe", "HarkenSearch.exe.sig", "HarkenSearchBootstrap.exe", "reaper_harken.dll"]
RUNTIME_INCLUDE = ["lucide.ttf", "splash_beta.png", "harken_thesaurus.json"]
# Select the current runtime trees explicitly. An incremental build may still
# contain obsolete contributors/community copies beside these destinations.
RUNTIME_DIRS = ["assets/icons/brand", "assets/logos/brand", "assets/avatars"]

# Optional directories bundled when AI features are enabled
AI_FEATURE_IDS = {"ai_engine", "ai_tags"}
MODELS_DIR_NAME = "models"
MANIFEST_ID_RE = re.compile(r"^[a-z][a-z0-9_]*$")
VERSION_STRING_RE = re.compile(r'SOUNDMANAGER_VERSION_STRING\s+"([^"]+)"')


# ---------------------------------------------------------------------------
# Manifest + Secret helpers
# ---------------------------------------------------------------------------

def load_manifest(manifest_path: Path = MANIFEST_PATH) -> dict:
    """Load and return the parsed pack_manifest.json."""
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    validate_manifest(manifest, manifest_path)
    return manifest


def _duplicates(values: list[str]) -> list[str]:
    seen: set[str] = set()
    repeated: set[str] = set()
    for value in values:
        if value in seen:
            repeated.add(value)
        seen.add(value)
    return sorted(repeated)


def validate_manifest(manifest: dict, source: str | Path = "pack_manifest.json") -> None:
    """Validate the feature manifest contract shared by packer UI and CLI."""
    errors: list[str] = []
    if not isinstance(manifest, dict):
        raise ValueError(f"{source}: manifest must be a JSON object")

    if manifest.get("version") != 1:
        errors.append("version must be 1")

    groups = manifest.get("groups")
    if not isinstance(groups, list) or not groups:
        errors.append("groups must be a non-empty list")
        groups = []

    group_ids: list[str] = []
    group_orders: list[int] = []
    for index, group in enumerate(groups):
        if not isinstance(group, dict):
            errors.append(f"groups[{index}] must be an object")
            continue
        group_id = group.get("id")
        if not isinstance(group_id, str) or not MANIFEST_ID_RE.fullmatch(group_id):
            errors.append(f"groups[{index}].id must match {MANIFEST_ID_RE.pattern}")
        else:
            group_ids.append(group_id)
        if not isinstance(group.get("name"), str) or not group["name"].strip():
            errors.append(f"groups[{index}].name must be non-empty")
        order = group.get("order")
        if type(order) is not int:
            errors.append(f"groups[{index}].order must be an integer")
        else:
            group_orders.append(order)

    for group_id in _duplicates(group_ids):
        errors.append(f"duplicate group id: {group_id}")
    if group_orders != sorted(group_orders):
        errors.append("groups must be sorted by order")

    features = manifest.get("features")
    if not isinstance(features, list) or not features:
        errors.append("features must be a non-empty list")
        features = []

    group_set = set(group_ids)
    feature_ids: list[str] = []
    names_en: list[str] = []
    for index, feature in enumerate(features):
        if not isinstance(feature, dict):
            errors.append(f"features[{index}] must be an object")
            continue
        feature_id = feature.get("id")
        feature_label = feature_id if isinstance(feature_id, str) else f"features[{index}]"
        if not isinstance(feature_id, str) or not MANIFEST_ID_RE.fullmatch(feature_id):
            errors.append(f"{feature_label}.id must match {MANIFEST_ID_RE.pattern}")
        else:
            feature_ids.append(feature_id)
        if feature.get("group") not in group_set:
            errors.append(f"{feature_label}.group must reference a manifest group")
        if feature.get("required_edition", "base") not in EDITION_MAP:
            errors.append(f"{feature_label}.required_edition must be one of {list(EDITION_MAP)}")
        for field, default in (("default_enabled", True), ("internal_test_default", False)):
            if type(feature.get(field, default)) is not bool:
                errors.append(f"{feature_label}.{field} must be a boolean")
        for field in ("name", "name_en"):
            if not isinstance(feature.get(field), str) or not feature[field].strip():
                errors.append(f"{feature_label}.{field} must be non-empty")
        if isinstance(feature.get("name_en"), str):
            names_en.append(feature["name_en"])
        dlls = feature.get("dlls", [])
        if not isinstance(dlls, list):
            errors.append(f"{feature_label}.dlls must be a list")
            dlls = []
        for dll in dlls:
            if not isinstance(dll, str) or not dll:
                errors.append(f"{feature_label}.dlls entries must be non-empty strings")
            elif "/" in dll or "\\" in dll or not dll.lower().endswith(".dll"):
                errors.append(f"{feature_label}.DLL entry must be a file-local .dll basename: {dll}")

    for feature_id in _duplicates(feature_ids):
        errors.append(f"duplicate feature id: {feature_id}")
    for name_en in _duplicates(names_en):
        errors.append(f"duplicate feature name_en: {name_en}")

    missing_ai_features = sorted(AI_FEATURE_IDS - set(feature_ids))
    if missing_ai_features:
        errors.append(f"missing AI feature ids: {missing_ai_features}")

    if errors:
        visible_errors = errors[:12]
        suffix = "" if len(errors) <= len(visible_errors) else f"\n- ... {len(errors) - len(visible_errors)} more"
        raise ValueError(f"{source}: invalid manifest\n- " + "\n- ".join(visible_errors) + suffix)


def load_secret(secret_path: Path = SECRET_PATH) -> bytes:
    """
    Load the 32-byte HMAC secret from .pack_secret.
    Raises FileNotFoundError with a helpful message if missing.
    """
    if not secret_path.exists():
        raise FileNotFoundError(
            f"Secret key not found: {secret_path}\n"
            f"Generate it with:\n"
            f"  python -c \"import os; open('tools/.pack_secret','wb').write(os.urandom(32))\""
        )
    data = secret_path.read_bytes()
    if len(data) != 32:
        raise ValueError(
            f"Secret key must be exactly 32 bytes, got {len(data)} bytes: {secret_path}"
        )
    return data


def read_current_version(version_header_path: Path = VERSION_HEADER_PATH) -> str:
    """Read SOUNDMANAGER_VERSION_STRING from src/Version.h."""
    text = version_header_path.read_text(encoding="utf-8")
    match = VERSION_STRING_RE.search(text)
    if not match:
        raise ValueError(f"Could not find SOUNDMANAGER_VERSION_STRING in {version_header_path}")
    return match.group(1)


def suggest_next_patch_version(version: str) -> str:
    """Increment x.y.z -> x.y.(z+1)."""
    parts = version.strip().split(".")
    if len(parts) != 3 or not all(part.isdigit() for part in parts):
        raise ValueError(f"Expected semantic version x.y.z, got: {version!r}")
    major, minor, patch = (int(part) for part in parts)
    return f"{major}.{minor}.{patch + 1}"


def load_release_history(history_path: Path = RELEASE_HISTORY_JSON_PATH) -> dict:
    """Load canonical release history or return an empty default."""
    if not history_path.exists():
        return {"schema_version": 1, "releases": []}
    with open(history_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(f"{history_path} must contain a JSON object")
    schema_version = int(data.get("schema_version", 1))
    releases = data.get("releases", [])
    if not isinstance(releases, list):
        raise ValueError(f"{history_path} must contain a releases array")
    return {"schema_version": schema_version, "releases": releases}


def serialize_release_history(history: dict) -> str:
    """Serialize canonical release history as UTF-8 JSON."""
    return json.dumps(history, ensure_ascii=False, indent=2) + "\n"


def render_release_history_markdown(history: dict) -> str:
    """Render a human-readable Markdown mirror from release history JSON."""
    lines = [
        "# Aris Release History",
        "",
        f"- Generated at: {datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}",
        "",
    ]
    releases = history.get("releases", [])
    if not releases:
        lines.extend(["_No releases yet._", ""])
        return "\n".join(lines)

    category_titles = {
        "add": "新增内容",
        "improve": "体验优化",
        "fix": "问题修复",
    }
    for release in releases:
        version = str(release.get("version", "")).strip() or "unknown"
        release_date = str(release.get("release_date", "")).strip() or "unknown-date"
        headline = str(release.get("headline", "")).strip()
        lines.append(f"## {version} · {release_date}")
        lines.append("")
        if headline:
            lines.append(f"> {headline}")
            lines.append("")

        items = release.get("items", [])
        grouped: dict[str, list[str]] = {"add": [], "improve": [], "fix": []}
        for item in items:
            if not isinstance(item, dict):
                continue
            text = str(item.get("text", "")).strip()
            category = str(item.get("category", "fix")).strip()
            if text and category in grouped:
                grouped[category].append(text)

        for category in ("add", "improve", "fix"):
            if not grouped[category]:
                continue
            lines.append(f"### {category_titles[category]}")
            lines.append("")
            for text in grouped[category]:
                lines.append(f"- {text}")
            lines.append("")

        notes = release.get("notes", [])
        if notes:
            lines.append("### 使用提醒")
            lines.append("")
            for note in notes:
                note_text = str(note).strip()
                if note_text:
                    lines.append(f"- {note_text}")
            lines.append("")

        source_refs = release.get("source_refs", [])
        if source_refs:
            lines.append("### Source Refs")
            lines.append("")
            for ref in source_refs:
                if not isinstance(ref, dict):
                    continue
                kind = str(ref.get("kind", "")).strip() or "source"
                label = str(ref.get("label", "")).strip()
                if label:
                    lines.append(f"- `{kind}`: {label}")
            lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def write_release_history_files(
    history: dict,
    history_json_path: Path = RELEASE_HISTORY_JSON_PATH,
    history_md_path: Path = RELEASE_HISTORY_MD_PATH,
) -> tuple[Path, Path]:
    """Persist canonical JSON and derived Markdown together."""
    history_json_path.write_text(serialize_release_history(history), encoding="utf-8")
    history_md_path.write_text(render_release_history_markdown(history), encoding="utf-8")
    return history_json_path, history_md_path


def write_release_notes_file(release_dir: str | Path, history: dict) -> Path:
    """Write the client-facing release notes JSON into the release directory."""
    release_dir = Path(release_dir)
    release_dir.mkdir(parents=True, exist_ok=True)
    notes_path = release_dir / RELEASE_NOTES_FILENAME
    notes_path.write_text(serialize_release_history(history), encoding="utf-8")
    return notes_path


def _has_release_detail(record: dict) -> bool:
    items = record.get("items")
    if isinstance(items, list) and any(
        isinstance(item, dict) and isinstance(item.get("text"), str)
        and item["text"].strip() for item in items
    ):
        return True
    sections = record.get("sections")
    if not isinstance(sections, list):
        return False
    for section in sections:
        if not isinstance(section, dict) or not isinstance(section.get("items"), list):
            continue
        for group in section["items"]:
            if not isinstance(group, dict) or not isinstance(group.get("list"), list):
                continue
            for item in group["list"]:
                if isinstance(item, str) and item.strip():
                    return True
                if (isinstance(item, dict) and
                    isinstance(item.get("description"), str) and
                    item["description"].strip()):
                    return True
    return False


def prepare_release_notes(
    version: str,
    release_dir: str | Path,
    history_path: str | Path = RELEASE_HISTORY_JSON_PATH,
) -> Path:
    """Fail closed unless this version has publishable notes and contributors."""
    history_path = Path(history_path)
    if not history_path.is_file():
        raise ValueError(f"release history is missing: {history_path}")
    history = load_release_history(history_path)
    if history.get("schema_version") not in (1, 2, 3):
        raise ValueError("release history schema is unsupported")
    releases = history["releases"]
    if not all(isinstance(item, dict) for item in releases):
        raise ValueError("release history contains an invalid record")
    versions = [item.get("version") for item in releases]
    if not all(isinstance(item, str) and item.strip() for item in versions):
        raise ValueError("release history contains an invalid version")
    if len(versions) != len(set(versions)):
        raise ValueError("release history contains duplicate versions")
    current = [item for item in releases if isinstance(item, dict) and item.get("version") == version]
    if len(current) != 1:
        raise ValueError(f"release history must contain exactly one record for {version}")
    record = current[0]
    for field in ("release_date", "headline"):
        if not isinstance(record.get(field), str) or not record[field].strip():
            raise ValueError(f"release {version} is missing {field}")
    try:
        parsed_date = datetime.strptime(record["release_date"], "%Y-%m-%d")
    except ValueError as error:
        raise ValueError(f"release {version} has an invalid release_date") from error
    if parsed_date.strftime("%Y-%m-%d") != record["release_date"]:
        raise ValueError(f"release {version} has an invalid release_date")
    contributors = record.get("contributors")
    if not isinstance(contributors, list) or not contributors or any(
        not isinstance(name, str) or not name.strip() for name in contributors
    ):
        raise ValueError(f"release {version} is missing contributors")
    if not _has_release_detail(record):
        raise ValueError(f"release {version} is missing detailed update notes")
    return write_release_notes_file(release_dir, history)


def find_inno_setup_compiler() -> Path:
    """Locate Inno Setup's ISCC.exe."""
    candidates = [
        Path(r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"),
        Path(r"C:\Program Files\Inno Setup 6\ISCC.exe"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        "Inno Setup Compiler not found. Please install Inno Setup 6 to build the installer."
    )


def build_installer_script(
    version: str,
    source_dir: str | Path,
    template_path: Path = INSTALLER_TEMPLATE_PATH,
    generated_path: Path = GENERATED_INSTALLER_PATH,
) -> Path:
    """Generate an installer script with the publish version and release dir."""
    template = template_path.read_text(encoding="utf-8")
    source_dir_str = str(Path(source_dir).resolve())
    script = re.sub(
        r'^#define MyAppVersion ".*"$',
        lambda _: f'#define MyAppVersion "{version}"',
        template,
        flags=re.MULTILINE,
    )
    script = re.sub(
        r'^#define SourceDir ".*"$',
        lambda _: f'#define SourceDir "{source_dir_str}"',
        script,
        flags=re.MULTILINE,
    )
    release_notes_source = (
        f'Source: "{{#SourceDir}}\\{RELEASE_NOTES_FILENAME}"; DestDir: "{{app}}"; '
        "Flags: ignoreversion skipifsourcedoesntexist"
    )
    if RELEASE_NOTES_FILENAME not in script:
        script = script.replace(
            'Source: "{#SourceDir}\\harken_features.sig"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist',
            'Source: "{#SourceDir}\\harken_features.sig"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist\n'
            + release_notes_source,
        )
    generated_path.write_text(script, encoding="utf-8")
    return generated_path


def compile_installer(
    version: str,
    source_dir: str | Path,
    template_path: Path = INSTALLER_TEMPLATE_PATH,
    generated_path: Path = GENERATED_INSTALLER_PATH,
    sign_tool_path: str | Path | None = None,
    trusted_signer_subject: str | None = None,
) -> Path:
    """Generate and compile an installer script for the given release directory.

    Both signing arguments must be given together. They enable the installer's
    legacy-uninstaller identity check; omitting both keeps the fail-closed path.
    """
    from tools.verify_release_assembly import verify_release_assembly

    notes = Path(source_dir) / RELEASE_NOTES_FILENAME
    if not notes.is_file():
        raise FileNotFoundError(f"required release notes are missing: {notes}")
    prepare_release_notes(version, source_dir, notes)
    if (sign_tool_path is None) != (trusted_signer_subject is None):
        raise ValueError("sign_tool_path and trusted_signer_subject must be set together")
    if trusted_signer_subject is not None and (
        not trusted_signer_subject.strip()
        or any(character in trusted_signer_subject for character in "'\"\r\n")
    ):
        raise ValueError("trusted signer subject contains characters that break the installer define")
    issues = verify_release_assembly(source_dir)
    for filename in ("HarkenSearchProToolsBridge.exe", "reaper_harken.dll"):
        if not (Path(source_dir) / filename).is_file():
            issues.append(f"missing required installer file: {filename}")
    if sign_tool_path is not None and not Path(sign_tool_path).is_file():
        issues.append(f"missing signature verifier: {sign_tool_path}")
    if issues:
        raise FileNotFoundError("\n".join(issues))
    stage_manual(source_dir)
    compiler_path = find_inno_setup_compiler()
    script_path = build_installer_script(version, source_dir, template_path, generated_path)
    command = [str(compiler_path)]
    if sign_tool_path is not None:
        command.append(f"/DSignToolPath={Path(sign_tool_path).resolve()}")
        command.append(f"/DTrustedSignerSubject={trusted_signer_subject}")
    command.append(str(script_path))
    result = subprocess.run(
        command,
        cwd=str(template_path.parent),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        details = (result.stdout + "\n" + result.stderr).strip()
        raise RuntimeError(
            f"Inno Setup compilation failed for {script_path}.\n{details}"
        )
    return INSTALLER_OUTPUT_DIR / f"HarkenSearch_Setup_v{version}.exe"


# ---------------------------------------------------------------------------
# Feature selection helpers
# ---------------------------------------------------------------------------

def get_edition_ceiling(manifest: dict, edition: str) -> list[str]:
    """
    Return all feature IDs whose required_edition is <= the given edition level.
    """
    edition_level = EDITION_MAP.get(edition)
    if edition_level is None:
        raise ValueError(f"Unknown edition: {edition!r}. Must be one of: {list(EDITION_MAP)}")
    return [
        f["id"]
        for f in manifest["features"]
        if EDITION_MAP.get(f.get("required_edition", "base"), 0) <= edition_level
    ]


def get_internal_test_defaults(manifest: dict) -> list[str]:
    """Return feature IDs that have internal_test_default=true."""
    return [f["id"] for f in manifest["features"] if f.get("internal_test_default", False)]


def get_default_enabled(manifest: dict) -> list[str]:
    """Return feature IDs that have default_enabled=true."""
    return [f["id"] for f in manifest["features"] if f.get("default_enabled", True)]


# ---------------------------------------------------------------------------
# JSON + signature generation
# ---------------------------------------------------------------------------

def generate_features_json(enabled_ids: list[str], edition: str, version: str) -> str:
    """
    Build the canonical harken_features.json string.

    The payload is deterministic: sort_keys=True, no spaces (separators=(',',':')).
    This ensures the signature is stable across platforms.
    """
    edition_level = EDITION_MAP.get(edition)
    if edition_level is None:
        raise ValueError(f"Unknown edition: {edition!r}")

    payload = {
        "edition": edition,
        "edition_level": edition_level,
        "enabled_features": sorted(enabled_ids),
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "version": version,
    }
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def sign_json(json_bytes: bytes, secret: bytes) -> bytes:
    """
    Compute HMAC-SHA256 over json_bytes using secret.
    Returns the raw 32-byte digest.
    """
    return hmac.new(secret, json_bytes, hashlib.sha256).digest()


# ---------------------------------------------------------------------------
# DLL collection
# ---------------------------------------------------------------------------

def collect_required_dlls(manifest: dict, enabled_ids: list[str]) -> list[str]:
    """
    Return the list of DLL filenames required by the enabled feature set.
    Deduplicates while preserving order of first appearance.
    """
    seen: set[str] = set()
    result: list[str] = []
    enabled_set = set(enabled_ids)
    for feature in manifest["features"]:
        if feature["id"] in enabled_set:
            for dll in feature.get("dlls", []):
                if dll not in seen:
                    seen.add(dll)
                    result.append(dll)
    return result


# ---------------------------------------------------------------------------
# ZIP packaging
# ---------------------------------------------------------------------------

def pack(
    release_dir: str | Path,
    output_dir: str | Path,
    edition: str,
    enabled_ids: list[str],
    version: str,
    secret: bytes | None = None,
    manifest: dict | None = None,
    extra_files: list[str] | None = None,
) -> Path:
    """
    Create a signed Aris distribution ZIP.

    Parameters
    ----------
    release_dir : path to the built release directory (contains HarkenSearch.exe etc.)
    output_dir  : directory where the ZIP will be written
    edition     : "base" | "pro" | "ultimate"
    enabled_ids : list of feature IDs to include
    version     : version string, e.g. "2.3.0"
    secret      : 32-byte HMAC secret (loaded from .pack_secret if None)
    manifest    : parsed manifest dict (loaded from pack_manifest.json if None)

    Returns
    -------
    Path to the created ZIP file.
    """
    release_dir = Path(release_dir)
    output_dir = Path(output_dir)

    notes = release_dir / RELEASE_NOTES_FILENAME
    if not notes.is_file():
        raise FileNotFoundError(f"required release notes are missing: {notes}")
    prepare_release_notes(version, release_dir, notes)
    output_dir.mkdir(parents=True, exist_ok=True)

    if secret is None:
        secret = load_secret()
    if manifest is None:
        manifest = load_manifest()
    if extra_files is None:
        extra_files = []

    # --- Validate edition ---
    if edition not in EDITION_MAP:
        raise ValueError(f"Unknown edition: {edition!r}. Must be one of: {list(EDITION_MAP)}")

    # --- Filter enabled_ids to edition ceiling ---
    ceiling = set(get_edition_ceiling(manifest, edition))
    filtered_ids = [fid for fid in enabled_ids if fid in ceiling]

    # --- Generate features JSON + signature ---
    features_json_str = generate_features_json(filtered_ids, edition, version)
    features_json_bytes = features_json_str.encode("utf-8")
    features_sig = sign_json(features_json_bytes, secret)

    # --- Collect required DLLs ---
    required_dlls = collect_required_dlls(manifest, filtered_ids)

    # --- Determine ZIP name ---
    date_str = datetime.now(timezone.utc).strftime("%Y%m%d")
    edition_label = edition.capitalize()
    zip_name = f"HarkenSearch_v{version}_{edition_label}_{date_str}.zip"
    zip_path = output_dir / zip_name

    # --- Build ZIP ---
    # A package without any fixed payload is unusable. Validate the complete
    # invariant before opening the archive so a failed build cannot leave a
    # plausible-looking partial ZIP behind.
    missing_always = [
        filename for filename in ALWAYS_INCLUDE
        if not (release_dir / filename).is_file()
    ]
    if missing_always:
        raise FileNotFoundError(
            f"Required package file(s) not found in {release_dir}: {missing_always}.\n"
            f"Please build Release first:\n"
            f"  cmake --build build_user --config Release -- //m"
        )

    manual = stage_manual(release_dir)
    missing_files: list[str] = []
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        written_arcnames: set[str] = set()

        def write_file_once(src: Path, arcname: str | Path) -> None:
            archive_name = str(arcname).replace("\\", "/")
            if archive_name in written_arcnames:
                return
            zf.write(src, arcname=archive_name)
            written_arcnames.add(archive_name)

        # Always-included binaries
        for filename in ALWAYS_INCLUDE:
            src = release_dir / filename
            write_file_once(src, filename)
        write_file_once(notes, RELEASE_NOTES_FILENAME)
        write_file_once(manual, RELATIVE_PDF)

        # Runtime assets copied by the CMake target. Missing these produces a
        # package that starts without the intended icon font/branding resources.
        for filename in RUNTIME_INCLUDE:
            src = release_dir / filename
            if src.exists():
                write_file_once(src, filename)
            else:
                missing_files.append(filename)

        for dirname in RUNTIME_DIRS:
            src_dir = release_dir / dirname
            if not src_dir.exists() or not src_dir.is_dir():
                missing_files.append(dirname + "/")
                continue
            for asset_file in sorted(src_dir.rglob("*")):
                if asset_file.is_file():
                    arcname = asset_file.relative_to(release_dir)
                    write_file_once(asset_file, arcname)

        # Write harken_features.json
        zf.writestr("harken_features.json", features_json_str)

        # Write harken_features.sig (raw 32 bytes — C++ reads exactly 32 bytes)
        zf.writestr("harken_features.sig", features_sig)

        # Required DLLs
        for dll in required_dlls:
            src = release_dir / dll
            if src.exists():
                write_file_once(src, dll)
            else:
                missing_files.append(dll)

        # models/ directory — include if any AI feature is enabled
        if any(fid in set(filtered_ids) for fid in AI_FEATURE_IDS):
            models_src = release_dir / MODELS_DIR_NAME
            if models_src.exists() and models_src.is_dir():
                for model_file in sorted(models_src.rglob("*")):
                    if model_file.is_file():
                        arcname = model_file.relative_to(release_dir)
                        write_file_once(model_file, arcname)

        for filename in extra_files:
            src = release_dir / filename
            if src.exists():
                write_file_once(src, filename)
            else:
                missing_files.append(filename)

    if missing_files:
        print(
            f"[pack_engine] WARNING: {len(missing_files)} file(s) not found in release dir "
            f"and were skipped: {missing_files}",
            file=sys.stderr,
        )

    # Also write features files to release_dir so Inno Setup can pick them up
    (release_dir / "harken_features.json").write_bytes(features_json_bytes)
    (release_dir / "harken_features.sig").write_bytes(features_sig)

    print(f"[pack_engine] Created: {zip_path}")
    print(f"[pack_engine] Edition: {edition}  Version: {version}")
    print(f"[pack_engine] Enabled features ({len(filtered_ids)}): {sorted(filtered_ids)}")
    print(f"[pack_engine] Required DLLs: {required_dlls}")
    print(f"[pack_engine] Features config also written to: {release_dir}")

    return zip_path


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Aris distribution packager — generates signed ZIP with feature whitelist.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Presets:
  internal_test  — enable features with internal_test_default=true
  all            — enable all features available for the selected edition
  none           — disable all optional features

Examples:
  python pack_engine.py --release-dir build_user/Release --output-dir dist \\
      --edition pro --version 2.3.0 --preset internal_test

  python pack_engine.py --release-dir build_user/Release --output-dir dist \\
      --edition ultimate --version 2.3.0 --preset all
        """,
    )
    parser.add_argument(
        "--release-dir",
        required=True,
        help="Path to the built release directory containing HarkenSearch.exe.",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Directory where the output ZIP will be written.",
    )
    parser.add_argument(
        "--edition",
        required=True,
        choices=list(EDITION_MAP),
        help="Target edition: base | pro | ultimate",
    )
    parser.add_argument(
        "--version",
        required=True,
        help="Version string, e.g. 2.3.0",
    )
    parser.add_argument(
        "--preset",
        choices=["internal_test", "all", "none"],
        default="all",
        help="Feature preset to use (default: all)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    if argv is None:
        argv = sys.argv[1:]
    if "--prepare-release-notes" in argv:
        prepare_parser = argparse.ArgumentParser(description="核对并复制当前版本更新说明")
        prepare_parser.add_argument("--prepare-release-notes", action="store_true")
        prepare_parser.add_argument("--version", required=True)
        prepare_parser.add_argument("--release-dir", type=Path, required=True)
        prepare_parser.add_argument("--history", type=Path, default=RELEASE_HISTORY_JSON_PATH)
        args = prepare_parser.parse_args(argv)
        try:
            notes = prepare_release_notes(args.version, args.release_dir, args.history)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            print(f"[pack_engine] ERROR: {error}", file=sys.stderr)
            return 1
        print(notes)
        return 0
    parser = _build_arg_parser()
    args = parser.parse_args(argv)

    try:
        manifest = load_manifest()
        secret = load_secret()
    except (FileNotFoundError, ValueError) as e:
        print(f"[pack_engine] ERROR: {e}", file=sys.stderr)
        return 1

    # Resolve feature list from preset
    if args.preset == "internal_test":
        enabled_ids = get_internal_test_defaults(manifest)
    elif args.preset == "none":
        enabled_ids = []
    else:  # "all"
        enabled_ids = get_edition_ceiling(manifest, args.edition)

    try:
        zip_path = pack(
            release_dir=args.release_dir,
            output_dir=args.output_dir,
            edition=args.edition,
            enabled_ids=enabled_ids,
            version=args.version,
            secret=secret,
            manifest=manifest,
        )
        print(f"[pack_engine] Done: {zip_path}")
        return 0
    except Exception as e:
        print(f"[pack_engine] FATAL: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
