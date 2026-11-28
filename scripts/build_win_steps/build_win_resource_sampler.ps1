# Resource sampler for Start-ResourceMonitor.
#
# Runs as a background job because the parent is blocked inside the compiler
# call, and a job cannot write the parent's variables - it is a separate
# process - so samples come back through the job output stream and are drained
# on stop. Counting only the build-tree executables is what separates "the
# compiler used 9 cores" from "the machine was 40% busy"; machine-wide counters
# cannot.

param([int]$ReportInterval = 15)

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
        Update-PeakRssSplit -RssNow $rssNow -RssBy $rssBy -Categories $categories -RefPeakRss ([ref]$peakRss) -RefPeakRssBy ([ref]$peakRssBy)

        if ($window -ge $ReportInterval) {
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

function Update-PeakRssSplit {
    param(
        [double]$RssNow,
        $RssBy,
        $Categories,
        [ref]$RefPeakRss,
        [ref]$RefPeakRssBy
    )
    # Freeze the split at the peak instant: taking it at report time
    # would mix instants and the parts would not add up to the total.
    if ($RssNow -gt $RefPeakRss.Value) {
        $RefPeakRss.Value = $RssNow
        foreach ($category in $Categories) { $RefPeakRssBy.Value[$category] = $RssBy[$category] }
    }
}