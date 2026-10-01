<#
.SYNOPSIS
Runs the clang-tidy check that gates pull requests (.github/workflows/clang_tidy.yml) on
your branch. Windows.

.DESCRIPTION
Configures a separate build directory (build-tidy) without the precompiled header, installs
the pinned clang-tidy into a virtual environment inside it, and runs
scripts/clang_tidy_diff.py the way CI does. Uncommitted changes are checked too.

CI runs on Linux. Here clang-tidy also sees Windows-only code and MSVC's standard library,
so it can report findings CI does not; a change that passes here and on Linux passes CI.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File scripts\run_clang_tidy.ps1
.EXAMPLE
powershell -ExecutionPolicy Bypass -File scripts\run_clang_tidy.ps1 -Fix
#>
param(
    # Revision to compare against (default: main of the remote that points at
    # OrcaSlicer/OrcaSlicer, else origin/main).
    [string]$Base = "",
    # Do not fetch that remote's main first.
    [switch]$NoFetch,
    # Build directory for the compile database.
    [string]$BuildDir = "build-tidy",
    # Dependency build directory (default: the deps\build* tree build_win.bat made).
    [string]$DepsDir = "",
    # x64 or arm64 (default: this machine's).
    [string]$Arch = "",
    # Parallel clang-tidy runs (default: all cores).
    [int]$Jobs = 0,
    # Apply clang-tidy's fixes (adds the missing includes).
    [switch]$Fix,
    # Install missing tools without asking.
    [switch]$Yes
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $Root $BuildDir }

# Ask before installing anything. Without a console, or with "no", print the command
# instead so it can be run by hand.
function Ask([string]$Question) {
    if ($Yes) { return $true }
    if ([Console]::IsInputRedirected) { return $false }
    $reply = Read-Host "$Question [y/N]"
    return $reply -match '^[Yy]'
}

function Has([string]$Command) {
    return [bool](Get-Command $Command -ErrorAction SilentlyContinue)
}

function Request-Install([string]$What, [string]$Command) {
    Write-Host "Missing: $What"
    if (Ask "Install it now with: $Command ?") {
        & cmd /c $Command
        if ($LASTEXITCODE -ne 0) { throw "Installing $What failed." }
        Write-Host "Installed. Open a new terminal so PATH picks it up, then run this again."
    } else {
        Write-Host "To install it yourself, run:"
        Write-Host "    $Command"
    }
    exit 1
}

if (-not $Arch) {
    if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { $Arch = "arm64" } else { $Arch = "x64" }
}

# --- System tools -------------------------------------------------------------

if (-not (Has "git")) { Request-Install "Git" "build_win.bat --install-deps" }

# Python: the py launcher, else a python.exe that is not the Microsoft Store stub.
$Python = $null
if (Has "py") {
    & py -3 --version *> $null
    if ($LASTEXITCODE -eq 0) { $Python = @("py", "-3") }
}
if (-not $Python -and (Has "python")) {
    & python --version *> $null
    if ($LASTEXITCODE -eq 0) { $Python = @("python") }
}
if (-not $Python) { Request-Install "Python 3" "winget install -e --id Python.Python.3.12" }
$PyExe = $Python[0]
$PyArgs = @($Python | Select-Object -Skip 1)

# Visual Studio provides the compiler, and its CMake component provides CMake and Ninja.
$Vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$VsPath = $null
if (Test-Path $Vswhere) {
    $VsPath = & $Vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $VsPath) { Request-Install "Visual Studio with the C++ tools" "build_win.bat --install-vs buildtools" }

