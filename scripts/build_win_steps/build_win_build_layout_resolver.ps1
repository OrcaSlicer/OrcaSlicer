# Unit: Build layout resolution
# Decides build directory, target, display preset and cleans if requested.

function Resolve-BuildTree {
    param(
        [string]$Config,
        [string]$Arch,
        [switch]$ClangCl,
        [switch]$Msvc,
        [switch]$Msbuild
    )

    $configDirs = @{
        "release"         = "build"
        "debug"           = "build-dbg"
        "relwithdebinfo"  = "build-dbginfo"
        "minsizerel"      = "build-minsize"
    }
    $configLower = $Config.ToLower()
    if (-not $configDirs.ContainsKey($configLower)) {
        throw "Unknown configuration '$Config'. Known configurations: release, debug, relwithdebinfo, minsizerel."
    }
    $treeName = $configDirs[$configLower]

    if ($ClangCl) { $treeName += "-clang" }
    if ($Msvc -and $Msbuild) { $treeName += "-msbuild" }
    if ($Arch -eq "ARM64") { $treeName += "-arm64" }

    return $treeName
}

function Resolve-BuildPreset {
    param(
        [string]$Preset,
        [switch]$ClangCl,
        [switch]$Ninja,
        [switch]$Msvc,
        [switch]$Msbuild,
        [string]$Cache
    )

    if ($Preset) { return $Preset }
    elseif ($ClangCl -and $Ninja -and $Cache -eq "ccache") { return "x64-clang-cache" }
    elseif ($ClangCl -and $Ninja) { return "x64-clang" }
    elseif ($Msvc -and $Ninja) { return "x64-msvc" }
    elseif ($Msvc -and $Msbuild) { return "x64-msbuild" }
    else { return "custom" }
}

function Resolve-BuildDir {
    param(
        [string]$TreeName,
        [string]$SlicerDir
    )
    if ($SlicerDir) { return $SlicerDir }
    return $TreeName
}

function Resolve-BuildLayout {
    param(
        [Parameter(Mandatory)] [string] $Config,
        [Parameter(Mandatory)] [string] $Arch,
        [switch] $ClangCl,
        [switch] $Ninja,
        [switch] $Msvc,
        [switch] $Msbuild,
        [string] $Preset,
        [string] $Cache,
        [string] $CcacheExe,
        [string] $SlicerDir,
        [string] $Target,
        [string] $DepsDir,
        [switch] $CleanDeps,
        [switch] $Clean,
        [int] $MaxJobs
    )

    $treeName = Resolve-BuildTree -Config $Config -Arch $Arch -ClangCl:$ClangCl -Msvc:$Msvc -Msbuild:$Msbuild
    $displayPreset = Resolve-BuildPreset -Preset $Preset -ClangCl:$ClangCl -Ninja:$Ninja -Msvc:$Msvc -Msbuild:$Msbuild -Cache:$Cache
    $buildDir = Resolve-BuildDir -TreeName $treeName -SlicerDir:$SlicerDir

    if (-not $Target) { $Target = "all" }

    if ($Cache -eq "ccache" -and -not $CcacheExe) {
        throw "ccache is required for cache builds. Install it, add it to PATH, or set ORCA_CCACHE."
    }

    Write-Status "preset"  "$displayPreset ($buildDir)"
    Write-Status "target"  "$Target ($Config)"
    Write-Status "arch"    $Arch
    Write-Status "compiler" $(if ($ClangCl) { "clang-cl" } else { "cl" })
    Write-Status "generator" $(if ($Ninja) { "Ninja" } else { "MSBuild" })
    Write-Status "jobs"    $(if ($MaxJobs -gt 0) { "$MaxJobs" } else { "auto" })

    Clean-Build -BuildDir $buildDir -Clean:$Clean
    Clean-Dependencies -DepsDir $DepsDir -CleanDeps:$CleanDeps

    return [pscustomobject]@{
        BuildDir       = $buildDir
        Target         = $Target
        DisplayPreset  = $displayPreset
        TreeName       = $treeName
    }
}

function Clean-Build {
    param(
        [string]$BuildDir,
        [switch]$Clean
    )
    if ($Clean -and (Test-Path $BuildDir)) {
        Write-Status "clean" "Removing $BuildDir"
        Remove-Item $BuildDir -Recurse -Force
    }
}

function Clean-Dependencies {
    param(
        [string]$DepsDir,
        [switch]$CleanDeps
    )
    if ($CleanDeps -and (Test-Path $DepsDir)) {
        Write-Status "clean" "Removing $DepsDir"
        Remove-Item $DepsDir -Recurse -Force
    }
}