[CmdletBinding()]
param(
    [string]$BuildRoot = 'D:\TEST\mp-r6',
    [string]$BazelSh = 'D:\Git\bin\bash.exe',
    [string]$VisualCpp = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC',
    [ValidateRange(1,32)][int]$Jobs = 6,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'SemanticRuntimeSourceAudit.psm1') -Force
$semanticSourceCommit = '6d31f1ebc3284db74d211d62bdc4f0a0c29ea120'
$semanticBuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$semanticDownloads = Join-Path $semanticBuildRoot 'downloads'
$semanticSource = Join-Path $semanticBuildRoot 'src'
$semanticInputs = @(
    @{ file='mediapipe-source.zip'; url="https://codeload.github.com/google-ai-edge/mediapipe/zip/$semanticSourceCommit"; sha256='ef1437815f8a6c7c0eb7f5fb8181d3ef542489f2576a971d72fa94b33d43c0d2' },
    @{ file='bazel.exe'; url='https://github.com/bazelbuild/bazel/releases/download/7.4.1/bazel-7.4.1-windows-x86_64.exe'; sha256='4a76eddf6c5115e1d93355fd11db5ac2fc20e58f197f5d65d3f21da92aa0925b' },
    # GitLab returned HTTP 403 for the public archive. The TensorFlow mirror
    # serves byte-identical content; retain the exact upstream WORKSPACE hash.
    @{ file='eigen-4c38131a16803130b66266a912029504f2cf23cd.tar.gz'; url='https://storage.googleapis.com/mirror.tensorflow.org/gitlab.com/libeigen/eigen/-/archive/4c38131a16803130b66266a912029504f2cf23cd/eigen-4c38131a16803130b66266a912029504f2cf23cd.tar.gz'; sha256='1a432ccbd597ea7b9faa1557b1752328d6adc1a3db8969f6fe793ff704be3bf0' },
    # The public GitHub release API serves the identical BCR-pinned artifact
    # when the github.com release redirect cannot establish a connection.
    @{ file='rules_js-v2.7.0.tar.gz'; url='https://api.github.com/repos/aspect-build/rules_js/releases/assets/305837769'; headers=@{Accept='application/octet-stream'}; sha256='9dd50d3bacb2fe1d4a721098981b70290fe9ac56d3625791f490d2ab94f2cac6' }
)
New-Item -ItemType Directory -Path $semanticDownloads -Force | Out-Null
foreach ($semanticInput in $semanticInputs) {
    $semanticFile = Join-Path $semanticDownloads $semanticInput.file
    if (-not (Test-Path -LiteralPath $semanticFile)) {
        Write-Host "Downloading pinned build input: $($semanticInput.file)"
        if ($semanticInput.ContainsKey('headers')) {
            Invoke-WebRequest -UseBasicParsing -Uri $semanticInput.url -Headers $semanticInput.headers -OutFile $semanticFile
        } else { Invoke-WebRequest -UseBasicParsing -Uri $semanticInput.url -OutFile $semanticFile }
    }
    if ((Get-FileHash -LiteralPath $semanticFile -Algorithm SHA256).Hash -ine $semanticInput.sha256) {
        throw "Build input checksum mismatch: $semanticFile"
    }
}
if (-not (Test-Path -LiteralPath $semanticSource)) {
    $semanticExtract = Assert-SemanticChildPath $semanticBuildRoot (Join-Path $semanticBuildRoot 'source-extract')
    $semanticMoveSource = Assert-SemanticChildPath $semanticBuildRoot (Join-Path $semanticExtract "mediapipe-$semanticSourceCommit")
    $semanticMoveDestination = Assert-SemanticChildPath $semanticBuildRoot $semanticSource
    Expand-Archive -LiteralPath (Join-Path $semanticDownloads 'mediapipe-source.zip') -DestinationPath $semanticExtract
    # Both recursive move paths are resolved and checked against this explicit
    # build root before moving. Former Orca workspaces are never involved.
    Move-Item -LiteralPath $semanticMoveSource -Destination $semanticMoveDestination
}
$semanticFactory = Join-Path $semanticSource 'mediapipe/tasks/cc/core/logging/factory/logging_factory.cc'
$semanticFactoryText = Get-Content -LiteralPath $semanticFactory -Raw
if ($semanticFactoryText -notmatch 'return TasksDummyLogger::Create\(\);') { throw 'Expected upstream dummy logger factory' }
$semanticFactoryBuild = Get-Content -LiteralPath (Join-Path $semanticSource 'mediapipe/tasks/cc/core/logging/factory/BUILD') -Raw
if ($semanticFactoryBuild -match 'wininet|winhttp|telemetry|metrics_logger') { throw 'Unexpected telemetry dependency in logger factory' }
$semanticTargetFile = Join-Path $semanticSource 'mediapipe/tasks/c/BUILD'
$semanticTarget = @'

