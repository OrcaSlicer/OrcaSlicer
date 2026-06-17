<#
.SYNOPSIS
    Cleans and fills empty translations in .po files (preserves all comments and formatting).
.DESCRIPTION
    1) For all entries EXCEPT those in $targetStrings: if any msgstr variant equals its source,
       reset that variant to "".
    2) For entries whose msgid is in $targetStrings: if a msgstr variant is empty, set it to its source.
.NOTES
    For plural entries, add the SINGULAR msgid to $targetStrings.
#>

param(
    [string]$Directory = (Resolve-Path "$PSScriptRoot\..\localization\i18n" -ErrorAction SilentlyContinue).Path
)

if (-not (Test-Path $Directory)) {
    Write-Warning "Default directory not found. Using current directory."
    $Directory = (Get-Location).Path
}

# ---------- Customize this list with the msgid values you want to fill ----------
# IMPORTANT: For plural entries, add the SINGULAR form (the first msgid).
$targetStrings = @(
	"Orca String Hell",
	"Z-Hop",
    "SVG",
    "mm",
    "in",
    "Ctrl+",
    "Alt+",
    "*",
    "WebView2 Runtime",
    "3DBenchy",
    "Cali Cat",
    "Hotend",
    "Copyright",
    "Factor K",
    "Factor N",
    "AMS",
    "mm³",
    "Pressure Advance",
    "PA: ",
    "Shift+",
    "mm/s",
    "mm/s²",
    "mm³/s",
    "°C",
    "Input Shaping",
    "VFA",
    "Timelapse",
    "Zoom",
    "bits",
    "Bambu Cool Plate SuperTack",
    "Cool Plate (SuperTack)",
    "—> ",
    "ms",
    "IP",
    "s",
    "mm²",
    "g/cm³",
    "TPMS-D",
    "TPMS-FK",
    "accel_to_decel",
    "HRC",
    "I3",
    "Hbot",
    "Delta",
    "%",
    "Klipper",
    "mm³/s²",
    "Direct Drive",
    "Bowden",
    "∆℃",
    "mtcpp",
    "...",
    "Jerk",
    "Jerk: ",
    "Jerk (mm/s)",
    "mstpp",
    "China",
	"Billow",
    "Imperial",
    "Anti-aliasing",
    "DEV host: api-dev.bambu-lab.com/v1",
    "QA  host: api-qa.bambu-lab.com/v1",
    "PRE host: api-pre.bambu-lab.com/v1",
    "Junction Deviation",
    "Jerk(XY)",
    "Beta",
    "Zig Zag",
    "Voronoi",
    "Perlin",
    "Ripple",
    "CoreXY",
    "FPS",
    "MZV",
    "ZV",
    "ZVD",
    "ZVDD",
    "ZVDDD",
    "EI",
    "EI2",
    "2HUMP_EI",
    "EI3",
    "3HUMP_EI",
    "DAA",
    "X",
    "Y",
    "Arachne",
    "- ℃",
    "DDE",
    "PLA",
    "ABS/ASA",
    "PETG",
    "PCTG",
    "TPU",
    "PA-CF",
    "PET-CF",
    "Hz",
    "SCV-V2",
    "Time-lapse",
    "ID",
    "Wiki",
    "Autodesk FDM Test"
)
# -------------------------------------------------------------------------------

Write-Host "Searching .po files in: $Directory" -ForegroundColor Cyan
$poFiles = Get-ChildItem -Path $Directory -Filter "*.po" -Recurse -File
if ($poFiles.Count -eq 0) { Write-Host "No .po files found." -ForegroundColor Yellow; exit }
Write-Host "Found $($poFiles.Count) file(s)." -ForegroundColor Cyan

