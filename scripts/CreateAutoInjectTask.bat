@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul
echo =======================================================
echo   OpenSteamTool - Setup Auto Inject (Standard Startup)
echo =======================================================
echo.
echo Configuring auto-start for current user...

set "INJECTOR_EXE=%~dp0ost-Injector.exe"

REM Clean up any legacy scheduled task if previously configured
schtasks /delete /tn "OpenSteamTool_AutoInject" /f >nul 2>&1

REM Register auto-start in current user registry (HKCU Run)
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "OpenSteamTool_AutoInject" /t REG_SZ /d "\"%INJECTOR_EXE%\" -watch" /f >nul

if %errorlevel% equ 0 (
    echo.
    echo =======================================================
    echo [SUCCESS] Auto-start configured successfully!
    echo Starting background watcher right now...
    start "" "%INJECTOR_EXE%" -watch
    echo.
    echo The background watcher is now running and will auto-start
    echo upon user logon [manageable via Windows Task Manager].
    echo No administrator privileges required.
    echo =======================================================
    echo.
    pause
    exit /b 0
) else (
    echo.
    echo =======================================================
    echo [FAILED] Failed to register startup item.
    echo =======================================================
    echo.
    pause
    exit /b 1
)
