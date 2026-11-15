cmake_minimum_required(VERSION 3.25)
project(Roclint VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/granite-models.json" ROCLINT_GRANITE_MANIFEST_JSON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/granite-models.json")

# -----------------------------------------------------------------------------
# Warnings are errors on every first-party target.
# -----------------------------------------------------------------------------
function(roclint_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /w14061 /WX /permissive- /utf-8
                                                 /external:anglebrackets /external:W0)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wswitch-enum -Werror -Wno-missing-field-initializers)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# 1. Core library (A00). Built twice: production, and a _testing variant that
#    compiles the ROCLINT_ENABLE_TEST_MOCKS seams. Release binaries never link
#    the _testing variant.
# -----------------------------------------------------------------------------
set(ROCLINT_CORE_SOURCES
    src/core/version.cpp
    src/core/Result.cpp
    src/core/Json.cpp
    src/core/crypto/Sha256.cpp
    src/core/prompt_state/SessionId.cpp
    src/core/prompt_state/RevisionId.cpp
    src/core/prompt_state/PromptRevision.cpp
    src/core/prompt_state/RevisionState.cpp
    src/core/prompt_state/TextNormalization.cpp
    src/core/scoped_constraint.cpp
    src/core/validation_result.cpp
    src/core/citation_evaluator.cpp
    src/core/constraint_reconciler.cpp
    src/analysis/SourceSpan.cpp
    src/analysis/NormalizedAnalysis.cpp
    src/analysis/AnalysisSnapshot.cpp
    src/analysis/PromptTriage.cpp
    src/analysis/MaterialityPolicy.cpp
    src/intent/InterventionStateMachine.cpp
    src/intent/ConfirmedIntent.cpp
    src/bridge/GraniteAdapter.cpp
    src/bridge/S1TriageAdapter.cpp
    src/bridge/GraniteRewriteAdapter.cpp
    src/bridge/GraniteGoldfishAdapter.cpp
    src/rewrite/RewriteProfile.cpp
    src/rewrite/RewriteProposal.cpp
    src/rewrite/RewriteValidator.cpp
    src/rewrite/CanonicalIntentExtractor.cpp
    src/rewrite/StructuredEntityExtractor.cpp
    src/rewrite/CreativeExpansion.cpp
    src/review/ApprovalRecord.cpp
)

function(roclint_add_core_library name enable_mocks)
    add_library(${name} STATIC ${ROCLINT_CORE_SOURCES})
    target_compile_definitions(${name} PRIVATE
        ROCLINT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        ROCLINT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        ROCLINT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        ROCLINT_VERSION_STRING="${PROJECT_VERSION}"
    )
    if(enable_mocks)
        target_compile_definitions(${name} PUBLIC ROCLINT_ENABLE_TEST_MOCKS=1)
    endif()
    target_include_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${name} PUBLIC cxx_std_20)
    roclint_apply_warnings(${name})
endfunction()

roclint_add_core_library(roclint_core OFF)

# -----------------------------------------------------------------------------
# 1b. Pinned native support: Tree-sitter, C++ grammar and SQLite FTS5
# -----------------------------------------------------------------------------
enable_language(C)
include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

set(ROCLINT_SQLITE_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/roclint_sqlite-src" CACHE PATH
    "Pinned SQLite 3.46.1 amalgamation source (fetched when absent)")
if(NOT EXISTS "${ROCLINT_SQLITE_SOURCE_DIR}/sqlite3.c")
    FetchContent_Declare(roclint_sqlite
        URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
        URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784)
    FetchContent_GetProperties(roclint_sqlite)
    if(NOT roclint_sqlite_POPULATED)
        FetchContent_Populate(roclint_sqlite)
    endif()
    set(ROCLINT_SQLITE_SOURCE_DIR "${roclint_sqlite_SOURCE_DIR}" CACHE PATH
        "Pinned SQLite 3.46.1 amalgamation source" FORCE)
endif()
add_library(roclint_sqlite STATIC "${ROCLINT_SQLITE_SOURCE_DIR}/sqlite3.c")
target_include_directories(roclint_sqlite PUBLIC "${ROCLINT_SQLITE_SOURCE_DIR}")
target_compile_definitions(roclint_sqlite PRIVATE SQLITE_ENABLE_FTS5=1 SQLITE_THREADSAFE=1)
if(MSVC)
    target_compile_options(roclint_sqlite PRIVATE /W3)
else()
    target_compile_options(roclint_sqlite PRIVATE -w)
endif()
add_library(SQLite3::SQLite3 ALIAS roclint_sqlite)

set(ROCLINT_TREE_SITTER_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/roclint_tree_sitter-src"
    CACHE PATH "Pinned Tree-sitter runtime source (fetched when absent)")
set(ROCLINT_TREE_SITTER_CPP_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/roclint_tree_sitter_cpp-src"
    CACHE PATH "Pinned Tree-sitter C++ grammar source (fetched when absent)")
if(NOT EXISTS "${ROCLINT_TREE_SITTER_SOURCE_DIR}/lib/src/parser.c")
    FetchContent_Declare(roclint_tree_sitter
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
        GIT_TAG da6fe9beb4f7f67beb75914ca8e0d48ae48d6406
        GIT_SHALLOW FALSE)
    FetchContent_GetProperties(roclint_tree_sitter)
    if(NOT roclint_tree_sitter_POPULATED)
        FetchContent_Populate(roclint_tree_sitter)
    endif()
    set(ROCLINT_TREE_SITTER_SOURCE_DIR "${roclint_tree_sitter_SOURCE_DIR}" CACHE PATH
        "Pinned Tree-sitter runtime source" FORCE)
endif()
if(NOT EXISTS "${ROCLINT_TREE_SITTER_CPP_SOURCE_DIR}/src/parser.c")
    FetchContent_Declare(roclint_tree_sitter_cpp
        GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
        GIT_TAG f41e1a044c8a84ea9fa8577fdd2eab92ec96de02
        GIT_SHALLOW FALSE)
    FetchContent_GetProperties(roclint_tree_sitter_cpp)
    if(NOT roclint_tree_sitter_cpp_POPULATED)
        FetchContent_Populate(roclint_tree_sitter_cpp)
    endif()
    set(ROCLINT_TREE_SITTER_CPP_SOURCE_DIR "${roclint_tree_sitter_cpp_SOURCE_DIR}" CACHE PATH
        "Pinned Tree-sitter C++ grammar source" FORCE)
endif()
file(GLOB ROCLINT_TREE_SITTER_RUNTIME CONFIGURE_DEPENDS
    "${ROCLINT_TREE_SITTER_SOURCE_DIR}/lib/src/*.c")
