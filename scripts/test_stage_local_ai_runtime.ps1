<#
.SYNOPSIS
Exercise local-runtime failure paths using disposable directories only.
.DESCRIPTION
Does not configure, compile, launch Orca, contact providers, or use a real runtime.
Every case invokes VerifyOnly against a fixture checkout. Junction cases preserve
their target sentinel and remove only the link before fixture cleanup.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$stageScript = Join-Path $PSScriptRoot 'stage_local_ai_runtime.ps1'
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$fixtureRoot = Join-Path $tempBase ('orca-local-runtime-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$passed = 0
$failed = New-Object 'System.Collections.Generic.List[string]'
$junctions = New-Object 'System.Collections.Generic.List[string]'

function Write-FixtureFile([string] $Path, [string] $Content = 'fixture') {
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($Path)) -Force | Out-Null
    Set-Content -LiteralPath $Path -Value $Content -Encoding UTF8
}

function New-Fixture([string] $Name) {
    $root = Join-Path $fixtureRoot $Name
    $repo = Join-Path $root 'checkout'
    $build = Join-Path $repo 'build'
    $output = Join-Path $build 'local-preview'
    $deps = Join-Path $root 'shared-dependencies'
    New-Item -ItemType Directory -Path (Join-Path $repo 'scripts'), $build, $output, $deps -Force | Out-Null
    $scriptPath = Join-Path $repo 'scripts/stage_local_ai_runtime.ps1'
    Copy-Item -LiteralPath $stageScript -Destination $scriptPath
    $cache = @("CMAKE_HOME_DIRECTORY:INTERNAL=$repo", "CMAKE_PREFIX_PATH:PATH=$deps",
        "CMAKE_COMMAND:INTERNAL=$((Get-Process -Id $PID).Path)", 'ORCA_AI_WINDOWS_INSTALLER:BOOL=ON') -join "`n"
    Write-FixtureFile (Join-Path $build 'CMakeCache.txt') $cache
    $owner = @{ schema_version = 1; tool = 'stage_local_ai_runtime.ps1';
        source_directory = $repo; build_directory = $build; output_directory = $output }
    Write-FixtureFile (Join-Path $output '.local-runtime-owner.json') ($owner | ConvertTo-Json)
    Write-FixtureFile (Join-Path $output '.local-runtime-verified.json') '{"status":"verified"}'
    return @{ Root = $root; Repo = $repo; Build = $build; Output = $output; Deps = $deps; Script = $scriptPath }
}

function Assert-Rejected($Fixture, [string] $Expected, [string] $Output = '') {
    if (-not $Output) { $Output = $Fixture.Output }
    $caught = $null
    try { & $Fixture.Script -BuildDir $Fixture.Build -OutputDir $Output -VerifyOnly | Out-Null }
    catch { $caught = $_.Exception.Message }
    if (-not $caught) { throw 'Verification unexpectedly succeeded.' }
    if (-not $caught.Contains($Expected)) { throw "Expected '$Expected', got '$caught'" }
}

function Assert-NoVerifiedMarker($Fixture) {
    if (Test-Path -LiteralPath (Join-Path $Fixture.Output '.local-runtime-verified.json')) {
        throw 'A stale success marker survived failed verification.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Fixture.Output '.local-runtime-owner.json'))) {
        throw 'Persistent directory ownership was lost.'
    }
}

function Add-MinimalRuntime($Fixture) {
    foreach ($name in @('orca-slicer.exe', 'OrcaSlicer.dll', 'python/python.exe', 'python/pythonw.exe',
        'python/python312.dll', 'resources/tools/uv/uv.exe',
        'resources/images/OrcaSlicer_gradient_circle.svg', 'resources/i18n/zh_CN/OrcaSlicer.mo',
        'resources/i18n/zh_TW/OrcaSlicer.mo', 'resources/tools/ai/orca_ai_build_info.json',
        'resources/tools/ai/orca_ai_runtime_dependencies.json')) {
        Write-FixtureFile (Join-Path $Fixture.Output $name)
    }
}

