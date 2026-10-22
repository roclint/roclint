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
    src/core/prompt_state/SessionId.cpp
    src/core/prompt_state/RevisionId.cpp
    src/core/prompt_state/PromptRevision.cpp
    src/core/prompt_state/RevisionState.cpp
    src/analysis/SourceSpan.cpp
    src/analysis/NormalizedAnalysis.cpp
    src/analysis/MaterialityPolicy.cpp
    src/intent/InterventionStateMachine.cpp
    src/intent/ConfirmedIntent.cpp
    src/bridge/GraniteAdapter.cpp
    src/bridge/S1TriageAdapter.cpp
    src/rewrite/RewriteProfile.cpp
    src/rewrite/RewriteProposal.cpp
    src/rewrite/RewriteValidator.cpp
    src/bridge/GraniteRewriteAdapter.cpp
    src/review/ApprovalRecord.cpp
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
# 1b. Define the Storage Static Library Target (roclint_storage) - A03 / A04
# -----------------------------------------------------------------------------
find_package(SQLite3 REQUIRED)

add_library(roclint_storage STATIC
    src/storage/Database.cpp
    src/storage/PromptRepository.cpp
)

target_link_libraries(roclint_storage PUBLIC
    roclint_core
    SQLite3::SQLite3
)

target_include_directories(roclint_storage PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(roclint_storage PUBLIC cxx_std_20)

# -----------------------------------------------------------------------------
# 2. Desktop Resident Host Target (roclint_host) - B00
# -----------------------------------------------------------------------------
option(BUILD_DESKTOP "Build the desktop resident host and shell" ON)

if(BUILD_DESKTOP)
    if(APPLE AND NOT DEFINED CMAKE_PREFIX_PATH)
        # Automatically discover Qt6 across Apple Silicon, Intel Macs, and brew aliases
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
            app/host/GlobalShortcutManager.hpp
            app/host/GlobalShortcutManager.cpp
            app/host/HostContext.hpp
            app/host/HostContext.cpp
            app/host/ScreenPlacement.hpp
            app/host/ScreenPlacement.cpp
        )

        target_include_directories(roclint_host_lib PUBLIC
            app/host
        )

        target_link_libraries(roclint_host_lib PUBLIC
            roclint_core
            roclint_storage
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
        )

        if(APPLE)
            target_sources(roclint_host_lib PRIVATE app/host/HostContext_mac.mm)
            set_source_files_properties(app/host/HostContext_mac.mm PROPERTIES COMPILE_OPTIONS "-x;objective-c++")
            target_link_libraries(roclint_host_lib PUBLIC "-framework Carbon" "-framework AppKit" "-framework ApplicationServices")
        elseif(WIN32)
            target_link_libraries(roclint_host_lib PUBLIC user32 psapi)
        endif()

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
    # Core smoke test
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

    target_compile_features(roclint_core PUBLIC cxx_std_20)
    
        # Prompt revision test 
    add_executable(roclint_prompt_revision_tests
        tests/core/prompt_state/testPromptRevision.cpp
    )
    target_link_libraries(roclint_prompt_revision_tests PRIVATE roclint_core)
    target_include_directories(roclint_prompt_revision_tests PRIVATE tests)
    target_compile_features(roclint_prompt_revision_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_prompt_revision COMMAND roclint_prompt_revision_tests)

    # Revision state test 
    add_executable(roclint_revision_state_tests
        tests/core/prompt_state/testRevisionState.cpp
    )
    target_link_libraries(roclint_revision_state_tests PRIVATE roclint_core)
    target_include_directories(roclint_revision_state_tests PRIVATE tests)
    target_compile_features(roclint_revision_state_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_revision_state COMMAND roclint_revision_state_tests)

    # Storage foundation test 
    add_executable(roclint_storage_tests
        tests/storage/test_database.cpp
    )
    target_link_libraries(roclint_storage_tests PRIVATE roclint_storage)
    target_include_directories(roclint_storage_tests PRIVATE tests)
    target_compile_features(roclint_storage_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_storage COMMAND roclint_storage_tests)

    # Prompt
    add_executable(roclint_prompt_repository_tests
        tests/storage/test_prompt_repository.cpp
    )
    target_link_libraries(roclint_prompt_repository_tests PRIVATE roclint_storage)
    target_include_directories(roclint_prompt_repository_tests PRIVATE tests)
    target_compile_features(roclint_prompt_repository_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_prompt_repository COMMAND roclint_prompt_repository_tests)

    # SourceSpan and evidence tests
    add_executable(roclint_source_span_tests
        tests/analysis/testSourceSpan.cpp
    )
    target_link_libraries(roclint_source_span_tests PRIVATE roclint_core)
    target_include_directories(roclint_source_span_tests PRIVATE tests)
    target_compile_features(roclint_source_span_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_source_span COMMAND roclint_source_span_tests)

    # NormalizedAnalysis validation tests
    add_executable(roclint_normalized_analysis_tests
        tests/analysis/testNormalizedAnalysis.cpp
    )
    target_link_libraries(roclint_normalized_analysis_tests PRIVATE roclint_core)
    target_include_directories(roclint_normalized_analysis_tests PRIVATE tests)
    target_compile_features(roclint_normalized_analysis_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_normalized_analysis COMMAND roclint_normalized_analysis_tests)

    # Materiality policy tests
    add_executable(roclint_materiality_policy_tests
        tests/analysis/testMaterialityPolicy.cpp
    )
    target_link_libraries(roclint_materiality_policy_tests PRIVATE roclint_core)
    target_include_directories(roclint_materiality_policy_tests PRIVATE tests)
    target_compile_features(roclint_materiality_policy_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_materiality_policy COMMAND roclint_materiality_policy_tests)

    # Granite adapter tests
    add_executable(roclint_granite_adapter_tests
        tests/bridge/test_granite_adapter.cpp
    )
    target_link_libraries(roclint_granite_adapter_tests PRIVATE roclint_core)
    target_include_directories(roclint_granite_adapter_tests PRIVATE tests)
    target_compile_features(roclint_granite_adapter_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_granite_adapter COMMAND roclint_granite_adapter_tests)

    # S1 triage adapter tests
    add_executable(roclint_s1_triage_adapter_tests
        tests/bridge/test_s1_triage_adapter.cpp
    )
    target_link_libraries(roclint_s1_triage_adapter_tests PRIVATE roclint_core)
    target_include_directories(roclint_s1_triage_adapter_tests PRIVATE tests)
    target_compile_features(roclint_s1_triage_adapter_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_s1_triage_adapter COMMAND roclint_s1_triage_adapter_tests)

    # Analysis Checkpoint integration tests
    add_executable(roclint_analysis_checkpoint_tests
        tests/analysis/test_analysis_checkpoint.cpp
    )
    target_link_libraries(roclint_analysis_checkpoint_tests PRIVATE roclint_core)
    target_include_directories(roclint_analysis_checkpoint_tests PRIVATE tests)
    target_compile_features(roclint_analysis_checkpoint_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_analysis_checkpoint COMMAND roclint_analysis_checkpoint_tests)

    # Fast triage checkpoint tests
    add_executable(roclint_jev_checkpoint_tests
        tests/analysis/test_jev_checkpoint.cpp
    )
    target_link_libraries(roclint_jev_checkpoint_tests PRIVATE roclint_core)
    target_include_directories(roclint_jev_checkpoint_tests PRIVATE tests)
    target_compile_features(roclint_jev_checkpoint_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_jev_checkpoint COMMAND roclint_jev_checkpoint_tests)

    # Intervention state machine tests
    add_executable(roclint_intervention_state_machine_tests
        tests/intent/testInterventionStateMachine.cpp
    )
    target_link_libraries(roclint_intervention_state_machine_tests PRIVATE roclint_core)
    target_include_directories(roclint_intervention_state_machine_tests PRIVATE tests)
    target_compile_features(roclint_intervention_state_machine_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_intervention_state_machine COMMAND roclint_intervention_state_machine_tests)

    # Clarification checkpoint tests
    add_executable(roclint_clarification_checkpoint_tests
        tests/intent/test_clarification_checkpoint.cpp
    )
    target_link_libraries(roclint_clarification_checkpoint_tests PRIVATE roclint_core)
    target_include_directories(roclint_clarification_checkpoint_tests PRIVATE tests)
    target_compile_features(roclint_clarification_checkpoint_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_clarification_checkpoint COMMAND roclint_clarification_checkpoint_tests)

    # Confirmed intent tests
    add_executable(roclint_confirmed_intent_tests
        tests/intent/testConfirmedIntent.cpp
    )
    target_link_libraries(roclint_confirmed_intent_tests PRIVATE roclint_core)
    target_include_directories(roclint_confirmed_intent_tests PRIVATE tests)
    target_compile_features(roclint_confirmed_intent_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_confirmed_intent COMMAND roclint_confirmed_intent_tests)

    # Intent checkpoint tests
    add_executable(roclint_intent_checkpoint_tests
        tests/intent/test_intent_checkpoint.cpp
    )
    target_link_libraries(roclint_intent_checkpoint_tests PRIVATE roclint_core)
    target_include_directories(roclint_intent_checkpoint_tests PRIVATE tests)
    target_compile_features(roclint_intent_checkpoint_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_intent_checkpoint COMMAND roclint_intent_checkpoint_tests)

    # Rewrite proposal tests
    add_executable(roclint_rewrite_proposal_tests
        tests/rewrite/testRewriteProposal.cpp
    )
    target_link_libraries(roclint_rewrite_proposal_tests PRIVATE roclint_core)
    target_include_directories(roclint_rewrite_proposal_tests PRIVATE tests)
    target_compile_features(roclint_rewrite_proposal_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_rewrite_proposal COMMAND roclint_rewrite_proposal_tests)

    # Rewrite profiles tests
    add_executable(roclint_rewrite_profiles_tests
        tests/rewrite/testRewriteProfiles.cpp
    )
    target_link_libraries(roclint_rewrite_profiles_tests PRIVATE roclint_core)
    target_include_directories(roclint_rewrite_profiles_tests PRIVATE tests)
    target_compile_features(roclint_rewrite_profiles_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_rewrite_profiles COMMAND roclint_rewrite_profiles_tests)

    # Rewrite checkpoint tests
    add_executable(roclint_rewrite_checkpoint_tests
        tests/rewrite/test_rewrite_checkpoint.cpp
    )
    target_link_libraries(roclint_rewrite_checkpoint_tests PRIVATE roclint_core)
    target_include_directories(roclint_rewrite_checkpoint_tests PRIVATE tests)
    target_compile_features(roclint_rewrite_checkpoint_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_rewrite_checkpoint COMMAND roclint_rewrite_checkpoint_tests)

    # Approval record tests
    add_executable(roclint_approval_record_tests
        tests/review/testApprovalRecord.cpp
    )
    target_link_libraries(roclint_approval_record_tests PRIVATE roclint_core roclint_storage)
    target_include_directories(roclint_approval_record_tests PRIVATE tests)
    target_compile_features(roclint_approval_record_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_approval_record COMMAND roclint_approval_record_tests)

    # Approval checkpoint tests
    add_executable(roclint_approval_checkpoint_tests
        tests/review/test_approval_checkpoint.cpp
    )
    target_link_libraries(roclint_approval_checkpoint_tests PRIVATE roclint_core roclint_storage)
    target_include_directories(roclint_approval_checkpoint_tests PRIVATE tests)
    target_compile_features(roclint_approval_checkpoint_tests PRIVATE cxx_std_20)
    add_test(NAME roclint_approval_checkpoint COMMAND roclint_approval_checkpoint_tests)


    # ViewModel 
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

        # Invocation 
        add_executable(roclint_invocation_tests
            tests/viewmodel/test_invocation_binding.cpp
        )

        target_link_libraries(roclint_invocation_tests PRIVATE
            roclint_host_lib
            roclint_core
            Qt6::Core
            Qt6::Gui
        )

        target_include_directories(roclint_invocation_tests PRIVATE tests app/host)
        target_compile_features(roclint_invocation_tests PRIVATE cxx_std_20)
        add_test(NAME roclint_invocation_binding COMMAND roclint_invocation_tests)

        # Host Persistence
        add_executable(roclint_persistence_tests
            tests/viewmodel/test_host_persistence.cpp
        )

        target_link_libraries(roclint_persistence_tests PRIVATE
            roclint_host_lib
            roclint_core
            roclint_storage
            Qt6::Core
        )

        target_include_directories(roclint_persistence_tests PRIVATE tests app/host)
        target_compile_features(roclint_persistence_tests PRIVATE cxx_std_20)
        add_test(NAME roclint_host_persistence COMMAND roclint_persistence_tests)
    endif()
endif()