# Load the developer environment, as build_win.bat does.
$HostArch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
$VsDevCmd = Join-Path $VsPath "Common7\Tools\VsDevCmd.bat"
$envLines = & cmd /c "`"$VsDevCmd`" -arch=$Arch -host_arch=$HostArch -no_logo >nul && set"
foreach ($line in $envLines) {
    $i = $line.IndexOf("=")
    if ($i -gt 0) { Set-Item -Path ("env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
}
if (-not (Has "cmake")) { Request-Install "CMake" "build_win.bat --install-deps" }
if (-not (Has "ninja")) { Request-Install "Ninja" "winget install -e --id Ninja-build.Ninja" }

# --- clang-tidy ---------------------------------------------------------------

$Requirements = Join-Path $Root "scripts\clang_tidy_requirements.txt"
$Pinned = ((Get-Content $Requirements | Select-String '^clang-tidy==(.+)$').Matches[0].Groups[1].Value).Trim()
$Venv = Join-Path $BuildDir "clang-tidy-venv"
$ClangTidy = $env:CLANG_TIDY

function Test-Pinned([string]$Exe) {
    if (-not $Exe -or -not (Test-Path $Exe)) { return $false }
    return [bool]((& $Exe --version) -match "version $([regex]::Escape($Pinned))")
}

if (-not $ClangTidy) {
    $ClangTidy = Join-Path $Venv "Scripts\clang-tidy.exe"
    if (-not (Test-Pinned $ClangTidy)) {
        if (Ask "clang-tidy $Pinned (the version CI uses) is not installed. Install it into $Venv?") {
            New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
            & $PyExe @PyArgs -m venv $Venv
            if ($LASTEXITCODE -ne 0) { throw "Could not create a Python virtual environment." }
            & "$Venv\Scripts\python.exe" -m pip install --quiet --upgrade pip
            & "$Venv\Scripts\python.exe" -m pip install --quiet -r $Requirements
            if ($LASTEXITCODE -ne 0) { throw "Installing clang-tidy $Pinned failed." }
        } else {
            Write-Host "To install it yourself, run:"
            Write-Host "    $($Python -join ' ') -m venv $Venv"
            Write-Host "    $Venv\Scripts\python.exe -m pip install -r scripts\clang_tidy_requirements.txt"
            exit 1
        }
    }
}
if (-not (Test-Pinned $ClangTidy)) {
    Write-Warning "$ClangTidy is not clang-tidy $Pinned, so results may differ from CI."
}

# --- Dependencies -------------------------------------------------------------

# build_win.bat names the tree after the compiler and architecture: deps\build for cl,
# deps\build-clang for clang-cl, with -arm64 appended on ARM64.
$Suffix = if ($Arch -eq "arm64") { "-arm64" } else { "" }
$Compiler = "cl"
# A configured build directory remembers where its dependencies are.
$Cache = Join-Path $BuildDir "CMakeCache.txt"
if (-not $DepsDir -and (Test-Path $Cache)) {
    $m = Select-String -Path $Cache -Pattern '^DEP_BUILD_DIR:[A-Z]*=(.+)$' | Select-Object -First 1
    if ($m) { $DepsDir = $m.Matches[0].Groups[1].Value }
}
if (-not $DepsDir) {
    if (Test-Path "deps\build$Suffix\OrcaSlicer_dep") {
        $DepsDir = "deps\build$Suffix"
    } elseif (Test-Path "deps\build-clang$Suffix\OrcaSlicer_dep") {
        $DepsDir = "deps\build-clang$Suffix"
    }
}
if ($DepsDir -and ($DepsDir -match 'clang')) { $Compiler = "clang-cl" }
if (-not $DepsDir -or -not (Test-Path (Join-Path $DepsDir "OrcaSlicer_dep"))) {
    $BuildDeps = "build_win.bat -d --arch $Arch"
    Write-Host "OrcaSlicer's dependencies are not built."
    if (Ask "Build them now with $BuildDeps? This takes a while.") {
        & cmd /c $BuildDeps
        if ($LASTEXITCODE -ne 0) { throw "Building the dependencies failed." }
        $DepsDir = "deps\build$Suffix"
    } else {
        Write-Host "Build them with $BuildDeps, or point to an existing build with -DepsDir."
        exit 1
    }
}
$DepsDir = (Resolve-Path $DepsDir).Path
if ($Compiler -eq "clang-cl" -and -not (Has "clang-cl")) {
    Request-Install "clang-cl (the Visual Studio C++ Clang tools)" "build_win.bat --install-vs buildtools"
}

# --- Compile database ---------------------------------------------------------

# The same configure as CI, with Ninja so CMake writes compile_commands.json. Forward
# slashes keep CMake from reading a backslash as an escape.
$cmakeArgs = @("-S", ".", "-B", $BuildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=$Compiler", "-DCMAKE_CXX_COMPILER=$Compiler",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DSLIC3R_PCH=OFF", "-DORCA_TOOLS=ON", "-DBUILD_TESTS=ON",
    "-DDEP_BUILD_DIR=$($DepsDir -replace '\\', '/')")
Write-Host "Configuring $BuildDir with $Compiler"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$Log = Join-Path $BuildDir "configure.log"
& cmake @cmakeArgs *> $Log
if ($LASTEXITCODE -ne 0) {
    Get-Content $Log -Tail 20
    throw "Configuring failed; the full log is in $Log."
}
& cmake --build $BuildDir --target git_commit_hash_header *> $null
if ($LASTEXITCODE -ne 0) { throw "Generating git_commit_hash.h failed." }

# --- Base revision ------------------------------------------------------------

if (-not $Base) {
    $Remote = "origin"
    foreach ($line in (& git remote -v)) {
        if ($line -match '^(\S+)\s+\S*github\.com[:/]OrcaSlicer/OrcaSlicer(\.git)?\s+\(fetch\)') {
            $Remote = $Matches[1]
            break
        }
    }
    if (-not $NoFetch) {
        & git fetch --quiet $Remote main
        if ($LASTEXITCODE -ne 0) { throw "git fetch $Remote main failed." }
    }
    $Base = "$Remote/main"
}
Write-Host "Comparing against $Base"

# --- Run ----------------------------------------------------------------------

$diffArgs = @("scripts\clang_tidy_diff.py", "-p", $BuildDir, "--base", $Base, "--clang-tidy", $ClangTidy)
if ($Jobs -gt 0) { $diffArgs += @("-j", "$Jobs") }
if ($Fix) { $diffArgs += @("--", "--fix") }
& $PyExe @PyArgs @diffArgs
exit $LASTEXITCODE