list(REMOVE_ITEM ROCLINT_TREE_SITTER_RUNTIME "${ROCLINT_TREE_SITTER_SOURCE_DIR}/lib/src/lib.c")
add_library(roclint_tree_sitter STATIC
    ${ROCLINT_TREE_SITTER_RUNTIME}
    "${ROCLINT_TREE_SITTER_CPP_SOURCE_DIR}/src/parser.c"
    "${ROCLINT_TREE_SITTER_CPP_SOURCE_DIR}/src/scanner.c")
target_include_directories(roclint_tree_sitter PUBLIC
    "${ROCLINT_TREE_SITTER_SOURCE_DIR}/lib/include"
    "${ROCLINT_TREE_SITTER_CPP_SOURCE_DIR}/bindings/c")
target_include_directories(roclint_tree_sitter PRIVATE
    "${ROCLINT_TREE_SITTER_SOURCE_DIR}/lib/src"
    "${ROCLINT_TREE_SITTER_CPP_SOURCE_DIR}/src")
if(MSVC)
    target_compile_options(roclint_tree_sitter PRIVATE /W3)
else()
    target_compile_options(roclint_tree_sitter PRIVATE -w)
endif()

find_package(Threads REQUIRED)

set(ROCLINT_STORAGE_SOURCES
    src/storage/Database.cpp
    src/storage/PayloadCodec.cpp
    src/storage/PromptRepository.cpp
    src/storage/AstSymbol.cpp
    src/storage/AstSymbolExtractor.cpp
    src/storage/CodebaseIndex.cpp
    src/rewrite/AstGroundingEngine.cpp
)

function(roclint_add_storage_library name core_target)
    add_library(${name} STATIC ${ROCLINT_STORAGE_SOURCES})
    target_link_libraries(${name} PUBLIC ${core_target} SQLite3::SQLite3 PRIVATE roclint_tree_sitter)
    target_include_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${name} PUBLIC cxx_std_20)
    roclint_apply_warnings(${name})
endfunction()

roclint_add_storage_library(roclint_storage roclint_core)

# Development-only synthetic evaluation adapter; no Phoenix dependency in the host.
add_executable(roclint_ast_evaluator tools/ast_grounding_evaluator.cpp)
target_link_libraries(roclint_ast_evaluator PRIVATE roclint_storage)
roclint_apply_warnings(roclint_ast_evaluator)

include(CTest)
if(BUILD_TESTING)
    roclint_add_core_library(roclint_core_testing ON)
    roclint_add_storage_library(roclint_storage_testing roclint_core_testing)
endif()

# -----------------------------------------------------------------------------
# 2. Desktop resident host (B00)
# -----------------------------------------------------------------------------
option(BUILD_DESKTOP "Build the desktop resident host and shell" ON)
option(ROCLINT_DEPLOY_QT "Stage a self-contained Windows host under build/<config>/bin" ON)
set(ROCLINT_HOST_OUTPUT_DIRECTORY "" CACHE PATH "Optional separate host output directory, for previews while another build is running")

