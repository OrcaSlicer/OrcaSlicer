# Unit: Configure and build
# Extracts the configure+build+install sequence from build_win.ps1 into a reusable function.

function Invoke-ConfigureAndBuild {
    param(
        [string] $Cmake,
        [string] $BuildDir,
        [string] $Config,
        [string] $Target,
        [int]    $MaxJobs,
        [string] $Generator,
        [string] $Compiler,
        [string[]] $CMakeArgs,
        [switch] $NoConfigure,
        [switch] $InstallSlicer
    )

    $timing = Configure-Build -Cmake $Cmake -BuildDir $BuildDir -Config $Config -Target $Target `
        -NoConfigure:$NoConfigure -MaxJobs $MaxJobs `
        -Generator $Generator -Compiler $Compiler -CMakeArgs $CMakeArgs

    $installTime = [TimeSpan]::Zero
    if ($InstallSlicer) {
        $installTime = Install-Build -Cmake $Cmake -BuildDir $BuildDir -Config $Config
    }

    return [pscustomobject]@{
        Timing      = $timing
        InstallTime = $installTime
    }
}

function Get-BaseCmakeArgs {
    param(
        [switch] $NoPch,
        [switch] $Tests,
        [switch] $Msbuild,
        [switch] $Ninja,
        [switch] $ClangCl,
        [string] $Arch
    )

    $cmakeArgs = @()
    if ($NoPch) { $cmakeArgs += "-DSLIC3R_PCH=OFF" }
    if ($Tests) { $cmakeArgs += "-DBUILD_TESTS=ON" }
    $cmakeArgs += "-DORCA_TOOLS=ON"
    if ($Msbuild) { $cmakeArgs += "-A", $Arch }
    if (-not $Ninja -and $ClangCl) { $cmakeArgs += "-T", "ClangCL" }

    return $cmakeArgs
}

function Pin-JpegToDeps {
    param(
        [string[]] $CmakeArgs,
        [string] $DepsDir
    )

    $cmakeArgs = $CmakeArgs

    # Pin JPEG to deps so find_package finds libjpeg-turbo with JCS_EXT_RGBA
    $depsInstallRoot = $DepsDir
    $depsInstallDir = Join-Path $depsInstallRoot "OrcaSlicer_dep/usr/local"
    if (Test-Path $depsInstallDir -PathType Container) {
        $depsRootFull = (Resolve-Path -LiteralPath $depsInstallRoot).Path
        $depsInstallDir = Join-Path $depsRootFull "OrcaSlicer_dep/usr/local"
        $depsInstallDir = $depsInstallDir.Replace('\', '/')
        $cmakeArgs += "-DDEP_BUILD_DIR=$($depsRootFull.Replace('\', '/'))"
        $cmakeArgs += "-DCMAKE_PREFIX_PATH=$depsInstallDir"
        $cmakeArgs += "-DJPEG_INCLUDE_DIR=$depsInstallDir/include"
        $jpegLib = Join-Path $depsInstallDir "lib/jpeg.lib"
        if (Test-Path $jpegLib -PathType Leaf) {
            $cmakeArgs += "-DJPEG_LIBRARY=$jpegLib"
        }
    }

    return $cmakeArgs
}

function Add-CcacheLaunchers {
    param(
        [string[]] $CmakeArgs,
        [string] $ccacheExe
    )

    $cmakeArgs = $CmakeArgs

    # CMake routes compiles through cache only when named as launcher
    if ($ccacheExe) {
        $cmakeArgs += "-DCMAKE_C_COMPILER_LAUNCHER=$ccacheExe"
        $cmakeArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER=$ccacheExe"
    }

    return $cmakeArgs
}

function Strip-StrawberryIncludeFromIncludeEnv {
    param(
        [string] $IncludeEnv
    )

    # Strip Strawberry Perl's MinGW include dir so it can't shadow deps' libjpeg-turbo
    if ($IncludeEnv) {
        $env:INCLUDE = ($IncludeEnv -split ';' | Where-Object { $_ -notlike '*Strawberry*' }) -join ';'
    }
}

function Remove-StrawberryJpegInclude {
    param([string]$RunnerTemp)

    # Strawberry's c\include has stale jpeglib.h; delete only c\include, keep perl for cmake.exe
    if ($RunnerTemp) {
        $strawberryInclude = Join-Path $RunnerTemp 'Strawberry\c\include'
        if (Test-Path $strawberryInclude -PathType Container) {
            Remove-Item $strawberryInclude -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Apply-JpegPinAndEnv {
    param(
        [string[]] $CmakeArgs,
        [string] $DepsDir,
        [string] $ccacheExe,
        [string] $IncludeEnv,
        [string] $RunnerTemp
    )

    $cmakeArgs = $CmakeArgs
    $cmakeArgs = Pin-JpegToDeps -CmakeArgs $cmakeArgs -DepsDir $DepsDir
    $cmakeArgs = Add-CcacheLaunchers -CmakeArgs $cmakeArgs -ccacheExe $ccacheExe
    Strip-StrawberryIncludeFromIncludeEnv -IncludeEnv $IncludeEnv
    Remove-StrawberryJpegInclude -RunnerTemp $RunnerTemp

    return $cmakeArgs
}

function Get-SlicerConfigureArgs {
    param(
        [string] $BuildDir,
        [string] $Config,
        [string] $Target,
        [switch] $NoPch,
        [switch] $Tests,
        [switch] $Msbuild,
        [switch] $Ninja,
        [switch] $ClangCl,
        [string] $Arch,
        [string] $DepsDir,
        [string] $ccacheExe,
        [string] $DepsCompiler,
        [string] $IncludeEnv,
        [string] $RunnerTemp
    )

    $baseArgs = Get-BaseCmakeArgs -NoPch:$NoPch -Tests:$Tests -Msbuild:$Msbuild -Ninja:$Ninja -ClangCl:$ClangCl -Arch $Arch
    return Apply-JpegPinAndEnv -CmakeArgs $baseArgs -DepsDir $DepsDir -ccacheExe $ccacheExe -IncludeEnv $IncludeEnv -RunnerTemp $RunnerTemp
}