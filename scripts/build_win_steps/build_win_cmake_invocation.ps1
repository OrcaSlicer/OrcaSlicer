# Unit: Configure, build and install
# Native output streaming, logging and failure reporting live in native.ps1.

# Render an argument list for the log, one flag per line:
#   -S src
#   -DNAME=VALUE      (-DNAME=VALUE is a single argument, not a pair)
#   -- -j 12          (everything after -- is a passthrough tail)
function Format-CommandLine {
    param([string[]] $Arguments)

    $lines = @()
    for ($i = 0; $i -lt $Arguments.Count; $i++) {
        $arg = $Arguments[$i]
        if ($arg -eq "--") {
            $lines += "  -- " + ($Arguments[($i + 1)..($Arguments.Count - 1)] -join " ")
            break
        }
        if ($arg -like "-D*=*") {
            $lines += "  $arg"
        } elseif ($i + 1 -lt $Arguments.Count -and $Arguments[$i + 1] -notlike "-D*=*") {
            $lines += "  $arg $($Arguments[$i + 1])"
            $i++
        } else {
            $lines += "  $arg"
        }
    }
    return ($lines -join "`n")
}

function Get-ConfigureArgs {
    param([string]$BuildDir, [string]$Config, [string]$Generator, [string]$Compiler, [string[]]$CMakeArgs)

    $configureArgs = @("-S", (Get-Location).Path, "-B", $BuildDir, "-G", $Generator)
    if ($Compiler) {
        $configureArgs += "-DCMAKE_C_COMPILER=$Compiler"
        $configureArgs += "-DCMAKE_CXX_COMPILER=$Compiler"
    }
    if ($CMakeArgs) { $configureArgs += $CMakeArgs }
    $configureArgs += "-DCMAKE_BUILD_TYPE=$Config"
    return $configureArgs
}

function Invoke-ConfigureStep {
    param(
        [string]   $Cmake,
        [string]   $BuildDir,
        [string]   $Config,
        [switch]   $NoConfigure,
        [string]   $Generator = "Ninja Multi-Config",
        [string]   $Compiler,
        [string[]] $CMakeArgs
    )

    # -NoConfigure only skips when the tree can actually be built: a missing
    # build dir or CMakeCache means configure has to run regardless.
    $cmakeCache = Join-Path $BuildDir "CMakeCache.txt"
    if ($NoConfigure -and (Test-Path $cmakeCache)) {
        Write-Status "configure" "Skipped (-NoConfigure)"
        return [TimeSpan]::Zero
    }
    if ($NoConfigure) {
        Write-Status "configure" "Build dir missing or no CMakeCache - running configure anyway"
    }

    $started = Get-Date
    # Drop the cache to force a fresh configure: a preset or compiler change
    # otherwise fails with "variables changed" instead of reconfiguring.
    if (Test-Path $cmakeCache) {
        Remove-Item $cmakeCache -Force -ErrorAction SilentlyContinue
    }
    $configureArgs = Get-ConfigureArgs -BuildDir $BuildDir -Config $Config -Generator $Generator `
        -Compiler $Compiler -CMakeArgs $CMakeArgs

    $ts = Get-TimeStamp
    Write-Host "[$ts] >>> $Cmake`n$(Format-CommandLine $configureArgs)"
    Invoke-BuildStep -Label "CMake configure" -LogPath (New-BuildLogPath "configure") `
                     -Command $Cmake -Arguments $configureArgs
    return (Get-Date) - $started
}

function Invoke-BuildTarget {
    param([string]$Cmake, [string]$BuildDir, [string]$Config, [string]$Target, [int]$MaxJobs)

    $started = Get-Date
    $buildArgs = @("--build", $BuildDir, "--config", $Config, "--target", $Target)
    if ($MaxJobs -gt 0) { $buildArgs += @("--", "-j", "$MaxJobs") }

    $ts = Get-TimeStamp
    Write-Host "[$ts] >>> $Cmake`n$(Format-CommandLine $buildArgs)"
    Invoke-BuildStep -Label "Build" -LogPath (New-BuildLogPath "build") `
                     -Command $Cmake -Arguments $buildArgs
    return (Get-Date) - $started
}

function Configure-Build {
    param(
        [string]   $Cmake,
        [string]   $BuildDir,
        [string]   $Config,
        [string]   $Target,
        [int]      $MaxJobs,
        [switch]   $NoConfigure,
        [string]   $Generator = "Ninja Multi-Config",
        [string]   $Compiler,
        [string[]] $CMakeArgs
    )
    Write-Host ""

    $configureTime = Invoke-ConfigureStep -Cmake $Cmake -BuildDir $BuildDir -Config $Config `
        -NoConfigure:$NoConfigure -Generator $Generator -Compiler $Compiler -CMakeArgs $CMakeArgs

    Write-Host ""
    $buildTime = Invoke-BuildTarget -Cmake $Cmake -BuildDir $BuildDir -Config $Config `
        -Target $Target -MaxJobs $MaxJobs

    return @{
        ConfigureTime = $configureTime
        BuildTime     = $buildTime
    }
}

# CMakeLists installs into <build dir>/OrcaSlicer, which is the tree CI packages.
function Install-Build {
    param(
        [string] $Cmake,
        [string] $BuildDir,
        [string] $Config
    )
    Write-Host ""
    $started = Get-Date
    $installArgs = @("--build", $BuildDir, "--config", $Config, "--target", "install")

    $ts = Get-TimeStamp
    Write-Host "[$ts] >>> $Cmake`n$(Format-CommandLine $installArgs)"
    Invoke-BuildStep -Label "Install" -LogPath (New-BuildLogPath "install") `
                     -Command $Cmake -Arguments $installArgs
    return (Get-Date) - $started
}
