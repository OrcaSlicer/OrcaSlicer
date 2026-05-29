[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$ArtifactDirectory,

    [string[]]$Files = @(
        "orca-slicer.exe",
        "OrcaSlicer.dll"
    ),

    [string]$SignToolPath
)

$ErrorActionPreference = "Stop"

function Resolve-SignToolPath {
    param(
        [string]$ExplicitPath
    )

    if ($ExplicitPath) {
        if (Test-Path -LiteralPath $ExplicitPath -PathType Leaf) {
            return (Resolve-Path -LiteralPath $ExplicitPath).Path
        }

        throw "SignTool was not found at '$ExplicitPath'."
    }

    $fromPath = Get-Command -Name "signtool.exe" -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    $candidateRoots = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Container) }

    foreach ($root in $candidateRoots) {
        $candidate = Get-ChildItem -LiteralPath $root -Recurse -Filter "signtool.exe" -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "\\(x64|arm64)\\signtool\.exe$" } |
            Sort-Object -Property FullName -Descending |
            Select-Object -First 1

        if ($candidate) {
            return $candidate.FullName
        }
    }

    throw "signtool.exe was not found. Install the Windows SDK or pass -SignToolPath."
}

$artifactRoot = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$signtool = Resolve-SignToolPath -ExplicitPath $SignToolPath

Write-Host "Using SignTool: $signtool"
Write-Host "Verifying Authenticode signatures in: $artifactRoot"

foreach ($relativePath in $Files) {
    $filePath = Join-Path $artifactRoot $relativePath
    if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
        throw "Expected signed file was not found: $filePath"
    }

    Write-Host "Verifying $relativePath"
    & $signtool verify /pa /all /tw /v $filePath
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool verification failed for '$relativePath' with exit code $LASTEXITCODE."
    }
}

Write-Host "Authenticode verification passed."
