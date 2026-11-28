<#
.SYNOPSIS
    Deletes the preset JSONs the vendor caches replace from profiles directories about to ship.

.DESCRIPTION
    build_preset_cache.bat runs this once, after generate_system_cache wrote one <vendor>.opc per
    vendor into the source profiles directory, naming every directory to make ship-ready. Only a
    vendor that has a cache is pruned, and only the preset files its vendor profile lists: data
    files beside them (BBL's filaments_color_codes.json, cli_config.json, ...) are read at runtime
    and stay, as do directories still holding covers, meshes or those data files. The vendor's own
    <vendor>.json goes too: the cache carries the vendor profile and the version it was built at.

    Every vendor profile is read from the source before any target is touched. A target may be the
    source directory itself, and pruning it deletes the vendor profiles, so reading them first is
    what keeps the lists intact for the targets after it. One process does all the deleting, which
    keeps this fast on a runner where starting PowerShell or deleting a file is slow.

.PARAMETER Profiles
    The profiles directory the caches were generated into.

.PARAMETER Targets
    The profiles directories to prune. Each already holds the caches.

.EXAMPLE
    powershell -File scripts\prune_cached_presets.ps1 -Profiles resources\profiles build\OrcaSlicer\resources\profiles
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Profiles,
    [Parameter(Mandatory, ValueFromRemainingArguments)] [string[]] $Targets
)
$ErrorActionPreference = 'Stop'

$Profiles = [IO.Path]::GetFullPath($Profiles)
$caches = @(Get-ChildItem -LiteralPath $Profiles -Filter *.opc -File)
if ($caches.Count -eq 0) { throw "no .opc caches in $Profiles" }

# vendor -> the preset paths its profile lists, relative to the vendor directory
$listed = @{}
foreach ($cache in $caches) {
    $vendor = $cache.BaseName
    $profile = Join-Path $Profiles "$vendor.json"
    if (-not [IO.File]::Exists($profile)) { throw "no $vendor.json beside $vendor.opc in $Profiles" }
    $paths = @()
    $json = ConvertFrom-Json ([IO.File]::ReadAllText($profile))
    foreach ($list in 'machine_model_list', 'process_list', 'filament_list', 'machine_list') {
        foreach ($entry in $json.$list) {
            if ($entry.sub_path) { $paths += $entry.sub_path }
        }
    }
    $listed[$vendor] = $paths
}

foreach ($target in $Targets) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $target = [IO.Path]::GetFullPath($target)
    if (-not [IO.Directory]::Exists($target)) { throw "profiles directory not found: $target" }
    $pruned = 0
    foreach ($vendor in $listed.Keys) {
        $dir = Join-Path $target $vendor
        if ([IO.Directory]::Exists($dir)) {
            foreach ($sub in $listed[$vendor]) {
                $file = Join-Path $dir $sub
                if ([IO.File]::Exists($file)) {
                    [IO.File]::Delete($file)
                    $pruned++
                }
            }
            # Deepest first, so a directory the deletes above emptied goes too.
            $dirs = @([IO.Directory]::GetDirectories($dir, '*', [IO.SearchOption]::AllDirectories))
            foreach ($candidate in ($dirs | Sort-Object Length -Descending)) {
                if (-not [IO.Directory]::EnumerateFileSystemEntries($candidate).GetEnumerator().MoveNext()) {
                    [IO.Directory]::Delete($candidate)
                }
            }
        }
        $profile = Join-Path $target "$vendor.json"
        if ([IO.File]::Exists($profile)) {
            [IO.File]::Delete($profile)
            $pruned++
        }
    }
    '{0}: {1} caches, dropped {2} preset JSONs in {3:n1} s' -f $target, $caches.Count, $pruned, $watch.Elapsed.TotalSeconds
}
