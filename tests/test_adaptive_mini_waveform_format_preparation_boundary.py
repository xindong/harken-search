from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/AdaptiveMiniWaveformFormatPreparation.h"
SOURCE_PATH = ROOT / "src/AdaptiveMiniWaveformFormatPreparation.cpp"

assert HEADER_PATH.is_file(), f"missing header: {HEADER_PATH}"
assert SOURCE_PATH.is_file(), f"missing source: {SOURCE_PATH}"

HEADER = HEADER_PATH.read_text(encoding="utf-8")
SOURCE = SOURCE_PATH.read_text(encoding="utf-8")
MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


for token in (
    "enum class AdaptiveMiniFormatPreparationDecision",
    "struct AdaptiveMiniFormatPreparationResult",
    "class AdaptiveMiniFormatPreparationSource",
    "class DatabaseAdaptiveMiniFormatPreparationSource final",
    "AdaptiveMiniFormatPreparationResult PrepareAdaptiveMiniWaveformFormat(",
):
    assert token in HEADER, token

for owned in (
    "source.usesAdaptiveMiniWaveform()",
    "source.countFilesNeedingWaveformCache()",
    "database_.usesAdaptiveMiniWaveform()",
    "database_.countFilesNeedingWaveformCache()",
):
    assert owned in SOURCE, owned

body = function_body(
    MAIN, "static void MaybeStartAdaptiveMiniWaveformFormatUpgrade(AppState& state)"
)
assert MAIN.count("PrepareAdaptiveMiniWaveformFormat(") == 1
for removed in (
    "state.database.usesAdaptiveMiniWaveform()",
    "state.database.countFilesNeedingWaveformCache()",
):
    assert removed not in body, removed
for removed in ("purgeAllMiniWaveform", "purged"):
    assert removed not in HEADER, removed
    assert removed not in SOURCE, removed
    assert removed not in body, removed
assert "AdaptiveMiniFormatPreparationDecision::Disabled" in body
assert "AdaptiveMiniFormatPreparationDecision::NoWork" in body
diag_pos = body.index("Adaptive mini format upgrade check:")
finalize_pos = body.index("FinalizeAdaptiveMiniWaveformFormat(")
assert diag_pos < finalize_pos

assert CMAKE.count("src/AdaptiveMiniWaveformFormatPreparation.cpp") == 1
assert BEHAVIOR.count("src/AdaptiveMiniWaveformFormatPreparation.cpp") == 1
assert "TestAdaptiveMiniWaveformFormatPreparation" in BEHAVIOR
assert "NAME TestAdaptiveMiniWaveformFormatPreparationBoundary" in BEHAVIOR

for forbidden in (
    "AppState.h",
    "AsyncWorker.h",
    "DiagnosticsLogger",
    "ModuleUI.h",
    "Platform::",
    "glDelete",
    "main.cpp",
    "WindowsMain",
):
    assert forbidden not in HEADER, forbidden
    assert forbidden not in SOURCE, forbidden

print("PASS: adaptive mini waveform format preparation boundary contracts")
