from pathlib import Path
import json
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src/OriginalAudioWriteConfirmGate.h"
SOURCE_PATH = ROOT / "src/OriginalAudioWriteConfirmGate.cpp"
LEDGER_PATH = ROOT / "locale/harken.ui.strings.json"
COVER_IMPORT_START = "// COVER IMPORT DIALOG"
COVER_IMPORT_END = "// AI 封面生成弹窗"

MAIN = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
APPSTATE = (ROOT / "src/AppState.h").read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BEHAVIOR = (ROOT / "cmake/harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
RULES = (ROOT / "docs/architecture/stage-4-dependency-rules.json").read_text(
    encoding="utf-8"
)
STAGE7 = (ROOT / "tools/stage7_api_contract_audit.py").read_text(encoding="utf-8")


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
    raise AssertionError("unterminated function: " + signature)


cover_start = MAIN.index(COVER_IMPORT_START)
cover_end = MAIN.index(COVER_IMPORT_END, cover_start)
cover_import = MAIN[cover_start:cover_end]
assert "MetadataWriter::writeAlbumArt" not in cover_import, (
    "cover import overlay slice still writes original audio before confirm"
)
assert "RequestOriginalAudioWriteConfirm(state)" in cover_import
assert "EvaluateOriginalAudioWriteConfirm" not in cover_import
assert "pendingOriginalAudioWriteKind" in cover_import
assert "ImportCover" in cover_import
assert "loadImageFile" in cover_import

write_cover_start = MAIN.index("L(Ctx_WriteCoverN)")
write_cover_end = MAIN.index("L(Meta_EmbedCoverHint)", write_cover_start)
write_cover = MAIN[write_cover_start:write_cover_end]
assert "StartEmbedMetadataBatch(state, selectedRecords);" not in write_cover, (
    "write-cover menu still starts embed batch before confirm"
)
assert "RequestOriginalAudioWriteConfirm(state)" in write_cover
assert "pendingOriginalAudioWriteRecords = selectedRecords" in write_cover

for key in (
    "Dlg_ConfirmWriteOriginalAudio",
    "Main_WriteOriginalAudioAsk",
    "Main_WriteOriginalAudioNote",
):
    assert key in MAIN, "missing confirm key " + key

assert "showOriginalAudioWriteConfirm" in APPSTATE
assert "OriginalAudioWritePendingKind" in APPSTATE
assert "pendingOriginalAudioWritePaths" in APPSTATE
assert "pendingOriginalAudioWriteRecords" in APPSTATE
assert "pendingOriginalAudioWriteImageData" in APPSTATE
assert "pendingOriginalAudioWriteMimeType" in APPSTATE
assert "pendingIds" not in APPSTATE.split("showCoverRemoveConfirm", 1)[-1][
    :1200
]

apply_body = function_body(MAIN, "static void ApplyPendingOriginalAudioWriteUnlocked")
assert "MetadataWriter::writeAlbumArt" in apply_body
assert "StartEmbedMetadataBatch(" in apply_body
assert "pendingOriginalAudioWriteRecords" in apply_body
assert "pendingOriginalAudioWritePaths" in apply_body
assert "coverImportIds" not in apply_body, "ae5-style coverImportIds loop must not mark hasCover"
assert "selectedIndicesSet" not in apply_body
assert "selectedResultIndex" not in apply_body
assert "filePath" in apply_body
assert "hasCover = true" in apply_body
assert "if (!succeeded.empty())" in apply_body
assert "assignEmbeddedCoverByPaths" in apply_body
assign_idx = apply_body.index("assignEmbeddedCoverByPaths")
empty_idx = apply_body.index("if (!succeeded.empty())")
assert empty_idx < assign_idx
assert "InvalidateFileCoverCaches(succeeded)" in apply_body
assert "ClearPendingOriginalAudioWrite(state)" in apply_body

gated_apply = function_body(MAIN, "static void ApplyPendingOriginalAudioWrite(")
assert "TryInvokeOriginalAudioWrite(" in gated_apply
assert "EvaluateOriginalAudioWriteConfirm(" in gated_apply
assert "hasCover" not in gated_apply
assert "writeAlbumArt" not in gated_apply

prompt_body = function_body(MAIN, "static void RequestOriginalAudioWriteConfirm")
assert "EvaluateOriginalAudioWriteConfirm(" in prompt_body
assert "state.settings.suppressConfirmDialogs" in prompt_body
assert "TryInvokeOriginalAudioWrite(" in prompt_body
assert "state.showOriginalAudioWriteConfirm = true" in prompt_body
assert "ImGui::OpenPopup(L(Dlg_ConfirmWriteOriginalAudio))" not in prompt_body
assert "ImGui::OpenPopup(L(Dlg_ConfirmWriteOriginalAudio))" in MAIN

popup_idx = MAIN.index("aura::ui::BeginAlertDialog(L(Dlg_ConfirmWriteOriginalAudio)")
popup_slice = MAIN[popup_idx : popup_idx + 1800]
assert "EscClosePopup" in popup_slice
assert "ClearPendingOriginalAudioWrite(state)" in popup_slice
assert "ApplyPendingOriginalAudioWrite(state)" in popup_slice
assert "L(OK)" in popup_slice
assert "L(Cancel)" in popup_slice
assert "L(Main_WriteOriginalAudioAsk)" in popup_slice
assert "L(Main_WriteOriginalAudioNote)" in popup_slice

reset_body = function_body(MAIN, "void ResetDatabaseState(AppState& state)")
assert "ClearPendingOriginalAudioWrite(state)" in reset_body
assert reset_body.find("ClearPendingOriginalAudioWrite") < reset_body.find(
    "searchWorker.stop"
)

switch_body = function_body(MAIN, "static bool SwitchDatabase")
assert "ClearPendingOriginalAudioWrite(state)" in switch_body
assert switch_body.find("IsNetworkPath") < switch_body.find(
    "ClearPendingOriginalAudioWrite"
) < switch_body.find("state.batchFeatureCancelRequested.store")

clear_body = function_body(MAIN, "static void ClearPendingOriginalAudioWrite(AppState& state) {")
assert "coverImportIds.clear()" in clear_body
assert "ImportCover" in clear_body
assert "pendingOriginalAudioWriteKind" in clear_body

assert HEADER_PATH.is_file(), "missing gate header after wiring assertions"
assert SOURCE_PATH.is_file(), "missing gate source after wiring assertions"
HEADER = HEADER_PATH.read_text(encoding="utf-8")
SOURCE = SOURCE_PATH.read_text(encoding="utf-8")
LEDGER = json.loads(LEDGER_PATH.read_text(encoding="utf-8"))

for token in (
    "enum class OriginalAudioWriteConfirmDecision",
    "struct OriginalAudioWriteConfirmRequest",
    "EvaluateOriginalAudioWriteConfirm(",
    "ShouldWriteOriginalAudio(",
    "TryInvokeOriginalAudioWrite(",
):
    assert token in HEADER, token

for forbidden in (
    "AppState",
    "ImGui",
    "MetadataWriter",
    "main.cpp",
    "WindowsMain",
    "Platform::",
):
    assert forbidden not in HEADER, forbidden
    assert forbidden not in SOURCE, forbidden

assert "src/OriginalAudioWriteConfirmGate.cpp" in CMAKE
assert "NAME TestOriginalAudioWriteConfirmGate" in BEHAVIOR
assert "tests/test_original_audio_write_confirm_gate.cpp" in BEHAVIOR
assert "tests/test_original_audio_write_confirm_gate_boundary.py" in BEHAVIOR
assert "NAME TestOriginalAudioWriteConfirmIo" in BEHAVIOR
assert "tests/test_original_audio_write_confirm_io.cpp" in BEHAVIOR
assert "EXCLUDE_FROM_ALL" in BEHAVIOR[
    BEHAVIOR.index("TestOriginalAudioWriteConfirmIo")
    - 80 : BEHAVIOR.index("TestOriginalAudioWriteConfirmIo")
    + 80
]
assert '"src/OriginalAudioWriteConfirmGate.h"' in RULES
assert '"src/OriginalAudioWriteConfirmGate.cpp"' in RULES
assert (
    '"src/OriginalAudioWriteConfirmGate.h": '
    '"tests/test_original_audio_write_confirm_gate_boundary.py"'
    in STAGE7
)

by_id = {entry["id"]: entry for entry in LEDGER["entries"]}
for key in (
    "Dlg_ConfirmWriteOriginalAudio",
    "Main_WriteOriginalAudioAsk",
    "Main_WriteOriginalAudioNote",
):
    entry = by_id[key]
    for lang in ("zh", "en", "ja"):
        assert entry[lang].strip(), key + "." + lang + " empty"

note = by_id["Main_WriteOriginalAudioNote"]
assert "原文件" in note["zh"]
assert "original file" in note["en"].lower()
assert "元ファイル" in note["ja"]
assert note["ja"] != note["zh"]
assert re.search(r"[぀-ヿ]", note["ja"]), "ja column must be Japanese"

ask = by_id["Main_WriteOriginalAudioAsk"]
for lang in ("zh", "en", "ja"):
    assert ask[lang].count("%d") == 1, lang + " fmt count drifted"

print("PASS: original audio write confirm gate boundary contracts")

IO_PATH = ROOT / "tests/test_original_audio_write_confirm_io.cpp"
IO = IO_PATH.read_text(encoding="utf-8")
fixture_ctor = function_body(IO, "explicit FixtureDir(")
destructor = function_body(IO, "~FixtureDir(")

assert "create_directories(approved" not in fixture_ctor, (
    "IO fixture still creates the approved root instead of read-only verifying it"
)
assert ".rfind(" not in IO, (
    "IO fixture still uses string-prefix rfind instead of a path-component boundary"
)
assert "lexically_relative" in IO, (
    "IO fixture lacks a path-component strict-child check"
)
assert "parent_path" in IO, (
    "IO fixture does not walk ancestors for reparse inspection"
)
assert destructor.find("remove_all") != -1, "IO fixture destructor no longer cleans the unique dir"
assert (
    "Reverify" in destructor or "InspectPath" in destructor or "VerifyUniqueFixture" in destructor
), "IO fixture destructor still removes without re-verifying identity"
assert destructor.find("remove_all") > max(
    destructor.find("Reverify"), destructor.find("InspectPath"), destructor.find("VerifyUniqueFixture")
), "IO fixture destructor still removes before re-verification"

for message in (
    "unconfirmed wav does not invoke writer",
    "cancelled wav does not invoke writer",
    "cancelled+confirmed wav does not invoke writer",
    "cancelled+suppress wav does not invoke writer",
    "cancelled+confirmed+suppress wav does not invoke writer",
    "unconfirmed mp3 does not invoke writer",
    "cancelled mp3 does not invoke writer",
    "cancelled+confirmed mp3 does not invoke writer",
    "cancelled+suppress mp3 does not invoke writer",
    "cancelled+confirmed+suppress mp3 does not invoke writer",
    "confirmed mp3 writeAlbumArt returned true",
):
    assert message in IO, "IO source missing dual-format coverage: " + message

assert "OriginalAudioWriteConfirmRequest{true, false, false}" not in IO, (
    "IO still writes MP3 through suppress-only without cancel coverage"
)
assert "approved root cleanup is refused" in IO
assert "sibling prefix path is rejected without create" in IO
assert "dotdot escape is rejected without create" in IO
assert "forced ancestor reparse is a guard decision reject" in IO

print("PASS: original audio write confirm IO source contracts")
write_helper_present = "bool GuardedWriteAlbumArt" in IO
read_helper_present = "bool GuardedReadAlbumArt" in IO
assert write_helper_present, (
    "IO still calls MetadataWriter without a fail-closed GuardedWriteAlbumArt wrapper"
)
assert read_helper_present, (
    "IO still calls MetadataReader without a fail-closed GuardedReadAlbumArt wrapper"
)

write_helper = function_body(IO, "bool GuardedWriteAlbumArt")
read_helper = function_body(IO, "bool GuardedReadAlbumArt")
assert "InspectPath" in write_helper
assert "InspectPath" in read_helper
assert "MetadataWriter::writeAlbumArt" in write_helper
assert "MetadataReader::readAlbumArt" in read_helper
assert write_helper.find("ok") != -1
assert write_helper.find("return") < write_helper.find("MetadataWriter::writeAlbumArt"), (
    "GuardedWriteAlbumArt still reaches writer without a fail-closed return"
)
assert read_helper.find("return") < read_helper.find("MetadataReader::readAlbumArt"), (
    "GuardedReadAlbumArt still reaches reader without a fail-closed return"
)
assert IO.count("MetadataWriter::writeAlbumArt") == 1, (
    "IO still has unwrapped MetadataWriter::writeAlbumArt call sites"
)
assert IO.count("MetadataReader::readAlbumArt") == 1, (
    "IO still has unwrapped MetadataReader::readAlbumArt call sites"
)
assert "++g_writeCalls" in write_helper
assert IO.count("++g_writeCalls") == 1, (
    "writer counter still increments outside the fail-closed helper"
)
assert "++g_readCalls" in read_helper
assert IO.count("++g_readCalls") == 1, (
    "reader counter still increments outside the fail-closed helper"
)

blocked = function_body(IO, "void ExpectBlockedWrite(")
assert "GuardedReadAlbumArt" in blocked
assert "MetadataReader::readAlbumArt" not in blocked
assert "GuardedWriteAlbumArt" in blocked
assert "MetadataWriter::writeAlbumArt" not in blocked

main_body = function_body(IO, "int main()")
assert "GuardedWriteAlbumArt" in main_body
assert "GuardedReadAlbumArt" in main_body
assert "MetadataWriter::writeAlbumArt" not in main_body
assert "MetadataReader::readAlbumArt" not in main_body
assert "forced reparse skips writer" in main_body
assert "forced reparse skips reader" in main_body

summary_idx = main_body.find("Original Audio Write Confirm IO")
cleanup_idx = main_body.find("CleanupNow")
return_idx = main_body.rfind("return g_failures")
assert cleanup_idx != -1 and summary_idx != -1 and cleanup_idx < summary_idx, (
    "main still prints failure summary before unique-dir cleanup can join g_failures"
)
assert cleanup_idx < return_idx, (
    "main still computes return before unique-dir cleanup can join g_failures"
)

destructor = function_body(IO, "~FixtureDir(")
assert "CleanupNow" in IO
assert "cleaned_" in destructor or "cleanupAttempted_" in destructor
assert destructor.find("remove_all") == -1 or "CleanupNow" in destructor, (
    "destructor still deletes without going through explicit cleanup that can no-op after success"
)

print("PASS: original audio write confirm IO fail-closed and cleanup-before-return contracts")
