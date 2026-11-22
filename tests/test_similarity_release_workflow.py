import hashlib
import json
from pathlib import Path
import zipfile

import tools.prepare_similarity_release_assets as asset_stager
from tools.verify_release_assembly import PINNED_SIMILARITY_SHA256, configured_similarity_files


ROOT = Path(__file__).resolve().parents[1]


def test_asset_staging_and_package_gate_pin_identical_hashes():
    assert {Path(name).name: digest for name, digest in PINNED_SIMILARITY_SHA256.items()} == asset_stager.PINNED


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


def test_official_sdk_source_is_the_versioned_microsoft_nuget_package():
    assert asset_stager.SDK_URL == (
        "https://api.nuget.org/v3-flatcontainer/"
        "microsoft.ml.onnxruntime/1.20.1/"
        "microsoft.ml.onnxruntime.1.20.1.nupkg"
    )
    assert asset_stager.SDK_SHA256 == (
        "9c32108e9a1c78b090a95c2a219f244dd5f067d4caf68fb0f967103f3f20839c"
    )
    assert asset_stager.SDK_SIZE == 108611147


def test_official_similarity_components_are_declared_as_distributed():
    metadata = json.loads((ROOT / "THIRD_PARTY_COMPONENTS.json").read_text(encoding="utf-8"))
    components = {component["name"]: component for component in metadata["components"]}

    assert components["ONNX Runtime"]["distributed"] is True
    assert components["LAION CLAP HTSAT unfused audio encoder"]["distributed"] is True
    assert set(configured_similarity_files(ROOT)) == set(PINNED_SIMILARITY_SHA256)


def test_stage_normalizes_the_official_nuget_sdk_layout(tmp_path, monkeypatch):
    archive = tmp_path / "Microsoft.ML.OnnxRuntime.1.20.1.nupkg"
    dll = b"official win-x64 runtime"
    lib = b"official import library"
    headers = {"onnxruntime_c_api.h": b"official header"}
    with zipfile.ZipFile(archive, "w") as package:
        for name, data in headers.items():
            package.writestr(f"build/native/include/{name}", data)
        package.writestr("runtimes/win-x64/native/onnxruntime.dll", dll)
        package.writestr("runtimes/win-x64/native/onnxruntime.lib", lib)
        package.writestr("runtimes/win-arm64/native/onnxruntime.dll", b"arm64 runtime")
        package.writestr("runtimes/win-x86/native/onnxruntime.dll", b"x86 runtime")
        package.writestr("LICENSE", b"official license")
        package.writestr("ThirdPartyNotices.txt", b"official notices")

    model_source = tmp_path / "models"
    model_source.mkdir()
    models = {
        "clap-audio.onnx": b"audio model",
        "clap-mel-filters.f64": b"mel filters",
        "clap-window.f64": b"window",
    }
    for name, data in models.items():
        (model_source / name).write_bytes(data)

    pinned = {
        "onnxruntime.dll": hashlib.sha256(dll).hexdigest(),
        **{name: hashlib.sha256(data).hexdigest() for name, data in models.items()},
    }
    monkeypatch.setattr(asset_stager, "PINNED", pinned)
    monkeypatch.setattr(
        asset_stager,
        "SDK_SHA256",
        hashlib.sha256(archive.read_bytes()).hexdigest(),
    )
    monkeypatch.setattr(asset_stager, "SDK_SIZE", archive.stat().st_size)
    monkeypatch.setattr(
        asset_stager,
        "SDK_FILE_SHA256",
        {
            "include/onnxruntime_c_api.h": hashlib.sha256(
                headers["onnxruntime_c_api.h"]
            ).hexdigest(),
            "lib/onnxruntime.dll": hashlib.sha256(dll).hexdigest(),
            "lib/onnxruntime.lib": hashlib.sha256(lib).hexdigest(),
            "LICENSE": hashlib.sha256(b"official license").hexdigest(),
            "ThirdPartyNotices.txt": hashlib.sha256(
                b"official notices"
            ).hexdigest(),
        },
    )

    sdk_root, model_dir = asset_stager.stage(
        tmp_path / "staged",
        model_source_dir=model_source,
        sdk_archive=archive,
    )

    assert (sdk_root / "include" / "onnxruntime_c_api.h").read_bytes() == headers[
        "onnxruntime_c_api.h"
    ]
    assert (sdk_root / "lib" / "onnxruntime.dll").read_bytes() == dll
    assert (sdk_root / "lib" / "onnxruntime.lib").read_bytes() == lib
    assert (sdk_root / "LICENSE").read_bytes() == b"official license"
    assert (sdk_root / "ThirdPartyNotices.txt").read_bytes() == b"official notices"
    assert not (sdk_root / "win-arm64").exists()
    assert not (sdk_root / "win-x86").exists()
    assert {path.name for path in model_dir.iterdir()} == set(models)
