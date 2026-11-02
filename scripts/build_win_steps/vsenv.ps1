# Unit: Visual Studio environment
# Captures the VS devcmd environment into the current PowerShell process.

function Capture-VsEnvironment {
    param([string]$VsInstall, [string]$Arch = "x64")
    $vcvarsall = Join-Path $VsInstall "VC\Auxiliary\Build\vcvarsall.bat"
    $tempBat = Join-Path $env:TEMP "orca-vcvars-$PID.bat"
    $tempEnv = Join-Path $env:TEMP "orca-vcvars-$PID.txt"

    # vcvarsall.bat architecture argument: x64 for x64, x64_arm64 for x64-hosted ARM64 cross
    $vcArch = if ($Arch -eq "ARM64") { "x64_arm64" } else { "x64" }

    try {
        # Backtick-n, not \n: a literal backslash-n leaves the generated .bat on one
        # logical line, so vcvarsall never runs and the captured env is the parent's.
        $body = "@echo off`r`ncall `"$vcvarsall`" $vcArch >nul 2>&1`r`nset > `"$tempEnv`" 2>&1`r`n"
        [IO.File]::WriteAllText($tempBat, $body, [Text.Encoding]::ASCII)
        & cmd.exe /d /c "call `"$tempBat`""
        if (-not (Test-Path $tempEnv)) {
            throw "vcvarsall.bat did not produce an environment file. Run '`"$vcvarsall`" $vcArch' to see the error."
        }
        Get-Content $tempEnv | ForEach-Object {
            if ($_ -match "^([^=]+)=(.*)$") {
                [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
            }
        }

        # Ensure ml64.exe (MASM assembler) is in PATH for cmake's Boost build.
        # vcvarsall.bat should put it there, but make it explicit so cmake's
        # auto-detection can't find a stale/wrong path instead.
        $ml64 = Get-Command ml64.exe -ErrorAction SilentlyContinue
        if ($ml64) {
            $ml64Dir = Split-Path -Parent $ml64.Source
            if (-not ($env:PATH -split ';' | Where-Object { $_ -eq $ml64Dir })) {
                $env:PATH = "$ml64Dir;$env:PATH"
            }
        }

        # vcvarsall prints a benign VsDevCmd error and can still end up with an
        # x86-only LIB. Catch that here instead of at link time.
        $archPattern = if ($Arch -eq "ARM64") { "arm64" } else { "x64" }
        if (-not $env:LIB -or $env:LIB -notmatch "(?i)\\$archPattern") {
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
                throw "The Visual Studio environment has no $archPattern library path in LIB. Running '`"$vcvarsall`" $vcArch' directly may show the cause."
            }
        }
    } finally {
        Remove-Item $tempBat, $tempEnv -ErrorAction SilentlyContinue
    }
}