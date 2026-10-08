cmake_minimum_required(VERSION 3.25)
project(Roclint VERSION 0.1.0 LANGUAGES CXX)

# Enforce C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# 1. Define the Core Static Library Target (roclint_core) - A00
# -----------------------------------------------------------------------------
add_library(roclint_core STATIC
    src/core/version.cpp
)

# Inject version variables directly into the compiler from CMake
target_compile_definitions(roclint_core PRIVATE
    ROCLINT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    ROCLINT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    ROCLINT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    ROCLINT_VERSION_STRING="${PROJECT_VERSION}"
)

# Tell the compiler where public headers live
target_include_directories(roclint_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(roclint_core PUBLIC cxx_std_20)

# -----------------------------------------------------------------------------
# 2. Desktop Resident Host Target (roclint_host) - B00
# -----------------------------------------------------------------------------
option(BUILD_DESKTOP "Build the desktop resident host and shell" ON)

if(BUILD_DESKTOP)
    if(APPLE AND NOT DEFINED CMAKE_PREFIX_PATH AND EXISTS "/opt/homebrew/opt/qt")
        list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt")
    endif()

    find_package(Qt6 6.2 COMPONENTS Core Gui Quick Qml Widgets QUIET)

    if(Qt6_FOUND)
        message(STATUS "Qt6 found (${Qt6_VERSION}). Building Roclint desktop host.")

        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)

        # Host Controller library (reusable for testing)
        add_library(roclint_host_lib STATIC
            app/host/HostController.hpp
            app/host/HostController.cpp
            app/host/TrayManager.hpp
            app/host/TrayManager.cpp
        )

        target_include_directories(roclint_host_lib PUBLIC
            app/host
        )

        target_link_libraries(roclint_host_lib PUBLIC
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
        )

        target_compile_features(roclint_host_lib PUBLIC cxx_std_20)

        # Desktop executable target
        add_executable(roclint_host
            app/host/main.cpp
            app/qml/shell/qml.qrc
        )

        target_link_libraries(roclint_host PRIVATE
            roclint_host_lib
            roclint_core
            Qt6::Core
            Qt6::Gui
            Qt6::Quick
            Qt6::Qml
            Qt6::Widgets
        )

        target_compile_features(roclint_host PRIVATE cxx_std_20)

        if(WIN32)
            set_target_properties(roclint_host PROPERTIES
                WIN32_EXECUTABLE FALSE # Kept console during development/testing for diagnostics
            )
        endif()
    else()
        message(STATUS "Qt6 not found. Skipping roclint_host desktop target. (Install Qt6 and set CMAKE_PREFIX_PATH to enable)")
    endif()
endif()

# -----------------------------------------------------------------------------
# 3. Define Tests & CTest Registration
# -----------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    # Core smoke test (A00)
    add_executable(roclint_core_tests
        tests/core/smokeTest.cpp
    )

    # Link our test against the roclint_core static library
    target_link_libraries(roclint_core_tests PRIVATE
        roclint_core
    )

    target_include_directories(roclint_core_tests PRIVATE tests)
    target_compile_features(roclint_core_tests PRIVATE cxx_std_20)

    # Register with CTest
    add_test(NAME roclint_core_smoke COMMAND roclint_core_tests)

    # ViewModel / Host lifecycle test (B00)
    if(BUILD_DESKTOP AND Qt6_FOUND)
        add_executable(roclint_viewmodel_tests
            tests/viewmodel/test_host_lifecycle.cpp
        )

        target_link_libraries(roclint_viewmodel_tests PRIVATE
            roclint_host_lib
            Qt6::Core
        )

        target_include_directories(roclint_viewmodel_tests PRIVATE tests)
        target_compile_features(roclint_viewmodel_tests PRIVATE cxx_std_20)
        add_test(NAME roclint_host_lifecycle COMMAND roclint_viewmodel_tests)
    endif()
endif()