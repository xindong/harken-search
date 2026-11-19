add_library(HarkenExternalAiLibrary STATIC EXCLUDE_FROM_ALL
    src/ExternalAiLibrary.cpp
    ${_similarity_store_sources})
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
