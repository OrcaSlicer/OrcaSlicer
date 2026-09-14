<#
.SYNOPSIS
Build and install a complete local AI preview without publishing or changing provider settings.
.DESCRIPTION
Reuses an existing CMake build. Output must be a dedicated child of that build directory,
without junctions or symlinks. A persistent ownership file protects nonempty directories.
Existing files are never recursively removed. After validating directory ownership,
VerifyOnly invalidates the previous verification marker and checks the saved build receipt.
#>
[CmdletBinding()]
param(
    [string] $BuildDir = 'build',
    [string] $OutputDir = 'build/local-startup-preview',
    [string] $Revision = 'local-startup-preview',
    [string] $CMakeExecutable,
    [switch] $VerifyOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\', '/')
$markerName = '.local-runtime-verified.json'
$ownerName = '.local-runtime-owner.json'
$receiptName = '.local-runtime-build.json'
# This deliberately bounded receipt covers the startup changes, not every C++
# source file in Orca. It cannot certify arbitrary unrelated working-tree edits.
$startupSources = @('CMakeLists.txt', 'src/CMakeLists.txt', 'src/slic3r/CMakeLists.txt',
    'src/slic3r/GUI/GUI_App.cpp', 'src/slic3r/GUI/GUI_App.hpp',
    'src/slic3r/GUI/GLCanvas3D.cpp', 'src/slic3r/GUI/GLCanvas3D.hpp',
    'src/slic3r/GUI/ImGuiWrapper.cpp', 'src/slic3r/GUI/ImGuiWrapper.hpp',
    'src/slic3r/GUI/WebViewDialog.cpp', 'src/slic3r/GUI/WebViewDialog.hpp',
    'src/slic3r/GUI/Widgets/LoadingBrand.hpp', 'src/OrcaSlicer_app_msvc.cpp',
    'localization/i18n/zh_CN/OrcaSlicer_zh_CN.po', 'localization/i18n/zh_TW/OrcaSlicer_zh_TW.po')
$startupResources = @('resources/images/OrcaSlicer_gradient_circle.svg',
    'resources/i18n/zh_CN/OrcaSlicer.mo', 'resources/i18n/zh_TW/OrcaSlicer.mo')

function Resolve-LocalPath([string] $Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { throw 'A directory path must not be empty.' }
    if (-not [IO.Path]::IsPathRooted($Value)) { $Value = Join-Path $repoRoot $Value }
    return [IO.Path]::GetFullPath($Value).TrimEnd('\', '/')
}

function Test-Within([string] $Child, [string] $Parent) {
    return $Child.Equals($Parent, [StringComparison]::OrdinalIgnoreCase) -or
        $Child.StartsWith($Parent.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar,
                          [StringComparison]::OrdinalIgnoreCase)
}

function Assert-PlainAncestors([string] $Path) {
    $candidate = $Path
    while ($candidate) {
        $entry = Get-Item -LiteralPath $candidate -Force -ErrorAction SilentlyContinue
        if ($entry) {
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse points are not allowed in the install path: $candidate"
            }
        }
        $candidate = [IO.Path]::GetDirectoryName($candidate)
    }
}

function Assert-PlainTree([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $pending = New-Object 'System.Collections.Generic.Queue[string]'
    $pending.Enqueue($Path)
    while ($pending.Count) {
        foreach ($entry in Get-ChildItem -LiteralPath $pending.Dequeue() -Force) {
            if ($entry.Name -eq '.git') { throw 'Git metadata is not allowed in the install tree.' }
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse points are not allowed in the install tree: $($entry.FullName)"
            }
            if ($entry.PSIsContainer) { $pending.Enqueue($entry.FullName) }
        }
    }
}

function Read-CacheValue([string] $Name) {
    $match = [regex]::Match($script:cacheText, '(?m)^' + [regex]::Escape($Name) + ':[^=]+=(.*)\r?$')
    if (-not $match.Success) { throw "Missing CMake cache entry: $Name" }
    return $match.Groups[1].Value.Trim()
}

function Invoke-Logged([string] $Executable, [string[]] $Arguments, [string] $LogName) {
    $logPath = Join-Path $script:logDir $LogName
    Add-Content -LiteralPath $logPath -Value ("`n[{0:o}] {1} {2}" -f (Get-Date), $Executable,
        (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')) -Encoding UTF8
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments 2>&1 | Tee-Object -FilePath $logPath -Append | Out-Host
        $commandExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $savedPreference }
    if ($commandExit -ne 0) { throw "Command failed (exit $commandExit); see $logPath" }
}

function Require-File([string] $Relative) {
    $path = Join-Path $script:outputPath $Relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required runtime file is missing: $Relative" }
    return $path
}

function Get-Fingerprints([string] $Root, [string[]] $RelativePaths) {
    $result = [ordered]@{}
    foreach ($relative in $RelativePaths) {
        $path = Join-Path $Root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Fingerprint input is missing: $relative" }
        $result[$relative] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $result
}

function Assert-Fingerprints([string] $Root, $Fingerprints, [string] $Label) {
    $items = @($Fingerprints.PSObject.Properties)
    if (-not $items.Count) { throw "Empty fingerprints in $Label" }
    foreach ($item in $items) {
        $path = [IO.Path]::GetFullPath((Join-Path $Root $item.Name))
        if (-not (Test-Within $path $Root) -or $path -eq $Root) { throw "Invalid relative path in $Label" }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required $Label file is missing: $($item.Name)" }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $item.Value) {
            throw "$Label changed since the build receipt: $($item.Name)"
        }
    }
}

function Get-InstallInventory {
    $manifest = Join-Path $script:buildPath 'install_manifest.txt'
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { throw 'CMake install_manifest.txt is missing.' }
    $installedFiles = New-Object 'System.Collections.Generic.List[string]'
    foreach ($line in Get-Content -LiteralPath $manifest) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $installedPath = [IO.Path]::GetFullPath($line)
        if (-not (Test-Within $installedPath $script:outputPath) -or $installedPath -eq $script:outputPath) {
            throw 'CMake install manifest belongs to another output directory.'
        }
        $relative = $installedPath.Substring($script:outputPath.Length + 1).Replace('\', '/')
        $null = Require-File $relative
        $installedFiles.Add($relative)
    }
    # Keep all installed paths, and hash the complete native/Python runtime.
    # This catches missing DLLs even when the verifier itself can still start.
    $criticalFiles = @($installedFiles | Where-Object { $_ -match '(?i)\.(dll|exe|pyd)$|^python/' })
    if ('OrcaSlicer.dll' -notin $criticalFiles -or 'python/python312.dll' -notin $criticalFiles -or
        'resources/tools/uv/uv.exe' -notin $criticalFiles) { throw 'CMake install manifest is missing required native/Python runtime entries.' }
    return @{ installed_files = $installedFiles.ToArray(); runtime_sha256 = (Get-Fingerprints $script:outputPath $criticalFiles) }
}

$buildPath = Resolve-LocalPath $BuildDir
$outputPath = Resolve-LocalPath $OutputDir
if ($outputPath -eq [IO.Path]::GetPathRoot($outputPath).TrimEnd('\', '/') -or
    (Test-Within $repoRoot $outputPath) -or (Test-Within $buildPath $outputPath)) {
    throw 'Output must not be a volume root, the repository/build root, or their ancestor.'
}
if ([IO.Path]::GetDirectoryName($outputPath) -ne $buildPath -or
    [IO.Path]::GetFileName($outputPath).StartsWith('.')) {
    throw 'Output must be a dedicated, non-hidden direct child of the selected build directory.'
}
foreach ($reserved in @('src', 'resources', 'tools', 'scripts', 'deps', 'deps_src', 'localization')) {
    if (Test-Within $outputPath (Join-Path $repoRoot $reserved)) { throw "Output overlaps source directory: $reserved" }
}
foreach ($reserved in @('src', 'tests', '_deps', 'CMakeFiles', 'local-runtime-logs')) {
    if (Test-Within $outputPath (Join-Path $buildPath $reserved)) { throw "Output overlaps build internals: $reserved" }
}
Assert-PlainAncestors $outputPath
Assert-PlainTree $outputPath
if ($Revision -notmatch '^[0-9A-Za-z._-]+$') { throw 'Revision contains unsupported characters.' }
$cachePath = Join-Path $buildPath 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) { throw 'BuildDir must contain an existing CMakeCache.txt.' }
$cacheText = Get-Content -LiteralPath $cachePath -Raw
if ((Resolve-LocalPath (Read-CacheValue 'CMAKE_HOME_DIRECTORY')) -ne $repoRoot) {
    throw 'The selected build belongs to another source checkout.'
}
foreach ($prefix in (Read-CacheValue 'CMAKE_PREFIX_PATH').Split(';')) {
    if (-not $prefix.Trim()) { continue }
    $dependencyPath = Resolve-LocalPath $prefix
    if ((Test-Within $outputPath $dependencyPath) -or (Test-Within $dependencyPath $outputPath)) {
        throw 'Output overlaps the shared dependency prefix.'
    }
}
if (-not $CMakeExecutable) { $CMakeExecutable = Read-CacheValue 'CMAKE_COMMAND' }
if (-not (Test-Path -LiteralPath $CMakeExecutable -PathType Leaf)) { throw 'The configured CMake executable is missing.' }
$ownerPath = Join-Path $outputPath $ownerName
$markerPath = Join-Path $outputPath $markerName
$receiptPath = Join-Path $outputPath $receiptName
if (Test-Path -LiteralPath $outputPath) {
    if (-not (Test-Path -LiteralPath $outputPath -PathType Container)) { throw 'Output must be a directory.' }
    if (Test-Path -LiteralPath $ownerPath -PathType Leaf) {
        $owner = Get-Content -LiteralPath $ownerPath -Raw | ConvertFrom-Json
        if ($owner.schema_version -ne 1 -or $owner.tool -ne 'stage_local_ai_runtime.ps1' -or
            $owner.source_directory -ne $repoRoot -or $owner.build_directory -ne $buildPath -or
            $owner.output_directory -ne $outputPath) { throw 'Output ownership does not match this source/build directory.' }
    } elseif (@(Get-ChildItem -LiteralPath $outputPath -Force).Count) {
        throw 'Refusing a nonempty output directory without a matching ownership file.'
    }
}
if (-not (Test-Path -LiteralPath $ownerPath)) {
    if ($VerifyOnly) { throw 'The output directory has no ownership file; run a complete local build first.' }
    New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
    [ordered]@{ schema_version = 1; tool = 'stage_local_ai_runtime.ps1'; source_directory = $repoRoot;
        build_directory = $buildPath; output_directory = $outputPath; created_utc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $ownerPath -Encoding UTF8
}
if (Test-Path -LiteralPath $markerPath) {
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) { throw 'The verification marker must be a regular file.' }
    Remove-Item -LiteralPath $markerPath -Force
}
$logDir = Join-Path $buildPath 'local-runtime-logs'
Assert-PlainAncestors $logDir
New-Item -ItemType Directory -Path $logDir -Force | Out-Null

if (-not $VerifyOnly) {
    $sourcesBeforeBuild = Get-Fingerprints $repoRoot $startupSources
    Invoke-Logged $CMakeExecutable @('-S', $repoRoot, '-B', $buildPath,
        '-DORCA_AI_WINDOWS_INSTALLER:BOOL=ON', '-DORCA_AI_DISTRIBUTION_CHANNEL:STRING=internal',
        "-DORCA_AI_PACKAGE_REVISION:STRING=$Revision", '-DORCA_AI_INTERNAL_DEFAULTS_FILE:FILEPATH=') 'configure.log'
    $cacheText = Get-Content -LiteralPath $cachePath -Raw
    Invoke-Logged $CMakeExecutable @('--build', $buildPath, '--config', 'Release', '--target',
        'OrcaSlicer_app_gui', 'COPY_DLLS', '--', '/m:2', '/p:CL_MPCount=4', '/verbosity:minimal') 'build.log'
    Assert-Fingerprints $repoRoot ($sourcesBeforeBuild | ConvertTo-Json | ConvertFrom-Json) 'startup source'
    foreach ($locale in @('zh_CN', 'zh_TW')) {
        New-Item -ItemType Directory -Path (Join-Path $repoRoot "resources/i18n/$locale") -Force | Out-Null
        Invoke-Logged (Join-Path $repoRoot 'tools/msgfmt.exe') @('--check-format', '-o',
            (Join-Path $repoRoot "resources/i18n/$locale/OrcaSlicer.mo"),
            (Join-Path $repoRoot "localization/i18n/$locale/OrcaSlicer_$locale.po")) 'localization.log'
    }
    # The install rules copy actual resources and the complete Sidecar manifest.
    # Never stage into build/src/Release/resources: that path is a source junction.
    Assert-PlainAncestors $outputPath
    Assert-PlainTree $outputPath
    Invoke-Logged $CMakeExecutable @('--install', $buildPath, '--config', 'Release', '--prefix', $outputPath) 'install.log'
    $inventory = Get-InstallInventory
    [ordered]@{ schema_version = 1; status = 'build_and_install_succeeded';
        build_completed_utc = [DateTime]::UtcNow.ToString('o'); output_directory = $outputPath;
        configuration = 'Release'; targets = @('OrcaSlicer_app_gui', 'COPY_DLLS');
        source_scope = 'startup files listed by stage_local_ai_runtime.ps1; not the entire C++ working tree';
        startup_source_sha256 = $sourcesBeforeBuild;
        startup_resource_sha256 = (Get-Fingerprints $repoRoot $startupResources);
        desktop_binary_sha256 = (Get-Fingerprints (Join-Path $buildPath 'src/Release') @('orca-slicer.exe', 'OrcaSlicer.dll'));
        installed_files = $inventory.installed_files; runtime_sha256 = $inventory.runtime_sha256 } |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
}

if ((Read-CacheValue 'ORCA_AI_WINDOWS_INSTALLER') -ne 'ON') { throw 'AI runtime packaging is not enabled in this build.' }
Assert-PlainTree $outputPath
$required = @('orca-slicer.exe', 'OrcaSlicer.dll', 'python/python.exe', 'python/pythonw.exe', 'python/python312.dll',
    'resources/tools/uv/uv.exe',
    'resources/images/OrcaSlicer_gradient_circle.svg', 'resources/i18n/zh_CN/OrcaSlicer.mo',
    'resources/i18n/zh_TW/OrcaSlicer.mo', 'resources/tools/ai/orca_ai_build_info.json',
    'resources/tools/ai/orca_ai_runtime_dependencies.json')
foreach ($relative in $required) { $null = Require-File $relative }
if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) { throw 'Local build receipt is missing; run a complete local build first.' }
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
if ($receipt.schema_version -ne 1 -or $receipt.status -ne 'build_and_install_succeeded' -or
    $receipt.output_directory -ne $outputPath) { throw 'The build receipt does not match this output directory.' }
foreach ($relative in $startupSources) {
    if ($relative -notin $receipt.startup_source_sha256.PSObject.Properties.Name) { throw "Build receipt is missing startup source: $relative" }
}
foreach ($relative in $startupResources) {
    if ($relative -notin $receipt.startup_resource_sha256.PSObject.Properties.Name) { throw "Build receipt is missing startup resource: $relative" }
}
Assert-Fingerprints $repoRoot $receipt.startup_source_sha256 'startup source'
Assert-Fingerprints $repoRoot $receipt.startup_resource_sha256 'startup resource'
Assert-Fingerprints $outputPath $receipt.startup_resource_sha256 'installed startup resource'
Assert-Fingerprints (Join-Path $buildPath 'src/Release') $receipt.desktop_binary_sha256 'desktop build binary'
Assert-Fingerprints $outputPath $receipt.desktop_binary_sha256 'installed desktop binary'
foreach ($relative in $receipt.installed_files) {
    $resolved = [IO.Path]::GetFullPath((Join-Path $outputPath $relative))
    if (-not (Test-Within $resolved $outputPath) -or $resolved -eq $outputPath) { throw 'Invalid install path in build receipt.' }
    $null = Require-File $relative
}
Assert-Fingerprints $outputPath $receipt.runtime_sha256 'installed native/Python runtime'
if (Test-Path -LiteralPath (Join-Path $outputPath 'resources/tools/ai/orca_ai_internal_defaults.json')) {
    throw 'Local runtime must not contain an embedded provider-defaults file.'
}

# Derive the Sidecar file set from the existing install rule instead of maintaining
# a second list that can silently omit a newly introduced Python module.
$cmakeSource = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
$runtimeBlock = [regex]::Match($cmakeSource, '(?s)set\(ORCA_AI_SIDECAR_RUNTIME_FILES\s+(.*?)\)\s*install\(FILES')
$runtimeFiles = @([regex]::Matches($runtimeBlock.Groups[1].Value, '/tools/ai/([^"/]+\.py)"') |
    ForEach-Object { $_.Groups[1].Value })
if ($runtimeFiles.Count -lt 3 -or $runtimeFiles -notcontains 'orca_ai_installed_bootstrap.py' -or
    $runtimeFiles -notcontains 'orca_ai_sidecar.py' -or $runtimeFiles -notcontains 'verify_bundled_runtime.py') {
    throw 'Unable to resolve the complete Sidecar install manifest.'
}
foreach ($name in $runtimeFiles) {
    $staged = Require-File "resources/tools/ai/$name"
    $source = Join-Path $repoRoot "tools/ai/$name"
    if ((Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash) { throw "Stale Sidecar source in runtime: $name" }
}
$allowedAiFiles = $runtimeFiles + @('orca_ai_build_info.json', 'orca_ai_runtime_dependencies.json')
foreach ($entry in Get-ChildItem -LiteralPath (Join-Path $outputPath 'resources/tools/ai') -Force) {
    if ($entry.PSIsContainer -and $entry.Name -eq '__pycache__') { continue }
    if ($entry.Name -notin $allowedAiFiles) { throw "Unexpected/stale file in Sidecar directory: $($entry.Name)" }
}
$identity = Get-Content -LiteralPath (Require-File 'resources/tools/ai/orca_ai_build_info.json') -Raw | ConvertFrom-Json
$dependencies = Get-Content -LiteralPath (Require-File 'resources/tools/ai/orca_ai_runtime_dependencies.json') -Raw | ConvertFrom-Json
$sourceHead = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $sourceHead -notmatch '^[a-f0-9]{40}$') { throw 'Cannot determine source commit.' }
if ($identity.schema_version -ne 1 -or $identity.application_commit -ne $sourceHead -or
    $identity.package_revision -ne $Revision -or $identity.distribution_channel -ne 'internal' -or
    $identity.sidecar_protocol_version -ne 2 -or $identity.sidecar_version -ne 'orcaslicer-ai-sidecar-v9') {
    throw 'Installed AI build identity does not match this local source and revision.'
}
foreach ($name in @('orca_ai_build_info.json', 'orca_ai_runtime_dependencies.json')) {
    if ((Get-FileHash -LiteralPath (Join-Path $outputPath "resources/tools/ai/$name") -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $buildPath $name) -Algorithm SHA256).Hash) {
        throw "Installed metadata does not match the configured build: $name"
    }
}
$pillow = @($dependencies.packages | Where-Object { $_.name -eq 'Pillow' })
$pillowHashes = @{ x64 = '7f84204dee22a783350679a0333981df803dac21a0190d706a50475e361c93f5';
    arm64 = 'af73337013e0b3b46f175e79492d96845b16126ddf79c438d7ea7ff27783a414' }
