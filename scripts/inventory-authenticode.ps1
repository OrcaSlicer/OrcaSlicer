[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$ArtifactDirectory
)

$ErrorActionPreference = "Stop"

function Get-RelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $rootWithSeparator = $Root.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $rootUri = [Uri]$rootWithSeparator
    $pathUri = [Uri]$Path

    [Uri]::UnescapeDataString($rootUri.MakeRelativeUri($pathUri).ToString()).Replace("/", [IO.Path]::DirectorySeparatorChar)
}

$artifactRoot = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$binaries = Get-ChildItem -LiteralPath $artifactRoot -Recurse -File |
    Where-Object { $_.Extension -in @(".exe", ".dll") } |
    Sort-Object -Property FullName

if (-not $binaries) {
    Write-Warning "No .exe or .dll files found under '$artifactRoot'."
    exit 0
}

$binaries | ForEach-Object {
    $signature = Get-AuthenticodeSignature -LiteralPath $_.FullName

    [pscustomobject]@{
        RelativePath = Get-RelativePath -Root $artifactRoot -Path $_.FullName
        Status = $signature.Status
        SignatureType = $signature.SignatureType
        Signer = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { "" }
        SignerThumbprint = if ($signature.SignerCertificate) { $signature.SignerCertificate.Thumbprint } else { "" }
        Timestamped = [bool]$signature.TimeStamperCertificate
        TimeStamper = if ($signature.TimeStamperCertificate) { $signature.TimeStamperCertificate.Subject } else { "" }
    }
} | Format-Table -AutoSize
