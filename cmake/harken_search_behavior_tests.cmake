# Extracted from Aura-Auro_Dev CMakeLists.txt for Aris behavior workflow integration.
# Maintains upstream test target wiring for behavior-contract modules.

add_executable(TestSettingsMigration EXCLUDE_FROM_ALL
    tests/test_settings_migration.cpp
)
target_compile_features(TestSettingsMigration PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestSettingsMigration PRIVATE /W3 /utf-8 /UNDEBUG)
else()
    target_compile_options(TestSettingsMigration PRIVATE -UNDEBUG)
endif()
add_test(NAME TestSettingsMigration COMMAND TestSettingsMigration)
aura_set_test_labels(TestSettingsMigration core)

if(WIN32)
    add_executable(TestFloatingWindowCorners EXCLUDE_FROM_ALL
        tests/test_floating_window_corners.cpp
        src/platform/floating_window_corners_win32.cpp
    )
    target_include_directories(TestFloatingWindowCorners PRIVATE src)
    target_compile_features(TestFloatingWindowCorners PRIVATE cxx_std_17)
    target_link_libraries(TestFloatingWindowCorners PRIVATE user32 gdi32)
    if(MSVC)
        target_compile_options(TestFloatingWindowCorners PRIVATE /W3 /utf-8 /UNDEBUG)
    else()
        target_compile_options(TestFloatingWindowCorners PRIVATE -UNDEBUG)
    endif()
    add_test(NAME TestFloatingWindowCorners COMMAND TestFloatingWindowCorners)
    aura_set_test_labels(TestFloatingWindowCorners core)
endif()

