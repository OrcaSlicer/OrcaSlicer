# Build the per-vendor system preset caches (one <vendor>.opc per vendor) by
# running the generate_system_cache.exe dev tool against a profiles directory,
# and make every profiles directory named on the command line ship-ready:
# install the caches into it and delete the preset JSONs they replace, so a
# build ships one copy of its presets instead of two.
#
#   scripts\build_preset_cache.ps1 [build_dir] [target_dir ...]
#
#   build_dir   defaults to "build"
#   target_dir  profiles directories to ship into. Caches are generated into
#               the source tree's resources\profiles, which is what every
#               packaging step copies from; a target may be that same
#               directory, which then only gets pruned.
#   -PruneSource
#               allow a target that is the directory the caches were generated
#               into (resources\profiles). Pruning it deletes the checkout's
#               own preset JSONs, which is a packaging step - not something a
#               build should do to a working tree by surprise. CI passes it.
#
# Shipping deletes, so it is a CI packaging step. A vendor's own <vendor>.json
# goes along with its preset JSONs: the cache carries the vendor profile and
# the version it was built at, so discovery, version checks and installing all
# read it there. Only a vendor that has a cache is pruned, so non-vendor JSONs
# (blacklist.json) are left alone, as are the vendor directories themselves -
# thumbnails, covers and bed models still live there. Only the preset files
# the vendor profile lists are deleted: data files beside them (BBL's
# filaments_color_codes.json, cli_config.json, ...) are read at runtime.

param(
    [string]$BuildDir = "build",
    [switch]$PruneSource,
    [string[]]$TargetDir = @()
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

# Validate build tree exists
if (-not (Test-Path $BuildDir -PathType Container)) {
    throw "ERROR: build tree not found: $BuildDir"
}

# Newest match wins: a stale binary silently produces a stale cache layout.
$tool = Get-ChildItem -Path $BuildDir -Filter "generate_system_cache.exe" -Recurse -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $tool) {
    throw "ERROR: generate_system_cache.exe not found under $BuildDir - build with -DORCA_TOOLS=ON`n       cmake -S $RepoRoot -B $BuildDir -DORCA_TOOLS=ON"
}

# Determine config from build dir name or default to Release
$config = "Release"
foreach ($c in @("Debug", "Release", "RelWithDebInfo", "MinSizeRel")) {
    if ($tool.DirectoryName -match $c) { $config = $c; break }
}

Write-Host "Building generate_system_cache in $BuildDir ($config)"
& cmake --build $BuildDir --config $config --target generate_system_cache
if ($LASTEXITCODE -ne 0) {
    throw "ERROR: could not build generate_system_cache - configure the build tree with -DORCA_TOOLS=ON:`n       cmake -S $RepoRoot -B $BuildDir -DORCA_TOOLS=ON"
}

# Re-find the tool after build (in case it was just built)
$tool = Get-ChildItem -Path $BuildDir -Filter "generate_system_cache.exe" -Recurse -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $tool) {
    throw "ERROR: generate_system_cache.exe not found under $BuildDir - build with -DORCA_TOOLS=ON"
}

$Profiles = Join-Path $RepoRoot "resources\profiles"
if (-not (Test-Path $Profiles -PathType Container)) {
    throw "ERROR: profiles directory not found: $Profiles"
}
$Profiles = (Get-Item $Profiles).FullName

# Add the slicer's runtime DLL directory to PATH so generate_system_cache.exe
# can resolve its dependencies (TKernel.dll etc.) without a full install step.
$dllDir = Get-ChildItem -Path $BuildDir -Filter "TKernel.dll" -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($dllDir) {
    $dllPath = (Split-Path -Parent $dllDir.FullName)
    Write-Host "Found TKernel.dll at: $dllPath"
    $env:PATH = $dllPath + ";" + $env:PATH
} else {
    Write-Warning "TKernel.dll NOT FOUND under $BuildDir - generate_system_cache.exe may fail to load dependencies"
    # Search for any DLLs in the build dir
    $dlls = Get-ChildItem -Path $BuildDir -Filter "*.dll" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 10
    Write-Host "Found DLLs in build dir:"
    $dlls | ForEach-Object { Write-Host "  $($_.FullName)" }
}
Write-Host "PATH for generate_system_cache.exe: $env:PATH"

# Also check if the tool itself exists and is executable
Write-Host "Tool path: $($tool.FullName)"
Write-Host "Tool exists: $(Test-Path $tool.FullName)"

Write-Host "Generating per-vendor preset caches in $Profiles"
Write-Host "Running: $($tool.FullName) --path $Profiles --log_level 2"

# Start clean so vendors that went away - and caches written by older tool
# versions - don't linger next to the freshly generated ones.
Get-ChildItem -Path $Profiles -Filter "*.opc" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -Path $Profiles -Filter "*.cache" -ErrorAction SilentlyContinue | Remove-Item -Force

& $tool.FullName --path $Profiles --log_level 2
$exitCode = $LASTEXITCODE
Write-Host "generate_system_cache.exe exited with code: $exitCode"
if ($exitCode -ne 0) { exit $exitCode }

# Process each target directory
foreach ($target in $TargetDir) {
    if (-not (Test-Path $target -PathType Container)) {
        throw "ERROR: profiles directory not found: $target"
    }
    $target = (Get-Item $target).FullName

    if ($target -eq $Profiles -and -not $PruneSource) {
        Write-Host ("{0}: skipped - this is where the caches were generated." -f $target)
        Write-Host "  Pass -PruneSource to prune it; that deletes this checkout's preset JSONs."
        continue
    }

    if ($target -ne $Profiles) {
        Copy-Item -Path (Join-Path $Profiles "*.opc") -Destination $target -Force
    }

    $shipped = 0
    $pruned = 0
    foreach ($opc in (Get-ChildItem -Path $Profiles -Filter "*.opc" -ErrorAction SilentlyContinue)) {
        $shipped++
        $vendor = $opc.BaseName
        $vendorDir = Join-Path $target $vendor

        if (Test-Path $vendorDir -PathType Container) {
            # Prune the vendor directory: it reads the profile's preset list,
            # and with -PruneSource the profile is the target's own.
            $vendorJson = Join-Path $Profiles "$vendor.json"
            if (Test-Path $vendorJson) {
                $profile = Get-Content -LiteralPath $vendorJson -Raw -Encoding UTF8 | ConvertFrom-Json
                $pruneCount = 0
                foreach ($key in @("machine_model_list", "process_list", "filament_list", "machine_list")) {
                    foreach ($entry in $profile.$key) {
                        $f = Join-Path $vendorDir $entry.sub_path
                        if (Test-Path -LiteralPath $f -PathType Leaf) {
                            Remove-Item -LiteralPath $f -Force
                            $pruneCount++
                        }
                    }
                }
                $pruned += $pruneCount
            }

            # Deepest first, so a directory the delete above emptied goes too
            $emptyDirs = Get-ChildItem -Path $vendorDir -Directory -Recurse -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending
            foreach ($d in $emptyDirs) {
                Remove-Item -LiteralPath $d.FullName -Force -ErrorAction SilentlyContinue
            }
        }

        if (Test-Path (Join-Path $target "$vendor.json") -PathType Leaf) {
            Remove-Item -LiteralPath (Join-Path $target "$vendor.json") -Force
            $pruned++
        }
    }

    Write-Host ("{0}: $shipped caches, dropped $pruned preset JSONs" -f $target)
}
