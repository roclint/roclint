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
    if(NOT IS_DIRECTORY "${QML_DIR}" OR NOT IS_DIRECTORY "${RUNTIME_DIR}")
        message(FATAL_ERROR "QML_DIR and RUNTIME_DIR must point to existing directories")
    endif()

    set(_macdeploy_args "${BUNDLE_DIR}" "-qmldir=${QML_DIR}" "-always-overwrite" "-codesign=-")
    set(_runtime_files
        "${RUNTIME_DIR}/julia_worker.py"
        "${RUNTIME_DIR}/roclint_process_guardian"
        "${RUNTIME_DIR}/roclint_workspace_helper")
    list(APPEND _macdeploy_args "-executable=${RUNTIME_DIR}/roclint_workspace_helper")
    if(DEFINED GRANITE_WORKER)
        list(APPEND _runtime_files "${GRANITE_WORKER}")
        list(APPEND _macdeploy_args "-executable=${GRANITE_WORKER}")
    endif()
    foreach(_file IN LISTS _runtime_files)
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "Required runtime file missing from the bundle: ${_file}")
        endif()
    endforeach()

    # Older setup runs put the virtualenv beside the Mach-O helpers.
    # Keep its package directories outside the bundle's executable directory.
    if(IS_DIRECTORY "${RUNTIME_DIR}/julia-python")
        set(_python_dir "${BUNDLE_DIR}/Contents/runtime")
        file(MAKE_DIRECTORY "${_python_dir}")
        if(EXISTS "${_python_dir}/julia-python")
            message(FATAL_ERROR "Two Julia Python environments were found in the bundle. Keep the intended one in ${_python_dir}.")
        endif()
        file(RENAME "${RUNTIME_DIR}/julia-python" "${_python_dir}/julia-python" RESULT _move_result)
        if(NOT "${_move_result}" STREQUAL "0")
            message(FATAL_ERROR "Could not move the legacy Julia Python environment into ${_python_dir}: ${_move_result}")
        endif()
    endif()

    function(roclint_run_deploy_step step)
        execute_process(COMMAND ${ARGN}
            RESULT_VARIABLE _result
            OUTPUT_VARIABLE _output
            ERROR_VARIABLE _error)
        if(NOT "${_result}" STREQUAL "0")
            message(FATAL_ERROR "${step} failed (${_result})\n${_output}\n${_error}")
        endif()
    endfunction()

    roclint_run_deploy_step(macdeployqt "${MACDEPLOYQT}" ${_macdeploy_args})
    roclint_run_deploy_step("codesign signing" codesign --force --deep --sign - "${BUNDLE_DIR}")
    roclint_run_deploy_step("codesign verification" codesign --verify --deep --strict "${BUNDLE_DIR}")

    foreach(_file IN ITEMS "Frameworks/QtCore.framework/QtCore" "PlugIns/platforms/libqcocoa.dylib")
        if(NOT EXISTS "${BUNDLE_DIR}/Contents/${_file}")
            message(FATAL_ERROR "Qt deployment is incomplete: ${_file} is missing")
        endif()
    endforeach()
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