function Add-BuildReceipt($Fixture) {
    Add-MinimalRuntime $Fixture
    $sourceNames = @('CMakeLists.txt', 'src/CMakeLists.txt', 'src/slic3r/CMakeLists.txt',
        'src/slic3r/GUI/GUI_App.cpp', 'src/slic3r/GUI/GUI_App.hpp',
        'src/slic3r/GUI/GLCanvas3D.cpp', 'src/slic3r/GUI/GLCanvas3D.hpp',
        'src/slic3r/GUI/ImGuiWrapper.cpp', 'src/slic3r/GUI/ImGuiWrapper.hpp',
        'src/slic3r/GUI/WebViewDialog.cpp', 'src/slic3r/GUI/WebViewDialog.hpp',
        'src/slic3r/GUI/Widgets/LoadingBrand.hpp', 'src/OrcaSlicer_app_msvc.cpp',
        'localization/i18n/zh_CN/OrcaSlicer_zh_CN.po', 'localization/i18n/zh_TW/OrcaSlicer_zh_TW.po')
    $sources = @{}
    foreach ($relative in $sourceNames) {
        Write-FixtureFile (Join-Path $Fixture.Repo $relative)
        $sources[$relative] = (Get-FileHash -LiteralPath (Join-Path $Fixture.Repo $relative)).Hash
    }
    $resources = @{}
    foreach ($relative in @('resources/images/OrcaSlicer_gradient_circle.svg',
        'resources/i18n/zh_CN/OrcaSlicer.mo', 'resources/i18n/zh_TW/OrcaSlicer.mo')) {
        Write-FixtureFile (Join-Path $Fixture.Repo $relative)
        $resources[$relative] = (Get-FileHash -LiteralPath (Join-Path $Fixture.Repo $relative)).Hash
    }
    $binaries = @{}
    foreach ($relative in @('orca-slicer.exe', 'OrcaSlicer.dll')) {
        Write-FixtureFile (Join-Path $Fixture.Build "src/Release/$relative")
        $binaries[$relative] = (Get-FileHash -LiteralPath (Join-Path $Fixture.Output $relative)).Hash
    }
    $native = @{}
    foreach ($relative in @('WebView2Loader.dll', 'python312.dll', 'python/Lib/pathlib.py',
        'resources/tools/ai/orca_ai_installed_bootstrap.py',
        'python/Lib/site-packages/PIL/_imaging.cp312-win_amd64.pyd')) {
        Write-FixtureFile (Join-Path $Fixture.Output $relative)
        $native[$relative] = (Get-FileHash -LiteralPath (Join-Path $Fixture.Output $relative)).Hash
    }
    $receipt = @{ schema_version = 1; status = 'build_and_install_succeeded'; output_directory = $Fixture.Output;
        startup_source_sha256 = $sources; startup_resource_sha256 = $resources; desktop_binary_sha256 = $binaries;
        installed_files = @($native.Keys); runtime_sha256 = $native }
    Write-FixtureFile (Join-Path $Fixture.Output '.local-runtime-build.json') ($receipt | ConvertTo-Json -Depth 6)
}

function Run-Case([string] $Name, [scriptblock] $Body) {
    try {
        & $Body (New-Fixture $Name)
        $script:passed++
        Write-Output "PASS $Name"
    } catch {
        $script:failed.Add($Name)
        Write-Output "FAIL $Name : $($_.Exception.Message)"
    }
}

