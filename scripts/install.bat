@echo off
REM One-command mongui installer for Windows — wraps install.ps1 so users don't
REM have to fight PowerShell's execution policy. Just run:  scripts\install.bat
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
