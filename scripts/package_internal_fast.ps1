[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BuildDir,

    [string] $OutputDir,

    [string] $Revision,

    [string] $NsisDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$resolvedBuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$cpackConfig = Join-Path $resolvedBuildDir 'CPackConfig.cmake'
$cmakeCache = Join-Path $resolvedBuildDir 'CMakeCache.txt'

if (-not (Test-Path -LiteralPath $cpackConfig -PathType Leaf)) {
    throw "CPack configuration not found: $cpackConfig"
}
if (-not (Test-Path -LiteralPath $cmakeCache -PathType Leaf)) {
    throw "CMake cache not found: $cmakeCache"
}

$currentHead = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $currentHead -notmatch '^[0-9a-fA-F]{40}$') {
    throw 'Unable to determine the current full Git revision.'
}
$currentBranch = (& git -C $repoRoot branch --show-current).Trim()
$teamConfig = Get-Content -LiteralPath (Join-Path $repoRoot '.github\team-collaboration.json') -Raw | ConvertFrom-Json
$packageBranches = @($teamConfig.branches.PSObject.Properties.Value)
if ($LASTEXITCODE -ne 0 -or $currentBranch -notin $packageBranches) {
    throw "Internal packages require a configured team branch, not '$currentBranch'."
}
$packageKind = if ($currentBranch -eq $teamConfig.integration_branch) { 'integration' } else { 'development' }
$integrationHead = (& git -C $repoRoot rev-parse "refs/remotes/origin/$($teamConfig.integration_branch)").Trim()
if ($LASTEXITCODE -ne 0 -or $integrationHead -notmatch '^[0-9a-fA-F]{40}$') {
    throw 'Fetch the team integration branch before packaging.'
}
$worktreeChanges = @(& git -C $repoRoot status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to verify that the source worktree is clean.'
}
if ($worktreeChanges.Count -gt 0) {
    throw "The source worktree is not clean. Commit or remove all changes before packaging.`n$($worktreeChanges -join "`n")"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repoRoot 'build\windows-installer'
}
$resolvedOutputDir = [System.IO.Path]::GetFullPath($OutputDir)
if ((Test-Path -LiteralPath $resolvedOutputDir) -and @(Get-ChildItem -LiteralPath $resolvedOutputDir -Force).Count -gt 0) {
    throw 'Use an empty output directory for each build attempt; existing artifacts must not be replaced.'
}
New-Item -ItemType Directory -Path $resolvedOutputDir -Force | Out-Null

if ([string]::IsNullOrWhiteSpace($Revision)) {
    $Revision = (& git -C $repoRoot rev-parse --short=10 $currentHead).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($Revision)) {
        throw 'Unable to determine the current Git revision.'
    }
}
if ($Revision -notmatch '^[0-9A-Za-z._-]+$') {
    throw "Revision contains unsupported filename characters: $Revision"
}

$cacheText = Get-Content -LiteralPath $cmakeCache -Raw
$sourceMatch = [regex]::Match($cacheText, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$')
if (-not $sourceMatch.Success) {
    throw 'The selected build directory does not record its CMake source directory.'
}
$configuredSource = [System.IO.Path]::GetFullPath($sourceMatch.Groups[1].Value.Trim())
if (-not [string]::Equals($configuredSource.TrimEnd('\'), $repoRoot.TrimEnd('\'), [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The selected build directory belongs to another source tree: $configuredSource"
}
if ($cacheText -notmatch '(?m)^ORCA_AI_WINDOWS_INSTALLER:BOOL=ON\s*$') {
    throw 'The selected build directory is not configured with ORCA_AI_WINDOWS_INSTALLER=ON.'
}
if ($cacheText -notmatch '(?m)^ORCA_AI_DISTRIBUTION_CHANNEL:STRING=internal\s*$') {
    throw 'The fast internal packager requires ORCA_AI_DISTRIBUTION_CHANNEL=internal.'
}
$revisionMatch = [regex]::Match($cacheText, '(?m)^ORCA_AI_PACKAGE_REVISION:STRING=(.+)$')
if (-not $revisionMatch.Success -or $revisionMatch.Groups[1].Value.Trim() -ne $Revision) {
    throw "The build was configured for a different package revision. Reconfigure with -DORCA_AI_PACKAGE_REVISION=$Revision."
}

$defaultsMatch = [regex]::Match($cacheText, '(?m)^ORCA_AI_INTERNAL_DEFAULTS_FILE:FILEPATH=(.+)$')
if ($defaultsMatch.Success -and -not [string]::IsNullOrWhiteSpace($defaultsMatch.Groups[1].Value)) {
    throw 'Provider credentials must come from machine or user environment variables; internal packages must not embed defaults.'
}

$buildInfoPath = Join-Path $resolvedBuildDir 'orca_ai_build_info.json'
if (-not (Test-Path -LiteralPath $buildInfoPath -PathType Leaf)) {
    throw 'AI build identity is missing. Reconfigure this build directory before packaging.'
}
try {
    $buildInfo = Get-Content -LiteralPath $buildInfoPath -Raw | ConvertFrom-Json
} catch {
    throw 'AI build identity is not valid JSON. Reconfigure this build directory.'
}
if ($buildInfo.schema_version -ne 1 -or $buildInfo.application_commit -ne $currentHead -or
    $buildInfo.package_revision -ne $Revision -or $buildInfo.distribution_channel -ne 'internal' -or
    $buildInfo.sidecar_protocol_version -ne 2 -or $buildInfo.sidecar_version -ne 'orcaslicer-ai-sidecar-v9') {
    throw 'AI build identity does not match the current source, package revision, channel, or Sidecar contract. Reconfigure and rebuild.'
}

$cpackMatch = [regex]::Match($cacheText, '(?m)^CMAKE_CPACK_COMMAND:INTERNAL=(.+)$')
if (-not $cpackMatch.Success) {
    throw 'Unable to resolve CPack from the selected CMake cache.'
}
$cpackExecutable = $cpackMatch.Groups[1].Value.Trim()
if (-not (Test-Path -LiteralPath $cpackExecutable -PathType Leaf)) {
    throw "Configured CPack executable does not exist: $cpackExecutable"
}
$cmakeMatch = [regex]::Match($cacheText, '(?m)^CMAKE_COMMAND:INTERNAL=(.+)$')
if (-not $cmakeMatch.Success -or -not (Test-Path -LiteralPath $cmakeMatch.Groups[1].Value.Trim() -PathType Leaf)) {
    throw 'Unable to resolve CMake from the selected build directory.'
}
$cmakeExecutable = $cmakeMatch.Groups[1].Value.Trim()
$pythonMatch = [regex]::Match($cacheText, '(?m)^Python3_EXECUTABLE:FILEPATH=(.+)$')
if (-not $pythonMatch.Success -or -not (Test-Path -LiteralPath $pythonMatch.Groups[1].Value.Trim() -PathType Leaf)) {
    throw 'Unable to resolve the bundled Python interpreter from the selected build directory.'
}
$bundledPython = $pythonMatch.Groups[1].Value.Trim()

& $bundledPython -I (Join-Path $repoRoot 'scripts\verify_ai_integration.py')
if ($LASTEXITCODE -ne 0) {
    throw "AI integration guardrails failed with exit code $LASTEXITCODE."
}

# An incremental build is normally a no-op, but it prevents a stale binary from
# being relabelled with the current source revision.
if ($cacheText -match '(?m)^CMAKE_GENERATOR:INTERNAL=Visual Studio') {
    & $cmakeExecutable --build $resolvedBuildDir --config Release --target OrcaSlicer_app_gui -- /m:2 /p:CL_MPCount=1 /p:UseMultiToolTask=false /p:BuildInParallel=false /nologo /v:minimal
} else {
    & $cmakeExecutable --build $resolvedBuildDir --config Release --target OrcaSlicer_app_gui --parallel 2
}
if ($LASTEXITCODE -ne 0) {
    throw "Incremental Release build failed with exit code $LASTEXITCODE."
}

# CPack only discovers NSIS from the registry or PATH. Internal builders often use
# the portable NSIS bundle, so make that supported instead of requiring a manual
# PATH edit before every package.
$nsisCandidates = @()
if (-not [string]::IsNullOrWhiteSpace($NsisDir)) {
    $nsisCandidates += [System.IO.Path]::GetFullPath($NsisDir)
}
$nsisCandidates += @(
    (Join-Path $env:ProgramFiles 'NSIS'),
    (Join-Path ${env:ProgramFiles(x86)} 'NSIS'),
    (Join-Path $env:TEMP 'nsis-3.12-portable')
)
$nsisRoot = $nsisCandidates |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ 'makensis.exe') -PathType Leaf } |
    Select-Object -First 1
if (-not $nsisRoot) {
    throw 'NSIS makensis.exe was not found. Pass -NsisDir or install NSIS in a standard location.'
}
$env:PATH = "$nsisRoot;$env:PATH"

Push-Location $repoRoot
try {
    & cmd.exe /d /c 'scripts\run_gettext.bat'
    if ($LASTEXITCODE -ne 0) {
        throw "Localization generation failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

$requiredCatalogs = @(
    (Join-Path $repoRoot 'resources\i18n\zh_CN\OrcaSlicer.mo'),
    (Join-Path $repoRoot 'resources\i18n\zh_TW\OrcaSlicer.mo')
)
foreach ($catalog in $requiredCatalogs) {
    if (-not (Test-Path -LiteralPath $catalog -PathType Leaf)) {
        throw "Required localization catalog was not generated: $catalog"
    }
}

$versionLine = Select-String -LiteralPath (Join-Path $repoRoot 'version.inc') -Pattern '^\s*set\(SoftFever_VERSION\s+"([^"]+)"\)' | Select-Object -First 1
if (-not $versionLine) {
    throw 'Unable to determine OrcaSlicer version from version.inc.'
}
$version = $versionLine.Matches[0].Groups[1].Value

$generatorPlatformLine = Select-String -LiteralPath $cmakeCache -Pattern '^CMAKE_GENERATOR_PLATFORM:[^=]+=(.+)$' | Select-Object -First 1
$processorLine = Select-String -LiteralPath $cmakeCache -Pattern '^CMAKE_SYSTEM_PROCESSOR:[^=]+=(.+)$' | Select-Object -First 1
$configuredArchitecture = if ($generatorPlatformLine) {
    $generatorPlatformLine.Matches[0].Groups[1].Value
} elseif ($processorLine) {
    $processorLine.Matches[0].Groups[1].Value
} else {
    ''
}
if ($configuredArchitecture -notmatch '^(?i:x64|amd64|x86_64|arm64|aarch64)$') {
    throw "Unsupported or missing Windows package architecture: '$configuredArchitecture'."
}
$architecture = if ($configuredArchitecture -match '^(?i:arm64|aarch64)$') { 'arm64' } else { 'x64' }
$runtimeDependenciesPath = Join-Path $resolvedBuildDir 'orca_ai_runtime_dependencies.json'
if (-not (Test-Path -LiteralPath $runtimeDependenciesPath -PathType Leaf)) {
    throw 'Pinned AI runtime dependency metadata is missing. Reconfigure this build directory.'
}
try {
    $runtimeDependencies = Get-Content -LiteralPath $runtimeDependenciesPath -Raw | ConvertFrom-Json
} catch {
    throw 'Pinned AI runtime dependency metadata is not valid JSON. Reconfigure this build directory.'
}
$pillowDependency = @($runtimeDependencies.packages) |
    Where-Object { $_.name -eq 'Pillow' } |
    Select-Object -First 1
$expectedPillowHash = if ($architecture -eq 'arm64') {
    'af73337013e0b3b46f175e79492d96845b16126ddf79c438d7ea7ff27783a414'
} else {
    '7f84204dee22a783350679a0333981df803dac21a0190d706a50475e361c93f5'
}
if ($runtimeDependencies.schema_version -ne 1 -or $runtimeDependencies.python.version -ne '3.12.13' -or
    $runtimeDependencies.python.isolation_flag -ne '-I' -or -not $pillowDependency -or
    $pillowDependency.version -ne '12.2.0' -or $pillowDependency.architecture -ne $architecture -or
    $pillowDependency.sha256 -ne $expectedPillowHash) {
    throw 'Pinned AI runtime dependency metadata does not match the supported Python/Pillow runtime.'
}
# A short CPack name keeps the NSIS staging path below the Windows path limit.
$shortPackageName = "OrcaAI_$Revision`_$architecture"
& $cpackExecutable --config $cpackConfig -G NSIS -C Release -B $resolvedOutputDir -D "CPACK_PACKAGE_FILE_NAME=$shortPackageName"
if ($LASTEXITCODE -ne 0) {
    throw "CPack failed with exit code $LASTEXITCODE."
}

$generatedInstaller = Join-Path $resolvedOutputDir "$shortPackageName.exe"
if (-not (Test-Path -LiteralPath $generatedInstaller -PathType Leaf)) {
    throw "Expected installer was not created: $generatedInstaller"
}

$finalName = "OrcaSlicer_AI_Internal_Fast_V$version`_$Revision`_$architecture.exe"
$finalInstaller = Join-Path $resolvedOutputDir $finalName
Move-Item -LiteralPath $generatedInstaller -Destination $finalInstaller -Force

$hash = (Get-FileHash -LiteralPath $finalInstaller -Algorithm SHA256).Hash
$hashFile = "$finalInstaller.sha256"
Set-Content -LiteralPath $hashFile -Value "$hash  $finalName" -Encoding ascii

$shortPortableName = "OrcaAIPortable_$Revision`_$architecture"
& $cpackExecutable --config $cpackConfig -G ZIP -C Release -B $resolvedOutputDir -D "CPACK_PACKAGE_FILE_NAME=$shortPortableName"
if ($LASTEXITCODE -ne 0) {
    throw "Portable CPack failed with exit code $LASTEXITCODE."
}
$generatedPortable = Join-Path $resolvedOutputDir "$shortPortableName.zip"
if (-not (Test-Path -LiteralPath $generatedPortable -PathType Leaf)) {
    throw "Expected portable package was not created: $generatedPortable"
}
$portableName = "OrcaSlicer_AI_Internal_Fast_V$version`_$Revision`_$architecture`_portable.zip"
$portablePackage = Join-Path $resolvedOutputDir $portableName
Move-Item -LiteralPath $generatedPortable -Destination $portablePackage -Force
$portableHash = (Get-FileHash -LiteralPath $portablePackage -Algorithm SHA256).Hash
$portableHashFile = "$portablePackage.sha256"
Set-Content -LiteralPath $portableHashFile -Value "$portableHash  $portableName" -Encoding ascii

# Inspect each completed artifact; source/defaults checks do not establish payload contents.
foreach ($artifact in @($finalInstaller, $portablePackage)) {
    & $bundledPython -I (Join-Path $repoRoot 'release\verify_package_contents.py') $artifact --report "$artifact.contents.json"
    if ($LASTEXITCODE -ne 0) {
        throw "Publication blocked: actual package inspection found credentials or could not complete. Review $artifact.contents.json. Local development remains permitted."
    }
    $inspection = Get-Content -LiteralPath "$artifact.contents.json" -Raw | ConvertFrom-Json
    $expectedHash = if ($artifact -eq $finalInstaller) { $hash } else { $portableHash }
    if ($inspection.status -ne 'NOT_DETECTED_WITHIN_SCOPE' -or $inspection.sha256 -ne $expectedHash -or
        (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash -ne $expectedHash) {
        throw 'Package inspection identity changed. Do not publish this artifact.'
    }
}

$finalSourceHead = (& git -C $repoRoot rev-parse HEAD).Trim()
$finalSourceChanges = @(& git -C $repoRoot status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or $finalSourceHead -ne $currentHead -or $finalSourceChanges.Count -gt 0) {
    throw 'Source changed during packaging. Keep this attempt for diagnosis and rebuild from a stable source revision.'
}

$integrationLockPath = Join-Path $repoRoot 'docs\architecture\ai-integration-lock.json'
if (-not (Test-Path -LiteralPath $integrationLockPath -PathType Leaf)) {
    throw "AI integration lock is missing: $integrationLockPath"
}
$integrationLock = Get-Content -LiteralPath $integrationLockPath -Raw | ConvertFrom-Json
$manifestPath = "$finalInstaller.manifest.json"
$releaseManifest = [ordered]@{
    schema_version = 3
    created_utc = [DateTime]::UtcNow.ToString('o')
    installer = $finalName
    installer_sha256 = $hash
    source_commit = $currentHead
    source_branch = $currentBranch
    source_clean = $true
    build_kind = $packageKind
    integration_baseline_commit = $integrationHead
    build_id = "$packageKind-$($currentHead.Substring(0,12))-windows-$architecture-Release-$Revision"
    application_version = $version
    package_revision = $Revision
    distribution_channel = 'internal'
    architecture = $architecture
    provider_configuration = [ordered]@{
        source = 'machine_or_user_environment'
        image_primary = @('OPENAI_PRO_API', 'OPENAI_PRO_URL')
        image_legacy_fallback = @('OPENAI_API_KEY', 'OPENAI_BASE_URL')
        packaged_credentials = $null
        inspection_result = 'not_detected_within_recorded_scope'
    }
    content_inspection = [ordered]@{
        installer_report = "$finalName.contents.json"
        portable_report = "$portableName.contents.json"
    }
    portable = $portableName
    portable_sha256 = $portableHash
    sidecar_version = $buildInfo.sidecar_version
    sidecar_protocol_version = $buildInfo.sidecar_protocol_version
    runtime_dependencies = $runtimeDependencies
    upstream = $integrationLock.upstream
    accepted_features = $integrationLock.feature_sources
}
$releaseManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8

[pscustomobject]@{
    Installer = $finalInstaller
    Sha256 = $hash
    ChecksumFile = $hashFile
    Manifest = $manifestPath
    Portable = $portablePackage
    PortableSha256 = $portableHash
    PortableChecksumFile = $portableHashFile
    Revision = $Revision
    Architecture = $architecture
    LocalizationCatalogs = $requiredCatalogs.Count
}
