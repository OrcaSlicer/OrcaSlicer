# Unit: Launch application after build
# Returns $true if launch was attempted (regardless of success), $false if skipped

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
            Start-Process -FilePath $exePath -Wait -ErrorAction Stop
            Write-Status "launch" "Process exited"
        } else {
            Start-Process -FilePath $exePath -ErrorAction Stop
            Write-Status "launch" "Started (background)"
        }
        return $true
    } catch {
        # Write-Warning, not Write-Error: the caller runs with $ErrorActionPreference
        # 'Stop', where Write-Error would turn a failed launch (a build that did
        # succeed) into a BUILD FAILED abort.
        Write-Warning "Failed to launch orca-slicer.exe: $($_.Exception.Message)"
        return $false
    }
}
