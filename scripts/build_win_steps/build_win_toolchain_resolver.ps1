# Unit: Toolchain resolution
# Resolves Visual Studio, clang-cl, CMake, Ninja and ccache.

function Resolve-Tool {
    param([string]$Name, [string]$EnvVar, [string]$Hint)
    $override = [Environment]::GetEnvironmentVariable($EnvVar)
    if ($override) {
        if (Test-Path $override) { return (Resolve-Path $override).Path }
        throw "$EnvVar points at '$override', which does not exist."
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    throw "No $Name found. $Hint"
}

function Find-VsInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($found) { return $found.Trim() }
    }
    foreach ($version in @("2022", "18")) {
        foreach ($root in @("$env:ProgramFiles\\Microsoft Visual Studio\\$version", "${env:ProgramFiles(x86)}\\Microsoft Visual Studio\\$version")) {
            if (-not (Test-Path $root)) { continue }
            $edition = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                Where-Object { Test-Path (Join-Path $_.FullName "VC\\Auxiliary\\Build\\vcvarsall.bat") } |
                Select-Object -First 1
            if ($edition) { return $edition.FullName }
        }
    }
    return $null
}

function Resolve-Compiler {
    param(
        [Parameter(Mandatory)] [string] $VsInstall,
        [switch] $ClangCl,
        [switch] $Msvc,
        [string] $Arch
    )
    $clangClExe = $null
    $clExe = $null

    if ($ClangCl) {
        if ($env:ORCA_CLANG_CL) {
            if (-not (Test-Path $env:ORCA_CLANG_CL)) { throw "ORCA_CLANG_CL points at '$env:ORCA_CLANG_CL', which does not exist." }
            $clangClExe = (Resolve-Path $env:ORCA_CLANG_CL).Path
        } else {
            $clangCmd = Get-Command clang-cl -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($clangCmd) { $clangClExe = $clangCmd.Source }
            else {
                $vsClang = Join-Path $VsInstall "VC\Tools\Llvm\x64\bin\clang-cl.exe"
                if (Test-Path $vsClang) { $clangClExe = $vsClang }
            }
        }
        if (-not $clangClExe) { throw "clang-cl was not found. Install LLVM and add it to PATH, set ORCA_CLANG_CL, or install the C++ Clang tools for Windows component in Visual Studio." }
    }

    if ($Msvc) {
        $hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
        $targetArch = if ($Arch -eq "ARM64") { "arm64" } else { "x64" }
        $vsCl = Get-ChildItem -Path (Join-Path $VsInstall "VC\Tools\MSVC\*\\bin\\Host$hostArch\$targetArch\cl.exe") -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($vsCl) { $clExe = $vsCl.FullName }
        else {
            $clCmd = Get-Command cl -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($clCmd) { $clExe = $clCmd.Source }
        }
        if (-not $clExe) { throw "cl.exe was not found. Install Visual Studio with the C++ workload, or point -ClangPath/ORCA_CLANG_CL at clang-cl.exe." }
    }

    [pscustomobject]@{ ClangClExe = $clangClExe; ClExe = $clExe }
}

function Download-CmakeFromGitHub {
    # Official CMake not on PATH after stripping Strawberry's bin dirs.
    # Strawberry's cmake has a broken FindIconv.cmake that fails slicer configure.
    Write-Warning "Official CMake not found on PATH. Downloading CMake from GitHub releases..."
    $cmakeVersion = "3.29.3"
    $cmakeDir = Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }) "CMake-$cmakeVersion"
    $cmakeZip = Join-Path $env:TEMP "cmake-$cmakeVersion-windows-x86_64.zip"
    $cmakeUrl = "https://github.com/Kitware/CMake/releases/download/v$cmakeVersion/cmake-$cmakeVersion-windows-x86_64.zip"
    if (-not (Test-Path $cmakeDir)) {
        Invoke-WebRequest -Uri $cmakeUrl -OutFile $cmakeZip -TimeoutSec 300
        Expand-Archive -Path $cmakeZip -DestinationPath $env:TEMP -Force
        Rename-Item -Path (Join-Path $env:TEMP "cmake-$cmakeVersion-windows-x86_64") -NewName $cmakeDir -Force
    }
    $cmake = Join-Path $cmakeDir "bin\cmake.exe"
    if (-not (Test-Path $cmake)) { throw "Downloaded CMake not found at $cmake" }
    Write-Status "cmake" "$cmake (downloaded from GitHub releases)"
    return $cmake
}

function Resolve-Cmake {
    try {
        $cmake = Resolve-Tool "cmake" "ORCA_CMAKE" "Install CMake 3.21 or newer and add it to PATH, or set ORCA_CMAKE."
    } catch {
        $cmake = Download-CmakeFromGitHub
    }

    $cmakeVersion = (& $cmake --version | Select-Object -First 1)
    if ($cmakeVersion -match "cmake version (\d+)\.(\d+)") {
        if ([int]$Matches[1] -lt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -lt 21)) {
            throw "CMake 3.21 or newer is required, found: $cmakeVersion"
        }
    }
    $cmake
}

function Resolve-Toolchain {
    param(
        [switch] $ClangCl,
        [switch] $Msvc,
        [switch] $Ninja,
        [string] $Cache,
        [string] $Arch,
        [string] $CcachePath
    )

    $vsInstall = Find-VsInstall
    if (-not $vsInstall) { throw "Visual Studio 2022 or newer with the C++ toolset was not found. Install the 'Desktop development with C++' workload." }

    $compiler = Resolve-Compiler -VsInstall $vsInstall -ClangCl:$ClangCl -Msvc:$Msvc -Arch $Arch
    $cmake = Resolve-Cmake

    $ninjaExe = $null
    if ($Ninja) {
        $ninjaExe = Resolve-Tool "ninja" "ORCA_NINJA" "Install Ninja and add it to PATH, or set ORCA_NINJA."
    }

    $ccacheExe = $null
    if ($Cache -and $Cache -ne "off") {
        if ($env:ORCA_CCACHE) {
            if (-not (Test-Path $env:ORCA_CCACHE)) { throw "ORCA_CCACHE points at '$env:ORCA_CCACHE', which does not exist." }
            $ccacheExe = (Resolve-Path $env:ORCA_CCACHE).Path
        } elseif ($CcachePath) {
            $ccacheExe = $CcachePath
        } else {
            $ccacheCmd = Get-Command ccache -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($ccacheCmd) { $ccacheExe = $ccacheCmd.Source }
        }
        if (-not $ccacheExe) { throw "ccache was not found. Install it, add it to PATH, or set ORCA_CCACHE." }
        Write-Status "ccache" $ccacheExe
    }

    Write-Status "clang-cl" $compiler.ClangClExe
    Write-Status "cmake"    $cmake
    Write-Status "ninja"    $ninjaExe
    if ($ccacheExe) { Write-Status "ccache" $ccacheExe }

    [pscustomobject]@{
        VsInstall   = $vsInstall
        ClangClExe  = $compiler.ClangClExe
        ClExe       = $compiler.ClExe
        Cmake       = $cmake
        NinjaExe    = $ninjaExe
        CcacheExe   = $ccacheExe
    }
}