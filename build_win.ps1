<#
.SYNOPSIS
    Build OrcaSlicer on Windows with clang-cl + Ninja (+ optional ccache).
.DESCRIPTION
    Run with -Help for a one-page reference of every option.
#>

param(
    [ValidateSet("x64-clang", "x64-clang-cache", "x64-clang-debug")]
    [string]$Preset,
    [string]$Config = "Release",
    [string]$Target,
    [switch]$Clean,
    [switch]$CleanDeps,
    [switch]$UnityTestLibslic3r,
    [switch]$NoMetrics,
    [switch]$PackDeps,
    [switch]$SkipDeps,
    [ValidateRange(1, 10)]
    [int]$MetricsInterval = 15,
    [ValidateRange(1, 64)]
    [int]$MaxJobs = 0,
    [switch]$Help,
    [switch]$Deps,
    [switch]$Slicer,
    [switch]$Tests,
    [switch]$RunTests,
    [switch]$InstallDeps,
    [string]$InstallVs,
    [switch]$KillJobs,
    [switch]$ClangCl,
    [switch]$Ninja,
    [switch]$Msvc,
    [switch]$Msbuild,
    [string]$Vs,
    [string]$ClangPath,
    [string]$Cache,
    [switch]$NoPch,
    [switch]$SlicerAsan,
    [switch]$NoConfigure,
    [switch]$NoGettext,
    [switch]$InstallSlicer,
    [string]$SlicerTarget,
    [string]$DepsTarget,
    [string]$DepsDir,
    [string]$SlicerDir,
    [string]$DepsArgs,
    [string]$SlicerArgs,
    [string]$Arch,
    [switch]$Verbose,
    [switch]$DryRun
)

# --- Actions (mirror build_win.bat) ---
# -d        build dependencies
# -s        build slicer
# --tests   build unit tests
# --run-tests build and run tests
# -u        install deps via winget
# --install-vs buildtools or ide
# -k        kill compiler/linker processes

# --- Toolchain ---
# -l        use clang-cl (default when -x given)
# -x        use Ninja Multi-Config
# --msvc    use cl.exe
# --msbuild use Visual Studio generator
# --vs      2019, 2022, 2026
# --clang-path path to clang-cl.exe
# --cache   ccache, sccache, off
# --no-pch  disable precompiled header

# --- Build scope ---
# -a        ASAN build
# --no-configure skip CMake configure
# --no-gettext skip translation regeneration
# -i        install slicer into tree
# --slicer-target one target name
# --deps-target one dep target name
# --deps-dir dependency tree path
# --build-dir slicer build tree path
# --deps-args extra CMake args for deps
# --slicer-args extra CMake args for slicer

# --- Tuning ---
# --arch    x64 or arm64
# -v        show compiler command lines
# -D        dry run, print commands only

