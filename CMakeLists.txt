cmake_minimum_required(VERSION 3.16)
project(HarkenSearch VERSION 1.0.41 LANGUAGES C CXX)

enable_testing()
find_package(Python3 COMPONENTS Interpreter QUIET)
find_package(Threads REQUIRED)

if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "Python3 is required to generate localization tables from locale/harken.ui.strings.json")
endif()
set(ARIS_LOCALE_LEDGER "${CMAKE_CURRENT_SOURCE_DIR}/locale/harken.ui.strings.json")
set(ARIS_LOCALE_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_localization.py")
set(ARIS_LOCALE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ARIS_LOCALE_GEN_H "${ARIS_LOCALE_GEN_DIR}/LocalizationGenerated.h")
set(ARIS_LOCALE_GEN_STR "${ARIS_LOCALE_GEN_DIR}/LocalizationStrings.generated.h")
add_custom_command(
    OUTPUT "${ARIS_LOCALE_GEN_H}" "${ARIS_LOCALE_GEN_STR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ARIS_LOCALE_GEN_DIR}"
    COMMAND ${Python3_EXECUTABLE}
        "${ARIS_LOCALE_GENERATOR}"
        --ledger "${ARIS_LOCALE_LEDGER}"
        --generate
        --output-dir "${ARIS_LOCALE_GEN_DIR}"
    DEPENDS "${ARIS_LOCALE_LEDGER}" "${ARIS_LOCALE_GENERATOR}"
    COMMENT "Generating localization tables from JSON ledger..."
    VERBATIM
)
add_custom_target(ArisLocalizationGenerated DEPENDS "${ARIS_LOCALE_GEN_H}" "${ARIS_LOCALE_GEN_STR}")
include_directories("${ARIS_LOCALE_GEN_DIR}")

function(aris_attach_generated_localization)
    get_property(_aris_locale_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_aris_locale_target IN LISTS _aris_locale_targets)
        if(_aris_locale_target STREQUAL "ArisLocalizationGenerated")
            continue()
        endif()
        get_target_property(_aris_locale_type ${_aris_locale_target} TYPE)
        if(_aris_locale_type STREQUAL "INTERFACE_LIBRARY" OR _aris_locale_type STREQUAL "UTILITY")
            continue()
        endif()
        add_dependencies(${_aris_locale_target} ArisLocalizationGenerated)
    endforeach()
endfunction()

function(aura_set_test_labels TEST_NAME)
    if(ARGN)
        set_property(TEST ${TEST_NAME} PROPERTY LABELS "${ARGN}")
    endif()
endfunction()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# GitHub is the current distribution backend. Keep the repository coordinate
# configurable so a future repository move only changes release configuration
# and the injected product build definition.
set(ARIS_UPDATE_GITHUB_REPOSITORY "xindong/harken-search" CACHE STRING
    "GitHub repository owner/name used for Aris update discovery and assets")
if(NOT ARIS_UPDATE_GITHUB_REPOSITORY MATCHES "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR
        "ARIS_UPDATE_GITHUB_REPOSITORY must be owner/name using safe GitHub characters")
endif()
set(ARIS_PUBLIC_UPDATE_GITHUB_REPOSITORY "xindong/harken-search" CACHE STRING
    "Canonical public GitHub repository used by public release builds")
if(NOT ARIS_PUBLIC_UPDATE_GITHUB_REPOSITORY MATCHES "^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR
        "ARIS_PUBLIC_UPDATE_GITHUB_REPOSITORY must be owner/name using safe GitHub characters")
endif()

set(ARIS_UPDATE_MIRROR_BASE_URL "https://harkensearch.xd.com/aris/" CACHE STRING
    "Optional exact byte mirror for trusted Aris GitHub release assets")
if(ARIS_UPDATE_MIRROR_BASE_URL AND
   (NOT ARIS_UPDATE_MIRROR_BASE_URL MATCHES "^https?://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._/-]*)?/$" OR
    ARIS_UPDATE_MIRROR_BASE_URL MATCHES "\\.\\."))
    message(FATAL_ERROR
        "ARIS_UPDATE_MIRROR_BASE_URL must be an exact HTTP(S) base URL ending in / without traversal")
endif()

option(ARIS_UPDATE_LOCAL_DEMO
    "Build the QA-only loopback local update demonstration" OFF)
option(ARIS_UPDATE_QA_CLI
    "Build the CLI-only real-release update QA entry" OFF)
option(ARIS_UPDATE_NOTICE_QA
    "Build the isolated native Update Center visual evidence seam" OFF)
set(ARIS_UPDATE_LOCAL_DEMO_BASE_URL "" CACHE STRING
    "QA-only local update mirror; valid only when ARIS_UPDATE_LOCAL_DEMO=ON")
set(ARIS_UPDATE_LOCAL_DEMO_OLD_VERSION "" CACHE STRING
    "QA-only current version identity")
set(ARIS_UPDATE_LOCAL_DEMO_NEW_VERSION "" CACHE STRING
    "QA-only candidate version identity")
set(ARIS_UPDATE_LOCAL_DEMO_OLD_BUILD_ID "" CACHE STRING
    "QA-only current build identity")
set(ARIS_UPDATE_LOCAL_DEMO_NEW_BUILD_ID "" CACHE STRING
    "QA-only candidate build identity")
set(ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION "" CACHE STRING
    "QA-only version compiled into the isolated demo executable")
set(ARIS_UPDATE_LOCAL_DEMO_BUILD_ID "" CACHE STRING
    "QA-only build id compiled into the isolated demo executable")
set(ARIS_UPDATE_LOCAL_DEMO_ROOT "" CACHE PATH
    "QA-only updater persistence root")
if(ARIS_UPDATE_LOCAL_DEMO)
    if(NOT ARIS_UPDATE_LOCAL_DEMO_BASE_URL STREQUAL
       "http://127.0.0.1:4328/harken-search-demo/")
        message(FATAL_ERROR
            "ARIS_UPDATE_LOCAL_DEMO_BASE_URL must be the exact QA loopback URL")
    endif()
    if(NOT ARIS_UPDATE_LOCAL_DEMO_OLD_VERSION STREQUAL "1.0.2" OR
       NOT ARIS_UPDATE_LOCAL_DEMO_NEW_VERSION STREQUAL "1.0.3" OR
       NOT ARIS_UPDATE_LOCAL_DEMO_OLD_BUILD_ID STREQUAL
           "local-demo-old-b76dd15d" OR
       NOT ARIS_UPDATE_LOCAL_DEMO_NEW_BUILD_ID STREQUAL
           "local-demo-new-b76dd15d")
        message(FATAL_ERROR
            "ARIS_UPDATE_LOCAL_DEMO requires the approved old/new version and build identities")
    endif()
    if(NOT ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION STREQUAL "1.0.2" AND
       NOT ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION STREQUAL "1.0.3")
        message(FATAL_ERROR
            "ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION must be 1.0.2 or 1.0.3")
    endif()
    if(NOT ARIS_UPDATE_LOCAL_DEMO_BUILD_ID MATCHES
       "^local-demo-(old|new)-b76dd15d$")
        message(FATAL_ERROR
            "ARIS_UPDATE_LOCAL_DEMO_BUILD_ID must be the approved old/new identity")
    endif()
    if(ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION STREQUAL ARIS_UPDATE_LOCAL_DEMO_OLD_VERSION AND
       NOT ARIS_UPDATE_LOCAL_DEMO_BUILD_ID STREQUAL ARIS_UPDATE_LOCAL_DEMO_OLD_BUILD_ID)
        message(FATAL_ERROR "demo old version must use the old build id")
    endif()
    if(ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION STREQUAL ARIS_UPDATE_LOCAL_DEMO_NEW_VERSION AND
       NOT ARIS_UPDATE_LOCAL_DEMO_BUILD_ID STREQUAL ARIS_UPDATE_LOCAL_DEMO_NEW_BUILD_ID)
        message(FATAL_ERROR "demo new version must use the new build id")
    endif()
    if(NOT ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION STREQUAL ARIS_UPDATE_LOCAL_DEMO_OLD_VERSION AND
       NOT ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION STREQUAL ARIS_UPDATE_LOCAL_DEMO_NEW_VERSION)
        message(FATAL_ERROR "demo build version must match an approved pair version")
    endif()
    execute_process(
        COMMAND git rev-parse --path-format=absolute --git-common-dir
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _aris_demo_git_common_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _aris_demo_git_common_result
    )
    if(NOT _aris_demo_git_common_result EQUAL 0 OR _aris_demo_git_common_dir STREQUAL "")
        message(FATAL_ERROR "ARIS_UPDATE_LOCAL_DEMO requires a Git common directory")
    endif()
    get_filename_component(_aris_demo_main_repo "${_aris_demo_git_common_dir}" DIRECTORY)
    get_filename_component(_aris_demo_repo_parent "${_aris_demo_main_repo}" DIRECTORY)
    file(TO_CMAKE_PATH
        "${_aris_demo_repo_parent}/aris-local-update-demo-20260803"
        _aris_demo_approved_root)
    if(ARIS_UPDATE_LOCAL_DEMO_ROOT STREQUAL "")
        set(ARIS_UPDATE_LOCAL_DEMO_ROOT "${_aris_demo_approved_root}" CACHE PATH
            "QA-only updater persistence root" FORCE)
    endif()
    file(TO_CMAKE_PATH "${ARIS_UPDATE_LOCAL_DEMO_ROOT}" _aris_demo_root_normalized)
    if(NOT _aris_demo_root_normalized STREQUAL _aris_demo_approved_root)
        message(FATAL_ERROR "ARIS_UPDATE_LOCAL_DEMO_ROOT must be the approved disposable DemoRoot")
    endif()
    set(AIRS_BUILD_ID "${ARIS_UPDATE_LOCAL_DEMO_BUILD_ID}")
    set(AIRS_RELEASE_CHANNEL "testing")
    set(ARIS_UPDATE_LOCAL_DEMO_SOURCE_COMMIT "" CACHE STRING
        "QA-only source commit; must equal clean Git HEAD when demo is enabled")
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _aris_demo_git_head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND git status --porcelain
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _aris_demo_git_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT ARIS_UPDATE_LOCAL_DEMO_SOURCE_COMMIT STREQUAL "${_aris_demo_git_head}" OR
       _aris_demo_git_status)
        message(FATAL_ERROR "ARIS_UPDATE_LOCAL_DEMO sourceCommit requires a clean worktree HEAD")
    endif()
    set(ARIS_UPDATE_MIRROR_BASE_URL
        "${ARIS_UPDATE_LOCAL_DEMO_BASE_URL}" CACHE STRING
        "Optional exact byte mirror for trusted Aris GitHub release assets" FORCE)
endif()

# AUR-P1-047 Slice 2 - REAPER SDK auto-detection. The SDK is not vendored; point
# AURO_REAPER_SDK_ROOT at a local checkout of justinfrankel/reaper-sdk (the sdk/
# subdirectory, which is zlib-licensed). The reaper-plugins/ tree of upstream is
# Aris does not consume the upstream LGPL v2.1 plug-in tree; keep it out of this repository.
# AURO_REAPER_SDK_PRESENT gates the HarkenSearchReaperBridge shared-lib target and
# the AURO_REAPER_SDK_PRESENT compile definition used in ReaperBridgeStub.cpp.
set(AURO_REAPER_SDK_ROOT "" CACHE PATH "Optional REAPER SDK root (path to reaper-sdk/sdk directory) for reaper_harken.dll stub")
if(AURO_REAPER_SDK_ROOT AND EXISTS "${AURO_REAPER_SDK_ROOT}/reaper_plugin.h")
    set(AURO_REAPER_SDK_PRESENT TRUE)
    add_compile_definitions(AURO_REAPER_SDK_PRESENT=1)
    message(STATUS "REAPER SDK detected: ${AURO_REAPER_SDK_ROOT}")
endif()

# SQLite source files
set(SQLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/sqlite)
set(SQLITE_SOURCES
    ${SQLITE_DIR}/sqlite3.c
)

# Force-enable FTS5 in the bundled SQLite amalgamation.
# We compile sqlite3.c as C, and apply these defs only to that translation unit.
set_source_files_properties(${SQLITE_DIR}/sqlite3.c PROPERTIES
    COMPILE_DEFINITIONS "SQLITE_ENABLE_FTS5;SQLITE_ENABLE_FTS3_PARENTHESIS"
)

# ImGui source files
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
# Platform-specific ImGui backends (F-031)
if(WIN32)
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
elseif(APPLE)
    list(APPEND IMGUI_SOURCES ${IMGUI_DIR}/backends/imgui_impl_osx.mm)
endif()

# Whisper.cpp directories
set(WHISPER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/whisper.cpp)
set(WHISPER_BUILD_DIR ${WHISPER_DIR}/build)
set(RLOTTIE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rlottie)
set(RLOTTIE_BIN_DIR "${RLOTTIE_DIR}/build/Release")
set(RLOTTIE_IMG_DIR "${RLOTTIE_DIR}/build/src/vector/stb/Release")

# WebView2 SDK (NuGet package)
set(WEBVIEW2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/webview2/build/native)

