function Launch-Application {
    param(
        [string] $BuildDir,
        [string] $Config,
        [switch] $Wait
    )

    $exePath = Join-Path $BuildDir "OrcaSlicer\orca-slicer.exe"
    if (-not (Test-Path $exePath)) {
        Write-Warning "Launch skipped: orca-slicer.exe not found at $exePath"
        return $false
    }

    Write-Step "[Launch] Starting orca-slicer.exe..."
    $ts = Get-TimeStamp
    Write-Host "[$ts] >>> $exePath"

    try {
        if ($Wait) {
            $proc = Start-Process -FilePath $exePath -Wait -PassThru -ErrorAction Stop
            Write-Host "  [launch]   Exited with code $($proc.ExitCode)"
        } else {
            Start-Process -FilePath $exePath -ErrorAction Stop
            Write-Host "  [launch]   Started (background)"
        }
        return $true
    } catch {
        # Write-Warning, not Write-Error: caller uses $ErrorActionPreference 'Stop';
        # Write-Error would turn a failed launch (build succeeded) into BUILD FAILED.
        Write-Warning "Failed to launch orca-slicer.exe: $($_.Exception.Message)"
        return $false
    }
}
