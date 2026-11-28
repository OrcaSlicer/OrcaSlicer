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
    [switch]$NoMetrics,
    [switch]$PackDeps,
    [switch]$SkipDeps,
    [ValidateRange(1, 10)]
    [int]$MetricsInterval = 15,
    [ValidateRange(1, 64)]
    [int]$MaxJobs = 0,
    [switch]$Help,
    [switch]$Deps = $true,
    [switch]$Slicer = $true,
    [switch]$Tests = $true,
    [switch]$ClangCl = $true,
    [switch]$Ninja = $true,
    [switch]$Msvc,
    [switch]$Msbuild,
    [string]$ClangPath,
    [string]$Cache,
    [switch]$NoPch,
    [switch]$NoConfigure,
    [switch]$InstallSlicer,
    [switch]$Launch,
    [switch]$LaunchWait,
    [string]$DepsDir,
    [string]$SlicerDir,
    [string]$Arch
)

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
        x64-clang-cache  clang-cl + ccache            (build-clang)
        x64-clang-debug  debug configuration          (build-dbg-clang)
      Omit it and ccache is enabled automatically when it is on PATH.

  -Config <name>
      CMake configuration to build. Default: Release.

  -Target <name>
      Build one target instead of everything, for example libslic3r.
      Default: all.

  -Clean
      Delete the active build directory before configuring, so it is set up
      from scratch. Dependencies are left alone. (default: off)

  -CleanDeps
      Delete the dependency tree (deps/build, or deps/build-arm64 on ARM64) so
      the next build rebuilds every dependency. Slow; pair it with -Clean. (default: off)

  -NoMetrics
      Skip the CPU / cores / RAM sampler and the resource chart afterwards. (default: off)

  -PackDeps
      Check and build the dependencies, write deps/OrcaSlicer_dep.zip, then
      exit without building OrcaSlicer. (default: off)

  -SkipDeps
      Trust whatever is already in deps/ and skip the dependency check. (default: off)

  -NoPch
      Configure with SLIC3R_PCH=OFF. (default: off)

  -NoConfigure
      Build the existing tree without re-running CMake configure. (default: off)

  -InstallSlicer
      Run the install target afterwards, which fills <build>/OrcaSlicer with a
      runnable tree. (default: off)

  -Launch
      Launch OrcaSlicer after successful build. Auto-enables -InstallSlicer. (default: off)
  -LaunchWait
      Wait for OrcaSlicer to exit before returning. Implies -Launch (and -InstallSlicer). (default: off)

  -MetricsInterval <n>
      Seconds between sampler rows (1-10). Default: 15, or 3 for the
      x64-clang-cache preset.

  -MaxJobs <n>
      Cap parallel compile jobs (1-64). Default: 0, meaning auto.

  ACTION SWITCHES (defaults: on)
  -Deps
      Build the dependencies and stop; the dep tree is the only output.
      A bare run builds dependencies, slicer and tests. (default: on)

  -Slicer
      Build OrcaSlicer. (default: on)

  -Tests
      Build unit tests. (default: on)

  TOOLCHAIN SWITCHES
  -ClangCl
      Use clang-cl compiler. (default: on)

  -Ninja
      Use Ninja generator (faster). (default: on)

  -Msvc
      Use MSVC compiler. (default: off)

  -Msbuild
      Use MSBuild generator. (default: off)

  -Cache <name>
      Enable ccache or sccache. (default: auto - enabled if ccache found in PATH)

  -DepsDir <path>
      Override dependency build directory. (default: deps/build or deps/build-arm64)

  -SlicerDir <path>
      Override slicer build directory. (default: build or build-arm64)

  -Arch <name>
      Target architecture. Default: x64 (or ARM64 on ARM64 hardware).

  -Config <name>
      CMake configuration to build. Default: Release.

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