# IXWebSocket (F-034 - Wwise Authoring API client over WebSocket + JSON-RPC 2.0)
# WAAPI uses plain ws://localhost:8080: no TLS, compression, or demo target.
set(USE_TLS OFF CACHE BOOL "ixwebsocket: disable TLS (WAAPI runs on localhost)" FORCE)
set(USE_MBED_TLS OFF CACHE BOOL "ixwebsocket: no mbedtls" FORCE)
set(USE_OPEN_SSL OFF CACHE BOOL "ixwebsocket: no openssl" FORCE)
set(USE_ZLIB OFF CACHE BOOL "ixwebsocket: disable permessage-deflate (WAAPI payload is small JSON)" FORCE)
set(BUILD_DEMO OFF CACHE BOOL "ixwebsocket: no demo exe" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "ixwebsocket: link as static" FORCE)
add_subdirectory(thirdparty/ixwebsocket EXCLUDE_FROM_ALL)
# Aris links all targets with /MD + _ITERATOR_DEBUG_LEVEL=0 even in Debug (to
# stay ABI-compatible with prebuilt third-party Release libs: whisper/aubio).
# Force the same on the freshly-built ixwebsocket target, or link fails with
# LNK2038 on RuntimeLibrary / _ITERATOR_DEBUG_LEVEL mismatch.
if(MSVC AND TARGET ixwebsocket)
    set_property(TARGET ixwebsocket PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreadedDLL")
    target_compile_definitions(ixwebsocket PRIVATE
        $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>
    )
endif()

# Diagnostics and test tools stay on for development builds and off for public releases.
option(AURA_BUILD_DIAGNOSTICS "Include diagnostics & dev tools (disable for release)" ON)
option(ARIS_ENABLE_IMGUI_VIEWPORTS "Enable Dear ImGui multi-viewport windows for floating self-Dock panels" ON)
option(ARIS_PREVIEW_ISOLATION "Preview-only isolation guards for historical UI replay" OFF)
set(ARIS_PREVIEW_ISOLATION_ROOT
    "D:/Aris-QA/RQ-20261010-NONDOCK-PREVIEW-preview-nondock-745"
    CACHE PATH "Compile-time fail-closed preview data root")
if(ARIS_PREVIEW_ISOLATION)
    if(ARIS_PREVIEW_ISOLATION_ROOT STREQUAL "")
        message(FATAL_ERROR "ARIS_PREVIEW_ISOLATION=ON requires ARIS_PREVIEW_ISOLATION_ROOT")
    endif()
    file(TO_CMAKE_PATH "${ARIS_PREVIEW_ISOLATION_ROOT}" _aris_preview_root)
    add_compile_definitions(
        ARIS_PREVIEW_ISOLATION=1
        ARIS_PREVIEW_ISOLATION_ROOT_UTF8="${_aris_preview_root}"
    )
endif()
# Internal beta removes developer UI entry points and the adjacent dev_mode.txt bypass; crash logs remain.
option(AURA_INTERNAL_BETA "Internal beta: strip dev UI entries & seal dev_mode.txt bypass" OFF)
# Release hardening: no PDB, whole-program optimization, and optional static CRT.
option(AURA_RELEASE_HARDEN "Release: no PDB, /GL, /LTCG (strip symbols, harder to reverse)" ON)
option(AURA_RELEASE_STATIC_CRT "Release: link CRT statically (/MT); requires deps built with /MT" OFF)
# Optional slim-build warnings promote unused-code warnings without enabling all /W4 noise.
# Default OFF; enable with:
#     cmake -DAURA_SLIM_WARN_UNUSED=ON ...
option(AURA_SLIM_WARN_UNUSED "Slim: surface unused-* warnings on the Aris target (C4100/4189/4505/4702)" OFF)
# GPL boundary: aubio is GPL-3. Linking it makes the resulting binary GPL.
# OFF (default) = permissive build, BPM detection returns N/A via BpmDetectorStub.
# ON = internal/GPL build, requires thirdparty/aubio to exist on disk.
option(AURO_ENABLE_AUBIO_BPM "Link GPL aubio for BPM detection (makes binary GPL)" OFF)
option(ARIS_PUBLIC_RELEASE "Build a public-release-safe surface" OFF)
option(AURO_BUNDLE_AVID_PTSL "Allow bundling Avid PTSLC_CPP.dll after manual license confirmation" OFF)
if(ARIS_PUBLIC_RELEASE AND ARIS_UPDATE_QA_CLI)
    message(FATAL_ERROR
        "ARIS_PUBLIC_RELEASE rejects ARIS_UPDATE_QA_CLI; configure the isolated QA channel with ARIS_PUBLIC_RELEASE=OFF")
endif()
if(ARIS_PUBLIC_RELEASE AND
   NOT ARIS_UPDATE_GITHUB_REPOSITORY STREQUAL ARIS_PUBLIC_UPDATE_GITHUB_REPOSITORY)
    message(FATAL_ERROR
        "ARIS_PUBLIC_RELEASE requires ARIS_UPDATE_GITHUB_REPOSITORY to equal the canonical public repository")
endif()
set(ARIS_STAGE6_PERFORMANCE_RUN_INDEX "" CACHE FILEPATH
    "Optional current five-probe run-index.json used by the Stage 6 regression gate")
set(AIRS_CUEVI_FEEDBACK_ENDPOINT "" CACHE STRING "Cuevi feedback submission endpoint")
set(AIRS_CUEVI_FEEDBACK_KEY "" CACHE STRING "Restricted Cuevi feedback key; never commit or print")
set(AIRS_CUEVI_PROJECT_ID "" CACHE STRING "Cuevi project ID for Airs feedback")
set(AIRS_CUEVI_BOARD_ID "" CACHE STRING "Cuevi board ID for Airs feedback")
set(AIRS_CUEVI_MILESTONE_ID "" CACHE STRING "Cuevi milestone ID for Airs feedback")
set(AIRS_BUILD_ID "dev" CACHE STRING "Airs build identity sent with feedback")
set(AIRS_RELEASE_CHANNEL "dev" CACHE STRING "Airs release channel sent with feedback")
if(ARIS_UPDATE_LOCAL_DEMO)
    # Demo ON owns these identities; permit the ordinary dev defaults, but
    # reject any non-demo override before forcing the canonical cache values.
    if(NOT AIRS_RELEASE_CHANNEL STREQUAL "dev" AND
       NOT AIRS_RELEASE_CHANNEL STREQUAL "testing")
        message(FATAL_ERROR
            "ARIS_UPDATE_LOCAL_DEMO rejects an overridden AIRS_RELEASE_CHANNEL")
    endif()
    if(NOT AIRS_BUILD_ID STREQUAL "dev" AND
       NOT AIRS_BUILD_ID STREQUAL ARIS_UPDATE_LOCAL_DEMO_BUILD_ID)
        message(FATAL_ERROR
            "ARIS_UPDATE_LOCAL_DEMO rejects an overridden AIRS_BUILD_ID")
    endif()
    set(AIRS_BUILD_ID "${ARIS_UPDATE_LOCAL_DEMO_BUILD_ID}" CACHE STRING
        "Airs build identity sent with feedback" FORCE)
    set(AIRS_RELEASE_CHANNEL "testing" CACHE STRING
        "Airs release channel sent with feedback" FORCE)
endif()
if(ARIS_PUBLIC_RELEASE AND AIRS_RELEASE_CHANNEL STREQUAL "qa")
    message(FATAL_ERROR
        "QA builds must set ARIS_PUBLIC_RELEASE=OFF; public-release mode would clear Cuevi feedback credentials")
endif()
if(ARIS_PUBLIC_RELEASE)
    set(AIRS_CUEVI_FEEDBACK_ENDPOINT "" CACHE STRING "Cuevi feedback submission endpoint" FORCE)
    set(AIRS_CUEVI_FEEDBACK_KEY "" CACHE STRING "Restricted Cuevi feedback key; never commit or print" FORCE)
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CueviFeedbackBuildConfig.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/CueviFeedbackBuildConfig.h"
    @ONLY
)
# Product edition: 0=Base, 1=Pro, 2=Ultimate.
if(NOT DEFINED AURA_EDITION)
    set(AURA_EDITION 0 CACHE STRING "Product edition: 0=Base 1=Pro 2=Ultimate")
endif()

if(WIN32)
    set(AURO_MEDIA_FOUNDATION_AUDIO_SOURCE src/platform/media_foundation_audio_win32.cpp)
    set(AURO_METADATA_LEGACY_TAG_CONTENT_SOURCE src/platform/metadata_legacy_tag_content_win32.cpp)
    set(AURO_SEARCH_TEXT_GB2312_SOURCE src/platform/search_text_gb2312_win32.cpp)
    set(AURO_THEME_JSON_DIALOGS_SOURCE src/platform/theme_json_dialogs_win32.cpp)
    set(AURO_DATABASE_FILE_DIALOG_SOURCE src/platform/database_file_dialog_win32.cpp)
    set(AURO_EXECUTABLE_FILE_DIALOG_SOURCE src/platform/executable_file_dialog_win32.cpp)
    set(AURO_AVATAR_IMAGE_FILE_DIALOG_SOURCE src/platform/avatar_image_file_dialog_win32.cpp)
    set(AURO_REAPER_FILE_LIST_DIALOG_SOURCE src/platform/reaper_file_list_dialog_win32.cpp)
    set(AURO_THESAURUS_JSON_DIALOG_SOURCE src/platform/thesaurus_json_dialog_win32.cpp)
    set(AURO_SETTINGS_IMPORT_DIALOG_SOURCE src/platform/settings_import_dialog_win32.cpp)
    set(AURO_COVER_IMAGE_IMPORT_DIALOG_SOURCE src/platform/cover_image_import_dialog_win32.cpp)
    set(AURO_REGION_WAV_EXPORT_DIALOG_SOURCE src/platform/region_wav_export_dialog_win32.cpp)
    set(AURO_AUDIO_FILE_DIALOG_SOURCE src/platform/audio_file_dialog_win32.cpp)
    set(AURO_PROTOOLS_PATH_ENCODING_SOURCE src/platform/pro_tools_path_encoding_win32.cpp)
else()
    set(AURO_MEDIA_FOUNDATION_AUDIO_SOURCE src/platform/media_foundation_audio_unsupported.cpp)
    set(AURO_METADATA_LEGACY_TAG_CONTENT_SOURCE src/platform/metadata_legacy_tag_content_std.cpp)
    set(AURO_SEARCH_TEXT_GB2312_SOURCE src/platform/search_text_gb2312_std.cpp)
    set(AURO_THEME_JSON_DIALOGS_SOURCE src/platform/theme_json_dialogs_std.cpp)
    set(AURO_DATABASE_FILE_DIALOG_SOURCE src/platform/database_file_dialog_std.cpp)
    set(AURO_EXECUTABLE_FILE_DIALOG_SOURCE src/platform/executable_file_dialog_std.cpp)
    set(AURO_AVATAR_IMAGE_FILE_DIALOG_SOURCE src/platform/avatar_image_file_dialog_std.cpp)
    set(AURO_REAPER_FILE_LIST_DIALOG_SOURCE src/platform/reaper_file_list_dialog_std.cpp)
    set(AURO_THESAURUS_JSON_DIALOG_SOURCE src/platform/thesaurus_json_dialog_std.cpp)
    set(AURO_SETTINGS_IMPORT_DIALOG_SOURCE src/platform/settings_import_dialog_std.cpp)
    set(AURO_COVER_IMAGE_IMPORT_DIALOG_SOURCE src/platform/cover_image_import_dialog_std.cpp)
    set(AURO_REGION_WAV_EXPORT_DIALOG_SOURCE src/platform/region_wav_export_dialog_std.cpp)
    set(AURO_AUDIO_FILE_DIALOG_SOURCE src/platform/audio_file_dialog_std.cpp)
    set(AURO_PROTOOLS_PATH_ENCODING_SOURCE src/platform/pro_tools_path_encoding_std.cpp)
endif()

