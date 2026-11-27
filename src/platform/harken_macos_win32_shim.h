// Umbrella for the macOS platform shim used by the shared UI sources.
//
// The shared UI (src/main.cpp and the settings dialogs) is written against the
// Windows API surface. On macOS it is built against this shim instead, split
// into one header per concern so the slices can land and be reviewed
// independently:
//
//   win32_compat.h        CRT spellings and message-box constants   (line A)
//   shim_window.h         window/surface types, placement, keys     (line A)
//   shim_ui.h             font factories, dialog helpers            (line B)
//   shim_settings.h       settings dialog helpers                   (line B)
//
// Rules:
//   - Windows never includes this umbrella; the guard below makes it inert.
//   - A slice adds only its own header here. Do not edit another slice's file.
//   - Anything needing real behaviour gets a macOS implementation behind a
//     Platform:: helper, not a silent no-op in the shim.
#pragma once

#ifndef _WIN32

#include "platform/win32_compat.h"
#include "platform/shim_window.h"

#endif  // !_WIN32
