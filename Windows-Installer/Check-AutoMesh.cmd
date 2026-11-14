@echo off
chcp 65001 >nul
cd /d "%~dp0"
if exist "AutoMesh-Installer.exe" (
    "AutoMesh-Installer.exe" --check
) else (
    py -3 installer.py --check
)
echo.
pause