if(BUILD_DESKTOP)
    if(APPLE AND NOT DEFINED CMAKE_PREFIX_PATH)
        foreach(_qt_search_path
            "/opt/homebrew/opt/qt"
            "/opt/homebrew/opt/qt@6"
            "/usr/local/opt/qt"
            "/usr/local/opt/qt@6"
        )
            if(EXISTS "${_qt_search_path}")
                list(APPEND CMAKE_PREFIX_PATH "${_qt_search_path}")
                break()
            endif()
        endforeach()
    endif()

    find_package(Qt6 6.2 COMPONENTS Core Gui Quick Qml Widgets Network Concurrent QUIET)

    if(Qt6_FOUND)
        message(STATUS "Qt6 found (${Qt6_VERSION}). Building Roclint desktop host.")

        if(WIN32 AND ROCLINT_DEPLOY_QT)
            get_target_property(_roclint_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
            if(NOT _roclint_qt_core_location)
                get_target_property(_roclint_qt_core_location Qt6::Core IMPORTED_LOCATION)
            endif()
            if(_roclint_qt_core_location)
                get_filename_component(_roclint_qt_bin "${_roclint_qt_core_location}" DIRECTORY)
                find_program(WINDEPLOYQT_EXECUTABLE NAMES windeployqt HINTS "${_roclint_qt_bin}" NO_DEFAULT_PATH)
            endif()
            if(NOT WINDEPLOYQT_EXECUTABLE)
                find_program(WINDEPLOYQT_EXECUTABLE NAMES windeployqt)
            endif()
            if(WINDEPLOYQT_EXECUTABLE)
                message(STATUS "Qt deployment enabled: ${WINDEPLOYQT_EXECUTABLE}")
            else()
                message(WARNING "windeployqt was not found; run start_roclint.bat to fall back to the Qt bin directory on PATH.")
            endif()
        elseif(APPLE AND ROCLINT_DEPLOY_QT)
            get_target_property(_roclint_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
            if(NOT _roclint_qt_core_location)
                get_target_property(_roclint_qt_core_location Qt6::Core IMPORTED_LOCATION)
            endif()
            if(_roclint_qt_core_location)
                get_filename_component(_roclint_qt_bin "${_roclint_qt_core_location}" DIRECTORY)
                find_program(MACDEPLOYQT_EXECUTABLE NAMES macdeployqt HINTS "${_roclint_qt_bin}" /opt/homebrew/opt/qt/bin /usr/local/opt/qt/bin NO_DEFAULT_PATH)
            endif()
            if(NOT MACDEPLOYQT_EXECUTABLE)
                find_program(MACDEPLOYQT_EXECUTABLE NAMES macdeployqt HINTS /opt/homebrew/opt/qt/bin /usr/local/opt/qt/bin)
            endif()
            if(MACDEPLOYQT_EXECUTABLE)
                message(STATUS "macOS Qt deployment enabled: ${MACDEPLOYQT_EXECUTABLE}")
            else()
                message(WARNING "macdeployqt was not found; install Qt Tools to enable standalone bundle deployment.")
            endif()
        endif()

        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)

        set(ROCLINT_HOST_SOURCES
            app/host/AsyncTask.hpp
            app/host/StorageService.hpp
            app/host/AsyncRepository.hpp
            app/host/HostController.hpp
            app/host/HostController.cpp
            app/host/HostControllerWorkspace.cpp
            app/host/CodebaseWatcher.hpp
            app/host/CodebaseWatcher.cpp
            app/host/HostControllerTriage.cpp
            app/host/HostControllerResidency.cpp
            app/host/GraniteResidencyPolicy.hpp
            app/host/OllamaClient.hpp
            app/host/OllamaClient.cpp
            app/host/RuntimeThread.hpp
            app/host/JuliaRuntimeWorker.hpp
            app/host/JuliaRuntimeWorker.cpp
            app/host/NativeGraniteRuntimeWorker.hpp
            app/host/NativeGraniteRuntimeWorker.cpp
            app/host/JuliaRuntime.hpp
            app/host/JuliaRuntime.cpp
            app/host/OwnedProcessJob.hpp
            app/host/WorkspaceInspection.hpp
            app/host/NativeGraniteRuntime.hpp
            app/host/NativeGraniteRuntime.cpp
            app/host/GraniteModelInstaller.hpp
            app/host/GraniteModelInstaller.cpp
            app/host/TrayManager.hpp
            app/host/TrayManager.cpp
            app/host/GlobalShortcutManager.hpp
            app/host/GlobalShortcutManager.cpp
            app/host/HostContext.hpp
            app/host/HostContext.cpp
            app/host/OutsideClickPolicy.hpp
            app/host/ScreenPlacement.hpp
            app/host/ScreenPlacement.cpp
            app/host/IAccessibilityBridge.hpp
            app/host/IAccessibilityBridge.cpp
            app/host/TerminalAdapter.hpp
            app/host/TerminalAdapter.cpp
        )

        function(roclint_add_host_library name core_target storage_target)
            add_library(${name} STATIC ${ROCLINT_HOST_SOURCES})
            target_include_directories(${name} PUBLIC app/host)
            target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
            target_link_libraries(${name} PUBLIC
                ${core_target}
                ${storage_target}
                Qt6::Core
                Qt6::Gui
                Qt6::Widgets
                Qt6::Network
                Qt6::Concurrent
            )
            if(APPLE)
                target_sources(${name} PRIVATE app/host/HostContext_mac.mm app/host/MacAccessibilityBridge.mm)
                set_source_files_properties(app/host/HostContext_mac.mm app/host/MacAccessibilityBridge.mm
                                            PROPERTIES COMPILE_OPTIONS "-x;objective-c++")
                target_link_libraries(${name} PUBLIC "-framework Carbon" "-framework AppKit" "-framework ApplicationServices")
            elseif(WIN32)
                target_sources(${name} PRIVATE app/host/WinAccessibilityBridge.cpp)
                target_link_libraries(${name} PUBLIC user32 psapi uiautomationcore ole32 oleaut32)
            endif()
            target_compile_features(${name} PUBLIC cxx_std_20)
            roclint_apply_warnings(${name})
        endfunction()

        roclint_add_host_library(roclint_host_lib roclint_core roclint_storage)
        if(BUILD_TESTING)
            roclint_add_host_library(roclint_host_lib_testing roclint_core_testing roclint_storage_testing)
        endif()

        # Default accelerator name before the native backend is resolved.
        # Overridden below once _roclint_backend is known (CPU / VULKAN / METAL).
        if(APPLE)
            set(_roclint_accelerator_name "METAL")
        elseif(WIN32)
            set(_roclint_accelerator_name "VULKAN")
        else()
            set(_roclint_accelerator_name "GPU")
        endif()

        add_executable(roclint_host
            app/host/main.cpp
            app/qml/shell/qml.qrc
        )
        if(WIN32)
            target_sources(roclint_host PRIVATE app/host/roclint.rc)
        elseif(APPLE)
            target_sources(roclint_host PRIVATE app/host/Roclint.icns)
            set_source_files_properties(app/host/Roclint.icns PROPERTIES
                MACOSX_PACKAGE_LOCATION "Resources")
        endif()
        if(ROCLINT_HOST_OUTPUT_DIRECTORY)
            set_target_properties(roclint_host PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${ROCLINT_HOST_OUTPUT_DIRECTORY}"
                PDB_OUTPUT_DIRECTORY "${ROCLINT_HOST_OUTPUT_DIRECTORY}")
        endif()
        target_link_libraries(roclint_host PRIVATE
            roclint_host_lib
            Qt6::Core
            Qt6::Gui
            Qt6::Quick
            Qt6::Qml
            Qt6::Widgets
            Qt6::Network
        )
        target_compile_features(roclint_host PRIVATE cxx_std_20)
        roclint_apply_warnings(roclint_host)

        add_executable(roclint_granite_contracts tools/granite_contracts.cpp)
        target_link_libraries(roclint_granite_contracts PRIVATE roclint_host_lib Qt6::Core)
        roclint_apply_warnings(roclint_granite_contracts)

        add_executable(roclint_report_verification_probe tools/report_verification_probe.cpp)
        set_target_properties(roclint_report_verification_probe PROPERTIES OUTPUT_NAME "report-verification-probe")
        target_link_libraries(roclint_report_verification_probe PRIVATE roclint_core)
        roclint_apply_warnings(roclint_report_verification_probe)

        add_executable(roclint_ast_grounding_benchmark tools/ast_grounding_benchmark.cpp)
        set_target_properties(roclint_ast_grounding_benchmark PROPERTIES OUTPUT_NAME "ast-grounding-benchmark")
        target_link_libraries(roclint_ast_grounding_benchmark PRIVATE roclint_storage)
        roclint_apply_warnings(roclint_ast_grounding_benchmark)

        option(ROCLINT_BUILD_NATIVE_GRANITE "Build the pinned native Granite helper" ON)
        set(ROCLINT_GRANITE_BACKEND "AUTO" CACHE STRING "Native backend: AUTO, CPU, VULKAN or METAL")
        set_property(CACHE ROCLINT_GRANITE_BACKEND PROPERTY STRINGS AUTO CPU VULKAN METAL)
        set(ROCLINT_LLAMA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/llama.cpp" CACHE PATH "Pinned clean llama.cpp checkout")
        if(ROCLINT_BUILD_NATIVE_GRANITE)
            string(JSON _roclint_llama_revision GET "${ROCLINT_GRANITE_MANIFEST_JSON}" runtime_revision)
            execute_process(COMMAND git -C "${ROCLINT_LLAMA_SOURCE_DIR}" rev-parse HEAD
                OUTPUT_VARIABLE _roclint_actual_llama OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _roclint_llama_git_result)
            execute_process(COMMAND git -C "${ROCLINT_LLAMA_SOURCE_DIR}" status --porcelain
                OUTPUT_VARIABLE _roclint_dirty_llama OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT _roclint_llama_git_result EQUAL 0 OR NOT _roclint_actual_llama STREQUAL _roclint_llama_revision OR _roclint_dirty_llama)
                message(FATAL_ERROR "Native Granite requires the pinned clean llama.cpp source. Run python tools/setup_granite.py --runtime-only, or explicitly set ROCLINT_BUILD_NATIVE_GRANITE=OFF for an Ollama-only development build.")
            endif()
            set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build native runtime dependencies statically" FORCE)
            set(LLAMA_BUILD_COMMON ON CACHE BOOL "Native chat templates and JSON grammar" FORCE)
            foreach(_llama_option LLAMA_BUILD_TESTS LLAMA_BUILD_TOOLS LLAMA_BUILD_EXAMPLES LLAMA_BUILD_SERVER
                    LLAMA_BUILD_APP LLAMA_BUILD_UI LLAMA_BUILD_MTMD LLAMA_OPENSSL LLAMA_SUBPROCESS LLAMA_LLGUIDANCE)
                set(${_llama_option} OFF CACHE BOOL "Unused in the owned native helper" FORCE)
            endforeach()
            set(GGML_NATIVE OFF CACHE BOOL "Do not specialize to the build machine" FORCE)
            set(_roclint_backend "${ROCLINT_GRANITE_BACKEND}")
            if(WIN32 AND NOT Vulkan_ROOT AND NOT DEFINED ENV{VULKAN_SDK})
                file(GLOB _roclint_vulkan_sdks LIST_DIRECTORIES TRUE "C:/VulkanSDK/*")
                list(SORT _roclint_vulkan_sdks COMPARE NATURAL ORDER DESCENDING)
                foreach(_sdk IN LISTS _roclint_vulkan_sdks)
                    if(EXISTS "${_sdk}/Bin/glslc.exe")
                        set(Vulkan_ROOT "${_sdk}" CACHE PATH "Vulkan SDK used by the native helper")
                        break()
                    endif()
                endforeach()
            endif()
            if(_roclint_backend STREQUAL "AUTO")
                if(APPLE)
                    set(_roclint_backend "METAL")
                else()
                    find_package(Vulkan QUIET COMPONENTS glslc)
                    if(Vulkan_FOUND)
                        set(_roclint_backend "VULKAN")
                    else()
                        set(_roclint_backend "CPU")
                    endif()
                endif()
            endif()
            if(NOT _roclint_backend MATCHES "^(CPU|VULKAN|METAL)$" OR (_roclint_backend STREQUAL "METAL" AND NOT APPLE))
                message(FATAL_ERROR "Unsupported native Granite backend: ${_roclint_backend}")
            endif()
            foreach(_field backend gpu_layers configuration_id)
                string(JSON _value GET "${ROCLINT_GRANITE_MANIFEST_JSON}" profiles ${_roclint_backend} ${_field})
                if(_field STREQUAL "gpu_layers")
                    string(JSON ROCLINT_GRANITE_MANIFEST_JSON SET "${ROCLINT_GRANITE_MANIFEST_JSON}" ${_field} "${_value}")
                else()
                    string(JSON ROCLINT_GRANITE_MANIFEST_JSON SET "${ROCLINT_GRANITE_MANIFEST_JSON}" ${_field} "\"${_value}\"")
                endif()
            endforeach()
            message(STATUS "Roclint native Granite backend: ${_roclint_backend}")
            # Update the accelerator name now that the real backend is known.
            set(_roclint_accelerator_name "${_roclint_backend}")
            set(GGML_CUDA OFF CACHE BOOL "CUDA is not part of these frozen profiles" FORCE)
            if(_roclint_backend STREQUAL "VULKAN")
                set(GGML_VULKAN ON CACHE BOOL "Selected native accelerator" FORCE)
            else()
                set(GGML_VULKAN OFF CACHE BOOL "Unselected native accelerator" FORCE)
            endif()
            if(_roclint_backend STREQUAL "METAL")
                set(GGML_METAL ON CACHE BOOL "Selected native accelerator" FORCE)
            else()
                set(GGML_METAL OFF CACHE BOOL "Unselected native accelerator" FORCE)
            endif()
            set(GGML_BACKEND_DL OFF CACHE BOOL "Statically link the selected native backend" FORCE)
            # llama.cpp's supported dependency build uses C++17. Keep that
            # directory's language mode independent of the C++20 application.
            set(_roclint_saved_cxx_standard ${CMAKE_CXX_STANDARD})
            set(CMAKE_CXX_STANDARD 17)
            add_subdirectory("${ROCLINT_LLAMA_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/native-llama" EXCLUDE_FROM_ALL)
            set(CMAKE_CXX_STANDARD ${_roclint_saved_cxx_standard})
            add_executable(roclint_granite_worker
                app/runtime/granite_worker.cpp
                app/runtime/GraniteEngine.cpp
                app/runtime/GraniteJsonGrammar.hpp
                app/runtime/GraniteEngine.hpp)
            target_include_directories(roclint_granite_worker PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
            target_include_directories(roclint_granite_worker SYSTEM PRIVATE
                "${ROCLINT_LLAMA_SOURCE_DIR}/common" "${ROCLINT_LLAMA_SOURCE_DIR}/include")
            target_link_libraries(roclint_granite_worker PRIVATE roclint_core Qt6::Core llama-common)
            roclint_apply_warnings(roclint_granite_worker)
            add_dependencies(roclint_host roclint_granite_worker)
        endif()

        add_custom_command(TARGET roclint_host POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:roclint_host>/runtime"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia_worker.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia-requirements.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia-requirements-directml.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/granite-models.json"
                "${CMAKE_CURRENT_BINARY_DIR}/generated/granite-runtime-config.json"
                "$<TARGET_FILE_DIR:roclint_host>/runtime"
            VERBATIM)
        # Refresh Python-only changes on ordinary incremental builds even when the host does not relink.
        if(UNIX)
            add_executable(roclint_process_guardian app/runtime/process_guardian.cpp)
            roclint_apply_warnings(roclint_process_guardian)
            add_dependencies(roclint_host roclint_process_guardian)
            add_custom_command(TARGET roclint_host POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:roclint_host>/runtime"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_process_guardian>" "$<TARGET_FILE_DIR:roclint_host>/runtime/")
            add_custom_target(roclint_guardian_runtime_files ALL
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:roclint_host>/runtime"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_process_guardian>" "$<TARGET_FILE_DIR:roclint_host>/runtime/"
                DEPENDS roclint_process_guardian)
        endif()
        add_executable(roclint_workspace_helper app/runtime/workspace_helper.cpp)
        target_link_libraries(roclint_workspace_helper PRIVATE Qt6::Core)
        add_dependencies(roclint_host roclint_workspace_helper)
        add_custom_command(TARGET roclint_host POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:roclint_host>/runtime"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_workspace_helper>" "$<TARGET_FILE_DIR:roclint_host>/runtime/")
        add_custom_target(roclint_workspace_runtime_files ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:roclint_host>/runtime"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_workspace_helper>" "$<TARGET_FILE_DIR:roclint_host>/runtime/"
            DEPENDS roclint_workspace_helper)

        add_custom_target(roclint_runtime_files ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:roclint_host>/runtime"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia_worker.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia-requirements.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia-requirements-directml.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/granite-models.json"
                "${CMAKE_CURRENT_BINARY_DIR}/generated/granite-runtime-config.json"
                "$<TARGET_FILE_DIR:roclint_host>/runtime"
            DEPENDS roclint_host
            VERBATIM)
        if(TARGET roclint_granite_worker)
            add_custom_command(TARGET roclint_host POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_granite_worker>" "$<TARGET_FILE_DIR:roclint_host>/runtime/"
                VERBATIM)
            add_custom_target(roclint_native_runtime_files ALL
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:roclint_host>/runtime"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_granite_worker>" "$<TARGET_FILE_DIR:roclint_host>/runtime/"
                DEPENDS roclint_host roclint_granite_worker VERBATIM)
        endif()

        if(WIN32 AND ROCLINT_DEPLOY_QT)
            set(_roclint_deploy_deps roclint_host roclint_runtime_files roclint_workspace_runtime_files)
            if(TARGET roclint_native_runtime_files)
                list(APPEND _roclint_deploy_deps roclint_native_runtime_files)
            endif()
            if(TARGET roclint_report_verification_probe)
                list(APPEND _roclint_deploy_deps roclint_report_verification_probe)
            endif()
            if(WINDEPLOYQT_EXECUTABLE)
                add_custom_target(roclint_deploy_qt ALL
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:roclint_host>/bin"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_host>"
                        "$<TARGET_FILE_DIR:roclint_host>/bin/roclint_host.exe"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:roclint_host>/runtime"
                        "$<TARGET_FILE_DIR:roclint_host>/bin/runtime"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_report_verification_probe>"
                        "$<TARGET_FILE_DIR:roclint_host>/bin/report-verification-probe.exe"
                    COMMAND ${CMAKE_COMMAND}
                        "-DWINDEPLOYQT=${WINDEPLOYQT_EXECUTABLE}"
                        "-DHOST_EXE=$<TARGET_FILE_DIR:roclint_host>/bin/roclint_host.exe"
                        "-DDEPLOY_DIR=$<TARGET_FILE_DIR:roclint_host>/bin"
                        "-DQML_DIR=${CMAKE_CURRENT_SOURCE_DIR}/app/qml"
                        "-DCONFIG_FLAG=$<IF:$<CONFIG:Debug>,--debug,--release>"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/deploy_qt.cmake"
                    DEPENDS ${_roclint_deploy_deps}
                    COMMENT "Staging self-contained Qt runtime under $<TARGET_FILE_DIR:roclint_host>/bin"
                    VERBATIM)
            else()
                add_custom_target(roclint_deploy_qt ALL
                    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:roclint_host>/bin"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_host>"
                        "$<TARGET_FILE_DIR:roclint_host>/bin/roclint_host.exe"
                    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:roclint_host>/runtime"
                        "$<TARGET_FILE_DIR:roclint_host>/bin/runtime"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_report_verification_probe>"
                        "$<TARGET_FILE_DIR:roclint_host>/bin/report-verification-probe.exe"
                    DEPENDS ${_roclint_deploy_deps}
                    COMMENT "Qt runtime deployment was requested but windeployqt was not found"
                    VERBATIM)
            endif()
        elseif(APPLE AND ROCLINT_DEPLOY_QT)
            set(_roclint_deploy_deps roclint_host roclint_runtime_files roclint_workspace_runtime_files)
            if(TARGET roclint_native_runtime_files)
                list(APPEND _roclint_deploy_deps roclint_native_runtime_files)
            endif()
            if(TARGET roclint_guardian_runtime_files)
                list(APPEND _roclint_deploy_deps roclint_guardian_runtime_files)
            endif()
            if(MACDEPLOYQT_EXECUTABLE)
                add_custom_target(roclint_deploy_qt ALL
                    COMMAND ${CMAKE_COMMAND}
                        "-DMACDEPLOYQT=${MACDEPLOYQT_EXECUTABLE}"
                        "-DBUNDLE_DIR=$<TARGET_BUNDLE_DIR:roclint_host>"
                        "-DQML_DIR=${CMAKE_CURRENT_SOURCE_DIR}/app/qml"
                        "-DEXECUTABLE_1=$<TARGET_FILE_DIR:roclint_host>/runtime/roclint_granite_worker"
                        "-DEXECUTABLE_2=$<TARGET_FILE_DIR:roclint_host>/runtime/roclint_workspace_helper"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/deploy_qt.cmake"
                    DEPENDS ${_roclint_deploy_deps}
                    COMMENT "Staging self-contained Qt runtime in $<TARGET_BUNDLE_DIR:roclint_host>"
                    VERBATIM)
            endif()
        endif()

        # Propagate the resolved accelerator name so HostController::acceleratorName()
        # always matches the backend that was actually compiled into the native runtime.
        # PUBLIC so that test executables linking roclint_host_lib/_testing also see the define.
        target_compile_definitions(roclint_host_lib PUBLIC
            ROCLINT_ACCELERATOR_NAME="${_roclint_accelerator_name}")
        if(BUILD_TESTING)
            target_compile_definitions(roclint_host_lib_testing PUBLIC
                ROCLINT_ACCELERATOR_NAME="${_roclint_accelerator_name}")
        endif()

        if(WIN32)
            set_target_properties(roclint_host PROPERTIES
                WIN32_EXECUTABLE TRUE # Everyday launch is a native GUI app; redirected diagnostics still work.
            )
        elseif(APPLE)
            set_target_properties(roclint_host PROPERTIES
                MACOSX_BUNDLE TRUE
                OUTPUT_NAME "Roclint"
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/app/host/Info.plist.in"
                MACOSX_BUNDLE_BUNDLE_NAME "Roclint"
                MACOSX_BUNDLE_GUI_IDENTIFIER "com.roclint.desktop"
                MACOSX_BUNDLE_ICON_FILE "Roclint.icns"
                MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            )
            add_custom_command(TARGET roclint_host POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "$<TARGET_FILE:roclint_host>"
                    "${CMAKE_CURRENT_BINARY_DIR}/roclint_host"
                COMMENT "Creating roclint_host symlink for CLI compatibility"
                VERBATIM)
        endif()
    else()
        message(STATUS "Qt6 not found. Skipping roclint_host desktop target. (Install Qt6 and set CMAKE_PREFIX_PATH to enable)")
    endif()
endif()

# -----------------------------------------------------------------------------
# 3. Tests (use _testing variants except explicit production-boundary checks)
# -----------------------------------------------------------------------------
configure_file(app/runtime/GraniteConfig.hpp.in generated/GraniteConfig.hpp @ONLY)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/granite-runtime-config.json" "${ROCLINT_GRANITE_MANIFEST_JSON}\n")
if(BUILD_TESTING)
    function(roclint_add_test test_name source link_target)
        add_executable(${test_name}_tests ${source})
        target_link_libraries(${test_name}_tests PRIVATE ${link_target} ${ARGN})
        target_include_directories(${test_name}_tests PRIVATE tests)
        target_compile_features(${test_name}_tests PRIVATE cxx_std_20)
        roclint_apply_warnings(${test_name}_tests)
        if(TARGET roclint_process_guardian)
            add_dependencies(${test_name}_tests roclint_process_guardian)
            add_custom_command(TARGET ${test_name}_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${test_name}_tests>/runtime"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:roclint_process_guardian>" "$<TARGET_FILE_DIR:${test_name}_tests>/runtime/")
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name}_tests)
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
        if(TARGET Qt6::Core)
            # Windows resolves Qt DLLs through PATH; CTest does not inherit a
            # developer shell's Qt environment, so prepend the kit's bin directory.
            set_tests_properties(${test_name} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
        if(TARGET Qt6::QOffscreenIntegrationPlugin)
            # windeployqt deploys qwindows, while GUI tests explicitly use offscreen.
            set_tests_properties(${test_name} PROPERTIES ENVIRONMENT
                "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>")
        endif()
    endfunction()

    roclint_add_test(roclint_core_smoke            tests/core/smokeTest.cpp                          roclint_core_testing)
    # Retain the original upstream build target alongside the clearer test executable name.
    add_custom_target(roclint_core_tests DEPENDS roclint_core_smoke_tests)
    roclint_add_test(roclint_core_primitives       tests/core/testCorePrimitives.cpp                 roclint_core_testing)
    roclint_add_test(roclint_scoped_constraint    tests/core/test_scoped_constraint.cpp             roclint_core_testing)
    roclint_add_test(roclint_citation_evaluator   tests/core/test_citation_evaluator.cpp            roclint_core_testing)
    roclint_add_test(roclint_constraint_reconciler tests/core/test_constraint_reconciler.cpp         roclint_core_testing)
    roclint_add_test(roclint_adversarial_negation tests/core/test_adversarial_m1.cpp                  roclint_core_testing)
    roclint_add_test(roclint_adversarial_rewrite tests/core/test_adversarial_comprehensive.cpp         roclint_core_testing)
    roclint_add_test(roclint_prompt_revision       tests/core/prompt_state/testPromptRevision.cpp    roclint_core_testing)
    roclint_add_test(roclint_revision_state        tests/core/prompt_state/testRevisionState.cpp     roclint_core_testing)
    roclint_add_test(roclint_storage               tests/storage/test_database.cpp                   roclint_storage_testing)
    roclint_add_test(roclint_ast_symbol_extractor  tests/storage/testAstSymbolExtractor.cpp           roclint_storage_testing)
    roclint_add_test(roclint_ast_grounding         tests/storage/testAstGroundingEngine.cpp           roclint_storage_testing)
    target_compile_definitions(roclint_ast_grounding_tests PRIVATE
        ROCLINT_AST_GROUNDING_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/tests/storage/ast_grounding_corpus.json")
    roclint_add_test(roclint_codebase_index_freshness tests/storage/testCodebaseIndexFreshness.cpp      roclint_storage_testing)
    target_include_directories(roclint_codebase_index_freshness_tests PRIVATE app/host)
    if(TARGET llama-common AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models/granite-4.2-3b/granite-4.2-3b-Q8_0.gguf")
        roclint_add_test(roclint_ast_token_budget tests/storage/testAstTokenBudget.cpp roclint_storage_testing llama-common)
        target_include_directories(roclint_ast_token_budget_tests SYSTEM PRIVATE
            "${ROCLINT_LLAMA_SOURCE_DIR}/include")
        target_compile_definitions(roclint_ast_token_budget_tests PRIVATE
            ROCLINT_GRANITE_TOKENIZER_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/models/granite-4.2-3b/granite-4.2-3b-Q8_0.gguf")
    endif()
    roclint_add_test(roclint_prompt_repository     tests/storage/test_prompt_repository.cpp          roclint_storage_testing)
    roclint_add_test(roclint_adversarial_persistence tests/storage/test_adversarial_m2.cpp             roclint_storage_testing)
    roclint_add_test(roclint_source_span           tests/analysis/testSourceSpan.cpp                 roclint_core_testing)
    roclint_add_test(roclint_normalized_analysis   tests/analysis/testNormalizedAnalysis.cpp         roclint_core_testing)
    roclint_add_test(roclint_analysis_snapshot     tests/analysis/testAnalysisSnapshot.cpp           roclint_storage_testing)
    roclint_add_test(roclint_assumption_scoring    tests/analysis/testAssumptionScoring.cpp          roclint_storage_testing)
    roclint_add_test(roclint_production_snapshot   tests/analysis/testProductionSnapshot.cpp         roclint_storage)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME roclint_julia_worker COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_julia_worker.py")
        set_tests_properties(roclint_julia_worker PROPERTIES TIMEOUT 30 ENVIRONMENT "PYTHONUTF8=1;PYTHONNOUSERSITE=1")
        add_test(NAME roclint_granite_probe COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_check_granite_runtime.py")
        set_tests_properties(roclint_granite_probe PROPERTIES TIMEOUT 30 ENVIRONMENT "PYTHONUTF8=1;PYTHONNOUSERSITE=1")
        add_test(NAME roclint_evaluation_runner COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_roclint_eval.py")
        set_tests_properties(roclint_evaluation_runner PROPERTIES TIMEOUT 30 ENVIRONMENT "PYTHONUTF8=1;PYTHONNOUSERSITE=1")
        if(TARGET roclint_granite_contracts)
            add_test(NAME roclint_grounded_rewrite_contracts COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_grounded_contracts.py")
            set_tests_properties(roclint_grounded_rewrite_contracts PROPERTIES TIMEOUT 60
                ENVIRONMENT "PYTHONUTF8=1;PYTHONNOUSERSITE=1;ROCLINT_CONTRACTS_TOOL=$<TARGET_FILE:roclint_granite_contracts>"
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
    endif()
    roclint_add_test(roclint_materiality_policy    tests/analysis/testMaterialityPolicy.cpp          roclint_core_testing)
    roclint_add_test(roclint_analysis_checkpoint   tests/analysis/test_analysis_checkpoint.cpp       roclint_core_testing)
    roclint_add_test(roclint_triage_audit_chain    tests/analysis/test_upstream_compatibility.cpp    roclint_storage_testing)
    roclint_add_test(roclint_jev_checkpoint        tests/analysis/test_jev_checkpoint.cpp            roclint_core_testing)
    roclint_add_test(roclint_granite_adapter       tests/bridge/test_granite_adapter.cpp             roclint_core_testing)
    roclint_add_test(roclint_s1_triage_adapter     tests/bridge/test_s1_triage_adapter.cpp           roclint_core_testing)
    roclint_add_test(roclint_granite_goldfish_adapter tests/bridge/test_granite_goldfish_adapter.cpp roclint_core_testing)
    roclint_add_test(roclint_intervention_state_machine tests/intent/testInterventionStateMachine.cpp roclint_core_testing)
    roclint_add_test(roclint_clarification_checkpoint tests/intent/test_clarification_checkpoint.cpp roclint_core_testing)
    roclint_add_test(roclint_confirmed_intent      tests/intent/testConfirmedIntent.cpp              roclint_core_testing)
    roclint_add_test(roclint_intent_checkpoint     tests/intent/test_intent_checkpoint.cpp           roclint_core_testing)
    roclint_add_test(roclint_rewrite_proposal      tests/rewrite/testRewriteProposal.cpp             roclint_core_testing)
    roclint_add_test(roclint_rewrite_profiles      tests/rewrite/testRewriteProfiles.cpp             roclint_core_testing)
    roclint_add_test(roclint_rewrite_checkpoint    tests/rewrite/test_rewrite_checkpoint.cpp         roclint_core_testing)
    roclint_add_test(roclint_structured_entity_extractor tests/rewrite/testStructuredEntityExtractor.cpp roclint_core_testing)
    roclint_add_test(roclint_creative_expansion    tests/rewrite/testCreativeExpansion.cpp           roclint_core_testing)
    roclint_add_test(roclint_approval_record       tests/review/testApprovalRecord.cpp               roclint_storage_testing)
    roclint_add_test(roclint_approval_checkpoint   tests/review/test_approval_checkpoint.cpp         roclint_storage_testing)

    if(BUILD_DESKTOP AND Qt6_FOUND)
        find_package(Qt6 6.2 REQUIRED COMPONENTS Test)
        roclint_add_test(roclint_granite_reasoning_policy tests/bridge/test_granite_reasoning_policy.cpp Qt6::Core)
        target_include_directories(roclint_granite_reasoning_policy_tests PRIVATE tests app/runtime "${CMAKE_CURRENT_BINARY_DIR}/generated")
        roclint_add_test(roclint_native_granite tests/bridge/test_native_granite_runtime.cpp roclint_host_lib_testing Qt6::Core Qt6::Network Qt6::Concurrent)
        target_compile_definitions(roclint_native_granite_tests PRIVATE
            ROCLINT_GRANITE_SOURCE_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/granite-models.json")
        target_include_directories(roclint_native_granite_tests PRIVATE app/runtime "${CMAKE_CURRENT_BINARY_DIR}/generated")
        roclint_add_test(roclint_host_lifecycle    tests/viewmodel/test_host_lifecycle.cpp    roclint_host_lib_testing Qt6::Core)
        roclint_add_test(roclint_invocation_binding tests/viewmodel/test_invocation_binding.cpp roclint_host_lib_testing Qt6::Core Qt6::Gui)
        roclint_add_test(roclint_async_persistence tests/viewmodel/test_async_persistence.cpp roclint_host_lib_testing Qt6::Core Qt6::Gui)
        roclint_add_test(roclint_host_persistence  tests/viewmodel/test_host_persistence.cpp  roclint_host_lib_testing Qt6::Core)
        roclint_add_test(roclint_scored_history    tests/viewmodel/test_scored_history.cpp    roclint_host_lib_testing Qt6::Core Qt6::Gui)
        roclint_add_test(roclint_host_controller   tests/viewmodel/test_host_controller.cpp   roclint_host_lib_testing Qt6::Core Qt6::Gui Qt6::Network)
        roclint_add_test(roclint_ast_freshness_integration tests/viewmodel/test_ast_freshness_integration.cpp roclint_host_lib_testing Qt6::Core Qt6::Gui)
        roclint_add_test(roclint_ast_stale_recovery tests/viewmodel/test_ast_stale_recovery.cpp roclint_host_lib_testing Qt6::Core Qt6::Gui)
        roclint_add_test(roclint_codebase_watcher tests/viewmodel/test_codebase_watcher.cpp roclint_host_lib_testing Qt6::Core)
        roclint_add_test(roclint_workspace_inspection tests/viewmodel/test_workspace_inspection.cpp roclint_host_lib_testing Qt6::Core)
        roclint_add_test(roclint_host_context      tests/viewmodel/test_host_context.cpp      roclint_host_lib_testing Qt6::Core Qt6::Gui)
        roclint_add_test(roclint_terminal_adapter  tests/viewmodel/test_terminal_adapter.cpp  roclint_host_lib_testing Qt6::Core)
        roclint_add_test(roclint_range_injection_policy tests/viewmodel/test_range_injection_policy.cpp roclint_host_lib_testing Qt6::Core)
        roclint_add_test(roclint_dual_mode         tests/viewmodel/test_dual_mode.cpp         roclint_host_lib_testing Qt6::Core Qt6::Gui)
        roclint_add_test(roclint_product_alignment tests/viewmodel/test_product_alignment.cpp roclint_host_lib_testing Qt6::Quick Qt6::Qml Qt6::Test)
        roclint_add_test(roclint_upstream_compatibility tests/viewmodel/test_upstream_compatibility.cpp roclint_host_lib_testing Qt6::Core)
        # Tests load these helpers beside the executable. On macOS that
        # directory is separate from the desktop app bundle.
        add_custom_target(roclint_test_runtime_files
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:roclint_workspace_inspection_tests>/runtime"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:roclint_workspace_helper>"
                "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/julia_worker.py"
                "$<TARGET_FILE_DIR:roclint_workspace_inspection_tests>/runtime/"
            DEPENDS roclint_workspace_helper VERBATIM)
        add_dependencies(roclint_workspace_inspection_tests roclint_test_runtime_files)
        add_dependencies(roclint_upstream_compatibility_tests roclint_test_runtime_files)
        target_sources(roclint_product_alignment_tests PRIVATE app/qml/shell/qml.qrc)
        if(WIN32)
            # A developer output folder can contain an older partial Qt deployment.
            # Resolve newly used QML modules (including QtCore.Settings) from the matching kit.
            set_property(TEST roclint_product_alignment APPEND PROPERTY ENVIRONMENT_MODIFICATION
                "QML_IMPORT_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../qml")
        endif()
    endif()
endif()

if(TARGET roclint_storage_tests)
    target_link_libraries(roclint_storage_tests PRIVATE Threads::Threads)
endif()
if(TARGET roclint_host_lib_testing)
    roclint_add_test(roclint_ipc_hardening tests/bridge/test_ipc_hardening.cpp roclint_host_lib_testing Qt6::Core Qt6::Concurrent)
    if(TARGET roclint_guardian_runtime_files)
        add_dependencies(roclint_ipc_hardening_tests roclint_guardian_runtime_files)
        add_dependencies(roclint_native_granite_tests roclint_guardian_runtime_files)
        add_dependencies(roclint_upstream_compatibility_tests roclint_guardian_runtime_files)
        if(Python3_Interpreter_FOUND)
            add_test(NAME roclint_process_guardian COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_process_guardian.py" --helper "$<TARGET_FILE:roclint_ipc_hardening_tests>")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# 4. Retrieval subsystem (R00) — roclint_retrieval library
# -----------------------------------------------------------------------------
set(ROCLINT_RETRIEVAL_SOURCES
    src/retrieval/RetrievalTypes.cpp
    src/retrieval/TextChunker.cpp
    src/retrieval/RetrievalIndex.cpp
    src/retrieval/RetrievalEngine.cpp
    src/retrieval/EmbeddingWorkerRuntime.cpp
)

function(roclint_add_retrieval_library name core_target storage_target)
    add_library(${name} STATIC ${ROCLINT_RETRIEVAL_SOURCES})
    target_link_libraries(${name} PUBLIC ${core_target} ${storage_target} SQLite3::SQLite3
                          PRIVATE Threads::Threads)
    target_include_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${name} PUBLIC cxx_std_20)
    roclint_apply_warnings(${name})
endfunction()

roclint_add_retrieval_library(roclint_retrieval roclint_core roclint_storage)

if(BUILD_TESTING)
    roclint_add_retrieval_library(roclint_retrieval_testing roclint_core_testing roclint_storage_testing)

    roclint_add_test(roclint_retrieval_index tests/retrieval/test_retrieval_index.cpp
                     roclint_retrieval_testing)
    target_include_directories(roclint_retrieval_index_tests PRIVATE tests)
    target_link_libraries(roclint_retrieval_index_tests PRIVATE Threads::Threads)

    # Integration test: runs only when Python and model assets are present.
    if(Python3_Interpreter_FOUND)
        add_test(NAME roclint_retrieval_integration
                 COMMAND "${Python3_EXECUTABLE}"
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/retrieval/test_retrieval_integration.py")
        set_tests_properties(roclint_retrieval_integration PROPERTIES
            TIMEOUT 300
            ENVIRONMENT "PYTHONUTF8=1;PYTHONNOUSERSITE=1"
            DISABLED TRUE)   # Disabled by default; requires model assets.
        message(STATUS "Retrieval integration test registered as DISABLED (needs model assets).")
    endif()

    # -------------------------------------------------------------------------
    # Roclint Retrieval Subsystem E2E Test Suite (Tiers 1-4)
    # -------------------------------------------------------------------------
    set(ROCLINT_E2E_SOURCES
        tests/retrieval/e2e/main.cpp
        tests/retrieval/e2e/test_e2e_tier1_storage.cpp
        tests/retrieval/e2e/test_e2e_tier1_coverage.cpp
        tests/retrieval/e2e/test_e2e_tier1_migration.cpp
        tests/retrieval/e2e/test_e2e_tier1_packaging.cpp
        tests/retrieval/e2e/test_e2e_tier1_model.cpp
        tests/retrieval/e2e/test_e2e_tier1_validation.cpp
        # Placeholder for subsequent tier test source files:
        # tests/retrieval/e2e/test_tier2_boundaries.cpp
        # tests/retrieval/e2e/test_tier3_regressions.cpp
        # tests/retrieval/e2e/test_tier4_workloads.cpp
    )

    add_executable(roclint_e2e_tests ${ROCLINT_E2E_SOURCES})
    target_include_directories(roclint_e2e_tests PRIVATE
        tests
        tests/retrieval/e2e
    )
    target_compile_features(roclint_e2e_tests PRIVATE cxx_std_20)
    roclint_apply_warnings(roclint_e2e_tests)
    target_link_libraries(roclint_e2e_tests PRIVATE
        roclint_retrieval_testing
        Threads::Threads
    )

    add_test(NAME roclint_e2e COMMAND roclint_e2e_tests)
    set_tests_properties(roclint_e2e PROPERTIES TIMEOUT 300 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    if(Python3_Interpreter_FOUND)
        target_compile_definitions(roclint_e2e_tests PRIVATE ROCLINT_TEST_PYTHON="${Python3_EXECUTABLE}")
    endif()
    if(TARGET Qt6::Core)
        set_tests_properties(roclint_e2e PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    # Fixture stress is separate from production retrieval/real-model evidence.
    roclint_add_test(roclint_retrieval_fixture_stress tests/retrieval/e2e/adversarial_stress.cpp
                     roclint_retrieval_testing Threads::Threads)
    target_include_directories(roclint_retrieval_fixture_stress_tests PRIVATE tests tests/retrieval/e2e)
    set_tests_properties(roclint_retrieval_fixture_stress PROPERTIES TIMEOUT 120)
    if(TARGET Qt6::Core)
        set_tests_properties(roclint_retrieval_fixture_stress PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endif()

endif()

# Retrieval benchmark tool (no model needed; uses internal mock runtime).
if(BUILD_DESKTOP OR BUILD_TESTING)
    add_executable(roclint_retrieval_benchmark tools/retrieval_benchmark.cpp)
    target_include_directories(roclint_retrieval_benchmark PRIVATE tests)
    target_link_libraries(roclint_retrieval_benchmark PRIVATE roclint_retrieval Threads::Threads)
    roclint_apply_warnings(roclint_retrieval_benchmark)
endif()