if ($Help) {
    @'
build_win.ps1 - Build OrcaSlicer on Windows (clang-cl + Ninja + optional ccache)

USAGE
  .\build_win.ps1 [options]
  .\build_win.ps1 -Help

OPTIONS
  -Preset <name>
      Which build tree to use.
        x64-clang        clang-cl, no ccache          (build-clang)
        x64-clang-cache  ccache, unity build          (build-clang-cache)
        x64-clang-debug  debug configuration tree     (build-dbg-clang)
      Omit it and the script picks x64-clang-cache when ccache is installed and
      its dependency tree is complete, otherwise x64-clang.

  -Config <name>
      CMake configuration to build. Default: Release.

  -Target <name>
      Build one target instead of everything, for example libslic3r.
      Default: all.

  -Clean
      Delete the active build directory before configuring, so it is set up
      from scratch. Dependencies are left alone.

  -CleanDeps
      Delete deps/build-clang and deps/build-clang-cache so the next build
      rebuilds every dependency. Slow; pair it with -Clean for a full reset.

  -UnityTestLibslic3r
      Force x64-clang-cache and build only libslic3r, using the unity build for
      fast test-and-fix cycles.

  -NoMetrics
      Skip the CPU / cores / RAM sampler and the resource chart afterwards.

  -PackDeps
      Check and build the dependencies, write deps/OrcaSlicer_dep.zip, then
      exit without building OrcaSlicer.

  -SkipDeps
      Trust whatever is already in deps/ and skip the dependency check.

  -MetricsInterval <n>
      Seconds between sampler rows (1-10). Default: 15, or 3 for the
      x64-clang-cache preset.

  -MaxJobs <n>
      Cap parallel compile jobs (1-64). Default: 0, meaning auto.

  -Help
      Print this page and exit.

ENVIRONMENT
  ORCA_CLANG_CL   Path to clang-cl.exe (otherwise PATH or the VS LLVM bundle).
  ORCA_CMAKE      Path to cmake.exe (otherwise PATH).
  ORCA_NINJA      Path to ninja.exe (otherwise PATH).
  ORCA_CCACHE     Path to ccache.exe (otherwise PATH).

EXAMPLES
  .\build_win.ps1
      Build everything with the best preset available on this machine.

  .\build_win.ps1 -Preset x64-clang-cache -Target libslic3r
      Rebuild only the slicing library through ccache.

  .\build_win.ps1 -CleanDeps -PackDeps
      Rebuild the dependencies from scratch and pack them into a zip.

  .\build_win.ps1 -NoMetrics -MaxJobs 8
      Same build without the resource monitor, eight jobs at a time.
'@ | Write-Host
    exit 0
}

# --- Derive toolchain from individual flags (mirror build_win.bat) ---
# When Preset is given, translate it to flags. Otherwise flags stand alone.
if ($Preset) {
    switch ($Preset) {
        "x64-clang"       { $ClangCl = $true; $Ninja = $true; $Arch = "x64" }
        "x64-clang-cache" { $ClangCl = $true; $Ninja = $true; $Cache = "ccache"; $Arch = "x64" }
        "x64-clang-debug" { $ClangCl = $true; $Ninja = $true; $Config = "Debug"; $Arch = "x64" }
    }
}

# Defaults
if (-not $Arch) { $Arch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "ARM64" } else { "x64" } }
if (-not $Config) { $Config = "Release" }
# Remember whether the caller pinned the dependency tree: the default has to be
# the per-(compiler, arch) tree deps actually build into, deps/<depName>, which is
# only known after the configuration is resolved below.
$DepsDirExplicit = [bool]$DepsDir
if (-not $DepsDir) { $DepsDir = "deps" }

# Resolve actions: if none specified but Tests/RunTests given, build slicer
$actions = @($Deps, $Slicer, $PackDeps, $InstallDeps, $InstallVs) | Where-Object { $_ }
if (-not $actions) {
    if ($Tests) { $Slicer = $true }
    if ($RunTests) { $Slicer = $true }
}
if (-not $actions) {
    Write-Host "Nothing to do. Pick an action: -d, -s, -p or -u. Run -h for the full list." -ForegroundColor Yellow
    exit 1
}

# --- Validation ---
if ($ClangCl -and $Msvc) {
    throw "--clang-cl and --msvc select different compilers."
}
if ($Ninja -and $Msbuild) {
    throw "--ninja and --msbuild select different generators."
}
if ($ClangPath -and -not $Ninja) {
    throw "--clang-path needs the Ninja generator; add -x."
}

# clang-cl implies no PCH
if ($ClangCl) { $NoPch = $true }

# Cache only valid with clang-cl + Ninja
if ($Cache -and $Cache -ne "off") {
    if (-not $Ninja -or -not $ClangCl) {
        throw "--cache needs clang-cl and Ninja; add -l -x."
    }
}

# Default generator: Ninja unless Msbuild
if (-not $Ninja -and -not $Msbuild) {
    $Ninja = $true
}

# Default compiler: clang-cl when Ninja, MSVC when Msbuild
if (-not $ClangCl -and -not $Msvc) {
    if ($Ninja) { $ClangCl = $true }
    else { $Msvc = $true }
}

