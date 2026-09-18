[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Destination,
    [string]$CacheDirectory = (Join-Path $PSScriptRoot '../.tmp/semantic-coloring/mediapipe'),
    [switch]$VerifyOnly,
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$semanticSourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$semanticManifestPath = Join-Path $semanticSourceRoot 'cmake/semantic_runtime_manifest.json'
$semanticManifest = Get-Content -LiteralPath $semanticManifestPath -Raw | ConvertFrom-Json
$semanticDestination = [IO.Path]::GetFullPath($Destination)
$semanticCache = [IO.Path]::GetFullPath($CacheDirectory)

function Assert-SemanticFile([string]$Path, $Specification) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing semantic file: $Path" }
    if ((Get-Item -LiteralPath $Path).Length -ne [long]$Specification.bytes) { throw "Semantic file size mismatch: $Path" }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Specification.sha256) { throw "Semantic file checksum mismatch: $Path" }
}

function Assert-SemanticRuntime([string]$Path) {
    foreach ($semanticSpec in $semanticManifest.wheel_entries) {
        Assert-SemanticFile (Join-Path $Path $semanticSpec.filename) $semanticSpec
    }
    foreach ($semanticSpec in $semanticManifest.downloads | Where-Object { $_.key -ne 'wheel' }) {
        Assert-SemanticFile (Join-Path $Path $semanticSpec.filename) $semanticSpec
    }
    foreach ($semanticMetadata in @('runtime-manifest.json', 'MODEL_LICENSES.md', 'providers.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $semanticMetadata) -PathType Leaf)) {
            throw "Incomplete semantic runtime: $semanticMetadata"
        }
    }
    $semanticStagedManifest = Get-Content -LiteralPath (Join-Path $Path 'runtime-manifest.json') -Raw | ConvertFrom-Json
    if ($semanticStagedManifest.schema -ne $semanticManifest.schema -or $semanticStagedManifest.source_commit -ne $semanticManifest.source_commit) {
        throw 'Unexpected semantic runtime manifest version'
    }
    $semanticProviders = Get-Content -LiteralPath (Join-Path $Path 'providers.json') -Raw | ConvertFrom-Json
    if ([string]::IsNullOrWhiteSpace($semanticProviders.body_provider) -or [string]::IsNullOrWhiteSpace($semanticProviders.face_provider)) {
        throw 'Both independent semantic provider ids are required'
    }
}

if ($VerifyOnly) {
    Assert-SemanticRuntime $semanticDestination
    Write-Output "Verified semantic runtime: $semanticDestination"
    exit 0
}

# Do not write this optional runtime into the sidecar resource whitelist or any
# source resources junction. The caller supplies its independent build/cache dir.
$semanticResourceRoot = [IO.Path]::GetFullPath((Join-Path $semanticSourceRoot 'resources')).TrimEnd('\', '/')
if ($semanticDestination -ieq $semanticResourceRoot -or $semanticDestination.StartsWith($semanticResourceRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Stage the semantic runtime outside source resources, under <exe>/ai/portrait_semantics or an independent cache directory'
}
New-Item -ItemType Directory -Path $semanticCache -Force | Out-Null
foreach ($semanticSpec in $semanticManifest.downloads) {
    $semanticCachedFile = Join-Path $semanticCache $semanticSpec.filename
    if (Test-Path -LiteralPath $semanticCachedFile) {
        # A corrupt cache is an explicit error, never silently downloaded over.
        Assert-SemanticFile $semanticCachedFile $semanticSpec
        continue
    }
    if ($Offline) { throw "Offline cache is missing: $($semanticSpec.filename)" }
    $semanticPartialFile = $semanticCachedFile + '.partial-' + [Guid]::NewGuid().ToString('N')
    try {
        Write-Output "Downloading pinned official artifact: $($semanticSpec.filename)"
        Invoke-WebRequest -UseBasicParsing -Uri $semanticSpec.url -OutFile $semanticPartialFile
        Assert-SemanticFile $semanticPartialFile $semanticSpec
        Move-Item -LiteralPath $semanticPartialFile -Destination $semanticCachedFile
    } finally {
        if (Test-Path -LiteralPath $semanticPartialFile -PathType Leaf) { Remove-Item -LiteralPath $semanticPartialFile }
    }
}

New-Item -ItemType Directory -Path $semanticDestination -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$semanticWheel = $semanticManifest.downloads | Where-Object { $_.key -eq 'wheel' }
$semanticArchive = [IO.Compression.ZipFile]::OpenRead((Join-Path $semanticCache $semanticWheel.filename))
try {
    foreach ($semanticSpec in $semanticManifest.wheel_entries) {
        $semanticEntry = $semanticArchive.GetEntry($semanticSpec.entry)
        if ($null -eq $semanticEntry) { throw "Pinned wheel entry is missing: $($semanticSpec.entry)" }
        $semanticTarget = Join-Path $semanticDestination $semanticSpec.filename
        [IO.Compression.ZipFileExtensions]::ExtractToFile($semanticEntry, $semanticTarget, $true)
        Assert-SemanticFile $semanticTarget $semanticSpec
    }
} finally { $semanticArchive.Dispose() }
foreach ($semanticSpec in $semanticManifest.downloads | Where-Object { $_.key -ne 'wheel' }) {
    $semanticModelSource = Join-Path $semanticCache $semanticSpec.filename
    $semanticModelDestination = Join-Path $semanticDestination $semanticSpec.filename
    if ($semanticModelSource -ine $semanticModelDestination) {
        Copy-Item -LiteralPath $semanticModelSource -Destination $semanticModelDestination -Force
    }
}
Copy-Item -LiteralPath $semanticManifestPath -Destination (Join-Path $semanticDestination 'runtime-manifest.json') -Force
Copy-Item -LiteralPath (Join-Path $semanticSourceRoot 'cmake/semantic_model_licenses.md') -Destination (Join-Path $semanticDestination 'MODEL_LICENSES.md') -Force
$semanticProviderPath = Join-Path $semanticDestination 'providers.json'
if (-not (Test-Path -LiteralPath $semanticProviderPath)) {
    $semanticProviderJson = $semanticManifest.default_providers | ConvertTo-Json
    [IO.File]::WriteAllText($semanticProviderPath, $semanticProviderJson + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
}
Assert-SemanticRuntime $semanticDestination
Write-Output "Prepared and verified CPU semantic runtime: $semanticDestination"
