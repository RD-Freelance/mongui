#Requires -Version 5.1
<#
.SYNOPSIS
    mongui uninstaller for Windows.

.DESCRIPTION
    Removes the installed mongui.exe (and its DLLs) and takes the install
    directory back off your user PATH. Run from the repo root:

        ./scripts/uninstall.bat
        # or:  powershell -ExecutionPolicy Bypass -File scripts/uninstall.ps1

    It does NOT remove Git, CMake, the MSVC build tools, or vcpkg — those are
    general-purpose tools that other software may rely on.

.PARAMETER InstallDir
    The directory mongui was installed to. Default: %LOCALAPPDATA%\Programs\mongui

.PARAMETER RemoveBuild
    Also delete the ./build directory.
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\mongui'),
    [switch]$RemoveBuild
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDir = Join-Path $RepoRoot 'build'

function Info($m) { Write-Host "==> $m" -ForegroundColor Green }
function Warn($m) { Write-Host "==> $m" -ForegroundColor Yellow }

$removed = $false

# 1. Remove the install directory (exe + DLLs).
if (Test-Path $InstallDir) {
    Info "Removing $InstallDir …"
    Remove-Item -Recurse -Force $InstallDir
    $removed = $true
} else {
    Warn "Install directory not found: $InstallDir"
}

# 2. Strip it from the user PATH.
$userPath = [Environment]::GetEnvironmentVariable('Path','User')
if ($userPath -and ($userPath -like "*$InstallDir*")) {
    Info "Removing $InstallDir from your user PATH …"
    $clean = ($userPath -split ';' | Where-Object { $_ -and ($_ -ne $InstallDir) }) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $clean, 'User')
    Warn "Open a new terminal for the PATH change to take effect."
    $removed = $true
}

# 3. Optionally remove the build directory.
if ($RemoveBuild -and (Test-Path $BuildDir)) {
    Info "Removing build directory $BuildDir …"
    Remove-Item -Recurse -Force $BuildDir
}

if ($removed) { Info "Done — mongui uninstalled." }
else          { Warn "Nothing to remove. If you used a custom -InstallDir, pass it again." }
