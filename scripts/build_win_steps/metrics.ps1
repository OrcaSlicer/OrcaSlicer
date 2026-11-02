# Unit: Build process metrics
# Monitors build-process CPU, cores and RAM during configure/build.
# Renders charts at the end via the PwshSpectreConsole helper.

function Stop-ProcessTree {
    param([int]$ProcessId)
    try {
        $proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
        if ($proc) {
            $children = Get-CimInstance Win32_Process -Filter "ParentProcessId=$ProcessId" -ErrorAction SilentlyContinue
            foreach ($child in $children) {
                Stop-ProcessTree -ProcessId $child.ProcessId
            }
            $proc | Stop-Process -Force -ErrorAction SilentlyContinue
        }
    } catch {
        # Process tree cleanup must never fail the build.
    }
}

function Start-BuildMonitor {
    param([string]$CsvPath, [int]$IntervalSeconds)
    $worker = {
        param($Path, $Interval)
        $header = "timestamp,build_cpu_percent,build_cores,build_rss_gb,cores_compiler,cores_linker,cores_build,cores_ccache,cpu_compiler,cpu_linker,cpu_build,cpu_ccache,ram_compiler,ram_linker,ram_build,ram_ccache"
        Set-Content -LiteralPath $Path -Value $header -Encoding ASCII

        # Names seen during a recorded compile, plus the MSVC equivalents.
        $toolCategory = @{
            "clang-cl" = "compiler"
            "clang"    = "compiler"
            "clang++"  = "compiler"
            "cl"       = "compiler"
            "llvm-rc"  = "compiler"
            "lld-link" = "linker"
            "link"     = "linker"
            "llvm-lib" = "linker"
            "ninja"    = "build"
            "cmake"    = "build"
            "msbuild"  = "build"
            "git"      = "build"
            "python"   = "build"
            "ccache"   = "ccache"
        }
        $categories = @("compiler", "linker", "build", "ccache")
        $logical = [Environment]::ProcessorCount
        $reportInterval = [math]::Max(1, $Interval)

        function Write-Sample($Time, $CpuPercent, $Cores, $RssGb, $CategoryCores, $CategoryCpu, $CategoryRss) {
            $categoryText = (@($CategoryCores) | ForEach-Object {
                $_.ToString("0.00", [Globalization.CultureInfo]::InvariantCulture)
            }) -join ","
            $cpuText = (@($CategoryCpu) | ForEach-Object {
                $_.ToString("0.0", [Globalization.CultureInfo]::InvariantCulture)
            }) -join ","
            $ramText = (@($CategoryRss) | ForEach-Object {
                $_.ToString("0.00", [Globalization.CultureInfo]::InvariantCulture)
            }) -join ","
            $line = "{0},{1},{2},{3},{4},{5},{6}" -f $Time.ToString("o"),
                $CpuPercent.ToString("0.0", [Globalization.CultureInfo]::InvariantCulture),
                $Cores.ToString("0.00", [Globalization.CultureInfo]::InvariantCulture),
                $RssGb.ToString("0.00", [Globalization.CultureInfo]::InvariantCulture),
                $categoryText, $cpuText, $ramText
            Add-Content -LiteralPath $Path -Value $line -Encoding ASCII
        }

        $start = Get-Date
        $zeroCores = @(0.0, 0.0, 0.0, 0.0)
        $zeroCpu = @(0.0, 0.0, 0.0, 0.0)
        $zeroRss = @(0.0, 0.0, 0.0, 0.0)
        Write-Sample $start 0.0 0.0 0.0 $zeroCores $zeroCpu $zeroRss
        $windowStart = $start
        $accumulatedCpu = @{}
        foreach ($category in $categories) { $accumulatedCpu[$category] = 0.0 }
        $maxRss = 0.0
        $maxRssCategories = @(0.0, 0.0, 0.0, 0.0)
        $previousCpu = @{}

        while ($true) {
            try {
                $now = Get-Date
                $elapsedInWindow = ($now - $windowStart).TotalSeconds
                $currentCpu = @{}
                $currentCategory = @{}
                $currentRss = @{}
                foreach ($name in $toolCategory.Keys) {
                    $category = $toolCategory[$name]
                    foreach ($process in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
                        try {
                            $id = [int]$process.Id
                            $cpuSeconds = if ($null -ne $process.CPU) { [double]$process.CPU } else { 0.0 }
                            $currentCpu[$id] = $cpuSeconds
                            $currentCategory[$id] = $category
                            $currentRss[$id] = [double]$process.WorkingSet64
                        } catch {
                            # The process can exit between enumeration and reading its counters.
                        }
                    }
                }

                foreach ($id in $currentCpu.Keys) {
                    $lastCpu = $previousCpu[$id]
                    if ($null -eq $lastCpu) {
                        # The process started after the window opened, so only the
                        # part of its lifetime that falls inside the window counts.
                        $delta = [math]::Min($currentCpu[$id], [math]::Max(0.0, $elapsedInWindow))
                    } elseif ($currentCpu[$id] -ge $lastCpu) {
                        $delta = $currentCpu[$id] - $lastCpu
                    } else {
                        $delta = 0.0
                    }
                    if ($delta -gt 0) {
                        $accumulatedCpu[$currentCategory[$id]] += $delta
                    }
                }

                $rssNow = 0.0
                $rssByCategory = @{}
                foreach ($category in $categories) { $rssByCategory[$category] = 0.0 }
                foreach ($id in $currentRss.Keys) {
                    $rssNow += $currentRss[$id]
                    $rssByCategory[$currentCategory[$id]] += $currentRss[$id]
                }
                if ($rssNow -gt $maxRss) {
                    $maxRss = $rssNow
                    # Freeze the split at the peak: taking it at report time would
                    # compare category values from a different instant and the
                    # stacked bars would not add up to the total.
                    $maxRssCategories = @($categories | ForEach-Object { $rssByCategory[$_] })
                }

                if ($elapsedInWindow -ge $reportInterval) {
                    $categoryCores = @()
                    $categoryCpu = @()
                    foreach ($category in $categories) {
                        $coresValue = [math]::Max(0.0, $accumulatedCpu[$category] / $elapsedInWindow)
                        $categoryCores += $coresValue
                        $categoryCpu += if ($logical -gt 0) { 100.0 * $coresValue / $logical } else { 0.0 }
                    }
                    $cores = ($categoryCores | Measure-Object -Sum).Sum
                    $cpuPercent = ($categoryCpu | Measure-Object -Sum).Sum
                    $categoryRss = @($maxRssCategories | ForEach-Object { $_ / 1GB })
                    Write-Sample $now $cpuPercent $cores ($maxRss / 1GB) $categoryCores $categoryCpu $categoryRss
                    $windowStart = $now
                    foreach ($category in $categories) { $accumulatedCpu[$category] = 0.0 }
                    $maxRss = 0.0
                    $maxRssCategories = @(0.0, 0.0, 0.0, 0.0)
                }

                $previousCpu = $currentCpu
            } catch {
                # Sampling must never fail the build.
            }
            # One second keeps short-lived compiler and linker processes in the
            # accounting; the report interval only controls CSV size.
            Start-Sleep -Seconds 1
        }
    }
    return Start-Job -ScriptBlock $worker -ArgumentList $CsvPath, $IntervalSeconds
}
