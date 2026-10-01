@echo off
setlocal EnableDelayedExpansion
chcp 65001 >nul
echo =======================================================
echo   OpenSteamTool - Remove Auto Inject
echo =======================================================
echo.
echo Removing startup item from registry...

REM Remove startup item from current user registry
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "OpenSteamTool_AutoInject" /f >nul 2>&1

REM Also remove any legacy scheduled task if present
schtasks /delete /tn "OpenSteamTool_AutoInject" /f >nul 2>&1

REM Terminate running watcher process if active
taskkill /f /im ost-Injector.exe >nul 2>&1

echo.
echo =======================================================
echo [SUCCESS] Auto-start removed and background watcher stopped!
echo =======================================================
echo.
pause
exit /b 0
