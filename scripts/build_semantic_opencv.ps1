[CmdletBinding()]
param(
    [string]$BuildRoot = 'D:\TEST\mp-r6',
    [string]$CMake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    [ValidateRange(1,32)][int]$Jobs = 6
)
$ErrorActionPreference = 'Stop'
$semanticRoot = [IO.Path]::GetFullPath($BuildRoot)
$semanticArchive = Join-Path $semanticRoot 'downloads/opencv-3.4.11.tar.gz'
$semanticSha = '10898a0268d8f8cbaf0354ddd1d9de6abaac84e3d9a6c9754f56a0aa3383d73b'
$semanticUrl = 'https://codeload.github.com/opencv/opencv/tar.gz/refs/tags/3.4.11'
New-Item -ItemType Directory -Path (Join-Path $semanticRoot 'downloads') -Force | Out-Null
if (-not (Test-Path -LiteralPath $semanticArchive)) {
    Invoke-WebRequest -UseBasicParsing -Uri $semanticUrl -OutFile $semanticArchive
}
if ((Get-FileHash -LiteralPath $semanticArchive -Algorithm SHA256).Hash -ine $semanticSha) { throw 'OpenCV source checksum mismatch' }
$semanticCvSource = Join-Path $semanticRoot 'opencv-3.4.11'
if (-not (Test-Path -LiteralPath $semanticCvSource)) {
    & tar.exe -xzf $semanticArchive -C $semanticRoot
    if ($LASTEXITCODE -ne 0) { throw 'OpenCV source extraction failed' }
}
$semanticAuditScript = Join-Path $PSScriptRoot 'audit_semantic_tar_source.py'
& python $semanticAuditScript --archive $semanticArchive --source $semanticCvSource --prefix opencv-3.4.11 --sha256 $semanticSha --output (Join-Path $semanticRoot 'opencv-source-files.json') --allow-opencv-cache-marker
if ($LASTEXITCODE -ne 0) { throw 'OpenCV source audit failed; unknown edits are not accepted' }
$semanticCvBuild = Join-Path $semanticRoot 'opencv-build'
$semanticCvInstall = Join-Path $semanticRoot 'opencv-install'
$semanticConfigure = @(
    '-S', $semanticCvSource, '-B', $semanticCvBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_INSTALL_PREFIX=$semanticCvInstall", "-DOPENCV_DOWNLOAD_PATH=$semanticRoot/downloads/opencv-cache", '-DBUILD_LIST=core,imgproc',
    '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_WITH_STATIC_CRT=ON', '-DBUILD_TESTS=OFF',
    '-DBUILD_PERF_TESTS=OFF', '-DBUILD_EXAMPLES=OFF', '-DBUILD_JAVA=OFF',
    '-DBUILD_opencv_python2=OFF', '-DBUILD_opencv_python3=OFF', '-DBUILD_ZLIB=ON',
    '-DBUILD_PROTOBUF=OFF', '-DWITH_PROTOBUF=OFF', '-DBUILD_opencv_apps=OFF',
    '-DWITH_JPEG=OFF', '-DWITH_PNG=OFF', '-DWITH_TIFF=OFF', '-DWITH_WEBP=OFF',
    '-DWITH_OPENEXR=OFF', '-DWITH_JASPER=OFF',
    '-DWITH_IPP=OFF', '-DWITH_ITT=OFF', '-DWITH_OPENCL=OFF', '-DWITH_EIGEN=OFF',
    '-DWITH_CUDA=OFF', '-DWITH_FFMPEG=OFF', '-DWITH_MSMF=OFF', '-DWITH_DSHOW=OFF',
    '-DCPU_BASELINE=SSE2', '-DCPU_DISPATCH=SSE4_1,SSE4_2,AVX,AVX2',
    '-DOPENCV_SKIP_PYTHON_LOADER=ON'
)
$semanticBuildStamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
$semanticConfigureLog = Join-Path $semanticRoot "opencv-configure-$semanticBuildStamp.log"
& $CMake @semanticConfigure *> $semanticConfigureLog
if ($LASTEXITCODE -ne 0) { Get-Content $semanticConfigureLog -Tail 50; throw 'OpenCV configuration failed' }
$semanticBuildLog = Join-Path $semanticRoot "opencv-build-$semanticBuildStamp.log"
& $CMake --build $semanticCvBuild --config Release --target INSTALL --parallel $Jobs *> $semanticBuildLog
if ($LASTEXITCODE -ne 0) { Get-Content $semanticBuildLog -Tail 50; throw 'OpenCV compilation failed' }
$semanticLibs = @('opencv_imgproc3411.lib','opencv_core3411.lib','zlib.lib')
$semanticEntries = @()
foreach ($semanticName in $semanticLibs) {
    $semanticFound = @(Get-ChildItem -LiteralPath $semanticCvInstall -Recurse -File -Filter $semanticName)
    if ($semanticFound.Count -ne 1) { throw "Expected one installed static library: $semanticName" }
    $semanticRelative = $semanticFound[0].FullName.Substring($semanticCvInstall.Length+1).Replace('\','/')
    $semanticEntries += [ordered]@{ path=$semanticRelative; sha256=(Get-FileHash $semanticFound[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$semanticLibraryNames = ($semanticEntries | ForEach-Object { '        "' + $_.path + '",' }) -join "`n"
$semanticBazelBuild = @"
load("@rules_cc//cc:cc_library.bzl", "cc_library")
licenses(["notice"])
cc_library(
    name = "opencv",
    srcs = [
$semanticLibraryNames
    ],
    hdrs = glob(["include/opencv2/**"]),
    includes = ["include"],
    linkstatic = True,
    visibility = ["//visibility:public"],
)
"@
$semanticBazelBuild | Set-Content -LiteralPath (Join-Path $semanticCvInstall 'BUILD') -Encoding UTF8
'workspace(name="windows_opencv")' | Set-Content -LiteralPath (Join-Path $semanticCvInstall 'WORKSPACE') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $semanticCvSource 'LICENSE') -Destination (Join-Path $semanticCvInstall 'LICENSE')
[ordered]@{
    schema='orca.semantic-opencv-source/v1'; source_url=$semanticUrl; source_sha256=$semanticSha;
    version='3.4.11'; purpose='CPU image tensor preprocessing and segmentation resize';
    configure=$semanticConfigure; libraries=$semanticEntries; crt='static'; modules=@('core','imgproc');
    configure_log=$semanticConfigureLog; build_log=$semanticBuildLog; completed_utc=[DateTime]::UtcNow.ToString('o')
} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $semanticRoot 'opencv-build-receipt.json') -Encoding UTF8
Write-Host "Built minimal static OpenCV dependency: $semanticCvInstall"
