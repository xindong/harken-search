/***************************************
*** REAPER Plug-in API
**
** Copyright (C) 2006-2015, Cockos Incorporated
**
**    This software is provided 'as-is', without any express or implied
**    warranty. In no event will the authors be held liable for any damages
**    arising from the use of this software.
**
**    Permission is granted to anyone to use this software for any purpose,
**    including commercial applications, and to alter it and redistribute it
**    freely, subject to the following restrictions:
**
**    1. The origin of this software must not be misrepresented; you must not
**       claim that you wrote the original software. If you use this software
**       in a product, an acknowledgment in the product documentation would be
**       appreciated but is not required.
**    2. Altered source versions must be plainly marked as such, and must not be
**       misrepresented as being the original software.
**    3. This notice may not be removed or altered from any source distribution.
***************************************/

#pragma once

// Minimal macOS ABI subset of the REAPER extension SDK. The original SDK is
// maintained by Cockos: https://github.com/justinfrankel/reaper-sdk/blob/main/sdk/reaper_plugin.h
// This locally written subset only describes fields used by this extension;
// it is not a replacement for the full SDK.

struct reaper_plugin_info_t {
    int caller_version;
    void* hwnd_main;
    int (*Register)(const char* name, void* information);
    void* (*GetFunc)(const char* name);
};

inline constexpr int kReaperPluginVersion = 0x20E;

extern "C" __attribute__((visibility("default")))
int ReaperPluginEntry(void* instance, reaper_plugin_info_t* host);
