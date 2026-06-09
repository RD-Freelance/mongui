#Requires -Version 5.1
<#
.SYNOPSIS
    mongui installer for Windows.

.DESCRIPTION
    One command (run from the repo root in PowerShell):

        ./scripts/install.ps1

    It will:
      1. Ensure Git, CMake, and the MSVC C++ build tools are installed
         (via winget when missing).
      2. Bootstrap vcpkg and install the MongoDB C driver.
      3. Build mongui (Release, x64).
      4. Install mongui.exe + its DLLs and add it to your user PATH.

    Requirements: Windows 10 1903+ (for ANSI/VT console support) — Windows
    Terminal is strongly recommended. Run in a normal (non-elevated) PowerShell;
    winget may pop UAC prompts when installing tools.

.PARAMETER InstallDir
    Where to place the binary. Default: %LOCALAPPDATA%\Programs\mongui
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\mongui')
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDir = Join-Path $RepoRoot 'build'
$VcpkgRoot = Join-Path $env:LOCALAPPDATA 'vcpkg'

function Info($m) { Write-Host "==> $m" -ForegroundColor Green }
function Warn($m) { Write-Host "==> $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "error: $m" -ForegroundColor Red; exit 1 }
function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

# ---------------------------------------------------------------------------
# 1. Prerequisites (Git, CMake, MSVC build tools)
# ---------------------------------------------------------------------------
function Ensure-Winget {
    if (-not (Have winget)) {
        Die "winget not found. Install 'App Installer' from the Microsoft Store, or install Git, CMake and Visual Studio Build Tools (C++) manually, then re-run."
    }
}

function Winget-Install($id) {
    Info "Installing $id …"
    winget install --id $id --source winget --accept-package-agreements --accept-source-agreements --silent -e
}

function Ensure-Tools {
    Ensure-Winget
    if (-not (Have git))   { Winget-Install 'Git.Git' }
    if (-not (Have cmake)) { Winget-Install 'Kitware.CMake' }

    # Detect an MSVC C++ toolchain via vswhere.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $hasVc = $false
    if (Test-Path $vswhere) {
        $vc = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vc) { $hasVc = $true }
    }
    if (-not $hasVc) {
        Warn "MSVC C++ build tools not found — installing Visual Studio Build Tools (this is a large download)…"
        winget install --id Microsoft.VisualStudio.2022.BuildTools --source winget `
            --accept-package-agreements --accept-source-agreements --silent -e `
            --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    }

    # Refresh PATH for this session so freshly installed tools are visible.
    $env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path','User')
}

# ---------------------------------------------------------------------------
# 2. vcpkg + MongoDB C driver
# ---------------------------------------------------------------------------
function Ensure-Vcpkg {
    if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
        if (-not (Test-Path $VcpkgRoot)) {
            Info "Cloning vcpkg to $VcpkgRoot …"
            git clone --depth 1 https://github.com/microsoft/vcpkg $VcpkgRoot
        }
        Info "Bootstrapping vcpkg …"
        & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
    }
    Info "Installing mongo-c-driver (vcpkg, x64-windows) … this can take a while."
    & (Join-Path $VcpkgRoot 'vcpkg.exe') install mongo-c-driver:x64-windows
}

# ---------------------------------------------------------------------------
# 3. Build mongui
# ---------------------------------------------------------------------------
function Build-Mongui {
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    $toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    Info "Configuring (CMake, vcpkg toolchain) …"
    cmake -S $RepoRoot -B $BuildDir -A x64 `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DVCPKG_TARGET_TRIPLET=x64-windows"
    Info "Building (Release) …"
    cmake --build $BuildDir --config Release
}

# ---------------------------------------------------------------------------
# 4. Install onto PATH
# ---------------------------------------------------------------------------
function Install-Binary {
    $exe = Join-Path $BuildDir 'Release\mongui.exe'
    if (-not (Test-Path $exe)) { Die "build did not produce $exe" }

    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Info "Installing to $InstallDir …"
    # Copy the exe plus the DLLs vcpkg placed next to it (applocal deps).
    Copy-Item $exe $InstallDir -Force
    Get-ChildItem (Join-Path $BuildDir 'Release') -Filter *.dll |
        ForEach-Object { Copy-Item $_.FullName $InstallDir -Force }

    # Add to the user PATH if not already present.
    $userPath = [Environment]::GetEnvironmentVariable('Path','User')
    if ($userPath -notlike "*$InstallDir*") {
        Info "Adding $InstallDir to your user PATH …"
        [Environment]::SetEnvironmentVariable('Path', "$userPath;$InstallDir", 'User')
        Warn "Open a new terminal for the PATH change to take effect."
    }
}

Write-Host "Installing mongui…" -ForegroundColor White
Ensure-Tools
Ensure-Vcpkg
Build-Mongui
Install-Binary
Write-Host ""
Info "Done. Open a new terminal and run:  mongui"
