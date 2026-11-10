# Similarity assets in official packages

The official Windows workflow requires HARKEN_SIMILARITY_MODEL_BASE_URL, a public
HTTPS directory containing clap-audio.onnx, clap-mel-filters.f64, and
clap-window.f64. These are the pinned exported files described in
docs/similarity-model-build.md. Do not point this variable at a mutable model
revision or a private workstation path. Until a versioned directory is hosted
and the repository variable is configured, official packaging deliberately fails.

tools/prepare_similarity_release_assets.py downloads the fixed ONNX Runtime
1.20.1 Windows SDK from the Microsoft GitHub release and checks its ZIP SHA256,
DLL SHA256, headers, import library, license, and notices. It then downloads
the three model files and checks each pinned SHA256. For local QA, use
--model-source-dir and --sdk-archive with already verified copies.

The workflow passes the staged paths to CMake. Its release assembly check and
installer compilation both require the ONNX Runtime and CLAP metadata, exact
file hashes, and shipped license files. Update these pins only with a reviewed
model/runtime revision and new native inference evidence.
