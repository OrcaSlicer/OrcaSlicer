<#
.SYNOPSIS
    Tests scripts/prune_cached_presets.ps1 on a small made-up profiles tree.

.DESCRIPTION
    Each case gets a fresh tree with two cached vendors, one vendor without a cache, data files
    beside the presets and a copy of it all standing in for an install tree, adjusts it with its
    Before block if it has one, then runs the prune in its own powershell -File process, as
    build_preset_cache.bat does, and asserts on the exit code, the output and which files are left.

.EXAMPLE
    powershell -File scripts/test_prune_cached_presets.ps1
#>
$ErrorActionPreference = 'Stop'

$prune = Join-Path $PSScriptRoot 'prune_cached_presets.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) 'prune_cached_presets_tests'

# Paths are written Windows-style; the swap lets the suite also run under pwsh elsewhere.
function Write-File([string] $path, [string] $text) {
    $path = $path.Replace('\', [IO.Path]::DirectorySeparatorChar)
    New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
    Set-Content -Path $path -Encoding ascii -Value $text
}

# A source profiles directory as generate_system_cache leaves it, and an install tree copied from it.
function New-Fixture {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    $src = Join-Path $work 'src'
    Write-File "$src\Acme.json" ('{"machine_model_list":[{"sub_path":"machine/Acme One.json"}],' +
        '"process_list":[{"sub_path":"process/0.20mm Standard @Acme.json"}],' +
        '"filament_list":[{"sub_path":"filament/Acme PLA.json"},{"sub_path":"filament/not shipped.json"}],' +
        '"machine_list":[{"sub_path":"machine/Acme One 0.4 nozzle.json"}]}')
    Write-File "$src\Acme.opc" 'cache'
    foreach ($preset in 'machine\Acme One.json', 'machine\Acme One 0.4 nozzle.json',
                        'process\0.20mm Standard @Acme.json', 'filament\Acme PLA.json') {
        Write-File "$src\Acme\$preset" '{}'
    }
    Write-File "$src\Acme\machine\Acme One_cover.png" 'png'
    Write-File "$src\Acme\filament\colors.json" '{}'
    Write-File "$src\Acme\shapes\bed.stl" 'solid'
    Write-File "$src\Beta.json" '{"process_list":[{"sub_path":"process/0.20mm Standard @Beta.json"}]}'
    Write-File "$src\Beta.opc" 'cache'
    Write-File "$src\Beta\process\0.20mm Standard @Beta.json" '{}'
    Write-File "$src\Gamma.json" '{"filament_list":[{"sub_path":"filament/Gamma PLA.json"}]}'
    Write-File "$src\Gamma\filament\Gamma PLA.json" '{}'
    Write-File "$src\blacklist.json" '{}'
    Copy-Item -Recurse $src (Join-Path $work 'install')
    New-Item -ItemType Directory -Force -Path (Join-Path $work 'empty') | Out-Null
}

$cases = @(
    @{ Name = 'the source is pruned first and the install tree after it still loses its presets'
       Args = @('-Profiles', '{work}\src', '{work}\src', '{work}\install'); Exit = 0
       Contains = @('{work}\src: 2 caches, dropped 7 preset JSONs', '{work}\install: 2 caches, dropped 7 preset JSONs')
       NotExists = @('{work}\src\Acme.json', '{work}\src\Acme\filament\Acme PLA.json',
                     '{work}\install\Acme.json', '{work}\install\Acme\machine\Acme One.json',
                     '{work}\install\Acme\machine\Acme One 0.4 nozzle.json',
                     '{work}\install\Acme\process', '{work}\install\Beta.json', '{work}\install\Beta\process')
       Exists = @('{work}\install\Acme.opc', '{work}\install\Acme\machine\Acme One_cover.png',
                  '{work}\install\Acme\filament\colors.json', '{work}\install\Acme\shapes\bed.stl',
                  '{work}\install\Beta', '{work}\install\Gamma.json', '{work}\install\Gamma\filament\Gamma PLA.json',
                  '{work}\install\blacklist.json') }
    @{ Name = 'pruning only the install tree leaves the source alone'
       Args = @('-Profiles', '{work}\src', '{work}\install'); Exit = 0
       Contains = @('{work}\install: 2 caches, dropped 7 preset JSONs')
       NotExists = @('{work}\install\Acme.json')
       Exists = @('{work}\src\Acme.json', '{work}\src\Acme\filament\Acme PLA.json') }
    @{ Name = 'a missing target is named'
       Args = @('-Profiles', '{work}\src', '{work}\nowhere'); Exit = 1
       Contains = @('profiles directory not found') }
    @{ Name = 'a source without caches is refused'
       Args = @('-Profiles', '{work}\empty', '{work}\install'); Exit = 1
       Contains = @('no .opc caches') }
    @{ Name = 'a cache whose vendor profile is already gone is refused'
       Before = { Remove-Item -LiteralPath (Join-Path $work 'src/Beta.json') }
       Args = @('-Profiles', '{work}\src', '{work}\install'); Exit = 1
       Contains = @('no Beta.json beside Beta.opc') }
)

function Resolve-Placeholders([string[]] $values) {
    foreach ($value in $values) { $value.Replace('{work}', $work).Replace('\', [IO.Path]::DirectorySeparatorChar) }
}

$failed = 0
foreach ($case in $cases) {
    New-Fixture
    if ($case.Before) { & $case.Before }
    $ErrorActionPreference = 'Continue'
    $output = (& powershell -NoProfile -File $prune @(Resolve-Placeholders $case.Args) 2>&1 | Out-String)
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $problems = @()
    if ($code -ne $case.Exit) { $problems += "exit $code, expected $($case.Exit)" }
    foreach ($text in (Resolve-Placeholders $case.Contains)) {
        if (-not $output.Contains($text)) { $problems += "no '$text' in the output" }
    }
    foreach ($path in (Resolve-Placeholders $case.Exists)) {
        if (-not (Test-Path -LiteralPath $path)) { $problems += "$path is gone" }
    }
    foreach ($path in (Resolve-Placeholders $case.NotExists)) {
        if (Test-Path -LiteralPath $path) { $problems += "$path is still there" }
    }
    if ($problems) {
        $failed++
        "  FAIL $($case.Name)"
        $problems | ForEach-Object { "       $_" }
        $output.TrimEnd() -split "`r?`n" | ForEach-Object { "       | $_" }
    } else {
        "  ok   $($case.Name)"
    }
}

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
"$($cases.Count - $failed) passed, $failed failed"
exit [int]($failed -gt 0)
