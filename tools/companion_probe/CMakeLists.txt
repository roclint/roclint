# Evaluation-only probe for the unmodified production HostController.
#
# This is a standalone project, not part of the Roclint product build. It links
# the already-built Release libraries read-only so the baseline can be captured
# without editing the repository CMakeLists or production sources.
cmake_minimum_required(VERSION 3.20)
project(roclint_companion_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

get_filename_component(ROCLINT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ROCLINT_BUILD "D:/Stardust/roclint/build/Release" CACHE PATH "Read-only Release build to link")

set(CMAKE_PREFIX_PATH "D:/Qt/6.8.2/msvc2022_64" CACHE STRING "Qt kit")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent)

add_executable(companion_probe companion_probe.cpp)
target_include_directories(companion_probe PRIVATE
    "${ROCLINT_SOURCE}/app/host"
    "${ROCLINT_SOURCE}/include"
    "D:/Stardust/roclint/build/_deps/roclint_sqlite-src"
    "D:/Stardust/roclint/build/_deps/roclint_tree_sitter-src/lib/include"
    "D:/Stardust/roclint/build/_deps/roclint_tree_sitter_cpp-src/include")
target_compile_definitions(companion_probe PRIVATE
    ROCLINT_ENABLE_TEST_MOCKS
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
target_compile_options(companion_probe PRIVATE /utf-8 /EHsc /bigobj)

target_link_libraries(companion_probe PRIVATE
    "${ROCLINT_BUILD}/roclint_host_lib_testing.lib"
    "${ROCLINT_BUILD}/roclint_storage_testing.lib"
    "${ROCLINT_BUILD}/roclint_core_testing.lib"
    "${ROCLINT_BUILD}/roclint_sqlite.lib"
    "${ROCLINT_BUILD}/roclint_tree_sitter.lib"
    Qt6::Widgets
    Qt6::Gui
    Qt6::Network
    Qt6::Concurrent
    Qt6::Core
    ws2_32 mpr userenv d3d11 dxgi dxguid d3d12 uiautomationcore
    ole32 oleaut32 shell32 uuid comdlg32 advapi32 gdi32 winspool psapi)
