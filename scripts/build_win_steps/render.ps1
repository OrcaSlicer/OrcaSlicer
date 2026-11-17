# Unit: Minimal rendering (native PowerShell, <100 lines)
# No external deps, works in PS 5.1+, outputs clean CI-friendly text

$script:RenderStarted = Get-Date

function Write-Step {
    param([string]$Text)
    $ts = Get-TimeStamp
    Write-Host "[$ts] $Text"
}

function Write-Status {
    param(
        [string]$Tag,
        [string]$Text,
        [string]$Color = "DarkGray"
    )
    Write-Host ("  {0,-9} {1}" -f "[$Tag]", $Text) -ForegroundColor $Color
}

function Write-ErrorLine {
    param([string]$Text)
    Write-Host "  [ERR] $Text" -ForegroundColor Red
}

function Write-WarningLine {
    param([string]$Text)
    Write-Host "  [WARN] $Text" -ForegroundColor Yellow
}

function Show-BuildMetrics {
    param([hashtable]$Timing)
    if (-not $Timing -or $Timing.Count -eq 0) { return }

    $fmt = {
        param($t)
        $h = [math]::Floor($t.TotalHours)
        $m = [math]::Floor($t.TotalMinutes) % 60
        $s = [math]::Round($t.Seconds)
        if ($h) { "{0}:{1:00}:{2:00}" -f $h, $m, $s }
        elseif ($m) { "{0}m {1:00}s" -f $m, $s }
        else { "{0}s" -f $s }
    }

    Write-Host ""
    Write-Host "  Total:     $(& $fmt $Timing.Total)" -ForegroundColor Cyan
    foreach ($k in $Timing.Keys) {
        if ($k -ne 'Total') {
            Write-Host "  $($k.PadRight(8)) $(& $fmt $Timing[$k])" -ForegroundColor DarkGray
        }
    }
}

function Get-TimeStamp {
    $span = (Get-Date) - $script:RenderStarted
    $h = [math]::Floor($span.TotalHours)
    $m = [math]::Floor($span.TotalMinutes) % 60
    $s = [math]::Round($span.Seconds)
    return "{0}:{1}:{2}" -f $h, $m, $s
}

function Show-CcacheStats {
    param([string]$CcachePath)
    if (-not $CcachePath -or -not (Test-Path $CcachePath)) { return }
    try {
        & $CcachePath -s | ForEach-Object { Write-Status "ccache" $_ }
    } catch {
        Write-Status "ccache" "Unavailable: $($_.Exception.Message)" "Yellow"
    }
}

# Progress helper
function Start-BuildProgress {
    param([string]$Activity, [string]$Status = "")
    Write-Progress -Activity $Activity -Status $Status -PercentComplete 0
}

function Update-BuildProgress {
    param([int]$Percent, [string]$Status = "")
    Write-Progress -PercentComplete $Percent -Status $Status
}

function Complete-BuildProgress {
    Write-Progress -Completed
}