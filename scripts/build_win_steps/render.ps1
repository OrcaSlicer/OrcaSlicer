# Unit: Console rendering. Native PowerShell only: no Spectre, no Python, no CSV.
# Sparklines turn the samples collected by native.ps1 into a readable shape.

# Block glyphs are single bytes in UTF-8 but multi-byte in the OEM code page that
# PS 5.1 defaults to when stdout is redirected, which is how CI captures it.
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

function Get-TimeStamp {
    param([datetime]$Since)
    if (-not $Since) { $Since = $global:BuildStarted }
    if (-not $Since) { $Since = Get-Date }
    $span = (Get-Date) - $Since
    return "{0}:{1:00}:{2:00}" -f [math]::Floor($span.TotalHours), ([math]::Floor($span.TotalMinutes) % 60), [math]::Round($span.Seconds)
}

function Write-Step {
    param([string]$Text)
    Write-Host "[$(Get-TimeStamp)] $Text"
}

function Write-Status {
    param(
        [string]$Tag,
        [string]$Text,
        [string]$Color = "DarkGray"
    )
    Write-Host ("  {0,-10} {1}" -f "[$Tag]", $Text) -ForegroundColor $Color
}

function Format-Duration {
    param([TimeSpan]$Span)
    if ($Span.TotalHours -ge 1) { return "{0}:{1:00}:{2:00}" -f [math]::Floor($Span.TotalHours), $Span.Minutes, $Span.Seconds }
    if ($Span.TotalMinutes -ge 1) { return "{0}m {1:00}s" -f [math]::Floor($Span.TotalMinutes), $Span.Seconds }
    return "{0}s" -f [math]::Round($Span.TotalSeconds)
}

# Renders values as one block glyph per column. Columns are fixed so the CPU,
# RAM and disk charts line up; a short build is not stretched to full width.
function Format-Sparkline {
    param(
        [double[]]$Values,
        [double]$Min = 0,
        [double]$Max,
        [int]$Width = 56
    )
    if (-not $Values -or $Values.Count -eq 0) { return "" }
    if (-not $Max -or $Max -le $Min) {
        $Max = ($Values | Measure-Object -Maximum).Maximum
        if ($Max -le $Min) { $Max = $Min + 1 }
    }
    $ramp = [char[]](0x2581..0x2588)
    $cols = [math]::Min($Width, $Values.Count)
    $sb = New-Object System.Text.StringBuilder
    for ($col = 0; $col -lt $cols; $col++) {
        $start = [int][math]::Floor($col * $Values.Count / $cols)
        $end   = [int][math]::Max($start, [math]::Floor(($col + 1) * $Values.Count / $cols) - 1)
        $avg = ($Values[$start..$end] | Measure-Object -Average).Average
        $idx = [int][math]::Round((($avg - $Min) / ($Max - $Min)) * 7)
        if ($idx -lt 0) { $idx = 0 } elseif ($idx -gt 7) { $idx = 7 }
        [void]$sb.Append($ramp[$idx])
    }
    return $sb.ToString()
}

function Show-CcacheStats {
    param([string]$CcachePath)
    if (-not $CcachePath -or -not (Test-Path $CcachePath)) { return }
    try {
        & $CcachePath -s | ForEach-Object { Write-Status "ccache" $_ }
    } catch {
        Write-Status "ccache" "unavailable: $($_.Exception.Message)" "Yellow"
    }
}
