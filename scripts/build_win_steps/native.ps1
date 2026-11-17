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

# Resource sampling, split by tool.
#
# The sampler runs in a background job because the parent is blocked inside the
# compiler call, and a job cannot write the parent's variables - it is a separate
# process - so samples come back through the job output stream and are drained on
# stop. Counting only the build-tree executables is what separates "the compiler
# used 9 cores" from "the machine was 40% busy"; machine-wide counters cannot.
$global:ResourceMonitorJob = $null
$global:ResourceSamples = @()
$global:ResourceCategories = @('compiler', 'linker', 'build', 'ccache')

function Start-ResourceMonitor {
    param([int]$IntervalSeconds = 15)
    $global:ResourceSamples = @()
    $global:ResourceMonitorStarted = $true

    $global:ResourceMonitorJob = Start-Job -ScriptBlock {
        param($reportInterval)

        $toolCategory = @{
            'clang-cl' = 'compiler'; 'clang' = 'compiler'; 'clang++' = 'compiler'
            'cl'       = 'compiler'; 'llvm-rc' = 'compiler'
            'lld-link' = 'linker';   'link' = 'linker';  'llvm-lib' = 'linker'
            'ninja'    = 'build';    'cmake' = 'build';  'msbuild' = 'build'
            'ccache'   = 'ccache'
        }
        $categories = @('compiler', 'linker', 'build', 'ccache')
        $logical = [Environment]::ProcessorCount
        $totalGb = [math]::Round((Get-CimInstance Win32_OperatingSystem).TotalVisibleMemorySize / 1MB, 1)

        $t0 = Get-Date
        $windowStart = $t0
        $previousCpu = @{}
        $accumulated = @{}
        foreach ($category in $categories) { $accumulated[$category] = 0.0 }
        $peakRss = 0.0
        $peakRssBy = @{}
        foreach ($category in $categories) { $peakRssBy[$category] = 0.0 }

        while ($true) {
            try {
                $now = Get-Date
                $window = ($now - $windowStart).TotalSeconds
                $currentCpu = @{}
                $currentCategory = @{}
                $currentRss = @{}

                # One enumeration, filtered by name. Asking Get-Process per tool
                # name costs 13 times as much and still only sees whatever happens
                # to be alive at that instant.
                foreach ($process in @(Get-Process -ErrorAction SilentlyContinue)) {
                    try {
                        $category = $toolCategory[$process.ProcessName]
                        if (-not $category) { continue }
                        $processId = [int]$process.Id
                        $currentCpu[$processId] = if ($null -ne $process.CPU) { [double]$process.CPU } else { 0.0 }
                        $currentCategory[$processId] = $category
                        $currentRss[$processId] = [double]$process.WorkingSet64
                    } catch {
                        # The process can exit between enumeration and the read.
                    }
                }

                foreach ($processId in $currentCpu.Keys) {
                    $last = $previousCpu[$processId]
                    # A process that appeared mid-window has no earlier counter, so
                    # only the part of its lifetime inside the window counts.
                    $delta = if ($null -eq $last) { [math]::Min($currentCpu[$processId], [math]::Max(0.0, $window)) }
                             elseif ($currentCpu[$processId] -ge $last) { $currentCpu[$processId] - $last }
                             else { 0.0 }
                    if ($delta -gt 0) { $accumulated[$currentCategory[$processId]] += $delta }
                }

                $rssNow = 0.0
                $rssBy = @{}
                foreach ($category in $categories) { $rssBy[$category] = 0.0 }
                foreach ($processId in $currentRss.Keys) {
                    $rssNow += $currentRss[$processId]
                    $rssBy[$currentCategory[$processId]] += $currentRss[$processId]
                }
                # Freeze the split at the peak instant: taking it at report time
                # would mix instants and the parts would not add up to the total.
                if ($rssNow -gt $peakRss) {
                    $peakRss = $rssNow
                    foreach ($category in $categories) { $peakRssBy[$category] = $rssBy[$category] }
                }

                if ($window -ge $reportInterval) {
                    $cores = @()
                    foreach ($category in $categories) {
                        # Cores, not percent: 400% on 8 cores is unreadable, 4.0 is not.
                        $cores += [math]::Max(0.0, $accumulated[$category] / $window)
                    }
                    [pscustomobject]@{
                        Elapsed = ($now - $t0).TotalSeconds
                        Cores   = $cores
                        RssGb   = $peakRss / 1GB
                        RssBy   = @($categories | ForEach-Object { $peakRssBy[$_] / 1GB })
                        TotalGb = $totalGb
                        Logical = $logical
                    }
                    $windowStart = $now
                    foreach ($category in $categories) { $accumulated[$category] = 0.0 }
                    $peakRss = 0.0
                    foreach ($category in $categories) { $peakRssBy[$category] = 0.0 }
                }
                $previousCpu = $currentCpu
            } catch {
                # Sampling must never fail the build.
            }
            # A compile can live 200 ms, so the sampling tick has to be well under
            # that or the process is never seen and its CPU goes uncounted. The
            # report interval is unrelated: it only sets chart resolution.
            Start-Sleep -Milliseconds 250
        }
    } -ArgumentList $IntervalSeconds

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
