# Unit: Console rendering. Native PowerShell only: no Spectre, no Python, no CSV.
# Sparklines turn the samples collected by native.ps1 into a readable shape.

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

# ccache counts one result lookup per compilation, and local_storage_hit already
# includes direct_cache_hit (likewise for misses). Summing both families inflates
# hits once and misses twice, so read a single pair instead.
function Get-CcacheCounters {
    param([string]$CcachePath)
    $counters = @{ Hits = 0; Misses = 0; CacheKiB = 0; MaxKiB = 0 }
    if (-not $CcachePath -or -not (Test-Path $CcachePath)) { return $counters }
    try {
        $raw = @{}
        foreach ($line in & $CcachePath --print-stats) {
            if ($line -match '^(\w+)\t(\d+)$') { $raw[$matches[1]] = [int]$matches[2] }
        }
        # ccache before 4.4 has no storage counters; fall back to the direct-mode pair.
        if ($raw.ContainsKey('local_storage_hit')) { $counters.Hits = $raw['local_storage_hit'] } else { $counters.Hits = $raw['direct_cache_hit'] }
        if ($raw.ContainsKey('local_storage_miss')) { $counters.Misses = $raw['local_storage_miss'] } else { $counters.Misses = $raw['cache_miss'] }
        $counters.CacheKiB = $raw['cache_size_kibibyte']
        $counters.MaxKiB   = $raw['max_cache_size_kibibyte']
    } catch { }
    return $counters
}

function Show-BuildSummary {
    param(
        $Timing,
        [TimeSpan]$InstallTime,
        [string]$CcachePath,
        $CcacheBefore
    )
    Write-Host ""
    Write-Host ""
    Write-Step "[7/7] Build finished"

    # Report this build's share, not the lifetime counters: those cover every build
    # since the cache was zeroed, so a cold first build drags the percentage down forever.
    if ($CcachePath) {
        if (-not $CcacheBefore) { $CcacheBefore = @{ Hits = 0; Misses = 0 } }
        $after = Get-CcacheCounters -CcachePath $CcachePath
        $cacheGB = "{0:N1}" -f ($after.CacheKiB / 1024 / 1024)
        $maxGB = "{0:N1}" -f ($after.MaxKiB / 1024 / 1024)
        $pct = if ($after.MaxKiB -gt 0) { [math]::Round(100 * $after.CacheKiB / $after.MaxKiB, 2) } else { 0 }
        $hits = $after.Hits - $CcacheBefore.Hits
        $calls = $hits + ($after.Misses - $CcacheBefore.Misses)
        if ($calls -gt 0) {
            $hitRate = [math]::Round(100 * $hits / $calls, 1)
            Write-Host ("  ccache: {0}% hit rate this build ({1}/{2}), {3} / {4} GB ({5}%)" -f $hitRate, $hits, $calls, $cacheGB, $maxGB, $pct)
        } else {
            Write-Host ("  ccache: no compilations this build, cache {0} / {1} GB ({2}%)" -f $cacheGB, $maxGB, $pct)
        }
    }

    # Resource summary
    Stop-ResourceMonitor
    Show-ResourceSummary

    # Success summary
    $total = $timing.ConfigureTime + $timing.BuildTime + $installTime
    $configureStr = Format-Duration $timing.ConfigureTime
    $buildStr     = Format-Duration $timing.BuildTime
    $totalStr     = Format-Duration $total

    Write-Host ("  Total:     {0}" -f $totalStr)
    Write-Host ("  Configure: {0}" -f $configureStr)
    Write-Host ("  Build:     {0}" -f $buildStr)
    if ($installTime.TotalSeconds -ge 1) {
        Write-Host ("  Install:   {0}" -f (Format-Duration $installTime))
    }
}

