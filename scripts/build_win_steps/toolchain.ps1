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
        foreach ($root in @("$env:ProgramFiles\Microsoft Visual Studio\$version", "${env:ProgramFiles(x86)}\Microsoft Visual Studio\$version")) {
            if (-not (Test-Path $root)) { continue }
            $edition = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                Where-Object { Test-Path (Join-Path $_.FullName "VC\Auxiliary\Build\vcvarsall.bat") } |
                Select-Object -First 1
            if ($edition) { return $edition.FullName }
        }
    }
    return $null
}