if ($dependencies.schema_version -ne 1 -or $dependencies.python.version -ne '3.12.13' -or
    $dependencies.python.isolation_flag -ne '-I' -or $pillow.Count -ne 1 -or
    $pillow[0].version -ne '12.2.0' -or -not $pillowHashes.ContainsKey($pillow[0].architecture) -or
    $pillow[0].sha256 -ne $pillowHashes[$pillow[0].architecture]) { throw 'Pinned Python/Pillow dependency metadata is invalid.' }
foreach ($name in @('orca-slicer.exe', 'OrcaSlicer.dll')) {
    if ((Get-FileHash -LiteralPath (Require-File $name) -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $buildPath "src/Release/$name") -Algorithm SHA256).Hash) {
        throw "Installed binary differs from the current build: $name"
    }
}
Invoke-Logged (Require-File 'python/python.exe') @('-I', '-B',
    (Require-File 'resources/tools/ai/verify_bundled_runtime.py'), '--python-root',
    (Join-Path $outputPath 'python'), '--expect-python', '3.12.13', '--expect-pillow', '12.2.0', '--json') 'verify.log'

$hashes = [ordered]@{}
foreach ($relative in $required + @($runtimeFiles | ForEach-Object { "resources/tools/ai/$_" })) {
    $hashes[$relative] = (Get-FileHash -LiteralPath (Require-File $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$sourceDirty = -not [string]::IsNullOrWhiteSpace((& git -C $repoRoot status --porcelain --untracked-files=normal) -join "`n")
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source working-tree state.' }
$report = [ordered]@{ schema_version = 1; status = 'verified'; verified_utc = [DateTime]::UtcNow.ToString('o');
    source_commit = $sourceHead; source_has_uncommitted_changes = $sourceDirty; package_revision = $Revision;
    output_directory = $outputPath; checks = @('complete_current_sidecar', 'matching_build_identity',
        'matching_desktop_binaries', 'startup_sources_match_build_receipt', 'complete_install_manifest',
        'native_python_runtime_hashes', 'matching_startup_resources', 'no_reparse_points',
        'no_embedded_provider_defaults', 'isolated_python_pillow_png');
    sha256 = $hashes }
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $markerPath -Encoding UTF8
Write-Output "Local AI runtime verified: $outputPath"
Write-Output "Verification marker: $markerPath"
