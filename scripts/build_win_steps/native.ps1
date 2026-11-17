# Unit: Native process invocation
# Shared helper so every CMake/Ninja call streams its output, keeps a log and
# turns a non-zero exit code into a terminating error with a visible tail.

function Invoke-BuildStep {
    param(
        [string]$Label,
        [string]$LogPath,
        [string[]]$Command,
        [string[]]$Arguments
    )

    $previousPreference = $ErrorActionPreference
    $exceptionCaught = $null
    $buffer = New-Object System.Text.StringBuilder
    # Native tools report progress and errors on stderr. With 'Stop' the first
    # such line becomes a terminating error that hides the real failure, so the
    # preference is relaxed for the duration of the call.
    $ErrorActionPreference = "Continue"
    $lastLine = @("")  # Array to hold mutable reference across ForEach-Object scope
    try {
        $proc = & $Command @Arguments 2>&1
        $proc | ForEach-Object {
            # Skip empty/whitespace-only lines
            if (-not ($_.ToString().Trim())) { return }
            $text = $_.ToString()
            # Merging native stderr with 2>&1 surfaces bare type names where the
            # record carries no message; they split the real error text.
            if ($text.Trim() -match '^System\.Management\.Automation\.\w+Exception$') { return }
            $span = if ($global:BuildStarted) { (Get-Date) - $global:BuildStarted } else { [TimeSpan]::Zero }
            $ts = "[{0}:{1:00}:{2:00}]" -f [math]::Floor($span.TotalHours), ([math]::Floor($span.TotalMinutes) % 60), [math]::Round($span.Seconds)
            $raw = "$ts $text"
            # Skip exact duplicate lines (CMake sometimes outputs same message twice)
            if ($raw -eq $lastLine[0]) { return }
            $lastLine[0] = $raw
            # Color real errors red (CMake writes progress to stderr too, so the
            # stream alone says nothing), normal output gray
            $isRealError = $raw -match '(?i)(error:|fatal:|FAILED:|^error\s|^fatal\s|^\s*error\s)'
            if ($isRealError) { Write-Host $raw -ForegroundColor Red } else { Write-Host $raw -ForegroundColor DarkGray }
            # Buffer every line so we can show the tail on failure
            [void]$buffer.AppendLine($raw)
        }
        $exitCode = $LASTEXITCODE
        $exceptionCaught = $null
    } catch {
        $exceptionCaught = $_
        $exitCode = 1
    } finally {
        $ErrorActionPreference = $previousPreference
    }

    # Persist the full output so we can show the tail on failure
    if ($LogPath) {
        $buffer.ToString() | Set-Content -LiteralPath $LogPath -Encoding UTF8
    }

    # The tail must go through Write-Host: callers capture this function's output,
    # so anything written to the success stream is discarded when the throw unwinds.
    if ($exceptionCaught) {
        Write-Host "--- $Label threw $($exceptionCaught.GetType().Name): $($exceptionCaught.Exception.Message) ---" -ForegroundColor Red
        if ($LogPath -and (Test-Path $LogPath)) {
            Write-Host "--- Last 30 lines of $LogPath ---" -ForegroundColor DarkGray
            Get-Content $LogPath | Select-Object -Last 30 | ForEach-Object { Write-Host $_ -ForegroundColor DarkGray }
        }
        throw "$Label failed due to exception ($($exceptionCaught.GetType().Name)): $($exceptionCaught.Exception.Message)"
    }
    if ($exitCode -ne 0) {
        Write-Host ""
        Write-Host "--- $Label failed (exit $exitCode) ---" -ForegroundColor Red
        if ($LogPath -and (Test-Path $LogPath)) {
            Write-Host "--- Last 30 lines of $LogPath ---" -ForegroundColor DarkGray
            Get-Content $LogPath | Select-Object -Last 30 | ForEach-Object { Write-Host $_ -ForegroundColor DarkGray }
        }
        throw "$Label failed with exit code $exitCode"
    }
}

function New-BuildLogPath {
    param([string]$Name)
    $logDir = Join-Path $env:TEMP "orca-build-logs"
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    return (Join-Path $logDir ("{0}-{1}.log" -f (Get-Date -Format "yyyyMMdd-HHmmss"), $Name))
}

# Resource sampling. The sampler runs in a background job because the parent is
# blocked inside the compiler invocation. A job cannot write to the parent's
# variables - it is a separate process - so samples travel back through the job
# output stream and are drained on stop.
$global:ResourceMonitorJob = $null
$global:ResourceSamples = @()

function Start-ResourceMonitor {
    param([int]$IntervalSeconds = 15)
    $global:ResourceSamples = @()
    $global:ResourceMonitorStarted = $true

    $global:ResourceMonitorJob = Start-Job -ScriptBlock {
        param($interval)
        $t0 = Get-Date
        while ($true) {
            # Win32_Processor reports machine-wide load without the one-second
            # settle that Get-Counter needs on first call.
            $cpu  = (Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Measure-Object LoadPercentage -Average).Average
            $os   = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
            $disk = Get-PSDrive C -ErrorAction SilentlyContinue
            [pscustomobject]@{
                Elapsed   = ((Get-Date) - $t0).TotalSeconds
                CPU       = [double]$cpu
                RAMUsed   = ($os.TotalVisibleMemorySize - $os.FreePhysicalMemory) / 1MB
                RAMTotal  = $os.TotalVisibleMemorySize / 1MB
                DiskFree  = $disk.Free / 1GB
                DiskTotal = ($disk.Used + $disk.Free) / 1GB
            }
            Start-Sleep -Seconds $interval
        }
    } -ArgumentList $IntervalSeconds

    Write-Status "sampler" "CPU, RAM and disk every ${IntervalSeconds}s"
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
    if ($samples.Count -eq 0) {
        # Only worth a line when the sampler actually ran; a failure before step 4
        # never started it.
        if ($global:ResourceMonitorStarted) { Write-Status "sampler" "no samples collected" "Yellow" }
        return
    }

    $cpu = @($samples | ForEach-Object { [double]$_.CPU })
    $ram = @($samples | ForEach-Object { [double]$_.RAMUsed })
    $dsk = @($samples | ForEach-Object { [double]$_.DiskFree })
    $last = $samples[-1]
    $header = if ($samples.Count -gt 1) {
        # Query latency makes the real gap larger than the requested interval.
        $gap = ($last.Elapsed - $samples[0].Elapsed) / ($samples.Count - 1)
        "  Resource use ({0} samples, {1:N0}s apart)" -f $samples.Count, $gap
    } else {
        "  Resource use (1 sample)"
    }

    Write-Host ""
    Write-Host $header -ForegroundColor Cyan

    Write-Host ("  CPU    avg {0:N0}%  peak {1:N0}%" -f ($cpu | Measure-Object -Average).Average, ($cpu | Measure-Object -Maximum).Maximum)
    Write-Host ("         " + (Format-Sparkline -Values $cpu -Min 0 -Max 100)) -ForegroundColor DarkGray

    Write-Host ("  RAM    peak {0:N1} of {1:N1} GB" -f ($ram | Measure-Object -Maximum).Maximum, $last.RAMTotal)
    Write-Host ("         " + (Format-Sparkline -Values $ram -Min 0 -Max $last.RAMTotal)) -ForegroundColor DarkGray

    Write-Host ("  Disk   min {0:N1} of {1:N1} GB free" -f ($dsk | Measure-Object -Minimum).Minimum, $last.DiskTotal)
    Write-Host ("         " + (Format-Sparkline -Values $dsk -Min 0 -Max $last.DiskTotal)) -ForegroundColor DarkGray
}
