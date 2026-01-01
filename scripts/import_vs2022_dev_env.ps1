param(
    [string]$Arch = "x64"
)

# This script imports the environment from the VS2022 Developer Command Prompt
# into the current PowerShell session.

$vsDevCmd = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\Tools\\VsDevCmd.bat"

if (-not (Test-Path $vsDevCmd)) {
    Write-Error "VsDevCmd.bat not found at: $vsDevCmd"
    return
}

# Run VsDevCmd in a temporary cmd.exe and dump its environment with 'set'.
$cmd = '"' + $vsDevCmd + '" -arch=' + $Arch + ' >nul 2>&1 & set'

cmd.exe /c $cmd | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') {
        $name, $value = $matches[1], $matches[2]
        # Update the current PowerShell process environment.
        Set-Item -Path "Env:$name" -Value $value
    }
}

Write-Host "Imported VS2022 Developer Command Prompt environment (arch=$Arch)." -ForegroundColor Green
