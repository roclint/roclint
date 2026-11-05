@echo off
setlocal
cd /d "%~dp0"

echo =======================================================
echo   Roclint Pinned Models Downloader for Windows
echo =======================================================
echo.
echo   Usage: download_models.bat [--verify] [--quantization Q8_0]
echo          --verify checks local models without downloading or creating a venv.
echo.

where python >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Python 3.11+ was not found on your PATH.
    echo Please install Python 3.11 or newer from https://www.python.org/
    echo and ensure "Add python.exe to PATH" is checked during installation.
    pause
    exit /b 1
)

python tools\download_all_models.py %*
if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Model download or integrity verification failed.
    pause
    exit /b %ERRORLEVEL%
)

echo.
echo =======================================================
echo   Setup Complete!
echo   Run start_roclint.bat to launch Roclint.
echo =======================================================
echo.
pause
