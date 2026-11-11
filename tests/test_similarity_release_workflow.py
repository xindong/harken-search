import json
from pathlib import Path

from tools.prepare_similarity_release_assets import PINNED
from tools.verify_release_assembly import PINNED_SIMILARITY_SHA256, configured_similarity_files


ROOT = Path(__file__).resolve().parents[1]


def test_asset_staging_and_package_gate_pin_identical_hashes():
    assert {Path(name).name: digest for name, digest in PINNED_SIMILARITY_SHA256.items()} == PINNED


def test_official_package_fetches_and_requires_pinned_similarity_bundle():
    workflow = (ROOT / ".github/workflows/package-update.yml").read_text(encoding="utf-8")
    for required in (
        "HARKEN_SIMILARITY_MODEL_BASE_URL",
        "prepare_similarity_release_assets.py",
        "-DHARKEN_ONNXRUNTIME_ROOT=",
        "-DHARKEN_SIMILARITY_MODEL_DIR=",
        "--require-similarity",
    ):
        assert required in workflow


def test_official_similarity_components_are_declared_as_distributed():
    metadata = json.loads((ROOT / "THIRD_PARTY_COMPONENTS.json").read_text(encoding="utf-8"))
    components = {component["name"]: component for component in metadata["components"]}

    assert components["ONNX Runtime"]["distributed"] is True
    assert components["LAION CLAP HTSAT unfused audio encoder"]["distributed"] is True
    assert set(configured_similarity_files(ROOT)) == set(PINNED_SIMILARITY_SHA256)
