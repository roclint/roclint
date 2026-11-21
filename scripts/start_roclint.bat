@echo off
setlocal
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "DEPLOY=%BUILD%\Release\bin\roclint_host.exe"
set "FALLBACK=%BUILD%\Release\roclint_host.exe"
set "QT_BIN=D:\Qt\6.8.2\msvc2022_64\bin"

echo Terminating any previous Roclint instances...
taskkill /F /IM roclint_host.exe 2>nul
taskkill /F /IM roclint_granite_worker.exe 2>nul
taskkill /F /IM roclint_workspace_helper.exe 2>nul
ping 127.0.0.1 -n 2 >nul

if exist "%DEPLOY%" goto launch_deploy

echo [*] Staging self-contained Qt runtime...
set "CMAKE_EXE="
where cmake >nul 2>nul && set "CMAKE_EXE=cmake"
if defined CMAKE_EXE goto deploy_build
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if NOT defined CMAKE_EXE goto deploy_missing

:deploy_build
"%CMAKE_EXE%" --build "%BUILD%" --config Release --target roclint_deploy_qt
if exist "%DEPLOY%" goto launch_deploy

:deploy_missing
echo [WARN] Self-contained deployment is unavailable; using the Qt kit on PATH.
set "PATH=%QT_BIN%;%PATH%"
set "HOST=%FALLBACK%"
set "WORKDIR=%BUILD%\Release"
goto launch

:launch_deploy
set "HOST=%DEPLOY%"
set "WORKDIR=%BUILD%\Release\bin"

:launch
if not exist "%HOST%" (
    echo [ERROR] roclint_host.exe was not found. Configure and build the Release target first.
    pause
    exit /b 1
)

if exist "%LOCALAPPDATA%\roclint\Roclint\roclint-host.lock" del /f /q "%LOCALAPPDATA%\roclint\Roclint\roclint-host.lock"

echo Launching Roclint Host from %HOST%
pushd "%WORKDIR%"
start "" "%HOST%" --show --pin --workspace "%ROOT%"
popd
