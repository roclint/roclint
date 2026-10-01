cmake_minimum_required(VERSION 3.25)
project(Cadroc VERSION 0.1.0 LANGUAGES CXX)

# Enforce C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------------------------------------------------------
# 1. Define the Core Static Library Target (cadroc_core)
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
# 2. Define the Test Runner & CTest Registration
# -----------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
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
endif()