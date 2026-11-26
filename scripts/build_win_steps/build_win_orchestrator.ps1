# Unit: Native process invocation
# Shared helper so every CMake/Ninja call streams its output, keeps a log and
# turns a non-zero exit code into a terminating error with a visible tail.

function Format-StepOutputLine {
    param([string]$Text, [ref]$LastLine)

    if (-not $Text.Trim()) { return }
    # Merging native stderr with 2>&1 surfaces bare type names where the
    # record carries no message; they split the real error text.
    if ($Text.Trim() -match '^System\.Management\.Automation\.\w+Exception$') { return }
    $span = if ($global:BuildStarted) { (Get-Date) - $global:BuildStarted } else { [TimeSpan]::Zero }
    $ts = "[{0}:{1:00}:{2:00}]" -f [math]::Floor($span.TotalHours), ([math]::Floor($span.TotalMinutes) % 60), [math]::Round($span.Seconds)
    $raw = "$ts $Text"
    # CMake sometimes prints the same message twice.
    if ($raw -eq $LastLine.Value) { return }
    $LastLine.Value = $raw
    return $raw
}

function Show-StepLogTail {
    param([string]$LogPath)

    if (-not $LogPath -or -not (Test-Path $LogPath)) { return }
    Write-Host "--- Last 30 lines of $LogPath ---" -ForegroundColor DarkGray
    Get-Content $LogPath | Select-Object -Last 30 | ForEach-Object { Write-Host $_ -ForegroundColor DarkGray }
}

function Invoke-BuildStep {
    param(
        [string]$Label,
        [string]$LogPath,
        [string[]]$Command,
        [string[]]$Arguments
    )

    $previousPreference = $ErrorActionPreference
    $exceptionCaught = $null
    # Stream the log line by line instead of buffering it for the whole step: a
    # 30 minute build killed with Ctrl+C must still leave the log of what ran.
    $logWriter = $null
    if ($LogPath) {
        $logWriter = New-Object System.IO.StreamWriter($LogPath, $false, (New-Object System.Text.UTF8Encoding($false)))
        $logWriter.AutoFlush = $true
    }
    # Native tools report progress and errors on stderr. With 'Stop' the first
    # such line becomes a terminating error that hides the real failure, so the
    # preference is relaxed for the duration of the call.
    $ErrorActionPreference = "Continue"
    $lastLine = ""
    try {
        # Pipe straight into ForEach-Object: assigning the call to a variable first
        # makes PowerShell collect every line before the loop starts, so a 30 minute
        # build prints nothing until it exits.
        & $Command @Arguments 2>&1 | ForEach-Object {
            $raw = Format-StepOutputLine -Text $_.ToString() -LastLine ([ref]$lastLine)
            if (-not $raw) { return }
            # CMake writes progress to stderr too, so the stream alone says nothing.
            if ($raw -match '(?i)(error:|fatal:|FAILED:|^error\s|^fatal\s|^\s*error\s)') {
                Write-Host $raw -ForegroundColor Red
            } else {
                Write-Host $raw -ForegroundColor DarkGray
            }
            if ($logWriter) { $logWriter.WriteLine($raw) }
        }
        $exitCode = $LASTEXITCODE
        $exceptionCaught = $null
    } catch {
        $exceptionCaught = $_
        $exitCode = 1
    } finally {
        $ErrorActionPreference = $previousPreference
        if ($logWriter) { $logWriter.Dispose() }
    }

    # The tail must go through Write-Host: callers capture this function's output,
    # so anything written to the success stream is discarded when the throw unwinds.
    if ($exceptionCaught) {
        Write-Host "--- $Label threw $($exceptionCaught.GetType().Name): $($exceptionCaught.Exception.Message) ---" -ForegroundColor Red
        Show-StepLogTail -LogPath $LogPath
        throw "$Label failed due to exception ($($exceptionCaught.GetType().Name)): $($exceptionCaught.Exception.Message)"
    }
    if ($exitCode -ne 0) {
        Write-Host ""
        Write-Host "--- $Label failed (exit $exitCode) ---" -ForegroundColor Red
        Show-StepLogTail -LogPath $LogPath
        throw "$Label failed with exit code $exitCode"
    }
}

function New-BuildLogPath {
    param([string]$Name)
    $logDir = Join-Path $env:TEMP "orca-build-logs"
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    return (Join-Path $logDir ("{0}-{1}.log" -f (Get-Date -Format "yyyyMMdd-HHmmss"), $Name))
}

