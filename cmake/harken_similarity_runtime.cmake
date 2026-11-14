set(HARKEN_ONNXRUNTIME_ROOT "" CACHE PATH "Pinned ONNX Runtime C++ SDK")
set(HARKEN_HNSWLIB_ROOT "" CACHE PATH "Pinned hnswlib header checkout")
set(HARKEN_SIMILARITY_MODEL_DIR "" CACHE PATH "Verified exported CLAP audio resources to bundle")
set(_harken_onnx_metadata_args)
if(APPLE AND HARKEN_ONNXRUNTIME_ROOT)
    file(READ "${HARKEN_ONNXRUNTIME_ROOT}/VERSION_NUMBER" _harken_onnx_version)
    string(STRIP "${_harken_onnx_version}" _harken_onnx_version)
    list(APPEND _harken_onnx_metadata_args --onnx-version "${_harken_onnx_version}")
endif()
if(Python3_Interpreter_FOUND)
    add_custom_command(TARGET HarkenSearch POST_BUILD
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/stage_similarity_metadata.py"
            --source "${CMAKE_SOURCE_DIR}/THIRD_PARTY_COMPONENTS.json"
            --output "$<TARGET_FILE_DIR:HarkenSearch>"
            --onnx $<BOOL:${HARKEN_ONNXRUNTIME_ROOT}>
            --hnsw $<BOOL:${HARKEN_HNSWLIB_ROOT}>
            --model $<BOOL:${HARKEN_SIMILARITY_MODEL_DIR}>
            ${_harken_onnx_metadata_args}
        COMMENT "Recording the configured similarity dependencies in release metadata"
        VERBATIM)
endif()
target_sources(HarkenSearch PRIVATE
    src/AcousticSimilarity.cpp src/AcousticSimilarityFile.cpp
    src/AudioEmbeddingService.cpp src/SimilarityAnalysisStore.cpp
    src/SimilarityIndexer.cpp src/SimilarityQueryV2.cpp)
if(HARKEN_ONNXRUNTIME_ROOT)
    target_compile_definitions(HarkenSearch PRIVATE HARKEN_WITH_ONNX=1)
    target_include_directories(HarkenSearch SYSTEM PRIVATE ${HARKEN_ONNXRUNTIME_ROOT}/include)
    find_library(HARKEN_ONNX_LIBRARY NAMES onnxruntime PATHS ${HARKEN_ONNXRUNTIME_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(HarkenSearch PRIVATE ${HARKEN_ONNX_LIBRARY})
    if(WIN32)
        add_custom_command(TARGET HarkenSearch POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${HARKEN_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll $<TARGET_FILE_DIR:HarkenSearch>)
    elseif(APPLE)
        set(_harken_onnx_macos_dylib
            "${HARKEN_ONNXRUNTIME_ROOT}/lib/libonnxruntime.${_harken_onnx_version}.dylib")
        if(NOT EXISTS "${_harken_onnx_macos_dylib}")
            message(FATAL_ERROR "ONNX Runtime macOS SDK is missing ${_harken_onnx_macos_dylib}")
        endif()
        set_property(TARGET HarkenSearch APPEND PROPERTY BUILD_RPATH "@executable_path")
        set_property(TARGET HarkenSearch APPEND PROPERTY INSTALL_RPATH "@executable_path")
        add_custom_command(TARGET HarkenSearch POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_harken_onnx_macos_dylib}" "$<TARGET_FILE_DIR:HarkenSearch>/libonnxruntime.${_harken_onnx_version}.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HARKEN_ONNXRUNTIME_ROOT}/LICENSE" "$<TARGET_FILE_DIR:HarkenSearch>/LICENSES/ONNX-Runtime-MIT.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HARKEN_ONNXRUNTIME_ROOT}/ThirdPartyNotices.txt" "$<TARGET_FILE_DIR:HarkenSearch>/LICENSES/ONNX-Runtime-ThirdPartyNotices.txt"
            COMMENT "Bundling pinned macOS ONNX Runtime beside HarkenSearch")
    endif()
endif()
if(HARKEN_HNSWLIB_ROOT)
    target_include_directories(HarkenSearch SYSTEM PRIVATE ${HARKEN_HNSWLIB_ROOT})
    target_compile_definitions(HarkenSearch PRIVATE HARKEN_WITH_HNSW=1)
endif()
if(HARKEN_SIMILARITY_MODEL_DIR)
    if(NOT HARKEN_ONNXRUNTIME_ROOT)
        message(FATAL_ERROR "A bundled similarity model requires the ONNX Runtime SDK")
    endif()
    foreach(_resource IN ITEMS clap-audio.onnx clap-mel-filters.f64 clap-window.f64)
        if(NOT EXISTS "${HARKEN_SIMILARITY_MODEL_DIR}/${_resource}")
            message(FATAL_ERROR "Missing CLAP resource: ${_resource}")
        endif()
        add_custom_command(TARGET HarkenSearch POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:HarkenSearch>/models/clap
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HARKEN_SIMILARITY_MODEL_DIR}/${_resource}" $<TARGET_FILE_DIR:HarkenSearch>/models/clap)
    endforeach()
endif()
