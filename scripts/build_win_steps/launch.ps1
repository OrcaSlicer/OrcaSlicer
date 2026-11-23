# Unit: Launch application after build
# Returns $true if launch was attempted (regardless of success), $false if skipped

function Launch-Application {
    param(
        [string] $BuildDir,
        [string] $Config,
        [switch] $Wait
    )

    $exePath = Join-Path $BuildDir "OrcaSlicer\OrcaSlicer.exe"
    if (-not (Test-Path $exePath)) {
        Write-Warning "Launch skipped: OrcaSlicer.exe not found at $exePath"
        return $false
    }

    Write-Step "[Launch] Starting OrcaSlicer.exe..."
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
        Write-Error "Failed to launch OrcaSlicer.exe: $($_.Exception.Message)"
        return $false
    }
}