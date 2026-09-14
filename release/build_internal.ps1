[CmdletBinding()]
param(
    [string] $BuildDir = 'build-validation',
    [string] $OutputDir = 'build\windows-installer',
    [string] $Revision,
    [string] $CMakeExecutable,
    [string] $NsisDir,
    [string] $SourceManifest,
    [switch] $SkipTargetedTests,
    [switch] $ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path

function Resolve-OperatorPath {
    param(
        [Parameter(Mandatory = $true)][string] $Path,
        [Parameter(Mandatory = $true)][string] $Label,
        [switch] $RequireLeaf,
        [switch] $RequireContainer
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Label is required."
    }
    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) {
        $Path
    } else {
        Join-Path $repoRoot $Path
    }
    $fullPath = [System.IO.Path]::GetFullPath($candidate)
    if ($RequireLeaf -and -not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "$Label does not exist: $fullPath"
    }
    if ($RequireContainer -and -not (Test-Path -LiteralPath $fullPath -PathType Container)) {
        throw "$Label does not exist: $fullPath"
    }
    return $fullPath
}

function Resolve-CMakeExecutable {
    param([string] $ExplicitPath, [string] $CacheText)

    $candidates = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $candidates.Add((Resolve-OperatorPath -Path $ExplicitPath -Label 'CMake executable' -RequireLeaf))
    }
    $cacheMatch = [regex]::Match($CacheText, '(?m)^CMAKE_COMMAND:INTERNAL=(.+)$')
    if ($cacheMatch.Success) {
        $candidates.Add($cacheMatch.Groups[1].Value.Trim())
    }
    $pathCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($pathCommand) {
        $candidates.Add($pathCommand.Source)
    }
    $visualStudioRoots = @(
        $env:ProgramFiles,
        ${env:ProgramFiles(x86)}
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    foreach ($visualStudioRoot in $visualStudioRoots) {
        foreach ($edition in @('Community', 'Professional', 'Enterprise', 'BuildTools')) {
            $candidates.Add((Join-Path $visualStudioRoot "Microsoft Visual Studio\2022\$edition\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"))
        }
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'CMake was not found. Pass -CMakeExecutable or install CMake/Visual Studio CMake tools.'
}

$buildPath = Resolve-OperatorPath -Path $BuildDir -Label 'Build directory' -RequireContainer
$cachePath = Join-Path $buildPath 'CMakeCache.txt'
$cpackPath = Join-Path $buildPath 'CPackConfig.cmake'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $cpackPath -PathType Leaf)) {
    throw "The build directory must already contain CMakeCache.txt and CPackConfig.cmake: $buildPath"
}
$cacheText = Get-Content -LiteralPath $cachePath -Raw
$sourceMatch = [regex]::Match($cacheText, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$')
if (-not $sourceMatch.Success) {
    throw 'The selected build cache has no source-directory identity.'
}
$configuredSource = [System.IO.Path]::GetFullPath($sourceMatch.Groups[1].Value.Trim()).TrimEnd('\')
if (-not [string]::Equals($configuredSource, $repoRoot.TrimEnd('\'), [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The selected build directory belongs to another checkout: $configuredSource"
}
$pythonMatch = [regex]::Match($cacheText, '(?m)^Python3_EXECUTABLE:FILEPATH=(.+)$')
if (-not $pythonMatch.Success -or -not (Test-Path -LiteralPath $pythonMatch.Groups[1].Value.Trim() -PathType Leaf)) {
    throw 'The configured Python interpreter is missing from the CMake cache.'
}
$sourcePython = $pythonMatch.Groups[1].Value.Trim()
$sourceArguments = @('-I', (Join-Path $repoRoot 'scripts\package_source_identity.py'), '--root', $repoRoot)
if (-not [string]::IsNullOrWhiteSpace($SourceManifest)) {
    $SourceManifest = Resolve-OperatorPath -Path $SourceManifest -Label 'Source manifest' -RequireLeaf
    $sourceArguments += @('--manifest', $SourceManifest)
}
function Get-InternalSourceIdentity {
    $sourceJson = & $sourcePython @sourceArguments
    if ($LASTEXITCODE -ne 0) { throw 'Internal source snapshot verification failed.' }
    return (($sourceJson -join "`n") | ConvertFrom-Json)
}
$sourceIdentity = Get-InternalSourceIdentity
$sourceHead = $sourceIdentity.source_commit
$branch = $sourceIdentity.source_branch

if ([string]::IsNullOrWhiteSpace($Revision)) {
    $shortHead = $sourceHead.Substring(0,10)
    $Revision = "$(Get-Date -Format yyyyMMdd)-$shortHead"
    if (-not $sourceIdentity.source_clean) {
        $Revision += "-snapshot-$($sourceIdentity.source_identity_sha256.Substring(0,10))"
    }
}
if ($Revision -notmatch '^[0-9A-Za-z._-]+$') {
    throw "Revision contains unsupported filename characters: $Revision"
}

$outputPath = Resolve-OperatorPath -Path $OutputDir -Label 'Output directory'
$cmakePath = Resolve-CMakeExecutable -ExplicitPath $CMakeExecutable -CacheText $cacheText
$nsisPath = $null
if (-not [string]::IsNullOrWhiteSpace($NsisDir)) {
    $nsisPath = Resolve-OperatorPath -Path $NsisDir -Label 'NSIS directory' -RequireContainer
    if (-not (Test-Path -LiteralPath (Join-Path $nsisPath 'makensis.exe') -PathType Leaf)) {
        throw "NSIS directory does not contain makensis.exe: $nsisPath"
    }
}

$validationResult = [pscustomobject]@{
    Ready                = $true
    Repository           = $repoRoot
    Branch               = $branch
    SourceCommit         = $sourceHead
    SourceIdentitySha256  = $sourceIdentity.source_identity_sha256
    SourceClean          = $sourceIdentity.source_clean
    Revision             = $Revision
    BuildDir             = $buildPath
    OutputDir            = $outputPath
    CMakeExecutable      = $cmakePath
    ProviderConfiguration = 'machine_or_user_environment'
}
if ($ValidateOnly) {
    $validationResult
    return
}

$revisionArg = "-DORCA_AI_PACKAGE_REVISION:STRING=$Revision"
$defaultsArg = '-DORCA_AI_INTERNAL_DEFAULTS_FILE:FILEPATH='
& $cmakePath -S $repoRoot -B $buildPath $revisionArg $defaultsArg `
    '-DORCA_AI_DISTRIBUTION_CHANNEL:STRING=internal' `
    '-DORCA_AI_WINDOWS_INSTALLER:BOOL=ON'
if ($LASTEXITCODE -ne 0) {
    throw "CMake reconfiguration failed with exit code $LASTEXITCODE."
}

$packageArguments = @{
    BuildDir = $buildPath
    OutputDir = $outputPath
    Revision = $Revision
}
if ($nsisPath) {
    $packageArguments.NsisDir = $nsisPath
}
if ($SourceManifest) {
    $packageArguments.SourceManifest = $SourceManifest
}
& (Join-Path $repoRoot 'scripts\package_internal_fast.ps1') @packageArguments
if ($LASTEXITCODE -ne 0) {
    throw "Internal packaging failed with exit code $LASTEXITCODE."
}

if (-not $SkipTargetedTests) {
    $updatedCacheText = Get-Content -LiteralPath $cachePath -Raw
    $pythonMatch = [regex]::Match($updatedCacheText, '(?m)^Python3_EXECUTABLE:FILEPATH=(.+)$')
    if (-not $pythonMatch.Success -or -not (Test-Path -LiteralPath $pythonMatch.Groups[1].Value.Trim() -PathType Leaf)) {
        throw 'The bundled Python interpreter is missing from the CMake cache.'
    }
    $pythonPath = $pythonMatch.Groups[1].Value.Trim()
    & $pythonPath -I (Join-Path $repoRoot 'tools\ai\test_integration_guardrails.py')
    if ($LASTEXITCODE -ne 0) { throw 'Python integration guardrail tests failed.' }
    & $pythonPath -I (Join-Path $repoRoot 'scripts\verify_ai_integration.py')
    if ($LASTEXITCODE -ne 0) { throw 'AI integration verification failed.' }

    & $cmakePath --build $buildPath --config Release --target slic3rutils_tests --parallel
    if ($LASTEXITCODE -ne 0) { throw 'slic3rutils_tests build failed.' }
    $testExecutable = Join-Path $buildPath 'tests\slic3rutils\Release\slic3rutils_tests.exe'
    if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
        throw "Focused test executable was not created: $testExecutable"
    }
    & $testExecutable '[ModelGenerationPresentation]' --reporter compact
    if ($LASTEXITCODE -ne 0) { throw 'Model-generation presentation tests failed.' }
    & $testExecutable '[AI][SmartSlicing]' --reporter compact
    if ($LASTEXITCODE -ne 0) { throw 'Smart-slicing tests failed.' }
}

$finalSourceIdentity = Get-InternalSourceIdentity
if ($finalSourceIdentity.source_identity_sha256 -ne $sourceIdentity.source_identity_sha256) {
    throw 'The source snapshot changed during packaging. Do not distribute this artifact.'
}

$manifestCandidates = @(Get-ChildItem -LiteralPath $outputPath -Filter "*_${Revision}_*.manifest.json" -File |
    Sort-Object LastWriteTime -Descending)
if ($manifestCandidates.Count -eq 0) {
    throw "No release manifest was created for revision $Revision in $outputPath"
}
$manifestPath = $manifestCandidates[0].FullName
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.source_commit -ne $sourceHead -or $manifest.source_identity_sha256 -ne $sourceIdentity.source_identity_sha256 -or $manifest.package_revision -ne $Revision -or
    $manifest.distribution_channel -ne 'internal') {
    throw 'The generated manifest does not match the locked source identity and revision.'
}
$installerPath = Join-Path $outputPath $manifest.installer
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "The manifest installer is missing: $installerPath"
}
$installerHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
if ($installerHash -ne $manifest.installer_sha256) {
    throw 'The final installer hash does not match its manifest.'
}
$portablePath = Join-Path $outputPath $manifest.portable
if (-not (Test-Path -LiteralPath $portablePath -PathType Leaf)) {
    throw "The manifest portable package is missing: $portablePath"
}
$portableHash = (Get-FileHash -LiteralPath $portablePath -Algorithm SHA256).Hash
if ($portableHash -ne $manifest.portable_sha256) {
    throw 'The portable package hash does not match its manifest.'
}

$sevenZipCommand = Get-Command 7z.exe -ErrorAction SilentlyContinue
$sevenZipPath = if ($sevenZipCommand) {
    $sevenZipCommand.Source
} else {
    Join-Path $env:ProgramFiles '7-Zip\7z.exe'
}
if (Test-Path -LiteralPath $sevenZipPath -PathType Leaf) {
    & $sevenZipPath t $installerPath
    if ($LASTEXITCODE -ne 0) { throw '7-Zip installer integrity verification failed.' }
} else {
    Write-Warning '7-Zip was not found; archive integrity was not independently tested.'
}

$installerItem = Get-Item -LiteralPath $installerPath
[pscustomobject]@{
    Installer       = $installerItem.FullName
    Portable        = $portablePath
    Manifest        = $manifestPath
    SizeBytes       = $installerItem.Length
    Sha256          = $installerHash
    PortableSha256  = $portableHash
    SourceCommit    = $sourceHead
    Revision        = $Revision
    SignatureStatus = (Get-AuthenticodeSignature -LiteralPath $installerPath).Status
}
