[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$ArtifactDirectory,

    [string[]]$Files = @(
        "orca-slicer.exe",
        "OrcaSlicer.dll"
    ),

    [string]$SignToolPath,

    # Accept signatures whose certificate chain terminates in an untrusted root.
    # Required for the SignPath test certificate (self-signed). Do NOT pass this
    # once a production CA-issued certificate is in use, so release builds enforce
    # a fully trusted chain.
    [switch]$AllowUntrustedRoot
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

    # Capture signtool output without letting native stderr (redirected via 2>&1)
    # raise a terminating NativeCommandError under $ErrorActionPreference = 'Stop'.
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $signtool verify /pa /all /tw /v $filePath 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    $output | ForEach-Object { Write-Host $_ }

    if ($exitCode -eq 0) {
        continue
    }

    if ($AllowUntrustedRoot) {
        # signtool interleaves its stdout (chain details) and stderr (the error)
        # unpredictably, so the error text cannot be matched reliably. Use the
        # structured Get-AuthenticodeSignature result instead: accept only when the
        # file is genuinely signed and the sole problem is that the chain terminates
        # in an untrusted root (i.e. the self-signed SignPath test certificate).
        $signature = Get-AuthenticodeSignature -LiteralPath $filePath
        $isSigned = ($signature.SignatureType -eq "Authenticode") -and ($null -ne $signature.SignerCertificate)
        $untrustedRootOnly = $signature.StatusMessage -match "terminated in a root certificate which is not trusted"
        if ($isSigned -and $untrustedRootOnly) {
            Write-Host "  Accepted: '$relativePath' is signed but its certificate chains to an untrusted root (expected for the SignPath test certificate)."
            continue
        }
    }

    throw "SignTool verification failed for '$relativePath' with exit code $exitCode."
}

Write-Host "Authenticode verification passed."

# signtool exits non-zero for the untrusted test-cert root even when we accept
# it above, leaving $LASTEXITCODE = 1. The GitHub Actions pwsh wrapper exits
# with that lingering code, so reset it to report success explicitly.
exit 0
