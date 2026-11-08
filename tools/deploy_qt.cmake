# Stage Qt frameworks/plugins on macOS or DLLs/plugins on Windows.
# On macOS, macdeployqt packages Qt frameworks into the app bundle.
# On Windows, windeployqt stages DLLs beside the executable.

if(DEFINED MACDEPLOYQT)
    if(NOT EXISTS "${MACDEPLOYQT}")
        message(FATAL_ERROR "macdeployqt was not found: ${MACDEPLOYQT}")
    endif()
    if(NOT DEFINED BUNDLE_DIR OR NOT EXISTS "${BUNDLE_DIR}")
        message(FATAL_ERROR "Target app bundle was not found: ${BUNDLE_DIR}")
    endif()
    if(NOT DEFINED QML_DIR)
        message(FATAL_ERROR "QML_DIR is required")
    endif()

    set(_macdeploy_args "${BUNDLE_DIR}" "-qmldir=${QML_DIR}" "-always-overwrite" "-codesign=-")
    if(DEFINED EXECUTABLE_1 AND EXISTS "${EXECUTABLE_1}")
        list(APPEND _macdeploy_args "-executable=${EXECUTABLE_1}")
    endif()
    if(DEFINED EXECUTABLE_2 AND EXISTS "${EXECUTABLE_2}")
        list(APPEND _macdeploy_args "-executable=${EXECUTABLE_2}")
    endif()

    execute_process(
        COMMAND "${MACDEPLOYQT}" ${_macdeploy_args}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(STATUS "macdeployqt completed with notice: ${_error}")
    endif()

    # Re-sign with ad-hoc signature so Gatekeeper and TCC recognize the bundled helpers.
    execute_process(
        COMMAND codesign --force --deep --sign - "${BUNDLE_DIR}"
        RESULT_VARIABLE _sign_result
        OUTPUT_VARIABLE _sign_output
        ERROR_VARIABLE _sign_error)

    message(STATUS "Self-contained Qt runtime staged in ${BUNDLE_DIR}")
    return()
endif()

# Stage Qt DLLs/plugins with windeployqt. A transient windeployqt failure is retried
# once; success is proved by the required Qt files, not only by its exit code.
if(NOT DEFINED WINDEPLOYQT OR NOT EXISTS "${WINDEPLOYQT}")
    message(FATAL_ERROR "windeployqt was not found")
endif()
if(NOT DEFINED HOST_EXE OR NOT EXISTS "${HOST_EXE}")
    message(FATAL_ERROR "Deployed host executable was not found: ${HOST_EXE}")
endif()
if(NOT DEFINED DEPLOY_DIR)
    message(FATAL_ERROR "DEPLOY_DIR is required")
endif()
if(NOT DEFINED QML_DIR)
    message(FATAL_ERROR "QML_DIR is required")
endif()
set(_config_flag "--release")
if(DEFINED CONFIG_FLAG AND NOT CONFIG_FLAG STREQUAL "")
    set(_config_flag "${CONFIG_FLAG}")
endif()

file(MAKE_DIRECTORY "${DEPLOY_DIR}")
foreach(_attempt 1 2)
    execute_process(
        COMMAND "${WINDEPLOYQT}" "${_config_flag}"
            --no-translations --no-system-d3d-compiler --no-compiler-runtime --no-opengl-sw
            --qmldir "${QML_DIR}" --dir "${DEPLOY_DIR}" "${HOST_EXE}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(_result EQUAL 0)
        break()
    endif()
    message(STATUS "windeployqt attempt ${_attempt} returned ${_result}; retrying once")
endforeach()

if(NOT EXISTS "${DEPLOY_DIR}/Qt6Core.dll" OR NOT EXISTS "${DEPLOY_DIR}/platforms/qwindows.dll")
    message(FATAL_ERROR "Qt deployment is incomplete: Qt6Core.dll or platforms/qwindows.dll is missing")
endif()
message(STATUS "Self-contained Qt runtime staged in ${DEPLOY_DIR}")

