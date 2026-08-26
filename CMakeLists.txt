cmake_minimum_required(VERSION 3.24)

project(UniversalScrollStitcher VERSION 0.1.0 LANGUAGES CXX)

option(UNIVERSAL_STITCHER_ENABLE_WGC "Enable Windows Graphics Capture when C++/WinRT is available" ON)
option(UNIVERSAL_STITCHER_BUILD_TESTS "Build the portable stitching-core tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

if (WIN32)
    # OpenCV is dynamically linked by the vcpkg package. Ship the complete
    # runtime set beside every executable so double-clicking the app (or
    # running a test from its output directory) does not depend on PATH,
    # Visual Studio, or a developer prompt.
    get_filename_component(UNIVERSAL_STITCHER_OPENCV_ROOT
        "${OpenCV_DIR}/../.." ABSOLUTE)
    file(GLOB UNIVERSAL_STITCHER_OPENCV_RUNTIME_DLLS
        "${UNIVERSAL_STITCHER_OPENCV_ROOT}/bin/*.dll")
    if (NOT UNIVERSAL_STITCHER_OPENCV_RUNTIME_DLLS)
        message(WARNING "No OpenCV runtime DLLs found under ${UNIVERSAL_STITCHER_OPENCV_ROOT}/bin")
    endif()

    function(copy_opencv_runtime target)
        foreach(runtime_dll IN LISTS UNIVERSAL_STITCHER_OPENCV_RUNTIME_DLLS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${runtime_dll}" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endforeach()
    endfunction()
endif()

add_library(stitch_core
    src/stitch/ScrollbarDetector.cpp
    src/stitch/VerticalShiftEstimator.cpp
    src/stitch/SeamFinder.cpp
    src/stitch/StripStore.cpp
    src/stitch/StitchSession.cpp
)
target_include_directories(stitch_core PUBLIC include)
target_include_directories(stitch_core SYSTEM PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(stitch_core PUBLIC ${OpenCV_LIBS})
target_compile_features(stitch_core PUBLIC cxx_std_20)
if (MSVC)
    target_compile_options(stitch_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(stitch_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (WIN32)
    add_library(stitch_export
        src/export/ImageExporter.cpp
    )
    target_include_directories(stitch_export PUBLIC include)
    target_link_libraries(stitch_export PUBLIC stitch_core windowscodecs ole32)
    target_compile_definitions(stitch_export PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

    add_executable(UniversalScrollStitcher WIN32
        src/main.cpp
        src/capture/WindowCapture.cpp
        src/capture/GraphicsCaptureSource.cpp
    )
    target_include_directories(UniversalScrollStitcher PRIVATE include)
    target_link_libraries(UniversalScrollStitcher PRIVATE stitch_core stitch_export user32 gdi32 shcore dwmapi comdlg32 shell32)
    target_compile_definitions(UniversalScrollStitcher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    copy_opencv_runtime(UniversalScrollStitcher)

    if (UNIVERSAL_STITCHER_ENABLE_WGC)
        find_path(CPPWINRT_INCLUDE_DIR winrt/base.h
            HINTS
                "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
                "$ENV{VCPKG_ROOT}/installed/$ENV{VCPKG_DEFAULT_TRIPLET}/include"
        )
        if (CPPWINRT_INCLUDE_DIR)
            message(STATUS "C++/WinRT headers found: ${CPPWINRT_INCLUDE_DIR}")
            target_include_directories(UniversalScrollStitcher PRIVATE ${CPPWINRT_INCLUDE_DIR})
            target_compile_definitions(UniversalScrollStitcher PRIVATE UNIVERSAL_STITCHER_HAS_WGC=1)
            target_link_libraries(UniversalScrollStitcher PRIVATE d3d11 dxgi windowsapp)
        else()
            message(WARNING "C++/WinRT headers not found; using the visible-window GDI capture fallback.")
        endif()
    endif()

    install(TARGETS UniversalScrollStitcher RUNTIME DESTINATION .)
    if (UNIVERSAL_STITCHER_OPENCV_RUNTIME_DLLS)
        install(FILES ${UNIVERSAL_STITCHER_OPENCV_RUNTIME_DLLS} DESTINATION .)
    endif()
    install(FILES README.md DESTINATION .)
    foreach(dependency IN ITEMS cppwinrt libjpeg-turbo libpng opencv4 zlib)
        set(dependency_notice
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${dependency}/copyright")
        if (EXISTS "${dependency_notice}")
            install(FILES "${dependency_notice}"
                    DESTINATION licenses
                    RENAME "${dependency}.txt")
        endif()
    endforeach()
else()
    message(STATUS "The GUI capture target is Windows-only; building stitch_core and tests.")
endif()

include(CTest)
if (BUILD_TESTING AND UNIVERSAL_STITCHER_BUILD_TESTS)
    add_executable(stitch_core_tests tests/core_tests.cpp)
    target_link_libraries(stitch_core_tests PRIVATE stitch_core)
    target_include_directories(stitch_core_tests PRIVATE include)
    add_test(NAME stitch_core_tests COMMAND stitch_core_tests)
    if (WIN32)
        add_executable(stitch_export_tests tests/export_tests.cpp)
        target_link_libraries(stitch_export_tests PRIVATE stitch_export)
        target_include_directories(stitch_export_tests PRIVATE include)
        add_test(NAME stitch_export_tests COMMAND stitch_export_tests)
        copy_opencv_runtime(stitch_core_tests)
        copy_opencv_runtime(stitch_export_tests)
        set_tests_properties(stitch_core_tests stitch_export_tests
            PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()
