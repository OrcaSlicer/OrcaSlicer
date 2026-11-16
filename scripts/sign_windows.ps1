<#
.SYNOPSIS
    Authenticode-signs the Windows binaries OrcaSlicer ships, or checks that they are signed.

.DESCRIPTION
    Signs with Jsign (https://github.com/ebourg/jsign) and the release key in Google Cloud KMS. The
    private key never leaves the HSM: Jsign sends each file's digest to Cloud KMS and builds the
    signature from the result and the certificate chain. Every signature is timestamped (RFC 3161),
    so it stays valid after the certificate expires.

    A directory is searched recursively for .exe, .dll and .pyd files. A file named directly is
    handled whatever its name, which covers the temporary files makensis passes for the uninstaller
    and the installer. Files that already carry a signature are left alone: they are vendor-signed
    redistributables such as the Microsoft runtime, and signing them again would nest a second
    signature under the vendor's.

    With -Verify nothing is signed. The check fails if any of those files is unsigned or has a
    damaged signature, if a file named directly is not signed with the release certificate, or if
    a signature made with the release certificate has no timestamp.

    build_orca.yml sets the environment. CMakeLists.txt wires the script into the NSIS installer
    build when ORCA_SIGN_KMS_KEY is set at configure time.

    Environment:
      ORCA_SIGN_KMS_KEY   Cloud KMS key version:
                          projects/<p>/locations/<l>/keyRings/<r>/cryptoKeys/<k>/cryptoKeyVersions/<n>
      ORCA_SIGN_CERTFILE  PEM certificate chain issued for that key, leaf certificate first.
      ORCA_SIGN_JSIGN     Path of the Jsign jar. Java comes from JAVA_HOME, else from PATH.
      ORCA_SIGN_TOKEN     OAuth access token allowed to sign with the key. Not needed for -Verify.

    Runs under PowerShell 7 and Windows PowerShell 5.1.

.PARAMETER Path
    Files and directories to sign or check.

.PARAMETER Verify
    Check the signatures instead of signing.

.EXAMPLE
    scripts\sign_windows.ps1 -Path build\OrcaSlicer, build\src\Release\OrcaSlicer_profile_validator.exe
#>
param(
    [Parameter(Mandatory)] [string[]] $Path,
    [switch] $Verify
)
$ErrorActionPreference = 'Stop'

# Sectigo issued the certificate; DigiCert's server takes over if Sectigo's does not answer.
$TimestampUrls = 'http://timestamp.sectigo.com,http://timestamp.digicert.com'

function Require-Env([string] $name) {
    $value = [Environment]::GetEnvironmentVariable($name)
    if (-not $value) {
        throw "$name is not set. See the description in $PSCommandPath."
    }
    $value
}

# The release certificate is the first one in the chain file.
function Get-ReleaseCertificate([string] $pemFile) {
    $pem = Get-Content -LiteralPath $pemFile -Raw
    $match = [regex]::Match($pem, '-----BEGIN CERTIFICATE-----\s*([A-Za-z0-9+/=\s]+?)\s*-----END CERTIFICATE-----')
    if (-not $match.Success) {
        throw "No certificate in $pemFile."
    }
    $der = [Convert]::FromBase64String(($match.Groups[1].Value -replace '\s', ''))
    New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 (, $der)
}

# Each entry is a file and whether it was named directly.
$files = @()
foreach ($item in $Path) {
    if (Test-Path -LiteralPath $item -PathType Container) {
        $files += Get-ChildItem -LiteralPath $item -Recurse -File |
            Where-Object { $_.Extension -in '.exe', '.dll', '.pyd' } |
            ForEach-Object { [pscustomobject] @{ Path = $_.FullName; Named = $false } }
    } elseif (Test-Path -LiteralPath $item -PathType Leaf) {
        $files += [pscustomobject] @{ Path = (Resolve-Path -LiteralPath $item).ProviderPath; Named = $true }
    } else {
        throw "$item does not exist."
    }
}

$certFile = Require-Env 'ORCA_SIGN_CERTFILE'
$release = Get-ReleaseCertificate $certFile

if ($Verify) {
    # A release certificate that does not chain to a trusted root (a self-signed test certificate)
    # leaves Windows reporting UnknownError for a signature that is otherwise sound.
    $chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
    $chain.ChainPolicy.RevocationMode = 'NoCheck'
    $trusted = $chain.Build($release)
    if (-not $trusted) {
        Write-Warning "The release certificate $($release.Subject) does not chain to a trusted root."
    }

    $problems = @()
    $ourCount = 0
    foreach ($file in $files) {
        $signature = Get-AuthenticodeSignature -LiteralPath $file.Path
        $signer = $signature.SignerCertificate
        $ours = $signer -and $signer.Thumbprint -eq $release.Thumbprint
        if ($ours) {
            $ourCount++
        }
        if (-not $signer) {
            $problems += "unsigned: $($file.Path)"
        } elseif ($file.Named -and -not $ours) {
            $problems += "signed by $($signer.Subject), not the release certificate: $($file.Path)"
        } elseif ($ours -and -not $signature.TimeStamperCertificate) {
            $problems += "no timestamp: $($file.Path)"
        } elseif ($signature.Status -ne 'Valid' -and -not ($ours -and -not $trusted -and $signature.Status -eq 'UnknownError')) {
            $problems += "signature status $($signature.Status): $($file.Path)"
        }
    }
    if ($problems) {
        $problems | ForEach-Object { Write-Host $_ }
        throw "$($problems.Count) of $($files.Count) files failed the signature check."
    }
    Write-Host "All $($files.Count) files are signed, $ourCount of them with $($release.Subject)."
    return
}

$key = Require-Env 'ORCA_SIGN_KMS_KEY'
$jsign = Require-Env 'ORCA_SIGN_JSIGN'
$null = Require-Env 'ORCA_SIGN_TOKEN'
$keyMatch = [regex]::Match($key, '^(projects/[^/]+/locations/[^/]+/keyRings/[^/]+)/cryptoKeys/[^/]+/cryptoKeyVersions/\d+$')
if (-not $keyMatch.Success) {
    throw "ORCA_SIGN_KMS_KEY must name a key version, not '$key'."
}
$keyRing = $keyMatch.Groups[1].Value

$unsigned = @($files | Where-Object { -not (Get-AuthenticodeSignature -LiteralPath $_.Path).SignerCertificate } |
    ForEach-Object { $_.Path })
Write-Host "$($unsigned.Count) of $($files.Count) files to sign; the rest are already signed."
if (-not $unsigned) {
    return
}

$java = 'java'
if ($env:JAVA_HOME) {
    $java = Join-Path $env:JAVA_HOME 'bin\java.exe'
}
# Jsign reads the files to sign from a list file, one path per line (UTF-8 without a BOM).
$list = [IO.Path]::GetTempFileName()
try {
    [IO.File]::WriteAllLines($list, [string[]] $unsigned)
    & $java -jar $jsign --storetype GOOGLECLOUD --keystore $keyRing --alias $key `
        --storepass env:ORCA_SIGN_TOKEN --certfile $certFile --alg SHA-256 `
        --tsaurl $TimestampUrls --tsmode RFC3161 --tsretries 5 --tsretrywait 15 `
        --name OrcaSlicer --url https://www.orcaslicer.com "@$list"
    if ($LASTEXITCODE -ne 0) {
        throw "Jsign failed with exit code $LASTEXITCODE."
    }
} finally {
    Remove-Item -LiteralPath $list -ErrorAction SilentlyContinue
}