# Application source files
# main.cpp is Windows-only (13K LOC); macOS uses app_entry_macos.mm
set(APP_SOURCES
    ${AURO_MEDIA_FOUNDATION_AUDIO_SOURCE}
    ${AURO_METADATA_LEGACY_TAG_CONTENT_SOURCE}
    ${AURO_SEARCH_TEXT_GB2312_SOURCE}
    ${AURO_THEME_JSON_DIALOGS_SOURCE}
    ${AURO_DATABASE_FILE_DIALOG_SOURCE}
    ${AURO_EXECUTABLE_FILE_DIALOG_SOURCE}
    ${AURO_AVATAR_IMAGE_FILE_DIALOG_SOURCE}
    ${AURO_REAPER_FILE_LIST_DIALOG_SOURCE}
    ${AURO_THESAURUS_JSON_DIALOG_SOURCE}
    ${AURO_SETTINGS_IMPORT_DIALOG_SOURCE}
    ${AURO_COVER_IMAGE_IMPORT_DIALOG_SOURCE}
    ${AURO_REGION_WAV_EXPORT_DIALOG_SOURCE}
    ${AURO_AUDIO_FILE_DIALOG_SOURCE}
    src/AppState.cpp
    src/Utils.cpp
    src/PathUtils.cpp
    # L1 [RQ-20260911-04]: Localization.cpp now ships through the HarkenSearchLocalization
    # static lib - auto-linked below by the engine-TU pass. Product behaviour is
    # byte-equivalent: the same unit, same beta-macro forwarding.
    # NOTE: keep comments in APP_SOURCES free of closing parens - stage6 shape-pins
    # split the list text at the first one.
    src/UIRenderer.cpp
    src/PlaylistQuickBar.cpp
    src/SidebarActivityBar.cpp
    src/FileInfoPanel.cpp
    src/BuiltinThemeScheme.cpp
    src/TransientScrollbarHost.cpp
    src/RenderFrameRequestPolicy.cpp
    src/AudioEngine.cpp
    src/platform/audio_file_miniaudio.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/platform/database_audio_probe.cpp
    src/platform/database_audio_probe_miniaudio.cpp
    src/platform/database_audio_probe_media_foundation.cpp
    src/CenterSenseDsp.cpp
    src/CenterSenseDataSource.cpp
    src/WaveformExtractor.cpp
    src/NativeFloatReader.cpp
    src/FileBrowser.cpp
    src/Database.cpp
    src/RecordRemovalWorkflow.cpp
    src/RecordRenameWorkflow.cpp
    src/BatchRenamePolicy.cpp
    src/BatchRenameExecutionWorkflow.cpp
    src/BrowserSystemFolderSearchPolicy.cpp
    src/BrowserSystemFolderSearchRuntime.cpp
    src/FilePlayerDeferredDbWarmupRuntime.cpp
    src/FilePlayerDeferredDbWarmupOperation.cpp
    src/FilePlayerDatabaseOpenWorkflow.cpp
    src/FilePlayerDatabaseOpenAdapter.cpp
    src/DatabaseSwitchWorkerRuntime.cpp
    src/DatabaseSwitchBlockingOperation.cpp
    src/DatabaseSwitchBlockingOperationAdapter.cpp
    src/DatabasePackageReplacement.cpp
    src/DatabaseSessionMediaCacheReset.cpp
    src/WaveformRebuildRuntimeState.cpp
    src/WaveformRebuildBatchSelector.cpp
    src/WaveformRebuildFailurePersistence.cpp
    src/WaveformRebuildRequestSubmission.cpp
    src/SelectedWaveformRebuildWorkflow.cpp
    src/SelectedRecordRescanWorkflow.cpp
    src/BpmBatchScanWorkflow.cpp
    src/BpmBatchResultWorkflow.cpp
    src/SingleMusicAnalysisResultWorkflow.cpp
    src/BpmBatchCompletionWorkflow.cpp
    src/CoverBatchOperationWorkflow.cpp
    src/EmbedMetadataBatchRuntime.cpp
    src/OriginalAudioWriteConfirmGate.cpp
    src/AuditionPluginChain.cpp
    src/CoverRebuildRecordProcessor.cpp
    src/CoverRebuildLifecycleWorkflow.cpp
    src/CoverRebuildBatchCommitWorkflow.cpp
    src/LoadAllBatchRuntime.cpp
    src/AsyncSearchLoadAllResultStateAdapter.cpp
    src/SameFolderScanWorkflow.cpp
    src/FileTypeVisibilityPolicy.cpp
    src/AsyncSearchFacetResultWorkflow.cpp
    src/AsyncSearchFacetResultStateAdapter.cpp
    src/RandomShufflePlaybackWorkflow.cpp
    src/RandomShufflePlaybackStateAdapter.cpp
    src/AsyncSearchRefreshFacetsWorkflow.cpp
    src/AsyncSearchRefreshFacetsAdapter.cpp
    src/AsyncSearchRefreshFacetsStateAdapter.cpp
    src/AsyncSearchAppendResultWorkflow.cpp
    src/AsyncSearchAppendResultStateAdapter.cpp
    src/AsyncSearchRefreshCountRepairWorkflow.cpp
    src/AsyncSearchRefreshCountRepairAdapter.cpp
    src/AsyncSearchRefreshCountRepairStateAdapter.cpp
    src/AsyncSearchRefreshCompletionWorkflow.cpp
    src/AsyncSearchRefreshCompletionStateAdapter.cpp
    src/AsyncSearchRefreshMemoryWorkflow.cpp
    src/AsyncSearchRefreshMemoryAdapter.cpp
    src/AsyncSearchRefreshMemoryStateAdapter.cpp
    src/AsyncSearchWaveformPreloadWorkflow.cpp
    src/AsyncSearchWaveformPreloadStateAdapter.cpp
    src/AsyncSearchCoverPreloadWorkflow.cpp
    src/AsyncSearchCoverPreloadAdapter.cpp
    src/AsyncSearchCoverPreloadStateAdapter.cpp
    src/AsyncSearchEmptyFallbackWorkflow.cpp
    src/AsyncSearchEmptyFallbackStateAdapter.cpp
    src/AsyncSearchRefreshDiagnosticsWorkflow.cpp
    src/AsyncSearchRefreshDiagnosticsStateAdapter.cpp
    src/AsyncSearchRefreshLoadingWorkflow.cpp
    src/AsyncSearchRefreshLoadingStateAdapter.cpp
    src/AsyncSearchRefreshResultsWorkflow.cpp
    src/AsyncSearchRefreshResultsStateAdapter.cpp
    src/AsyncSearchRefreshResultCompositionAdapter.cpp
    src/AsyncSearchResultApplicationAdapter.cpp
    src/AsyncSearchResultApplicationWorkflow.cpp
    src/AsyncSearchResultEffectFactory.cpp
    src/SearchFilterStatePolicy.cpp
    src/SearchKeywordCompiler.cpp
    src/SearchFilterFileTypeAdapter.cpp
    src/SearchPresetFilterBuilders.cpp
    src/DatabaseSearchFilterBuilder.cpp
    src/CoverWallFilterActivityPolicy.cpp
    src/CoverFacetItemBuilder.cpp
    src/CoverCachedKeyItemBuilder.cpp
    src/CoverWallPageResultBuilder.cpp
    src/CoverWallFacetRefreshPolicy.cpp
    src/CoverWallCachePolicy.cpp
    src/CoverWallTextureMirrorPolicy.cpp
    src/CoverWallFailureCooldownPolicy.cpp
    src/CoverWallVisibleMirrorAudit.cpp
    src/DatabasePathNormalizationPolicy.cpp
    src/AsyncSearchResultPumpWorkflow.cpp
    src/AsyncSearchResultFilterAdapter.cpp
    src/AsyncSearchCoverPrimingRuntimeAdapter.cpp
    src/CoverWallRefreshStateAdapter.cpp
    src/CoverKeyFilterWorkflow.cpp
    src/CoverKeyFilterStateAdapter.cpp
    src/PlaybackHistoryStateAdapter.cpp
    src/WaveformDetailSchedulingStateAdapter.cpp
    src/WaveformViewResetStateAdapter.cpp
    src/AsyncSearchFacetWarmStartPolicy.cpp
    src/AsyncSearchCountFacetRepairSubmission.cpp
    src/PlaylistDisplayedFileCountPolicy.cpp
    src/AsyncSearchBrowsePreviewWorkflow.cpp
    src/AsyncSearchBrowsePreviewRefreshWorkflow.cpp
    src/AsyncSearchBrowsePreviewRefreshStateAdapter.cpp
    src/AsyncSearchResultLifecycleWorkflow.cpp
    src/AsyncSearchResultLifecycleStateAdapter.cpp
    src/AsyncSearchResultDispatchWorkflow.cpp
    src/AsyncSearchQueryTimingRegistry.cpp
    src/PlaylistResultFilterWorkflow.cpp
    src/AsyncSearchPlaylistContextWorkflow.cpp
    src/AsyncSearchPlaylistContextStateAdapter.cpp
    src/AsyncSearchPlaylistResultPreparationWorkflow.cpp
    src/AsyncSearchPlaylistResultPreparationStateAdapter.cpp
    src/WaveformRebuildRecordWorkflow.cpp
    src/WaveformRebuildBatchLifecycle.cpp
    src/WaveformRebuildCancellationRecovery.cpp
    src/WaveformRebuildCancellationRecoveryStateAdapter.cpp
    src/AITranscriptionResultStateAdapter.cpp
    src/ScanWaveformBackfillDrainWorkflow.cpp
    src/AdaptiveMiniWaveformFormatFinalization.cpp
    src/AdaptiveMiniWaveformFormatPreparation.cpp
    src/AdaptiveMiniWaveformRebuildActivation.cpp
    src/WaveformRebuildPumpGate.cpp
    src/DatabaseSessionScanStateReset.cpp
    src/DatabaseSessionViewStateReset.cpp
    src/SameFolderScanRuntime.cpp
    src/SearchQuery.cpp
    src/SearchCommands.cpp
    src/SearchIntent.cpp
    src/MetadataReader.cpp
    src/TagVocabularyMapper.cpp
    src/MusicQueue.cpp
    src/GaplessEngine.cpp
    src/MetadataWriter.cpp
    src/CoverReplacementFoundation.cpp
    src/CoverModule.cpp
    src/stb_image_impl.cpp
    src/ExternalLibraryCover.cpp
    src/AIEngine.cpp
    src/VoiceDeliveryAcceptance.cpp
    src/VoiceAssetOrganizer.cpp
    src/ThemeMascot.cpp
    src/Theme.cpp
    src/ThemeEditor.cpp
    src/ThemeInspector.cpp
    src/AnnotationBridge.cpp
    src/CueviFeedbackClient.cpp
    src/CueviFeedbackCompletionWorkflow.cpp
    src/ConsentManager.cpp
    src/SingleChannelExportWorkflow.cpp
    src/SingleChannelExportCompletionWorkflow.cpp
    src/CoverKeyCachePersistenceWorkflow.cpp
    src/DeferredSidebarBootstrapWorkflow.cpp
    src/ReaperFileListImportWorkflow.cpp
    src/HarkenSearchThemeManager.cpp
    # New metadata and workflow modules
    src/UCSCategory.cpp
    src/MetadataFields.cpp
    src/TagSystem.cpp
    src/UndoSystem.cpp
    src/EmbedScript.cpp
    src/NamingScript.cpp
    src/TransferEngine.cpp
    src/ResultActionMenu.cpp
    src/EditorSendSurfaceModel.cpp
    src/SendToTargetSurfaceModel.cpp
    src/HostSendRuntime.cpp
    src/HostSendModel.cpp
    src/ProToolsBridge.cpp
    src/CubaseAdapter.cpp
    src/WorkflowEngine.cpp
    src/ModuleUI.cpp
    # DAW send widget (split button + dropdown)
    src/DAWPanel.cpp
    # DAW bridge auto-installer registry (AUR-P1-047)
    src/DawBridgeInstaller.cpp
    src/ReaperBridgeDoctor.cpp
    src/ReaperBridgeClient.cpp
    # Memory management and file system monitoring
    src/MemoryBudget.cpp
    src/platform/cover_discovery_std.cpp
    src/platform/qa_files_std.cpp
    src/platform/filesystem_std.cpp
    src/platform/file_watcher.cpp
    # Async worker (waveform + search thread pool)
    src/AsyncWorker.cpp
    # F-001 slice7-A: post-scan cover + L1 waveform backfill queue (AppState-owned)
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    # WebUI Bridge (WebView2 -> C++ JSON-RPC bridge)
    # Product edition support plus Ed25519 license verification
    src/ProductEdition.cpp
    src/ProductModeRouting.cpp
    src/FeatureManifest.cpp
    src/ed25519/tweetnacl.c
    src/update/UpdateManifest.cpp
    src/update/UpdateCheckService.cpp
    src/update/UpdateApplicationService.cpp
    src/update/UpdateCheckHttpsTransport.cpp
    src/platform/update_check_https_transport_win32.cpp
    src/platform/update_check_https_transport_unsupported.cpp
    src/update/HarkenSearchUpdateComposition.cpp
    src/update/HarkenSearchUpdateFailureExplanation.cpp
    src/update/HarkenSearchUpdateNoticeUi.cpp
    src/update/HarkenSearchUpdateNoticeModel.cpp
    src/release_notes/ReleaseNotes.cpp
    src/bootstrap/HarkenSearchBootstrapPlan.cpp
    src/bootstrap/HarkenSearchBootstrapAdmission.cpp
    src/update/UpdateCoordinator.cpp
    src/update/UpdateDownloadManager.cpp
    src/update/UpdateHttpTransport.cpp
    src/update/UpdateRuntime.cpp
    src/update/UpdateStateStore.cpp
    src/platform/update_persistence_port.cpp
    src/platform/update_bootstrap_authority_unsupported.cpp
    src/platform/update_bootstrap_authority_base.cpp
    src/platform/update_bootstrap_authority_win32.cpp
    src/platform/update_deployment_identity_win32.cpp
    src/platform/update_bootstrap_health_port.cpp
    src/platform/update_staging_capacity_port.cpp
    src/platform/update_http_transport.cpp
    # Audio similarity search (F-028)
    src/FeatureExtractor.cpp
    src/SimilaritySearch.cpp
    src/SimilarityWorkflow.cpp
    src/SimilarityQueryService.cpp
    src/SimilarSoundsWindow.cpp
    src/RelinkWorkflow.cpp
    # BPM / key / mood detection (F-033)
    src/BpmDetector.cpp
    src/KeyDetector.cpp
    src/MoodAnalyzer.cpp
    src/MusicAnalyzer.cpp
    # Wwise Authoring API client (F-034) - S1 skeleton; S2 wires real WebSocket transport
    src/WaapiClient.cpp
    src/WaapiResponseParse.cpp
    # Send to Wwise (F-034 S3+S4) - config types + wildcards/template engine
    src/WwiseTransferConfig.cpp
    src/WwiseStructureTemplate.cpp
    # Send to Wwise (F-034 S5-1) - Preview Diff algorithm
    src/WwiseDiff.cpp
    # Send to Wwise (F-034 S5-2) - WAAPI response parser + Originals scanner
    src/WwiseQuery.cpp
    # Send to Wwise (F-034 S5-2) - WAAPI orchestrator (split from pure parser
    # so unit tests don't need to link ixwebsocket's Debug-ABI static lib).
    src/WwiseQueryClient.cpp
    # Send to Wwise (F-034 S3) - persistence hook (sibling JSON next to settings.ini)
    src/WwiseTransferPersist.cpp
    # Send to Wwise (F-034 S6) - transfer execution (WAV copy + WAAPI audio.import + Property Template)
    src/WwiseTransferExecute.cpp
    # Send to Wwise (F-034 S7) - Transfer Panel UI
    src/WwiseTransferPanel.cpp
    # Aris UI v1 (F-035) - shared widget library for all windows
    src/HarkenSearchIcons.cpp
    src/HarkenSearchWidgets.cpp
    src/HarkenSearchWidgetsFocus.cpp
    src/ui/CoreWorkspaceStyle.cpp
    src/ui/ModuleWindowShell.cpp
    src/frontend/AuditionFrontendController.cpp
    src/frontend/AuditionNativeIntentWorkflow.cpp
    src/frontend/AppStateAuditionFrontendAdapter.cpp
    src/ui/AuditionTransportView.cpp
    src/WelcomeGreeting.cpp
    # Acrylic Art Fader v5.1 (F-016b) - themed fader rendering
    src/ImGuiAcrylicFader.cpp
    # Custom avatar (F-026 / AUR-UI-SETTINGS-AVATAR-001)
    src/AvatarCropper.cpp
    src/AvatarCropperIO.cpp
    src/DockLayout.cpp
    src/DockDragSession.cpp
)

# Platform abstraction layer (F-031)
if(WIN32)
    set(AURO_EMERGENCY_FILE_WRITER_SOURCE src/platform/emergency_file_writer_win32.cpp)
    set(AURO_THREAD_ID_SOURCE src/platform/thread_id_win32.cpp)
    list(APPEND APP_SOURCES
        src/main.cpp
        src/WindowsMain.cpp
        src/DefaultAudioPlayerAssociation.cpp
        src/platform/filesystem_win32.cpp
        src/platform/floating_window_corners_win32.cpp
        src/platform/image_codec_win32.cpp
        src/platform/platform_win32.cpp
        src/platform/system_win32.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
        ${AURO_EMERGENCY_FILE_WRITER_SOURCE}
        ${AURO_THREAD_ID_SOURCE}
        src/platform/text_encoding_ascii.cpp
        src/platform/named_pipe_win32.cpp
        src/platform/text_encoding_win32.cpp
        src/platform/executable_path_win32.cpp
        src/platform/pe_inspection_win32.cpp
        src/platform/file_hash_win32.cpp
        src/platform/hmac_sha256_win32.cpp
        src/platform/process_launch_win32.cpp
        src/platform/process_query_win32.cpp
        src/platform/durable_file_ops_win32.cpp
    )
