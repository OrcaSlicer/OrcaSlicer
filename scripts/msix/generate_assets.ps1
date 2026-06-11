# Generates the MSIX package logo assets from the master vector logo
# (resources\images\OrcaSlicer_gradient_circle.svg). Each PNG is rendered from
# the SVG at its exact target size (true per-size vector rasterization, not
# downscaled from one bitmap), preserving alpha transparency in the corners
# outside the circle (the manifest uses BackgroundColor="transparent").
#
# Run once locally on Windows (re-run only if the logo changes), then commit
# the PNGs in assets/. CI never runs this script.
#
# Prerequisite: Python 3 with the resvg-py package (pip install resvg-py).
# It bundles the resvg SVG renderer, needed because the master SVG uses
# gradients with alpha-fade stops that System.Drawing cannot rasterize.
param(
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$source   = Join-Path $repoRoot 'resources\images\OrcaSlicer_gradient_circle.svg'
$outDir   = Join-Path $PSScriptRoot 'assets'
New-Item -ItemType Directory -Force $outDir | Out-Null

$sizes = [ordered]@{
    'Square150x150Logo.png'                              = 150
    'Square44x44Logo.png'                                = 44
    'Square44x44Logo.targetsize-44_altform-unplated.png' = 44
    'StoreLogo.png'                                      = 50
}

$py = @'
import sys
from pathlib import Path

import resvg_py

svg, out_dir = sys.argv[1], Path(sys.argv[2])
for spec in sys.argv[3:]:
    name, px = spec.rsplit('=', 1)
    px = int(px)
    data = resvg_py.svg_to_bytes(svg_path=svg, width=px, height=px)
    (out_dir / name).write_bytes(bytes(data))
    print(f'Wrote {name} ({px}x{px})')
'@

$renderScript = Join-Path $env:TEMP 'orca_msix_render.py'
Set-Content -Path $renderScript -Value $py -Encoding utf8
try {
    $specs = foreach ($name in $sizes.Keys) { "$name=$($sizes[$name])" }
    & $Python $renderScript $source $outDir @specs
    if ($LASTEXITCODE -ne 0) {
        throw 'resvg render failed. Is resvg-py installed? (pip install resvg-py)'
    }
}
finally {
    Remove-Item $renderScript -ErrorAction SilentlyContinue
}