if(WIN32)
    add_executable(TestWin32UnifiedCaptionLayout EXCLUDE_FROM_ALL
        tests/test_win32_unified_caption_layout.cpp
    )
    target_include_directories(TestWin32UnifiedCaptionLayout PRIVATE src)
    target_compile_features(TestWin32UnifiedCaptionLayout PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(TestWin32UnifiedCaptionLayout PRIVATE /W3 /utf-8 /UNDEBUG)
    endif()
    add_test(NAME TestWin32UnifiedCaptionLayout COMMAND TestWin32UnifiedCaptionLayout)
    aura_set_test_labels(TestWin32UnifiedCaptionLayout core)
endif()

if(WIN32)
    add_executable(TestWin32UnifiedCaption EXCLUDE_FROM_ALL
        tests/test_win32_unified_caption.cpp
        src/platform/win32_unified_caption.cpp
    )
    target_include_directories(TestWin32UnifiedCaption PRIVATE src)
    target_compile_features(TestWin32UnifiedCaption PRIVATE cxx_std_17)
    target_link_libraries(TestWin32UnifiedCaption PRIVATE user32 gdi32 gdiplus dwmapi)
    if(MSVC)
        target_compile_options(TestWin32UnifiedCaption PRIVATE /W3 /utf-8 /UNDEBUG)
    endif()
    add_test(NAME TestWin32UnifiedCaption COMMAND TestWin32UnifiedCaption)
    aura_set_test_labels(TestWin32UnifiedCaption core)
endif()

if(WIN32)
    set(AURO_AUDIO_TEXT_SOURCE src/platform/text_encoding_win32.cpp)
    set(_auro_audio_selected_text text_encoding_win32.cpp)
    set(_auro_audio_unselected_text text_encoding_std.cpp)
else()
    set(AURO_AUDIO_TEXT_SOURCE src/platform/text_encoding_std.cpp)
    set(_auro_audio_selected_text text_encoding_std.cpp)
    set(_auro_audio_unselected_text text_encoding_win32.cpp)
endif()

add_executable(TestPlatformFilesystem EXCLUDE_FROM_ALL
    tests/test_platform_filesystem.cpp
    src/platform/filesystem_std.cpp
)
if(WIN32)
    target_sources(TestPlatformFilesystem PRIVATE
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_compile_definitions(TestPlatformFilesystem PRIVATE
        AURO_FILESYSTEM_ATOMIC_REPLACE_TESTING
    )
endif()
target_include_directories(TestPlatformFilesystem PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestPlatformFilesystem PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestPlatformFilesystem PRIVATE /W3 /utf-8)
    set_target_properties(TestPlatformFilesystem PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlatformFilesystem COMMAND TestPlatformFilesystem)
aura_set_test_labels(TestPlatformFilesystem core)

if(WIN32)
    add_executable(TestPlatformFilePrefixProbe EXCLUDE_FROM_ALL
        tests/test_platform_file_prefix_probe.cpp
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_include_directories(TestPlatformFilePrefixProbe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestPlatformFilePrefixProbe PRIVATE cxx_std_17)
    target_compile_definitions(TestPlatformFilePrefixProbe PRIVATE
        AURO_FILESYSTEM_FILE_PREFIX_PROBE_TESTING
    )
    if(MSVC)
        target_compile_options(TestPlatformFilePrefixProbe PRIVATE /W3 /utf-8)
        set_target_properties(TestPlatformFilePrefixProbe PROPERTIES
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        )
    endif()
   add_test(NAME TestPlatformFilePrefixProbe COMMAND TestPlatformFilePrefixProbe)
   aura_set_test_labels(TestPlatformFilePrefixProbe core)
 
    add_executable(TestNetworkUnreachableProbe EXCLUDE_FROM_ALL
        tests/test_preview_nas_probe.cpp
        src/platform/filesystem_std.cpp
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_include_directories(TestNetworkUnreachableProbe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestNetworkUnreachableProbe PRIVATE cxx_std_17)
    target_compile_definitions(TestNetworkUnreachableProbe PRIVATE
        AURO_FILESYSTEM_PROBE_PATH_TESTING
        AURO_FILESYSTEM_ATOMIC_REPLACE_TESTING
    )
    if(MSVC)
        target_compile_options(TestNetworkUnreachableProbe PRIVATE /W3 /utf-8)
        set_target_properties(TestNetworkUnreachableProbe PROPERTIES
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        )
    endif()
    add_test(NAME TestNetworkUnreachableProbe COMMAND TestNetworkUnreachableProbe)
    aura_set_test_labels(TestNetworkUnreachableProbe core)
endif()

add_executable(TestAudioPlaybackCursor EXCLUDE_FROM_ALL
    tests/test_audio_playback_cursor.cpp
    tests/audio_playback_miniaudio_test_seam.cpp
    src/platform/audio_playback_miniaudio.cpp
)
target_include_directories(TestAudioPlaybackCursor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_features(TestAudioPlaybackCursor PRIVATE cxx_std_17)
target_compile_definitions(TestAudioPlaybackCursor PRIVATE AURO_AUDIO_PLAYBACK_TESTING)
if(MSVC)
    target_compile_options(TestAudioPlaybackCursor PRIVATE /W3 /utf-8)
    set_target_properties(TestAudioPlaybackCursor PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestAudioPlaybackCursor COMMAND TestAudioPlaybackCursor)
aura_set_test_labels(TestAudioPlaybackCursor core)

add_executable(TestAudioPlaybackInit EXCLUDE_FROM_ALL
    tests/test_audio_playback_init.cpp
    tests/audio_playback_miniaudio_test_seam.cpp
    src/platform/audio_playback_miniaudio.cpp
)
target_include_directories(TestAudioPlaybackInit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_features(TestAudioPlaybackInit PRIVATE cxx_std_17)
target_compile_definitions(TestAudioPlaybackInit PRIVATE AURO_AUDIO_PLAYBACK_TESTING)
if(MSVC)
    target_compile_options(TestAudioPlaybackInit PRIVATE /W3 /utf-8)
    set_target_properties(TestAudioPlaybackInit PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestAudioPlaybackInit COMMAND TestAudioPlaybackInit)
aura_set_test_labels(TestAudioPlaybackInit core)

add_executable(TestAudioDataSourceFormat EXCLUDE_FROM_ALL
    tests/test_audio_data_source_format.cpp
    tests/miniaudio_impl.cpp
    src/platform/audio_data_source_miniaudio.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestAudioDataSourceFormat PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestAudioDataSourceFormat PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAudioDataSourceFormat PRIVATE /W3 /utf-8)
    set_target_properties(TestAudioDataSourceFormat PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestAudioDataSourceFormat COMMAND TestAudioDataSourceFormat)
aura_set_test_labels(TestAudioDataSourceFormat core)

if(WIN32)
    add_executable(TestProToolsBridgeExecutablePath EXCLUDE_FROM_ALL
        tests/test_pro_tools_bridge_executable_path.cpp
        src/ProToolsBridge.cpp
        src/platform/filesystem_std.cpp
        src/platform/filesystem_win32.cpp
        src/platform/process_query_win32.cpp
        src/platform/process_launch_win32.cpp
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_include_directories(TestProToolsBridgeExecutablePath PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestProToolsBridgeExecutablePath PRIVATE cxx_std_17)
    target_compile_definitions(TestProToolsBridgeExecutablePath PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        target_compile_options(TestProToolsBridgeExecutablePath PRIVATE /W3 /utf-8)
        set_target_properties(TestProToolsBridgeExecutablePath PROPERTIES
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        )
    endif()
    add_test(NAME TestProToolsBridgeExecutablePath COMMAND TestProToolsBridgeExecutablePath)
    aura_set_test_labels(TestProToolsBridgeExecutablePath core)
endif()

add_executable(TestMetadataReaderPathEncoding EXCLUDE_FROM_ALL
    tests/test_metadata_reader_path_encoding.cpp
    src/MetadataReader.cpp
    src/platform/filesystem_std.cpp
)
if(WIN32)
    target_sources(TestMetadataReaderPathEncoding PRIVATE
        src/platform/text_encoding_win32.cpp
    )
else()
    target_sources(TestMetadataReaderPathEncoding PRIVATE
        src/platform/text_encoding_std.cpp
    )
endif()
target_include_directories(TestMetadataReaderPathEncoding PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestMetadataReaderPathEncoding PRIVATE cxx_std_17)
target_compile_definitions(TestMetadataReaderPathEncoding PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
if(MSVC)
    target_compile_options(TestMetadataReaderPathEncoding PRIVATE /W3 /utf-8)
    set_target_properties(TestMetadataReaderPathEncoding PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestMetadataReaderPathEncoding COMMAND TestMetadataReaderPathEncoding)
aura_set_test_labels(TestMetadataReaderPathEncoding core)

add_executable(TestMetadataReaderContentEncoding EXCLUDE_FROM_ALL
    tests/test_metadata_reader_content_encoding.cpp
    src/MetadataReader.cpp
    src/platform/filesystem_std.cpp
)
if(WIN32)
    target_sources(TestMetadataReaderContentEncoding PRIVATE
        src/platform/text_encoding_win32.cpp
    )
else()
    target_sources(TestMetadataReaderContentEncoding PRIVATE
        src/platform/text_encoding_std.cpp
    )
endif()
target_include_directories(TestMetadataReaderContentEncoding PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestMetadataReaderContentEncoding PRIVATE cxx_std_17)
target_compile_definitions(TestMetadataReaderContentEncoding PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
if(MSVC)
    target_compile_options(TestMetadataReaderContentEncoding PRIVATE /W3 /utf-8)
    set_target_properties(TestMetadataReaderContentEncoding PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestMetadataReaderContentEncoding COMMAND TestMetadataReaderContentEncoding)
aura_set_test_labels(TestMetadataReaderContentEncoding core)

add_executable(TestMetadataWriterImageLoad EXCLUDE_FROM_ALL
    tests/test_metadata_writer_image_load.cpp
    src/MetadataWriter.cpp
    src/platform/filesystem_std.cpp
)
if(WIN32)
    target_sources(TestMetadataWriterImageLoad PRIVATE
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
else()
    target_sources(TestMetadataWriterImageLoad PRIVATE
        src/platform/text_encoding_std.cpp
    )
endif()
target_include_directories(TestMetadataWriterImageLoad PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestMetadataWriterImageLoad PRIVATE cxx_std_17)
target_compile_definitions(TestMetadataWriterImageLoad PRIVATE
    AURA_TEST_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
if(MSVC)
    target_compile_options(TestMetadataWriterImageLoad PRIVATE /W3 /utf-8)
    set_target_properties(TestMetadataWriterImageLoad PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestMetadataWriterImageLoad COMMAND TestMetadataWriterImageLoad)
aura_set_test_labels(TestMetadataWriterImageLoad core)

if(WIN32)
    add_executable(TestPlatformRegistryString EXCLUDE_FROM_ALL
        tests/test_platform_registry_string.cpp
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_include_directories(TestPlatformRegistryString PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(TestPlatformRegistryString PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_link_libraries(TestPlatformRegistryString PRIVATE
        shell32 advapi32 ole32 comdlg32
    )
    if(MSVC)
        target_compile_options(TestPlatformRegistryString PRIVATE /W3 /utf-8)
        set_target_properties(TestPlatformRegistryString PROPERTIES
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        )
    endif()
    add_test(NAME TestPlatformRegistryString COMMAND TestPlatformRegistryString)
    aura_set_test_labels(TestPlatformRegistryString core)

    add_executable(TestDefaultAudioPlayerAssociation EXCLUDE_FROM_ALL
        tests/test_default_audio_player_association.cpp
        src/DefaultAudioPlayerAssociation.cpp
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
        src/platform/executable_path_win32.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
    )
    target_include_directories(TestDefaultAudioPlayerAssociation PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestDefaultAudioPlayerAssociation PRIVATE cxx_std_17)
    target_compile_definitions(TestDefaultAudioPlayerAssociation PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_link_libraries(TestDefaultAudioPlayerAssociation PRIVATE
        shell32 advapi32 ole32 comdlg32 shlwapi
    )
    if(MSVC)
        target_compile_options(TestDefaultAudioPlayerAssociation PRIVATE /W3 /utf-8)
        set_target_properties(TestDefaultAudioPlayerAssociation PROPERTIES
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        )
    endif()
    add_test(NAME TestDefaultAudioPlayerAssociation COMMAND TestDefaultAudioPlayerAssociation)
    aura_set_test_labels(TestDefaultAudioPlayerAssociation core)

    add_executable(TestPlatformEnvironmentVariable EXCLUDE_FROM_ALL
        tests/test_platform_environment_variable.cpp
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_include_directories(TestPlatformEnvironmentVariable PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestPlatformEnvironmentVariable PRIVATE cxx_std_17)
    target_compile_definitions(TestPlatformEnvironmentVariable PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_link_libraries(TestPlatformEnvironmentVariable PRIVATE
        shell32 advapi32 ole32 comdlg32
    )
    if(MSVC)
        target_compile_options(TestPlatformEnvironmentVariable PRIVATE /W3 /utf-8)
        set_target_properties(TestPlatformEnvironmentVariable PROPERTIES
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        )
    endif()
    add_test(NAME TestPlatformEnvironmentVariable COMMAND TestPlatformEnvironmentVariable)
    aura_set_test_labels(TestPlatformEnvironmentVariable core)
endif()

add_executable(TestAudioEngineEnvironmentPolicy EXCLUDE_FROM_ALL
    tests/test_audio_engine_environment_policy.cpp
)
target_include_directories(TestAudioEngineEnvironmentPolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestAudioEngineEnvironmentPolicy PRIVATE cxx_std_17)
target_compile_definitions(TestAudioEngineEnvironmentPolicy PRIVATE
    AURO_AUDIO_ENGINE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/src/AudioEngine.cpp"
)
if(MSVC)
    target_compile_options(TestAudioEngineEnvironmentPolicy PRIVATE /W3 /utf-8)
    set_target_properties(TestAudioEngineEnvironmentPolicy PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestAudioEngineEnvironmentPolicy COMMAND TestAudioEngineEnvironmentPolicy)
aura_set_test_labels(TestAudioEngineEnvironmentPolicy core)

add_executable(TestHostSendRuntime EXCLUDE_FROM_ALL
    tests/test_host_send_runtime.cpp
    src/HostSendRuntime.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestHostSendRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestHostSendRuntime PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestHostSendRuntime PRIVATE /W3 /utf-8)
    set_target_properties(TestHostSendRuntime PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestHostSendRuntime COMMAND TestHostSendRuntime)
aura_set_test_labels(TestHostSendRuntime core)

add_executable(TestStage6AudioDeviceProbe EXCLUDE_FROM_ALL
    tests/test_stage6_audio_device_probe.cpp
)
target_include_directories(TestStage6AudioDeviceProbe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestStage6AudioDeviceProbe PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestStage6AudioDeviceProbe PRIVATE /W3 /utf-8)
    set_target_properties(TestStage6AudioDeviceProbe PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()

add_executable(TestStage6FileWatcherProbe EXCLUDE_FROM_ALL
    tests/test_stage6_file_watcher_probe.cpp
    src/platform/file_watcher.cpp
    src/DiagnosticsLogger.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestStage6FileWatcherProbe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestStage6FileWatcherProbe PRIVATE cxx_std_17)
target_compile_definitions(TestStage6FileWatcherProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(TestStage6FileWatcherProbe PRIVATE /W3 /utf-8)
    set_target_properties(TestStage6FileWatcherProbe PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()

add_executable(TestStage6PlaybackLoadSeekProbe EXCLUDE_FROM_ALL
    tests/test_stage6_playback_load_seek_probe.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/CoverReplacementFoundation.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/NativeFloatReader.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/filesystem_win32.cpp
    src/TimeStretch.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestStage6PlaybackLoadSeekProbe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestStage6PlaybackLoadSeekProbe PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestStage6PlaybackLoadSeekProbe PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5
)
if(MSVC)
    target_compile_options(TestStage6PlaybackLoadSeekProbe PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestStage6PlaybackLoadSeekProbe PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestStage6PlaybackLoadSeekProbe COMMAND TestStage6PlaybackLoadSeekProbe)
aura_set_test_labels(TestStage6PlaybackLoadSeekProbe audio)

add_executable(TestStage6ScanTerminalThroughputProbe EXCLUDE_FROM_ALL
    tests/test_stage6_scan_terminal_throughput_probe.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/CoverReplacementFoundation.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/NativeFloatReader.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/filesystem_win32.cpp
    src/TimeStretch.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestStage6ScanTerminalThroughputProbe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestStage6ScanTerminalThroughputProbe PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestStage6ScanTerminalThroughputProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5)
if(MSVC)
    target_compile_options(TestStage6ScanTerminalThroughputProbe PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestStage6ScanTerminalThroughputProbe PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestStage6ScanTerminalThroughputProbe COMMAND TestStage6ScanTerminalThroughputProbe)
set_tests_properties(TestStage6ScanTerminalThroughputProbe PROPERTIES TIMEOUT 60)
aura_set_test_labels(TestStage6ScanTerminalThroughputProbe db)

add_executable(TestSearchSemanticMatrix EXCLUDE_FROM_ALL
    tests/test_search_semantic_matrix.cpp
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
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/NativeFloatReader.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/filesystem_win32.cpp
    src/TimeStretch.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestSearchSemanticMatrix PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestSearchSemanticMatrix PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestSearchSemanticMatrix PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5)
if(MSVC)
    target_compile_options(TestSearchSemanticMatrix PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestSearchSemanticMatrix PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE" LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestSearchSemanticMatrix COMMAND TestSearchSemanticMatrix)
aura_set_test_labels(TestSearchSemanticMatrix db)

# Test: PRD fuzzy search DB integration for CJK, pinyin, initials, path, and snippets.

add_executable(TestResultRowHighlightSpans EXCLUDE_FROM_ALL
    tests/test_result_row_highlight_spans.cpp
)
target_include_directories(TestResultRowHighlightSpans PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestResultRowHighlightSpans PRIVATE /W3 /utf-8)
    set_target_properties(TestResultRowHighlightSpans PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestResultRowHighlightSpans COMMAND TestResultRowHighlightSpans)
aura_set_test_labels(TestResultRowHighlightSpans db)

add_executable(TestSearchHitColumnIdentity EXCLUDE_FROM_ALL
    tests/test_search_hit_column_identity.cpp
    src/SearchQuery.cpp
)
target_include_directories(TestSearchHitColumnIdentity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(TestSearchHitColumnIdentity PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestSearchHitColumnIdentity COMMAND TestSearchHitColumnIdentity)
aura_set_test_labels(TestSearchHitColumnIdentity db)

add_executable(TestSettingsSearchHighlightSpans EXCLUDE_FROM_ALL
    tests/test_settings_search_highlight_spans.cpp
)
target_include_directories(TestSettingsSearchHighlightSpans PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(WIN32)
    target_sources(TestSettingsSearchHighlightSpans PRIVATE
        src/platform/search_text_gb2312_win32.cpp
    )
else()
    target_sources(TestSettingsSearchHighlightSpans PRIVATE
        src/platform/search_text_gb2312_std.cpp
    )
endif()
if(MSVC)
    target_compile_options(TestSettingsSearchHighlightSpans PRIVATE /W3 /utf-8)
    set_target_properties(TestSettingsSearchHighlightSpans PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSettingsSearchHighlightSpans COMMAND TestSettingsSearchHighlightSpans)
aura_set_test_labels(TestSettingsSearchHighlightSpans db)

add_executable(TestSearchQueryTrigram EXCLUDE_FROM_ALL
    tests/test_search_query_trigram.cpp
    src/SearchQuery.cpp
    src/Utils.cpp
)
target_include_directories(TestSearchQueryTrigram PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestSearchQueryTrigram PRIVATE /W3 /utf-8)
    set_target_properties(TestSearchQueryTrigram PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSearchQueryTrigram COMMAND TestSearchQueryTrigram)
aura_set_test_labels(TestSearchQueryTrigram db)

add_executable(TestSearchRecall EXCLUDE_FROM_ALL
    tests/test_search_recall.cpp
    src/SearchQuery.cpp
    src/Utils.cpp
)
target_include_directories(TestSearchRecall PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestSearchRecall PRIVATE /W3 /utf-8)
    set_target_properties(TestSearchRecall PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSearchRecall COMMAND TestSearchRecall)
aura_set_test_labels(TestSearchRecall db)

add_executable(TestThesaurusBidirectional EXCLUDE_FROM_ALL
    tests/test_thesaurus_bidirectional.cpp
    src/SearchQuery.cpp
    src/Utils.cpp
)
target_include_directories(TestThesaurusBidirectional PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestThesaurusBidirectional PRIVATE /W3 /utf-8)
    set_target_properties(TestThesaurusBidirectional PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestThesaurusBidirectional COMMAND TestThesaurusBidirectional)
aura_set_test_labels(TestThesaurusBidirectional db)

add_executable(TestDbSearchFacets EXCLUDE_FROM_ALL
    tests/test_db_search_facets.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/TimeStretch.cpp
    src/CoverReplacementFoundation.cpp
    ${SQLITE_SOURCES}
)
if(WIN32)
    target_sources(TestDbSearchFacets PRIVATE
        src/NativeFloatReader.cpp
        src/platform/audio_data_source_miniaudio.cpp
        src/platform/filesystem_win32.cpp
    )
else()
    target_sources(TestDbSearchFacets PRIVATE
        src/NativeFloatReader.cpp
        src/platform/audio_data_source_miniaudio.cpp
    )
endif()
target_include_directories(TestDbSearchFacets PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestDbSearchFacets PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestDbSearchFacets PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5)
if(MSVC)
    target_compile_options(TestDbSearchFacets PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestDbSearchFacets PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE" LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestDbSearchFacets COMMAND TestDbSearchFacets)
aura_set_test_labels(TestDbSearchFacets db)

# Test: white-box + integration semantic matrix for SearchQuery/buildSearchWhere.

add_executable(TestTrigramE2E EXCLUDE_FROM_ALL
    tests/test_trigram_e2e.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/NativeFloatReader.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/filesystem_win32.cpp
    src/TimeStretch.cpp
    src/CoverReplacementFoundation.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestTrigramE2E PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestTrigramE2E PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestTrigramE2E PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5)
if(MSVC)
    target_compile_options(TestTrigramE2E PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestTrigramE2E PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE" LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestTrigramE2E COMMAND TestTrigramE2E)
aura_set_test_labels(TestTrigramE2E db)

# Test: DB search/facet semantic consistency for tag pills and NOT keyword.

add_executable(TestBrowserSearch EXCLUDE_FROM_ALL
    tests/test_browser_search.cpp
    src/PathUtils.cpp
    tests/playlist_folder_tree_path_conversion_stub.cpp
)
if(MSVC)
    target_compile_options(TestBrowserSearch PRIVATE /W3 /utf-8)
    set_target_properties(TestBrowserSearch PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestBrowserSearch COMMAND TestBrowserSearch)
aura_set_test_labels(TestBrowserSearch core)

add_executable(TestFuzzySearchPrd EXCLUDE_FROM_ALL
    tests/test_fuzzy_search_prd.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/CoverReplacementFoundation.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/TimeStretch.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestFuzzySearchPrd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestFuzzySearchPrd PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestFuzzySearchPrd PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5)
if(MSVC)
    target_compile_options(TestFuzzySearchPrd PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestFuzzySearchPrd PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE" LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestFuzzySearchPrd COMMAND TestFuzzySearchPrd)
aura_set_test_labels(TestFuzzySearchPrd db)

# Test: DestroyDatabasePackage deletes all files (db + wal/shm + Cache)

add_executable(TestScanRenameQa EXCLUDE_FROM_ALL
    tests/test_scan_rename_qa.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestScanRenameQa PRIVATE
    ${SQLITE_DIR}
)
target_compile_definitions(TestScanRenameQa PRIVATE
    SQLITE_ENABLE_FTS5
)
if(MSVC)
    target_compile_options(TestScanRenameQa PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestScanRenameQa PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestScanRenameQa COMMAND TestScanRenameQa)
aura_set_test_labels(TestScanRenameQa db)

# F-034: Relink missing files integration tests

add_executable(TestBackfillRecoveryWorker EXCLUDE_FROM_ALL
    tests/test_backfill_recovery_worker.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/CoverReplacementFoundation.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/platform/filesystem_win32.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/NativeFloatReader.cpp
    src/platform/audio_data_source_miniaudio.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
    src/TimeStretch.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestBackfillRecoveryWorker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestBackfillRecoveryWorker PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestBackfillRecoveryWorker PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5
    AURA_TEST_SCAN_STALL_SECONDS=0
    AURA_TEST_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)
if(MSVC)
    target_compile_options(TestBackfillRecoveryWorker PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestBackfillRecoveryWorker PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestBackfillRecoveryWorker COMMAND TestBackfillRecoveryWorker)
aura_set_test_labels(TestBackfillRecoveryWorker db)

# T1.3: head+tail+size XXH3 stream hash anti-collision ctest.
# See docs/plans/2026-04-30-cover-folder-canonical-key-refactor.md (T1.3)

add_executable(TestScanWaveformCacheContract EXCLUDE_FROM_ALL
    tests/test_scan_waveform_cache_contract.cpp
    tests/test_database_link_stubs.cpp
    tests/test_scan_database_audio_probe_stub.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/TimeStretch.cpp
    src/CoverReplacementFoundation.cpp
    src/NativeFloatReader.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/filesystem_win32.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestScanWaveformCacheContract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestScanWaveformCacheContract PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestScanWaveformCacheContract PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5
    AURA_TEST_SCAN_STALL_SECONDS=0
    AURA_TEST_SCAN_SLOW_WORKER_MS=500
    AURO_TEST_WAV_DATABASE_AUDIO_PROBE=1
)
if(MSVC)
    target_compile_options(TestScanWaveformCacheContract PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestScanWaveformCacheContract PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE" LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestScanWaveformCacheContract COMMAND TestScanWaveformCacheContract)
aura_set_test_labels(TestScanWaveformCacheContract db)

add_executable(TestScanPauseResume EXCLUDE_FROM_ALL
    tests/test_scan_pause_resume.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/TimeStretch.cpp
    src/CoverReplacementFoundation.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestScanPauseResume PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestScanPauseResume PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestScanPauseResume PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5
    AURA_TEST_SCAN_STALL_SECONDS=0
    AURA_TEST_SCAN_SLOW_WORKER_MS=120
)
if(MSVC)
    target_compile_options(TestScanPauseResume PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestScanPauseResume PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE" LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestScanPauseResume COMMAND TestScanPauseResume)
aura_set_test_labels(TestScanPauseResume db)

# Test (T-295): cross-tab data sync. Verifies dataRevision bumps on
# tag/favorite/metadata writes and unit-tests the syncFromTab refresh
# trigger. Skips src/AppState.cpp on purpose, replicating the 4-line
# revision check inline avoids dragging in the entire AppState symbol
# graph (FileBrowser, GaplessEngine, MusicAnalyzer, etc).

add_executable(TestWaveformPresentationPolicy EXCLUDE_FROM_ALL
    tests/test_waveform_presentation_policy.cpp
)
target_include_directories(TestWaveformPresentationPolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(TestWaveformPresentationPolicy PRIVATE /W3 /utf-8)
    set_target_properties(TestWaveformPresentationPolicy PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestWaveformPresentationPolicy COMMAND TestWaveformPresentationPolicy)
aura_set_test_labels(TestWaveformPresentationPolicy core)

add_executable(TestImportPrecomputeWaveform EXCLUDE_FROM_ALL
    tests/test_import_precompute_waveform.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/TimeStretch.cpp
    src/CoverReplacementFoundation.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestImportPrecomputeWaveform PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestImportPrecomputeWaveform PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestImportPrecomputeWaveform PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5)
if(MSVC)
    target_compile_options(TestImportPrecomputeWaveform PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestImportPrecomputeWaveform PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE" LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestImportPrecomputeWaveform COMMAND TestImportPrecomputeWaveform)
aura_set_test_labels(TestImportPrecomputeWaveform db)

# F-001 slice7-B (#346) perf bench: scanDirectoryEx with completeImport
# true vs false on a real folder. Not added as ctest -- run manually:
#   .\build_dbg\Debug\TestScanPerfBoom.exe "H:\Sound effect\综合\BoomLibrary"

add_executable(TestCoverPathNorm EXCLUDE_FROM_ALL
    tests/test_cover_path_norm.cpp
    src/Utils.cpp
)
if(WIN32)
    target_sources(TestCoverPathNorm PRIVATE
        src/platform/text_encoding_win32.cpp
    )
else()
    target_sources(TestCoverPathNorm PRIVATE
        src/platform/text_encoding_std.cpp
    )
endif()
target_include_directories(TestCoverPathNorm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestCoverPathNorm PRIVATE /W3 /utf-8)
    set_target_properties(TestCoverPathNorm PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCoverPathNorm COMMAND TestCoverPathNorm)
aura_set_test_labels(TestCoverPathNorm core)

# Aris canonical path helpers (T1.2 of cover-folder canonical key refactor).
# See docs/plans/2026-04-30-canonical-display-separation.md

add_executable(TestOutputDeviceComboItem EXCLUDE_FROM_ALL
    tests/test_output_device_combo_item.cpp
)
target_include_directories(TestOutputDeviceComboItem PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestOutputDeviceComboItem PRIVATE /W3 /utf-8)
    set_target_properties(TestOutputDeviceComboItem PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestOutputDeviceComboItem COMMAND TestOutputDeviceComboItem)
aura_set_test_labels(TestOutputDeviceComboItem core)

add_executable(TestCoverLaneOrder EXCLUDE_FROM_ALL
    tests/test_cover_lane_order.cpp
)
target_include_directories(TestCoverLaneOrder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestCoverLaneOrder PRIVATE /W3 /utf-8)
    set_target_properties(TestCoverLaneOrder PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCoverLaneOrder COMMAND TestCoverLaneOrder)
aura_set_test_labels(TestCoverLaneOrder core)

add_executable(TestCoverClickPerf EXCLUDE_FROM_ALL
    tests/test_cover_click_perf.cpp
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
    src/platform/filesystem_win32.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestCoverClickPerf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestCoverClickPerf PRIVATE
    opengl32
    gdi32
    ole32
    windowscodecs
    uuid
    shell32
    comdlg32
    winmm
)
target_compile_definitions(TestCoverClickPerf PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SQLITE_ENABLE_FTS5
)
if(MSVC)
    target_compile_options(TestCoverClickPerf PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestCoverClickPerf PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCoverClickPerf COMMAND TestCoverClickPerf)
aura_set_test_labels(TestCoverClickPerf perf)

# Theme editor color contract tests (theme schema / JSON / waveform bridge)

add_executable(TestCoverDiagnostics EXCLUDE_FROM_ALL
    tests/test_cover_diagnostics.cpp
    tests/test_cover_runtime_stubs.cpp
    src/CoverModule.cpp
    src/platform/cover_discovery_std.cpp
    src/PathUtils.cpp
    src/Utils.cpp
    src/MemoryBudget.cpp
    src/DiagnosticsLogger.cpp
)
if(WIN32)
    target_sources(TestCoverDiagnostics PRIVATE src/platform/text_encoding_win32.cpp)
else()
    target_sources(TestCoverDiagnostics PRIVATE src/platform/text_encoding_std.cpp)
endif()
target_include_directories(TestCoverDiagnostics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${IMGUI_DIR}
)
target_compile_definitions(TestCoverDiagnostics PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
if(MSVC)
    target_compile_options(TestCoverDiagnostics PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestCoverDiagnostics PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCoverDiagnostics COMMAND TestCoverDiagnostics)
aura_set_test_labels(TestCoverDiagnostics core)

# T2.8: One-shot folder_path canonical migration ctest.
# See docs/plans/2026-04-30-cover-folder-canonical-key-refactor.md

add_executable(TestSwitchDatabaseBoundary EXCLUDE_FROM_ALL
    tests/test_switch_database_boundary.cpp
)
target_include_directories(TestSwitchDatabaseBoundary PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(TestSwitchDatabaseBoundary PRIVATE
    AURA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
if(MSVC)
    target_compile_options(TestSwitchDatabaseBoundary PRIVATE /W3 /utf-8)
    set_target_properties(TestSwitchDatabaseBoundary PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSwitchDatabaseBoundary COMMAND TestSwitchDatabaseBoundary)
aura_set_test_labels(TestSwitchDatabaseBoundary core)

# Clear-Play-History side-effect contract: must not touch cover/waveform caches.
# Pins the 2026-06-12 bug fix where the Tools menu entry incorrectly called
# performGlobalMemoryGC() and wiped the cover wall textures.

add_executable(TestStartupReconciliation EXCLUDE_FROM_ALL
    tests/test_startup_reconciliation.cpp
    src/DeferredSidebarBootstrapWorkflow.cpp
    src/ReaperFileListImportWorkflow.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/CoverReplacementFoundation.cpp
    src/Database.cpp
    src/BackfillQueue.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/TimeStretch.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestStartupReconciliation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestStartupReconciliation PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestStartupReconciliation PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5 AURA_TEST_STUB_BACKFILL_SCAN_WRITE_GUARD)
if(MSVC)
    target_compile_options(TestStartupReconciliation PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestStartupReconciliation PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE" LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestStartupReconciliation COMMAND TestStartupReconciliation)
aura_set_test_labels(TestStartupReconciliation db)

add_executable(TestOggPlaybackSupport EXCLUDE_FROM_ALL
    tests/test_ogg_playback_support.cpp
)
target_compile_features(TestOggPlaybackSupport PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestOggPlaybackSupport PRIVATE /W3 /utf-8)
    set_target_properties(TestOggPlaybackSupport PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestOggPlaybackSupport COMMAND TestOggPlaybackSupport)
aura_set_test_labels(TestOggPlaybackSupport core)

add_executable(TestMetadataLegacyTagContent EXCLUDE_FROM_ALL
    tests/test_metadata_legacy_tag_content.cpp
    ${AURO_METADATA_LEGACY_TAG_CONTENT_SOURCE}
)
target_include_directories(TestMetadataLegacyTagContent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestMetadataLegacyTagContent PRIVATE cxx_std_17)
add_test(NAME TestMetadataLegacyTagContent COMMAND TestMetadataLegacyTagContent)
aura_set_test_labels(TestMetadataLegacyTagContent core)

add_executable(TestCoverDiscovery EXCLUDE_FROM_ALL
    tests/test_cover_discovery.cpp
    src/platform/cover_discovery_std.cpp
)
target_include_directories(TestCoverDiscovery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestCoverDiscovery PRIVATE cxx_std_17)
target_compile_definitions(TestCoverDiscovery PRIVATE ARIS_COVER_DISCOVERY_TESTING=1)
if(MSVC)
    target_compile_options(TestCoverDiscovery PRIVATE /W3 /utf-8)
    set_target_properties(TestCoverDiscovery PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCoverDiscovery COMMAND TestCoverDiscovery)
aura_set_test_labels(TestCoverDiscovery core)

add_executable(TestQaFiles EXCLUDE_FROM_ALL
    tests/test_qa_files.cpp
    src/platform/qa_files_std.cpp
    src/platform/filesystem_std.cpp
)
if(WIN32)
    target_sources(TestQaFiles PRIVATE
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
endif()
target_include_directories(TestQaFiles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestQaFiles PRIVATE cxx_std_17)
target_compile_definitions(TestQaFiles PRIVATE ARIS_QA_FILES_TESTING=1)
if(MSVC)
    target_compile_options(TestQaFiles PRIVATE /W3 /utf-8)
    set_target_properties(TestQaFiles PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestQaFiles COMMAND TestQaFiles)
aura_set_test_labels(TestQaFiles core)

add_executable(TestProToolsPathEncoding EXCLUDE_FROM_ALL
    tests/test_pro_tools_path_encoding.cpp
    ${AURO_PROTOOLS_PATH_ENCODING_SOURCE}
)
target_include_directories(TestProToolsPathEncoding PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestProToolsPathEncoding PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestProToolsPathEncoding PRIVATE /W3 /utf-8)
    set_target_properties(TestProToolsPathEncoding PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestProToolsPathEncoding COMMAND TestProToolsPathEncoding)
aura_set_test_labels(TestProToolsPathEncoding core)

add_executable(TestWmaDecodeSupport EXCLUDE_FROM_ALL
    tests/test_wma_decode_support.cpp
    ${AURO_MEDIA_FOUNDATION_AUDIO_SOURCE}
)
if(WIN32)
    target_sources(TestWmaDecodeSupport PRIVATE
        src/platform/text_encoding_win32.cpp
    )
    target_link_libraries(TestWmaDecodeSupport PRIVATE
        mfplat mfreadwrite mfuuid
    )
endif()
target_include_directories(TestWmaDecodeSupport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestWmaDecodeSupport PRIVATE cxx_std_17)
target_compile_definitions(TestWmaDecodeSupport PRIVATE
    NOMINMAX
    AURO_TEST_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)
if(MSVC)
    target_compile_options(TestWmaDecodeSupport PRIVATE /W3 /utf-8)
    set_target_properties(TestWmaDecodeSupport PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestWmaDecodeSupport COMMAND TestWmaDecodeSupport)
aura_set_test_labels(TestWmaDecodeSupport core)

add_executable(TestMediaFoundationHeaderSelfContained EXCLUDE_FROM_ALL
    tests/test_media_foundation_header_self_contained.cpp
)
target_include_directories(TestMediaFoundationHeaderSelfContained PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestMediaFoundationHeaderSelfContained PRIVATE cxx_std_17)
set_property(TARGET TestMediaFoundationHeaderSelfContained PROPERTY LINK_LIBRARIES "")
if(MSVC)
    target_compile_options(TestMediaFoundationHeaderSelfContained PRIVATE /W3 /utf-8)
    set_target_properties(TestMediaFoundationHeaderSelfContained PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestMediaFoundationHeaderSelfContained
    COMMAND TestMediaFoundationHeaderSelfContained)
aura_set_test_labels(TestMediaFoundationHeaderSelfContained core)
add_executable(TestAudioFileReader EXCLUDE_FROM_ALL
    tests/test_stage6_audio_file_reader.cpp
    src/platform/audio_file_miniaudio.cpp
    tests/audio_file_miniaudio_test_seam.cpp
    tests/miniaudio_impl.cpp
)
target_sources(TestAudioFileReader PRIVATE ${AURO_AUDIO_TEXT_SOURCE})
function(_auro_assert_source_count target basename expected)
    get_target_property(_sources ${target} SOURCES)
    set(_count 0)
    foreach(_source IN LISTS _sources)
        get_filename_component(_name "${_source}" NAME)
        if(_name STREQUAL "${basename}")
            math(EXPR _count "${_count} + 1")
        endif()
    endforeach()
    if(NOT _count EQUAL ${expected})
        message(FATAL_ERROR "${target}: expected ${basename}=${expected}, got ${_count}")
    endif()
endfunction()
_auro_assert_source_count(TestAudioFileReader audio_file_miniaudio.cpp 1)
_auro_assert_source_count(TestAudioFileReader miniaudio_impl.cpp 1)
_auro_assert_source_count(TestAudioFileReader audio_file_miniaudio_test_seam.cpp 1)
_auro_assert_source_count(TestAudioFileReader "${_auro_audio_selected_text}" 1)
_auro_assert_source_count(TestAudioFileReader "${_auro_audio_unselected_text}" 0)
_auro_assert_source_count(TestAudioFileReader AudioEngine.cpp 0)
_auro_assert_source_count(HarkenSearch audio_file_miniaudio.cpp 1)
_auro_assert_source_count(HarkenSearch miniaudio_impl.cpp 0)
_auro_assert_source_count(HarkenSearch audio_file_miniaudio_test_seam.cpp 0)
_auro_assert_source_count(HarkenSearch AudioEngine.cpp 1)
_auro_assert_source_count(HarkenSearch "${_auro_audio_selected_text}" 1)
_auro_assert_source_count(HarkenSearch "${_auro_audio_unselected_text}" 0)
target_compile_definitions(TestAudioFileReader PRIVATE AURO_AUDIO_FILE_TESTING)
target_include_directories(TestAudioFileReader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
function(_auro_assert_include_count target include_path expected)
    get_target_property(_includes ${target} INCLUDE_DIRECTORIES)
    set(_count 0)
    foreach(_include IN LISTS _includes)
        if(_include STREQUAL "${include_path}")
            math(EXPR _count "${_count} + 1")
        endif()
    endforeach()
    if(NOT _count EQUAL ${expected})
        message(FATAL_ERROR "${target}: expected include ${include_path}=${expected}, got ${_count}")
    endif()
endfunction()
function(_auro_assert_compile_definition_count target definition expected)
    get_target_property(_definitions ${target} COMPILE_DEFINITIONS)
    if(_definitions MATCHES "-NOTFOUND$")
        set(_definitions "")
    endif()
    set(_count 0)
    foreach(_definition IN LISTS _definitions)
        if(_definition STREQUAL "${definition}" OR _definition MATCHES "^${definition}=")
            math(EXPR _count "${_count} + 1")
        endif()
    endforeach()
    if(NOT _count EQUAL ${expected})
        message(FATAL_ERROR "${target}: expected definition ${definition}=${expected}, got ${_count}")
    endif()
endfunction()
function(_auro_assert_link_library_count target expected)
    get_target_property(_links ${target} LINK_LIBRARIES)
    if(_links MATCHES "-NOTFOUND$")
        set(_links "")
    endif()
    list(LENGTH _links _count)
    if(NOT _count EQUAL ${expected})
        message(FATAL_ERROR "${target}: expected link library count=${expected}, got ${_count}: ${_links}")
    endif()
endfunction()
_auro_assert_include_count(TestAudioFileReader "${CMAKE_CURRENT_SOURCE_DIR}/tests" 1)
_auro_assert_include_count(HarkenSearch "${CMAKE_CURRENT_SOURCE_DIR}/tests" 0)
target_compile_features(TestAudioFileReader PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAudioFileReader PRIVATE /W3 /utf-8)
    set_target_properties(TestAudioFileReader PROPERTIES
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestAudioFileReaderBasic
    COMMAND TestAudioFileReader --group basic)
add_test(NAME TestAudioFileReaderStatus
    COMMAND TestAudioFileReader --group status)
aura_set_test_labels(TestAudioFileReaderBasic core)
aura_set_test_labels(TestAudioFileReaderStatus core)
add_executable(TestFeatureExtractorAudioFile EXCLUDE_FROM_ALL
    tests/test_feature_extractor_audio_file.cpp
    tests/audio_file_reader_scripted.cpp
    src/FeatureExtractor.cpp
)
target_include_directories(TestFeatureExtractorAudioFile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_features(TestFeatureExtractorAudioFile PRIVATE cxx_std_17)
_auro_assert_source_count(TestFeatureExtractorAudioFile audio_file_reader_scripted.cpp 1)
_auro_assert_source_count(TestFeatureExtractorAudioFile FeatureExtractor.cpp 1)
_auro_assert_source_count(TestFeatureExtractorAudioFile miniaudio_impl.cpp 0)
_auro_assert_source_count(TestFeatureExtractorAudioFile audio_file_miniaudio.cpp 0)
_auro_assert_source_count(TestFeatureExtractorAudioFile AudioEngine.cpp 0)
add_test(NAME TestFeatureExtractorAudioFile COMMAND TestFeatureExtractorAudioFile)
aura_set_test_labels(TestFeatureExtractorAudioFile core)
add_executable(TestKeyDetectorAudioFile EXCLUDE_FROM_ALL
    tests/test_key_detector_audio_file.cpp
    tests/audio_file_reader_scripted.cpp
    src/KeyDetector.cpp
)
target_include_directories(TestKeyDetectorAudioFile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_features(TestKeyDetectorAudioFile PRIVATE cxx_std_17)
_auro_assert_source_count(TestKeyDetectorAudioFile KeyDetector.cpp 1)
_auro_assert_source_count(TestKeyDetectorAudioFile audio_file_reader_scripted.cpp 1)
_auro_assert_source_count(TestKeyDetectorAudioFile miniaudio_impl.cpp 0)
_auro_assert_source_count(TestKeyDetectorAudioFile "${_auro_audio_selected_text}" 0)
_auro_assert_source_count(TestKeyDetectorAudioFile "${_auro_audio_unselected_text}" 0)
_auro_assert_source_count(TestKeyDetectorAudioFile audio_file_miniaudio.cpp 0)
_auro_assert_source_count(TestKeyDetectorAudioFile AudioEngine.cpp 0)
add_test(NAME TestKeyDetectorAudioFile COMMAND TestKeyDetectorAudioFile)
aura_set_test_labels(TestKeyDetectorAudioFile core)
add_executable(TestBpmDetectorAudioFile EXCLUDE_FROM_ALL
    tests/test_bpm_detector_audio_file.cpp
    tests/audio_file_reader_scripted.cpp
    src/BpmDetector.cpp
)
target_include_directories(TestBpmDetectorAudioFile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestBpmDetectorAudioFile PRIVATE cxx_std_17)
_auro_assert_source_count(TestBpmDetectorAudioFile BpmDetector.cpp 1)
_auro_assert_source_count(TestBpmDetectorAudioFile audio_file_reader_scripted.cpp 1)
_auro_assert_source_count(TestBpmDetectorAudioFile miniaudio_impl.cpp 0)
_auro_assert_source_count(TestBpmDetectorAudioFile audio_file_miniaudio.cpp 0)
_auro_assert_source_count(TestBpmDetectorAudioFile AudioEngine.cpp 0)
add_test(NAME TestBpmDetectorAudioFile COMMAND TestBpmDetectorAudioFile)
aura_set_test_labels(TestBpmDetectorAudioFile core)
add_executable(TestAudioFileHeaderSelfContained EXCLUDE_FROM_ALL
    tests/test_audio_file_header_self_contained.cpp
)
target_include_directories(TestAudioFileHeaderSelfContained PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestAudioFileHeaderSelfContained PRIVATE cxx_std_17)
set_property(TARGET TestAudioFileHeaderSelfContained PROPERTY LINK_LIBRARIES "")
if(MSVC)
    target_compile_options(TestAudioFileHeaderSelfContained PRIVATE /W3 /utf-8)
    set_target_properties(TestAudioFileHeaderSelfContained PROPERTIES
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestAudioFileHeaderSelfContained
    COMMAND TestAudioFileHeaderSelfContained)
aura_set_test_labels(TestAudioFileHeaderSelfContained core)
add_executable(TestDatabaseAudioProbeHeaderSelfContained EXCLUDE_FROM_ALL
    tests/test_database_audio_probe_header.cpp
)
set_property(TARGET TestDatabaseAudioProbeHeaderSelfContained PROPERTY LINK_LIBRARIES "")
target_include_directories(TestDatabaseAudioProbeHeaderSelfContained PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestDatabaseAudioProbeHeaderSelfContained PRIVATE cxx_std_17)
_auro_assert_source_count(TestDatabaseAudioProbeHeaderSelfContained test_database_audio_probe_header.cpp 1)
_auro_assert_source_count(HarkenSearch test_database_audio_probe_header.cpp 0)
foreach(_target IN ITEMS TestDatabaseAudioProbeHeaderSelfContained HarkenSearch)
    foreach(_source IN ITEMS
            database_audio_probe_path_utf8_adapter.cpp
            test_database_audio_probe_routing.cpp
            database_audio_probe_scripted_backend.cpp
            test_database_audio_probe_backend.cpp
            database_audio_probe_miniaudio_test_seam.cpp
            test_database_audio_probe_integration.cpp)
        _auro_assert_source_count(${_target} ${_source} 0)
    endforeach()
    foreach(_definition IN ITEMS
            AURO_DATABASE_AUDIO_PROBE_TEST_WIN32
            AURO_DATABASE_AUDIO_PROBE_TEST_STD
            AURO_DATABASE_AUDIO_PROBE_TESTING)
        _auro_assert_compile_definition_count(${_target} ${_definition} 0)
    endforeach()
    _auro_assert_include_count(${_target} "${CMAKE_CURRENT_SOURCE_DIR}/tests" 0)
endforeach()
foreach(_source IN ITEMS database_audio_probe.cpp database_audio_probe_miniaudio.cpp
        database_audio_probe_media_foundation.cpp)
    _auro_assert_source_count(TestDatabaseAudioProbeHeaderSelfContained ${_source} 0)
    _auro_assert_source_count(HarkenSearch ${_source} 1)
endforeach()
_auro_assert_source_count(HarkenSearch AudioEngine.cpp 1)
_auro_assert_source_count(HarkenSearch miniaudio_impl.cpp 0)
_auro_assert_link_library_count(TestDatabaseAudioProbeHeaderSelfContained 0)
if(MSVC)
    target_compile_options(TestDatabaseAudioProbeHeaderSelfContained PRIVATE /W3 /utf-8)
    set_target_properties(TestDatabaseAudioProbeHeaderSelfContained PROPERTIES
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestDatabaseAudioProbeHeaderSelfContained
    COMMAND TestDatabaseAudioProbeHeaderSelfContained)
aura_set_test_labels(TestDatabaseAudioProbeHeaderSelfContained core)
if(WIN32)
    set(AURO_DATABASE_PROBE_ROUTING_TEXT_SOURCE src/platform/text_encoding_win32.cpp)
else()
    set(AURO_DATABASE_PROBE_ROUTING_TEXT_SOURCE src/platform/text_encoding_std.cpp)
endif()
add_executable(TestDatabaseAudioProbeRoutingWin32 EXCLUDE_FROM_ALL
    tests/test_database_audio_probe_routing.cpp
    tests/database_audio_probe_scripted_backend.cpp
    tests/database_audio_probe_path_utf8_adapter.cpp
    src/platform/database_audio_probe.cpp
    ${AURO_DATABASE_PROBE_ROUTING_TEXT_SOURCE}
)
set_property(TARGET TestDatabaseAudioProbeRoutingWin32 PROPERTY LINK_LIBRARIES "")
add_executable(TestDatabaseAudioProbeRoutingStd EXCLUDE_FROM_ALL
    tests/test_database_audio_probe_routing.cpp
    tests/database_audio_probe_scripted_backend.cpp
    src/platform/database_audio_probe.cpp
)
set_property(TARGET TestDatabaseAudioProbeRoutingStd PROPERTY LINK_LIBRARIES "")
foreach(_target IN ITEMS TestDatabaseAudioProbeRoutingWin32 TestDatabaseAudioProbeRoutingStd)
    target_include_directories(${_target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_features(${_target} PRIVATE cxx_std_17)
    _auro_assert_source_count(${_target} test_database_audio_probe_routing.cpp 1)
    _auro_assert_source_count(${_target} database_audio_probe_scripted_backend.cpp 1)
    _auro_assert_source_count(${_target} database_audio_probe.cpp 1)
    target_compile_definitions(${_target} PRIVATE AURA_BUILD_DIAGNOSTICS=0)
    foreach(_source IN ITEMS database_audio_probe_miniaudio.cpp
            database_audio_probe_media_foundation.cpp
            miniaudio_impl.cpp AudioEngine.cpp)
        _auro_assert_source_count(${_target} ${_source} 0)
    endforeach()
    _auro_assert_compile_definition_count(${_target} AURO_DATABASE_AUDIO_PROBE_TESTING 0)
    _auro_assert_include_count(${_target} "${CMAKE_CURRENT_SOURCE_DIR}/tests" 1)
    _auro_assert_link_library_count(${_target} 0)
    if(MSVC)
        target_compile_options(${_target} PRIVATE /W3 /utf-8)
        set_target_properties(${_target} PROPERTIES
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE")
    endif()
endforeach()
target_compile_definitions(TestDatabaseAudioProbeRoutingWin32 PRIVATE
    AURO_DATABASE_AUDIO_PROBE_TEST_WIN32=1)
target_compile_definitions(TestDatabaseAudioProbeRoutingStd PRIVATE
    AURO_DATABASE_AUDIO_PROBE_TEST_STD=1)
_auro_assert_compile_definition_count(TestDatabaseAudioProbeRoutingWin32 AURO_DATABASE_AUDIO_PROBE_TEST_WIN32 1)
_auro_assert_compile_definition_count(TestDatabaseAudioProbeRoutingWin32 AURO_DATABASE_AUDIO_PROBE_TEST_STD 0)
_auro_assert_compile_definition_count(TestDatabaseAudioProbeRoutingStd AURO_DATABASE_AUDIO_PROBE_TEST_STD 1)
_auro_assert_compile_definition_count(TestDatabaseAudioProbeRoutingStd AURO_DATABASE_AUDIO_PROBE_TEST_WIN32 0)
_auro_assert_source_count(TestDatabaseAudioProbeRoutingWin32 database_audio_probe_path_utf8_adapter.cpp 1)
_auro_assert_source_count(TestDatabaseAudioProbeRoutingStd database_audio_probe_path_utf8_adapter.cpp 0)
if(WIN32)
    _auro_assert_source_count(TestDatabaseAudioProbeRoutingWin32 text_encoding_win32.cpp 1)
    _auro_assert_source_count(TestDatabaseAudioProbeRoutingWin32 text_encoding_std.cpp 0)
else()
    _auro_assert_source_count(TestDatabaseAudioProbeRoutingWin32 text_encoding_std.cpp 1)
    _auro_assert_source_count(TestDatabaseAudioProbeRoutingWin32 text_encoding_win32.cpp 0)
endif()
_auro_assert_source_count(TestDatabaseAudioProbeRoutingStd text_encoding_win32.cpp 0)
_auro_assert_source_count(TestDatabaseAudioProbeRoutingStd text_encoding_std.cpp 0)
_auro_assert_source_count(HarkenSearch database_audio_probe_path_utf8_adapter.cpp 0)
_auro_assert_source_count(HarkenSearch Utils.cpp 1)
add_test(NAME TestDatabaseAudioProbeRoutingWin32 COMMAND TestDatabaseAudioProbeRoutingWin32)
add_test(NAME TestDatabaseAudioProbeRoutingStd COMMAND TestDatabaseAudioProbeRoutingStd)
aura_set_test_labels(TestDatabaseAudioProbeRoutingWin32 core)
aura_set_test_labels(TestDatabaseAudioProbeRoutingStd core)
foreach(_policy IN ITEMS WIN32 STD)
    if("${_policy}" STREQUAL "WIN32")
        set(_suffix Win32)
    else()
        set(_suffix Std)
    endif()
    set(_target TestDatabaseAudioProbeBackend${_suffix})
    add_executable(${_target} EXCLUDE_FROM_ALL
        tests/test_database_audio_probe_backend.cpp
        tests/database_audio_probe_miniaudio_test_seam.cpp
        tests/miniaudio_impl.cpp
        src/platform/database_audio_probe_miniaudio.cpp)
    set_property(TARGET ${_target} PROPERTY LINK_LIBRARIES "")
    target_include_directories(${_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_features(${_target} PRIVATE cxx_std_17)
    target_compile_definitions(${_target} PRIVATE AURO_DATABASE_AUDIO_PROBE_TESTING=1 AURO_DATABASE_AUDIO_PROBE_TEST_${_policy}=1)
    _auro_assert_source_count(${_target} test_database_audio_probe_backend.cpp 1)
    _auro_assert_source_count(${_target} database_audio_probe_miniaudio_test_seam.cpp 1)
    _auro_assert_source_count(${_target} miniaudio_impl.cpp 1)
    _auro_assert_source_count(${_target} database_audio_probe_miniaudio.cpp 1)
    foreach(_source IN ITEMS database_audio_probe.cpp
            database_audio_probe_scripted_backend.cpp database_audio_probe_media_foundation.cpp
            database_audio_probe_path_utf8_adapter.cpp text_encoding_win32.cpp text_encoding_std.cpp AudioEngine.cpp)
        _auro_assert_source_count(${_target} ${_source} 0)
    endforeach()
    _auro_assert_compile_definition_count(${_target} AURO_DATABASE_AUDIO_PROBE_TESTING 1)
    _auro_assert_include_count(${_target} "${CMAKE_CURRENT_SOURCE_DIR}/tests" 1)
    _auro_assert_link_library_count(${_target} 0)
    if(MSVC)
        target_compile_options(${_target} PRIVATE /W3 /utf-8)
        set_target_properties(${_target} PROPERTIES
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE" LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE")
    endif()
    add_test(NAME ${_target} COMMAND ${_target})
    aura_set_test_labels(${_target} core)
endforeach()
_auro_assert_compile_definition_count(TestDatabaseAudioProbeBackendWin32 AURO_DATABASE_AUDIO_PROBE_TEST_WIN32 1)
_auro_assert_compile_definition_count(TestDatabaseAudioProbeBackendWin32 AURO_DATABASE_AUDIO_PROBE_TEST_STD 0)
_auro_assert_compile_definition_count(TestDatabaseAudioProbeBackendStd AURO_DATABASE_AUDIO_PROBE_TEST_STD 1)
_auro_assert_compile_definition_count(TestDatabaseAudioProbeBackendStd AURO_DATABASE_AUDIO_PROBE_TEST_WIN32 0)
_auro_assert_source_count(HarkenSearch database_audio_probe_miniaudio_test_seam.cpp 0)
add_executable(TestDatabaseAudioProbeIntegration EXCLUDE_FROM_ALL
    tests/test_database_audio_probe_integration.cpp
    tests/miniaudio_impl.cpp
    src/platform/database_audio_probe.cpp
    src/platform/database_audio_probe_miniaudio.cpp
    src/platform/database_audio_probe_media_foundation.cpp
    ${AURO_MEDIA_FOUNDATION_AUDIO_SOURCE})
set_property(TARGET TestDatabaseAudioProbeIntegration PROPERTY LINK_LIBRARIES)
if(WIN32)
    target_sources(TestDatabaseAudioProbeIntegration PRIVATE
        tests/database_audio_probe_path_utf8_adapter.cpp
        src/platform/text_encoding_win32.cpp)
    target_link_libraries(TestDatabaseAudioProbeIntegration PRIVATE
        mfplat mfreadwrite mfuuid)
endif()
target_include_directories(TestDatabaseAudioProbeIntegration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestDatabaseAudioProbeIntegration PRIVATE cxx_std_17)
target_compile_definitions(TestDatabaseAudioProbeIntegration PRIVATE AURA_BUILD_DIAGNOSTICS=0)
_auro_assert_source_count(TestDatabaseAudioProbeIntegration test_database_audio_probe_integration.cpp 1)
_auro_assert_source_count(TestDatabaseAudioProbeIntegration database_audio_probe.cpp 1)
_auro_assert_source_count(TestDatabaseAudioProbeIntegration database_audio_probe_miniaudio.cpp 1)
_auro_assert_source_count(TestDatabaseAudioProbeIntegration database_audio_probe_media_foundation.cpp 1)
_auro_assert_source_count(TestDatabaseAudioProbeIntegration miniaudio_impl.cpp 1)
foreach(_source IN ITEMS database_audio_probe_scripted_backend.cpp
        database_audio_probe_miniaudio_test_seam.cpp AudioEngine.cpp)
    _auro_assert_source_count(TestDatabaseAudioProbeIntegration ${_source} 0)
endforeach()
if(WIN32)
    set(_auro_database_probe_selected_mf media_foundation_audio_win32.cpp)
    set(_auro_database_probe_unselected_mf media_foundation_audio_unsupported.cpp)
else()
    set(_auro_database_probe_selected_mf media_foundation_audio_unsupported.cpp)
    set(_auro_database_probe_unselected_mf media_foundation_audio_win32.cpp)
endif()
foreach(_target IN ITEMS TestDatabaseAudioProbeIntegration HarkenSearch)
    _auro_assert_source_count(${_target} ${_auro_database_probe_selected_mf} 1)
    _auro_assert_source_count(${_target} ${_auro_database_probe_unselected_mf} 0)
endforeach()
foreach(_definition IN ITEMS AURO_DATABASE_AUDIO_PROBE_TESTING
        AURO_DATABASE_AUDIO_PROBE_TEST_WIN32 AURO_DATABASE_AUDIO_PROBE_TEST_STD)
    _auro_assert_compile_definition_count(TestDatabaseAudioProbeIntegration ${_definition} 0)
endforeach()
_auro_assert_include_count(TestDatabaseAudioProbeIntegration "${CMAKE_CURRENT_SOURCE_DIR}/tests" 0)
if(WIN32)
    _auro_assert_source_count(TestDatabaseAudioProbeIntegration database_audio_probe_path_utf8_adapter.cpp 1)
    _auro_assert_source_count(TestDatabaseAudioProbeIntegration text_encoding_win32.cpp 1)
    _auro_assert_source_count(TestDatabaseAudioProbeIntegration text_encoding_std.cpp 0)
    _auro_assert_link_library_count(TestDatabaseAudioProbeIntegration 3)
    get_target_property(_auro_database_probe_integration_links TestDatabaseAudioProbeIntegration LINK_LIBRARIES)
    if(NOT "${_auro_database_probe_integration_links}" STREQUAL "mfplat;mfreadwrite;mfuuid")
        message(FATAL_ERROR "TestDatabaseAudioProbeIntegration: unexpected links: ${_auro_database_probe_integration_links}")
    endif()
else()
    _auro_assert_source_count(TestDatabaseAudioProbeIntegration database_audio_probe_path_utf8_adapter.cpp 0)
    _auro_assert_source_count(TestDatabaseAudioProbeIntegration text_encoding_win32.cpp 0)
    _auro_assert_source_count(TestDatabaseAudioProbeIntegration text_encoding_std.cpp 0)
    _auro_assert_link_library_count(TestDatabaseAudioProbeIntegration 0)
endif()
if(MSVC)
    target_compile_options(TestDatabaseAudioProbeIntegration PRIVATE /W3 /utf-8)
    set_target_properties(TestDatabaseAudioProbeIntegration PROPERTIES
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE" LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestDatabaseAudioProbeIntegration COMMAND TestDatabaseAudioProbeIntegration)
aura_set_test_labels(TestDatabaseAudioProbeIntegration core)
add_executable(TestCenterSense EXCLUDE_FROM_ALL
    tests/test_center_sense.cpp
    src/CenterSenseDsp.cpp
)
target_include_directories(TestCenterSense PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestCenterSense PRIVATE /W3 /utf-8)
    set_target_properties(TestCenterSense PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCenterSense COMMAND TestCenterSense)
aura_set_test_labels(TestCenterSense core)

add_executable(TestFreqShift EXCLUDE_FROM_ALL
    tests/test_freq_shift.cpp
    src/CenterSenseDsp.cpp
)
target_include_directories(TestFreqShift PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestFreqShift PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestFreqShift PRIVATE /W3 /utf-8)
    set_target_properties(TestFreqShift PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestFreqShift COMMAND TestFreqShift)
aura_set_test_labels(TestFreqShift core)

add_executable(TestMagicEQDsp EXCLUDE_FROM_ALL
    tests/test_magic_eq_dsp.cpp
    src/CenterSenseDsp.cpp
)
target_include_directories(TestMagicEQDsp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestMagicEQDsp PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestMagicEQDsp PRIVATE /W3 /utf-8)
    set_target_properties(TestMagicEQDsp PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestMagicEQDsp COMMAND TestMagicEQDsp)
aura_set_test_labels(TestMagicEQDsp core)

if(WIN32)
    add_executable(TestMagicEQCaptureLoss EXCLUDE_FROM_ALL
        tests/test_magic_eq_capture_loss_win32.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
    target_include_directories(TestMagicEQCaptureLoss PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends)
    target_compile_definitions(TestMagicEQCaptureLoss PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
    target_compile_features(TestMagicEQCaptureLoss PRIVATE cxx_std_17)
    target_link_libraries(TestMagicEQCaptureLoss PRIVATE user32 gdi32 imm32 dwmapi)
    if(MSVC)
        target_compile_options(TestMagicEQCaptureLoss PRIVATE /W3 /utf-8)
    endif()
    add_test(NAME TestMagicEQCaptureLoss COMMAND TestMagicEQCaptureLoss)
    aura_set_test_labels(TestMagicEQCaptureLoss core)
endif()

add_test(NAME TestMagicEQUiContract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_magic_eq_ui_contract.py)
aura_set_test_labels(TestMagicEQUiContract core)
add_test(NAME TestMagicEQScopeContract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_magic_eq_scope_contract.py)
aura_set_test_labels(TestMagicEQScopeContract core)
add_test(NAME TestMagicEQSpectrumContract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_magic_eq_spectrum_contract.py)
aura_set_test_labels(TestMagicEQSpectrumContract audio)
add_test(NAME TestMagicEQDawDeliveryContract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_magic_eq_daw_delivery_contract.py)
aura_set_test_labels(TestMagicEQDawDeliveryContract audio)
add_test(NAME TestMagicEQMonoPlaybackContract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_magic_eq_mono_playback_contract.py)
aura_set_test_labels(TestMagicEQMonoPlaybackContract core)
add_executable(TestMagicEQLocalization EXCLUDE_FROM_ALL
    tests/test_magic_eq_localization.cpp
    src/Localization.cpp)
add_dependencies(TestMagicEQLocalization ArisLocalizationGenerated)
target_include_directories(TestMagicEQLocalization PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
if(MSVC)
    target_compile_options(TestMagicEQLocalization PRIVATE /W3 /utf-8)
    set_target_properties(TestMagicEQLocalization PROPERTIES
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestMagicEQLocalization COMMAND TestMagicEQLocalization)
aura_set_test_labels(TestMagicEQLocalization core)

add_executable(TestCenterSenseDataSource EXCLUDE_FROM_ALL
    tests/test_center_sense_data_source.cpp
    tests/miniaudio_impl.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/platform/audio_data_source_miniaudio.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestCenterSenseDataSource PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestCenterSenseDataSource PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestCenterSenseDataSource PRIVATE /W3 /utf-8)
    set_target_properties(TestCenterSenseDataSource PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCenterSenseDataSource COMMAND TestCenterSenseDataSource)
aura_set_test_labels(TestCenterSenseDataSource core)


# TimeStretch WSOLA unit tests (F-019)

add_executable(TestTimeStretch EXCLUDE_FROM_ALL
    test_time_stretch.cpp
    src/TimeStretch.cpp
)
target_include_directories(TestTimeStretch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(TestTimeStretch PRIVATE /W3 /utf-8)
    set_target_properties(TestTimeStretch PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestTimeStretch COMMAND TestTimeStretch)
aura_set_test_labels(TestTimeStretch core)

# DiagnosticsLogger filter quality: Metric exclusion, run collapse, maxLines

add_executable(TestTimeStretch2 EXCLUDE_FROM_ALL
    tests/test_time_stretch.cpp
    src/TimeStretch.cpp
)
target_include_directories(TestTimeStretch2 PRIVATE src)
target_compile_definitions(TestTimeStretch2 PRIVATE NOMINMAX)
target_compile_features(TestTimeStretch2 PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestTimeStretch2 PRIVATE /W3 /utf-8)
    set_target_properties(TestTimeStretch2 PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestTimeStretch2 COMMAND TestTimeStretch2)
aura_set_test_labels(TestTimeStretch2 core)

# Tier 4: External dependency modules

# KeyDetector: musical key detection via chromagram and Krumhansl-Schmuckler

add_executable(TestChannelRoutingSettings EXCLUDE_FROM_ALL
    test_channel_routing_settings.cpp
)
target_include_directories(TestChannelRoutingSettings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(TestChannelRoutingSettings PRIVATE /W3 /utf-8)
    set_target_properties(TestChannelRoutingSettings PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestChannelRoutingSettings COMMAND TestChannelRoutingSettings)
aura_set_test_labels(TestChannelRoutingSettings core)

add_executable(TestMultichannelMeter EXCLUDE_FROM_ALL
    tests/test_multichannel_meter.cpp
)
target_include_directories(TestMultichannelMeter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(TestMultichannelMeter PRIVATE /W3 /utf-8)
    set_target_properties(TestMultichannelMeter PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestMultichannelMeter COMMAND TestMultichannelMeter)
aura_set_test_labels(TestMultichannelMeter core)

# F-019-channel: export one native channel from a multi-channel file.

add_executable(TestProcessedFileExport EXCLUDE_FROM_ALL
    tests/test_processed_file_export.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/CoverReplacementFoundation.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/NativeFloatReader.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/TimeStretch.cpp
    ${SQLITE_SOURCES}
)
if(WIN32)
    target_sources(TestProcessedFileExport PRIVATE src/platform/filesystem_win32.cpp)
endif()
target_include_directories(TestProcessedFileExport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestProcessedFileExport PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestProcessedFileExport PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5)
if(MSVC)
    target_compile_options(TestProcessedFileExport PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestProcessedFileExport PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestProcessedFileExport COMMAND TestProcessedFileExport)
aura_set_test_labels(TestProcessedFileExport audio)

# NOT-keyword chip tokenizer unit test

add_executable(TestSingleChannelExport EXCLUDE_FROM_ALL
    tests/test_single_channel_export.cpp
    src/SingleChannelExportWorkflow.cpp
    src/SingleChannelExportCompletionWorkflow.cpp
    src/TransferEngine.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/CoverReplacementFoundation.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/NativeFloatReader.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/filesystem_win32.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
    src/TimeStretch.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestSingleChannelExport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestSingleChannelExport PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestSingleChannelExport PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5)
if(MSVC)
    target_compile_options(TestSingleChannelExport PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestSingleChannelExport PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSingleChannelExport COMMAND TestSingleChannelExport)
aura_set_test_labels(TestSingleChannelExport audio)

add_test(
    NAME TestSingleChannelExportWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_single_channel_export_workflow_boundary.py
)
set_tests_properties(TestSingleChannelExportWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestSingleChannelExportWorkflowBoundary core stage4)

add_test(
    NAME TestSingleChannelExportCompletionWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_single_channel_export_completion_workflow_boundary.py
)
set_tests_properties(TestSingleChannelExportCompletionWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestSingleChannelExportCompletionWorkflowBoundary core stage4)

add_test(
    NAME TestCoverKeyCachePersistenceWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_key_cache_persistence_workflow_boundary.py
)
set_tests_properties(TestCoverKeyCachePersistenceWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestCoverKeyCachePersistenceWorkflowBoundary core stage4)

add_test(
    NAME TestDeferredSidebarBootstrapWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_deferred_sidebar_bootstrap_workflow_boundary.py
)
set_tests_properties(TestDeferredSidebarBootstrapWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestDeferredSidebarBootstrapWorkflowBoundary core stage4)

add_test(
    NAME TestReaperFileListImportWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_file_list_import_workflow_boundary.py
)
set_tests_properties(TestReaperFileListImportWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestReaperFileListImportWorkflowBoundary core stage4)

# Processed full-file export used by multi-file drag/send transfer.

add_executable(TestSilenceSkipGate EXCLUDE_FROM_ALL
    tests/test_silence_skip_gate.cpp
)
target_include_directories(TestSilenceSkipGate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestSilenceSkipGate PRIVATE /W3 /utf-8)
    set_target_properties(TestSilenceSkipGate PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG   "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSilenceSkipGate COMMAND TestSilenceSkipGate)
aura_set_test_labels(TestSilenceSkipGate core)

# Sidebar section order parser + drag-reorder (VS Code-style Activity Bar)

add_executable(TestSearchWorkspace EXCLUDE_FROM_ALL
    tests/test_search_workspace.cpp
    src/WaveformRebuildRuntimeState.cpp
    src/DatabaseSessionMediaCacheReset.cpp
    src/DatabaseSessionScanStateReset.cpp
    src/DatabaseSessionViewStateReset.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/CoverReplacementFoundation.cpp
    src/BackfillQueue.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/TimeStretch.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestSearchWorkspace PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestSearchWorkspace PRIVATE
    opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm
    mfplat mfreadwrite mfuuid)
target_compile_definitions(TestSearchWorkspace PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SQLITE_ENABLE_FTS5
    AURA_TEST_STUB_BACKFILL_SCAN_WRITE_GUARD
)
if(MSVC)
    target_compile_options(TestSearchWorkspace PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestSearchWorkspace PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSearchWorkspace COMMAND TestSearchWorkspace)
aura_set_test_labels(TestSearchWorkspace core)

add_executable(TestTagCloudContext EXCLUDE_FROM_ALL
    tests/test_tag_cloud_context.cpp
)
target_include_directories(TestTagCloudContext PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestTagCloudContext PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestTagCloudContext PRIVATE /W3 /utf-8)
    set_target_properties(TestTagCloudContext PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestTagCloudContext COMMAND TestTagCloudContext)
aura_set_test_labels(TestTagCloudContext core stage4)

add_test(
    NAME TestTagCloudContextBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tag_cloud_context_boundary.py
)
set_tests_properties(TestTagCloudContextBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestTagCloudContextBoundary core stage4)

add_test(
    NAME TestDatabaseSessionViewStateResetBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_session_view_state_reset_boundary.py"
)
set_tests_properties(TestDatabaseSessionViewStateResetBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
aura_set_test_labels(TestDatabaseSessionViewStateResetBoundary core stage4)

add_test(
    NAME TestDatabaseSessionScanStateResetBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_session_scan_state_reset_boundary.py"
)
set_tests_properties(TestDatabaseSessionScanStateResetBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
aura_set_test_labels(TestDatabaseSessionScanStateResetBoundary core stage4)

add_test(
    NAME TestDatabaseSessionMediaCacheResetBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_session_media_cache_reset_boundary.py"
)
set_tests_properties(TestDatabaseSessionMediaCacheResetBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestDatabaseSessionMediaCacheResetBoundary core stage4)

add_test(
    NAME TestWaveformRebuildRuntimeStateBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_rebuild_runtime_state_boundary.py"
)
set_tests_properties(TestWaveformRebuildRuntimeStateBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestWaveformRebuildRuntimeStateBoundary core stage4)

add_executable(TestWaveformRebuildBatchSelector EXCLUDE_FROM_ALL
    tests/test_waveform_rebuild_batch_selector.cpp
    src/WaveformRebuildBatchSelector.cpp
)
target_include_directories(TestWaveformRebuildBatchSelector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestWaveformRebuildBatchSelector PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestWaveformRebuildBatchSelector COMMAND TestWaveformRebuildBatchSelector)
aura_set_test_labels(TestWaveformRebuildBatchSelector core db)

add_test(
    NAME TestWaveformRebuildBatchSelectorBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_rebuild_batch_selector_boundary.py"
)
set_tests_properties(TestWaveformRebuildBatchSelectorBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestWaveformRebuildBatchSelectorBoundary core stage4)

add_executable(TestWaveformRebuildFailurePersistence EXCLUDE_FROM_ALL
    tests/test_waveform_rebuild_failure_persistence.cpp
    src/WaveformRebuildFailurePersistence.cpp
)
target_include_directories(TestWaveformRebuildFailurePersistence PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestWaveformRebuildFailurePersistence PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestWaveformRebuildFailurePersistence
    COMMAND TestWaveformRebuildFailurePersistence
)
aura_set_test_labels(TestWaveformRebuildFailurePersistence core db)

add_test(
    NAME TestWaveformRebuildFailurePersistenceBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_rebuild_failure_persistence_boundary.py"
)
set_tests_properties(TestWaveformRebuildFailurePersistenceBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestWaveformRebuildFailurePersistenceBoundary core stage4)

add_executable(TestWaveformRebuildRequestSubmission EXCLUDE_FROM_ALL
    tests/test_waveform_rebuild_request_submission.cpp
    src/WaveformRebuildRequestSubmission.cpp
)
target_include_directories(TestWaveformRebuildRequestSubmission PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestWaveformRebuildRequestSubmission PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestWaveformRebuildRequestSubmission
    COMMAND TestWaveformRebuildRequestSubmission
)
aura_set_test_labels(TestWaveformRebuildRequestSubmission core audio)

add_test(
    NAME TestWaveformRebuildRequestSubmissionBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_rebuild_request_submission_boundary.py"
)
set_tests_properties(TestWaveformRebuildRequestSubmissionBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestWaveformRebuildRequestSubmissionBoundary core stage4)

add_executable(TestAdaptiveMiniWaveformFormatFinalization EXCLUDE_FROM_ALL
    tests/test_adaptive_mini_waveform_format_finalization.cpp
    src/AdaptiveMiniWaveformFormatFinalization.cpp
)
target_include_directories(TestAdaptiveMiniWaveformFormatFinalization PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAdaptiveMiniWaveformFormatFinalization PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestAdaptiveMiniWaveformFormatFinalization
    COMMAND TestAdaptiveMiniWaveformFormatFinalization
)
aura_set_test_labels(TestAdaptiveMiniWaveformFormatFinalization core db)

add_test(
    NAME TestAdaptiveMiniWaveformFormatFinalizationBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_adaptive_mini_waveform_format_finalization_boundary.py"
)
set_tests_properties(TestAdaptiveMiniWaveformFormatFinalizationBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAdaptiveMiniWaveformFormatFinalizationBoundary core stage4)

add_executable(TestAdaptiveMiniWaveformFormatPreparation EXCLUDE_FROM_ALL
    tests/test_adaptive_mini_waveform_format_preparation.cpp
    src/AdaptiveMiniWaveformFormatPreparation.cpp
)
target_include_directories(TestAdaptiveMiniWaveformFormatPreparation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAdaptiveMiniWaveformFormatPreparation PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestAdaptiveMiniWaveformFormatPreparation
    COMMAND TestAdaptiveMiniWaveformFormatPreparation
)
aura_set_test_labels(TestAdaptiveMiniWaveformFormatPreparation core db)

add_test(
    NAME TestAdaptiveMiniWaveformFormatPreparationBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_adaptive_mini_waveform_format_preparation_boundary.py"
)
set_tests_properties(TestAdaptiveMiniWaveformFormatPreparationBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAdaptiveMiniWaveformFormatPreparationBoundary core stage4)

add_executable(TestAdaptiveMiniWaveformRebuildActivation EXCLUDE_FROM_ALL
    tests/test_adaptive_mini_waveform_rebuild_activation.cpp
    src/AdaptiveMiniWaveformRebuildActivation.cpp
)
target_include_directories(TestAdaptiveMiniWaveformRebuildActivation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAdaptiveMiniWaveformRebuildActivation PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestAdaptiveMiniWaveformRebuildActivation
    COMMAND TestAdaptiveMiniWaveformRebuildActivation
)
aura_set_test_labels(TestAdaptiveMiniWaveformRebuildActivation core)

add_test(
    NAME TestAdaptiveMiniWaveformRebuildActivationBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_adaptive_mini_waveform_rebuild_activation_boundary.py"
)
set_tests_properties(TestAdaptiveMiniWaveformRebuildActivationBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAdaptiveMiniWaveformRebuildActivationBoundary core stage4)

add_executable(TestWaveformRebuildPumpGate EXCLUDE_FROM_ALL
    tests/test_waveform_rebuild_pump_gate.cpp
    src/WaveformRebuildPumpGate.cpp
)
target_include_directories(TestWaveformRebuildPumpGate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestWaveformRebuildPumpGate PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestWaveformRebuildPumpGate
    COMMAND TestWaveformRebuildPumpGate
)
aura_set_test_labels(TestWaveformRebuildPumpGate core)

add_test(
    NAME TestWaveformRebuildPumpGateBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_rebuild_pump_gate_boundary.py"
)
set_tests_properties(TestWaveformRebuildPumpGateBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestWaveformRebuildPumpGateBoundary core stage4)

add_executable(TestOriginalAudioWriteConfirmGate EXCLUDE_FROM_ALL
    tests/test_original_audio_write_confirm_gate.cpp
    src/OriginalAudioWriteConfirmGate.cpp
)
target_include_directories(TestOriginalAudioWriteConfirmGate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestOriginalAudioWriteConfirmGate PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestOriginalAudioWriteConfirmGate
    COMMAND TestOriginalAudioWriteConfirmGate
)
aura_set_test_labels(TestOriginalAudioWriteConfirmGate core)

add_executable(TestAuditionPluginChain EXCLUDE_FROM_ALL
    tests/test_audition_plugin_chain.cpp
    src/AuditionPluginChain.cpp
)
target_include_directories(TestAuditionPluginChain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAuditionPluginChain PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestAuditionPluginChain
    COMMAND TestAuditionPluginChain
)
aura_set_test_labels(TestAuditionPluginChain core)

add_executable(TestLoudnessFromSamples EXCLUDE_FROM_ALL
    tests/test_loudness_from_samples.cpp
    src/AudioEngine.cpp
    src/DiagnosticsLogger.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/audio_file_miniaudio.cpp
    src/platform/filesystem_std.cpp
    src/platform/emergency_file_writer_win32.cpp
    src/platform/thread_id_win32.cpp
    src/TimeStretch.cpp
    src/WaveformExtractor.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/platform/media_foundation_audio_win32.cpp
    src/NativeFloatReader.cpp
)
target_include_directories(TestLoudnessFromSamples PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui
)
target_compile_features(TestLoudnessFromSamples PRIVATE cxx_std_17)
target_compile_definitions(TestLoudnessFromSamples PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    AURO_TEST_EXTERNAL_QUERY_LENGTH
)
if(WIN32)
    target_sources(TestLoudnessFromSamples PRIVATE
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_link_libraries(TestLoudnessFromSamples PRIVATE ole32 winmm)
    target_link_libraries(TestLoudnessFromSamples PRIVATE mfplat mfreadwrite mfuuid)
endif()
if(MSVC)
    target_compile_options(TestLoudnessFromSamples PRIVATE /W3 /utf-8 /Gy)
    set_target_properties(TestLoudnessFromSamples PROPERTIES
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE /OPT:REF /INCREMENTAL:NO"
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE /OPT:REF /INCREMENTAL:NO"
    )
    target_link_options(TestLoudnessFromSamples PRIVATE /OPT:REF /INCREMENTAL:NO)
endif()
set_property(TARGET TestLoudnessFromSamples PROPERTY VS_GLOBAL_OptimizeReferences true)
set_property(TARGET TestLoudnessFromSamples PROPERTY VS_GLOBAL_EnableCOMDATFolding true)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Testing/loudness-from-samples")
add_test(
    NAME TestLoudnessFromSamples
    COMMAND TestLoudnessFromSamples
)
set_tests_properties(TestLoudnessFromSamples PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Testing/loudness-from-samples"
    TIMEOUT 60
)
aura_set_test_labels(TestLoudnessFromSamples audio)

add_test(
    NAME TestOriginalAudioWriteConfirmGateBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_audio_write_confirm_gate_boundary.py"
)
set_tests_properties(TestOriginalAudioWriteConfirmGateBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestOriginalAudioWriteConfirmGateBoundary core stage4)

if(WIN32)
    add_executable(TestOriginalAudioWriteConfirmIo EXCLUDE_FROM_ALL
        tests/test_original_audio_write_confirm_io.cpp
        src/OriginalAudioWriteConfirmGate.cpp
        src/MetadataWriter.cpp
        src/MetadataReader.cpp
        src/platform/filesystem_std.cpp
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
        src/platform/metadata_legacy_tag_content_win32.cpp
    )
    target_include_directories(TestOriginalAudioWriteConfirmIo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestOriginalAudioWriteConfirmIo PRIVATE cxx_std_17)
    target_compile_definitions(TestOriginalAudioWriteConfirmIo PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        AURO_FILESYSTEM_PROBE_PATH_TESTING
    )
    if(MSVC)
        target_compile_options(TestOriginalAudioWriteConfirmIo PRIVATE /W3 /utf-8)
        set_target_properties(TestOriginalAudioWriteConfirmIo PROPERTIES
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        )
    endif()
    add_test(NAME TestOriginalAudioWriteConfirmIo COMMAND TestOriginalAudioWriteConfirmIo)
    aura_set_test_labels(TestOriginalAudioWriteConfirmIo core)
endif()

add_executable(TestWaveformRebuildRecordWorkflow EXCLUDE_FROM_ALL
    tests/test_waveform_rebuild_record_workflow.cpp
    src/WaveformRebuildRecordWorkflow.cpp
    src/WaveformRebuildFailurePersistence.cpp
    src/WaveformRebuildRequestSubmission.cpp
)
target_include_directories(TestWaveformRebuildRecordWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestWaveformRebuildRecordWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestWaveformRebuildRecordWorkflow
    COMMAND TestWaveformRebuildRecordWorkflow
)
aura_set_test_labels(TestWaveformRebuildRecordWorkflow core audio)

add_test(
    NAME TestWaveformRebuildRecordWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_rebuild_record_workflow_boundary.py"
)
set_tests_properties(TestWaveformRebuildRecordWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestWaveformRebuildRecordWorkflowBoundary core stage4)

add_executable(TestWaveformRebuildBatchLifecycle EXCLUDE_FROM_ALL
    tests/test_waveform_rebuild_batch_lifecycle.cpp
    src/WaveformRebuildBatchLifecycle.cpp
    src/WaveformRebuildBatchSelector.cpp
    src/AdaptiveMiniWaveformFormatFinalization.cpp
)
target_include_directories(TestWaveformRebuildBatchLifecycle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestWaveformRebuildBatchLifecycle PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestWaveformRebuildBatchLifecycle
    COMMAND TestWaveformRebuildBatchLifecycle
)
aura_set_test_labels(TestWaveformRebuildBatchLifecycle core db)

add_test(
    NAME TestWaveformRebuildBatchLifecycleBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_rebuild_batch_lifecycle_boundary.py"
)
set_tests_properties(TestWaveformRebuildBatchLifecycleBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestWaveformRebuildBatchLifecycleBoundary core stage4)

add_executable(TestWaveformRebuildCancellationRecovery EXCLUDE_FROM_ALL
    tests/test_waveform_rebuild_cancellation_recovery.cpp
    src/WaveformRebuildCancellationRecovery.cpp
    src/AdaptiveMiniWaveformFormatFinalization.cpp
)
target_include_directories(TestWaveformRebuildCancellationRecovery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestWaveformRebuildCancellationRecovery PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestWaveformRebuildCancellationRecovery
    COMMAND TestWaveformRebuildCancellationRecovery
)
aura_set_test_labels(TestWaveformRebuildCancellationRecovery core db)

add_test(
    NAME TestWaveformRebuildCancellationRecoveryBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_rebuild_cancellation_recovery_boundary.py"
)
set_tests_properties(TestWaveformRebuildCancellationRecoveryBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestWaveformRebuildCancellationRecoveryBoundary core stage4)

add_executable(TestScanWaveformBackfillDrainWorkflow EXCLUDE_FROM_ALL
    tests/test_scan_waveform_backfill_drain_workflow.cpp
    src/ScanWaveformBackfillDrainWorkflow.cpp
)
target_include_directories(TestScanWaveformBackfillDrainWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestScanWaveformBackfillDrainWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestScanWaveformBackfillDrainWorkflow
    COMMAND TestScanWaveformBackfillDrainWorkflow
)
aura_set_test_labels(TestScanWaveformBackfillDrainWorkflow core db)

add_test(
    NAME TestScanWaveformBackfillDrainWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scan_waveform_backfill_drain_workflow_boundary.py"
)
set_tests_properties(TestScanWaveformBackfillDrainWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestScanWaveformBackfillDrainWorkflowBoundary core stage4)

add_executable(TestSelectedWaveformRebuildWorkflow EXCLUDE_FROM_ALL
    tests/test_selected_waveform_rebuild_workflow.cpp
    src/SelectedWaveformRebuildWorkflow.cpp
    src/WaveformRebuildRequestSubmission.cpp
)
target_include_directories(TestSelectedWaveformRebuildWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestSelectedWaveformRebuildWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestSelectedWaveformRebuildWorkflow
    COMMAND TestSelectedWaveformRebuildWorkflow
)
aura_set_test_labels(TestSelectedWaveformRebuildWorkflow core audio)

add_test(
    NAME TestSelectedWaveformRebuildWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_selected_waveform_rebuild_workflow_boundary.py"
)
set_tests_properties(TestSelectedWaveformRebuildWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestSelectedWaveformRebuildWorkflowBoundary core stage4)

add_executable(TestSelectedRecordRescanWorkflow EXCLUDE_FROM_ALL
    tests/test_selected_record_rescan_workflow.cpp
    src/SelectedRecordRescanWorkflow.cpp
)
target_include_directories(TestSelectedRecordRescanWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestSelectedRecordRescanWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestSelectedRecordRescanWorkflow
    COMMAND TestSelectedRecordRescanWorkflow
)
aura_set_test_labels(TestSelectedRecordRescanWorkflow core db)

add_test(
    NAME TestSelectedRecordRescanWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_selected_record_rescan_workflow_boundary.py"
)
set_tests_properties(TestSelectedRecordRescanWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestSelectedRecordRescanWorkflowBoundary core stage4)

add_executable(TestBpmBatchScanWorkflow EXCLUDE_FROM_ALL
    tests/test_bpm_batch_scan_workflow.cpp
    src/BpmBatchScanWorkflow.cpp
)
target_include_directories(TestBpmBatchScanWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestBpmBatchScanWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestBpmBatchScanWorkflow
    COMMAND TestBpmBatchScanWorkflow
)
aura_set_test_labels(TestBpmBatchScanWorkflow core audio db)

add_test(
    NAME TestBpmBatchScanWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bpm_batch_scan_workflow_boundary.py"
)
set_tests_properties(TestBpmBatchScanWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestBpmBatchScanWorkflowBoundary core stage4)

add_executable(TestBpmBatchResultWorkflow EXCLUDE_FROM_ALL
    tests/test_bpm_batch_result_workflow.cpp
    src/BpmBatchResultWorkflow.cpp
)
target_include_directories(TestBpmBatchResultWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestBpmBatchResultWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestBpmBatchResultWorkflow
    COMMAND TestBpmBatchResultWorkflow
)
aura_set_test_labels(TestBpmBatchResultWorkflow core audio db)

add_test(
    NAME TestBpmBatchResultWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bpm_batch_result_workflow_boundary.py"
)
set_tests_properties(TestBpmBatchResultWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestBpmBatchResultWorkflowBoundary core stage4)

add_executable(TestSingleMusicAnalysisResultWorkflow EXCLUDE_FROM_ALL
    tests/test_single_music_analysis_result_workflow.cpp
    src/SingleMusicAnalysisResultWorkflow.cpp
)
target_include_directories(TestSingleMusicAnalysisResultWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestSingleMusicAnalysisResultWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestSingleMusicAnalysisResultWorkflow
    COMMAND TestSingleMusicAnalysisResultWorkflow
)
aura_set_test_labels(TestSingleMusicAnalysisResultWorkflow core audio db)

add_test(
    NAME TestSingleMusicAnalysisResultWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_single_music_analysis_result_workflow_boundary.py"
)
set_tests_properties(TestSingleMusicAnalysisResultWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestSingleMusicAnalysisResultWorkflowBoundary core stage4)

add_executable(TestBpmBatchCompletionWorkflow EXCLUDE_FROM_ALL
    tests/test_bpm_batch_completion_workflow.cpp
    src/BpmBatchCompletionWorkflow.cpp
)
target_include_directories(TestBpmBatchCompletionWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestBpmBatchCompletionWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestBpmBatchCompletionWorkflow COMMAND TestBpmBatchCompletionWorkflow)
aura_set_test_labels(TestBpmBatchCompletionWorkflow core audio)

add_test(
    NAME TestBpmBatchCompletionWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bpm_batch_completion_workflow_boundary.py"
)
set_tests_properties(TestBpmBatchCompletionWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestBpmBatchCompletionWorkflowBoundary core stage4)

add_executable(TestCoverBatchOperationWorkflow EXCLUDE_FROM_ALL
    tests/test_cover_batch_operation_workflow.cpp
    src/CoverBatchOperationWorkflow.cpp
)
target_include_directories(TestCoverBatchOperationWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestCoverBatchOperationWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestCoverBatchOperationWorkflow COMMAND TestCoverBatchOperationWorkflow)
aura_set_test_labels(TestCoverBatchOperationWorkflow core db)

add_test(
    NAME TestCoverBatchOperationWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_batch_operation_workflow_boundary.py"
)
set_tests_properties(TestCoverBatchOperationWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverBatchOperationWorkflowBoundary core stage4)

add_executable(TestEmbedMetadataBatchRuntime EXCLUDE_FROM_ALL
    tests/test_embed_metadata_batch_runtime.cpp
    src/EmbedMetadataBatchRuntime.cpp
)
target_include_directories(TestEmbedMetadataBatchRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestEmbedMetadataBatchRuntime PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestEmbedMetadataBatchRuntime COMMAND TestEmbedMetadataBatchRuntime)
aura_set_test_labels(TestEmbedMetadataBatchRuntime core db)

add_test(
    NAME TestEmbedMetadataBatchRuntimeBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_embed_metadata_batch_runtime_boundary.py"
)
set_tests_properties(TestEmbedMetadataBatchRuntimeBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestEmbedMetadataBatchRuntimeBoundary core stage4)

add_executable(TestCoverRebuildRecordProcessor EXCLUDE_FROM_ALL
    tests/test_cover_rebuild_record_processor.cpp
    src/CoverRebuildRecordProcessor.cpp
)
target_include_directories(TestCoverRebuildRecordProcessor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestCoverRebuildRecordProcessor PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestCoverRebuildRecordProcessor COMMAND TestCoverRebuildRecordProcessor)
aura_set_test_labels(TestCoverRebuildRecordProcessor core db)

add_test(
    NAME TestCoverRebuildRecordProcessorBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_rebuild_record_processor_boundary.py"
)
set_tests_properties(TestCoverRebuildRecordProcessorBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverRebuildRecordProcessorBoundary core stage4)

add_executable(TestCoverRebuildLifecycleWorkflow EXCLUDE_FROM_ALL
    tests/test_cover_rebuild_lifecycle_workflow.cpp
    src/CoverRebuildLifecycleWorkflow.cpp
)
target_include_directories(TestCoverRebuildLifecycleWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestCoverRebuildLifecycleWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestCoverRebuildLifecycleWorkflow COMMAND TestCoverRebuildLifecycleWorkflow)
aura_set_test_labels(TestCoverRebuildLifecycleWorkflow core db)

add_test(
    NAME TestCoverRebuildLifecycleWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_rebuild_lifecycle_workflow_boundary.py"
)
set_tests_properties(TestCoverRebuildLifecycleWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverRebuildLifecycleWorkflowBoundary core stage4)

add_executable(TestCoverRebuildBatchCommitWorkflow EXCLUDE_FROM_ALL
    tests/test_cover_rebuild_batch_commit_workflow.cpp
    src/CoverRebuildBatchCommitWorkflow.cpp
)
target_include_directories(TestCoverRebuildBatchCommitWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestCoverRebuildBatchCommitWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestCoverRebuildBatchCommitWorkflow COMMAND TestCoverRebuildBatchCommitWorkflow)
aura_set_test_labels(TestCoverRebuildBatchCommitWorkflow core db)

add_test(
    NAME TestCoverRebuildBatchCommitWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_rebuild_batch_commit_workflow_boundary.py"
)
set_tests_properties(TestCoverRebuildBatchCommitWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverRebuildBatchCommitWorkflowBoundary core stage4)

add_executable(TestLoadAllBatchRuntime EXCLUDE_FROM_ALL
    tests/test_load_all_batch_runtime.cpp
    src/LoadAllBatchRuntime.cpp
)
target_include_directories(TestLoadAllBatchRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestLoadAllBatchRuntime PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestLoadAllBatchRuntime COMMAND TestLoadAllBatchRuntime)
aura_set_test_labels(TestLoadAllBatchRuntime core)

add_test(
    NAME TestLoadAllBatchRuntimeBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_load_all_batch_runtime_boundary.py"
        -q
)
set_tests_properties(TestLoadAllBatchRuntimeBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestLoadAllBatchRuntimeBoundary core stage4)

add_test(
    NAME TestLoadAllBatchRuntimeStage6
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_load_all_batch_runtime_stage6.py"
        -q
)
set_tests_properties(TestLoadAllBatchRuntimeStage6 PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestLoadAllBatchRuntimeStage6 core stage4)

add_executable(TestSameFolderScanWorkflow EXCLUDE_FROM_ALL
    tests/test_same_folder_scan_workflow.cpp
    src/SameFolderScanWorkflow.cpp
)
target_include_directories(TestSameFolderScanWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestSameFolderScanWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestSameFolderScanWorkflow COMMAND TestSameFolderScanWorkflow)
aura_set_test_labels(TestSameFolderScanWorkflow core)

add_test(
    NAME TestSameFolderScanWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_same_folder_scan_workflow_boundary.py"
        -q
)
set_tests_properties(TestSameFolderScanWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestSameFolderScanWorkflowBoundary core stage4)

add_test(
    NAME TestSameFolderScanWorkflowStage6
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_same_folder_scan_workflow_stage6.py"
        -q
)
set_tests_properties(TestSameFolderScanWorkflowStage6 PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestSameFolderScanWorkflowStage6 core stage4)

add_test(
    NAME TestAITranscriptionResultStateAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ai_transcription_result_state_adapter_boundary.py"
        -q
)
set_tests_properties(TestAITranscriptionResultStateAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAITranscriptionResultStateAdapterBoundary core stage4)

add_test(
    NAME TestAsyncSearchRefreshResultCompositionAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_search_refresh_result_composition_adapter_boundary.py"
        -q
)
set_tests_properties(TestAsyncSearchRefreshResultCompositionAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAsyncSearchRefreshResultCompositionAdapterBoundary core stage4)

add_test(
    NAME TestAsyncSearchResultApplicationAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_search_result_application_adapter_boundary.py"
        -q
)
set_tests_properties(TestAsyncSearchResultApplicationAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAsyncSearchResultApplicationAdapterBoundary core stage4)

add_test(
    NAME TestAsyncSearchResultApplicationWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_search_result_application_workflow_boundary.py"
        -q
)
set_tests_properties(TestAsyncSearchResultApplicationWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAsyncSearchResultApplicationWorkflowBoundary core stage4)

add_test(
    NAME TestAsyncSearchResultEffectFactoryBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_search_result_effect_factory_boundary.py"
        -q
)
set_tests_properties(TestAsyncSearchResultEffectFactoryBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAsyncSearchResultEffectFactoryBoundary core stage4)

add_test(
    NAME TestSearchFilterStatePolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_search_filter_state_policy_boundary.py"
        -q
)
set_tests_properties(TestSearchFilterStatePolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestSearchFilterStatePolicyBoundary core stage4)

add_test(
    NAME TestSearchKeywordCompilerBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_search_keyword_compiler_boundary.py"
        -q
)
set_tests_properties(TestSearchKeywordCompilerBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestSearchKeywordCompilerBoundary core stage4)

add_test(
    NAME TestSearchFilterFileTypeAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_search_filter_file_type_adapter_boundary.py"
        -q
)
set_tests_properties(TestSearchFilterFileTypeAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestSearchFilterFileTypeAdapterBoundary core stage4)

add_test(
    NAME TestDatabasePathNormalizationPolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_path_normalization_policy_boundary.py"
        -q
)
set_tests_properties(TestDatabasePathNormalizationPolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestDatabasePathNormalizationPolicyBoundary core stage4)

add_test(
    NAME TestSearchPresetFilterBuildersBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_search_preset_filter_builders_boundary.py"
        -q
)
set_tests_properties(TestSearchPresetFilterBuildersBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestSearchPresetFilterBuildersBoundary core stage4)

add_test(
    NAME TestFavoritesRemovalContract
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_favorites_removal_contract.py"
        -q
)
set_tests_properties(TestFavoritesRemovalContract PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestFavoritesRemovalContract core)

add_test(
    NAME TestDatabaseSearchFilterBuilderBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_search_filter_builder_boundary.py"
        -q
)
set_tests_properties(TestDatabaseSearchFilterBuilderBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestDatabaseSearchFilterBuilderBoundary core stage4)

add_test(
    NAME TestCoverWallFilterActivityPolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_wall_filter_activity_policy_boundary.py"
        -q
)
set_tests_properties(TestCoverWallFilterActivityPolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverWallFilterActivityPolicyBoundary core stage4)

add_test(
    NAME TestCoverFacetItemBuilderBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_facet_item_builder_boundary.py"
        -q
)
set_tests_properties(TestCoverFacetItemBuilderBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverFacetItemBuilderBoundary core stage4)

add_test(
    NAME TestCoverCachedKeyItemBuilderBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_cached_key_item_builder_boundary.py"
        -q
)
set_tests_properties(TestCoverCachedKeyItemBuilderBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverCachedKeyItemBuilderBoundary core stage4)

add_test(
    NAME TestCoverWallPageResultBuilderBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_wall_page_result_builder_boundary.py"
        -q
)
set_tests_properties(TestCoverWallPageResultBuilderBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverWallPageResultBuilderBoundary core stage4)

add_test(
    NAME TestCoverWallFacetRefreshPolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_wall_facet_refresh_policy_boundary.py"
        -q
)
set_tests_properties(TestCoverWallFacetRefreshPolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverWallFacetRefreshPolicyBoundary core stage4)

add_test(
    NAME TestCoverWallCachePolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_wall_cache_policy_boundary.py"
        -q
)
set_tests_properties(TestCoverWallCachePolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverWallCachePolicyBoundary core stage4)

add_test(
    NAME TestCoverWallTextureMirrorPolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_wall_texture_mirror_policy_boundary.py"
        -q
)
set_tests_properties(TestCoverWallTextureMirrorPolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverWallTextureMirrorPolicyBoundary core stage4)

add_test(
    NAME TestCoverWallFailureCooldownPolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_wall_failure_cooldown_policy_boundary.py"
        -q
)
set_tests_properties(TestCoverWallFailureCooldownPolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverWallFailureCooldownPolicyBoundary core stage4)

add_test(
    NAME TestCoverWallVisibleMirrorAuditBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_wall_visible_mirror_audit_boundary.py"
        -q
)
set_tests_properties(TestCoverWallVisibleMirrorAuditBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverWallVisibleMirrorAuditBoundary core stage4)

add_test(
    NAME TestAsyncSearchResultPumpWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_search_result_pump_workflow_boundary.py"
        -q
)
set_tests_properties(TestAsyncSearchResultPumpWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAsyncSearchResultPumpWorkflowBoundary core stage4)

add_test(
    NAME TestAsyncSearchResultFilterAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_search_result_filter_adapter_boundary.py"
        -q
)
set_tests_properties(TestAsyncSearchResultFilterAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAsyncSearchResultFilterAdapterBoundary core stage4)

add_test(
    NAME TestAsyncSearchCoverPrimingRuntimeAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_search_cover_priming_runtime_adapter_boundary.py"
        -q
)
set_tests_properties(TestAsyncSearchCoverPrimingRuntimeAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAsyncSearchCoverPrimingRuntimeAdapterBoundary core stage4)

add_test(
    NAME TestCoverWallRefreshStateAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_wall_refresh_state_adapter_boundary.py"
        -q
)
set_tests_properties(TestCoverWallRefreshStateAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverWallRefreshStateAdapterBoundary core stage4)

add_test(
    NAME TestPlaylistQuickBarBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_playlist_quick_bar_boundary.py"
        -q
)
set_tests_properties(TestPlaylistQuickBarBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestPlaylistQuickBarBoundary core stage4)

add_test(
    NAME TestSidebarActivityBarBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sidebar_activity_bar_boundary.py"
        -q
)
set_tests_properties(TestSidebarActivityBarBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestSidebarActivityBarBoundary core stage4)

add_test(
    NAME TestFileInfoPanelBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_file_info_panel_boundary.py"
        -q
)
set_tests_properties(TestFileInfoPanelBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestFileInfoPanelBoundary core stage4)

add_test(
    NAME TestBuiltinThemeSchemeBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_builtin_theme_scheme_boundary.py"
        -q
)
set_tests_properties(TestBuiltinThemeSchemeBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestBuiltinThemeSchemeBoundary core stage4)

add_executable(TestDragPauseOnStartPolicy EXCLUDE_FROM_ALL
    tests/test_drag_pause_on_start_policy.cpp
)
target_include_directories(TestDragPauseOnStartPolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestDragPauseOnStartPolicy PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestDragPauseOnStartPolicy COMMAND TestDragPauseOnStartPolicy)
aura_set_test_labels(TestDragPauseOnStartPolicy core)

add_test(
    NAME TestDragPauseOnStartPolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_drag_pause_on_start_policy_boundary.py"
        -q
)
set_tests_properties(TestDragPauseOnStartPolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestDragPauseOnStartPolicyBoundary core)

add_executable(TestFolderTreePathModePolicy EXCLUDE_FROM_ALL
    tests/test_folder_tree_path_mode_policy.cpp
)
target_include_directories(TestFolderTreePathModePolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestFolderTreePathModePolicy PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestFolderTreePathModePolicy COMMAND TestFolderTreePathModePolicy)
aura_set_test_labels(TestFolderTreePathModePolicy core)

add_test(
    NAME TestFolderTreePathModePolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_folder_tree_path_mode_policy_boundary.py"
        -q
)
set_tests_properties(TestFolderTreePathModePolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestFolderTreePathModePolicyBoundary core)

add_executable(TestFileTypeVisibilityPolicy EXCLUDE_FROM_ALL
    tests/test_file_type_visibility_policy.cpp
    src/FileTypeVisibilityPolicy.cpp
)
target_include_directories(TestFileTypeVisibilityPolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestFileTypeVisibilityPolicy PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestFileTypeVisibilityPolicy COMMAND TestFileTypeVisibilityPolicy)
aura_set_test_labels(TestFileTypeVisibilityPolicy core)

add_test(
    NAME TestFileTypeVisibilityPolicyStage6
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_file_type_visibility_policy_stage6.py"
        -q
)
set_tests_properties(TestFileTypeVisibilityPolicyStage6 PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestFileTypeVisibilityPolicyStage6 core stage4)

add_executable(TestAsyncSearchFacetResultWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_facet_result_workflow.cpp
    src/AsyncSearchFacetResultWorkflow.cpp
)
target_include_directories(TestAsyncSearchFacetResultWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchFacetResultWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchFacetResultWorkflow COMMAND TestAsyncSearchFacetResultWorkflow)
aura_set_test_labels(TestAsyncSearchFacetResultWorkflow core db)

add_executable(TestRandomShufflePlaybackWorkflow EXCLUDE_FROM_ALL
    tests/test_random_shuffle_playback_workflow.cpp
    src/RandomShufflePlaybackWorkflow.cpp
)
target_include_directories(TestRandomShufflePlaybackWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestRandomShufflePlaybackWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestRandomShufflePlaybackWorkflow COMMAND TestRandomShufflePlaybackWorkflow)
aura_set_test_labels(TestRandomShufflePlaybackWorkflow core audio)

add_test(
    NAME TestRandomShufflePlaybackStateAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_random_shuffle_playback_state_adapter_boundary.py"
        -q
)
set_tests_properties(TestRandomShufflePlaybackStateAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestRandomShufflePlaybackStateAdapterBoundary core stage4)

add_executable(TestAsyncSearchRefreshFacetsWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_refresh_facets_workflow.cpp
    src/AsyncSearchRefreshFacetsWorkflow.cpp
)
target_include_directories(TestAsyncSearchRefreshFacetsWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchRefreshFacetsWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchRefreshFacetsWorkflow COMMAND TestAsyncSearchRefreshFacetsWorkflow)
aura_set_test_labels(TestAsyncSearchRefreshFacetsWorkflow core db)

add_executable(TestAsyncSearchRefreshFacetsAdapter EXCLUDE_FROM_ALL
    tests/test_async_search_refresh_facets_adapter.cpp
    src/AsyncSearchRefreshFacetsAdapter.cpp
    src/AsyncSearchRefreshFacetsWorkflow.cpp
)
target_include_directories(TestAsyncSearchRefreshFacetsAdapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchRefreshFacetsAdapter PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchRefreshFacetsAdapter COMMAND TestAsyncSearchRefreshFacetsAdapter)
aura_set_test_labels(TestAsyncSearchRefreshFacetsAdapter core db)

add_executable(TestAsyncSearchAppendResultWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_append_result_workflow.cpp
    src/AsyncSearchAppendResultWorkflow.cpp
)
target_include_directories(TestAsyncSearchAppendResultWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchAppendResultWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchAppendResultWorkflow COMMAND TestAsyncSearchAppendResultWorkflow)
aura_set_test_labels(TestAsyncSearchAppendResultWorkflow core db)

add_executable(TestAsyncSearchRefreshCountRepairWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_refresh_count_repair_workflow.cpp
    src/AsyncSearchRefreshCountRepairWorkflow.cpp
)
target_include_directories(TestAsyncSearchRefreshCountRepairWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchRefreshCountRepairWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchRefreshCountRepairWorkflow COMMAND TestAsyncSearchRefreshCountRepairWorkflow)
aura_set_test_labels(TestAsyncSearchRefreshCountRepairWorkflow core db)

add_executable(TestAsyncSearchRefreshCountRepairAdapter EXCLUDE_FROM_ALL
    tests/test_async_search_refresh_count_repair_adapter.cpp
    src/AsyncSearchRefreshCountRepairAdapter.cpp
    src/AsyncSearchRefreshCountRepairWorkflow.cpp
)
target_include_directories(TestAsyncSearchRefreshCountRepairAdapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchRefreshCountRepairAdapter PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchRefreshCountRepairAdapter COMMAND TestAsyncSearchRefreshCountRepairAdapter)
aura_set_test_labels(TestAsyncSearchRefreshCountRepairAdapter core db)

add_executable(TestAsyncSearchRefreshCompletionWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_refresh_completion_workflow.cpp
    src/AsyncSearchRefreshCompletionWorkflow.cpp
)
target_include_directories(TestAsyncSearchRefreshCompletionWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchRefreshCompletionWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchRefreshCompletionWorkflow COMMAND TestAsyncSearchRefreshCompletionWorkflow)
aura_set_test_labels(TestAsyncSearchRefreshCompletionWorkflow core db)

add_executable(TestAsyncSearchRefreshMemoryWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_refresh_memory_workflow.cpp
    src/AsyncSearchRefreshMemoryWorkflow.cpp
)
target_include_directories(TestAsyncSearchRefreshMemoryWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchRefreshMemoryWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchRefreshMemoryWorkflow COMMAND TestAsyncSearchRefreshMemoryWorkflow)
aura_set_test_labels(TestAsyncSearchRefreshMemoryWorkflow core audio)

add_executable(TestAsyncSearchRefreshMemoryAdapter EXCLUDE_FROM_ALL
    tests/test_async_search_refresh_memory_adapter.cpp
    src/AsyncSearchRefreshMemoryAdapter.cpp
    src/AsyncSearchRefreshMemoryWorkflow.cpp
)
target_include_directories(TestAsyncSearchRefreshMemoryAdapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchRefreshMemoryAdapter PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchRefreshMemoryAdapter COMMAND TestAsyncSearchRefreshMemoryAdapter)
aura_set_test_labels(TestAsyncSearchRefreshMemoryAdapter core audio)

add_executable(TestAsyncSearchRefreshLoadingWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_refresh_loading_workflow.cpp
    src/AsyncSearchRefreshLoadingWorkflow.cpp
)
target_include_directories(TestAsyncSearchRefreshLoadingWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchRefreshLoadingWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchRefreshLoadingWorkflow COMMAND TestAsyncSearchRefreshLoadingWorkflow)
aura_set_test_labels(TestAsyncSearchRefreshLoadingWorkflow core audio)

add_executable(TestAsyncSearchWaveformPreloadWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_waveform_preload_workflow.cpp
    src/AsyncSearchWaveformPreloadWorkflow.cpp
)
target_include_directories(TestAsyncSearchWaveformPreloadWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchWaveformPreloadWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchWaveformPreloadWorkflow COMMAND TestAsyncSearchWaveformPreloadWorkflow)
aura_set_test_labels(TestAsyncSearchWaveformPreloadWorkflow core audio)

add_executable(TestAsyncSearchCoverPreloadWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_cover_preload_workflow.cpp
    src/AsyncSearchCoverPreloadWorkflow.cpp
)
target_include_directories(TestAsyncSearchCoverPreloadWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchCoverPreloadWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchCoverPreloadWorkflow COMMAND TestAsyncSearchCoverPreloadWorkflow)
aura_set_test_labels(TestAsyncSearchCoverPreloadWorkflow core audio)

add_executable(TestAsyncSearchCoverPreloadAdapter EXCLUDE_FROM_ALL
    tests/test_async_search_cover_preload_adapter.cpp
    src/AsyncSearchCoverPreloadAdapter.cpp
)
target_include_directories(TestAsyncSearchCoverPreloadAdapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchCoverPreloadAdapter PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchCoverPreloadAdapter COMMAND TestAsyncSearchCoverPreloadAdapter)
aura_set_test_labels(TestAsyncSearchCoverPreloadAdapter core audio)

add_executable(TestAsyncSearchEmptyFallbackWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_empty_fallback_workflow.cpp
    src/AsyncSearchEmptyFallbackWorkflow.cpp
)
target_include_directories(TestAsyncSearchEmptyFallbackWorkflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestAsyncSearchEmptyFallbackWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAsyncSearchEmptyFallbackWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchEmptyFallbackWorkflow COMMAND TestAsyncSearchEmptyFallbackWorkflow)
aura_set_test_labels(TestAsyncSearchEmptyFallbackWorkflow core db)

add_executable(TestAsyncSearchRefreshDiagnosticsWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_refresh_diagnostics_workflow.cpp
    src/AsyncSearchRefreshDiagnosticsWorkflow.cpp
)
target_include_directories(TestAsyncSearchRefreshDiagnosticsWorkflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestAsyncSearchRefreshDiagnosticsWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAsyncSearchRefreshDiagnosticsWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchRefreshDiagnosticsWorkflow COMMAND TestAsyncSearchRefreshDiagnosticsWorkflow)
aura_set_test_labels(TestAsyncSearchRefreshDiagnosticsWorkflow core)

add_executable(TestAsyncSearchRefreshResultsWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_refresh_results_workflow.cpp
    src/AsyncSearchRefreshResultsWorkflow.cpp
)
target_include_directories(TestAsyncSearchRefreshResultsWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSearchRefreshResultsWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchRefreshResultsWorkflow COMMAND TestAsyncSearchRefreshResultsWorkflow)
aura_set_test_labels(TestAsyncSearchRefreshResultsWorkflow core db)

add_executable(TestCoverKeyFilterWorkflow EXCLUDE_FROM_ALL
    tests/test_cover_key_filter_workflow.cpp
    src/CoverKeyFilterWorkflow.cpp
)
target_include_directories(TestCoverKeyFilterWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestCoverKeyFilterWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestCoverKeyFilterWorkflow COMMAND TestCoverKeyFilterWorkflow)
aura_set_test_labels(TestCoverKeyFilterWorkflow core db)

add_test(
    NAME TestCoverKeyFilterWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_key_filter_workflow_boundary.py"
        -q
)
set_tests_properties(TestCoverKeyFilterWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverKeyFilterWorkflowBoundary core stage4)

add_test(
    NAME TestCoverKeyFilterStateAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_key_filter_state_adapter_boundary.py"
        -q
)
set_tests_properties(TestCoverKeyFilterStateAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestCoverKeyFilterStateAdapterBoundary core stage4)

add_test(
    NAME TestPlaybackHistoryStateAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_playback_history_state_adapter_boundary.py"
        -q
)
set_tests_properties(TestPlaybackHistoryStateAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestPlaybackHistoryStateAdapterBoundary core stage4)

add_executable(TestPlayStatsRecorder EXCLUDE_FROM_ALL
    tests/test_play_stats_recorder.cpp
)
target_include_directories(TestPlayStatsRecorder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestPlayStatsRecorder PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestPlayStatsRecorder PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestPlayStatsRecorder PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlayStatsRecorder COMMAND TestPlayStatsRecorder)
aura_set_test_labels(TestPlayStatsRecorder core audio)

add_test(
    NAME TestPlayStatsRecorderBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_play_stats_recorder_boundary.py"
        -q
)
set_tests_properties(TestPlayStatsRecorderBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestPlayStatsRecorderBoundary core stage4)

add_test(
    NAME TestWaveformDetailSchedulingStateAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_detail_scheduling_state_adapter_boundary.py"
        -q
)
set_tests_properties(TestWaveformDetailSchedulingStateAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestWaveformDetailSchedulingStateAdapterBoundary core stage4)

add_test(
    NAME TestWaveformViewResetStateAdapterBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_view_reset_state_adapter_boundary.py"
        -q
)
set_tests_properties(TestWaveformViewResetStateAdapterBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestWaveformViewResetStateAdapterBoundary core stage4)

add_test(
    NAME TestAsyncSearchFacetWarmStartPolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_search_facet_warm_start_policy_boundary.py"
        -q
)
set_tests_properties(TestAsyncSearchFacetWarmStartPolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAsyncSearchFacetWarmStartPolicyBoundary core stage4)

add_test(
    NAME TestAsyncSearchCountFacetRepairSubmissionBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_search_count_facet_repair_submission_boundary.py"
        -q
)
set_tests_properties(TestAsyncSearchCountFacetRepairSubmissionBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestAsyncSearchCountFacetRepairSubmissionBoundary core stage4)

add_executable(TestPlaylistDisplayedFileCountPolicy EXCLUDE_FROM_ALL
    tests/test_playlist_displayed_file_count_policy.cpp
    src/PlaylistDisplayedFileCountPolicy.cpp
)
target_include_directories(TestPlaylistDisplayedFileCountPolicy PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestPlaylistDisplayedFileCountPolicy PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestPlaylistDisplayedFileCountPolicy PRIVATE /W3 /MP /utf-8)
endif()
add_test(
    NAME TestPlaylistDisplayedFileCountPolicy
    COMMAND TestPlaylistDisplayedFileCountPolicy
)
aura_set_test_labels(TestPlaylistDisplayedFileCountPolicy core stage4)

add_test(
    NAME TestPlaylistDisplayedFileCountPolicyBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_playlist_displayed_file_count_policy_boundary.py"
        -q
)
set_tests_properties(TestPlaylistDisplayedFileCountPolicyBoundary PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    TIMEOUT 60
)
aura_set_test_labels(TestPlaylistDisplayedFileCountPolicyBoundary core stage4)

add_executable(TestAsyncSearchBrowsePreviewWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_browse_preview_workflow.cpp
    src/AsyncSearchBrowsePreviewWorkflow.cpp
)
target_include_directories(TestAsyncSearchBrowsePreviewWorkflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestAsyncSearchBrowsePreviewWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAsyncSearchBrowsePreviewWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchBrowsePreviewWorkflow COMMAND TestAsyncSearchBrowsePreviewWorkflow)
aura_set_test_labels(TestAsyncSearchBrowsePreviewWorkflow core ui)

add_executable(TestAsyncSearchBrowsePreviewRefreshWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_browse_preview_refresh_workflow.cpp
    src/AsyncSearchBrowsePreviewRefreshWorkflow.cpp
    src/AsyncSearchBrowsePreviewWorkflow.cpp
)
target_include_directories(TestAsyncSearchBrowsePreviewRefreshWorkflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestAsyncSearchBrowsePreviewRefreshWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAsyncSearchBrowsePreviewRefreshWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchBrowsePreviewRefreshWorkflow COMMAND TestAsyncSearchBrowsePreviewRefreshWorkflow)
aura_set_test_labels(TestAsyncSearchBrowsePreviewRefreshWorkflow core ui)

add_executable(TestAsyncSearchResultLifecycleWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_result_lifecycle_workflow.cpp
    src/AsyncSearchResultLifecycleWorkflow.cpp
)
target_include_directories(TestAsyncSearchResultLifecycleWorkflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestAsyncSearchResultLifecycleWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAsyncSearchResultLifecycleWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchResultLifecycleWorkflow COMMAND TestAsyncSearchResultLifecycleWorkflow)
aura_set_test_labels(TestAsyncSearchResultLifecycleWorkflow core)

add_executable(TestAsyncSearchResultDispatchWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_result_dispatch_workflow.cpp
    src/AsyncSearchResultDispatchWorkflow.cpp
)
target_include_directories(TestAsyncSearchResultDispatchWorkflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestAsyncSearchResultDispatchWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAsyncSearchResultDispatchWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchResultDispatchWorkflow COMMAND TestAsyncSearchResultDispatchWorkflow)
aura_set_test_labels(TestAsyncSearchResultDispatchWorkflow core)

add_executable(TestAsyncSearchQueryTimingRegistry EXCLUDE_FROM_ALL
    tests/test_async_search_query_timing_registry.cpp
    src/AsyncSearchQueryTimingRegistry.cpp
)
target_include_directories(TestAsyncSearchQueryTimingRegistry PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestAsyncSearchQueryTimingRegistry PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAsyncSearchQueryTimingRegistry PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchQueryTimingRegistry COMMAND TestAsyncSearchQueryTimingRegistry)
aura_set_test_labels(TestAsyncSearchQueryTimingRegistry core)

add_executable(TestPlaylistResultFilterWorkflow EXCLUDE_FROM_ALL
    tests/test_playlist_result_filter_workflow.cpp
    src/PlaylistResultFilterWorkflow.cpp
    src/FileTypeVisibilityPolicy.cpp
)
target_include_directories(TestPlaylistResultFilterWorkflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestPlaylistResultFilterWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestPlaylistResultFilterWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestPlaylistResultFilterWorkflow COMMAND TestPlaylistResultFilterWorkflow)
aura_set_test_labels(TestPlaylistResultFilterWorkflow core)

add_executable(TestAsyncSearchPlaylistContextWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_playlist_context_workflow.cpp
    src/AsyncSearchPlaylistContextWorkflow.cpp
)
target_include_directories(TestAsyncSearchPlaylistContextWorkflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestAsyncSearchPlaylistContextWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAsyncSearchPlaylistContextWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchPlaylistContextWorkflow COMMAND TestAsyncSearchPlaylistContextWorkflow)
aura_set_test_labels(TestAsyncSearchPlaylistContextWorkflow core)

add_executable(TestAsyncSearchPlaylistResultPreparationWorkflow EXCLUDE_FROM_ALL
    tests/test_async_search_playlist_result_preparation_workflow.cpp
    src/AsyncSearchPlaylistResultPreparationWorkflow.cpp
)
target_include_directories(TestAsyncSearchPlaylistResultPreparationWorkflow PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(TestAsyncSearchPlaylistResultPreparationWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAsyncSearchPlaylistResultPreparationWorkflow PRIVATE /W3 /MP /utf-8)
endif()
add_test(NAME TestAsyncSearchPlaylistResultPreparationWorkflow COMMAND TestAsyncSearchPlaylistResultPreparationWorkflow)
aura_set_test_labels(TestAsyncSearchPlaylistResultPreparationWorkflow core)

add_executable(TestSidebarOrder EXCLUDE_FROM_ALL
    tests/test_sidebar_order.cpp
)
target_include_directories(TestSidebarOrder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestSidebarOrder PRIVATE /W3 /utf-8)
    set_target_properties(TestSidebarOrder PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSidebarOrder COMMAND TestSidebarOrder)
aura_set_test_labels(TestSidebarOrder ui)

# Desktop resize shell contract: keep top search, side docks, center workspace,
# and bottom buttons adaptive instead of fixed-width/fixed-height only.

add_executable(TestResultActionMenuModel EXCLUDE_FROM_ALL
    tests/test_result_action_menu_model.cpp
    src/ResultActionMenu.cpp
)
target_include_directories(TestResultActionMenuModel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(TestResultActionMenuModel PRIVATE /W3 /utf-8)
    set_target_properties(TestResultActionMenuModel PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestResultActionMenuModel COMMAND TestResultActionMenuModel)
aura_set_test_labels(TestResultActionMenuModel core)

add_executable(TestShortcutPresets EXCLUDE_FROM_ALL
    tests/test_shortcut_presets.cpp
    tests/test_appstate_link_stubs.cpp
    tests/test_shortcut_presets_link_stubs.cpp
    src/AppState.cpp
)
target_include_directories(TestShortcutPresets PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${IMGUI_DIR}
)
if(MSVC)
    target_compile_options(TestShortcutPresets PRIVATE /W3 /utf-8)
    set_target_properties(TestShortcutPresets PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestShortcutPresets COMMAND TestShortcutPresets)
aura_set_test_labels(TestShortcutPresets core)

# F-033 Tab Presets: pure-logic round-trip tests with no AppState dependency

add_executable(TestUserActionLog EXCLUDE_FROM_ALL
    tests/test_user_action_log.cpp
    src/UserActionLog.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestUserActionLog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(TestUserActionLog PRIVATE AURA_BUILD_DIAGNOSTICS=1 NOMINMAX)
target_compile_features(TestUserActionLog PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUserActionLog PRIVATE /W3 /utf-8)
    set_target_properties(TestUserActionLog PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUserActionLog COMMAND TestUserActionLog)
aura_set_test_labels(TestUserActionLog core)

# WorkflowEngine unit tests (node execution, pipeline, serialization)

add_executable(TestWorkflowEnginePersistence EXCLUDE_FROM_ALL
    tests/test_workflow_engine_persistence.cpp
    src/WorkflowEngine.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestWorkflowEnginePersistence PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestWorkflowEnginePersistence PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestWorkflowEnginePersistence PRIVATE /W3 /utf-8)
    set_target_properties(TestWorkflowEnginePersistence PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestWorkflowEnginePersistence COMMAND TestWorkflowEnginePersistence)
aura_set_test_labels(TestWorkflowEnginePersistence core)

add_executable(TestWwiseTransferPersistence EXCLUDE_FROM_ALL
    tests/test_wwise_transfer_persistence.cpp
    src/WwiseTransferPersist.cpp
    src/WwiseTransferConfig.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestWwiseTransferPersistence PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestWwiseTransferPersistence PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestWwiseTransferPersistence PRIVATE /W3 /utf-8)
    set_target_properties(TestWwiseTransferPersistence PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestWwiseTransferPersistence COMMAND TestWwiseTransferPersistence)
aura_set_test_labels(TestWwiseTransferPersistence core)

add_executable(TestOpenWith EXCLUDE_FROM_ALL
    tests/test_open_with.cpp
    src/Utils.cpp
)
target_include_directories(TestOpenWith PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sqlite
)
target_compile_definitions(TestOpenWith PRIVATE AURA_BUILD_DIAGNOSTICS=0)
if(MSVC)
    target_compile_options(TestOpenWith PRIVATE /W3 /utf-8 /EHa)
    set_target_properties(TestOpenWith PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
target_link_libraries(TestOpenWith PRIVATE shlwapi)
add_test(NAME TestOpenWith COMMAND TestOpenWith)
aura_set_test_labels(TestOpenWith core)

add_executable(TestStage6TextEncoding EXCLUDE_FROM_ALL
    tests/test_stage6_text_encoding.cpp
)
if(WIN32)
    target_sources(TestStage6TextEncoding PRIVATE
        src/platform/text_encoding_win32.cpp
        src/platform/text_encoding_ascii.cpp
    )
else()
    target_sources(TestStage6TextEncoding PRIVATE
        src/platform/text_encoding_std.cpp
        src/platform/text_encoding_ascii.cpp
    )
endif()
target_include_directories(TestStage6TextEncoding PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestStage6TextEncoding PRIVATE /W3 /utf-8)
    set_target_properties(TestStage6TextEncoding PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestStage6TextEncoding COMMAND TestStage6TextEncoding)
aura_set_test_labels(TestStage6TextEncoding core)

add_executable(TestUtilsTextEncoding EXCLUDE_FROM_ALL
    tests/test_utils_text_encoding.cpp
    src/Utils.cpp
)
if(WIN32)
    target_sources(TestUtilsTextEncoding PRIVATE
        src/platform/text_encoding_win32.cpp
    )
else()
    target_sources(TestUtilsTextEncoding PRIVATE
        src/platform/text_encoding_std.cpp
    )
endif()
target_include_directories(TestUtilsTextEncoding PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestUtilsTextEncoding PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUtilsTextEncoding PRIVATE /W3 /utf-8)
    set_target_properties(TestUtilsTextEncoding PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUtilsTextEncoding COMMAND TestUtilsTextEncoding)
aura_set_test_labels(TestUtilsTextEncoding core)

add_executable(TestNamingScriptConflict EXCLUDE_FROM_ALL
    tests/test_naming_script_conflict.cpp
    src/NamingScript.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestNamingScriptConflict PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestNamingScriptConflict PRIVATE /W3 /utf-8)
    set_target_properties(TestNamingScriptConflict PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestNamingScriptConflict COMMAND TestNamingScriptConflict)
aura_set_test_labels(TestNamingScriptConflict core)

add_executable(TestBatchRenamePolicy EXCLUDE_FROM_ALL
    tests/test_batch_rename_policy.cpp
    tests/test_get_exe_dir_stub.cpp
    src/BatchRenamePolicy.cpp
    src/Utils.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
if(WIN32)
    target_sources(TestBatchRenamePolicy PRIVATE
        src/platform/filesystem_win32.cpp
    )
endif()
target_include_directories(TestBatchRenamePolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestBatchRenamePolicy PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestBatchRenamePolicy PRIVATE /W3 /utf-8)
    set_target_properties(TestBatchRenamePolicy PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestBatchRenamePolicy COMMAND TestBatchRenamePolicy)
aura_set_test_labels(TestBatchRenamePolicy core stage4)

add_executable(TestBatchRenameExecutionWorkflow EXCLUDE_FROM_ALL
    tests/test_batch_rename_execution_workflow.cpp
    src/BatchRenameExecutionWorkflow.cpp
    src/Utils.cpp
    tests/test_get_exe_dir_stub.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
if(WIN32)
    target_sources(TestBatchRenameExecutionWorkflow PRIVATE
        src/platform/filesystem_win32.cpp
    )
endif()
target_include_directories(TestBatchRenameExecutionWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestBatchRenameExecutionWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestBatchRenameExecutionWorkflow PRIVATE /W3 /utf-8)
    set_target_properties(TestBatchRenameExecutionWorkflow PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(
    NAME TestBatchRenameExecutionWorkflow
    COMMAND TestBatchRenameExecutionWorkflow
)
aura_set_test_labels(TestBatchRenameExecutionWorkflow core stage4)

add_executable(TestBrowserSystemFolderSearchPolicy EXCLUDE_FROM_ALL
    tests/test_browser_system_folder_search_policy.cpp
    src/BrowserSystemFolderSearchPolicy.cpp
)
target_include_directories(TestBrowserSystemFolderSearchPolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestBrowserSystemFolderSearchPolicy PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestBrowserSystemFolderSearchPolicy PRIVATE /W3 /utf-8)
    set_target_properties(TestBrowserSystemFolderSearchPolicy PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(
    NAME TestBrowserSystemFolderSearchPolicy
    COMMAND TestBrowserSystemFolderSearchPolicy
)
aura_set_test_labels(TestBrowserSystemFolderSearchPolicy core stage4)

add_executable(TestBrowserSystemFolderSearchRuntime EXCLUDE_FROM_ALL
    tests/test_browser_system_folder_search_runtime.cpp
    src/BrowserSystemFolderSearchRuntime.cpp
    src/BrowserSystemFolderSearchPolicy.cpp
    src/PathUtils.cpp
    src/Utils.cpp
    tests/test_get_exe_dir_stub.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
if(WIN32)
    target_sources(TestBrowserSystemFolderSearchRuntime PRIVATE
        src/platform/filesystem_win32.cpp
    )
endif()
target_include_directories(TestBrowserSystemFolderSearchRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestBrowserSystemFolderSearchRuntime PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestBrowserSystemFolderSearchRuntime PRIVATE /W3 /utf-8)
    set_target_properties(TestBrowserSystemFolderSearchRuntime PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(
    NAME TestBrowserSystemFolderSearchRuntime
    COMMAND TestBrowserSystemFolderSearchRuntime
)
aura_set_test_labels(TestBrowserSystemFolderSearchRuntime core stage4)

add_executable(TestFilePlayerDeferredDbWarmupRuntime EXCLUDE_FROM_ALL
    tests/test_file_player_deferred_db_warmup_runtime.cpp
    src/FilePlayerDeferredDbWarmupRuntime.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
if(WIN32)
    target_sources(TestFilePlayerDeferredDbWarmupRuntime PRIVATE
        src/platform/filesystem_win32.cpp
    )
endif()
target_include_directories(TestFilePlayerDeferredDbWarmupRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestFilePlayerDeferredDbWarmupRuntime PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestFilePlayerDeferredDbWarmupRuntime PRIVATE /W3 /utf-8)
    set_target_properties(TestFilePlayerDeferredDbWarmupRuntime PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(
    NAME TestFilePlayerDeferredDbWarmupRuntime
    COMMAND TestFilePlayerDeferredDbWarmupRuntime
)
aura_set_test_labels(TestFilePlayerDeferredDbWarmupRuntime core stage4)

add_executable(TestFilePlayerDatabaseOpenWorkflow EXCLUDE_FROM_ALL
    tests/test_file_player_database_open_workflow.cpp
    src/FilePlayerDatabaseOpenWorkflow.cpp
)
target_include_directories(TestFilePlayerDatabaseOpenWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestFilePlayerDatabaseOpenWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestFilePlayerDatabaseOpenWorkflow PRIVATE /W3 /utf-8)
    set_target_properties(TestFilePlayerDatabaseOpenWorkflow PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(
    NAME TestFilePlayerDatabaseOpenWorkflow
    COMMAND TestFilePlayerDatabaseOpenWorkflow
)
aura_set_test_labels(TestFilePlayerDatabaseOpenWorkflow core stage4)

add_executable(TestDatabaseSwitchWorkerRuntime EXCLUDE_FROM_ALL
    tests/test_database_switch_worker_runtime.cpp
    src/DatabaseSwitchWorkerRuntime.cpp
)
target_include_directories(TestDatabaseSwitchWorkerRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestDatabaseSwitchWorkerRuntime PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestDatabaseSwitchWorkerRuntime PRIVATE /W3 /utf-8)
    set_target_properties(TestDatabaseSwitchWorkerRuntime PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(
    NAME TestDatabaseSwitchWorkerRuntime
    COMMAND TestDatabaseSwitchWorkerRuntime
)
aura_set_test_labels(TestDatabaseSwitchWorkerRuntime core stage4)

add_executable(TestDatabaseSwitchBlockingOperation EXCLUDE_FROM_ALL
    tests/test_database_switch_blocking_operation.cpp
    src/DatabaseSwitchBlockingOperation.cpp
    src/DiagnosticsLoggerStub.cpp
)
target_include_directories(TestDatabaseSwitchBlockingOperation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestDatabaseSwitchBlockingOperation PRIVATE cxx_std_17)
target_compile_definitions(TestDatabaseSwitchBlockingOperation PRIVATE
    AURA_BUILD_DIAGNOSTICS=0
)
if(MSVC)
    target_compile_options(TestDatabaseSwitchBlockingOperation PRIVATE /W3 /utf-8)
    set_target_properties(TestDatabaseSwitchBlockingOperation PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(
    NAME TestDatabaseSwitchBlockingOperation
    COMMAND TestDatabaseSwitchBlockingOperation
)
aura_set_test_labels(TestDatabaseSwitchBlockingOperation core stage4)

add_executable(TestDatabasePackageReplacement EXCLUDE_FROM_ALL
    tests/test_database_package_replacement.cpp
    src/DatabasePackageReplacement.cpp
    src/platform/filesystem_std.cpp
)
if(WIN32)
    target_sources(TestDatabasePackageReplacement PRIVATE
        src/platform/filesystem_win32.cpp src/platform/text_encoding_win32.cpp)
endif()
target_include_directories(TestDatabasePackageReplacement PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestDatabasePackageReplacement PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestDatabasePackageReplacement PRIVATE /utf-8)
endif()
if(WIN32)
    set(_db_replacement_test_root "D:/Aris-QA/test-databases/database-package-replacement")
else()
    set(_db_replacement_test_root "${CMAKE_CURRENT_BINARY_DIR}/Testing/database-package-replacement")
endif()
add_test(NAME TestDatabasePackageReplacement
    COMMAND TestDatabasePackageReplacement "${_db_replacement_test_root}")
aura_set_test_labels(TestDatabasePackageReplacement core database)

add_executable(TestSameFolderScanRuntime EXCLUDE_FROM_ALL
    tests/test_same_folder_scan_runtime.cpp
    src/SameFolderScanRuntime.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
if(WIN32)
    target_sources(TestSameFolderScanRuntime PRIVATE
        src/platform/filesystem_win32.cpp
    )
endif()
target_include_directories(TestSameFolderScanRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestSameFolderScanRuntime PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestSameFolderScanRuntime PRIVATE /W3 /utf-8)
    set_target_properties(TestSameFolderScanRuntime PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(
    NAME TestSameFolderScanRuntime
    COMMAND TestSameFolderScanRuntime
)
aura_set_test_labels(TestSameFolderScanRuntime core stage4)

add_executable(TestAvatarCropperIO EXCLUDE_FROM_ALL
    tests/test_avatar_cropper_io.cpp
    src/AvatarCropper.cpp
    src/AvatarCropperIO.cpp
    src/stb_image_impl.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
if(WIN32)
    target_sources(TestAvatarCropperIO PRIVATE src/platform/filesystem_win32.cpp)
endif()
target_include_directories(TestAvatarCropperIO PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_definitions(TestAvatarCropperIO PRIVATE NOMINMAX)
if(MSVC)
    target_compile_options(TestAvatarCropperIO PRIVATE /W3 /utf-8)
    set_target_properties(TestAvatarCropperIO PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestAvatarCropperIO COMMAND TestAvatarCropperIO)
aura_set_test_labels(TestAvatarCropperIO core)

add_executable(TestPlatformExecutablePath EXCLUDE_FROM_ALL
    tests/test_platform_executable_path.cpp
)
target_include_directories(TestPlatformExecutablePath PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(WIN32)
    target_sources(TestPlatformExecutablePath PRIVATE
        src/platform/executable_path_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
elseif(APPLE)
    target_sources(TestPlatformExecutablePath PRIVATE
        src/platform/executable_path_macos.mm
        src/platform/text_encoding_std.cpp
    )
    target_link_libraries(TestPlatformExecutablePath PRIVATE "-framework Foundation")
endif()
if(MSVC)
    target_compile_options(TestPlatformExecutablePath PRIVATE /W3 /utf-8)
    set_target_properties(TestPlatformExecutablePath PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlatformExecutablePath COMMAND TestPlatformExecutablePath)
aura_set_test_labels(TestPlatformExecutablePath core)

add_executable(TestPlatformFileHash EXCLUDE_FROM_ALL
    tests/test_platform_file_hash.cpp
)
target_include_directories(TestPlatformFileHash PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(WIN32)
    target_sources(TestPlatformFileHash PRIVATE
        src/platform/file_hash_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_link_libraries(TestPlatformFileHash PRIVATE bcrypt)
elseif(APPLE)
    target_sources(TestPlatformFileHash PRIVATE
        src/platform/file_hash_macos.cpp
    )
    target_link_libraries(TestPlatformFileHash PRIVATE "-framework CommonCrypto")
endif()
if(MSVC)
    target_compile_options(TestPlatformFileHash PRIVATE /W3 /utf-8)
    set_target_properties(TestPlatformFileHash PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlatformFileHash COMMAND TestPlatformFileHash)
aura_set_test_labels(TestPlatformFileHash core)

add_executable(TestPlatformDurableFileOpsCopy EXCLUDE_FROM_ALL
    tests/test_platform_durable_file_ops_copy.cpp
)
target_include_directories(TestPlatformDurableFileOpsCopy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestPlatformDurableFileOpsCopy PRIVATE cxx_std_17)
if(WIN32)
    target_sources(TestPlatformDurableFileOpsCopy PRIVATE
        src/platform/durable_file_ops_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
elseif(APPLE)
    target_sources(TestPlatformDurableFileOpsCopy PRIVATE
        src/platform/durable_file_ops_macos.cpp
    )
endif()
if(MSVC)
    target_compile_options(TestPlatformDurableFileOpsCopy PRIVATE /W3 /utf-8)
    set_target_properties(TestPlatformDurableFileOpsCopy PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlatformDurableFileOpsCopy COMMAND TestPlatformDurableFileOpsCopy)
aura_set_test_labels(TestPlatformDurableFileOpsCopy core)

add_executable(TestPlatformDurableFileOpsReplaceDelete EXCLUDE_FROM_ALL
    tests/test_platform_durable_file_ops_replace_delete.cpp
)
target_include_directories(TestPlatformDurableFileOpsReplaceDelete PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestPlatformDurableFileOpsReplaceDelete PRIVATE cxx_std_17)
if(WIN32)
    target_sources(TestPlatformDurableFileOpsReplaceDelete PRIVATE
        src/platform/durable_file_ops_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
elseif(APPLE)
    target_sources(TestPlatformDurableFileOpsReplaceDelete PRIVATE
        src/platform/durable_file_ops_macos.cpp
    )
endif()
if(MSVC)
    target_compile_options(TestPlatformDurableFileOpsReplaceDelete PRIVATE /W3 /utf-8)
    set_target_properties(TestPlatformDurableFileOpsReplaceDelete PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlatformDurableFileOpsReplaceDelete COMMAND TestPlatformDurableFileOpsReplaceDelete)
aura_set_test_labels(TestPlatformDurableFileOpsReplaceDelete core)

add_executable(TestMetadataFields EXCLUDE_FROM_ALL
    tests/test_metadata_fields.cpp
    src/MetadataFields.cpp
)
target_include_directories(TestMetadataFields PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestMetadataFields PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestMetadataFields PRIVATE /W3 /utf-8)
    set_target_properties(TestMetadataFields PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestMetadataFields COMMAND TestMetadataFields)
aura_set_test_labels(TestMetadataFields core)

# F-013: EmbedScript unit tests

add_executable(TestTagSystem EXCLUDE_FROM_ALL
    tests/test_tag_system.cpp
    src/TagSystem.cpp
)
target_include_directories(TestTagSystem PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestTagSystem PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestTagSystem PRIVATE /W3 /utf-8)
    set_target_properties(TestTagSystem PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestTagSystem COMMAND TestTagSystem)
aura_set_test_labels(TestTagSystem core)

# F-020: MemoryBudget unit tests

add_executable(TestTagVocabularyMapper EXCLUDE_FROM_ALL
    tests/test_tag_vocabulary_mapper.cpp
    src/TagVocabularyMapper.cpp
)
target_include_directories(TestTagVocabularyMapper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestTagVocabularyMapper PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestTagVocabularyMapper PRIVATE /W3 /utf-8)
    set_target_properties(TestTagVocabularyMapper PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestTagVocabularyMapper COMMAND TestTagVocabularyMapper)
aura_set_test_labels(TestTagVocabularyMapper core)

# MoodAnalyzer unit tests

add_executable(TestMultiTabSync EXCLUDE_FROM_ALL
    tests/test_multi_tab_sync.cpp
    tests/test_database_link_stubs.cpp
    src/Database.cpp
    src/CoverReplacementFoundation.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/WaveformExtractor.cpp
    src/TimeStretch.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestMultiTabSync PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestMultiTabSync PRIVATE opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm)
target_compile_definitions(TestMultiTabSync PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5)
if(MSVC)
    target_compile_options(TestMultiTabSync PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestMultiTabSync PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE" LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestMultiTabSync COMMAND TestMultiTabSync)
aura_set_test_labels(TestMultiTabSync db)

# Minimal test: cover path normalization (Utils only, no Database/ImGui)

add_executable(TestSubPlaylists EXCLUDE_FROM_ALL
    tests/test_sub_playlists.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/CoverReplacementFoundation.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/TimeStretch.cpp
    src/WaveformExtractor.cpp
    src/NativeFloatReader.cpp
    src/platform/filesystem_std.cpp
    src/platform/file_watcher.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/audio_file_miniaudio.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestSubPlaylists PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestSubPlaylists PRIVATE
    opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm
)
target_compile_definitions(TestSubPlaylists PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5
)
if(MSVC)
    target_compile_options(TestSubPlaylists PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestSubPlaylists PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSubPlaylists COMMAND TestSubPlaylists)
aura_set_test_labels(TestSubPlaylists core)

add_executable(TestPlaylistFolderTree EXCLUDE_FROM_ALL
    tests/test_playlist_folder_tree.cpp
    tests/playlist_folder_tree_path_conversion_stub.cpp
    src/PathUtils.cpp
)
target_include_directories(TestPlaylistFolderTree PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestPlaylistFolderTree PRIVATE /W3 /utf-8)
    set_target_properties(TestPlaylistFolderTree PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlaylistFolderTree COMMAND TestPlaylistFolderTree)
aura_set_test_labels(TestPlaylistFolderTree core)

add_executable(TestPlaylistScopeCounts EXCLUDE_FROM_ALL
    tests/test_playlist_scope_counts.cpp
)
target_include_directories(TestPlaylistScopeCounts PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestPlaylistScopeCounts PRIVATE /W3 /utf-8)
    set_target_properties(TestPlaylistScopeCounts PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlaylistScopeCounts COMMAND TestPlaylistScopeCounts)
aura_set_test_labels(TestPlaylistScopeCounts core)

add_executable(TestPlaylistCoverScope EXCLUDE_FROM_ALL
    tests/test_playlist_cover_scope.cpp
)
target_include_directories(TestPlaylistCoverScope PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
if(MSVC)
    target_compile_options(TestPlaylistCoverScope PRIVATE /W3 /utf-8)
    set_target_properties(TestPlaylistCoverScope PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlaylistCoverScope COMMAND TestPlaylistCoverScope)
aura_set_test_labels(TestPlaylistCoverScope core)

if(WIN32)
    add_executable(TestPreviewPlaylistDatabase EXCLUDE_FROM_ALL
        tests/test_preview_playlist_database.cpp
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
    target_include_directories(TestPreviewPlaylistDatabase PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
        ${SQLITE_DIR}
        ${IMGUI_DIR}
    )
    target_link_libraries(TestPreviewPlaylistDatabase PRIVATE
        opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm mfplat mfreadwrite mfuuid
    )
    target_compile_definitions(TestPreviewPlaylistDatabase PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5
        AURO_TEST_EXTERNAL_PATH_ATTRIBUTES
        ARIS_PREVIEW_ISOLATION=1
    )
    if(MSVC)
        target_compile_options(TestPreviewPlaylistDatabase PRIVATE
            /W3 /MP /utf-8
            "SHELL:/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/preview_playlist_isolation_root.h"
        )
        set_target_properties(TestPreviewPlaylistDatabase PROPERTIES
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        )
    endif()
    add_test(NAME PreviewPlaylistDatabase COMMAND TestPreviewPlaylistDatabase)
    aura_set_test_labels(PreviewPlaylistDatabase db)
endif()

add_test(
    NAME TestPreviewPlaylistContract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_preview_playlist_contract.py --build-dir ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(TestPreviewPlaylistContract PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestPreviewPlaylistContract core)

add_test(NAME TestPtslSdkLayout
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ptsl_sdk_layout.py)
aura_set_test_labels(TestPtslSdkLayout core)
add_test(NAME TestLocalizedDialogFilters
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_localized_dialog_filters.py)
aura_set_test_labels(TestLocalizedDialogFilters core)

add_executable(TestSetPlaylistParent EXCLUDE_FROM_ALL
    tests/test_set_playlist_parent.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    src/AppState.cpp
    src/Database.cpp
    src/BackfillQueue.cpp
    src/BackfillWorkerPool.cpp
    src/PathUtils.cpp
    src/platform/file_watcher.cpp
    src/Utils.cpp
    src/MetadataReader.cpp
    src/SearchQuery.cpp
    src/CoverReplacementFoundation.cpp
    src/SimilaritySearch.cpp
    src/AudioEngine.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/TimeStretch.cpp
    src/WaveformExtractor.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestSetPlaylistParent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestSetPlaylistParent PRIVATE
    opengl32
    gdi32
    ole32
    windowscodecs
    uuid
    shell32
    comdlg32
    winmm
)
target_compile_definitions(TestSetPlaylistParent PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SQLITE_ENABLE_FTS5
)
if(MSVC)
    target_compile_options(TestSetPlaylistParent PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestSetPlaylistParent PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSetPlaylistParent COMMAND TestSetPlaylistParent)
aura_set_test_labels(TestSetPlaylistParent db)

# Test: F-002-ext trigram substring search end-to-end
# Guards against the regression where UNION(FTS, trigram) branch of
# searchFiles silently returned 0 results because the outer SELECT
# referenced snippet(audio_files_fts, ...) and rank, which are not reachable
# through the UNION subquery.

add_executable(TestPlaylistQuickBarMotion EXCLUDE_FROM_ALL
    tests/test_playlist_quickbar_motion.cpp
)
target_include_directories(TestPlaylistQuickBarMotion PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(TestPlaylistQuickBarMotion PRIVATE /W3 /utf-8)
    set_target_properties(TestPlaylistQuickBarMotion PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlaylistQuickBarMotion COMMAND TestPlaylistQuickBarMotion)
aura_set_test_labels(TestPlaylistQuickBarMotion core)

add_executable(TestExportRegionPreserve EXCLUDE_FROM_ALL
    tests/test_export_region_preserve.cpp
    tests/test_transfer_engine_link_stubs.cpp
    src/TransferEngine.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestExportRegionPreserve PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(TestExportRegionPreserve PRIVATE /W3 /utf-8)
    set_target_properties(TestExportRegionPreserve PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestExportRegionPreserve COMMAND TestExportRegionPreserve)
aura_set_test_labels(TestExportRegionPreserve core)

# Waveform region editor: pure helper tests (fade clamp, trim refine window)

add_executable(TestVoiceDeliveryAcceptance EXCLUDE_FROM_ALL
    tests/test_voice_delivery_acceptance.cpp
    src/VoiceDeliveryAcceptance.cpp
)
target_include_directories(TestVoiceDeliveryAcceptance PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestVoiceDeliveryAcceptance PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestVoiceDeliveryAcceptance PRIVATE /W3 /utf-8)
    set_target_properties(TestVoiceDeliveryAcceptance PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestVoiceDeliveryAcceptance COMMAND TestVoiceDeliveryAcceptance)
aura_set_test_labels(TestVoiceDeliveryAcceptance core)

add_executable(TestSendToTargetSurfaceModel EXCLUDE_FROM_ALL
    tests/test_send_to_target_surface_model.cpp
    src/SendToTargetSurfaceModel.cpp
)
target_include_directories(TestSendToTargetSurfaceModel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(TestSendToTargetSurfaceModel PRIVATE /W3 /utf-8)
    set_target_properties(TestSendToTargetSurfaceModel PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSendToTargetSurfaceModel COMMAND TestSendToTargetSurfaceModel)
aura_set_test_labels(TestSendToTargetSurfaceModel core)

add_executable(TestCubaseDirectInsertContract EXCLUDE_FROM_ALL
    tests/test_cubase_direct_insert_contract.cpp
    tests/test_cubase_executable_path_stub.cpp
    tests/test_host_send_platform_stub.cpp
    src/CubaseAdapter.cpp
    src/HostSendModel.cpp
    src/DiagnosticsLogger.cpp
    src/Utils.cpp
    src/platform/named_pipe_win32.cpp
    src/platform/process_launch_win32.cpp
    src/platform/text_encoding_win32.cpp
)

# The contract test exercises the same helper-launch path used by packaged
# Cubase/Nuendo integrations. Keep both helpers next to the test executable so
# the test cannot silently fall back to an in-process mock.
foreach(_auro_steinberg_helper IN ITEMS HarkenSearchCubaseBridgeStub HarkenSearchNuendoBridgeStub)
    add_executable(${_auro_steinberg_helper} EXCLUDE_FROM_ALL
        src/CubaseBridgeStubMain.cpp
        src/platform/named_pipe_win32.cpp
        src/platform/system_win32.cpp
        src/platform/text_encoding_ascii.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_compile_features(${_auro_steinberg_helper} PRIVATE cxx_std_17)
    target_compile_definitions(${_auro_steinberg_helper} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        target_compile_options(${_auro_steinberg_helper} PRIVATE /W3 /utf-8)
    endif()
    target_link_libraries(${_auro_steinberg_helper} PRIVATE shell32 advapi32 ole32 comdlg32)
endforeach()
add_dependencies(TestCubaseDirectInsertContract
    HarkenSearchCubaseBridgeStub
    HarkenSearchNuendoBridgeStub
)
target_include_directories(TestCubaseDirectInsertContract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${IMGUI_DIR}
)
target_link_libraries(TestCubaseDirectInsertContract PRIVATE shell32 shlwapi)
target_compile_definitions(TestCubaseDirectInsertContract PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    AURO_CUBASE_ADAPTER_TESTING=1
    AURO_NAMED_PIPE_CLIENT_TESTING=1
    AURO_PROCESS_LAUNCH_TESTING=1
)
if(MSVC)
    target_compile_options(TestCubaseDirectInsertContract PRIVATE /W3 /utf-8)
    set_target_properties(TestCubaseDirectInsertContract PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCubaseDirectInsertContract COMMAND TestCubaseDirectInsertContract)
aura_set_test_labels(TestCubaseDirectInsertContract core)

add_executable(TestAppSettingsDefaults EXCLUDE_FROM_ALL
    tests/test_app_settings_defaults.cpp
)
target_include_directories(TestAppSettingsDefaults PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
)
if(MSVC)
    target_compile_options(TestAppSettingsDefaults PRIVATE /W3 /utf-8)
    set_target_properties(TestAppSettingsDefaults PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestAppSettingsDefaults COMMAND TestAppSettingsDefaults)
aura_set_test_labels(TestAppSettingsDefaults core)

add_executable(TestConsentManager EXCLUDE_FROM_ALL
    tests/test_consent_manager.cpp
    src/ConsentManager.cpp
)
target_include_directories(TestConsentManager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(WIN32)
    target_sources(TestConsentManager PRIVATE
        src/platform/filesystem_std.cpp
        src/platform/filesystem_win32.cpp
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_compile_definitions(TestConsentManager PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    target_sources(TestConsentManager PRIVATE src/platform/filesystem_std.cpp)
endif()
if(MSVC)
    target_compile_options(TestConsentManager PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestConsentManager COMMAND TestConsentManager)
aura_set_test_labels(TestConsentManager core legal)

add_test(
    NAME TestLegalDocumentsContract
    COMMAND ${Python3_EXECUTABLE} -m pytest tests/test_legal_documents_contract.py -q
)
set_tests_properties(TestLegalDocumentsContract PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestLegalDocumentsContract core legal)

add_executable(TestAppStateHeaderNativeFree EXCLUDE_FROM_ALL
    tests/test_app_state_header_native_free.cpp
)
target_include_directories(TestAppStateHeaderNativeFree PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
)
if(MSVC)
    target_compile_options(TestAppStateHeaderNativeFree PRIVATE /W3 /utf-8)
    set_target_properties(TestAppStateHeaderNativeFree PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestAppStateHeaderNativeFree COMMAND TestAppStateHeaderNativeFree)
aura_set_test_labels(TestAppStateHeaderNativeFree core)

add_executable(TestProductEdition EXCLUDE_FROM_ALL
    tests/test_product_edition.cpp
    src/ProductEdition.cpp
    src/ed25519/tweetnacl.c
)
target_include_directories(TestProductEdition PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_definitions(TestProductEdition PRIVATE
    AURA_BUILD_DIAGNOSTICS=0
    AURA_INTERNAL_BETA=0
    AURA_EDITION=0
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
if(MSVC)
    target_compile_options(TestProductEdition PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestProductEdition PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestProductEdition COMMAND TestProductEdition)
aura_set_test_labels(TestProductEdition core)

add_executable(TestFeatureManifest EXCLUDE_FROM_ALL
    tests/test_feature_manifest.cpp
    src/FeatureManifest.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestFeatureManifest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(WIN32)
    target_sources(TestFeatureManifest PRIVATE
        src/platform/hmac_sha256_win32.cpp
    )
    target_link_libraries(TestFeatureManifest PRIVATE bcrypt)
else()
    target_sources(TestFeatureManifest PRIVATE
        src/platform/hmac_sha256_unsupported.cpp
    )
endif()
if(MSVC)
    target_compile_options(TestFeatureManifest PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestFeatureManifest PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestFeatureManifest COMMAND TestFeatureManifest)
aura_set_test_labels(TestFeatureManifest core)

add_executable(TestSettingsEncryption EXCLUDE_FROM_ALL
    tests/test_settings_encryption.cpp
    src/Utils.cpp
)
target_sources(TestSettingsEncryption PRIVATE
    src/platform/filesystem_std.cpp
)
if(WIN32)
    target_sources(TestSettingsEncryption PRIVATE
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
        src/platform/filesystem_win32.cpp
        src/platform/process_query_win32.cpp
        src/platform/process_launch_win32.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
    )
    target_compile_definitions(TestSettingsEncryption PRIVATE
        ARIS_PREVIEW_ISOLATION=1
        ARIS_PREVIEW_ISOLATION_ROOT_UTF8="${CMAKE_CURRENT_BINARY_DIR}/Testing/settings-encryption"
    )
else()
    target_sources(TestSettingsEncryption PRIVATE tests/test_settings_encryption_layout_stub.cpp)
endif()
target_include_directories(TestSettingsEncryption PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestSettingsEncryption PRIVATE /W3 /utf-8)
    set_target_properties(TestSettingsEncryption PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
if(WIN32)
    target_link_libraries(TestSettingsEncryption PRIVATE advapi32 crypt32 shell32 ole32 comdlg32 shlwapi)
endif()
add_test(NAME TestSettingsEncryption COMMAND TestSettingsEncryption --source-contract-only)
aura_set_test_labels(TestSettingsEncryption core)
if(WIN32)
    add_executable(TestHarkenLegacyFallbackDatabasePath EXCLUDE_FROM_ALL
        tests/test_harken_legacy_fallback_database_path.cpp
        src/Utils.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
        src/platform/text_encoding_win32.cpp
        src/platform/filesystem_std.cpp
        src/platform/filesystem_win32.cpp
        src/platform/system_win32.cpp
    )
    target_include_directories(TestHarkenLegacyFallbackDatabasePath PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(TestHarkenLegacyFallbackDatabasePath PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(TestHarkenLegacyFallbackDatabasePath PRIVATE /utf-8 /Gy)
        target_link_options(TestHarkenLegacyFallbackDatabasePath PRIVATE /OPT:REF)
    endif()
    target_link_libraries(TestHarkenLegacyFallbackDatabasePath PRIVATE
        shell32 ole32 shlwapi advapi32 crypt32
    )
    add_test(NAME TestHarkenLegacyFallbackDatabasePath COMMAND TestHarkenLegacyFallbackDatabasePath)
    aura_set_test_labels(TestHarkenLegacyFallbackDatabasePath core)
endif()
add_executable(TestShutdownExitContract EXCLUDE_FROM_ALL
    tests/test_shutdown_exit_contract.cpp
)
target_include_directories(TestShutdownExitContract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(TestShutdownExitContract PRIVATE
    AURA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
if(MSVC)
    target_compile_options(TestShutdownExitContract PRIVATE /W3 /utf-8)
    set_target_properties(TestShutdownExitContract PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestShutdownExitContract COMMAND TestShutdownExitContract)
aura_set_test_labels(TestShutdownExitContract core)
if(Python3_Interpreter_FOUND)
    add_test(NAME TestSiliconFlowSecurityContract
        COMMAND ${Python3_EXECUTABLE} -m pytest -q
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_siliconflow_security_contract.py)
    aura_set_test_labels(TestSiliconFlowSecurityContract core)
endif()

# dB-norm helper for Wwise-style RMS silhouette rendering (pure math, no deps).

add_executable(TestCoverWallPageDedupe EXCLUDE_FROM_ALL
    tests/test_cover_wall_page_dedupe.cpp
    src/CoverWallPageResultBuilder.cpp
    src/PathUtils.cpp
)
target_include_directories(TestCoverWallPageDedupe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_compile_features(TestCoverWallPageDedupe PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestCoverWallPageDedupe PRIVATE /utf-8)
endif()
add_test(NAME CoverWallPageDedupe COMMAND TestCoverWallPageDedupe)
aura_set_test_labels(CoverWallPageDedupe db)

add_executable(TestDatabaseFeatures EXCLUDE_FROM_ALL
    tests/test_database_features.cpp
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
    ${AURO_AUDIO_TEXT_SOURCE}
    ${SQLITE_SOURCES}
)
if(WIN32)
    target_sources(TestDatabaseFeatures PRIVATE src/platform/filesystem_win32.cpp)
endif()
target_include_directories(TestDatabaseFeatures PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestDatabaseFeatures PRIVATE
    opengl32
    gdi32
    ole32
    windowscodecs
    uuid
    shell32
    comdlg32
    winmm
)
target_compile_definitions(TestDatabaseFeatures PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SQLITE_ENABLE_FTS5
)
if(MSVC)
    target_compile_options(TestDatabaseFeatures PRIVATE /W3 /MP /utf-8)
    # Enable console for test program
    set_target_properties(TestDatabaseFeatures PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestDatabaseFeatures COMMAND TestDatabaseFeatures)
aura_set_test_labels(TestDatabaseFeatures db)
add_test(NAME TestFavoritesMigrationError COMMAND TestDatabaseFeatures)
set_tests_properties(TestFavoritesMigrationError PROPERTIES
    ENVIRONMENT "HARKEN_FAVORITES_MIGRATION_ONLY=1")
aura_set_test_labels(TestFavoritesMigrationError db)

add_executable(TestPlaylistPerformanceContract EXCLUDE_FROM_ALL
    tests/test_playlist_performance_contract.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    tests/test_get_exe_dir_stub.cpp
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
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/audio_file_miniaudio.cpp
    src/platform/filesystem_std.cpp
    src/platform/media_foundation_audio_win32.cpp
    src/platform/metadata_legacy_tag_content_win32.cpp
    src/platform/search_text_gb2312_win32.cpp
    ${AURO_EMERGENCY_FILE_WRITER_SOURCE}
    ${AURO_THREAD_ID_SOURCE}
    ${AURO_AUDIO_TEXT_SOURCE}
    ${SQLITE_SOURCES}
)
if(WIN32)
    target_sources(TestPlaylistPerformanceContract PRIVATE src/platform/filesystem_win32.cpp)
endif()
target_include_directories(TestPlaylistPerformanceContract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestPlaylistPerformanceContract PRIVATE
    opengl32
    gdi32
    ole32
    windowscodecs
    mfplat
    mfreadwrite
    mfuuid
    uuid
    shell32
    comdlg32
    winmm
)
target_compile_definitions(TestPlaylistPerformanceContract PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SQLITE_ENABLE_FTS5
    AURO_TEST_EXTERNAL_EXE_DIR=1
    AURO_TEST_EXTERNAL_PATH_ATTRIBUTES=1
)
if(MSVC)
    target_compile_options(TestPlaylistPerformanceContract PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestPlaylistPerformanceContract PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPlaylistPerformanceContract COMMAND TestPlaylistPerformanceContract)
aura_set_test_labels(TestPlaylistPerformanceContract perf)

add_executable(TestPerfOptimizations EXCLUDE_FROM_ALL
    tests/test_perf_optimizations.cpp
    tests/test_database_link_stubs.cpp
    tests/test_appstate_link_stubs.cpp
    tests/test_get_exe_dir_stub.cpp
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
    src/platform/audio_file_miniaudio.cpp
    src/platform/filesystem_std.cpp
    src/platform/media_foundation_audio_win32.cpp
    src/platform/metadata_legacy_tag_content_win32.cpp
    src/platform/search_text_gb2312_win32.cpp
    ${AURO_EMERGENCY_FILE_WRITER_SOURCE}
    ${AURO_THREAD_ID_SOURCE}
    ${AURO_AUDIO_TEXT_SOURCE}
    ${SQLITE_SOURCES}
)
if(WIN32)
    target_sources(TestPerfOptimizations PRIVATE src/platform/filesystem_win32.cpp)
endif()
target_include_directories(TestPerfOptimizations PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestPerfOptimizations PRIVATE
    opengl32
    gdi32
    ole32
    windowscodecs
    mfplat
    mfreadwrite
    mfuuid
    uuid
    shell32
    comdlg32
    winmm
)
target_compile_definitions(TestPerfOptimizations PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SQLITE_ENABLE_FTS5
    AURO_TEST_EXTERNAL_EXE_DIR=1
    AURO_TEST_EXTERNAL_PATH_ATTRIBUTES=1
)
if(MSVC)
    target_compile_options(TestPerfOptimizations PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestPerfOptimizations PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestPerfOptimizations COMMAND TestPerfOptimizations)
aura_set_test_labels(TestPerfOptimizations perf)

foreach(_auro_db_behavior_target
    TestSearchSemanticMatrix
    TestDbSearchFacets
    TestTrigramE2E
    TestFuzzySearchPrd
    TestBackfillRecoveryWorker
    TestScanWaveformCacheContract
    TestImportPrecomputeWaveform
    TestCoverClickPerf
    TestStartupReconciliation
    TestProcessedFileExport
    TestSingleChannelExport
    TestSearchWorkspace
    TestMultiTabSync
    TestSubPlaylists
    TestSetPlaylistParent
    TestDatabaseFeatures
    TestPlaylistPerformanceContract
    TestPerfOptimizations
)
    if(TARGET ${_auro_db_behavior_target} AND AURO_BEHAVIOR_PLATFORM_SOURCES)
        target_sources(${_auro_db_behavior_target} PRIVATE ${AURO_BEHAVIOR_PLATFORM_SOURCES})
        if(WIN32)
            target_link_libraries(${_auro_db_behavior_target} PRIVATE shlwapi advapi32)
        endif()
    endif()
endforeach()

add_executable(TestCoverWallLabContract EXCLUDE_FROM_ALL
    tests/test_coverwall_lab_contract.cpp
    src/coverwall_lab/CoverWallLabState.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestCoverWallLabContract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestCoverWallLabContract PRIVATE cxx_std_17)
target_compile_definitions(TestCoverWallLabContract PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if(MSVC)
    target_compile_options(TestCoverWallLabContract PRIVATE /W3 /utf-8)
    set_target_properties(TestCoverWallLabContract PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCoverWallLabContract COMMAND TestCoverWallLabContract)
aura_set_test_labels(TestCoverWallLabContract ui)

add_executable(TestCoverReplacementFoundation EXCLUDE_FROM_ALL
    tests/test_cover_replacement_foundation.cpp
    src/CoverReplacementFoundation.cpp
)
target_include_directories(TestCoverReplacementFoundation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(TestCoverReplacementFoundation PRIVATE cxx_std_17)
target_compile_definitions(TestCoverReplacementFoundation PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if(MSVC)
    target_compile_options(TestCoverReplacementFoundation PRIVATE /W3 /utf-8)
    set_target_properties(TestCoverReplacementFoundation PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCoverReplacementFoundation COMMAND TestCoverReplacementFoundation)
aura_set_test_labels(TestCoverReplacementFoundation core)

add_executable(TestCoverBrowseSurfaceState EXCLUDE_FROM_ALL
    tests/test_cover_browse_surface_state.cpp
)
target_include_directories(TestCoverBrowseSurfaceState PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(TestCoverBrowseSurfaceState PRIVATE cxx_std_17)
target_compile_definitions(TestCoverBrowseSurfaceState PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if(MSVC)
    target_compile_options(TestCoverBrowseSurfaceState PRIVATE /W3 /utf-8)
    set_target_properties(TestCoverBrowseSurfaceState PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCoverBrowseSurfaceState COMMAND TestCoverBrowseSurfaceState)
aura_set_test_labels(TestCoverBrowseSurfaceState ui)

add_executable(TestBackgroundIoPolicy EXCLUDE_FROM_ALL
    tests/test_background_io_policy.cpp
)
target_include_directories(TestBackgroundIoPolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestBackgroundIoPolicy PRIVATE /W3 /utf-8)
    set_target_properties(TestBackgroundIoPolicy PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestBackgroundIoPolicy COMMAND TestBackgroundIoPolicy)
aura_set_test_labels(TestBackgroundIoPolicy core)

add_executable(TestAsyncSnapshotWorker EXCLUDE_FROM_ALL
    tests/test_async_snapshot_worker.cpp
)
target_include_directories(TestAsyncSnapshotWorker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestAsyncSnapshotWorker PRIVATE /W3 /utf-8)
    set_target_properties(TestAsyncSnapshotWorker PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestAsyncSnapshotWorker COMMAND TestAsyncSnapshotWorker)
aura_set_test_labels(TestAsyncSnapshotWorker core)

add_executable(TestFrameBudgetDiagnostics EXCLUDE_FROM_ALL
    tests/test_frame_budget_diagnostics.cpp
    src/FrameBudgetDiagnostics.cpp
)
target_include_directories(TestFrameBudgetDiagnostics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(TestFrameBudgetDiagnostics PRIVATE
    AURA_BUILD_DIAGNOSTICS=1 AURA_INTERNAL_BETA=0
)
add_library(TestFrameBudgetDiagnosticsNoop OBJECT EXCLUDE_FROM_ALL
    tests/test_frame_budget_diagnostics.cpp
)
target_include_directories(TestFrameBudgetDiagnosticsNoop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(TestFrameBudgetDiagnosticsNoop PRIVATE
    AURA_BUILD_DIAGNOSTICS=0 AURA_INTERNAL_BETA=1
)
add_dependencies(TestFrameBudgetDiagnostics TestFrameBudgetDiagnosticsNoop)
if(MSVC)
    target_compile_options(TestFrameBudgetDiagnostics PRIVATE /W3 /utf-8)
    target_compile_options(TestFrameBudgetDiagnosticsNoop PRIVATE /W3 /utf-8)
    set_target_properties(TestFrameBudgetDiagnostics PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestFrameBudgetDiagnostics COMMAND TestFrameBudgetDiagnostics)
aura_set_test_labels(TestFrameBudgetDiagnostics core)

if(WIN32)
    add_executable(TestDawBridgeInstaller EXCLUDE_FROM_ALL
        tests/test_daw_bridge_installer.cpp
        tests/test_daw_bridge_installer_stubs.cpp
        src/DawBridgeInstaller.cpp
    )
    target_include_directories(TestDawBridgeInstaller PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(TestDawBridgeInstaller PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        target_compile_options(TestDawBridgeInstaller PRIVATE /W3 /utf-8)
        set_target_properties(TestDawBridgeInstaller PROPERTIES
            LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        )
    endif()
    add_test(NAME TestDawBridgeInstaller COMMAND TestDawBridgeInstaller)
    aura_set_test_labels(TestDawBridgeInstaller daw)
endif()

add_executable(TestReaperBridgeContract EXCLUDE_FROM_ALL
    tests/test_reaper_bridge_contract.cpp
    src/ReaperBridgeClient.cpp
    src/ReaperBridgeDoctor.cpp
    src/platform/filesystem_std.cpp
    src/platform/named_pipe_win32.cpp
    src/platform/system_win32.cpp
    src/platform/text_encoding_win32.cpp
    src/platform/text_encoding_ascii.cpp
    src/platform/executable_path_win32.cpp
    src/platform/pe_inspection_win32.cpp
    src/platform/file_hash_win32.cpp
    src/platform/durable_file_ops_win32.cpp
    src/platform/harken_user_data_layout.cpp
    src/platform/harken_legacy_data_migrator.cpp
)
target_include_directories(TestReaperBridgeContract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(TestReaperBridgeContract PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    ARIS_REAPER_TEST_DLL_PATH="$<TARGET_FILE:reaper_harken>"
)
target_link_libraries(TestReaperBridgeContract PRIVATE
    version shell32 advapi32 bcrypt ole32 comdlg32
)
add_dependencies(TestReaperBridgeContract reaper_harken)
if(MSVC)
    target_compile_options(TestReaperBridgeContract PRIVATE /W3 /utf-8)
    set_target_properties(TestReaperBridgeContract PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestReaperBridgeContract COMMAND TestReaperBridgeContract)
aura_set_test_labels(TestReaperBridgeContract daw)

# AUR-P1-047 Slice 2 - REAPER extension stub DLL (reaper_harken.dll).
# REAPER scans UserPlugins\ for files whose name starts with "reaper_".
# OUTPUT_NAME "reaper_harken" ensures the file is named reaper_harken.dll.
# Only built when the REAPER SDK submodule is present (AURO_REAPER_SDK_PRESENT).
if(WIN32 AND AURO_REAPER_SDK_PRESENT)
    add_library(HarkenSearchReaperBridge SHARED EXCLUDE_FROM_ALL
        src/ReaperBridgeStub.cpp
    )
    set_target_properties(HarkenSearchReaperBridge PROPERTIES
        OUTPUT_NAME "reaper_harken"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )
    target_include_directories(HarkenSearchReaperBridge PRIVATE
        "${AURO_REAPER_SDK_ROOT}"
    )
    if(MSVC)
        target_compile_options(HarkenSearchReaperBridge PRIVATE /W3 /utf-8)
    endif()
endif()

# AUR-P1-047 Slice 2: REAPER bridge stub load test.
# Builds the DLL in test mode, LoadLibraryW, calls entry point with mock rec,
# verifies return codes. Gracefully skips when SDK is absent.

add_executable(TestCoverReplacementIntegration EXCLUDE_FROM_ALL
    tests/test_cover_replacement_integration.cpp
)
target_include_directories(TestCoverReplacementIntegration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(TestCoverReplacementIntegration PRIVATE cxx_std_17)
target_compile_definitions(TestCoverReplacementIntegration PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if(MSVC)
    target_compile_options(TestCoverReplacementIntegration PRIVATE /W3 /utf-8)
    set_target_properties(TestCoverReplacementIntegration PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCoverReplacementIntegration COMMAND TestCoverReplacementIntegration)
aura_set_test_labels(TestCoverReplacementIntegration core)

add_executable(TestMiniWaveformCap EXCLUDE_FROM_ALL
    tests/test_mini_waveform_cap.cpp
)
target_include_directories(TestMiniWaveformCap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(TestMiniWaveformCap PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(TestMiniWaveformCap PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestMiniWaveformCap PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE" LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()
add_test(NAME TestMiniWaveformCap COMMAND TestMiniWaveformCap)
aura_set_test_labels(TestMiniWaveformCap core)

add_executable(TestMiniWaveformScheduler EXCLUDE_FROM_ALL
    tests/test_mini_waveform_scheduler.cpp
)
target_include_directories(TestMiniWaveformScheduler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestMiniWaveformScheduler PRIVATE /W3 /utf-8)
    set_target_properties(TestMiniWaveformScheduler PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestMiniWaveformScheduler COMMAND TestMiniWaveformScheduler)
aura_set_test_labels(TestMiniWaveformScheduler core)

add_executable(TestMiniWaveformPresentation EXCLUDE_FROM_ALL
    tests/test_mini_waveform_presentation.cpp
)
target_include_directories(TestMiniWaveformPresentation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(TestMiniWaveformPresentation PRIVATE /W3 /utf-8)
    set_target_properties(TestMiniWaveformPresentation PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestMiniWaveformPresentation COMMAND TestMiniWaveformPresentation)
aura_set_test_labels(TestMiniWaveformPresentation core)

add_executable(TestMiniRenderMath EXCLUDE_FROM_ALL
    tests/test_mini_render_math.cpp
)
target_include_directories(TestMiniRenderMath PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestMiniRenderMath PRIVATE /W3 /utf-8)
    set_target_properties(TestMiniRenderMath PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestMiniRenderMath COMMAND TestMiniRenderMath)
aura_set_test_labels(TestMiniRenderMath core)

add_executable(TestMiniEnvelopeBuild EXCLUDE_FROM_ALL
    tests/test_mini_envelope_build.cpp
    tests/miniaudio_impl.cpp
    src/DiagnosticsLogger.cpp
    src/WaveformExtractor.cpp
    src/TimeStretch.cpp
    src/NativeFloatReader.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
    ${AURO_EMERGENCY_FILE_WRITER_SOURCE}
    ${AURO_THREAD_ID_SOURCE}
)
if(WIN32)
    target_sources(TestMiniEnvelopeBuild PRIVATE src/platform/media_foundation_audio_win32.cpp)
    target_link_libraries(TestMiniEnvelopeBuild PRIVATE
        mfplat mfreadwrite mfuuid
    )
endif()
target_include_directories(TestMiniEnvelopeBuild PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_definitions(TestMiniEnvelopeBuild PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
if(MSVC)
    target_compile_options(TestMiniEnvelopeBuild PRIVATE /W3 /utf-8)
    set_target_properties(TestMiniEnvelopeBuild PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestMiniEnvelopeBuild COMMAND TestMiniEnvelopeBuild)
aura_set_test_labels(TestMiniEnvelopeBuild core)

add_executable(TestWaveformCachePolicy EXCLUDE_FROM_ALL
    tests/test_waveform_cache_policy.cpp
)
target_include_directories(TestWaveformCachePolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestWaveformCachePolicy PRIVATE /W3 /utf-8)
    set_target_properties(TestWaveformCachePolicy PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestWaveformCachePolicy COMMAND TestWaveformCachePolicy)
aura_set_test_labels(TestWaveformCachePolicy core)

add_executable(TestWaveformCacheBudgetPolicy EXCLUDE_FROM_ALL
    tests/test_waveform_cache_budget_policy.cpp
)
target_include_directories(TestWaveformCacheBudgetPolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestWaveformCacheBudgetPolicy PRIVATE /W3 /utf-8)
    set_target_properties(TestWaveformCacheBudgetPolicy PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestWaveformCacheBudgetPolicy COMMAND TestWaveformCacheBudgetPolicy)
aura_set_test_labels(TestWaveformCacheBudgetPolicy core)

add_executable(TestWaveformPrewarmPolicy EXCLUDE_FROM_ALL
    tests/test_waveform_prewarm_policy.cpp
)
target_include_directories(TestWaveformPrewarmPolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestWaveformPrewarmPolicy PRIVATE /W3 /utf-8)
    set_target_properties(TestWaveformPrewarmPolicy PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestWaveformPrewarmPolicy COMMAND TestWaveformPrewarmPolicy)
aura_set_test_labels(TestWaveformPrewarmPolicy core)

# Test: signed min/max mini envelope generation.


add_executable(TestWaveformBucketMath EXCLUDE_FROM_ALL
    tests/test_waveform_bucket_math.cpp
)
target_include_directories(TestWaveformBucketMath PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
if(MSVC)
    target_compile_options(TestWaveformBucketMath PRIVATE /W3 /utf-8)
    set_target_properties(TestWaveformBucketMath PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestWaveformBucketMath COMMAND TestWaveformBucketMath)
aura_set_test_labels(TestWaveformBucketMath core)


add_executable(TestDatabaseNavNonblockingContract EXCLUDE_FROM_ALL
    tests/test_database_nav_nonblocking_contract.cpp
)
if(MSVC)
    target_compile_options(TestDatabaseNavNonblockingContract PRIVATE /W3 /utf-8)
    set_target_properties(TestDatabaseNavNonblockingContract PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestDatabaseNavNonblockingContract COMMAND TestDatabaseNavNonblockingContract)
aura_set_test_labels(TestDatabaseNavNonblockingContract core)

# F-033 Phase 0 QA: DB schema migration + TagVocabularyMapper unit tests


add_executable(TestTransportContrast EXCLUDE_FROM_ALL tests/test_transport_contrast.cpp)
add_executable(TestEmptyStateContrast EXCLUDE_FROM_ALL tests/test_empty_state_contrast.cpp src/BuiltinThemeScheme.cpp)
add_dependencies(TestEmptyStateContrast ArisLocalizationGenerated)
target_include_directories(TestEmptyStateContrast PRIVATE src ${IMGUI_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated thirdparty)
if(MSVC)
    target_compile_options(TestEmptyStateContrast PRIVATE /utf-8)
endif()
add_test(NAME TestEmptyStateContrast COMMAND TestEmptyStateContrast)
add_executable(TestMistBlueTheme EXCLUDE_FROM_ALL tests/test_mist_blue_theme.cpp src/BuiltinThemeScheme.cpp)
add_dependencies(TestMistBlueTheme ArisLocalizationGenerated)
target_include_directories(TestMistBlueTheme PRIVATE src ${IMGUI_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated thirdparty)
if(MSVC)
    target_compile_options(TestMistBlueTheme PRIVATE /utf-8)
endif()
add_test(NAME TestMistBlueTheme COMMAND TestMistBlueTheme)
aura_set_test_labels(TestMistBlueTheme ui)
aura_set_test_labels(TestEmptyStateContrast ui)
add_test(NAME TestEmptyStateThemeAudit COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_empty_state_theme_audit.py)
aura_set_test_labels(TestEmptyStateThemeAudit ui)
add_test(NAME TestInfoFamilyThemeAudit COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_info_family_theme_audit.py)
aura_set_test_labels(TestInfoFamilyThemeAudit ui)
target_include_directories(TestTransportContrast PRIVATE src ${IMGUI_DIR})
add_test(NAME TestTransportContrast COMMAND TestTransportContrast)
aura_set_test_labels(TestTransportContrast ui)
add_test(NAME TestTransportThemeAudit COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_transport_theme_audit.py)
aura_set_test_labels(TestTransportThemeAudit ui)
add_executable(TestTransportBlendInput EXCLUDE_FROM_ALL tests/test_transport_blend_input.cpp
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)
target_include_directories(TestTransportBlendInput PRIVATE src ${IMGUI_DIR})
add_test(NAME TestTransportBlendInput COMMAND TestTransportBlendInput)
aura_set_test_labels(TestTransportBlendInput ui)

add_executable(TestThemeColors EXCLUDE_FROM_ALL
    tests/test_theme_colors.cpp
    src/Theme.cpp
    src/ui/CoreWorkspaceStyle.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
    src/HarkenSearchThemeManager.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
add_dependencies(TestThemeColors ArisLocalizationGenerated)
target_include_directories(TestThemeColors PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${IMGUI_DIR}
)
target_link_libraries(TestThemeColors PRIVATE advapi32)
if(MSVC)
    target_compile_options(TestThemeColors PRIVATE /W3 /utf-8)
    set_target_properties(TestThemeColors PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestThemeColors COMMAND TestThemeColors)
aura_set_test_labels(TestThemeColors nightly)

add_executable(TestCoreWorkspaceStyle EXCLUDE_FROM_ALL
    tests/test_core_workspace_style.cpp
    src/ui/CoreWorkspaceStyle.cpp
    src/ui/ModuleWindowShell.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
target_include_directories(TestCoreWorkspaceStyle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${IMGUI_DIR}
)
target_compile_features(TestCoreWorkspaceStyle PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestCoreWorkspaceStyle PRIVATE /W3 /utf-8)
    set_target_properties(TestCoreWorkspaceStyle PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCoreWorkspaceStyle COMMAND TestCoreWorkspaceStyle)
aura_set_test_labels(TestCoreWorkspaceStyle core)

add_test(
    NAME TestCoreWorkspaceStyleBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_core_workspace_style_boundary.py
)
set_tests_properties(TestCoreWorkspaceStyleBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestCoreWorkspaceStyleBoundary core)

add_executable(TestAuditionFrontendController EXCLUDE_FROM_ALL
    tests/test_audition_frontend_controller.cpp
    src/frontend/AuditionFrontendController.cpp
    src/ui/AuditionTransportView.cpp
)
target_include_directories(TestAuditionFrontendController PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestAuditionFrontendController PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAuditionFrontendController PRIVATE /W3 /utf-8)
    set_target_properties(TestAuditionFrontendController PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestAuditionFrontendController COMMAND TestAuditionFrontendController)
aura_set_test_labels(TestAuditionFrontendController core ui)

add_executable(TestAuditionNativeIntentWorkflow EXCLUDE_FROM_ALL
    tests/test_audition_appstate_adapter_workflow.cpp
    src/frontend/AuditionNativeIntentWorkflow.cpp
)
target_include_directories(TestAuditionNativeIntentWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestAuditionNativeIntentWorkflow PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestAuditionNativeIntentWorkflow PRIVATE /W3 /utf-8)
    set_target_properties(TestAuditionNativeIntentWorkflow PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestAuditionNativeIntentWorkflow COMMAND TestAuditionNativeIntentWorkflow)
aura_set_test_labels(TestAuditionNativeIntentWorkflow core ui)

add_test(
    NAME TestAuditionFrontendBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audition_frontend_boundary.py
)
set_tests_properties(TestAuditionFrontendBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestAuditionFrontendBoundary core ui stage4)

add_executable(TestReverseReloadContract EXCLUDE_FROM_ALL
    tests/test_reverse_reload_contract.cpp
)
target_include_directories(TestReverseReloadContract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestReverseReloadContract PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestReverseReloadContract PRIVATE /W3 /utf-8)
    set_target_properties(TestReverseReloadContract PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestReverseReloadContract COMMAND TestReverseReloadContract)
aura_set_test_labels(TestReverseReloadContract core)

add_executable(TestScanWorkflowState EXCLUDE_FROM_ALL
    tests/test_scan_workflow_state.cpp
)
target_include_directories(TestScanWorkflowState PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestScanWorkflowState PRIVATE /W3 /utf-8)
    set_target_properties(TestScanWorkflowState PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestScanWorkflowState COMMAND TestScanWorkflowState)
aura_set_test_labels(TestScanWorkflowState core)

if(Python3_Interpreter_FOUND)
    add_test(
        NAME TestScanPauseResumeContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scan_pause_resume_contract.py"
    )
    set_tests_properties(TestScanPauseResumeContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestScanPauseResumeContract core)

    add_test(
        NAME TestScanBackgroundIndicatorContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scan_background_indicator_contract.py"
    )
    set_tests_properties(TestScanBackgroundIndicatorContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestScanBackgroundIndicatorContract core)

    add_test(
        NAME TestScanQueryStatusContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scan_query_status_contract.py"
    )
    set_tests_properties(TestScanQueryStatusContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestScanQueryStatusContract core)

    add_test(
        NAME TestWaveformFooterLoudnessLabelContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_waveform_footer_loudness_label_contract.py"
    )
    set_tests_properties(TestWaveformFooterLoudnessLabelContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestWaveformFooterLoudnessLabelContract core)

    add_test(
        NAME TestEmptyResultsPresetOverlapContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_empty_results_preset_overlap_contract.py"
    )
    set_tests_properties(TestEmptyResultsPresetOverlapContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestEmptyResultsPresetOverlapContract core)

    add_test(
        NAME TestEmptyListInnerChromeContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_empty_list_inner_chrome_contract.py"
    )
    set_tests_properties(TestEmptyListInnerChromeContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestEmptyListInnerChromeContract core)

    add_test(
        NAME TestEmptyLibraryHeaderInsetContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_empty_library_header_inset_contract.py"
    )
    set_tests_properties(TestEmptyLibraryHeaderInsetContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestEmptyLibraryHeaderInsetContract core)

    add_test(
        NAME TestPlayerFooterControlStripContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_player_footer_control_strip_contract.py"
    )
    set_tests_properties(TestPlayerFooterControlStripContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestPlayerFooterControlStripContract core)

    add_test(
        NAME TestScanDialogWaveformPolicyCopyContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scan_dialog_waveform_policy_copy_contract.py"
    )
    set_tests_properties(TestScanDialogWaveformPolicyCopyContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestScanDialogWaveformPolicyCopyContract core)

    add_test(
        NAME TestScanDialogSimilarIndexOptionContract
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scan_dialog_similar_index_option_contract.py"
    )
    set_tests_properties(TestScanDialogSimilarIndexOptionContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestScanDialogSimilarIndexOptionContract core)
endif()

# Standalone Utils tests share a narrow executable-directory platform stub.
add_library(HarkenSearchTestPlatformExeDirStub OBJECT
    tests/test_platform_exe_dir_stub.cpp
)

foreach(_auro_utils_test IN ITEMS
    TestStage6PlaybackLoadSeekProbe
    TestStage6ScanTerminalThroughputProbe
    TestSearchSemanticMatrix
    TestSearchQueryTrigram
    TestSearchRecall
    TestThesaurusBidirectional
    TestDbSearchFacets
    TestTrigramE2E
    TestFuzzySearchPrd
    TestBackfillRecoveryWorker
    TestScanWaveformCacheContract
    TestScanPauseResume
    TestImportPrecomputeWaveform
    TestCoverPathNorm
    TestCoverClickPerf
    TestStartupReconciliation
    TestProcessedFileExport
    TestSingleChannelExport
    TestSearchWorkspace
    TestOpenWith
    TestMultiTabSync
    TestSubPlaylists
    TestPlaylistFolderTree
    TestSetPlaylistParent
    TestCubaseDirectInsertContract
    TestSettingsEncryption
    TestDatabaseFeatures
    TestUtilsTextEncoding
)
    # This target now links the real system adapter, which owns GetExeDir.
    if(WIN32 AND _auro_utils_test STREQUAL "TestSettingsEncryption")
        continue()
    endif()
    target_compile_definitions(${_auro_utils_test} PRIVATE
        AURO_TEST_EXTERNAL_EXE_DIR=1
    )
    target_sources(${_auro_utils_test} PRIVATE
        $<TARGET_OBJECTS:HarkenSearchTestPlatformExeDirStub>
    )
endforeach()

foreach(_auro_utils_filesystem_test IN ITEMS
    TestSearchQueryTrigram
    TestSearchRecall
    TestThesaurusBidirectional
    TestCoverPathNorm
    TestOpenWith
    TestPlaylistFolderTree
    TestSettingsEncryption
    TestUtilsTextEncoding
)
    target_sources(${_auro_utils_filesystem_test} PRIVATE src/platform/filesystem_std.cpp)
endforeach()

foreach(_auro_utils_encoding_test IN ITEMS
    TestSearchQueryTrigram
    TestSearchRecall
    TestThesaurusBidirectional
    TestCoverPathNorm
    TestOpenWith
    TestPlaylistFolderTree
    TestSettingsEncryption
    TestUtilsTextEncoding
)
    get_target_property(_auro_utils_encoding_sources ${_auro_utils_encoding_test} SOURCES)
    if(WIN32)
        set(_auro_utils_encoding_source src/platform/text_encoding_win32.cpp)
    else()
        set(_auro_utils_encoding_source src/platform/text_encoding_std.cpp)
    endif()
    list(FIND _auro_utils_encoding_sources "${_auro_utils_encoding_source}" _auro_utils_encoding_index)
    if(_auro_utils_encoding_index EQUAL -1)
        target_sources(${_auro_utils_encoding_test} PRIVATE ${_auro_utils_encoding_source})
    endif()
endforeach()

set(AURO_WAVEFORM_TEXT_ENCODING_TARGETS
    TestStage6PlaybackLoadSeekProbe
    TestStage6ScanTerminalThroughputProbe
    TestSearchSemanticMatrix
    TestDbSearchFacets
    TestTrigramE2E
    TestFuzzySearchPrd
    TestBackfillRecoveryWorker
    TestScanWaveformCacheContract
    TestScanPauseResume
    TestImportPrecomputeWaveform
    TestCoverClickPerf
    TestStartupReconciliation
    TestProcessedFileExport
    TestSingleChannelExport
    TestSearchWorkspace
    TestMultiTabSync
    TestSubPlaylists
    TestSetPlaylistParent
    TestDatabaseFeatures
)
foreach(_auro_waveform_text_target IN LISTS AURO_WAVEFORM_TEXT_ENCODING_TARGETS)
    target_sources(${_auro_waveform_text_target} PRIVATE
        ${AURO_MEDIA_FOUNDATION_AUDIO_SOURCE}
    )
    if(WIN32)
        target_sources(${_auro_waveform_text_target} PRIVATE
            src/platform/text_encoding_win32.cpp
        )
        target_link_libraries(${_auro_waveform_text_target} PRIVATE
            mfplat mfreadwrite mfuuid
        )
    else()
        target_sources(${_auro_waveform_text_target} PRIVATE
            src/platform/text_encoding_std.cpp
        )
    endif()
endforeach()

# Waveform-backed behavior targets share the native reader/data-source PAL.
# Keep the topology explicit while avoiding duplicate source entries for the
# probes that already list these files in their executable declarations.
foreach(_auro_waveform_pal_target IN LISTS AURO_WAVEFORM_TEXT_ENCODING_TARGETS)
    target_compile_definitions(${_auro_waveform_pal_target} PRIVATE
        AURO_TEST_EXTERNAL_PATH_ATTRIBUTES=1
    )
    get_target_property(_auro_waveform_sources ${_auro_waveform_pal_target} SOURCES)
    if(NOT _auro_waveform_sources)
        continue()
    endif()
    foreach(_auro_waveform_source IN ITEMS
            src/NativeFloatReader.cpp
            src/platform/audio_data_source_miniaudio.cpp
            src/platform/audio_file_miniaudio.cpp)
        list(FIND _auro_waveform_sources "${_auro_waveform_source}" _auro_waveform_source_index)
        if(_auro_waveform_source_index EQUAL -1)
            target_sources(${_auro_waveform_pal_target} PRIVATE ${_auro_waveform_source})
        endif()
    endforeach()
    if(WIN32)
        list(FIND _auro_waveform_sources "src/platform/filesystem_win32.cpp" _auro_waveform_fs_index)
        if(_auro_waveform_fs_index EQUAL -1)
            target_sources(${_auro_waveform_pal_target} PRIVATE src/platform/filesystem_win32.cpp)
        endif()
    endif()
endforeach()

set(AURO_METADATA_LEGACY_TAG_CONTENT_TARGETS
    TestMetadataReaderPathEncoding
    TestMetadataReaderContentEncoding
    TestStage6PlaybackLoadSeekProbe
    TestStage6ScanTerminalThroughputProbe
    TestSearchSemanticMatrix
    TestDbSearchFacets
    TestTrigramE2E
    TestFuzzySearchPrd
    TestBackfillRecoveryWorker
    TestScanWaveformCacheContract
    TestScanPauseResume
    TestImportPrecomputeWaveform
    TestCoverClickPerf
    TestStartupReconciliation
    TestProcessedFileExport
    TestSingleChannelExport
    TestSearchWorkspace
    TestMultiTabSync
    TestSubPlaylists
    TestSetPlaylistParent
    TestDatabaseFeatures
)
foreach(_target IN LISTS AURO_METADATA_LEGACY_TAG_CONTENT_TARGETS)
    target_sources(${_target} PRIVATE
        ${AURO_METADATA_LEGACY_TAG_CONTENT_SOURCE}
    )
endforeach()

set(AURO_SEARCH_TEXT_GB2312_TARGETS
    TestStage6PlaybackLoadSeekProbe
    TestStage6ScanTerminalThroughputProbe
    TestSearchSemanticMatrix
    TestSearchQueryTrigram
    TestSearchRecall
    TestThesaurusBidirectional
    TestDbSearchFacets
    TestTrigramE2E
    TestFuzzySearchPrd
    TestBackfillRecoveryWorker
    TestScanWaveformCacheContract
    TestScanPauseResume
    TestImportPrecomputeWaveform
    TestCoverClickPerf
    TestStartupReconciliation
    TestProcessedFileExport
    TestSingleChannelExport
    TestSearchWorkspace
    TestMultiTabSync
    TestSubPlaylists
    TestSetPlaylistParent
    TestDatabaseFeatures
)
foreach(_target IN LISTS AURO_SEARCH_TEXT_GB2312_TARGETS)
    target_sources(${_target} PRIVATE
        ${AURO_SEARCH_TEXT_GB2312_SOURCE}
    )
endforeach()

add_test(
    NAME TestMainEncodingContract
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_main_encoding_contract.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
aura_set_test_labels(TestMainEncodingContract core)

add_test(
    NAME TestAudioEngineWaveformInternalTypesContract
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audio_engine_waveform_internal_types.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
aura_set_test_labels(TestAudioEngineWaveformInternalTypesContract core)

# Theme editor bindings for playlist gradient + waveform background

add_executable(TestEmergencyFileWriter EXCLUDE_FROM_ALL
    tests/test_emergency_file_writer.cpp
    ${AURO_EMERGENCY_FILE_WRITER_SOURCE}
)
target_include_directories(TestEmergencyFileWriter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestEmergencyFileWriter PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestEmergencyFileWriter PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestEmergencyFileWriter COMMAND TestEmergencyFileWriter)
aura_set_test_labels(TestEmergencyFileWriter core)

add_executable(TestScanDatabaseOpenDiagnostics EXCLUDE_FROM_ALL
    tests/test_scan_database_open_diagnostics.cpp
    src/ScanDatabaseOpenDiagnostics.cpp
)
target_include_directories(TestScanDatabaseOpenDiagnostics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestScanDatabaseOpenDiagnostics PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestScanDatabaseOpenDiagnostics PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestScanDatabaseOpenDiagnostics COMMAND TestScanDatabaseOpenDiagnostics)
aura_set_test_labels(TestScanDatabaseOpenDiagnostics core)

add_executable(TestDiagnosticsLoggerPersistence EXCLUDE_FROM_ALL
    tests/test_diagnostics_logger_persistence.cpp
    src/DiagnosticsLogger.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
    ${AURO_EMERGENCY_FILE_WRITER_SOURCE}
    ${AURO_THREAD_ID_SOURCE}
)
target_include_directories(TestDiagnosticsLoggerPersistence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestDiagnosticsLoggerPersistence PRIVATE cxx_std_17)
target_compile_definitions(TestDiagnosticsLoggerPersistence PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(TestDiagnosticsLoggerPersistence PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestDiagnosticsLoggerPersistence COMMAND TestDiagnosticsLoggerPersistence)
aura_set_test_labels(TestDiagnosticsLoggerPersistence core)

add_executable(TestDiagnosticFileLifecycle
    tests/test_diagnostic_file_lifecycle.cpp
    src/DiagnosticFileLifecycle.cpp
    src/platform/filesystem_std.cpp
)
if(WIN32)
    target_sources(TestDiagnosticFileLifecycle PRIVATE src/platform/text_encoding_win32.cpp src/platform/filesystem_win32.cpp)
else()
    target_sources(TestDiagnosticFileLifecycle PRIVATE src/platform/text_encoding_std.cpp)
endif()
target_include_directories(TestDiagnosticFileLifecycle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(TestDiagnosticFileLifecycle PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestDiagnosticFileLifecycle PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestDiagnosticFileLifecycle COMMAND TestDiagnosticFileLifecycle)
aura_set_test_labels(TestDiagnosticFileLifecycle core)

add_executable(TestNativeFloatReaderContract EXCLUDE_FROM_ALL
    tests/test_native_float_reader_contract.cpp
    src/NativeFloatReader.cpp
    src/platform/audio_data_source_miniaudio.cpp
    tests/miniaudio_impl.cpp
)
target_include_directories(TestNativeFloatReaderContract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestNativeFloatReaderContract PRIVATE cxx_std_17)
if(WIN32)
    target_sources(TestNativeFloatReaderContract PRIVATE src/platform/text_encoding_win32.cpp)
else()
    target_sources(TestNativeFloatReaderContract PRIVATE src/platform/text_encoding_std.cpp)
endif()
if(MSVC)
    target_compile_options(TestNativeFloatReaderContract PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestNativeFloatReaderContract COMMAND TestNativeFloatReaderContract)
aura_set_test_labels(TestNativeFloatReaderContract audio)

add_executable(TestWaveformNativeReaderMiniSeq EXCLUDE_FROM_ALL
    tests/test_waveform_native_reader_mini_seq.cpp
)
target_compile_features(TestWaveformNativeReaderMiniSeq PRIVATE cxx_std_17)
add_test(NAME TestWaveformNativeReaderMiniSeq COMMAND TestWaveformNativeReaderMiniSeq)
aura_set_test_labels(TestWaveformNativeReaderMiniSeq audio)

add_executable(TestWaveformNativeReaderSeekPreview EXCLUDE_FROM_ALL
    tests/test_waveform_native_reader_seek_preview.cpp
)
target_compile_features(TestWaveformNativeReaderSeekPreview PRIVATE cxx_std_17)
add_test(NAME TestWaveformNativeReaderSeekPreview COMMAND TestWaveformNativeReaderSeekPreview)
aura_set_test_labels(TestWaveformNativeReaderSeekPreview audio)

add_executable(TestWaveformNativeReaderDetail EXCLUDE_FROM_ALL
    tests/test_waveform_native_reader_detail.cpp
)
target_compile_features(TestWaveformNativeReaderDetail PRIVATE cxx_std_17)
add_test(NAME TestWaveformNativeReaderDetail COMMAND TestWaveformNativeReaderDetail)
aura_set_test_labels(TestWaveformNativeReaderDetail audio)

add_executable(TestWaveformNativeReaderRms EXCLUDE_FROM_ALL
    tests/test_waveform_native_reader_rms.cpp
)
target_compile_features(TestWaveformNativeReaderRms PRIVATE cxx_std_17)
add_test(NAME TestWaveformNativeReaderRms COMMAND TestWaveformNativeReaderRms)
aura_set_test_labels(TestWaveformNativeReaderRms audio)

add_executable(TestWaveformNativeReaderNoLegacyDecoder EXCLUDE_FROM_ALL
    tests/test_waveform_native_reader_no_legacy_decoder.cpp
)
target_compile_features(TestWaveformNativeReaderNoLegacyDecoder PRIVATE cxx_std_17)
add_test(NAME TestWaveformNativeReaderNoLegacyDecoder COMMAND TestWaveformNativeReaderNoLegacyDecoder)
aura_set_test_labels(TestWaveformNativeReaderNoLegacyDecoder audio)

if(WIN32)
    add_executable(TestNativeFloatReaderWindowsContract EXCLUDE_FROM_ALL
        tests/test_native_float_reader_windows_contract.cpp
    )
    target_compile_definitions(TestNativeFloatReaderWindowsContract PRIVATE
        ARIS_TEST_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_features(TestNativeFloatReaderWindowsContract PRIVATE cxx_std_17)
    add_test(NAME TestNativeFloatReaderWindowsContract COMMAND TestNativeFloatReaderWindowsContract)
    aura_set_test_labels(TestNativeFloatReaderWindowsContract audio)
endif()

if(WIN32)
    add_executable(TestPlatformNamedPipeServer EXCLUDE_FROM_ALL
        tests/test_platform_named_pipe_server.cpp
        src/platform/named_pipe_win32.cpp
    )
    target_include_directories(TestPlatformNamedPipeServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(TestPlatformNamedPipeServer PRIVATE AURO_NAMED_PIPE_SERVER_TESTING=1)
    target_compile_features(TestPlatformNamedPipeServer PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(TestPlatformNamedPipeServer PRIVATE /W3 /utf-8)
    endif()
    add_test(NAME TestPlatformNamedPipeServer COMMAND TestPlatformNamedPipeServer)
    aura_set_test_labels(TestPlatformNamedPipeServer core)

    add_executable(TestPlatformNamedPipeClient EXCLUDE_FROM_ALL
        tests/test_platform_named_pipe_client.cpp
        src/platform/named_pipe_win32.cpp
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_include_directories(TestPlatformNamedPipeClient PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(TestPlatformNamedPipeClient PRIVATE AURO_NAMED_PIPE_CLIENT_TESTING=1)
    target_compile_features(TestPlatformNamedPipeClient PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(TestPlatformNamedPipeClient PRIVATE /W3 /utf-8)
    endif()
    add_test(NAME TestPlatformNamedPipeClient COMMAND TestPlatformNamedPipeClient)
    aura_set_test_labels(TestPlatformNamedPipeClient core)

    add_executable(TestPlatformProcessLaunch EXCLUDE_FROM_ALL
        tests/test_platform_process_launch.cpp
        src/platform/process_launch_win32.cpp
    )
    target_include_directories(TestPlatformProcessLaunch PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(TestPlatformProcessLaunch PRIVATE AURO_PROCESS_LAUNCH_TESTING=1)
    target_compile_features(TestPlatformProcessLaunch PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(TestPlatformProcessLaunch PRIVATE /W3 /utf-8)
    endif()
    add_test(NAME TestPlatformProcessLaunch COMMAND TestPlatformProcessLaunch)
    aura_set_test_labels(TestPlatformProcessLaunch core)

    add_executable(TestPlatformProcessQuery EXCLUDE_FROM_ALL
        tests/test_platform_process_query.cpp
        src/platform/process_query_win32.cpp
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
    target_include_directories(TestPlatformProcessQuery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(TestPlatformProcessQuery PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(TestPlatformProcessQuery PRIVATE /W3 /utf-8)
    endif()
    add_test(NAME TestPlatformProcessQuery COMMAND TestPlatformProcessQuery)
    aura_set_test_labels(TestPlatformProcessQuery core)
endif()

foreach(_auro_diagnostics_test IN ITEMS
    TestStage6FileWatcherProbe TestStage6PlaybackLoadSeekProbe TestStage6ScanTerminalThroughputProbe
    TestSearchSemanticMatrix TestDbSearchFacets TestTrigramE2E TestFuzzySearchPrd
    TestBackfillRecoveryWorker TestScanWaveformCacheContract TestScanPauseResume
    TestImportPrecomputeWaveform TestCoverClickPerf TestCoverDiagnostics TestStartupReconciliation
    TestProcessedFileExport TestSingleChannelExport TestSearchWorkspace TestMultiTabSync
    TestSubPlaylists TestSetPlaylistParent TestCubaseDirectInsertContract TestDatabaseFeatures
)
    target_sources(${_auro_diagnostics_test} PRIVATE
        src/platform/filesystem_std.cpp
        ${AURO_EMERGENCY_FILE_WRITER_SOURCE}
        ${AURO_THREAD_ID_SOURCE}
    )
endforeach()

# Main-line tests added after the Stage 6 branch split. Keep them wired against
# the final PAL sources used by the merged tree.
add_executable(TestAnnotationBridgeSnapshot EXCLUDE_FROM_ALL
    tests/test_annotation_bridge.cpp
    src/AnnotationBridge.cpp
    src/ThemeInspector.cpp
    src/platform/filesystem_std.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
if(WIN32)
    target_sources(TestAnnotationBridgeSnapshot PRIVATE
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
endif()
target_include_directories(TestAnnotationBridgeSnapshot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${IMGUI_DIR}
)
if(MSVC)
    target_compile_options(TestAnnotationBridgeSnapshot PRIVATE /W3 /utf-8)
endif()
add_test(NAME AnnotationBridgeSnapshot COMMAND TestAnnotationBridgeSnapshot)
aura_set_test_labels(AnnotationBridgeSnapshot core)

add_executable(TestWorkerThreadPolicy EXCLUDE_FROM_ALL
    tests/test_worker_thread_policy.cpp
)
target_include_directories(TestWorkerThreadPolicy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(TestWorkerThreadPolicy PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestWorkerThreadPolicy COMMAND TestWorkerThreadPolicy)
aura_set_test_labels(TestWorkerThreadPolicy core)

add_executable(TestRenderFrameRequestPolicy EXCLUDE_FROM_ALL
    tests/test_render_frame_request_policy.cpp
    src/RenderFrameRequestPolicy.cpp
)
target_include_directories(TestRenderFrameRequestPolicy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestRenderFrameRequestPolicy PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestRenderFrameRequestPolicy PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestRenderFrameRequestPolicy COMMAND TestRenderFrameRequestPolicy)
aura_set_test_labels(TestRenderFrameRequestPolicy core)

add_test(
    NAME TestStage4DependencyAudit
    COMMAND ${Python3_EXECUTABLE} -m pytest tests/test_stage4_dependency_audit.py -q
)
set_tests_properties(TestStage4DependencyAudit PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestStage4DependencyAudit core)

add_test(
    NAME TestFeedbackTransportBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest tests/test_feedback_transport_boundary.py -q
)
set_tests_properties(TestFeedbackTransportBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestFeedbackTransportBoundary core)

add_executable(TestMemoryBudget EXCLUDE_FROM_ALL
    tests/test_memory_budget.cpp
    src/MemoryBudget.cpp
)
target_include_directories(TestMemoryBudget PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestMemoryBudget PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestMemoryBudget COMMAND TestMemoryBudget)
aura_set_test_labels(TestMemoryBudget core)

add_executable(TestPlatformAtomicFileReplace EXCLUDE_FROM_ALL
    tests/test_platform_atomic_file_replace.cpp
    src/platform/filesystem_std.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
if(WIN32)
    target_sources(TestPlatformAtomicFileReplace PRIVATE
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
endif()
target_include_directories(TestPlatformAtomicFileReplace PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestPlatformAtomicFileReplace PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestPlatformAtomicFileReplace COMMAND TestPlatformAtomicFileReplace)
aura_set_test_labels(TestPlatformAtomicFileReplace core)

add_executable(TestTransferEmbed EXCLUDE_FROM_ALL
    tests/test_transfer_embed.cpp
    src/EmbedScript.cpp
    src/MetadataReader.cpp
    src/MetadataWriter.cpp
    src/platform/filesystem_std.cpp
    src/platform/metadata_legacy_tag_content_win32.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
if(WIN32)
    target_sources(TestTransferEmbed PRIVATE src/platform/filesystem_win32.cpp)
endif()
target_include_directories(TestTransferEmbed PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
if(MSVC)
    target_compile_options(TestTransferEmbed PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestTransferEmbed COMMAND TestTransferEmbed)
aura_set_test_labels(TestTransferEmbed core)

add_executable(TestMainPerfLogPal EXCLUDE_FROM_ALL
    tests/test_main_perf_log_pal.cpp
)
if(MSVC)
    target_compile_options(TestMainPerfLogPal PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestMainPerfLogPal COMMAND TestMainPerfLogPal)
aura_set_test_labels(TestMainPerfLogPal core)

add_test(
    NAME TestMetadataWriterAtomicSwapContract
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_metadata_writer_atomic_swap_contract.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
aura_set_test_labels(TestMetadataWriterAtomicSwapContract core)

add_test(
    NAME TestCoverRemoveResultContract
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_remove_result_contract.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
aura_set_test_labels(TestCoverRemoveResultContract core)
add_executable(TestSimilaritySearch EXCLUDE_FROM_ALL
    tests/test_similarity_search.cpp
    tests/test_similarity_search_database_stub.cpp
    src/SimilaritySearch.cpp
    ${AURO_AUDIO_TEXT_SOURCE}
)
target_include_directories(TestSimilaritySearch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestSimilaritySearch PRIVATE /W3 /utf-8)
    set_target_properties(TestSimilaritySearch PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSimilaritySearch COMMAND TestSimilaritySearch)
aura_set_test_labels(TestSimilaritySearch core)

add_executable(TestSimilarityWorkflow EXCLUDE_FROM_ALL
    tests/test_similarity_workflow.cpp
    src/SimilarityWorkflow.cpp
)
target_include_directories(TestSimilarityWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(TestSimilarityWorkflow PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(TestSimilarityWorkflow PRIVATE /W3 /utf-8)
    set_target_properties(TestSimilarityWorkflow PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSimilarityWorkflow COMMAND TestSimilarityWorkflow)
aura_set_test_labels(TestSimilarityWorkflow core)

add_executable(TestFeatureExtractor EXCLUDE_FROM_ALL
    tests/test_feature_extractor.cpp
    tests/miniaudio_impl.cpp
    src/FeatureExtractor.cpp
    src/platform/audio_file_miniaudio.cpp
    src/Utils.cpp
)
if(WIN32)
    target_sources(TestFeatureExtractor PRIVATE src/platform/text_encoding_win32.cpp)
    target_sources(TestFeatureExtractor PRIVATE
        src/platform/filesystem_std.cpp
        src/platform/system_win32.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
    )
else()
    target_sources(TestFeatureExtractor PRIVATE
        src/platform/text_encoding_std.cpp
        src/platform/filesystem_std.cpp
    )
endif()
target_include_directories(TestFeatureExtractor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MSVC)
    target_compile_options(TestFeatureExtractor PRIVATE /W3 /utf-8)
    set_target_properties(TestFeatureExtractor PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestFeatureExtractor COMMAND TestFeatureExtractor)
aura_set_test_labels(TestFeatureExtractor core)

add_executable(TestSimilarityQueryService EXCLUDE_FROM_ALL
    tests/test_similarity_query_service.cpp
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
    src/SimilarityWorkflow.cpp
    src/SimilarityQueryService.cpp
    src/FeatureExtractor.cpp
    src/AudioEngine.cpp
    src/TagVocabularyMapper.cpp
    src/DiagnosticsLogger.cpp
    src/CenterSenseDataSource.cpp
    src/CenterSenseDsp.cpp
    src/TimeStretch.cpp
    src/WaveformExtractor.cpp
    src/NativeFloatReader.cpp
    src/platform/filesystem_std.cpp
    src/platform/audio_data_source_miniaudio.cpp
    src/platform/audio_file_miniaudio.cpp
    src/platform/audio_playback_miniaudio.cpp
    src/platform/media_foundation_audio_win32.cpp
    src/platform/metadata_legacy_tag_content_win32.cpp
    src/platform/emergency_file_writer_win32.cpp
    src/platform/thread_id_win32.cpp
    ${SQLITE_SOURCES}
)
target_include_directories(TestSimilarityQueryService PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${SQLITE_DIR}
    ${IMGUI_DIR}
)
target_link_libraries(TestSimilarityQueryService PRIVATE
    opengl32 gdi32 ole32 windowscodecs uuid shell32 comdlg32 winmm Threads::Threads
)
target_compile_definitions(TestSimilarityQueryService PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SQLITE_ENABLE_FTS5
    AURO_TEST_REAL_SIMILARITY_WORKFLOW=1
    AURO_TEST_EXTERNAL_PATH_ATTRIBUTES=1
)
if(WIN32)
    target_sources(TestSimilarityQueryService PRIVATE
        src/platform/filesystem_win32.cpp
        src/platform/text_encoding_win32.cpp
        src/platform/search_text_gb2312_win32.cpp
    )
    target_link_libraries(TestSimilarityQueryService PRIVATE
        mfplat mfreadwrite mfuuid
    )
else()
    target_sources(TestSimilarityQueryService PRIVATE
        src/platform/text_encoding_std.cpp
        src/platform/search_text_gb2312_std.cpp
    )
endif()
if(MSVC)
    target_compile_options(TestSimilarityQueryService PRIVATE /W3 /MP /utf-8)
    set_target_properties(TestSimilarityQueryService PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestSimilarityQueryService COMMAND TestSimilarityQueryService)
aura_set_test_labels(TestSimilarityQueryService db)

add_executable(TestLocalization EXCLUDE_FROM_ALL
    tests/test_localization.cpp
    src/Localization.cpp
)
add_dependencies(TestLocalization ArisLocalizationGenerated)
target_include_directories(TestLocalization PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${IMGUI_DIR}
)
if(MSVC)
    target_compile_options(TestLocalization PRIVATE /W3 /utf-8)
    set_target_properties(TestLocalization PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestLocalization COMMAND TestLocalization)
aura_set_test_labels(TestLocalization core)

if(Python3_Interpreter_FOUND)
    add_test(
        NAME TestLocalizationLedger
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_localization_ledger.py"
    )
    set_tests_properties(TestLocalizationLedger PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestLocalizationLedger core)

    add_test(
        NAME TestLanguagePurity
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_localization_purity.py"
    )
    set_tests_properties(TestLanguagePurity PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestLanguagePurity core)

    add_test(
        NAME TestPurity3ComboAndReleaseNotesContract
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_purity3_combo_and_release_notes_contract.py"
    )
    set_tests_properties(TestPurity3ComboAndReleaseNotesContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestPurity3ComboAndReleaseNotesContract core)

endif()
if(Python3_Interpreter_FOUND)
    add_custom_target(TestSimilarSoundsWindowContract
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_similar_sounds_window_contract.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestSimilarSoundsWindowContract
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_similar_sounds_window_contract.py"
    )
    set_tests_properties(TestSimilarSoundsWindowContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestSimilarSoundsWindowContract ui)

    add_custom_target(TestRelinkWorkflowBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_relink_workflow_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestRelinkWorkflowBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_relink_workflow_boundary.py"
    )
    set_tests_properties(TestRelinkWorkflowBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestRelinkWorkflowBoundary ui)

add_executable(TestRelinkWorkflowLifecycle EXCLUDE_FROM_ALL
    tests/test_relink_workflow_lifecycle.cpp
)
target_compile_features(TestRelinkWorkflowLifecycle PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestRelinkWorkflowLifecycle PRIVATE /W3 /utf-8)
    set_target_properties(TestRelinkWorkflowLifecycle PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestRelinkWorkflowLifecycle COMMAND TestRelinkWorkflowLifecycle)
aura_set_test_labels(TestRelinkWorkflowLifecycle ui)


    add_custom_target(TestDatabaseOpenDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_open_dialog_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestDatabaseOpenDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_open_dialog_boundary.py"
    )
    set_tests_properties(TestDatabaseOpenDialogBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestDatabaseOpenDialogBoundary platform stage4)

    add_custom_target(TestExecutableFileDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_executable_file_dialog_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestExecutableFileDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_executable_file_dialog_boundary.py"
    )
    set_tests_properties(TestExecutableFileDialogBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestExecutableFileDialogBoundary platform stage4)

    add_custom_target(TestAvatarImageFileDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_avatar_image_file_dialog_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestAvatarImageFileDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_avatar_image_file_dialog_boundary.py"
    )
    set_tests_properties(TestAvatarImageFileDialogBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestAvatarImageFileDialogBoundary platform stage4)

    add_custom_target(TestReaperFileListDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_file_list_dialog_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestReaperFileListDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reaper_file_list_dialog_boundary.py"
    )
    set_tests_properties(TestReaperFileListDialogBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestReaperFileListDialogBoundary platform stage4)

    add_custom_target(TestThesaurusJsonDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_thesaurus_json_dialog_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestThesaurusJsonDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_thesaurus_json_dialog_boundary.py"
    )
    set_tests_properties(TestThesaurusJsonDialogBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestThesaurusJsonDialogBoundary platform stage4)

    add_custom_target(TestSettingsImportDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_settings_import_dialog_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestSettingsImportDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_settings_import_dialog_boundary.py"
    )
    set_tests_properties(TestSettingsImportDialogBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestSettingsImportDialogBoundary platform stage4)

    add_custom_target(TestCoverImageImportDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_image_import_dialog_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestCoverImageImportDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cover_image_import_dialog_boundary.py"
    )
    set_tests_properties(TestCoverImageImportDialogBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestCoverImageImportDialogBoundary platform stage4)

    add_custom_target(TestRegionWavExportDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_region_wav_export_dialog_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestRegionWavExportDialogBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_region_wav_export_dialog_boundary.py"
    )
    set_tests_properties(TestRegionWavExportDialogBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestRegionWavExportDialogBoundary platform stage4)

    add_custom_target(TestRecordRemovalWorkflowBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_record_removal_workflow_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestRecordRemovalWorkflowBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_record_removal_workflow_boundary.py"
    )
    set_tests_properties(TestRecordRemovalWorkflowBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestRecordRemovalWorkflowBoundary core stage4)

    add_custom_target(TestRecordRenameWorkflowBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_record_rename_workflow_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestRecordRenameWorkflowBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_record_rename_workflow_boundary.py"
    )
    set_tests_properties(TestRecordRenameWorkflowBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestRecordRenameWorkflowBoundary core stage4)

    add_custom_target(TestBatchRenamePolicyBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_batch_rename_policy_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestBatchRenamePolicyBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_batch_rename_policy_boundary.py"
    )
    set_tests_properties(TestBatchRenamePolicyBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestBatchRenamePolicyBoundary core stage4)

    add_custom_target(TestBatchRenameExecutionWorkflowBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_batch_rename_execution_workflow_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestBatchRenameExecutionWorkflowBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_batch_rename_execution_workflow_boundary.py"
    )
    set_tests_properties(TestBatchRenameExecutionWorkflowBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestBatchRenameExecutionWorkflowBoundary core stage4)

    add_custom_target(TestBrowserSystemFolderSearchPolicyBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_browser_system_folder_search_policy_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestBrowserSystemFolderSearchPolicyBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_browser_system_folder_search_policy_boundary.py"
    )
    set_tests_properties(TestBrowserSystemFolderSearchPolicyBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestBrowserSystemFolderSearchPolicyBoundary core stage4)

    add_custom_target(TestBrowserSystemFolderSearchRuntimeBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_browser_system_folder_search_runtime_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestBrowserSystemFolderSearchRuntimeBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_browser_system_folder_search_runtime_boundary.py"
    )
    set_tests_properties(TestBrowserSystemFolderSearchRuntimeBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestBrowserSystemFolderSearchRuntimeBoundary core stage4)

    add_custom_target(TestFilePlayerDeferredDbWarmupRuntimeBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_file_player_deferred_db_warmup_runtime_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestFilePlayerDeferredDbWarmupRuntimeBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_file_player_deferred_db_warmup_runtime_boundary.py"
    )
    set_tests_properties(TestFilePlayerDeferredDbWarmupRuntimeBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestFilePlayerDeferredDbWarmupRuntimeBoundary core stage4)

    add_custom_target(TestFilePlayerDatabaseOpenWorkflowBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_file_player_database_open_workflow_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestFilePlayerDatabaseOpenWorkflowBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_file_player_database_open_workflow_boundary.py"
    )
    set_tests_properties(TestFilePlayerDatabaseOpenWorkflowBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestFilePlayerDatabaseOpenWorkflowBoundary core stage4)

    add_custom_target(TestDatabaseSwitchWorkerRuntimeBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_switch_worker_runtime_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestDatabaseSwitchWorkerRuntimeBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_switch_worker_runtime_boundary.py"
    )
    set_tests_properties(TestDatabaseSwitchWorkerRuntimeBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestDatabaseSwitchWorkerRuntimeBoundary core stage4)

    add_custom_target(TestDatabaseSwitchBlockingOperationBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_switch_blocking_operation_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestDatabaseSwitchBlockingOperationBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_database_switch_blocking_operation_boundary.py"
    )
    set_tests_properties(TestDatabaseSwitchBlockingOperationBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestDatabaseSwitchBlockingOperationBoundary core stage4)

    add_custom_target(TestSameFolderScanRuntimeBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_same_folder_scan_runtime_boundary.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestSameFolderScanRuntimeBoundary
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_same_folder_scan_runtime_boundary.py"
    )
    set_tests_properties(TestSameFolderScanRuntimeBoundary PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestSameFolderScanRuntimeBoundary core stage4)

    add_custom_target(TestWebUiResultRenderBudgetContract
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_webui_result_render_budget_contract.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestWebUiResultRenderBudgetContract
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_webui_result_render_budget_contract.py"
    )
    set_tests_properties(TestWebUiResultRenderBudgetContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestWebUiResultRenderBudgetContract core stage6)

    add_custom_target(TestWebUiTransportPollingBudgetContract
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_webui_transport_polling_budget_contract.py"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    add_test(
        NAME TestWebUiTransportPollingBudgetContract
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_webui_transport_polling_budget_contract.py"
    )
    set_tests_properties(TestWebUiTransportPollingBudgetContract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    aura_set_test_labels(TestWebUiTransportPollingBudgetContract core stage6)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_cuevi_feedback_client.cpp)
add_executable(TestCueviFeedbackClient EXCLUDE_FROM_ALL
    tests/test_cuevi_feedback_client.cpp
    src/CueviFeedbackClient.cpp
)
if(WIN32)
    target_sources(TestCueviFeedbackClient PRIVATE
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
    )
elseif(APPLE)
    target_sources(TestCueviFeedbackClient PRIVATE
        src/platform/system_macos.mm
        src/platform/text_encoding_std.cpp
    )
else()
    target_sources(TestCueviFeedbackClient PRIVATE src/platform/text_encoding_std.cpp)
endif()
target_include_directories(TestCueviFeedbackClient PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestCueviFeedbackClient PRIVATE cxx_std_17)
if(WIN32)
    target_compile_definitions(TestCueviFeedbackClient PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(TestCueviFeedbackClient PRIVATE wininet shell32 advapi32 ole32 comdlg32)
endif()
if(MSVC)
    target_compile_options(TestCueviFeedbackClient PRIVATE /W3 /utf-8)
    set_target_properties(TestCueviFeedbackClient PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestCueviFeedbackClient COMMAND TestCueviFeedbackClient)
aura_set_test_labels(TestCueviFeedbackClient core)
endif()

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_cuevi_feedback_live_client.cpp)
add_executable(TestCueviFeedbackLiveClient EXCLUDE_FROM_ALL
    tests/test_cuevi_feedback_live_client.cpp
    src/CueviFeedbackClient.cpp
)
if(WIN32)
    target_sources(TestCueviFeedbackLiveClient PRIVATE
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
        src/platform/harken_user_data_layout.cpp
        src/platform/harken_legacy_data_migrator.cpp
    )
    target_compile_definitions(TestCueviFeedbackLiveClient PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(TestCueviFeedbackLiveClient PRIVATE wininet shell32 advapi32 ole32 comdlg32)
else()
    target_sources(TestCueviFeedbackLiveClient PRIVATE src/platform/text_encoding_std.cpp)
endif()
target_include_directories(TestCueviFeedbackLiveClient PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestCueviFeedbackLiveClient PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestCueviFeedbackLiveClient PRIVATE /W3 /utf-8)
endif()
if(ARIS_ENABLE_LIVE_FEEDBACK_E2E)
    add_test(NAME TestCueviFeedbackLiveE2E COMMAND
        ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_cuevi_feedback_e2e.py
        --probe-exe $<TARGET_FILE:TestCueviFeedbackLiveClient>
    )
endif()

endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_cuevi_feedback_completion_workflow.cpp)
add_executable(TestCueviFeedbackCompletionWorkflow EXCLUDE_FROM_ALL
    tests/test_cuevi_feedback_completion_workflow.cpp
    src/CueviFeedbackCompletionWorkflow.cpp
    src/CueviFeedbackClient.cpp
)
if(WIN32)
    target_sources(TestCueviFeedbackCompletionWorkflow PRIVATE
        src/platform/system_win32.cpp
        src/platform/text_encoding_win32.cpp
    )
elseif(APPLE)
    target_sources(TestCueviFeedbackCompletionWorkflow PRIVATE
        src/platform/system_macos.mm
        src/platform/text_encoding_std.cpp
    )
else()
    target_sources(TestCueviFeedbackCompletionWorkflow PRIVATE
        src/platform/text_encoding_std.cpp
    )
endif()

endif()
add_executable(TestUpdateManifest EXCLUDE_FROM_ALL
    tests/test_update_manifest.cpp
    src/update/UpdateManifest.cpp
    src/ed25519/tweetnacl.c
)
target_include_directories(TestUpdateManifest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestUpdateManifest PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdateManifest PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateManifest PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateManifest COMMAND TestUpdateManifest)
aura_set_test_labels(TestUpdateManifest core)

add_test(
    NAME TestUpdateManifestBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_manifest_boundary.py
)
set_tests_properties(TestUpdateManifestBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateManifestBoundary core stage4)

add_executable(TestUpdateCheckService EXCLUDE_FROM_ALL
    tests/test_update_check_service.cpp
    src/update/UpdateCheckService.cpp
    src/update/UpdateManifest.cpp
    src/update/UpdateRuntime.cpp
    src/update/UpdateCoordinator.cpp
    src/update/UpdateDownloadManager.cpp
    src/update/UpdateHttpTransport.cpp
    src/update/UpdateStateStore.cpp
    src/ed25519/tweetnacl.c
)
target_include_directories(TestUpdateCheckService PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestUpdateCheckService PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdateCheckService PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateCheckService PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateCheckService COMMAND TestUpdateCheckService)
aura_set_test_labels(TestUpdateCheckService core stage4)

add_executable(TestUpdateApplicationService EXCLUDE_FROM_ALL
    tests/test_update_application_service.cpp
    src/update/UpdateApplicationService.cpp
    src/update/UpdateCheckService.cpp
    src/update/UpdateManifest.cpp
    src/update/UpdateRuntime.cpp
    src/update/UpdateCoordinator.cpp
    src/update/UpdateDownloadManager.cpp
    src/update/UpdateHttpTransport.cpp
    src/update/UpdateStateStore.cpp
    src/ed25519/tweetnacl.c
)
target_include_directories(TestUpdateApplicationService PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestUpdateApplicationService PRIVATE cxx_std_17)
target_compile_definitions(TestUpdateApplicationService PRIVATE
    ARIS_UPDATE_GITHUB_REPOSITORY="xindong/aris"
)
if(MSVC)
    target_compile_options(TestUpdateApplicationService PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateApplicationService PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateApplicationService COMMAND TestUpdateApplicationService)
aura_set_test_labels(TestUpdateApplicationService core stage4)
add_test(
    NAME TestUpdatePersistedErrorLifecycle
    COMMAND TestUpdateApplicationService --persisted-error-lifecycle
)
aura_set_test_labels(TestUpdatePersistedErrorLifecycle core stage4)

add_test(
    NAME TestUpdateApplicationServiceBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_application_service_boundary.py
)
set_tests_properties(TestUpdateApplicationServiceBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateApplicationServiceBoundary core stage4)

# Not EXCLUDE_FROM_ALL: this guards user-facing wording, so it must build and
# run in the ordinary test sweep rather than only when someone asks for it.
add_executable(TestUpdateFailureExplanation
    tests/test_update_failure_explanation.cpp
    src/update/HarkenSearchUpdateFailureExplanation.cpp
)
target_include_directories(TestUpdateFailureExplanation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestUpdateFailureExplanation PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdateFailureExplanation PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateFailureExplanation PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateFailureExplanation COMMAND TestUpdateFailureExplanation)
aura_set_test_labels(TestUpdateFailureExplanation core stage4)

# Guards against an old persisted failure being shown as the current state.
add_executable(TestUpdateStaleError
    tests/test_update_stale_error.cpp
    src/update/HarkenSearchUpdateFailureExplanation.cpp
)
target_include_directories(TestUpdateStaleError PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestUpdateStaleError PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdateStaleError PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateStaleError PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateStaleError COMMAND TestUpdateStaleError)
aura_set_test_labels(TestUpdateStaleError core stage4)

# Also guards user-facing wording, so it runs in the ordinary sweep.
add_executable(TestBootstrapExitExplanation
    tests/test_bootstrap_exit_explanation.cpp
    src/bootstrap/HarkenSearchBootstrapExitExplanation.cpp
    src/platform/update_bootstrap_authority_base.cpp
    src/update/UpdateDownloadManager.cpp
    src/platform/update_persistence_port.cpp
    src/platform/text_encoding_win32.cpp
)
target_include_directories(TestBootstrapExitExplanation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_link_libraries(TestBootstrapExitExplanation PRIVATE bcrypt)
target_compile_definitions(TestBootstrapExitExplanation PRIVATE
    ARIS_UPDATE_PERSISTENCE_TESTING=1
)
target_compile_features(TestBootstrapExitExplanation PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestBootstrapExitExplanation PRIVATE /W3 /utf-8)
    set_target_properties(TestBootstrapExitExplanation PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestBootstrapExitExplanation COMMAND TestBootstrapExitExplanation)
aura_set_test_labels(TestBootstrapExitExplanation core stage4)

# Guards self-update on non-system drives; must run in the ordinary sweep.
add_executable(TestUpdaterBaseVolume
    tests/test_updater_base_volume.cpp
    src/platform/update_bootstrap_authority_base.cpp
)
target_include_directories(TestUpdaterBaseVolume PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestUpdaterBaseVolume PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdaterBaseVolume PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdaterBaseVolume PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdaterBaseVolume COMMAND TestUpdaterBaseVolume)
aura_set_test_labels(TestUpdaterBaseVolume core stage4)

if(WIN32)
add_executable(TestUpdateCheckHttpsTransport EXCLUDE_FROM_ALL
    tests/test_update_check_https_transport.cpp
    src/update/UpdateCheckHttpsTransport.cpp
    src/platform/update_check_https_transport_win32.cpp
)
target_include_directories(TestUpdateCheckHttpsTransport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestUpdateCheckHttpsTransport PRIVATE cxx_std_17)
target_compile_definitions(TestUpdateCheckHttpsTransport PRIVATE
    ARIS_UPDATE_MIRROR_BASE_URL=\"https://harkensearch.xd.com/aris/\"
)
target_link_libraries(TestUpdateCheckHttpsTransport PRIVATE wininet)
if(MSVC)
    target_compile_options(TestUpdateCheckHttpsTransport PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateCheckHttpsTransport PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateCheckHttpsTransport COMMAND TestUpdateCheckHttpsTransport)
aura_set_test_labels(TestUpdateCheckHttpsTransport core stage4 stage6)
endif()

add_test(
    NAME TestUpdateCheckHttpsTransportBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_check_https_transport_boundary.py
)
set_tests_properties(TestUpdateCheckHttpsTransportBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateCheckHttpsTransportBoundary core stage4 stage6)

add_test(
    NAME TestUpdateCheckServiceBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_check_service_boundary.py
)
set_tests_properties(TestUpdateCheckServiceBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateCheckServiceBoundary core stage4)

add_executable(TestUpdateCoordinator EXCLUDE_FROM_ALL
    tests/test_update_coordinator.cpp
    src/update/UpdateCoordinator.cpp
)
target_include_directories(TestUpdateCoordinator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestUpdateCoordinator PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdateCoordinator PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateCoordinator PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateCoordinator COMMAND TestUpdateCoordinator)
aura_set_test_labels(TestUpdateCoordinator core stage4)

add_executable(TestUpdateRuntime EXCLUDE_FROM_ALL
    tests/test_update_runtime.cpp
    src/update/UpdateRuntime.cpp
    src/update/UpdateCoordinator.cpp
    src/update/UpdateDownloadManager.cpp
    src/update/UpdateHttpTransport.cpp
    src/update/UpdateStateStore.cpp
)
target_include_directories(TestUpdateRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestUpdateRuntime PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdateRuntime PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateRuntime PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateRuntime COMMAND TestUpdateRuntime)
aura_set_test_labels(TestUpdateRuntime core stage4)

add_executable(TestHarkenSearchBootstrapPlan EXCLUDE_FROM_ALL
    tests/bootstrap/test_aris_bootstrap_plan.cpp
)
target_link_libraries(TestHarkenSearchBootstrapPlan PRIVATE HarkenSearchBootstrapCore)
target_compile_features(TestHarkenSearchBootstrapPlan PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestHarkenSearchBootstrapPlan PRIVATE /W4 /WX /permissive- /utf-8)
    set_target_properties(TestHarkenSearchBootstrapPlan PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestHarkenSearchBootstrapPlan COMMAND TestHarkenSearchBootstrapPlan)
aura_set_test_labels(TestHarkenSearchBootstrapPlan core stage4)

add_test(
    NAME TestHarkenSearchBootstrapPlanBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_aris_bootstrap_plan_boundary.py
)
set_tests_properties(TestHarkenSearchBootstrapPlanBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestHarkenSearchBootstrapPlanBoundary core stage4)

add_test(
    NAME TestUpdateRuntimeBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_runtime_boundary.py
)
set_tests_properties(TestUpdateRuntimeBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateRuntimeBoundary core stage4)

add_test(
    NAME TestUpdateCoordinatorBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_coordinator_boundary.py
)
set_tests_properties(TestUpdateCoordinatorBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateCoordinatorBoundary core stage4)

add_executable(TestUpdateDownloadManager EXCLUDE_FROM_ALL
    tests/test_update_download_manager.cpp
    src/update/UpdateDownloadManager.cpp
)
target_include_directories(TestUpdateDownloadManager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestUpdateDownloadManager PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdateDownloadManager PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateDownloadManager PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateDownloadManager COMMAND TestUpdateDownloadManager)
aura_set_test_labels(TestUpdateDownloadManager core stage4)

add_test(
    NAME TestUpdateDownloadManagerBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_download_manager_boundary.py
)
set_tests_properties(TestUpdateDownloadManagerBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateDownloadManagerBoundary core stage4)

add_executable(TestUpdateStateStore EXCLUDE_FROM_ALL
    tests/test_update_state_store.cpp
    src/update/UpdateStateStore.cpp
)
target_include_directories(TestUpdateStateStore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestUpdateStateStore PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(TestUpdateStateStore PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateStateStore PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateStateStore COMMAND TestUpdateStateStore)
aura_set_test_labels(TestUpdateStateStore core stage4)

add_test(
    NAME TestUpdateStateStoreBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_state_store_boundary.py
)
set_tests_properties(TestUpdateStateStoreBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateStateStoreBoundary core stage4)

if(WIN32)
add_executable(TestUpdateHttpTransport EXCLUDE_FROM_ALL
    tests/test_update_http_transport.cpp
    src/update/UpdateHttpTransport.cpp
    src/update/UpdateDownloadManager.cpp
)
target_include_directories(TestUpdateHttpTransport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestUpdateHttpTransport PRIVATE cxx_std_17)
target_compile_definitions(TestUpdateHttpTransport PRIVATE
    ARIS_UPDATE_MIRROR_BASE_URL=\"https://harkensearch.xd.com/aris/\"
)
if(MSVC)
    target_compile_options(TestUpdateHttpTransport PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateHttpTransport PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateHttpTransport COMMAND TestUpdateHttpTransport)
aura_set_test_labels(TestUpdateHttpTransport core stage4)

add_executable(TestUpdateHttpTransportPlatform EXCLUDE_FROM_ALL
    tests/test_update_http_transport_platform.cpp
    src/update/UpdateHttpTransport.cpp
    src/update/UpdateDownloadManager.cpp
    src/platform/update_http_transport.cpp
    src/platform/text_encoding_win32.cpp
    src/platform/text_encoding_ascii.cpp
)
target_include_directories(TestUpdateHttpTransportPlatform PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(TestUpdateHttpTransportPlatform PRIVATE cxx_std_17)
target_compile_definitions(TestUpdateHttpTransportPlatform PRIVATE
    ARIS_UPDATE_HTTP_TRANSPORT_TESTING=1
    ARIS_UPDATE_MIRROR_BASE_URL=\"https://harkensearch.xd.com/aris/\"
)
target_link_libraries(TestUpdateHttpTransportPlatform PRIVATE wininet)
if(MSVC)
    target_compile_options(TestUpdateHttpTransportPlatform PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdateHttpTransportPlatform PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdateHttpTransportPlatform COMMAND TestUpdateHttpTransportPlatform)
aura_set_test_labels(TestUpdateHttpTransportPlatform core stage4 stage6)

add_test(
    NAME TestUpdateHttpTransportBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_http_transport_boundary.py
)
set_tests_properties(TestUpdateHttpTransportBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateHttpTransportBoundary core stage4 stage6)

add_executable(TestUpdatePersistencePort EXCLUDE_FROM_ALL
    tests/test_update_persistence_port.cpp
    src/update/UpdateDownloadManager.cpp
    src/update/UpdateHttpTransport.cpp
    src/platform/update_persistence_port.cpp
    src/platform/update_bootstrap_authority_base.cpp
    src/platform/filesystem_std.cpp
)
target_sources(TestUpdatePersistencePort PRIVATE
    src/platform/filesystem_win32.cpp
    src/platform/text_encoding_win32.cpp
    src/platform/text_encoding_ascii.cpp
)
target_link_libraries(TestUpdatePersistencePort PRIVATE
    bcrypt shell32 ole32
)
target_include_directories(TestUpdatePersistencePort PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestUpdatePersistencePort PRIVATE cxx_std_17)
target_compile_definitions(TestUpdatePersistencePort PRIVATE
    ARIS_UPDATE_PERSISTENCE_TESTING=1
)
if(MSVC)
    target_compile_options(TestUpdatePersistencePort PRIVATE /W3 /utf-8)
    set_target_properties(TestUpdatePersistencePort PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(NAME TestUpdatePersistencePort COMMAND TestUpdatePersistencePort)
aura_set_test_labels(TestUpdatePersistencePort core stage4 stage6)

add_test(
    NAME TestUpdatePersistencePortBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_persistence_port_boundary.py
)
set_tests_properties(TestUpdatePersistencePortBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdatePersistencePortBoundary core stage4 stage6)

add_test(
    NAME TestUpdateProductComposition
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_product_composition.py
)
set_tests_properties(TestUpdateProductComposition PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateProductComposition core stage4)

add_test(
    NAME TestUpdateCompositionPlatformBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_composition_platform_boundary.py
)
set_tests_properties(TestUpdateCompositionPlatformBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateCompositionPlatformBoundary core stage6)

add_test(
    NAME TestUpdateBootstrapAuthorityPortBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_bootstrap_authority_port_boundary.py
)
set_tests_properties(TestUpdateBootstrapAuthorityPortBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateBootstrapAuthorityPortBoundary core stage6)

add_test(
    NAME TestUpdateBootstrapHealthCapacityBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_update_bootstrap_health_capacity_boundary.py
)
set_tests_properties(TestUpdateBootstrapHealthCapacityBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestUpdateBootstrapHealthCapacityBoundary core stage6)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/test_cuevi_feedback_completion_workflow.cpp)
target_include_directories(TestCueviFeedbackCompletionWorkflow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
)
target_compile_features(TestCueviFeedbackCompletionWorkflow PRIVATE cxx_std_17)
if(WIN32)
    target_compile_definitions(TestCueviFeedbackCompletionWorkflow PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_link_libraries(TestCueviFeedbackCompletionWorkflow PRIVATE
        wininet shell32 advapi32 ole32 comdlg32
    )
endif()
if(MSVC)
    target_compile_options(TestCueviFeedbackCompletionWorkflow PRIVATE /W3 /utf-8)
    set_target_properties(TestCueviFeedbackCompletionWorkflow PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    )
endif()
add_test(
    NAME TestCueviFeedbackCompletionWorkflow
    COMMAND TestCueviFeedbackCompletionWorkflow
)
aura_set_test_labels(TestCueviFeedbackCompletionWorkflow core stage4)
endif()

add_test(
    NAME TestCueviFeedbackCompletionWorkflowBoundary
    COMMAND ${Python3_EXECUTABLE} -m pytest -q
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cuevi_feedback_completion_workflow_boundary.py
)
set_tests_properties(TestCueviFeedbackCompletionWorkflowBoundary PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
aura_set_test_labels(TestCueviFeedbackCompletionWorkflowBoundary core stage4)

# These focused tests do not exercise credentials or user-data migration.
# Supply inert test dependencies without linking the real credential manager.
foreach(_utils_isolated_target IN ITEMS
    TestBatchRenameExecutionWorkflow TestBatchRenamePolicy
    TestBrowserSystemFolderSearchRuntime TestCoverDiagnostics TestCoverPathNorm
    TestCubaseDirectInsertContract TestOpenWith TestSearchQueryTrigram
    TestSearchRecall TestThesaurusBidirectional TestUtilsTextEncoding)
    target_sources(${_utils_isolated_target} PRIVATE
        tests/test_settings_encryption_layout_stub.cpp
        tests/test_utils_credentials_stub.cpp)
    target_include_directories(${_utils_isolated_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endforeach()

if(WIN32)
    foreach(_filesystem_target IN ITEMS
        TestExportRegionPreserve TestHostSendRuntime
        TestMetadataReaderContentEncoding TestMetadataReaderPathEncoding
        TestNamingScriptConflict)
        target_sources(${_filesystem_target} PRIVATE src/platform/filesystem_win32.cpp)
    endforeach()
    # Real system adapter tests/helpers also need its user-data layout closure.
    foreach(_system_target IN ITEMS
        HarkenSearchCubaseBridgeStub HarkenSearchNuendoBridgeStub TestConsentManager
        TestCueviFeedbackCompletionWorkflow TestPlatformEnvironmentVariable
        TestPlatformNamedPipeClient TestPlatformProcessQuery
        TestPlatformRegistryString TestProToolsBridgeExecutablePath)
        if(_system_target STREQUAL "TestCueviFeedbackCompletionWorkflow" AND NOT TARGET TestCueviFeedbackCompletionWorkflow)
            continue()
        endif()
        target_sources(${_system_target} PRIVATE
            src/platform/harken_user_data_layout.cpp
            src/platform/harken_legacy_data_migrator.cpp)
        target_include_directories(${_system_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(${_system_target} PRIVATE advapi32 crypt32 shell32 ole32 comdlg32 shlwapi)
    endforeach()
endif()

add_executable(TestInfoPageNavigation tests/test_info_page_navigation.cpp)
add_executable(TestInfoEditSession tests/test_info_edit_session.cpp)
target_include_directories(TestInfoEditSession PRIVATE src)
add_test(NAME InfoEditSession COMMAND TestInfoEditSession)
aura_set_test_labels(InfoEditSession ui info)
add_executable(TestInfoArtworkFooterLayout tests/test_info_artwork_footer_layout.cpp
    thirdparty/imgui/imgui.cpp thirdparty/imgui/imgui_draw.cpp
    thirdparty/imgui/imgui_tables.cpp thirdparty/imgui/imgui_widgets.cpp)
target_include_directories(TestInfoArtworkFooterLayout PRIVATE src thirdparty/imgui)
add_test(NAME InfoArtworkFooterLayout COMMAND TestInfoArtworkFooterLayout)
aura_set_test_labels(InfoArtworkFooterLayout ui info)
target_include_directories(TestInfoPageNavigation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME InfoPageNavigation COMMAND TestInfoPageNavigation)
aura_set_test_labels(InfoPageNavigation ui info)

foreach(_standalone_localized_target IN ITEMS
    TestMetadataWriterImageLoad TestShortcutPresets TestTransferEmbed TestConsentManager)
    target_sources(${_standalone_localized_target} PRIVATE src/Localization.cpp)
    add_dependencies(${_standalone_localized_target} ArisLocalizationGenerated)
endforeach()