foreach ($file in $poFiles) {
    Write-Host "`nProcessing: $($file.Name)" -ForegroundColor Cyan
    $lines = Get-Content -Path $file.FullName
    $total = $lines.Count
    $modified = $false

    # ----- Parser: collect entries with line indexes -----
    $entries = @()
    $i = 0
    $inEntry = $false
    $current = $null

    while ($i -lt $lines.Count) {
        $line = $lines[$i]

        if (-not $inEntry -and ($line -match '^\s*$' -or $line -match '^#')) {
            $i++
            continue
        }

        if ($line -match '^msgid\s') {
            if ($inEntry -and $current) { $entries += $current }
            $current = @{
                msgid       = ""
                msgid_plural = $null
                msgstrs     = @{}
                startLine   = $i
                endLine     = $i
            }
            $inEntry = $true

            $fullMsgId = ""
            $j = $i
            while ($j -lt $lines.Count -and $lines[$j] -match '^msgid\s+"(.*)"') {
                $fullMsgId += $matches[1]
                $j++
                while ($j -lt $lines.Count -and $lines[$j] -match '^"\s*(.*)"') {
                    $fullMsgId += $matches[1]
                    $j++
                }
                break
            }
            $current.msgid = $fullMsgId
            $current.endLine = $j - 1
            $i = $j
            continue
        }

        if ($inEntry -and $current) {
            if ($line -match '^\s*$' -or $line -match '^msgid\s') {
                $entries += $current
                $inEntry = $false
                $current = $null
                continue
            }

            if ($line -match '^msgid_plural\s+"(.*)"') {
                $plural = $matches[1]
                $j = $i + 1
                while ($j -lt $lines.Count -and $lines[$j] -match '^"\s*(.*)"') {
                    $plural += $matches[1]
                    $j++
                }
                $current.msgid_plural = $plural
                $current.endLine = $j - 1
                $i = $j
                continue
            }

            if ($line -match '^msgstr(?:\[(\d+)\])?\s+"(.*)"') {
                $index = $matches[1]
                if ($index -eq $null) { $index = "" }
                $str = $matches[2]
                $start = $i
                $j = $i + 1
                while ($j -lt $lines.Count -and $lines[$j] -match '^"\s*(.*)"') {
                    $str += $matches[1]
                    $j++
                }
                $end = $j - 1
                $current.msgstrs[$index] = @{
                    value   = $str
                    startLine = $start
                    endLine   = $end
                }
                $current.endLine = $end
                $i = $j
                continue
            }

            $current.endLine = $i
            $i++
        }
        else {
            $i++
        }
    }
    if ($inEntry -and $current) { $entries += $current }

    # ----- Process entries -----
    Write-Progress -Activity "Processing $($file.Name)" -Status "Cleaning and filling" -PercentComplete 50

    foreach ($entry in $entries) {
        $msgid = $entry.msgid
        if ([string]::IsNullOrEmpty($msgid)) { continue }  # skip header

        $isTarget = $targetStrings -contains $msgid
        Write-Host "  Entry: '$msgid' (Target: $isTarget)" -ForegroundColor DarkGray

        # STEP 1: Clean (non-target only)
        if (-not $isTarget) {
            $keys = @($entry.msgstrs.Keys)
            foreach ($idx in $keys) {
                $info = $entry.msgstrs[$idx]
                $currentVal = $info.value
                if ($idx -eq "") {
                    $source = $msgid
                } else {
                    $source = if ($entry.msgid_plural -ne $null) { $entry.msgid_plural } else { $msgid }
                }
                if ($currentVal -eq $source) {
                    $entry.msgstrs[$idx].value = ""
                    $start = $info.startLine
                    $end = $info.endLine
                    if ($idx -eq "") {
                        $lines[$start] = 'msgstr ""'
                    } else {
                        $lines[$start] = 'msgstr[' + $idx + '] ""'
                    }
                    for ($j = $start + 1; $j -le $end; $j++) { $lines[$j] = $null }
                    $modified = $true
                    Write-Host "    Cleaned: [$idx] (was equal to source)" -ForegroundColor Yellow
                }
            }
        }

        # STEP 2: Fill (target only)
        if ($isTarget) {
            $keys = @($entry.msgstrs.Keys)
            foreach ($idx in $keys) {
                $info = $entry.msgstrs[$idx]
                $currentVal = $info.value
                if ([string]::IsNullOrEmpty($currentVal)) {
                    if ($idx -eq "") {
                        $source = $msgid
                    } else {
                        $source = if ($entry.msgid_plural -ne $null) { $entry.msgid_plural } else { $msgid }
                    }
                    $entry.msgstrs[$idx].value = $source
                    $start = $info.startLine
                    $end = $info.endLine
                    if ($idx -eq "") {
                        $lines[$start] = 'msgstr "' + $source + '"'
                    } else {
                        $lines[$start] = 'msgstr[' + $idx + '] "' + $source + '"'
                    }
                    for ($j = $start + 1; $j -le $end; $j++) { $lines[$j] = $null }
                    $modified = $true
                    Write-Host "    Filled: [$idx] with '$source'" -ForegroundColor Green
                }
            }
        }
    }

    if ($modified) {
        $lines = $lines | Where-Object { $_ -ne $null }
        $lines | Set-Content -Path $file.FullName -Encoding UTF8
        Write-Host "  File updated." -ForegroundColor Green
    } else {
        Write-Host "  No changes needed." -ForegroundColor Gray
    }
    Write-Progress -Activity "Processing $($file.Name)" -Completed
}

Write-Host "`nDone." -ForegroundColor Magenta