elseif(APPLE)
    set(AURO_EMERGENCY_FILE_WRITER_SOURCE src/platform/emergency_file_writer_std.cpp)
    set(AURO_THREAD_ID_SOURCE src/platform/thread_id_std.cpp)
    list(APPEND APP_SOURCES
        src/platform/image_codec_macos.mm
        src/platform/platform_macos.mm
        src/platform/system_macos.mm
        ${AURO_EMERGENCY_FILE_WRITER_SOURCE}
        ${AURO_THREAD_ID_SOURCE}
        src/platform/text_encoding_ascii.cpp
        src/platform/named_pipe_macos.mm
        src/platform/text_encoding_std.cpp
        src/platform/executable_path_macos.mm
        src/platform/pe_inspection_macos.mm
        src/platform/file_hash_macos.cpp
        src/platform/hmac_sha256_unsupported.cpp
        src/platform/process_launch_macos.cpp
        src/platform/process_query_unsupported.cpp
        src/platform/durable_file_ops_macos.cpp
        src/platform/app_entry_macos.mm
        src/platform/main_stubs_macos.cpp
    )
    # whisper.cpp and aubio are not available on macOS; use no-op stubs.
    list(REMOVE_ITEM APP_SOURCES src/AIEngine.cpp src/BpmDetector.cpp src/KeyDetector.cpp)
    list(APPEND APP_SOURCES src/AIEngineStub.cpp src/BpmDetectorStub.cpp src/KeyDetectorStub.cpp)
endif()


