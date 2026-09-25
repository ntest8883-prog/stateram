@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0UNINSTALL_BROWSER_BRIDGE_CURRENT_USER.ps1"
pause
