# Unit: Configure, build and install
# Native output streaming, logging and failure reporting live in native.ps1.

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

    $configureTime = [TimeSpan]::Zero
    if ($NoConfigure) {
        Write-Status "configure" "Skipped (-NoConfigure)"
    } else {
        $configureStarted = Get-Date
        # Remove CMakeCache.txt to force fresh configure (avoids "variables changed" error on preset/compiler change)
        $cmakeCache = Join-Path $BuildDir "CMakeCache.txt"
        if (Test-Path $cmakeCache) {
            Remove-Item $cmakeCache -Force -ErrorAction SilentlyContinue
        }
        $configureArgs = @("-S", (Get-Location).Path, "-B", $BuildDir, "-G", $Generator)
        if ($Compiler) {
            $configureArgs += "-DCMAKE_C_COMPILER=$Compiler"
            $configureArgs += "-DCMAKE_CXX_COMPILER=$Compiler"
        }
        if ($CMakeArgs) { $configureArgs += $CMakeArgs }
        $configureArgs += "-DCMAKE_BUILD_TYPE=$Config"

        $ts = Get-TimeStamp
        Write-Host "[$ts] >>> $Cmake $($configureArgs -join ' ')"
        $logConfigure = New-BuildLogPath "configure"
        Invoke-BuildStep -Label "CMake configure" -LogPath $logConfigure -Command $Cmake -Arguments $configureArgs
        $configureTime = (Get-Date) - $configureStarted
    }

    Write-Host ""
    $buildStarted = Get-Date
    $ts = Get-TimeStamp
    $buildArgs = @("--build", $BuildDir, "--config", $Config, "--target", $Target)
    if ($MaxJobs -gt 0) { $buildArgs += @("--", "-j", "$MaxJobs") }
    Write-Host "[$ts] >>> $Cmake $($buildArgs -join ' ')"
    $logBuild = New-BuildLogPath "build"
    Invoke-BuildStep -Label "Build" -LogPath $logBuild -Command $Cmake -Arguments $buildArgs
    $buildTime = (Get-Date) - $buildStarted

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
    $ts = Get-TimeStamp
    $installArgs = @("--build", $BuildDir, "--config", $Config, "--target", "install")
    Write-Host "[$ts] >>> $Cmake $($installArgs -join ' ')"
    Invoke-BuildStep -Label "Install" -LogPath (New-BuildLogPath "install") -Command $Cmake -Arguments $installArgs
    return (Get-Date) - $started
}
