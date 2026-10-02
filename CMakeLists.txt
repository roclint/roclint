cmake_minimum_required(VERSION 3.25)
project(Cadroc VERSION 0.1.0 LANGUAGES CXX)

# Enforce C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------------------------------------------------------
# 1. Define the Core Static Library Target (cadroc_core) - A00
# -----------------------------------------------------------------------------
add_library(cadroc_core STATIC
    src/core/version.cpp
)

# Inject version variables directly into the compiler from CMake
target_compile_definitions(cadroc_core PRIVATE
    CADROC_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CADROC_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CADROC_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    CADROC_VERSION_STRING="${PROJECT_VERSION}"
)

# Tell the compiler where public headers live
target_include_directories(cadroc_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(cadroc_core PUBLIC cxx_std_20)

# -----------------------------------------------------------------------------
# 2. Desktop Resident Host Target (cadroc_host) - B00
# -----------------------------------------------------------------------------
option(BUILD_DESKTOP "Build the desktop resident host and shell" ON)

if(BUILD_DESKTOP)
    find_package(Qt6 6.2 COMPONENTS Core Gui Quick Qml Widgets QUIET)

    if(Qt6_FOUND)
        message(STATUS "Qt6 found (${Qt6_VERSION}). Building Cadroc desktop host.")

        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)

        # Host Controller library (reusable for testing)
        add_library(cadroc_host_lib STATIC
            app/host/HostController.hpp
            app/host/HostController.cpp
            app/host/TrayManager.hpp
            app/host/TrayManager.cpp
        )

        target_include_directories(cadroc_host_lib PUBLIC
            app/host
        )

        target_link_libraries(cadroc_host_lib PUBLIC
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
        )

        target_compile_features(cadroc_host_lib PUBLIC cxx_std_20)

        # Desktop executable target
        add_executable(cadroc_host
            app/host/main.cpp
            app/qml/shell/qml.qrc
        )

        target_link_libraries(cadroc_host PRIVATE
            cadroc_host_lib
            cadroc_core
            Qt6::Core
            Qt6::Gui
            Qt6::Quick
            Qt6::Qml
            Qt6::Widgets
        )

        target_compile_features(cadroc_host PRIVATE cxx_std_20)

        if(WIN32)
            set_target_properties(cadroc_host PROPERTIES
                WIN32_EXECUTABLE FALSE # Kept console during development/testing for diagnostics
            )
        endif()
    else()
        message(STATUS "Qt6 not found. Skipping cadroc_host desktop target. (Install Qt6 and set CMAKE_PREFIX_PATH to enable)")
    endif()
endif()

# -----------------------------------------------------------------------------
# 3. Define Tests & CTest Registration
# -----------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    # Core smoke test (A00)
    add_executable(cadroc_core_tests
        tests/core/smokeTest.cpp
    )

    # Link our test against the cadroc_core static library
    target_link_libraries(cadroc_core_tests PRIVATE
        cadroc_core
    )

    target_compile_features(cadroc_core_tests PRIVATE cxx_std_20)

    # Register with CTest
    add_test(NAME cadroc_core_smoke COMMAND cadroc_core_tests)

    # ViewModel / Host lifecycle test (B00)
    if(BUILD_DESKTOP AND Qt6_FOUND)
        add_executable(cadroc_viewmodel_tests
            tests/viewmodel/test_host_lifecycle.cpp
        )

        target_link_libraries(cadroc_viewmodel_tests PRIVATE
            cadroc_host_lib
            Qt6::Core
        )

        target_compile_features(cadroc_viewmodel_tests PRIVATE cxx_std_20)
        add_test(NAME cadroc_host_lifecycle COMMAND cadroc_viewmodel_tests)
    endif()
endif()