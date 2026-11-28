# Unit: Visual Studio environment
# Captures the VS devcmd environment into the current PowerShell process.

function Import-VsEnvironmentFile {
    param([string]$Path)

    Get-Content $Path | ForEach-Object {
        if ($_ -match "^([^=]+)=(.*)$") {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
        }
    }
}

function Ensure-Ml64InPath {
    # vcvarsall.bat should put ml64.exe there, but make it explicit so cmake's
    # Boost build cannot pick a stale/wrong MASM from the parent PATH instead.
    $ml64 = Get-Command ml64.exe -ErrorAction SilentlyContinue
    if (-not $ml64) { return }
    $ml64Dir = Split-Path -Parent $ml64.Source
    if (-not ($env:PATH -split ';' | Where-Object { $_ -eq $ml64Dir })) {
        $env:PATH = "$ml64Dir;$env:PATH"
    }
}

function Resolve-VcvarsallArch {
    param([string]$Arch)
    # x64 for x64, x64_arm64 for x64-hosted ARM64 cross
    if ($Arch -eq "ARM64") { return "x64_arm64" }
    return "x64"
}

function Build-VcvarsallBatchBody {
    param([string]$Vcvarsall, [string]$VcArch)
    # Backtick-n, not \n: a literal backslash-n leaves the generated .bat on one
    # logical line, so vcvarsall never runs and the captured env is the parent's.
    return "@echo off`r`ncall `"$Vcvarsall`" $VcArch >nul 2>&1`r`nset > `"$tempEnv`" 2>&1`r`n"
}

function Ensure-VsLibPathHasArch {
    param([string]$VsInstall, [string]$Arch, [string]$Vcvarsall, [string]$VcArch)

    # vcvarsall prints a benign VsDevCmd error and can still end up with an
    # x86-only LIB. Catch that here instead of at link time.
    $archPattern = if ($Arch -eq "ARM64") { "arm64" } else { "x64" }
    if ($env:LIB -and $env:LIB -match "(?i)\\$archPattern") { return }

    $msvcRoot = Join-Path $VsInstall "VC\Tools\MSVC"
    $msvcVersion = Get-ChildItem $msvcRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1

    $kitRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Lib"
    $kitVersion = Get-ChildItem $kitRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1

    if ($msvcVersion -and $kitVersion) {
        $libPaths = @(
            (Join-Path $msvcVersion.FullName "lib\$archPattern"),
            (Join-Path $kitVersion.FullName  "ucrt\$archPattern"),
            (Join-Path $kitVersion.FullName  "um\$archPattern")
        ) | Where-Object { Test-Path $_ }

        if ($libPaths.Count -gt 0) {
            $env:LIB = ($libPaths -join ";") + $(if ($env:LIB) { ";" + $env:LIB } else { "" })
        }
    }

    if (-not $env:LIB -or $env:LIB -notmatch "(?i)\\$archPattern") {
        throw "The Visual Studio environment has no $archPattern library path in LIB. Running '`"$Vcvarsall`" $VcArch' directly may show the cause."
    }
}

function Capture-VsEnvironment {
    param([string]$VsInstall, [string]$Arch = "x64")
    $vcvarsall = Join-Path $VsInstall "VC\Auxiliary\Build\vcvarsall.bat"
    $tempBat = Join-Path $env:TEMP "orca-vcvars-$PID.bat"
    $tempEnv = Join-Path $env:TEMP "orca-vcvars-$PID.txt"

    $vcArch = Resolve-VcvarsallArch -Arch $Arch

    try {
        $body = Build-VcvarsallBatchBody -Vcvarsall $vcvarsall -VcArch $vcArch
        $tempEnv = $tempEnv # ensure variable exists
        [IO.File]::WriteAllText($tempBat, $body, [Text.Encoding]::ASCII)
        & cmd.exe /d /c "call `"$tempBat`""
        if (-not (Test-Path $tempEnv)) {
            throw "vcvarsall.bat did not produce an environment file. Run '`"$vcvarsall`" $vcArch' to see the error."
        }
        Import-VsEnvironmentFile -Path $tempEnv
        Ensure-Ml64InPath
        Ensure-VsLibPathHasArch -VsInstall $VsInstall -Arch $Arch -Vcvarsall $vcvarsall -VcArch $vcArch
    } finally {
        Remove-Item $tempBat, $tempEnv -ErrorAction SilentlyContinue
    }
}