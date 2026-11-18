"""UI cover discovery must stay memory/queue-only, including cache hits."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 1
    for pos in range(start + 1, len(source)):
        depth += (source[pos] == "{") - (source[pos] == "}")
        if depth == 0:
            return source[start + 1:pos]
    raise AssertionError(signature)


def test_ui_cover_requests_do_not_probe_files_or_compute_identity():
    source = (ROOT / "src/CoverModule.cpp").read_text(encoding="utf-8")
    for signature in ("bool LoadCurrentFileCoverTexture(",
                      "bool GetEmbeddedCoverTextureByPath(",
                      "static bool RequestEmbeddedCoverIdentity("):
        region = body(source, signature)
        for blocking in ("ComputeEmbeddedCoverKeyForFile", "readAlbumArt",
                         "TryGetPathMtime", "ReadFileBytes", "saveCoverThumb",
                         "DownscaleForDbStorage", ".join(", ".wait("):
            assert blocking not in region, (signature, blocking)
    assert "task.discoverIdentity" in body(source, "static void AsyncLoaderThread()")
    assert "g_identityRefresh.invalidate(fp)" in body(source, "void InvalidateFileCoverCaches(")
    assert "g_identityRefresh.clear()" in body(source, "void ClearAllCoverCaches()")


def test_probe_does_not_hold_metadata_lock_during_disk_io():
    source = (ROOT / "src/CoverModule.cpp").read_text(encoding="utf-8")
    region = body(source, "static bool ComputeEmbeddedCoverKeyForFile(")
    # Lock scopes may copy/publish memory and enqueue metadata, but cannot stat,
    # parse audio, decode images or synchronously write the database.
    scopes = []
    for line in region.splitlines():
        for char in line:
            if char == "{": scopes.append(False)
            elif char == "}" and scopes: scopes.pop()
        if "lock_guard" in line and scopes: scopes[-1] = True
        if any(scopes):
            assert not any(x in line for x in (
                "TryGetPathMtime", "readAlbumArt", "ReadFileBytes",
                "DownscaleForDbStorage", "saveCoverThumb")), line


if __name__ == "__main__":
    test_ui_cover_requests_do_not_probe_files_or_compute_identity()
    test_probe_does_not_hold_metadata_lock_during_disk_io()
    print("Cover identity UI boundary checks passed")