# --vs only meaningful with MSBuild
if ($Vs -and -not $Msbuild) {
    Write-Host "Warning: --vs is only used with the Visual Studio generator (--msbuild)." -ForegroundColor Yellow
}

# Display preset name (derived from flags when Preset not given)
$displayPreset = $Preset

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$UnitsDir = Join-Path $RepoRoot "scripts\build_win_steps"

. (Join-Path $UnitsDir "native.ps1")
. (Join-Path $UnitsDir "toolchain.ps1")
. (Join-Path $UnitsDir "vsenv.ps1")
. (Join-Path $UnitsDir "deps.ps1")
. (Join-Path $UnitsDir "metrics.ps1")
. (Join-Path $UnitsDir "build.ps1")
. (Join-Path $UnitsDir "ccache_stats.ps1")

function Write-Status {
    param(
        [string]$Tag,
        [string]$Text,
        [string]$Color = "DarkGray"
    )
    Write-Host ("  {0,-9} {1}" -f "[$Tag]", $Text) -ForegroundColor $Color
}

# Locate PowerShell 7 for PwshSpectreConsole rich output (Python 'rich' is forbidden).
$Pwsh7 = Join-Path $env:LOCALAPPDATA "opencode\pwsh7\pwsh.exe"
if (-not (Test-Path $Pwsh7)) {
    $Pwsh7 = $null
    Write-Status "pwsh7" "PowerShell 7 not found; falling back to plain output." "Yellow"
}
$PwshRenderScript = Join-Path $UnitsDir "render.ps1"

$started = Get-Date
$global:BuildStarted = $started
$metricsJob = $null
$metricsCsv = $null

$configureStr = "-"
$buildStr     = "-"
$totalStr     = "-"
$buildSucceeded = $false

Push-Location $RepoRoot

