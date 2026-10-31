# Wwise WAAPI Provenance

**Scope:** Aris Wwise transfer request construction
**Reviewed:** 2026-07-11
**Authority:** Audiokinetic public documentation

## Rule

Aris may use Wwise behavior only when the request method, argument names, and
result interpretation are supported by Audiokinetic's published WAAPI
documentation. A third-party integration plugin is not an API specification
and is not an accepted source for Aris implementation details.

## Public specification map

| Aris behavior | WAAPI method / field | Published source |
| --- | --- | --- |
| Import one or more audio files | `ak.wwise.core.audio.import`; `importOperation`, `default.importLanguage`, `imports`, `audioFile`, `objectPath` | <https://www.audiokinetic.com/library/edge/?id=ak__wwise__core__audio__import.html&source=SDK> |
| Map imported results to source files | `objects[].originalRelativeFilePath` | <https://www.audiokinetic.com/library/edge/?id=ak__wwise__core__audio__import.html&source=SDK> |
| Apply one Property Template to imported objects | `ak.wwise.core.object.pasteProperties`; `source`, `targets[]` | <https://www.audiokinetic.com/library/edge/?id=ak_wwise_core_object_pasteproperties.html&source=SDK> |
| Begin one undoable Aris batch | `ak.wwise.core.undo.beginGroup`; `displayName` | <https://www.audiokinetic.com/library/edge/?id=ak_wwise_core_undo_begingroup.html&source=SDK> |
| End the undo group | `ak.wwise.core.undo.endGroup` | <https://www.audiokinetic.com/library/edge/?id=ak_wwise_core_undo_endgroup.html&source=SDK> |
| Create tagged Actor-Mixer hierarchy paths | `objectPath` object-type tags used by `audio.import` | <https://www.audiokinetic.com/library/edge/?id=ak__wwise__core__audio__import.html&source=SDK> |

## Source binding

- `src/WwiseTransferExecute.cpp::buildAudioImportArguments` constructs only the
  documented import fields.
- `src/WwiseTransferExecute.cpp::buildPastePropertiesArguments` always sends a
  `source` string and a `targets` array.
- `src/WwiseTransferExecute.cpp::buildUndoBeginArguments` uses the documented
  `displayName`, while the guard guarantees `endGroup` on every exit.
- `tests/test_stage1_provenance_gate.py` rejects private-binary provenance
  markers and pins these request fields and authoritative links.

## Review boundary

This note establishes public-spec provenance for the current request surface;
it does not assert that any external plugin source was independently audited,
nor does it grant permission to redistribute Wwise or third-party binaries.