# Orca R6: minimum CPU C ABI surface; public source logging factory is dummy-only.
cc_binary(
    name = "orca_portrait_semantics.dll",
    linkshared = True,
    linkstatic = True,
    deps = [
        "//mediapipe/tasks/c/core:common",
        "//mediapipe/tasks/c/vision/core:image_c_lib",
        "//mediapipe/tasks/c/vision/face_landmarker:face_landmarker_c_lib",
        "//mediapipe/tasks/c/vision/image_segmenter:image_segmenter_c_lib",
    ],
)
'@
Assert-SemanticSourceSnapshot -Archive (Join-Path $semanticDownloads 'mediapipe-source.zip') -Source $semanticSource -Commit $semanticSourceCommit -TargetAddition $semanticTarget -ReceiptDirectory $semanticBuildRoot
$semanticNoSwift = Join-Path $semanticBuildRoot 'no-swift-toolchain'
New-Item -ItemType Directory -Path $semanticNoSwift -Force | Out-Null
'# This CPU C ABI target contains no Swift actions.' | Set-Content -LiteralPath (Join-Path $semanticNoSwift 'BUILD') -Encoding UTF8
'workspace(name="build_bazel_rules_swift_local_config")' | Set-Content -LiteralPath (Join-Path $semanticNoSwift 'WORKSPACE') -Encoding UTF8
$semanticBuildArguments = @(
    "--output_user_root=$semanticBuildRoot/bzl", 'build', '-c', 'opt', "--jobs=$Jobs",
    '--local_resources=memory=10000', '--define=MEDIAPIPE_DISABLE_GPU=1',
    '--conlyopt=/std:c11', '--conlyopt=/experimental:c11atomics',
    '--host_conlyopt=/std:c11', '--host_conlyopt=/experimental:c11atomics',
    '--copt=/utf-8', '--host_copt=/utf-8', '--define=protobuf_allow_msvc=true',
    '--copt=/Zc:preprocessor', '--host_copt=/Zc:preprocessor',
    '--features=static_link_msvcrt', '--host_features=static_link_msvcrt', '--define=xnn_enable_avx512amx=false',
    '--repo_env=HERMETIC_PYTHON_VERSION=3.12', "--repository_cache=$semanticBuildRoot/repository-cache",
    "--distdir=$semanticBuildRoot/downloads",
    "--override_repository=rules_swift~~non_module_deps~build_bazel_rules_swift_local_config=$semanticNoSwift",
    "--override_repository=windows_opencv=$semanticBuildRoot/opencv-install",
    '//mediapipe/tasks/c:orca_portrait_semantics.dll'
)
$semanticReceipt = [ordered]@{
    schema='orca.semantic-offline-build/v1'; source_commit=$semanticSourceCommit;
    prepared_utc=[DateTime]::UtcNow.ToString('o'); build_inputs=$semanticInputs;
    target='//mediapipe/tasks/c:orca_portrait_semantics.dll'; arguments=$semanticBuildArguments;
    bazel_sh=$BazelSh; visual_cpp=$VisualCpp;
    logging_factory_sha256=(Get-FileHash -LiteralPath $semanticFactory -Algorithm SHA256).Hash.ToLowerInvariant();
    logger='upstream TasksDummyLogger'; status='prepared-not-verified';
    boundary='Build-time downloads only. Candidate runtime still requires dependency, C ABI inference, and application network validation.'
}
$semanticReceipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $semanticBuildRoot 'build-input-receipt.json') -Encoding UTF8
if ($PrepareOnly) { Write-Host "Prepared source build: $semanticSource"; exit 0 }
& (Join-Path $PSScriptRoot 'build_semantic_opencv.ps1') -BuildRoot $semanticBuildRoot -Jobs $Jobs
if (-not (Test-Path -LiteralPath $BazelSh)) { throw "Missing build shell: $BazelSh" }
if (-not (Test-Path -LiteralPath $VisualCpp)) { throw "Missing Windows C++ toolchain: $VisualCpp" }
$semanticLog = Join-Path $semanticBuildRoot ('build-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '.log')
$semanticPreviousEnvironment = @{ BAZEL_SH=$env:BAZEL_SH; BAZEL_VC=$env:BAZEL_VC; PATH=$env:PATH; JAVA_HOME=$env:JAVA_HOME }
Push-Location $semanticSource
try {
    $env:BAZEL_SH = $BazelSh
    $env:BAZEL_VC = $VisualCpp
    $semanticGitRoot = Split-Path (Split-Path $BazelSh -Parent) -Parent
    $env:PATH = (Join-Path $semanticGitRoot 'usr/bin') + ';' + $env:PATH
    # rules_java 7.10's no-system-JDK branch contains an invalid bootstrap
    # toolchain label. The pinned Bazel distribution contains a Java runtime;
    # that runtime satisfies local_java_repository's java executable check.
    # This C++ target does not require javac or ship Java to the application.
    & (Join-Path $semanticDownloads 'bazel.exe') "--output_user_root=$semanticBuildRoot/bzl" version *> (Join-Path $semanticBuildRoot 'bazel-version.log')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize the pinned Bazel toolchain' }
    $semanticJdks = @(Get-ChildItem -LiteralPath (Join-Path $semanticBuildRoot 'bzl/install') -Directory | ForEach-Object { Join-Path $_.FullName 'embedded_tools/jdk' } | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'bin/java.exe') })
    if ($semanticJdks.Count -ne 1) { throw 'Expected one embedded JDK for pinned Bazel 7.4.1' }
    $env:JAVA_HOME = $semanticJdks[0]
    $semanticOutputBase = & (Join-Path $semanticDownloads 'bazel.exe') "--output_user_root=$semanticBuildRoot/bzl" info output_base 2> (Join-Path $semanticBuildRoot 'bazel-output-base.log')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve the private Bazel output base' }
    $semanticOutputBase = ($semanticOutputBase | Select-Object -Last 1).Trim()
    $semanticCompat = Join-Path $PSScriptRoot 'apply_semantic_build_compat.py'
    & python $semanticCompat --output-base $semanticOutputBase --receipts (Join-Path $semanticBuildRoot 'compatibility')
    if ($LASTEXITCODE -ne 0) { throw 'Source-build compatibility verification failed' }
    & (Join-Path $semanticDownloads 'bazel.exe') @semanticBuildArguments *> $semanticLog
    $semanticExit = $LASTEXITCODE
    if ($semanticExit -ne 0 -and (Get-Content -LiteralPath $semanticLog -Raw) -match 'overlay_directories.py[\s\S]*WinError 1314') {
        # The first clean build may only fetch LLVM during target analysis.
        # Preserve that failed attempt, apply the declared host-only patch,
        # and retry once using the same verified dependency/source inputs.
        & python $semanticCompat --output-base $semanticOutputBase --receipts (Join-Path $semanticBuildRoot 'compatibility')
        if ($LASTEXITCODE -ne 0) { throw 'Newly fetched LLVM source failed compatibility verification' }
        $semanticLog = [IO.Path]::ChangeExtension($semanticLog, 'host-copy-retry.log')
        & (Join-Path $semanticDownloads 'bazel.exe') @semanticBuildArguments *> $semanticLog
        $semanticExit = $LASTEXITCODE
    }
    Get-Content -LiteralPath $semanticLog -Tail 60
    if ($semanticExit -ne 0) { throw "MediaPipe source build failed ($semanticExit): $semanticLog" }
} finally {
    Pop-Location
    foreach ($semanticEnvironmentName in $semanticPreviousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($semanticEnvironmentName, $semanticPreviousEnvironment[$semanticEnvironmentName], 'Process')
    }
}
Assert-SemanticSourceSnapshot -Archive (Join-Path $semanticDownloads 'mediapipe-source.zip') -Source $semanticSource -Commit $semanticSourceCommit -TargetAddition $semanticTarget -ReceiptDirectory $semanticBuildRoot
Write-Host "Source target built. Do not replace the production runtime until candidate verification passes. Log: $semanticLog"
