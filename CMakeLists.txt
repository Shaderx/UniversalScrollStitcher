cmake_minimum_required(VERSION 3.24)

project(UniversalScrollStitcher VERSION 0.1.0 LANGUAGES CXX)

option(UNIVERSAL_STITCHER_ENABLE_WGC "Enable Windows Graphics Capture when C++/WinRT is available" ON)
option(UNIVERSAL_STITCHER_BUILD_TESTS "Build the portable stitching-core tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

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
    add_executable(UniversalScrollStitcher WIN32
        src/main.cpp
        src/capture/WindowCapture.cpp
        src/capture/GraphicsCaptureSource.cpp
        src/export/ImageExporter.cpp
    )
    target_include_directories(UniversalScrollStitcher PRIVATE include)
    target_link_libraries(UniversalScrollStitcher PRIVATE stitch_core user32 gdi32 shcore dwmapi windowscodecs ole32 comdlg32)
    target_compile_definitions(UniversalScrollStitcher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

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
else()
    message(STATUS "The GUI capture target is Windows-only; building stitch_core and tests.")
endif()

include(CTest)
if (BUILD_TESTING AND UNIVERSAL_STITCHER_BUILD_TESTS)
    add_executable(stitch_core_tests tests/core_tests.cpp)
    target_link_libraries(stitch_core_tests PRIVATE stitch_core)
    target_include_directories(stitch_core_tests PRIVATE include)
    add_test(NAME stitch_core_tests COMMAND stitch_core_tests)
endif()
