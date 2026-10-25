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
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wswitch-enum -Werror)
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
    src/rewrite/RewriteProfile.cpp
    src/rewrite/RewriteProposal.cpp
    src/rewrite/RewriteValidator.cpp
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
# 1b. Storage library (A03 / A04)
# -----------------------------------------------------------------------------
find_package(SQLite3 REQUIRED)
if(NOT TARGET SQLite3::SQLite3 AND TARGET SQLite::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

set(ROCLINT_STORAGE_SOURCES
    src/storage/Database.cpp
    src/storage/PayloadCodec.cpp
    src/storage/PromptRepository.cpp
)

function(roclint_add_storage_library name core_target)
    add_library(${name} STATIC ${ROCLINT_STORAGE_SOURCES})
    target_link_libraries(${name} PUBLIC ${core_target} SQLite3::SQLite3)
    target_include_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${name} PUBLIC cxx_std_20)
    roclint_apply_warnings(${name})
endfunction()

roclint_add_storage_library(roclint_storage roclint_core)

include(CTest)
if(BUILD_TESTING)
    roclint_add_core_library(roclint_core_testing ON)
    roclint_add_storage_library(roclint_storage_testing roclint_core_testing)
endif()

# -----------------------------------------------------------------------------
# 2. Desktop resident host (B00)
# -----------------------------------------------------------------------------
option(BUILD_DESKTOP "Build the desktop resident host and shell" ON)
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

        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)

        set(ROCLINT_HOST_SOURCES
            app/host/HostController.hpp
            app/host/HostController.cpp
            app/host/HostControllerWorkspace.cpp
            app/host/HostControllerTriage.cpp
            app/host/HostControllerResidency.cpp
            app/host/GraniteResidencyPolicy.hpp
            app/host/OllamaClient.hpp
            app/host/OllamaClient.cpp
            app/host/JuliaRuntime.hpp
            app/host/JuliaRuntime.cpp
            app/host/OwnedProcessJob.hpp
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
        target_link_libraries(roclint_granite_contracts PRIVATE roclint_core Qt6::Core)
        roclint_apply_warnings(roclint_granite_contracts)

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
                WIN32_EXECUTABLE FALSE # Kept console during development/testing for diagnostics
            )
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
        add_test(NAME ${test_name} COMMAND ${test_name}_tests)
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
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
    roclint_add_test(roclint_prompt_revision       tests/core/prompt_state/testPromptRevision.cpp    roclint_core_testing)
    roclint_add_test(roclint_revision_state        tests/core/prompt_state/testRevisionState.cpp     roclint_core_testing)
    roclint_add_test(roclint_storage               tests/storage/test_database.cpp                   roclint_storage_testing)
    roclint_add_test(roclint_prompt_repository     tests/storage/test_prompt_repository.cpp          roclint_storage_testing)
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
    endif()
    roclint_add_test(roclint_materiality_policy    tests/analysis/testMaterialityPolicy.cpp          roclint_core_testing)
    roclint_add_test(roclint_analysis_checkpoint   tests/analysis/test_analysis_checkpoint.cpp       roclint_core_testing)
    roclint_add_test(roclint_triage_audit_chain    tests/analysis/test_upstream_compatibility.cpp    roclint_storage_testing)
    roclint_add_test(roclint_jev_checkpoint        tests/analysis/test_jev_checkpoint.cpp            roclint_core_testing)
    roclint_add_test(roclint_granite_adapter       tests/bridge/test_granite_adapter.cpp             roclint_core_testing)
    roclint_add_test(roclint_s1_triage_adapter     tests/bridge/test_s1_triage_adapter.cpp           roclint_core_testing)
    roclint_add_test(roclint_intervention_state_machine tests/intent/testInterventionStateMachine.cpp roclint_core_testing)
    roclint_add_test(roclint_clarification_checkpoint tests/intent/test_clarification_checkpoint.cpp roclint_core_testing)
    roclint_add_test(roclint_confirmed_intent      tests/intent/testConfirmedIntent.cpp              roclint_core_testing)
    roclint_add_test(roclint_intent_checkpoint     tests/intent/test_intent_checkpoint.cpp           roclint_core_testing)
    roclint_add_test(roclint_rewrite_proposal      tests/rewrite/testRewriteProposal.cpp             roclint_core_testing)
    roclint_add_test(roclint_rewrite_profiles      tests/rewrite/testRewriteProfiles.cpp             roclint_core_testing)
    roclint_add_test(roclint_rewrite_checkpoint    tests/rewrite/test_rewrite_checkpoint.cpp         roclint_core_testing)
    roclint_add_test(roclint_approval_record       tests/review/testApprovalRecord.cpp               roclint_storage_testing)
    roclint_add_test(roclint_approval_checkpoint   tests/review/test_approval_checkpoint.cpp         roclint_storage_testing)

    if(BUILD_DESKTOP AND Qt6_FOUND)
        find_package(Qt6 6.2 REQUIRED COMPONENTS Test)
        roclint_add_test(roclint_native_granite tests/bridge/test_native_granite_runtime.cpp roclint_host_lib_testing Qt6::Core Qt6::Network Qt6::Concurrent)
        target_compile_definitions(roclint_native_granite_tests PRIVATE
            ROCLINT_GRANITE_SOURCE_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/granite-models.json")
        target_include_directories(roclint_native_granite_tests PRIVATE app/runtime "${CMAKE_CURRENT_BINARY_DIR}/generated")
        roclint_add_test(roclint_host_lifecycle    tests/viewmodel/test_host_lifecycle.cpp    roclint_host_lib_testing Qt6::Core)
        roclint_add_test(roclint_invocation_binding tests/viewmodel/test_invocation_binding.cpp roclint_host_lib_testing Qt6::Core Qt6::Gui)
        roclint_add_test(roclint_host_persistence  tests/viewmodel/test_host_persistence.cpp  roclint_host_lib_testing Qt6::Core)
        roclint_add_test(roclint_scored_history    tests/viewmodel/test_scored_history.cpp    roclint_host_lib_testing Qt6::Core Qt6::Gui)
        roclint_add_test(roclint_host_controller   tests/viewmodel/test_host_controller.cpp   roclint_host_lib_testing Qt6::Core Qt6::Gui Qt6::Network)
        roclint_add_test(roclint_host_context      tests/viewmodel/test_host_context.cpp      roclint_host_lib_testing Qt6::Core)
        roclint_add_test(roclint_terminal_adapter  tests/viewmodel/test_terminal_adapter.cpp  roclint_host_lib_testing Qt6::Core)
        roclint_add_test(roclint_dual_mode         tests/viewmodel/test_dual_mode.cpp         roclint_host_lib_testing Qt6::Core Qt6::Gui)
        roclint_add_test(roclint_product_alignment tests/viewmodel/test_product_alignment.cpp roclint_host_lib_testing Qt6::Quick Qt6::Qml Qt6::Test)
        roclint_add_test(roclint_upstream_compatibility tests/viewmodel/test_upstream_compatibility.cpp roclint_host_lib_testing Qt6::Core)
        target_sources(roclint_product_alignment_tests PRIVATE app/qml/shell/qml.qrc)
    endif()
endif()
