function(harken_bootstrap_progress_ui target)
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/bootstrap-ui/BootstrapProgressPage.generated.h")
    if(NOT TARGET harken_bootstrap_progress_page)
        add_custom_command(OUTPUT "${_generated}"
            COMMAND "${Python3_EXECUTABLE}" "${_root}/tools/generate_bootstrap_progress_page.py"
                --root "${_root}" --output "${_generated}"
            DEPENDS "${_root}/tools/generate_bootstrap_progress_page.py"
                "${_root}/src/bootstrap/BootstrapProgressPage.html"
                "${_root}/assets/icons/source/harken-app-icons/Search/Search transparent 64.png"
                "${_root}/assets/logos/brand/harken-search-menu-wordmark-black.png"
                "${_root}/assets/logos/brand/harken-search-menu-wordmark-white.png"
            VERBATIM)
        add_custom_target(harken_bootstrap_progress_page DEPENDS "${_generated}")
    endif()
    add_dependencies(${target} harken_bootstrap_progress_page)
    target_sources(${target} PRIVATE "${_root}/src/bootstrap/BootstrapProgressView.cpp"
        "${_root}/src/bootstrap/WindowsBootstrapWebView.cpp")
    target_include_directories(${target} PRIVATE "${_root}/thirdparty/webview2/build/native/include"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/bootstrap-ui")
    target_link_libraries(${target} PRIVATE
        "${_root}/thirdparty/webview2/build/native/x64/WebView2LoaderStatic.lib"
        version shlwapi)
endfunction()
