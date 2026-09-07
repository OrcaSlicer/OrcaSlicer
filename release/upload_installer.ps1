[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ManifestPath,
    [string] $SshTarget,
    [string] $RemoteDownloadDir = '/srv/3dprint-beer/data/downloads',
    [string] $RemoteOwner = 'web',
    [string] $RemoteGroup = 'web',
    [string] $EmployeeId,
    [switch] $RestrictedPublisher,
    [ValidateRange(1048576, 8388608)]
    [int] $RestrictedChunkSizeBytes = 4MB,
    [switch] $AllowSourceMismatch,
    [switch] $ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function ConvertTo-CanonicalEmployeeId {
    param([Parameter(Mandatory = $true)][string] $Value)

    $match = [regex]::Match($Value.Trim(), '^(?i:s)?0*([1-9][0-9]*)$')
    if (-not $match.Success) {
        throw "EmployeeId must be a complete numeric ID with optional s/S and zero prefix: $Value"
    }
    return $match.Groups[1].Value
}

function ConvertTo-LowerHex {
    param([Parameter(Mandatory = $true)][byte[]] $Bytes)
    return (($Bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Invoke-RestrictedSshText {
    param(
        [Parameter(Mandatory = $true)][string] $SshExecutable,
        [Parameter(Mandatory = $true)][string] $Target,
        [Parameter(Mandatory = $true)][string] $RemoteCommand
    )

    $lines = @(& $SshExecutable -o BatchMode=yes -o ConnectTimeout=15 `
        -o ServerAliveInterval=15 -o ServerAliveCountMax=2 -- $Target $RemoteCommand 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Restricted SSH command failed with exit code ${LASTEXITCODE}: $($lines -join ' ')"
    }
    return $lines
}

function Invoke-RestrictedSshBytes {
    param(
        [Parameter(Mandatory = $true)][string] $SshExecutable,
        [Parameter(Mandatory = $true)][string] $Target,
        [Parameter(Mandatory = $true)][string] $RemoteCommand,
        [Parameter(Mandatory = $true)][byte[]] $Buffer,
        [Parameter(Mandatory = $true)][int] $Count
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $SshExecutable
    $startInfo.Arguments = '-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=2 -- {0} {1}' -f `
        $Target, $RemoteCommand
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw 'Unable to start the restricted SSH chunk process.' }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    try {
        try {
            $process.StandardInput.BaseStream.Write($Buffer, 0, $Count)
        } finally {
            $process.StandardInput.Close()
        }
        if (-not $process.WaitForExit(180000)) {
            $process.Kill()
            throw 'Restricted SSH chunk process exceeded 180 seconds.'
        }
        $exitCode = $process.ExitCode
        $stdout = $stdoutTask.GetAwaiter().GetResult().Trim()
        $stderr = $stderrTask.GetAwaiter().GetResult().Trim()
    } finally {
        $process.Dispose()
    }
    if ($exitCode -ne 0) {
        throw "Restricted upload chunk failed with exit code ${exitCode}: $stderr"
    }
    return $stdout
}

function Invoke-RestrictedUpload {
    param(
        [Parameter(Mandatory = $true)][string] $Target,
        [Parameter(Mandatory = $true)][string] $CanonicalEmployeeId,
        [Parameter(Mandatory = $true)][System.IO.FileInfo] $Installer,
        [Parameter(Mandatory = $true)][string] $Sha256,
        [Parameter(Mandatory = $true)][string] $SourceCommit,
        [Parameter(Mandatory = $true)][string] $Revision,
        [Parameter(Mandatory = $true)][int] $ChunkSizeBytes
    )

    $sshCommand = Get-Command ssh.exe -ErrorAction SilentlyContinue
    if (-not $sshCommand) { $sshCommand = Get-Command ssh -ErrorAction SilentlyContinue }
    if (-not $sshCommand) { throw 'OpenSSH ssh was not found.' }

    $statusLines = @(Invoke-RestrictedSshText -SshExecutable $sshCommand.Source -Target $Target `
        -RemoteCommand "status $CanonicalEmployeeId")
    if ($statusLines -notcontains "employee_id=$CanonicalEmployeeId" -or
        $statusLines -notcontains 'protocol=2') {
        throw 'Restricted publisher identity/protocol check failed.'
    }

    $beginLines = @(Invoke-RestrictedSshText -SshExecutable $sshCommand.Source -Target $Target `
        -RemoteCommand ('begin {0} {1} {2} {3} {4} {5} {6}' -f $CanonicalEmployeeId,
            $Installer.Name, $Installer.Length, $Sha256.ToLowerInvariant(), $SourceCommit, $Revision,
            $ChunkSizeBytes))
    $offsetLine = $beginLines | Where-Object { $_ -match '^offset=[0-9]+$' } | Select-Object -First 1
    if (-not $offsetLine) { throw 'Restricted server did not return a resumable offset.' }
    [long] $offset = ($offsetLine -split '=', 2)[1]
    if ($offset -lt 0 -or $offset -gt $Installer.Length -or
        ($offset -ne $Installer.Length -and $offset % $ChunkSizeBytes -ne 0)) {
        throw "Restricted server returned an invalid offset: $offset"
    }

    $fileStream = [System.IO.File]::OpenRead($Installer.FullName)
    try {
        [void] $fileStream.Seek($offset, [System.IO.SeekOrigin]::Begin)
        $buffer = New-Object byte[] $ChunkSizeBytes
        $shaAlgorithm = [System.Security.Cryptography.SHA256]::Create()
        try {
            while ($offset -lt $Installer.Length) {
                $wanted = [int][Math]::Min([long]$ChunkSizeBytes, $Installer.Length - $offset)
                $count = 0
                while ($count -lt $wanted) {
                    $read = $fileStream.Read($buffer, $count, $wanted - $count)
                    if ($read -eq 0) { throw 'Installer ended before its recorded size.' }
                    $count += $read
                }
                $chunkHash = ConvertTo-LowerHex -Bytes $shaAlgorithm.ComputeHash($buffer, 0, $count)
                $chunkResult = Invoke-RestrictedSshBytes -SshExecutable $sshCommand.Source -Target $Target `
                    -RemoteCommand ('append {0} {1} {2} {3} {4}' -f $CanonicalEmployeeId,
                        $Installer.Name, $offset, $count, $chunkHash) -Buffer $buffer -Count $count
                $nextOffset = $offset + $count
                if ($chunkResult -notmatch "(?m)^offset=$nextOffset$") {
                    throw "Restricted server did not acknowledge chunk offset $nextOffset."
                }
                $offset = $nextOffset
            }
        } finally {
            $shaAlgorithm.Dispose()
        }
    } finally {
        $fileStream.Dispose()
    }

    $commitLines = @(Invoke-RestrictedSshText -SshExecutable $sshCommand.Source -Target $Target `
        -RemoteCommand ('commit {0} {1} {2} {3} {4} {5}' -f $CanonicalEmployeeId,
            $Installer.Name, $Installer.Length, $Sha256.ToLowerInvariant(), $SourceCommit, $Revision))
    $receipt = $commitLines -join "`n"
    if ($receipt -notmatch '(?m)^uploaded=true$' -or
        $receipt -notmatch "(?m)^employee_id=$CanonicalEmployeeId$") {
        throw "Restricted server did not return a valid publication receipt.`n$receipt"
    }
    return $receipt
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$resolvedManifest = (Resolve-Path -LiteralPath $ManifestPath).Path
$manifest = Get-Content -LiteralPath $resolvedManifest -Raw | ConvertFrom-Json
foreach ($field in @('installer', 'installer_sha256', 'source_commit', 'package_revision', 'distribution_channel')) {
    if (-not $manifest.PSObject.Properties[$field] -or [string]::IsNullOrWhiteSpace([string]$manifest.$field)) {
        throw "Release manifest is missing field: $field"
    }
}
if ($manifest.distribution_channel -ne 'internal' -or $manifest.source_commit -notmatch '^[0-9a-f]{40}$' -or
    $manifest.installer_sha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
    $manifest.package_revision -notmatch '^[0-9A-Za-z._-]+$') {
    throw 'Release manifest identity is invalid or is not an internal release.'
}
if ($manifest.installer -notmatch '^[0-9A-Za-z._-]+\.exe$') {
    throw "Unsafe installer filename in manifest: $($manifest.installer)"
}

$installerPath = Join-Path (Split-Path -Parent $resolvedManifest) $manifest.installer
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "Manifest installer does not exist beside the manifest: $installerPath"
}
$installerItem = Get-Item -LiteralPath $installerPath
$localHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
if ($localHash -ne $manifest.installer_sha256) {
    throw 'Installer SHA-256 does not match the release manifest.'
}

# Fresh offline checks precede ValidateOnly, readiness and every network operation.
# Historical manifests and an internal channel label are not credential clearance.
$pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
if (-not $pythonCommand) { $pythonCommand = Get-Command python3 -ErrorAction SilentlyContinue }
if (-not $pythonCommand) { throw 'Python 3.11+ is required for actual package inspection. Publication is blocked.' }
$artifactsToInspect = @($installerPath)
if ($manifest.PSObject.Properties['portable']) {
    if ($manifest.portable -notmatch '^[0-9A-Za-z._-]+\.zip$' -or
        -not $manifest.PSObject.Properties['portable_sha256']) {
        throw 'Invalid portable package identity in release manifest.'
    }
    $portablePath = Join-Path (Split-Path -Parent $resolvedManifest) $manifest.portable
    if (-not (Test-Path -LiteralPath $portablePath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $portablePath -Algorithm SHA256).Hash -ne $manifest.portable_sha256) {
        throw 'Portable package is missing or its hash does not match the manifest.'
    }
    $artifactsToInspect += $portablePath
}
foreach ($artifact in $artifactsToInspect) {
    & $pythonCommand.Source -I (Join-Path $PSScriptRoot 'verify_package_contents.py') $artifact --report "$artifact.contents.json"
    if ($LASTEXITCODE -ne 0) {
        throw "Publication blocked: package findings or incomplete inspection. Review $artifact.contents.json. No network operation performed."
    }
    $inspection = Get-Content -LiteralPath "$artifact.contents.json" -Raw | ConvertFrom-Json
    $expectedHash = if ($artifact -eq $installerPath) { $manifest.installer_sha256 } else { $manifest.portable_sha256 }
    if ($inspection.status -ne 'NOT_DETECTED_WITHIN_SCOPE' -or $inspection.sha256 -ne $expectedHash -or
        (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash -ne $expectedHash) {
        throw 'Package inspection identity changed or does not match the manifest. Publication blocked.'
    }
}

$gitHead = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $gitHead -notmatch '^[0-9a-f]{40}$') {
    throw 'Unable to read the current repository source identity.'
}
$gitStatus = @(& git -C $repoRoot status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw 'Unable to verify the current worktree.' }
if (-not $AllowSourceMismatch -and ($gitHead -ne $manifest.source_commit -or $gitStatus.Count -gt 0)) {
    throw 'Current clean HEAD must equal manifest source_commit before upload. Rebuild or use -AllowSourceMismatch only for read-only validation of an older artifact.'
}
$canonicalEmployeeId = $null
if ($RestrictedPublisher) {
    $canonicalEmployeeId = ConvertTo-CanonicalEmployeeId -Value $EmployeeId
}

$validationResult = [pscustomobject]@{
    Ready          = $true
    Manifest       = $resolvedManifest
    Installer      = $installerItem.FullName
    SizeBytes      = $installerItem.Length
    Sha256         = $localHash
    SourceCommit   = $manifest.source_commit
    CurrentHead    = $gitHead
    SourceMatches  = ($gitHead -eq $manifest.source_commit)
    UploadMode     = if ($RestrictedPublisher) { 'restricted' } else { 'administrator' }
    EmployeeId     = $canonicalEmployeeId
    ChunkSizeBytes = if ($RestrictedPublisher) { $RestrictedChunkSizeBytes } else { $null }
}
if ($ValidateOnly) {
    $validationResult
    return
}

if ($AllowSourceMismatch) {
    throw '-AllowSourceMismatch is permitted only together with -ValidateOnly.'
}

if ([string]::IsNullOrWhiteSpace($SshTarget) -or $SshTarget -notmatch '^[0-9A-Za-z_.@:-]+$') {
    throw 'Pass a shell-safe local SSH alias or user@host through -SshTarget.'
}
if ($RestrictedPublisher) {
    $receipt = Invoke-RestrictedUpload -Target $SshTarget -CanonicalEmployeeId $canonicalEmployeeId `
        -Installer $installerItem -Sha256 $localHash -SourceCommit $manifest.source_commit `
        -Revision $manifest.package_revision -ChunkSizeBytes $RestrictedChunkSizeBytes
    [pscustomobject]@{
        Uploaded       = $true
        UploadMode     = 'restricted'
        EmployeeId     = $canonicalEmployeeId
        Installer      = $manifest.installer
        SizeBytes      = $installerItem.Length
        Sha256         = $localHash
        SourceCommit   = $manifest.source_commit
        Receipt        = $receipt
    }
    return
}
if ($RemoteDownloadDir -notmatch '^/[0-9A-Za-z._/-]+$' -or
    $RemoteOwner -notmatch '^[0-9A-Za-z._-]+$' -or $RemoteGroup -notmatch '^[0-9A-Za-z._-]+$') {
    throw 'Remote directory, owner, or group contains unsupported shell characters.'
}

$remoteTemp = "/tmp/$($manifest.installer).upload-$([Guid]::NewGuid().ToString('N'))"
$remoteFinal = "$($RemoteDownloadDir.TrimEnd('/'))/$($manifest.installer)"
& scp $installerPath "${SshTarget}:$remoteTemp"
if ($LASTEXITCODE -ne 0) {
    throw "SCP upload failed with exit code $LASTEXITCODE."
}

$remoteCommand = 'set -eu; size=$(stat -c %s -- "{0}"); hash=$(sha256sum -- "{0}" | cut -d " " -f 1); test "$size" = "{1}"; test "$hash" = "{2}"; install -o "{3}" -g "{4}" -m 0644 -- "{0}" "{5}"; rm -f -- "{0}"; stat -c "%n %s bytes %U:%G %a" -- "{5}"; sha256sum -- "{5}"' -f `
    $remoteTemp, $installerItem.Length, $localHash.ToLowerInvariant(), $RemoteOwner, $RemoteGroup, $remoteFinal
& ssh $SshTarget $remoteCommand
if ($LASTEXITCODE -ne 0) {
    throw "Remote verification or installation failed with exit code $LASTEXITCODE. The unique temporary upload was left for diagnosis: $remoteTemp"
}

[pscustomobject]@{
    Uploaded       = $true
    SshTarget      = $SshTarget
    RemoteInstaller = $remoteFinal
    SizeBytes      = $installerItem.Length
    Sha256         = $localHash
    SourceCommit   = $manifest.source_commit
}
