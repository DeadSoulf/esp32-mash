@echo off
chcp 65001 >nul
cd /d "%~dp0"
if exist "AutoMesh-Installer.exe" (
    "AutoMesh-Installer.exe" %*
) else (
    where py >nul 2>nul
    if errorlevel 1 (
        echo Python not found and AutoMesh-Installer.exe is absent.
        echo Use the full installer package.
        pause
        exit /b 1
    )
    py -3 installer.py %*
)
echo.
if not "%~1"=="--check" pause

