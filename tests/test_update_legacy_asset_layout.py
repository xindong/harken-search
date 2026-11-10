"""Keep About assets installable by the published 1.0.42 updater.

That updater validates every destination parent before making any mutation.
The original 42 payload contains assets/icons/brand, but no contributors folder.
"""
from pathlib import Path, PurePosixPath
import re

ROOT = Path(__file__).resolve().parents[1]


def test_cindy_asset_uses_a_directory_present_in_the_original_42_payload():
    main = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
    references = re.findall(r'LoadPackagedAsset[(]"([^"]*cindy[.]png)"[)]', main)
    assert len(references) == 1
    asset = PurePosixPath(references[0])
    # Published 42 ZIP sha256:
    # 695c620d2419d32f6264dfae6899c5a71a60600b9fb01f6ae2fd81d7ec7a95ce
    assert asset.parent == PurePosixPath('assets/icons/brand'), (
        f'{asset.parent} is absent in original 42; its updater rejects the package before replacement'
    )
    assert (ROOT / asset).read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
    cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
    assert '"$<TARGET_FILE_DIR:HarkenSearch>/assets/icons/brand"' in cmake
    assert '"$<TARGET_FILE_DIR:HarkenSearch>/assets/icons/contributors"' not in cmake


def test_every_packaged_asset_parent_exists_in_the_original_42_payload():
    # Exact file-parent inventory from the published ZIP identified above.
    original_parents = {
        PurePosixPath("assets/avatars/dicebear/lorelei-neutral"),
        PurePosixPath("assets/icons/brand"),
        PurePosixPath("assets/logos/brand"),
    }
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    copies = re.findall(
        r'copy_directory\s+"\$\{CMAKE_CURRENT_SOURCE_DIR\}/(assets/[^"\n]+)"'
        r'\s+"\$<TARGET_FILE_DIR:HarkenSearch>/(assets/[^"\n]+)"',
        cmake,
    )
    assert copies, "No packaged asset mappings were inspected"
    incompatible = []
    for source, destination in copies:
        for file in (ROOT / source).rglob("*"):
            if file.is_file():
                packaged = PurePosixPath(destination) / file.relative_to(ROOT / source).as_posix()
                if packaged.parent not in original_parents:
                    incompatible.append(str(packaged))
    assert not incompatible, (
        "Published 42 updater cannot create these asset parents: " + ", ".join(incompatible)
    )