try {
    function Get-TimeStamp {
        $span = (Get-Date) - $global:BuildStarted
        $h = [math]::Floor($span.TotalHours); $m = [math]::Floor($span.TotalMinutes) % 60; $s = [math]::Round($span.Seconds)
        return "{0}:{1}:{2}" -f $h, $m, $s
    }

    function Write-Step {
        param([string]$Text)
        $ts = Get-TimeStamp
        Write-Host "[$ts] $Text"
    }

    # Spectre output has to be produced by PowerShell 7, then replayed through
    # Write-Host: PowerShell 5.1 decodes native stdout with the console code page,
    # which mangles the box-drawing glyphs unless it is switched to UTF-8 here.
    function Invoke-SpectreHelper {
        param([string]$Action, [object[]]$Arguments = @())
        if (-not $Pwsh7) { return $false }
        $previousEncoding = [Console]::OutputEncoding
        try {
            [Console]::OutputEncoding = [Text.Encoding]::UTF8
            & $Pwsh7 -NoProfile -File $PwshRenderScript $Action @Arguments 2>&1 | ForEach-Object { Write-Host $_ }
        } finally {
            [Console]::OutputEncoding = $previousEncoding
        }
        return $true
    }

    function Show-BuildMetrics {
        param([string]$CsvPath, [string]$Preset, [string]$Config)
        if (-not $CsvPath -or -not (Test-Path $CsvPath)) { return }
        if (-not (Invoke-SpectreHelper -Action "chart" -Arguments @($CsvPath))) {
            Write-Status "metrics" "PowerShell 7 not found; skipping metrics chart." "Yellow"
        }
    }

    $fmt = {
        param($t)
        $h = [math]::Floor($t.TotalHours)
        $m = [math]::Floor($t.TotalMinutes) % 60
        $s = [math]::Round($t.Seconds)
        if ($h) { "\{0\}:\{1:00\}:\{2:00\}" -f $h, $m, $s }
        elseif ($m) { "{0}m {1:00}s" -f $m, $s }
        else { "{0}s" -f $s }
    }

    # --- 1. Locate Visual Studio and the toolchain ---
    Write-Step "[1/7] Locating Visual Studio and tools..."

    $vsInstall = Find-VsInstall
    if (-not $vsInstall) {
        throw "Visual Studio 2022 or newer with the C++ toolset was not found. Install the 'Desktop development with C++' workload."
    }
    Write-Status "vs"      $vsInstall

    # Resolve compiler
    $clangClExe = $null
    $clExe   = $null
    if ($ClangCl) {
        if ($env:ORCA_CLANG_CL) {
            if (-not (Test-Path $env:ORCA_CLANG_CL)) { throw "ORCA_CLANG_CL points at '$env:ORCA_CLANG_CL', which does not exist." }
            $clangClExe = (Resolve-Path $env:ORCA_CLANG_CL).Path
        } else {
            $clangCmd = Get-Command clang-cl -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($clangCmd) {
                $clangClExe = $clangCmd.Source
            } else {
                $vsClang = Join-Path $vsInstall "VC\Tools\Llvm\x64\bin\clang-cl.exe"
                if (Test-Path $vsClang) { $clangClExe = $vsClang }
            }
        }
        if (-not $clangClExe) {
            throw "clang-cl was not found. Install LLVM and add it to PATH, set ORCA_CLANG_CL, or install the C++ Clang tools for Windows component in Visual Studio."
        }
        Write-Status "clang-cl" $clangClExe
    }
    if ($Msvc) {
        # Prefer the toolset belonging to the VS that vswhere selected. A stale x86
        # entry on PATH otherwise wins and CMake happily configures a 32-bit compiler.
        # Join-Path does not glob, so resolve the toolset with Get-ChildItem. The host
        # arch is the machine running the build, not the target.
        $hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
        $targetArch = if ($Arch -eq "ARM64") { "arm64" } else { "x64" }
        $vsCl = Get-ChildItem -Path (Join-Path $vsInstall "VC\Tools\MSVC\*\bin\Host$hostArch\$targetArch\cl.exe") -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($vsCl) {
            $clExe = $vsCl.FullName
        } else {
            $clCmd = Get-Command cl -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($clCmd) { $clExe = $clCmd.Source }
        }
        if (-not $clExe) {
            throw "cl.exe was not found. Install Visual Studio with the C++ workload, or set --msvc with a valid VS installation."
        }
        Write-Status "cl" $clExe
    }

    # Strawberry Perl's c\bin\cmake.exe shadows the official CMake on PATH (Strawberry
    # is prepended to PATH by the CI workflow). Strawberry's cmake has a broken
    # FindIconv.cmake that fails the slicer CMake configure (missing Iconv_INCLUDE_DIR)
    # and tries to download "uv" over SSL. Strip only Strawberry's c\bin from PATH
    # (where cmake.exe lives) but keep c\perl\bin (needed for Perl scripts in dep
    # builds like OpenSSL configure).
    $env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*Strawberry\c\bin' }) -join ';'

    try {
        $cmake = Resolve-Tool "cmake" "ORCA_CMAKE" "Install CMake 3.21 or newer and add it to PATH, or set ORCA_CMAKE."
    } catch {
        # Official CMake not on PATH after stripping Strawberry's bin dirs. Download it
        # from GitHub releases so the slicer build has a working CMake (Strawberry's
        # cmake has a broken FindIconv.cmake that fails the slicer configure).
        Write-Warning "Official CMake not found on PATH. Downloading CMake from GitHub releases..."
        $cmakeVersion = "3.29.3"
        $cmakeDir = Join-Path $env:RUNNER_TEMP "CMake-$cmakeVersion"
        $cmakeZip = Join-Path $env:TEMP "cmake-$cmakeVersion-windows-x86_64.zip"
        $cmakeUrl = "https://github.com/Kitware/CMake/releases/download/v$cmakeVersion/cmake-$cmakeVersion-windows-x86_64.zip"
        if (-not (Test-Path $cmakeDir)) {
            Invoke-WebRequest -Uri $cmakeUrl -OutFile $cmakeZip -TimeoutSec 300
            Expand-Archive -Path $cmakeZip -DestinationPath $env:TEMP -Force
            Rename-Item -Path (Join-Path $env:TEMP "cmake-$cmakeVersion-windows-x86_64") -NewName $cmakeDir -Force
        }
        $cmake = Join-Path $cmakeDir "bin\cmake.exe"
        if (-not (Test-Path $cmake)) {
            throw "Downloaded CMake not found at $cmake"
        }
        Write-Status "cmake" "$cmake (downloaded from GitHub releases)"
    }
    if ($Ninja) {
        $ninjaExe = Resolve-Tool "ninja" "ORCA_NINJA" "Install Ninja and add it to PATH, or set ORCA_NINJA."
    } else {
        $ninjaExe = $null
    }
    $ccacheExe = $null
    if ($Cache -and $Cache -ne "off") {
        if ($env:ORCA_CCACHE) {
            if (-not (Test-Path $env:ORCA_CCACHE)) { throw "ORCA_CCACHE points at '$env:ORCA_CCACHE', which does not exist." }
            $ccacheExe = (Resolve-Path $env:ORCA_CCACHE).Path
        } else {
            $ccacheCmd = Get-Command ccache -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($ccacheCmd) { $ccacheExe = $ccacheCmd.Source }
        }
        if (-not $ccacheExe) { throw "ccache was not found. Install it, add it to PATH, or set ORCA_CCACHE." }
        Write-Status "ccache" $ccacheExe
    }

    $cmakeVersion = (& $cmake --version | Select-Object -First 1)
    if ($cmakeVersion -match "cmake version (\d+)\.(\d+)") {
        if ([int]$Matches[1] -lt 3 -or ([int]$Matches[1] -eq 3 -and [int]$Matches[2] -lt 21)) {
            throw "CMake 3.21 or newer is required, found: $cmakeVersion"
        }
    }

    Write-Status "clang-cl" $clangClExe
    Write-Status "cmake"    $cmake
    Write-Status "ninja"    $ninjaExe
    if ($ccacheExe) { Write-Status "ccache" $ccacheExe }

    # --- 2. Choose build tree and target ---
    Write-Step "[2/7] Determining build configuration..."

    # Build tree names per configuration (mirrors build_win.bat cfg_dir_*)
    $configDirs = @{
        "release"         = "build"
        "debug"           = "build-dbg"
        "relwithdebinfo"  = "build-dbginfo"
        "minsizerel"      = "build-minsize"
    }
    # Dependency trees are shared across configurations and mirror build_win.bat's
    # cfg_dep_* table: the deps project is configured for a multi-config generator,
    # so CMAKE_BUILD_TYPE is ignored, DEP_DEBUG stays OFF and every configuration
    # is served by the same libraries. Only Debug keeps its own tree. Splitting the
    # tree per configuration made .ps1 rebuild dependencies that .bat reuses.
    $depDirs = @{
        "release"         = "build"
        "debug"           = "build-dbg"
        "relwithdebinfo"  = "build"
        "minsizerel"      = "build"
    }
    $configLower = $Config.ToLower()
    if (-not $configDirs.ContainsKey($configLower)) {
        throw "Unknown configuration '$Config'. Known configurations: release, debug, relwithdebinfo, minsizerel."
    }
    $treeName = $configDirs[$configLower]
    $depName  = $depDirs[$configLower]

    # Suffixes for compiler and architecture
    if ($ClangCl) { $treeName += "-clang"; $depName += "-clang" }
    if ($Arch -eq "ARM64") { $treeName += "-arm64"; $depName += "-arm64" }

    $buildDir = $treeName
    if ($SlicerDir) { $buildDir = $SlicerDir }

    # Unity test preset forces clang-cl + cache
    if ($UnityTestLibslic3r) {
        $ClangCl = $true; $Ninja = $true; $Cache = "ccache"
        if (-not $Target) { $Target = "libslic3r" }
    }

    if (-not $Target) { $Target = "all" }

    # Cache preset name for display (from flags)
    $displayPreset = if ($Preset) { $Preset }
                     elseif ($ClangCl -and $Ninja -and $Cache -eq "ccache") { "x64-clang-cache" }
                     elseif ($ClangCl -and $Ninja) { "x64-clang" }
                     elseif ($Msvc -and $Ninja) { "x64-msvc" }
                     elseif ($Msvc -and $Msbuild) { "x64-msbuild" }
                     else { "custom" }

    if ($Cache -eq "ccache" -and -not $ccacheExe) {
        throw "ccache is required for cache builds. Install it, add it to PATH, or set ORCA_CCACHE."
    }

    Write-Status "preset"  "$displayPreset ($buildDir)"
    Write-Status "target"  "$Target ($Config)"
    Write-Status "arch"    $Arch
    Write-Status "compiler" $(if ($ClangCl) { "clang-cl" } else { "cl" })
    Write-Status "generator" $(if ($Ninja) { "Ninja" } else { "MSBuild" })
    Write-Status "jobs"    $(if ($MaxJobs -gt 0) { "$MaxJobs" } else { "auto" })

    if ($CleanDeps) {
        foreach ($d in @("deps\build-clang", "deps\build-clang-cache")) {
            if (Test-Path $d) {
                Write-Status "clean" "Removing $d"
                Remove-Item $d -Recurse -Force
            }
        }
    }
    if ($Clean -and (Test-Path $buildDir)) {
        Write-Status "clean" "Removing $buildDir"
        Remove-Item $buildDir -Recurse -Force
    }

    # --- 3. Capture the Visual Studio environment ---
    Write-Step "[3/7] Setting up the Visual Studio environment..."
    Capture-VsEnvironment -VsInstall $vsInstall -Arch $Arch

    $toolDirs = @($clangClExe, $clExe, $cmake, $ninjaExe, $ccacheExe) |
        Where-Object { $_ } |
        ForEach-Object { Split-Path -Parent $_ } |
        Select-Object -Unique
    $env:PATH = ($toolDirs -join ";") + ";" + $env:PATH
    Write-Status "env" "VS $Arch environment captured, tool dirs prepended to PATH"

    # --- 4. Start metrics monitor ---
    if (-not $NoMetrics) {
        Write-Step "[4/7] Starting build-process resource monitor..."
        $metricsCsv = Join-Path $env:TEMP "orca-build-metrics-$PID.csv"
        Remove-Item $metricsCsv -ErrorAction SilentlyContinue
        Write-Status "sampler" "Every ${MetricsInterval}s (polling every 2s)"
        try {
            $metricsJob = Start-BuildMonitor -CsvPath $metricsCsv -IntervalSeconds $MetricsInterval
            Write-Status "sampler" "Job started (id $($metricsJob.Id))" "DarkGray"
        } catch {
            Write-Status "sampler" "Unavailable: $($_.Exception.Message)" "Yellow"
            $metricsJob = $null
        }
    } else {
        Write-Step "[4/7] Resource monitoring disabled (-NoMetrics)"
    }

    # --- 5. Ensure dependencies are built ---
    if ($SkipDeps) {
        Write-Step "[5/7] Skipping dependencies (-SkipDeps)"
    } else {
        Write-Step "[5/7] Checking dependencies..."
        # Deps tree name mirrors build tree naming (minus arch suffix for shared deps)
        $depTreeName = $depName
        $depsBuildDir = Join-Path "deps" $depTreeName
        $depsCompiler = if ($ClangCl) { $clangClExe } else { $clExe }
        Check-Dependencies -Cmake $cmake -Compiler $depsCompiler -Config $Config -DepsBuildDir $depsBuildDir

        if ($PackDeps) {
            Write-Step "[5.1/7] Packing dependencies..."
            $zipPath = Pack-Dependencies -DepsBuildDir $depsBuildDir -OutputPath (Join-Path $RepoRoot "deps\OrcaSlicer_dep.zip")
            Write-Host ""
            if (Invoke-SpectreHelper -Action "rule") {
                [void](Invoke-SpectreHelper -Action "step" -Arguments @("", "Dependencies packed successfully!"))
                [void](Invoke-SpectreHelper -Action "step" -Arguments @("", "Archive: $zipPath"))
                [void](Invoke-SpectreHelper -Action "rule")
            } else {
                Write-Host "==========================================" -ForegroundColor Green
                Write-Host "  Dependencies packed successfully!"      -ForegroundColor Green
                Write-Host "  Archive: $zipPath"
                Write-Host "==========================================" -ForegroundColor Green
            }
            return
        }
    }

    $timing = $null
    if ($Slicer) {
        # --- 6. Configure and build ---
        Write-Step "[6/7] Configuring and building..."
        $generator = if ($Msbuild) { "Visual Studio 17 2022" } else { "Ninja Multi-Config" }
        $compiler = if ($ClangCl) { $clangClExe } else { $clExe }
        $cmakeArgs = @()
        if ($Cache -eq "ccache") {
            $cmakeArgs += "-DSLIC3R_UNITY_BUILD=ON"
        }
        if ($Tests) {
            $cmakeArgs += "-DBUILD_TESTS=ON"
        }
        # Enable ORCA_TOOLS to build generate_system_cache.exe for preset cache generation
        $cmakeArgs += "-DORCA_TOOLS=ON"
        # MSBuild + ARM64 needs -A arch
        if ($Msbuild -and $Arch -eq "ARM64") {
            $cmakeArgs += "-A", "ARM64"
        }
        # Ninja + clang-cl under VS generator needs ClangCL toolset
        if (-not $Ninja -and $ClangCl) {
            $cmakeArgs += "-T", "ClangCL"
        }
        # Point CMake at the deps install dir so find_package(JPEG) etc. find
        # libjpeg-turbo (with JCS_EXT_RGBA) from deps instead of system libjpeg.
        # Strawberry Perl's c\include is added to the compiler include path and
        # contains an older jpeglib.h (no JCS_EXT_RGBA) that wins over deps'
        # libjpeg-turbo due to include-path ordering. Explicitly pin JPEG to deps.
        # Without an explicit -DepsDir the tree is deps/<depName>, not deps/.
        $depsInstallRoot = if ($DepsDirExplicit) { $DepsDir } else { Join-Path "deps" $depName }
        $depsInstallDir = Join-Path $depsInstallRoot "OrcaSlicer_dep/usr/local"
        if (Test-Path $depsInstallDir -PathType Container) {
            # CMakeLists.txt force-derives CMAKE_PREFIX_PATH from DEP_BUILD_DIR, so
            # pinning CMAKE_PREFIX_PATH alone is ignored. Pass the dependency tree
            # explicitly whenever it is not the default deps/<build dir>, as
            # build_win.bat does, so a tree shared by several configurations resolves.
            $depsRootFull = (Resolve-Path -LiteralPath $depsInstallRoot).Path
            $depsInstallDir = Join-Path $depsRootFull "OrcaSlicer_dep/usr/local"
            $cmakeArgs += "-DDEP_BUILD_DIR=$($depsRootFull.Replace('\', '/'))"
            $cmakeArgs += "-DCMAKE_PREFIX_PATH=$depsInstallDir"
            $cmakeArgs += "-DJPEG_INCLUDE_DIR=$depsInstallDir/include"
            $jpegLib = Join-Path $depsInstallDir "lib/jpeg.lib"
            if (Test-Path $jpegLib -PathType Leaf) {
                $cmakeArgs += "-DJPEG_LIBRARY=$jpegLib"
            }
        }
        # Strip Strawberry Perl's MinGW include dir from INCLUDE env var so it
        # can't shadow deps' libjpeg-turbo headers during compilation.
        if ($env:INCLUDE) {
            $env:INCLUDE = ($env:INCLUDE -split ';' | Where-Object { $_ -notlike '*Strawberry*' }) -join ';'
        }
        # Strawberry Perl is needed for cmake.exe (used by Configure-Build), but
        # its c\include dir contains an older jpeglib.h (no JCS_EXT_RGBA) that
        # shadows deps' libjpeg-turbo. Delete only c\include so cmake.exe keeps
        # working but CMake can't find the stale JPEG header.
        $strawberryInclude = Join-Path $env:RUNNER_TEMP 'Strawberry\c\include'
        if (Test-Path $strawberryInclude -PathType Container) {
            Remove-Item $strawberryInclude -Recurse -Force -ErrorAction SilentlyContinue
        }
        $timing = Configure-Build -Cmake $cmake -BuildDir $buildDir -Config $Config -Target $Target `
                                  -UnityTest:$UnityTestLibslic3r -MaxJobs $MaxJobs `
                                  -Generator $generator -Compiler $compiler -CMakeArgs $cmakeArgs

        # --- 7. Build summary ---
        Write-Host ""
        Write-Host ""
        Write-Step "[7/7] Build finished"

        $ccacheLines = @(Show-CcacheStats -Ccache $ccacheExe -NoMetrics:$NoMetrics)
        if ($ccacheLines.Count -gt 0) {
            $ccacheData = Join-Path $env:TEMP "orca-build-ccache.txt"
            $ccacheLines | Set-Content -LiteralPath $ccacheData -Encoding UTF8
            try {
                if (-not (Invoke-SpectreHelper -Action "textpanel" -Arguments @($ccacheData, "Ccache statistics"))) {
                    $ccacheLines | ForEach-Object { Write-Host $_ }
                }
            } finally {
                Remove-Item -LiteralPath $ccacheData -ErrorAction SilentlyContinue
            }
        }

        # --- 8. Resource chart ---
        if ($metricsJob) {
            Stop-Job   $metricsJob -ErrorAction SilentlyContinue
            Remove-Job $metricsJob -Force -ErrorAction SilentlyContinue
            $metricsJob = $null
        }
        Show-BuildMetrics -CsvPath $metricsCsv -Preset $displayPreset -Config $Config

        # --- 9. Success summary ---
        $total = $timing.ConfigureTime + $timing.BuildTime
        $configureStr = & $fmt $timing.ConfigureTime
        $buildStr     = & $fmt $timing.BuildTime
        $totalStr     = & $fmt $total

        $summaryData = Join-Path $env:TEMP "orca-build-summary.txt"
        @($totalStr, $configureStr, $buildStr) | Set-Content -LiteralPath $summaryData -Encoding ASCII
        try {
            $rendered = Invoke-SpectreHelper -Action "panel" -Arguments @($summaryData)
        } finally {
            Remove-Item -LiteralPath $summaryData -ErrorAction SilentlyContinue
        }
        if (-not $rendered) {
            Write-Host ("  Total:     {0}" -f $totalStr)
            Write-Host ("  Configure: {0}" -f $configureStr)
            Write-Host ("  Build:     {0}" -f $buildStr)
        }

        $buildSucceeded = $true
    }
} catch {
    if ($metricsJob) {
        Stop-Job   $metricsJob -ErrorAction SilentlyContinue
        Remove-Job $metricsJob -Force -ErrorAction SilentlyContinue
        $metricsJob = $null
    }
    Show-BuildMetrics -CsvPath $metricsCsv -Preset $displayPreset -Config $Config

    Write-Host ""
    Write-Host "BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo) {
        Write-Host ("COMMAND: " + $_.InvocationInfo.MyCommand) -ForegroundColor DarkRed
        Write-Host ("INVOKED AS: " + $_.InvocationInfo.InvocationName) -ForegroundColor DarkRed
        Write-Host ("AT LINE: " + $_.InvocationInfo.ScriptLineNumber + " -> " + "$($_.InvocationInfo.Line)".Trim()) -ForegroundColor DarkRed
        Write-Host ("EXCEPTION: " + $_.Exception.GetType().FullName) -ForegroundColor DarkRed
    }
    if ($_.ScriptStackTrace) {
        Write-Host $_.ScriptStackTrace -ForegroundColor DarkRed
    }
    exit 1
} finally {
    if ($metricsJob) {
        Stop-Job   $metricsJob -ErrorAction SilentlyContinue
        Remove-Job $metricsJob -Force -ErrorAction SilentlyContinue
        $metricsJob = $null
    }
    Pop-Location
}

exit 0



