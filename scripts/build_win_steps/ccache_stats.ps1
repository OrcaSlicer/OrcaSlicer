# Unit: Ccache statistics
# Captures the ccache counters so the caller can render them in a Spectre panel.

function Show-CcacheStats {
    param([string]$Ccache, [switch]$NoMetrics)
    if (-not $Ccache -or $NoMetrics) { return @() }
    return @(& $Ccache -s 2>&1 | ForEach-Object { "$_" })
}
