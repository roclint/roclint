@echo off
echo Terminating any previous Roclint instances...
taskkill /F /IM roclint_host.exe 2>nul
taskkill /F /IM roclint_granite_worker.exe 2>nul
taskkill /F /IM roclint_workspace_helper.exe 2>nul
ping 127.0.0.1 -n 2 >nul

set "PATH=D:\Qt\6.8.2\msvc2022_64\bin;%PATH%"
cd /d "D:\Stardust\roclint\build\Release"
if exist "%LOCALAPPDATA%\roclint\Roclint\roclint-host.lock" del /f /q "%LOCALAPPDATA%\roclint\Roclint\roclint-host.lock"

echo Launching Roclint Host...
start "" "D:\Stardust\roclint\build\Release\roclint_host.exe" --show --pin --workspace "D:\Stardust\roclint"
