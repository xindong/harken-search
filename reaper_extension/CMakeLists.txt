# reaper_harken.dll — REAPER Extension for Aris "Send to DAW"
# Build: cmake --build build_dbg --config Debug --target reaper_harken

cmake_minimum_required(VERSION 3.16)

if(APPLE)
    # Host-side candidate; do not bundle or install until a real REAPER
    # session and the product installation flow have been verified.
    add_library(reaper_harken MODULE
        reaper_harken_macos.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/reaper_bridge_endpoint_macos.cpp
    )
    target_compile_features(reaper_harken PRIVATE cxx_std_17)
    target_include_directories(reaper_harken PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(reaper_harken PRIVATE Threads::Threads)
    set_target_properties(reaper_harken PROPERTIES
        PREFIX ""
        SUFFIX ".dylib"
        OUTPUT_NAME "reaper_harken"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reaper-plugin-qa"
    )
    return()
endif()

add_library(reaper_harken SHARED
    reaper_harken.cpp
    reaper_harken.rc
)

target_compile_features(reaper_harken PRIVATE cxx_std_17)
target_include_directories(reaper_harken PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Windows-only, link kernel32 for Named Pipes + process APIs
target_link_libraries(reaper_harken PRIVATE kernel32)

# Output name must be reaper_*.dll for REAPER to auto-load
set_target_properties(reaper_harken PROPERTIES
    OUTPUT_NAME "reaper_harken"
    PREFIX ""                     # No "lib" prefix
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)

# MSVC: /utf-8, /EHsc, no incremental for DLL stability
if(MSVC)
    target_compile_options(reaper_harken PRIVATE /utf-8 /EHsc /W3)
endif()
