# Generates the MSIX package logo assets from the 192px master logo.
# Run once on Windows (re-run if the logo changes), then commit the PNGs in assets/.
# Scale-200 variants would need a >192px master; regenerate from OrcaSlicer.svg if ever needed.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$source   = Join-Path $repoRoot 'resources\images\OrcaSlicer_192px.png'
$outDir   = Join-Path $PSScriptRoot 'assets'
New-Item -ItemType Directory -Force $outDir | Out-Null

$sizes = [ordered]@{
    'Square150x150Logo.png'                              = 150
    'Square44x44Logo.png'                                = 44
    'Square44x44Logo.targetsize-44_altform-unplated.png' = 44
    'StoreLogo.png'                                      = 50
}

$src = [System.Drawing.Image]::FromFile($source)
foreach ($name in $sizes.Keys) {
    $px  = $sizes[$name]
    $bmp = New-Object System.Drawing.Bitmap($px, $px)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $gfx.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $gfx.DrawImage($src, 0, 0, $px, $px)
    $gfx.Dispose()
    $bmp.Save((Join-Path $outDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "Wrote $name (${px}x${px})"
}
$src.Dispose()
