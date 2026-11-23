# Unit: Main build orchestration
# This module contains the core build logic extracted from build_win.ps1
# for better testability and readability.

function Invoke-BuildOrchestration {
    param(
        [string]   $Preset,
        [string]   $Config,
        [string]   $Target,
        [switch]   $Clean,
        [switch]   $CleanDeps,
        [switch]   $NoMetrics,
        [switch]   $PackDeps,
        [switch]   $SkipDeps,
        [int]      $MetricsInterval,
        [int]      $MaxJobs,
        [switch]   $Deps,
        [switch]   $Slicer,
        [switch]   $Tests,
        [switch]   $RunTests,
        [switch]   $InstallDeps,
        [string]   $InstallVs,
        [switch]   $ClangCl,
        [switch]   $Ninja,
        [switch]   $Msvc,
        [switch]   $Msbuild,
        [string]   $ClangPath,
        [string]   $Cache,
        [switch]   $NoPch,
        [switch]   $NoConfigure,
        [switch]   $InstallSlicer,
        [string]   $DepsDir,
        [string]   $SlicerDir,
        [string]   $Arch,
        [switch]   $Launch,
        [switch]   $LaunchWait
    )

    $ErrorActionPreference = "Stop"
    $RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
    $UnitsDir = Join-Path $RepoRoot "scripts\build_win_steps"

    # Load step modules
    . (Join-Path $UnitsDir "native.ps1")
    . (Join-Path $UnitsDir "toolchain.ps1")
    . (Join-Path $UnitsDir "vsenv.ps1")
    . (Join-Path $UnitsDir "deps.ps1")
    . (Join-Path $UnitsDir "build.ps1")
    . (Join-Path $UnitsDir "render.ps1")
    . (Join-Path $UnitsDir "launch.ps1")

    # ... [rest of the build logic would go here]
    # This is a placeholder showing the modular structure
    Write-Host "Invoke-BuildOrchestration called with parameters:"
    Write-Host "  Preset: $Preset"
    Write-Host "  Config: $Config"
    Write-Host "  Launch: $Launch"
    Write-Host "  LaunchWait: $LaunchWait"
}