$global:ResourceMonitorJob = $null
$global:ResourceSamples = @()
$global:ResourceCategories = @('compiler', 'linker', 'build', 'ccache')

function Start-ResourceMonitor {
    param([int]$IntervalSeconds = 15)
    $global:ResourceSamples = @()
    $global:ResourceMonitorStarted = $true
    $global:ResourceMonitorJob = Start-Job -FilePath (Join-Path $PSScriptRoot "build_win_resource_sampler.ps1") `
        -ArgumentList $IntervalSeconds
    Write-Status "sampler" "build-tree CPU and RAM every ${IntervalSeconds}s"
}

function Stop-ResourceMonitor {
    if (-not $global:ResourceMonitorJob) { return }
    # Drain before stopping: emitted objects sit in the job's output buffer and
    # go with the job once it is removed.
    $global:ResourceSamples = @(Receive-Job -Job $global:ResourceMonitorJob -ErrorAction SilentlyContinue)
    Stop-Job $global:ResourceMonitorJob -ErrorAction SilentlyContinue
    Remove-Job $global:ResourceMonitorJob -Force -ErrorAction SilentlyContinue
    $global:ResourceMonitorJob = $null
}

function Show-ResourceSummary {
    $samples = @($global:ResourceSamples | Where-Object { $null -ne $_ })
    if ($samples.Count -eq 0) { return }

    $last = $samples[-1]
    $logical = [math]::Max(1.0, [double]$last.Logical)
    $totalGb = [double]$last.TotalGb

    $header = if ($samples.Count -gt 1) {
        "  Resource use ({0} samples, {1:N0}s apart)" -f $samples.Count, (($last.Elapsed - $samples[0].Elapsed) / ($samples.Count - 1))
    } else {
        "  Resource use (1 sample)"
    }

    # Every CPU row shares the 0..logical scale, so a tool's shape can be read
    # against the total instead of needing its own axis.
    $cores = @($samples | ForEach-Object { ($_.Cores | Measure-Object -Sum).Sum })
    $rss = @($samples | ForEach-Object { [double]$_.RssGb })

    Write-Host ""
    Write-Host $header -ForegroundColor Cyan

    Write-Host ("  CPU    avg {0:N1}  peak {1:N1} of {2:N0} cores" -f `
        ($cores | Measure-Object -Average).Average, ($cores | Measure-Object -Maximum).Maximum, $logical)
    Write-Host ("    total    " + (Format-Sparkline -Values $cores -Min 0 -Max $logical)) -ForegroundColor Cyan

    # Share is over the whole run, not the peak: a tool can own the peak instant
    # and still be minor overall, and that is what the split is asked about.
    $coreSum = ($cores | Measure-Object -Sum).Sum
    for ($i = 0; $i -lt $global:ResourceCategories.Count; $i++) {
        $values = @($samples | ForEach-Object { [double]$_.Cores[$i] })
        $peak = ($values | Measure-Object -Maximum).Maximum
        if ($peak -lt 0.05) { continue }
        $share = if ($coreSum -gt 0) { 100 * ($values | Measure-Object -Sum).Sum / $coreSum } else { 0 }
        Write-Host ("    {0,-8} {1,4:N1} cores {2,3:N0}%  {3}" -f $global:ResourceCategories[$i], $peak, $share, (Format-Sparkline -Values $values -Min 0 -Max $logical)) -ForegroundColor DarkGray
    }

    Write-Host ("  RAM    peak {0:N1} of {1:N1} GB" -f ($rss | Measure-Object -Maximum).Maximum, $totalGb)
    Write-Host ("    total    " + (Format-Sparkline -Values $rss -Min 0 -Max $totalGb)) -ForegroundColor Cyan

    $rssSum = ($rss | Measure-Object -Sum).Sum
    for ($i = 0; $i -lt $global:ResourceCategories.Count; $i++) {
        $values = @($samples | ForEach-Object { [double]$_.RssBy[$i] })
        $peak = ($values | Measure-Object -Maximum).Maximum
        if ($peak -lt 0.05) { continue }
        $share = if ($rssSum -gt 0) { 100 * ($values | Measure-Object -Sum).Sum / $rssSum } else { 0 }
        Write-Host ("    {0,-8} {1,4:N1} GB   {2,3:N0}%  {3}" -f $global:ResourceCategories[$i], $peak, $share, (Format-Sparkline -Values $values -Min 0 -Max $totalGb)) -ForegroundColor DarkGray
    }
}
