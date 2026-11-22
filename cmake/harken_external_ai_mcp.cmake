add_library(HarkenExternalAiLibrary STATIC EXCLUDE_FROM_ALL
    src/ExternalAiLibrary.cpp
    src/ExternalAiAuthorization.cpp
    src/ExternalAiAuthorizationStore.cpp
    ${_similarity_store_sources}
    ${AURO_SEARCH_TEXT_GB2312_SOURCE})
set_target_properties(HarkenExternalAiLibrary PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/internal-archive"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/internal-archive/Debug"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/internal-archive/Release"
)
target_include_directories(HarkenExternalAiLibrary PUBLIC src thirdparty PRIVATE "${SQLITE_DIR}")
target_link_libraries(HarkenExternalAiLibrary PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(HarkenExternalAiLibrary PRIVATE bcrypt)
endif()
if(MSVC)
    target_compile_options(HarkenExternalAiLibrary PRIVATE /W3 /utf-8)
endif()

add_executable(TestExternalAiLibrary EXCLUDE_FROM_ALL tests/test_external_ai_library.cpp)
target_include_directories(TestExternalAiLibrary PRIVATE "${SQLITE_DIR}")
target_link_libraries(TestExternalAiLibrary PRIVATE HarkenExternalAiLibrary)
if(MSVC)
    target_compile_options(TestExternalAiLibrary PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestExternalAiLibrary COMMAND TestExternalAiLibrary "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(TestExternalAiLibrary PROPERTIES TIMEOUT 30)
aura_set_test_labels(TestExternalAiLibrary core)

add_executable(HarkenExternalAiMcp EXCLUDE_FROM_ALL src/ExternalAiMcp.cpp)
target_link_libraries(HarkenExternalAiMcp PRIVATE HarkenExternalAiLibrary)
if(MSVC)
    target_compile_options(HarkenExternalAiMcp PRIVATE /W3 /utf-8)
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME TestExternalAiMcp
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_external_ai_mcp.py"
            "$<TARGET_FILE:HarkenExternalAiMcp>")
    set_tests_properties(TestExternalAiMcp PROPERTIES TIMEOUT 120 ENVIRONMENT
        "TEMP=${CMAKE_CURRENT_BINARY_DIR};TMP=${CMAKE_CURRENT_BINARY_DIR};TMPDIR=${CMAKE_CURRENT_BINARY_DIR};PYTHONDONTWRITEBYTECODE=1")
    aura_set_test_labels(TestExternalAiMcp core)
endif()

add_executable(TestExternalAiAuthorization EXCLUDE_FROM_ALL
    tests/test_external_ai_authorization.cpp
    src/ExternalAiAuthorization.cpp
    src/ExternalAiAuthorizationStore.cpp
    src/platform/filesystem_std.cpp)
target_include_directories(TestExternalAiAuthorization PRIVATE src thirdparty)
if(WIN32)
    target_sources(TestExternalAiAuthorization PRIVATE
        src/platform/filesystem_win32.cpp src/platform/text_encoding_win32.cpp
        src/platform/text_encoding_ascii.cpp)
    target_link_libraries(TestExternalAiAuthorization PRIVATE shlwapi)
endif()
if(MSVC)
    target_compile_options(TestExternalAiAuthorization PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestExternalAiAuthorization COMMAND TestExternalAiAuthorization "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(TestExternalAiAuthorization PROPERTIES TIMEOUT 30)
aura_set_test_labels(TestExternalAiAuthorization core)

# One-click client links (Cindy / Codex) own their own config-file editor, so the
# test target links only that translation unit and the header-only JSON library.
add_executable(TestExternalAiClientLink EXCLUDE_FROM_ALL
    tests/test_external_ai_client_link.cpp
    src/ExternalAiClientLink.cpp)
target_include_directories(TestExternalAiClientLink PRIVATE src thirdparty)
if(MSVC)
    target_compile_options(TestExternalAiClientLink PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestExternalAiClientLink COMMAND TestExternalAiClientLink "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(TestExternalAiClientLink PROPERTIES TIMEOUT 30)
aura_set_test_labels(TestExternalAiClientLink core)

# The connector ships beside HarkenSearch.exe, so one-click links can point the
# client at a path that already exists on the user's machine.
if(WIN32 AND TARGET HarkenSearch AND TARGET HarkenExternalAiMcp)
    add_dependencies(HarkenSearch HarkenExternalAiMcp)
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:HarkenExternalAiMcp>"
            "$<TARGET_FILE_DIR:HarkenSearch>/HarkenExternalAiMcp.exe"
        COMMENT "Copying HarkenExternalAiMcp.exe beside HarkenSearch.exe..."
        VERBATIM)
endif()

# Harken-side rules for one confirmed Playlist write. The database adapter and the
# pipe are separate; this target pins the decision logic on its own.
add_executable(TestExternalAiPlaylistBridge EXCLUDE_FROM_ALL
    tests/test_external_ai_playlist_bridge.cpp
    src/ExternalAiPlaylistBridge.cpp
    src/ExternalAiAuthorization.cpp)
target_include_directories(TestExternalAiPlaylistBridge PRIVATE src thirdparty)
if(MSVC)
    target_compile_options(TestExternalAiPlaylistBridge PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestExternalAiPlaylistBridge COMMAND TestExternalAiPlaylistBridge "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(TestExternalAiPlaylistBridge PROPERTIES TIMEOUT 30)
aura_set_test_labels(TestExternalAiPlaylistBridge core)

add_executable(TestExternalAiBridgeServer EXCLUDE_FROM_ALL
    tests/test_external_ai_bridge_server.cpp
    src/ExternalAiBridgeServer.cpp)
target_include_directories(TestExternalAiBridgeServer PRIVATE src)
target_compile_features(TestExternalAiBridgeServer PRIVATE cxx_std_17)
target_link_libraries(TestExternalAiBridgeServer PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(TestExternalAiBridgeServer PRIVATE /W3 /utf-8)
endif()
add_test(NAME TestExternalAiBridgeServer COMMAND TestExternalAiBridgeServer)
set_tests_properties(TestExternalAiBridgeServer PROPERTIES TIMEOUT 30)
aura_set_test_labels(TestExternalAiBridgeServer core)