try {
    Run-Case 'missing-executable-clears-success' { param($fx)
        Assert-Rejected $fx 'Required runtime file is missing: orca-slicer.exe'
        Assert-NoVerifiedMarker $fx
    }
    foreach ($missing in @('OrcaSlicer.dll', 'python/python312.dll', 'resources/tools/uv/uv.exe',
        'resources/images/OrcaSlicer_gradient_circle.svg', 'resources/i18n/zh_CN/OrcaSlicer.mo',
        'resources/tools/ai/orca_ai_build_info.json')) {
        $caseName = 'missing-' + $missing.Replace('/', '-').Replace('.', '-')
        Run-Case $caseName { param($fx)
            Add-MinimalRuntime $fx
            Remove-Item -LiteralPath (Join-Path $fx.Output $missing)
            Assert-Rejected $fx "Required runtime file is missing: $missing"
            Assert-NoVerifiedMarker $fx
        }
    }
    Run-Case 'missing-build-receipt-clears-success' { param($fx)
        Add-MinimalRuntime $fx
        Assert-Rejected $fx 'Local build receipt is missing'
        Assert-NoVerifiedMarker $fx
    }
    foreach ($missingDependency in @('WebView2Loader.dll', 'python312.dll', 'python/Lib/pathlib.py',
        'resources/tools/ai/orca_ai_installed_bootstrap.py',
        'python/Lib/site-packages/PIL/_imaging.cp312-win_amd64.pyd')) {
        Run-Case ('missing-manifest-' + $missingDependency.Replace('/', '-').Replace('.', '-')) { param($fx)
            Add-BuildReceipt $fx
            Remove-Item -LiteralPath (Join-Path $fx.Output $missingDependency)
            Assert-Rejected $fx "Required runtime file is missing: $missingDependency"
            Assert-NoVerifiedMarker $fx
        }
    }
    Run-Case 'changed-startup-source-invalidates-build-receipt' { param($fx)
        Add-BuildReceipt $fx
        Write-FixtureFile (Join-Path $fx.Repo 'src/slic3r/GUI/Widgets/LoadingBrand.hpp') 'changed source'
        Assert-Rejected $fx 'startup source changed since the build receipt'
        Assert-NoVerifiedMarker $fx
    }
    Run-Case 'changed-source-logo-invalidates-build-receipt' { param($fx)
        Add-BuildReceipt $fx
        Write-FixtureFile (Join-Path $fx.Repo 'resources/images/OrcaSlicer_gradient_circle.svg') 'changed logo'
        Assert-Rejected $fx 'startup resource changed since the build receipt'
        Assert-NoVerifiedMarker $fx
    }
    Run-Case 'changed-installed-translation-is-rejected' { param($fx)
        Add-BuildReceipt $fx
        Write-FixtureFile (Join-Path $fx.Output 'resources/i18n/zh_CN/OrcaSlicer.mo') 'stale translation'
        Assert-Rejected $fx 'installed startup resource changed since the build receipt'
        Assert-NoVerifiedMarker $fx
    }
    Run-Case 'changed-runtime-dll-is-rejected' { param($fx)
        Add-BuildReceipt $fx
        Write-FixtureFile (Join-Path $fx.Output 'WebView2Loader.dll') 'stale binary'
        Assert-Rejected $fx 'installed native/Python runtime changed since the build receipt'
        Assert-NoVerifiedMarker $fx
    }
    Run-Case 'unowned-directory-is-not-modified' { param($fx)
        Remove-Item -LiteralPath (Join-Path $fx.Output '.local-runtime-owner.json')
        $marker = Join-Path $fx.Output '.local-runtime-verified.json'
        $before = (Get-FileHash -LiteralPath $marker).Hash
        Assert-Rejected $fx 'Refusing a nonempty output directory'
        if ((Get-FileHash -LiteralPath $marker).Hash -ne $before) { throw 'Unowned files were modified.' }
    }
    Run-Case 'wrong-owner-is-not-modified' { param($fx)
        $ownerPath = Join-Path $fx.Output '.local-runtime-owner.json'
        $owner = Get-Content -LiteralPath $ownerPath -Raw | ConvertFrom-Json
        $owner.source_directory = Join-Path $fx.Root 'another-checkout'
        Write-FixtureFile $ownerPath ($owner | ConvertTo-Json)
        Assert-Rejected $fx 'Output ownership does not match'
        if (-not (Test-Path -LiteralPath (Join-Path $fx.Output '.local-runtime-verified.json'))) {
            throw 'Another checkout output was modified.'
        }
    }
    Run-Case 'git-checkout-is-not-modified' { param($fx)
        Write-FixtureFile (Join-Path $fx.Output '.git/HEAD') 'ref: refs/heads/user-branch'
        Assert-Rejected $fx 'Git metadata is not allowed in the install tree'
        if (-not (Test-Path -LiteralPath (Join-Path $fx.Output '.local-runtime-verified.json'))) {
            throw 'A nested Git checkout was modified.'
        }
    }
    Run-Case 'repository-and-build-roots-rejected' { param($fx)
        Assert-Rejected $fx 'Output must not be a volume root' $fx.Repo
        Assert-Rejected $fx 'Output must not be a volume root' $fx.Build
    }
    Run-Case 'source-and-external-output-rejected' { param($fx)
        Assert-Rejected $fx 'Output must be a dedicated' (Join-Path $fx.Repo 'src')
        Assert-Rejected $fx 'Output must be a dedicated' (Join-Path $fx.Root 'outside')
        Assert-Rejected $fx 'Output must be a dedicated' (Join-Path $fx.Build '.git')
    }
    Run-Case 'build-internals-rejected' { param($fx)
        foreach ($relative in @('src', 'tests', '_deps', 'CMakeFiles', 'local-runtime-logs')) {
            Assert-Rejected $fx 'Output overlaps build internals' (Join-Path $fx.Build $relative)
        }
    }
    Run-Case 'shared-prefix-list-protected-before-marker-removal' { param($fx)
        $cachePath = Join-Path $fx.Build 'CMakeCache.txt'
        $cache = (Get-Content -LiteralPath $cachePath -Raw).Replace(
            "CMAKE_PREFIX_PATH:PATH=$($fx.Deps)", "CMAKE_PREFIX_PATH:PATH=$($fx.Deps);$($fx.Output)")
        Write-FixtureFile $cachePath $cache
        Assert-Rejected $fx 'Output overlaps the shared dependency prefix'
        if (-not (Test-Path -LiteralPath (Join-Path $fx.Output '.local-runtime-verified.json'))) {
            throw 'Protected shared dependencies were modified.'
        }
    }
    Run-Case 'nested-junction-target-is-untouched' { param($fx)
        $target = Join-Path $fx.Root 'junction-target'
        $sentinel = Join-Path $target 'keep.txt'
        Write-FixtureFile $sentinel 'preserve this target'
        $link = Join-Path $fx.Output 'resources'
        New-Item -ItemType Junction -Path $link -Target $target | Out-Null
        $script:junctions.Add($link)
        $before = (Get-FileHash -LiteralPath $sentinel).Hash
        Assert-Rejected $fx 'Reparse points are not allowed in the install tree'
        if ((Get-FileHash -LiteralPath $sentinel).Hash -ne $before -or
            @(Get-ChildItem -LiteralPath $target -Force).Count -ne 1) { throw 'Junction target was modified.' }
    }
    Run-Case 'junction-output-is-untouched' { param($fx)
        $link = Join-Path $fx.Build 'linked-preview'
        New-Item -ItemType Junction -Path $link -Target $fx.Output | Out-Null
        $script:junctions.Add($link)
        Assert-Rejected $fx 'Reparse points are not allowed in the install path' $link
        if (-not (Test-Path -LiteralPath (Join-Path $fx.Output '.local-runtime-verified.json'))) {
            throw 'Junction output was modified.'
        }
    }
} finally {
    # Validate the exact disposable tree before deleting anything. Remove links
    # non-recursively with the native .NET directory API, never their targets.
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    if ([IO.Path]::GetDirectoryName($resolvedFixture) -ne $tempBase -or
        -not [IO.Path]::GetFileName($resolvedFixture).StartsWith('orca-local-runtime-tests-')) {
        throw 'Refusing cleanup outside the disposable fixture directory.'
    }
    foreach ($link in $junctions) {
        $resolvedLink = [IO.Path]::GetFullPath($link)
        if (-not $resolvedLink.StartsWith($resolvedFixture + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing cleanup of an unexpected junction.'
        }
        if (Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue) { [IO.Directory]::Delete($link) }
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
Write-Output "$passed passed, $($failed.Count) failed"
if ($failed.Count) { exit 1 }