if ($Preset) { switch ($Preset) { "x64-clang"       { $ClangCl = $true; $Ninja = $true; $Arch = "x64" } "x64-clang-cache" { $ClangCl = $true; $Ninja = $true; $Cache = "ccache"; $Arch = "x64" } "x64-clang-debug" { $ClangCl = $true; $Ninja = $true; $Config = "Debug"; $Arch = "x64" } } }

if (-not $Arch) { $Arch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "ARM64" } else { "x64" } }
if (-not $Config) { $Config = "Release" }
if (-not $DepsDir) { $DepsDir = if ($Arch -eq "ARM64") { "deps/build-arm64" } else { "deps/build" } }
$DepsDir = $DepsDir.Replace('\', '/')

$otherAction = $PSBoundParameters.ContainsKey('Slicer') -or $PSBoundParameters.ContainsKey('Tests') -or $PSBoundParameters.ContainsKey('PackDeps')
if ($PSBoundParameters.ContainsKey('Deps') -and -not $otherAction) { $Slicer = $false; $Tests = $false }

if (-not ($Deps -or $Slicer -or $PackDeps)) { Write-Host "Nothing to do. Pick an action: -Deps, -Slicer or -PackDeps. Run -Help for the full list." -ForegroundColor Yellow; exit 1 }

if ($ClangCl -and $Msvc) { throw "-ClangCl and -Msvc select different compilers." }
if ($Ninja -and $Msbuild) { throw "-Ninja and -Msbuild select different generators." }
if ($ClangPath -and -not $Ninja) { throw "-ClangPath needs the Ninja generator; do not combine it with -Msbuild." }

if ($ClangPath) { $env:ORCA_CLANG_CL = $ClangPath }

if (-not $Ninja -and -not $Msbuild) { $Ninja = $true }
if (-not $ClangCl -and -not $Msvc) { if ($Ninja) { $ClangCl = $true } else { $Msvc = $true } }

$displayPreset = $Preset

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$UnitsDir = Join-Path $RepoRoot "scripts\build_win_steps"

. (Join-Path $UnitsDir "build_win_orchestrator.ps1")
. (Join-Path $UnitsDir "build_win_toolchain_resolver.ps1")
. (Join-Path $UnitsDir "build_win_build_layout_resolver.ps1")
. (Join-Path $UnitsDir "build_win_vs_environment_capture.ps1")
. (Join-Path $UnitsDir "build_win_dependency_tasks.ps1")
. (Join-Path $UnitsDir "build_win_cmake_invocation.ps1")
. (Join-Path $UnitsDir "build_win_console_output.ps1")
. (Join-Path $UnitsDir "build_win_slicer_arg_builder.ps1")
if ($LaunchWait) { $Launch = $true }
if ($Launch) { if (-not $InstallSlicer) { Write-Status "auto" "InstallSlicer enabled (required by -Launch/-LaunchWait)"; $InstallSlicer = $true } }
. (Join-Path $UnitsDir "build_win_launch.ps1")

# Resolve ccache BEFORE stripping Strawberry's c\bin from PATH.
# Strawberry bundles ccache but its c\bin also shadows CMake, so we strip it.
$ccachePath = $null
$ccacheCmd = Get-Command ccache -ErrorAction SilentlyContinue | Select-Object -First 1
if ($ccacheCmd) { $ccachePath = $ccacheCmd.Source }
else {
    $strawberryCandidates = @()
    if ($env:STRAWBERRY_HOME) { $strawberryCandidates += Join-Path $env:STRAWBERRY_HOME 'c\bin\ccache.exe' }
    $strawberryCandidates += Join-Path $env:LOCALAPPDATA 'Strawberry\c\bin\ccache.exe'
    $strawberryCandidates += 'C:\Strawberry\c\bin\ccache.exe'
    foreach ($c in $strawberryCandidates) {
        if (Test-Path $c) { $ccachePath = $c; break }
    }
}

# Strawberry's c\bin shadows CMake (broken FindIconv.cmake) — strip it.
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*Strawberry\c\bin' }) -join ';'

# Strawberry home: CI sets STRAWBERRY_HOME; locally use RUNNER_TEMP or TEMP.
$strawberryHome = if ($env:STRAWBERRY_HOME) { $env:STRAWBERRY_HOME } elseif ($env:RUNNER_TEMP) { Join-Path $env:RUNNER_TEMP 'Strawberry' } else { Join-Path $env:TEMP 'Strawberry' }

if (-not $Cache -or $Cache -eq "") {
    if ($ccachePath) {
        $Cache = "ccache"
        Write-Status "ccache" "Auto-enabled (found at $ccachePath)"
    } else {
        Write-Warning "ccache not found in PATH - incremental builds will not be cached. Install with 'winget install ccache' for faster rebuilds."
    }
}

if ($Cache -and $Cache -ne "off") {
    if (-not $Ninja -or -not $ClangCl) { throw "-Cache needs -ClangCl and -Ninja." }
    $ccacheDir = if ($env:CCACHE_DIR) { $env:CCACHE_DIR } else { Join-Path $env:USERPROFILE ".ccache" }
    if (-not (Test-Path $ccacheDir)) { New-Item -ItemType Directory -Force -Path $ccacheDir | Out-Null }
    $env:CCACHE_DIR = $ccacheDir
    Write-Status "ccache" "Cache directory: $ccacheDir"
}

$started = Get-Date; $global:BuildStarted = $started; $buildSucceeded = $false

Push-Location $RepoRoot
try {
    Write-Step "[1/7] Locating Visual Studio and tools..."
    $tools = Resolve-Toolchain -ClangCl $ClangCl -Msvc $Msvc -Ninja $Ninja -Cache $Cache -Arch $Arch -CcachePath $ccachePath
    $vsInstall = $tools.VsInstall; $clangClExe = $tools.ClangClExe; $clExe = $tools.ClExe; $cmake = $tools.Cmake; $ninjaExe = $tools.NinjaExe; $ccacheExe = $tools.CcacheExe
    Write-Status "vs" $vsInstall; if ($clExe) { Write-Status "cl" $clExe }; Write-Status "cmake" $cmake; Write-Status "ninja" $ninjaExe; if ($ccacheExe) { Write-Status "ccache" $ccacheExe }

    Write-Step "[2/7] Determining build configuration..."
    $layout = Resolve-BuildLayout -Config $Config -Arch $Arch -ClangCl:$ClangCl -Ninja:$Ninja -Msvc:$Msvc -Msbuild:$Msbuild -Preset $Preset -Cache $Cache -CcacheExe $CcacheExe -SlicerDir $SlicerDir -Target $Target -CleanDeps:$CleanDeps -Clean:$Clean -MaxJobs $MaxJobs
    $buildDir = $layout.BuildDir; $Target = $layout.Target; $displayPreset = $layout.DisplayPreset

    Write-Step "[3/7] Setting up the Visual Studio environment..."
    Capture-VsEnvironment -VsInstall $vsInstall -Arch $Arch
    @($clangClExe, $clExe, $cmake, $ninjaExe, $ccacheExe) | Where-Object { $_ } | ForEach-Object { Split-Path -Parent $_ } | Select-Object -Unique | ForEach-Object { $env:PATH = $_ + ";" + $env:PATH }
    Write-Status "env" "VS $Arch environment captured, tool dirs prepended to PATH"

    if (-not $NoMetrics) { Write-Step "[4/7] Starting build-process resource monitor..."; Start-ResourceMonitor -IntervalSeconds $MetricsInterval } else { Write-Step "[4/7] Resource monitoring disabled (-NoMetrics)" }

    if ($SkipDeps) { Write-Step "[5/7] Skipping dependencies (-SkipDeps)" } else {
        Write-Step "[5/7] Checking dependencies..."
        $depsBuildDir = $DepsDir; $depsCompiler = if ($ClangCl) { $clangClExe } else { $clExe }
        Check-Dependencies -Cmake $cmake -Compiler $depsCompiler -Config $Config -DepsBuildDir $depsBuildDir -MaxJobs $MaxJobs
        if ($PackDeps) { Write-Step "[5.1/7] Packing dependencies..."; $zipPath = Pack-Dependencies -DepsBuildDir $depsBuildDir -OutputPath (Join-Path $RepoRoot "deps\OrcaSlicer_dep.zip"); Write-Host ""; Write-Host "  Dependencies packed successfully!" -ForegroundColor Green; Write-Host "  Archive: $zipPath"; return }
    }

    if (-not $Slicer) { Stop-ResourceMonitor; Write-Step "[6/6] Dependencies ready in $DepsDir"; return }

    Write-Step "[6/7] Configuring and building..."
    $cmakeArgs = Get-SlicerConfigureArgs -BuildDir $buildDir -Config $Config -Target $Target -NoPch:$NoPch -Tests:$Tests -Msbuild:$Msbuild -Ninja:$Ninja -ClangCl:$ClangCl -Arch $Arch -DepsDir $DepsDir -ccacheExe $ccacheExe -DepsCompiler $depsCompiler -IncludeEnv $env:INCLUDE -StrawberryHome $strawberryHome

    $generator = if ($Msbuild) { $vsMajor = if ($vsInstall -match '\\(1[6-9]|[2-9][0-9])\\') { $matches[0].Trim('\') } else { "17" }; $vsGenMap = @{ 16 = "Visual Studio 16 2019"; 17 = "Visual Studio 17 2022"; 18 = "Visual Studio 18 2026" }; $vsGenMap[$vsMajor] } else { "Ninja Multi-Config" }
    if (-not $generator) { $generator = "Visual Studio 17 2022" }
    Write-Status "generator" $generator

    $configureCompiler = if ($Msbuild) { $null } elseif ($ClangCl) { $clangClExe } else { $clExe }
    # Strip Strawberry one final time: VS env capture may have added it back
    $env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notlike '*Strawberry\c\bin' }) -join ';'
    $ccacheBefore = Get-CcacheCounters -CcachePath $ccacheExe
    $result = Invoke-ConfigureAndBuild -Cmake $cmake -BuildDir $buildDir -Config $Config -Target $Target -MaxJobs $MaxJobs -Generator $generator -Compiler $configureCompiler -CMakeArgs $cmakeArgs -NoConfigure:$NoConfigure -InstallSlicer:$InstallSlicer
    $timing = $result.Timing; $installTime = $result.InstallTime

    Show-BuildSummary -Timing $timing -InstallTime $installTime -CcachePath $ccacheExe -CcacheBefore $ccacheBefore
    $buildSucceeded = $true
    if ($Launch -and $buildSucceeded) { Launch-Application -BuildDir $buildDir -Config $Config -Wait:$LaunchWait }
} catch {
    Stop-ResourceMonitor; Show-ResourceSummary; Write-Host ""; Write-Host "BUILD FAILED: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.InvocationInfo) { Write-Host ("COMMAND: " + $_.InvocationInfo.MyCommand) -ForegroundColor DarkRed; Write-Host ("INVOKED AS: " + $_.InvocationInfo.InvocationName) -ForegroundColor DarkRed; Write-Host ("AT LINE: " + $_.InvocationInfo.ScriptLineNumber + " -> " + "$($_.InvocationInfo.Line)".Trim()) -ForegroundColor DarkRed; Write-Host ("EXCEPTION: " + $_.Exception.GetType().FullName) -ForegroundColor DarkRed }
    if ($_.ScriptStackTrace) { Write-Host $_.ScriptStackTrace -ForegroundColor DarkRed }
    exit 1
} finally { Stop-ResourceMonitor; Pop-Location }

exit 0