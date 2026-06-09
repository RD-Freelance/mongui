@echo off
REM One-command mongui uninstaller for Windows. Run:  scripts\uninstall.bat
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