if(WIN32)
    add_executable(TestHarkenLegacyDataMigrator EXCLUDE_FROM_ALL
        tests/test_harken_legacy_data_migrator.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
    )
    target_include_directories(TestHarkenLegacyDataMigrator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestHarkenLegacyDataMigrator PRIVATE cxx_std_17)
    target_compile_options(TestHarkenLegacyDataMigrator PRIVATE /utf-8)
    target_link_libraries(TestHarkenLegacyDataMigrator PRIVATE shell32 ole32)
    add_test(NAME HarkenLegacyDataMigrator COMMAND TestHarkenLegacyDataMigrator)

    add_executable(TestFloatingPanelClampPolicy EXCLUDE_FROM_ALL tests/test_floating_panel_clamp_policy.cpp)
    target_include_directories(TestFloatingPanelClampPolicy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(TestFloatingPanelClampPolicy PRIVATE cxx_std_17)
    target_compile_options(TestFloatingPanelClampPolicy PRIVATE /utf-8)
    add_test(NAME FloatingPanelClampPolicy COMMAND TestFloatingPanelClampPolicy)
    add_executable(TestSettingsBodyScrollHeight EXCLUDE_FROM_ALL tests/test_settings_body_scroll_height.cpp)
    target_include_directories(TestSettingsBodyScrollHeight PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(TestSettingsBodyScrollHeight PRIVATE cxx_std_17)
    target_compile_options(TestSettingsBodyScrollHeight PRIVATE /utf-8)
    add_test(NAME SettingsBodyScrollHeight COMMAND TestSettingsBodyScrollHeight)
    add_test(NAME SettingsBodyScrollHeightSourceContract
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_settings_body_scroll_height.py")
    set_tests_properties(SettingsBodyScrollHeightSourceContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(TestDockLayoutTree EXCLUDE_FROM_ALL tests/test_dock_layout_tree.cpp src/DockLayout.cpp)
    target_include_directories(TestDockLayoutTree PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(TestDockLayoutTree PRIVATE cxx_std_17)
    target_compile_options(TestDockLayoutTree PRIVATE /utf-8)
    add_test(NAME DockLayoutTree COMMAND TestDockLayoutTree)
    add_test(NAME InfoPanelStartupLayout
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_info_panel_startup_layout.py
            --build-dir ${CMAKE_CURRENT_BINARY_DIR}/Testing/info-panel-startup)
    add_test(NAME NoDatabaseSeparator
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_no_database_separator.py
            --build-dir ${CMAKE_CURRENT_BINARY_DIR}/Testing/no-database-separator)
    add_executable(TestDockHitTest EXCLUDE_FROM_ALL tests/test_dock_hit_test.cpp src/DockLayout.cpp)
    target_include_directories(TestDockHitTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(TestDockHitTest PRIVATE cxx_std_17)
    target_compile_options(TestDockHitTest PRIVATE /utf-8)
    add_test(NAME DockHitTest COMMAND TestDockHitTest)
    # Isolated Dock frame replay: no product settings, database, or legal-flow bypass.
    add_executable(TestDockPerfReplay EXCLUDE_FROM_ALL
        tests/test_dock_hit_test.cpp src/DockLayout.cpp src/DockDragSession.cpp
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(TestDockPerfReplay PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_compile_features(TestDockPerfReplay PRIVATE cxx_std_17)
    target_compile_options(TestDockPerfReplay PRIVATE /utf-8)
    target_compile_definitions(TestDockPerfReplay PRIVATE DOCK_PERF_REPLAY=1)
    target_link_libraries(TestDockPerfReplay PRIVATE opengl32 gdi32 user32)

    add_executable(TestDockFrameLive EXCLUDE_FROM_ALL
        tests/test_dock_frame_live.cpp
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(TestDockFrameLive PRIVATE src ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_compile_features(TestDockFrameLive PRIVATE cxx_std_17)
    target_compile_options(TestDockFrameLive PRIVATE /utf-8)
    target_link_libraries(TestDockFrameLive PRIVATE opengl32 gdi32 user32)
    add_test(NAME DockFrameLive COMMAND TestDockFrameLive)
    add_executable(TestDetachedViewportInput EXCLUDE_FROM_ALL
        tests/test_detached_viewport_input_win32.cpp
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
    target_include_directories(TestDetachedViewportInput PRIVATE src ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_compile_features(TestDetachedViewportInput PRIVATE cxx_std_17)
    target_compile_options(TestDetachedViewportInput PRIVATE /utf-8)
    target_link_libraries(TestDetachedViewportInput PRIVATE gdi32 user32 dwmapi)
    add_test(NAME DetachedViewportInput COMMAND TestDetachedViewportInput)
    set_tests_properties(DetachedViewportInput PROPERTIES LABELS "dock" TIMEOUT 30)
    add_executable(TestDockDragSession EXCLUDE_FROM_ALL tests/test_dock_drag_session.cpp src/DockLayout.cpp src/DockDragSession.cpp)
    target_include_directories(TestDockDragSession PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(TestDockDragSession PRIVATE cxx_std_17)
    target_compile_options(TestDockDragSession PRIVATE /utf-8)
    add_test(NAME DockDragSession COMMAND TestDockDragSession)
    add_executable(TestDockNativeCaptionBridge EXCLUDE_FROM_ALL tests/test_dock_native_caption_bridge.cpp)
    target_include_directories(TestDockNativeCaptionBridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(TestDockNativeCaptionBridge PRIVATE cxx_std_17)
    target_compile_options(TestDockNativeCaptionBridge PRIVATE /utf-8)
    add_test(NAME DockNativeCaptionBridge COMMAND TestDockNativeCaptionBridge)
    set_tests_properties(DockLayoutTree DockHitTest DockDragSession DockNativeCaptionBridge
        PROPERTIES LABELS "dock")
    add_executable(TestViewportGlLifecycle EXCLUDE_FROM_ALL tests/test_viewport_gl_lifecycle.cpp)
    target_include_directories(TestViewportGlLifecycle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(TestViewportGlLifecycle PRIVATE cxx_std_17)
    target_compile_options(TestViewportGlLifecycle PRIVATE /utf-8)
    add_test(NAME ViewportGlLifecycle COMMAND TestViewportGlLifecycle)
    add_test(NAME TestImguiDockingViewportContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_imgui_docking_viewport_contract.py")
    set_tests_properties(TestImguiDockingViewportContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(TestPreviewIsolationGuard EXCLUDE_FROM_ALL
        tests/test_preview_isolation_guard.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
    )
    target_include_directories(TestPreviewIsolationGuard PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestPreviewIsolationGuard PRIVATE cxx_std_17)
    target_compile_definitions(TestPreviewIsolationGuard PRIVATE
        ARIS_PREVIEW_ISOLATION=1
        ARIS_PREVIEW_ISOLATION_ROOT_UTF8="${CMAKE_CURRENT_BINARY_DIR}/Testing/preview-isolation-guard")
    target_compile_options(TestPreviewIsolationGuard PRIVATE /utf-8)
    target_link_libraries(TestPreviewIsolationGuard PRIVATE shell32 ole32)
    add_test(NAME PreviewIsolationGuard COMMAND TestPreviewIsolationGuard)

    add_executable(TestSearchTextIndex EXCLUDE_FROM_ALL
        tests/test_search_text_index.cpp
        ${AURO_SEARCH_TEXT_GB2312_SOURCE}
    )
    target_include_directories(TestSearchTextIndex PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestSearchTextIndex PRIVATE cxx_std_17)
    target_compile_options(TestSearchTextIndex PRIVATE /utf-8)
    add_test(NAME SearchTextIndex COMMAND TestSearchTextIndex)
    aura_set_test_labels(SearchTextIndex core)
endif()

add_executable(TestUpdateNoticePresentation EXCLUDE_FROM_ALL
    tests/test_update_notice_presentation.cpp
)
target_include_directories(TestUpdateNoticePresentation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${IMGUI_DIR}
)
target_compile_features(TestUpdateNoticePresentation PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdateNoticePresentation PRIVATE /utf-8)
endif()
add_test(NAME UpdateNoticePresentation COMMAND TestUpdateNoticePresentation)

add_executable(TestUpdateNoticeContent EXCLUDE_FROM_ALL
    tests/test_update_notice_content.cpp
    src/update/HarkenSearchUpdateNoticeModel.cpp
    src/release_notes/ReleaseNotes.cpp)
target_include_directories(TestUpdateNoticeContent PRIVATE src thirdparty)
target_compile_features(TestUpdateNoticeContent PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdateNoticeContent PRIVATE /utf-8)
endif()
add_test(NAME UpdateNoticeContent COMMAND TestUpdateNoticeContent)

if(WIN32)
    add_executable(TestUpdateReadyReleaseNotes EXCLUDE_FROM_ALL
        tests/test_update_ready_release_notes.cpp
        src/update/UpdateDownloadManager.cpp src/update/UpdateHttpTransport.cpp
        src/platform/update_persistence_port.cpp src/platform/update_bootstrap_authority_base.cpp
        src/platform/text_encoding_win32.cpp src/platform/text_encoding_ascii.cpp)
    target_include_directories(TestUpdateReadyReleaseNotes PRIVATE src thirdparty)
    target_compile_features(TestUpdateReadyReleaseNotes PRIVATE cxx_std_17)
    target_compile_definitions(TestUpdateReadyReleaseNotes PRIVATE
        ARIS_UPDATE_PERSISTENCE_TESTING=1 ARIS_UPDATE_RELEASE_NOTES=1)
    target_compile_options(TestUpdateReadyReleaseNotes PRIVATE /W3 /utf-8)
    target_link_libraries(TestUpdateReadyReleaseNotes PRIVATE bcrypt shell32 ole32)
endif()

if(WIN32)
    add_executable(HarkenSearchUpdateNoticeNativeQa WIN32 EXCLUDE_FROM_ALL
        tests/test_update_notice_presentation.cpp
        src/update/HarkenSearchUpdateNoticeUi.cpp
        src/update/HarkenSearchUpdateNoticeModel.cpp
        src/release_notes/ReleaseNotes.cpp
        src/update/HarkenSearchUpdateFailureExplanation.cpp
        src/Theme.cpp
        src/DiagnosticsLoggerStub.cpp
        src/platform/filesystem_std.cpp
        src/platform/system_win32.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
        src/platform/text_encoding_win32.cpp
        src/ui/CoreWorkspaceStyle.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(HarkenSearchUpdateNoticeNativeQa PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_compile_definitions(HarkenSearchUpdateNoticeNativeQa PRIVATE
        ARIS_UPDATE_NOTICE_NATIVE_QA=1 AURA_BUILD_DIAGNOSTICS=0)
    target_compile_options(HarkenSearchUpdateNoticeNativeQa PRIVATE /utf-8)
    target_link_libraries(HarkenSearchUpdateNoticeNativeQa PRIVATE opengl32 gdi32 user32)
endif()

# GPL boundary (see AURO_ENABLE_AUBIO_BPM above): swap BPM impl for the no-op
# stub on non-Apple builds when aubio is disabled. macOS already stubs above.
if(NOT AURO_ENABLE_AUBIO_BPM AND NOT APPLE)
    list(REMOVE_ITEM APP_SOURCES src/BpmDetector.cpp)
    list(APPEND APP_SOURCES src/BpmDetectorStub.cpp)
endif()

# 诊断模块：开发构建用完整实现，上线用 Stub + DevTools 头文件内联空实现
list(APPEND APP_SOURCES src/DiagnosticFileLifecycle.cpp)
if(AURA_BUILD_DIAGNOSTICS)
    list(APPEND APP_SOURCES src/ScanDatabaseOpenDiagnostics.cpp src/DiagnosticsLogger.cpp src/DevToolsModule.cpp src/GMPanel.cpp src/UserActionLog.cpp src/DevToolsHarkenSearchSmoke.cpp src/FrameBudgetDiagnostics.cpp)
else()
    list(APPEND APP_SOURCES src/DiagnosticsLoggerStub.cpp src/UserActionLogStub.cpp)
endif()

# Windows resource file (for application icon)
set(RESOURCE_FILES
    resources.rc
)

# Create executable (WIN32 for Windows GUI app without console; MACOSX_BUNDLE for macOS)
if(WIN32)
    add_executable(HarkenSearch WIN32
        ${APP_SOURCES}
        ${IMGUI_SOURCES}
        ${SQLITE_SOURCES}
        ${RESOURCE_FILES}
    )
elseif(APPLE)
    add_executable(HarkenSearch MACOSX_BUNDLE
        ${APP_SOURCES}
        ${IMGUI_SOURCES}
        ${SQLITE_SOURCES}
    )
else()
    add_executable(HarkenSearch
        ${APP_SOURCES}
        ${IMGUI_SOURCES}
        ${SQLITE_SOURCES}
    )
endif()

set_target_properties(HarkenSearch PROPERTIES OUTPUT_NAME HarkenSearch)
target_compile_definitions(HarkenSearch PRIVATE ARIS_UPDATE_RELEASE_NOTES=1)

if(ARIS_UPDATE_NOTICE_QA)
    target_compile_definitions(HarkenSearch PRIVATE ARIS_UPDATE_NOTICE_QA=1)
endif()

# Pure transaction core shared by the future independent Bootstrap executable
# and its contract tests. Platform/process/filesystem adapters are deliberately
# not part of this target.
add_library(HarkenSearchBootstrapCore STATIC EXCLUDE_FROM_ALL
    src/bootstrap/HarkenSearchBootstrapPlan.cpp
)
set_target_properties(HarkenSearchBootstrapCore PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
)
target_include_directories(HarkenSearchBootstrapCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(HarkenSearchBootstrapCore PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(HarkenSearchBootstrapCore PRIVATE /W4 /WX /EHsc /permissive- /utf-8)
endif()
set_target_properties(HarkenSearchBootstrapCore PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/internal-archive"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/internal-archive/Debug"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/internal-archive/Release"
)

# L1 link base (RQ-20260911-04): engine TUs (Database/AudioEngine/AIEngine/
# ReaperBridgeDoctor) reference GetLocalizedString; a single static lib cannot
# serve both the product (MD runtime, line ~1071) and the MDd test default
# (LNK2038), so the unit is compiled PER TARGET — byte-equivalent to the former
# in-product placement by construction. Auto-application is the engine-TU pass
# at the end of this file; per-target manual wiring is contract-forbidden.

# Independent Windows Bootstrap composition.  The package verification key is
# deliberately an explicit build input: ordinary developer builds remain
# unprovisioned and reject every production handoff, while release packaging
# must provide both values.  No fallback/test key is embedded here.
if(WIN32)
    option(ARIS_BOOTSTRAP_RELEASE_BUILD
        "Require release-provisioned package signing key for HarkenSearchBootstrap" OFF)
    set(ARIS_BOOTSTRAP_PACKAGE_KEY_ID "" CACHE STRING
        "Release-injected Ed25519 package key id for HarkenSearchBootstrap")
    set(ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX "" CACHE STRING
        "Release-injected Ed25519 package public key (64 lowercase hex chars)")
    if((ARIS_BOOTSTRAP_PACKAGE_KEY_ID AND
        NOT ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX) OR
       (ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX AND
        NOT ARIS_BOOTSTRAP_PACKAGE_KEY_ID))
        message(FATAL_ERROR
            "HarkenSearchBootstrap package key id and public key must be supplied together")
    endif()
    if((ARIS_BOOTSTRAP_RELEASE_BUILD OR ARIS_PUBLIC_RELEASE) AND
       (NOT ARIS_BOOTSTRAP_PACKAGE_KEY_ID OR
        NOT ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX))
        message(FATAL_ERROR
            "HarkenSearchBootstrap release builds require injected package key id and public key")
    endif()
    if(ARIS_UPDATE_LOCAL_DEMO AND
       (NOT ARIS_BOOTSTRAP_PACKAGE_KEY_ID OR
        NOT ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX))
        message(FATAL_ERROR
            "ARIS_UPDATE_LOCAL_DEMO requires an externally provisioned package key identity")
    endif()
    if(ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
        string(LENGTH "${ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX}" _aris_bootstrap_key_len)
        if(NOT _aris_bootstrap_key_len EQUAL 64 OR
           NOT ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX MATCHES "^[0-9a-f]+$" OR
           ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX MATCHES "^0+$")
            message(FATAL_ERROR
                "ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX must be a nonzero 32-byte lowercase hex key")
        endif()
        # TweetNaCl's verification primitive does not itself reject every
        # low-order/non-canonical Ed25519 public-key encoding.  Provisioning
        # is therefore fail-closed against the complete small-order blacklist;
        # runtime admission applies the same check before verification.
        set(_aris_bootstrap_ed25519_small_order_keys
            "0000000000000000000000000000000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000080"
            "0100000000000000000000000000000000000000000000000000000000000000"
            "0100000000000000000000000000000000000000000000000000000000000080"
            "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800"
            "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880"
            "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eaa78aad612"
            "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eaa78aad692"
            "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"
            "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
            "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"
            "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
            "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"
            "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
        )
        list(FIND _aris_bootstrap_ed25519_small_order_keys
            "${ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX}"
            _aris_bootstrap_small_order_key_index)
        # Compressed Edwards y is little-endian with the x sign in bit 255.
        # Reject y >= 2^255-19 without relying on unsupported regex intervals.
        string(SUBSTRING "${ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX}" 0 2
            _aris_bootstrap_key_y_low)
        string(SUBSTRING "${ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX}" 2 60
            _aris_bootstrap_key_y_middle)
        string(SUBSTRING "${ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX}" 62 2
            _aris_bootstrap_key_y_high_with_sign)
        math(EXPR _aris_bootstrap_key_y_high
            "0x${_aris_bootstrap_key_y_high_with_sign} & 0x7f")
        set(_aris_bootstrap_noncanonical_y FALSE)
        if(_aris_bootstrap_key_y_high EQUAL 127 AND
           _aris_bootstrap_key_y_middle MATCHES "^f+$" AND
           NOT _aris_bootstrap_key_y_low STRLESS "ed")
            set(_aris_bootstrap_noncanonical_y TRUE)
        endif()
        if(NOT _aris_bootstrap_small_order_key_index EQUAL -1 OR
           _aris_bootstrap_noncanonical_y)
            message(FATAL_ERROR
                "ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX must be a strict Ed25519 public key")
        endif()
        string(LENGTH "${ARIS_BOOTSTRAP_PACKAGE_KEY_ID}" _aris_bootstrap_key_id_len)
        if(_aris_bootstrap_key_id_len LESS 1 OR
           _aris_bootstrap_key_id_len GREATER 64 OR
           NOT ARIS_BOOTSTRAP_PACKAGE_KEY_ID MATCHES "^[A-Za-z0-9_.-]+$")
            message(FATAL_ERROR
                "ARIS_BOOTSTRAP_PACKAGE_KEY_ID contains unsupported characters")
        endif()
    endif()

    add_executable(HarkenSearchBootstrap WIN32
        src/bootstrap/HarkenSearchBootstrapMain.cpp
        src/bootstrap/HarkenSearchBootstrapAdmission.cpp
        src/bootstrap/HarkenSearchBootstrapExitExplanation.cpp
        src/bootstrap/HarkenSearchBootstrapRuntime.cpp
        src/bootstrap/WindowsBootstrapFilesystem.cpp
        src/bootstrap/WindowsBootstrapFilesystemWin32.cpp
        src/bootstrap/WindowsBootstrapJournal.cpp
        src/bootstrap/WindowsBootstrapRuntimeWin32.cpp
        src/platform/update_bootstrap_authority_base.cpp
        src/platform/update_persistence_port.cpp
        src/platform/text_encoding_win32.cpp
        src/ed25519/tweetnacl.c
    )
    set_target_properties(HarkenSearchBootstrap PROPERTIES OUTPUT_NAME HarkenSearchBootstrap)
    target_include_directories(HarkenSearchBootstrap PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    )
    target_compile_features(HarkenSearchBootstrap PRIVATE cxx_std_17)
    target_link_libraries(HarkenSearchBootstrap PRIVATE
        HarkenSearchBootstrapCore
        advapi32
        crypt32
        shell32
        ole32
        user32
        wintrust
        bcrypt
    )
    if(MSVC)
        target_compile_options(HarkenSearchBootstrap PRIVATE /W4 /WX /EHsc /permissive- /utf-8)
    endif()
    # Multi-config generators select Release at build time.  Keep ordinary
    # developer Debug builds unprovisioned, but make an unprovisioned Release
    # artifact fail closed even when configure was run without the explicit
    # release option.
    target_compile_definitions(HarkenSearchBootstrap PRIVATE
        $<$<CONFIG:Release>:ARIS_BOOTSTRAP_RELEASE_CONFIG=1>
    )
    if(ARIS_BOOTSTRAP_PACKAGE_KEY_ID)
        target_compile_definitions(HarkenSearchBootstrap PRIVATE
            ARIS_BOOTSTRAP_PACKAGE_KEY_ID="${ARIS_BOOTSTRAP_PACKAGE_KEY_ID}"
            ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX="${ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX}"
        )
        if(ARIS_UPDATE_LOCAL_DEMO)
            target_compile_definitions(HarkenSearchBootstrap PRIVATE
                ARIS_UPDATE_LOCAL_DEMO=1
                ARIS_UPDATE_LOCAL_DEMO_ROOT="${_aris_demo_root_normalized}"
            )
        endif()

        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        execute_process(
            COMMAND git rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE ARIS_RUNTIME_PAIR_SOURCE_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY
        )
        set(ARIS_RUNTIME_PAIR_HEADER
            "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/HarkenSearchRuntimePairIdentity.h")
        add_custom_command(
            OUTPUT "${ARIS_RUNTIME_PAIR_HEADER}"
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_runtime_pair.py"
                bootstrap-header
                --bootstrap "$<TARGET_FILE:HarkenSearchBootstrap>"
                --output "${ARIS_RUNTIME_PAIR_HEADER}"
                --source-commit "${ARIS_RUNTIME_PAIR_SOURCE_COMMIT}"
                --build-id "${AIRS_BUILD_ID}"
                --channel "${AIRS_RELEASE_CHANNEL}"
                --config "$<CONFIG>"
                --key-id "${ARIS_BOOTSTRAP_PACKAGE_KEY_ID}"
                --public-key-hex "${ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX}"
            DEPENDS HarkenSearchBootstrap
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_runtime_pair.py"
            COMMENT "Generating config-specific Aris runtime-pair identity..."
            VERBATIM
        )
        add_custom_target(HarkenSearchRuntimePairIdentity DEPENDS "${ARIS_RUNTIME_PAIR_HEADER}")
    endif()
endif()

# Release-only million-record similarity performance gate. Keep this explicit
# target out of the default build and ordinary CTest suite.
add_executable(SimilarityBench EXCLUDE_FROM_ALL
    tools/similarity_bench.cpp
    src/SimilaritySearch.cpp
    tests/test_similarity_search_database_stub.cpp
)
if(WIN32)
    target_sources(SimilarityBench PRIVATE src/platform/text_encoding_win32.cpp)
else()
    target_sources(SimilarityBench PRIVATE src/platform/text_encoding_std.cpp)
endif()
target_include_directories(SimilarityBench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/sqlite
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui
)
target_link_libraries(SimilarityBench PRIVATE Threads::Threads)
if(MSVC)
    set_property(TARGET SimilarityBench PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreadedDLL")
    target_compile_options(SimilarityBench PRIVATE /W3 /utf-8)
endif()

target_compile_definitions(HarkenSearch PRIVATE
    AURA_BUILD_DIAGNOSTICS=$<IF:$<BOOL:${AURA_BUILD_DIAGNOSTICS}>,1,0>
    AURA_INTERNAL_BETA=$<IF:$<BOOL:${AURA_INTERNAL_BETA}>,1,0>
    AURA_EDITION=${AURA_EDITION}
    ARIS_ENABLE_IMGUI_VIEWPORTS=$<IF:$<BOOL:${ARIS_ENABLE_IMGUI_VIEWPORTS}>,1,0>
    ARIS_UPDATE_GITHUB_REPOSITORY=\"${ARIS_UPDATE_GITHUB_REPOSITORY}\"
    ARIS_UPDATE_MIRROR_BASE_URL=\"${ARIS_UPDATE_MIRROR_BASE_URL}\"
    ARIS_UPDATE_LOCAL_DEMO=$<IF:$<BOOL:${ARIS_UPDATE_LOCAL_DEMO}>,1,0>
)
if(ARIS_UPDATE_QA_CLI)
    target_compile_definitions(HarkenSearch PRIVATE ARIS_UPDATE_QA_CLI=1)
endif()
if(ARIS_UPDATE_LOCAL_DEMO)
    target_compile_definitions(HarkenSearch PRIVATE
        ARIS_UPDATE_LOCAL_DEMO_BASE_URL=\"${ARIS_UPDATE_LOCAL_DEMO_BASE_URL}\"
        ARIS_UPDATE_LOCAL_DEMO_OLD_VERSION=\"${ARIS_UPDATE_LOCAL_DEMO_OLD_VERSION}\"
        ARIS_UPDATE_LOCAL_DEMO_NEW_VERSION=\"${ARIS_UPDATE_LOCAL_DEMO_NEW_VERSION}\"
        ARIS_UPDATE_LOCAL_DEMO_OLD_BUILD_ID=\"${ARIS_UPDATE_LOCAL_DEMO_OLD_BUILD_ID}\"
        ARIS_UPDATE_LOCAL_DEMO_NEW_BUILD_ID=\"${ARIS_UPDATE_LOCAL_DEMO_NEW_BUILD_ID}\"
        ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION=\"${ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION}\"
        ARIS_UPDATE_LOCAL_DEMO_BUILD_ID=\"${ARIS_UPDATE_LOCAL_DEMO_BUILD_ID}\"
        ARIS_UPDATE_LOCAL_DEMO_ROOT=\"${_aris_demo_root_normalized}\"
    )
endif()

# Debug compatibility with third-party prebuilt Release libs:
# - Force /MD (not /MDd) in Debug to avoid RuntimeLibrary mismatch (LNK2038).
# - Force STL iterator debug level 0 in Debug to match prebuilt libs.
    # Release may use /MT for deployment without a CRT DLL; otherwise keep /MD.
if(MSVC)
    if(AURA_RELEASE_STATIC_CRT)
        set_property(TARGET HarkenSearch PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    else()
        set_property(TARGET HarkenSearch PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()
    target_compile_definitions(HarkenSearch PRIVATE
        $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>
    )
endif()

# REAPER extension DLL (reaper_harken.dll): Named Pipe bridge for "Send to DAW".
# Build explicitly: cmake --build build_dbg --config Debug --target reaper_harken
if(WIN32)
    add_subdirectory(reaper_extension)
    add_dependencies(HarkenSearch reaper_harken)
endif()

set(AURO_PROTOOLS_PTSL_SDK_ROOT "" CACHE PATH "Optional local Avid Pro Tools Scripting SDK root or built PTSLC_CPP drop folder; not vendored")
set(AURO_PROTOOLS_PTSL_DROP_DIR "${CMAKE_BINARY_DIR}/Debug/ProToolsSdk" CACHE PATH "Local built PTSLC_CPP drop folder used by HarkenSearchProToolsBridge")
if(WIN32)
    add_executable(HarkenSearchProToolsBridge
        src/HarkenSearchProToolsBridgeMain.cpp
        src/ProToolsBridge.cpp
        ${AURO_PROTOOLS_PATH_ENCODING_SOURCE}
        src/platform/filesystem_std.cpp
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
        src/platform/executable_path_win32.cpp
        src/platform/process_launch_win32.cpp
        src/platform/process_query_win32.cpp
        src/platform/system_win32.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
    )
    target_include_directories(HarkenSearchProToolsBridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    set(AURO_PROTOOLS_PTSL_EFFECTIVE_ROOT "")
    if(AURO_PROTOOLS_PTSL_SDK_ROOT)
        set(AURO_PROTOOLS_PTSL_EFFECTIVE_ROOT "${AURO_PROTOOLS_PTSL_SDK_ROOT}")
    elseif(EXISTS "${AURO_PROTOOLS_PTSL_DROP_DIR}/PTSLC_CPP.lib" AND EXISTS "${AURO_PROTOOLS_PTSL_DROP_DIR}/include/PTSLC_CPP/CppPTSLClient.h")
        set(AURO_PROTOOLS_PTSL_EFFECTIVE_ROOT "${AURO_PROTOOLS_PTSL_DROP_DIR}")
    endif()
    if(AURO_PROTOOLS_PTSL_EFFECTIVE_ROOT)
        include(cmake/ptsl_sdk_layout.cmake)
        aris_resolve_ptsl_sdk("${AURO_PROTOOLS_PTSL_EFFECTIVE_ROOT}"
            AURO_PROTOOLS_PTSL_INCLUDE AURO_PROTOOLS_PTSL_LIBRARY AURO_PROTOOLS_PTSL_RUNTIME)
        target_compile_definitions(HarkenSearchProToolsBridge PRIVATE AURO_PROTOOLS_PTSL_ENABLED=1)
        target_include_directories(HarkenSearchProToolsBridge PRIVATE "${AURO_PROTOOLS_PTSL_INCLUDE}")
        target_link_libraries(HarkenSearchProToolsBridge PRIVATE "${AURO_PROTOOLS_PTSL_LIBRARY}")
        if(AURO_BUNDLE_AVID_PTSL)
            if(NOT EXISTS "${AURO_PROTOOLS_PTSL_RUNTIME}")
                message(FATAL_ERROR "PTSL SDK runtime DLL is missing")
            endif()
            add_custom_command(TARGET HarkenSearchProToolsBridge POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${AURO_PROTOOLS_PTSL_RUNTIME}"
                    "$<TARGET_FILE_DIR:HarkenSearchProToolsBridge>/PTSLC_CPP.dll")
        endif()
    endif()
    if(MSVC)
        target_compile_options(HarkenSearchProToolsBridge PRIVATE /W3 /utf-8)
        # Avid PTSLC_CPP.dll is a dynamic SDK wrapper. Its README warns that the
        # client executable must use the same MSVC CRT as the wrapper; the local
        # SDK drop is built with the shared release CRT, so keep this helper on
        # /MD even from Aris's Debug build to avoid allocator/CRT boundary
        # failures during CppPTSLClient construction.
        set_property(TARGET HarkenSearchProToolsBridge PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        set_target_properties(HarkenSearchProToolsBridge PROPERTIES
            OUTPUT_NAME HarkenSearchProToolsBridge
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        )
    endif()
endif()

add_dependencies(HarkenSearch ArisLocalizationGenerated)
target_include_directories(HarkenSearch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SQLITE_DIR}
    # Whisper.cpp includes
    ${WHISPER_DIR}/include
    ${WHISPER_DIR}/ggml/include
    # WebView2 SDK includes
    ${WEBVIEW2_DIR}/include
)
if(ARIS_BOOTSTRAP_PACKAGE_KEY_ID AND ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
    target_compile_definitions(HarkenSearch PRIVATE
        ARIS_BOOTSTRAP_PACKAGE_KEY_ID="${ARIS_BOOTSTRAP_PACKAGE_KEY_ID}"
        ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX="${ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX}"
        ARIS_RUNTIME_PAIR_PROVISIONED=1
    )
    target_include_directories(HarkenSearch PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")
    add_dependencies(HarkenSearch HarkenSearchRuntimePairIdentity)
    add_dependencies(HarkenSearch HarkenSearchBootstrap)
endif()
if(AURO_ENABLE_AUBIO_BPM AND WIN32)
    # aubio: BPM and onset detection (F-033), GPL-3 (see AURO_ENABLE_AUBIO_BPM)
    target_include_directories(HarkenSearch PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/aubio/include)
endif()
# Whisper.cpp libraries
# This target links static libraries and does not require Whisper/GGML runtime DLLs.
set(WHISPER_LIBS
    ${WHISPER_BUILD_DIR}/src/Release/whisper.lib
    ${WHISPER_BUILD_DIR}/ggml/src/Release/ggml.lib
    ${WHISPER_BUILD_DIR}/ggml/src/Release/ggml-base.lib
    ${WHISPER_BUILD_DIR}/ggml/src/Release/ggml-cpu.lib
)

# Platform-specific libraries (F-031)
if(WIN32)
    target_link_libraries(HarkenSearch PRIVATE
        Threads::Threads
        opengl32
        gdi32
        dwmapi
        ole32
        windowscodecs
        mfplat
        mfreadwrite
        mfuuid
        uuid
        shell32
        advapi32
        comdlg32
        version     # platform/pe_inspection_win32.cpp: GetFileVersionInfoW
        bcrypt      # REAPER bridge SHA-256 verification
        wininet     # For model download
        # Whisper.cpp
        ${WHISPER_LIBS}
        shlwapi
        # MiniDump crash handler
        dbghelp
        # F-034 Wwise WAAPI client
        ixwebsocket
    )
elseif(APPLE)
    target_link_libraries(HarkenSearch PRIVATE
        Threads::Threads
        "-framework Cocoa"
        "-framework CoreServices"      # FSEvents
        "-framework OpenGL"
        "-framework ImageIO"           # Image encode/decode
        "-framework IOKit"             # Machine UUID
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework GameController"    # imgui_impl_osx uses GCController
        # macOS builds omit Whisper/aubio until compatible libraries are supplied.
    )
else()
    target_link_libraries(HarkenSearch PRIVATE Threads::Threads)
endif()

if(AURO_ENABLE_AUBIO_BPM AND WIN32)
    target_link_libraries(HarkenSearch PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/aubio/lib/aubio.lib)
endif()

# Ensure the Pro Tools bridge helper is built alongside Aris on Windows.
if(WIN32)
    add_dependencies(HarkenSearch HarkenSearchProToolsBridge)
endif()

# Compiler definitions
target_compile_definitions(HarkenSearch PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    # Require FTS5 in the bundled sqlite amalgamation
    SQLITE_ENABLE_FTS5
)

# MSVC specific settings
if(MSVC)
    # /utf-8 for UTF-8 source files with Chinese characters
    # /openmp for OpenMP support (required by whisper.cpp)
    # /EHa lets catch(...) handle SEH faults and is required for
    #        robust scanning of potentially corrupted audio files in worker threads.
    # /GR- disables RTTI and prevents class-name recovery from vtables; the codebase has no dynamic_cast.
    target_compile_options(HarkenSearch PRIVATE /W3 /MP /utf-8 /openmp /EHa /GR- /bigobj)
    # Optionally promote selected unused-code warnings to W1 without enabling all /W4 warnings.
    # Follow-up cleanup is tracked by the SLIM-Phase1 cards.
    if(AURA_SLIM_WARN_UNUSED)
        target_compile_options(HarkenSearch PRIVATE
            /w14100   # C4100: unreferenced formal parameter
            /w14189   # C4189: local variable initialized but not referenced
            /w14505   # C4505: unreferenced function with internal linkage has been removed
            /w14702   # C4702: unreachable code
        )
    endif()
    # Disable console window
    set_target_properties(HarkenSearch PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:WINDOWS"
    )
    # Release hardening: strip symbols, enable whole-program optimization, and protect control flow.
    if(AURA_RELEASE_HARDEN)
        target_compile_options(HarkenSearch PRIVATE
            $<$<CONFIG:Release>:/GL /guard:cf>
        )
        target_link_options(HarkenSearch PRIVATE
            $<$<CONFIG:Release>:/LTCG /DEBUG:NONE /guard:cf>
        )
    endif()
endif()

# Keep the Windows process manifest in the final PE.  Aris installs under a
# protected directory, so the product must request elevation at launch; this
# same manifest also carries the DPI-awareness declarations.
if(MSVC AND WIN32)
    if(ARIS_PREVIEW_ISOLATION)
        set(_aris_manifest "${CMAKE_CURRENT_SOURCE_DIR}/resources/HarkenSearch.preview.manifest")
    else()
        set(_aris_manifest "${CMAKE_CURRENT_SOURCE_DIR}/resources/HarkenSearch.manifest")
    endif()
    target_link_options(HarkenSearch PRIVATE
        "/MANIFESTUAC:NO"
        "/MANIFESTINPUT:${_aris_manifest}"
    )
endif()

# Copy resources (if any)
# add_custom_command(TARGET HarkenSearch POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy_directory
#     ${CMAKE_CURRENT_SOURCE_DIR}/resources
#     $<TARGET_FILE_DIR:HarkenSearch>/resources
# )

# Optional pre-build step closes a running HarkenSearch.exe so the linker can replace it without LNK1104.
option(AURA_BUILD_CLOSE_RUNNING_AURO "Force-close running HarkenSearch.exe before linking" OFF)
if(WIN32 AND AURA_BUILD_CLOSE_RUNNING_AURO)
    add_custom_command(TARGET HarkenSearch PRE_BUILD
        COMMAND cmd /c "taskkill /IM HarkenSearch.exe /F >nul 2>&1 & exit 0"
        COMMENT "Pre-build: closing HarkenSearch.exe because AURA_BUILD_CLOSE_RUNNING_AURO=ON"
    )
endif()

# Post-build: create the models directory and copy WebView2, Whisper/GGML, and rlottie runtime DLLs.
if(WIN32)
    set(_aris_bootstrap_runtime_args)
    if(ARIS_BOOTSTRAP_PACKAGE_KEY_ID AND ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
        list(APPEND _aris_bootstrap_runtime_args
            "-DBOOTSTRAP_EXE=$<TARGET_FILE:HarkenSearchBootstrap>")
    endif()
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DOUTDIR=$<TARGET_FILE_DIR:HarkenSearch>"
            "-DWEBVIEW2_DLL=${WEBVIEW2_DIR}/x64/WebView2Loader.dll"
            "-DRLOTTIE_BIN=${RLOTTIE_BIN_DIR}"
            "-DRLOTTIE_IMG=${RLOTTIE_IMG_DIR}"
            ${_aris_bootstrap_runtime_args}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_harken_search_runtime_deps.cmake"
        COMMENT "Copying WebView2Loader.dll..."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        VERBATIM
    )
else()
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:HarkenSearch>/models")
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WEBVIEW2_DIR}/x64/WebView2Loader.dll" "$<TARGET_FILE_DIR:HarkenSearch>/WebView2Loader.dll"
        COMMENT "Copying WebView2Loader.dll...")
endif()

# F-035 T4: copy Lucide icon font next to HarkenSearch.exe so release builds can
# find it at runtime (dev builds also find it via the project-relative path
# in MergeLucideFont's fallback list).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/lucide/lucide.ttf")
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/lucide/lucide.ttf"
            "$<TARGET_FILE_DIR:HarkenSearch>/lucide.ttf"
        COMMENT "Copying lucide.ttf...")
endif()

# F-035 T5: copy brand icon PNGs next to HarkenSearch.exe. Search icons are derived
# from user-supplied PNGs; see assets/icons/source/harken-app-icons/README.md.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/brand")
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/brand"
            "$<TARGET_FILE_DIR:HarkenSearch>/assets/icons/brand"
        COMMENT "Copying brand icons...")
endif()

# Copy brand logo lockups next to HarkenSearch.exe. These are larger wordmark assets
# used by splash/about/welcome surfaces and are kept separate from the small
# 20px/40px toolbar brand icons above.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/logos/brand")
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/logos/brand"
            "$<TARGET_FILE_DIR:HarkenSearch>/assets/logos/brand"
        COMMENT "Copying brand logos...")
endif()

# Copy local startup/user avatar pool. Startup WebView must not depend on the
# public DiceBear API, so generated SVG avatars are shipped beside HarkenSearch.exe.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/avatars")
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/avatars"
            "$<TARGET_FILE_DIR:HarkenSearch>/assets/avatars"
        COMMENT "Copying local avatar assets...")
endif()

# Copy beta splash image to output directory
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/splash_beta.png")
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/splash_beta.png"
            "$<TARGET_FILE_DIR:HarkenSearch>/splash_beta.png"
        COMMENT "Copying splash_beta.png...")
endif()

if(WIN32)
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:reaper_harken>"
            "$<TARGET_FILE_DIR:HarkenSearch>/reaper_harken.dll"
        COMMENT "Copying reaper_harken.dll beside HarkenSearch.exe..."
        VERBATIM
    )
endif()

set(ARIS_RELEASE_METADATA_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_COMPONENTS.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/ASSET_PROVENANCE.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/BINARY_PROVENANCE.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/PRIVACY.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/AI_FEATURE_DISCLOSURE.md"
)

foreach(_meta IN LISTS ARIS_RELEASE_METADATA_FILES)
    if(EXISTS "${_meta}")
        add_custom_command(TARGET HarkenSearch POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_meta}" "$<TARGET_FILE_DIR:HarkenSearch>/"
            COMMENT "Copying release metadata: ${_meta}")
    endif()
endforeach()

if(Python3_Interpreter_FOUND)
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_third_party_sbom.py"
            --manifest "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_COMPONENTS.json"
            --output "$<TARGET_FILE_DIR:HarkenSearch>/third-party-sbom.cdx.json"
        COMMENT "Generating deterministic third-party CycloneDX SBOM..."
        VERBATIM)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES")
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES"
            "$<TARGET_FILE_DIR:HarkenSearch>/LICENSES"
        COMMENT "Copying LICENSES directory...")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/docs/legal")
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/legal"
            "$<TARGET_FILE_DIR:HarkenSearch>/docs/legal"
        COMMENT "Copying legal documents...")
endif()

# Behavior-module test target wiring for the integrated Aris workflow layer.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/harken_search_behavior_tests.cmake" OPTIONAL)

# Batch1 pinyin Database behavior test. This target pins isolation ROOT to the
# canonical test-databases card directory; HarkenSearch keeps the product preview ROOT.
if(WIN32)
    add_executable(TestPreviewPinyinDatabase EXCLUDE_FROM_ALL
        tests/test_preview_pinyin_database.cpp
        tests/test_database_link_stubs.cpp
        tests/test_appstate_link_stubs.cpp
        src/AppState.cpp
        src/CoverReplacementFoundation.cpp
        src/Database.cpp
        src/BackfillQueue.cpp
        src/BackfillWorkerPool.cpp
        src/PathUtils.cpp
        src/platform/file_watcher.cpp
        src/Utils.cpp
        src/MetadataReader.cpp
        src/SearchQuery.cpp
        src/SimilaritySearch.cpp
        src/AudioEngine.cpp
        src/platform/audio_playback_miniaudio.cpp
        src/TagVocabularyMapper.cpp
        src/DiagnosticsLogger.cpp
        src/CenterSenseDataSource.cpp
        src/CenterSenseDsp.cpp
        src/TimeStretch.cpp
        src/WaveformExtractor.cpp
        src/NativeFloatReader.cpp
        src/platform/audio_data_source_miniaudio.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
        src/platform/filesystem_win32.cpp
        src/platform/filesystem_std.cpp
        src/platform/audio_file_miniaudio.cpp
        ${AURO_EMERGENCY_FILE_WRITER_SOURCE}
        ${AURO_THREAD_ID_SOURCE}
        ${AURO_METADATA_LEGACY_TAG_CONTENT_SOURCE}
        ${AURO_MEDIA_FOUNDATION_AUDIO_SOURCE}
        ${AURO_AUDIO_TEXT_SOURCE}
        ${AURO_SEARCH_TEXT_GB2312_SOURCE}
        ${SQLITE_SOURCES}
    )
    target_include_directories(TestPreviewPinyinDatabase PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
        ${SQLITE_DIR}
        ${IMGUI_DIR}
    )
    target_link_libraries(TestPreviewPinyinDatabase PRIVATE
        opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm mfplat mfreadwrite mfuuid
    )
    target_compile_definitions(TestPreviewPinyinDatabase PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5
        AURO_TEST_EXTERNAL_PATH_ATTRIBUTES
    )
    target_compile_options(TestPreviewPinyinDatabase PRIVATE
        /W3 /MP /utf-8
        "SHELL:/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/preview_pinyin_isolation_root.h"
    )
    set_target_properties(TestPreviewPinyinDatabase PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
   add_test(NAME PreviewPinyinDatabase COMMAND TestPreviewPinyinDatabase)
   aura_set_test_labels(PreviewPinyinDatabase db)
    add_executable(TestPreviewNasProbe EXCLUDE_FROM_ALL
        tests/test_preview_nas_probe.cpp
        src/platform/filesystem_std.cpp
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_include_directories(TestPreviewNasProbe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestPreviewNasProbe PRIVATE cxx_std_17)
    target_compile_definitions(TestPreviewNasProbe PRIVATE
        AURO_FILESYSTEM_PROBE_PATH_TESTING
        AURO_FILESYSTEM_ATOMIC_REPLACE_TESTING
    )
    target_compile_options(TestPreviewNasProbe PRIVATE /W3 /utf-8)
    set_target_properties(TestPreviewNasProbe PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
    add_test(NAME PreviewNasProbe COMMAND TestPreviewNasProbe)

    add_executable(TestPreviewNasDatabase EXCLUDE_FROM_ALL
        tests/test_preview_nas_database.cpp
        tests/test_database_link_stubs.cpp
        tests/test_appstate_link_stubs.cpp
        src/AppState.cpp
        src/CoverReplacementFoundation.cpp
        src/Database.cpp
        src/BackfillQueue.cpp
        src/BackfillWorkerPool.cpp
        src/PathUtils.cpp
        src/platform/file_watcher.cpp
        src/Utils.cpp
        src/MetadataReader.cpp
        src/MetadataWriter.cpp
        src/SearchQuery.cpp
        src/SimilaritySearch.cpp
        src/AudioEngine.cpp
        src/platform/audio_playback_miniaudio.cpp
        src/TagVocabularyMapper.cpp
        src/DiagnosticsLogger.cpp
        src/CenterSenseDataSource.cpp
        src/CenterSenseDsp.cpp
        src/TimeStretch.cpp
        src/WaveformExtractor.cpp
        src/NativeFloatReader.cpp
        src/platform/audio_data_source_miniaudio.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
        src/platform/filesystem_win32.cpp
        src/platform/filesystem_std.cpp
        src/platform/audio_file_miniaudio.cpp
        ${AURO_EMERGENCY_FILE_WRITER_SOURCE}
        ${AURO_THREAD_ID_SOURCE}
        ${AURO_METADATA_LEGACY_TAG_CONTENT_SOURCE}
        ${AURO_MEDIA_FOUNDATION_AUDIO_SOURCE}
        ${AURO_AUDIO_TEXT_SOURCE}
        ${AURO_SEARCH_TEXT_GB2312_SOURCE}
        ${SQLITE_SOURCES}
    )
    target_include_directories(TestPreviewNasDatabase PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
        ${SQLITE_DIR}
        ${IMGUI_DIR}
    )
    target_link_libraries(TestPreviewNasDatabase PRIVATE
        opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm mfplat mfreadwrite mfuuid
    )
    target_compile_definitions(TestPreviewNasDatabase PRIVATE
        ARIS_PREVIEW_ISOLATION=1
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5
        AURO_TEST_EXTERNAL_PATH_ATTRIBUTES
        AURO_FILESYSTEM_PROBE_PATH_TESTING
    )
    target_compile_options(TestPreviewNasDatabase PRIVATE
        /W3 /MP /utf-8
        "SHELL:/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/preview_nas_isolation_root.h"
    )
    set_target_properties(TestPreviewNasDatabase PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
    add_test(NAME PreviewNasDatabase COMMAND TestPreviewNasDatabase)
    aura_set_test_labels(PreviewNasDatabase db)
endif()

aris_attach_generated_localization()

# Generate integrity signature file (SHA-256 hash of exe for tamper detection)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_sig.py"
            "$<TARGET_FILE:HarkenSearch>" "$<TARGET_FILE:HarkenSearch>.sig"
        COMMENT "Generating HarkenSearch.exe.sig (integrity signature)..."
        VERBATIM
    )

    if(ARIS_BOOTSTRAP_PACKAGE_KEY_ID AND ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
        add_custom_command(TARGET HarkenSearch POST_BUILD
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_runtime_pair.py"
                bootstrap-sidecar
                --bootstrap "$<TARGET_FILE_DIR:HarkenSearch>/HarkenSearchBootstrap.exe"
                --bootstrap-sig "$<TARGET_FILE_DIR:HarkenSearch>/HarkenSearchBootstrap.exe.sig"
            COMMENT "Generating HarkenSearchBootstrap.exe.sig (integrity signature)..."
            VERBATIM
        )
    endif()

    if(ARIS_BOOTSTRAP_PACKAGE_KEY_ID AND ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
        add_custom_command(TARGET HarkenSearch POST_BUILD
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_runtime_pair.py"
                finalize
                --aris "$<TARGET_FILE:HarkenSearch>"
                --bootstrap "$<TARGET_FILE_DIR:HarkenSearch>/HarkenSearchBootstrap.exe"
                --bootstrap-sig "$<TARGET_FILE_DIR:HarkenSearch>/HarkenSearchBootstrap.exe.sig"
                --receipt "$<TARGET_FILE_DIR:HarkenSearch>/harken-runtime-pair.json"
                --source-commit "${ARIS_RUNTIME_PAIR_SOURCE_COMMIT}"
                --build-id "${AIRS_BUILD_ID}"
                --channel "${AIRS_RELEASE_CHANNEL}"
                --config "$<CONFIG>"
                --key-id "${ARIS_BOOTSTRAP_PACKAGE_KEY_ID}"
                --public-key-hex "${ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX}"
            COMMENT "Finalizing canonical Aris runtime-pair receipt..."
            VERBATIM
        )
    endif()

    if(ARIS_BOOTSTRAP_PACKAGE_KEY_ID AND ARIS_BOOTSTRAP_PACKAGE_PUBLIC_KEY_HEX)
        add_custom_command(TARGET HarkenSearch POST_BUILD
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_release_assembly.py"
                "$<TARGET_FILE_DIR:HarkenSearch>"
                --source-commit "${ARIS_RUNTIME_PAIR_SOURCE_COMMIT}"
            COMMENT "Verifying the complete Release assembly..."
            VERBATIM
        )
    endif()


    add_test(
        NAME TestOpenClawHarness
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s .openclaw/tests -p test_harness_*.py
    )
    set_tests_properties(TestOpenClawHarness PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestOpenClawHarness gui-harness)

    add_test(
        NAME TestCueviTaskContext
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_cuevi_task_context -v
    )
    set_tests_properties(TestCueviTaskContext PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestCueviTaskContext core)

    add_test(
        NAME TestCueviQueue
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_cuevi_queue -v
    )
    set_tests_properties(TestCueviQueue PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestCueviQueue core)

    add_test(
        NAME TestRetiredFeedbackCredentialScan
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_trello_deadcode_scan -v
    )
    set_tests_properties(TestRetiredFeedbackCredentialScan PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestRetiredFeedbackCredentialScan core)

    add_test(
        NAME TestCueviFeedbackPublicReleaseGate
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_cuevi_feedback_public_release_gate -v
    )
    set_tests_properties(TestCueviFeedbackPublicReleaseGate PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestCueviFeedbackPublicReleaseGate core)

    add_test(
        NAME TestCrashReportDialogSuppressed
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_crash_report_dialog_suppressed -v
    )
    set_tests_properties(TestCrashReportDialogSuppressed PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestCrashReportDialogSuppressed core)

    add_test(
        NAME TestHarkenSearchBuildIdentityScan
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_harken_search_build_identity_scan -v
    )
    set_tests_properties(TestHarkenSearchBuildIdentityScan PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHarkenSearchBuildIdentityScan core)

    add_test(
        NAME TestHarkenSearchVisibleCopyScan
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_harken_search_visible_copy_scan -v
    )
    set_tests_properties(TestHarkenSearchVisibleCopyScan PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHarkenSearchVisibleCopyScan core)
    add_test(
        NAME TestHarkenSearchDeliverableNamesScan
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_harken_search_deliverable_names_scan -v
    )
    set_tests_properties(TestHarkenSearchDeliverableNamesScan PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHarkenSearchDeliverableNamesScan core)

    add_test(
        NAME TestHarkenSearchUpdateEndpointsScan
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_harken_search_update_endpoints_scan -v
    )
    set_tests_properties(TestHarkenSearchUpdateEndpointsScan PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHarkenSearchUpdateEndpointsScan core)

    add_test(
        NAME TestHarkenLegacyDataDirScan
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_harken_legacy_data_dir_scan -v
    )
    set_tests_properties(TestHarkenLegacyDataDirScan PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHarkenLegacyDataDirScan core)

    add_test(
        NAME TestHarkenSearchWorkflowCueviContract
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_auro_workflow_cuevi_contract -v
    )
    set_tests_properties(TestHarkenSearchWorkflowCueviContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHarkenSearchWorkflowCueviContract core)

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_aris_design_review_server.py)
        add_test(
            NAME TestArisDesignReviewServer
            COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_aris_design_review_server -v
        )
        set_tests_properties(TestArisDesignReviewServer PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        aura_set_test_labels(TestArisDesignReviewServer core ui)
    endif()

    add_test(
        NAME TestDailySelfReview
        COMMAND ${Python3_EXECUTABLE} -m unittest
            tests.test_daily_self_review_model
            tests.test_daily_self_review_collectors
            tests.test_daily_self_review_rules
            tests.test_daily_self_review
            tests.test_daily_self_review_scheduler
            -v
    )
    set_tests_properties(TestDailySelfReview PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestDailySelfReview core)

    add_test(
        NAME TestHarkenSearchIntake
        COMMAND ${Python3_EXECUTABLE} -m unittest tests.test_aris_intake -v
    )
    set_tests_properties(TestHarkenSearchIntake PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHarkenSearchIntake core)

    add_test(
        NAME TestPackEngine
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pack_engine.py"
    )
    set_tests_properties(TestPackEngine PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestPackEngine core)

    add_test(
        NAME TestInstallerLifecycleContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_installer_lifecycle_contract.py"
    )
    set_tests_properties(TestInstallerLifecycleContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestInstallerLifecycleContract core)

    add_test(
        NAME TestQaExePackagingContract
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_qa_exe_packaging_contract.py"
    )
    set_tests_properties(TestQaExePackagingContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestQaExePackagingContract core)

    add_test(
        NAME TestReaperSendProductization
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_send_productization.py"
    )
    set_tests_properties(TestReaperSendProductization PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperSendProductization daw)

    add_test(
        NAME TestHarkenSearchProcessGate
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_auro_process_gate.py"
    )
    set_tests_properties(TestHarkenSearchProcessGate PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHarkenSearchProcessGate core)

    add_test(
        NAME TestHarkenSearchGate
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_auro_gate.py"
    )
    set_tests_properties(TestHarkenSearchGate PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHarkenSearchGate core)

    add_test(
        NAME TestHarkenSearchGateCompatibility
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_auro_gate.py"
    )
    set_tests_properties(TestHarkenSearchGateCompatibility PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHarkenSearchGateCompatibility core)

    add_test(
        NAME TestBehaviorContractCheck
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_behavior_contract_check.py"
    )
    set_tests_properties(TestBehaviorContractCheck PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestBehaviorContractCheck core)

    add_test(
        NAME TestBehaviorRegressionSuite
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_behavior_regression_suite.py"
    )
    set_tests_properties(TestBehaviorRegressionSuite PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestBehaviorRegressionSuite core)

    add_test(
        NAME TestStage6PlatformBoundary
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage6_platform_boundary.py"
    )
    set_tests_properties(TestStage6PlatformBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestStage6PlatformBoundary core)

    add_test(
        NAME TestStage6AIEnginePalBoundary
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage6_ai_engine_pal_boundary.py"
    )
    set_tests_properties(TestStage6AIEnginePalBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestStage6AIEnginePalBoundary core)

    add_test(
        NAME TestAvatarCropperIOContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_avatar_cropper_io_contract.py"
    )
    set_tests_properties(TestAvatarCropperIOContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestAvatarCropperIOContract core)

    add_test(
        NAME TestDawBridgeInstallerPlatformContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_daw_bridge_installer_platform_contract.py"
    )
    set_tests_properties(TestDawBridgeInstallerPlatformContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestDawBridgeInstallerPlatformContract core)

    add_test(
        NAME TestReaperBridgeClientPlatformContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_client_platform_contract.py"
    )
    set_tests_properties(TestReaperBridgeClientPlatformContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeClientPlatformContract core)

    add_test(
        NAME TestMainReaperPlatformContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main_reaper_platform_contract.py"
    )
    set_tests_properties(TestMainReaperPlatformContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestMainReaperPlatformContract core)

    add_test(
        NAME TestReaperBridgeDoctorFilesystemContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_filesystem_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorFilesystemContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorFilesystemContract core)

    add_test(
        NAME TestReaperBridgeDoctorTextEncodingContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_text_encoding_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorTextEncodingContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorTextEncodingContract core)

    add_test(
        NAME TestReaperBridgeDoctorExecutablePathContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_executable_path_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorExecutablePathContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorExecutablePathContract core)

    add_test(
        NAME TestReaperBridgeDoctorMonotonicContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_monotonic_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorMonotonicContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorMonotonicContract core)

    add_test(
        NAME TestReaperBridgeDoctorUserDataContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_user_data_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorUserDataContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorUserDataContract core)

    add_test(
        NAME TestReaperBridgeDoctorProcessQueryContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_process_query_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorProcessQueryContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorProcessQueryContract core)

    add_test(
        NAME TestReaperBridgeDoctorPeInspectionContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_pe_inspection_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorPeInspectionContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorPeInspectionContract core)

    add_test(
        NAME TestReaperBridgeDoctorFileHashContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_file_hash_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorFileHashContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorFileHashContract core)

    add_test(
        NAME TestReaperBridgeDoctorDurableFileOpsContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_durable_file_ops_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorDurableFileOpsContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorDurableFileOpsContract core)

    add_test(
        NAME TestReaperBridgeDoctorDurableFileOpsCopyContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_durable_file_ops_copy_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorDurableFileOpsCopyContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorDurableFileOpsCopyContract core)

    add_test(
        NAME TestReaperBridgeDoctorDurableFileOpsReplaceDeleteContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_durable_file_ops_replace_delete_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorDurableFileOpsReplaceDeleteContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorDurableFileOpsReplaceDeleteContract core)

    add_test(
        NAME TestReaperBridgeDoctorAsciiCompareContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_ascii_compare_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorAsciiCompareContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorAsciiCompareContract core)

    add_test(
        NAME TestReaperBridgeDoctorRegisteredExecutableContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_registered_executable_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorRegisteredExecutableContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorRegisteredExecutableContract core)

    add_test(
        NAME TestReaperBridgeDoctorEnvironmentContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_bridge_doctor_environment_contract.py"
    )
    set_tests_properties(TestReaperBridgeDoctorEnvironmentContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperBridgeDoctorEnvironmentContract core)

    add_test(
        NAME TestHostSendModelPlatformContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_send_model_platform_contract.py"
    )
    set_tests_properties(TestHostSendModelPlatformContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHostSendModelPlatformContract core)

    add_test(
        NAME TestHostSendRuntimePlatformContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host_send_runtime_platform_contract.py"
    )
    set_tests_properties(TestHostSendRuntimePlatformContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestHostSendRuntimePlatformContract core)

    add_test(
        NAME TestModuleUiFilesystemContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_module_ui_filesystem_contract.py"
    )
    set_tests_properties(TestModuleUiFilesystemContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestModuleUiFilesystemContract core)

    add_test(
        NAME TestAppStateNativeIncludeContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_app_state_native_include_contract.py"
    )
    set_tests_properties(TestAppStateNativeIncludeContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestAppStateNativeIncludeContract core)

    add_test(
        NAME TestImGuiLoopIdGate
        COMMAND ${Python3_EXECUTABLE} -m pytest
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_imgui_loop_id_gate.py"
            -q
    )
    set_tests_properties(TestImGuiLoopIdGate PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestImGuiLoopIdGate core)

    add_test(
        NAME TestStage6PerformanceBaseline
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage6_performance_baseline.py"
            --manifest "${CMAKE_CURRENT_SOURCE_DIR}/docs/plans/refactor/stage-6-performance-baseline.json"
    )
    set_tests_properties(TestStage6PerformanceBaseline PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestStage6PerformanceBaseline core)

    if(ARIS_STAGE6_PERFORMANCE_RUN_INDEX)
        add_test(
            NAME TestStage6PerformanceCurrentRun
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage6_performance_baseline.py"
                --manifest "${CMAKE_CURRENT_SOURCE_DIR}/docs/plans/refactor/stage-6-performance-baseline.json"
                --run-index "${ARIS_STAGE6_PERFORMANCE_RUN_INDEX}"
        )
        set_tests_properties(TestStage6PerformanceCurrentRun PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        aura_set_test_labels(TestStage6PerformanceCurrentRun perf stage6)
    endif()

    add_test(
        NAME TestStage5CoverageTool
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage5_coverage.py"
    )
    set_tests_properties(TestStage5CoverageTool PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestStage5CoverageTool core)

    add_test(
        NAME TestGraphifyAuditGraph
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_graphify_audit_graph.py"
    )
    set_tests_properties(TestGraphifyAuditGraph PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestGraphifyAuditGraph core)

    add_test(
        NAME TestStage1ProvenanceGate
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage1_provenance_gate.py"
    )
    set_tests_properties(TestStage1ProvenanceGate PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestStage1ProvenanceGate core)

    add_test(
        NAME TestStage2DeadCodeAudit
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage2_dead_code_audit.py"
    )
    set_tests_properties(TestStage2DeadCodeAudit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestStage2DeadCodeAudit core)

    add_test(
        NAME TestStage2RemovalGate
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage2_removal_gate.py"
    )
    set_tests_properties(TestStage2RemovalGate PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestStage2RemovalGate core)

    add_test(
        NAME TestStage3HumanStyleAudit
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage3_human_style_audit.py"
    )
    set_tests_properties(TestStage3HumanStyleAudit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestStage3HumanStyleAudit core)

    add_test(
        NAME TestStage3StyleGate
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_stage3_style_gate.py"
    )
    set_tests_properties(TestStage3StyleGate PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestStage3StyleGate core)

    add_test(
        NAME TestFolderBrowserContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/qa_folder_browser_contract.py"
    )
    set_tests_properties(TestFolderBrowserContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestFolderBrowserContract core)

    add_test(
        NAME TestScanQuickStartContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scan_quick_start_contract.py"
    )
    set_tests_properties(TestScanQuickStartContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestScanQuickStartContract core)

    add_test(
        NAME TestScanCountCopyThreadContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scan_count_copy_thread_contract.py"
    )
    set_tests_properties(TestScanCountCopyThreadContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestScanCountCopyThreadContract core)
    if(WIN32)
        add_test(NAME TestScanErrorSnapshot
            COMMAND ${Python3_EXECUTABLE} -m pytest
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scan_error_snapshot.py" -q
                --basetemp=${CMAKE_CURRENT_BINARY_DIR}/scan-error-snapshot-test)
        aura_set_test_labels(TestScanErrorSnapshot core)
    endif()

    add_test(
        NAME TestSendToTargetUiContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_send_to_target_ui_contract.py"
    )
    set_tests_properties(TestSendToTargetUiContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestSendToTargetUiContract core)


    add_test(
        NAME TestProToolsToolsContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protools_tools_contract.py"
    )
    set_tests_properties(TestProToolsToolsContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestProToolsToolsContract core)

    add_test(
        NAME TestWindowsMainPathContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_main_path_contract.py"
    )
    set_tests_properties(TestWindowsMainPathContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestWindowsMainPathContract core)

    add_test(
        NAME TestWindowsDpiAwarenessContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_dpi_awareness_contract.py"
    )
    set_tests_properties(TestWindowsDpiAwarenessContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestWindowsDpiAwarenessContract core)

    add_test(
        NAME TestDatabaseCleanupPalContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_cleanup_pal.py"
    )
    set_tests_properties(TestDatabaseCleanupPalContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestDatabaseCleanupPalContract db)

    add_test(
        NAME TestDatabaseErrorFormatNoTruncContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_error_format_no_trunc.py"
    )
    set_tests_properties(TestDatabaseErrorFormatNoTruncContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestDatabaseErrorFormatNoTruncContract db)

    add_test(
        NAME TestDatabasePerfLogPalContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_perf_log_pal.py"
    )
    set_tests_properties(TestDatabasePerfLogPalContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestDatabasePerfLogPalContract db)

    add_test(
        NAME TestAsyncWorkerPerfLogPalContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_worker_perf_log_pal.py"
    )
    set_tests_properties(TestAsyncWorkerPerfLogPalContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestAsyncWorkerPerfLogPalContract core)

    add_test(
        NAME TestStage7ApiContract
        COMMAND ${Python3_EXECUTABLE} -m pytest
            tests/test_stage7_api_contract_audit.py
            tests/test_stage7_include_cycle_contract.py
            tests/test_stage7_platform_contract.py
            -q
    )
    set_tests_properties(TestStage7ApiContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestStage7ApiContract core)
endif()

# ---------------------------------------------------------------------------
# L1 link base (RQ-20260911-04, architecture ruling A): engine-TU auto-source.
# Any target whose SOURCES contain one of the four engine units gets the
# localization unit compiled INTO ITSELF (per-target flags/runtime reproduce
# the former in-product placement exactly; a shared static lib cannot — the
# product is MD, tests MDd, LNK2038). The pass runs last so every target in
# this dir (product, behaviour tests, benches) is covered; per-target manual
# wiring is contract-forbidden.
set(AURA_LOCALIZATION_ENGINE_TUS
    src/Database.cpp
    src/AudioEngine.cpp
    src/AIEngine.cpp
    src/ReaperBridgeDoctor.cpp
)
get_directory_property(AURA_L1_ALL_TARGETS BUILDSYSTEM_TARGETS)
foreach(_t ${AURA_L1_ALL_TARGETS})
    get_target_property(_srcs ${_t} SOURCES)
    if(NOT _srcs)
        continue()
    endif()
    set(_t_needs_loc FALSE)
    foreach(_tu ${AURA_LOCALIZATION_ENGINE_TUS})
        foreach(_s ${_srcs})
            if(_s MATCHES "(^|/)${_tu}$")
                set(_t_needs_loc TRUE)
            endif()
        endforeach()
    endforeach()
    if(_t_needs_loc)
        target_sources(${_t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/Localization.cpp")
        add_dependencies(${_t} ArisLocalizationGenerated)
        # L1b: targets also compiling Utils.cpp reference the HarkenUserData family
        # (EnsureLayout/LegacyAuroDbFilePath = migrator unit, ResolveRoots = layout
        # unit — the second latent class from card-era source lists). Minimal
        # closure by PROOF: system_win32/text_encoding_win32 are NOT injected —
        # several db tests bind them through *_link_stubs.cpp and real units would
        # LNK2005-collide (measured). Per-file double-compile guards, WIN32 only.
        if(WIN32)
            set(_t_has_utils FALSE)
            set(_t_has_migrator FALSE)
            set(_t_has_layout FALSE)
            foreach(_s ${_srcs})
                if(_s MATCHES "(^|/)src/Utils.cpp$")
                    set(_t_has_utils TRUE)
                endif()
                if(_s MATCHES "(^|/)harken_legacy_data_migrator\\.cpp$")
                    set(_t_has_migrator TRUE)
                endif()
                if(_s MATCHES "(^|/)harken_user_data_layout\\.cpp$")
                    set(_t_has_layout TRUE)
                endif()
            endforeach()
            if(_t_has_utils)
                if(NOT _t_has_migrator)
                    target_sources(${_t} PRIVATE
                        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/harken_legacy_data_migrator.cpp")
                endif()
                # EnsureLayout consumes ResolveRoots (defined in the layout unit).
                if(NOT _t_has_layout)
                    target_sources(${_t} PRIVATE
                        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/harken_user_data_layout.cpp")
                endif()
            endif()
        endif()
    endif()
endforeach()

if(Python3_Interpreter_FOUND)
    add_test(
        NAME TestLocalizationLinkContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_localization_link_contract.py"
    )
    set_tests_properties(TestLocalizationLinkContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestLocalizationLinkContract core)
endif()
