Set-StrictMode -Version Latest

function Assert-SemanticChildPath([string]$Root, [string]$Path) {
    $semanticRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    $semanticPath = [IO.Path]::GetFullPath($Path)
    if (-not $semanticPath.StartsWith($semanticRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Semantic build path is outside its explicit root: $semanticPath"
    }
    return $semanticPath
}

function Get-SemanticStreamHash([IO.Stream]$Stream) {
    $semanticHasher = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($semanticHasher.ComputeHash($Stream))).Replace('-','').ToLowerInvariant() }
    finally { $semanticHasher.Dispose() }
}

function Assert-SemanticSourceSnapshot {
    param([string]$Archive, [string]$Source, [string]$Commit, [string]$TargetAddition, [string]$ReceiptDirectory)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $semanticSource = [IO.Path]::GetFullPath($Source).TrimEnd('\','/')
    $semanticPrefix = "mediapipe-$Commit/"
    $semanticTargetRelative = 'mediapipe/tasks/c/BUILD'
    $semanticOriginal = @{}
    $semanticRows = [Collections.Generic.List[object]]::new()
    $semanticArchive = [IO.Compression.ZipFile]::OpenRead($Archive)
    $semanticTargetText = $null
    $semanticOriginalTarget = $null
    try {
        foreach ($semanticEntry in $semanticArchive.Entries) {
            if ($semanticEntry.FullName.EndsWith('/')) { continue }
            if (-not $semanticEntry.FullName.StartsWith($semanticPrefix, [StringComparison]::Ordinal)) { throw 'Unexpected source archive root' }
            $semanticRelative = $semanticEntry.FullName.Substring($semanticPrefix.Length)
            $semanticFile = Assert-SemanticChildPath $semanticSource (Join-Path $semanticSource $semanticRelative)
            if (-not (Test-Path -LiteralPath $semanticFile -PathType Leaf)) { throw "Missing upstream source file: $semanticRelative" }
            $semanticStream = $semanticEntry.Open()
            try { $semanticArchiveHash = Get-SemanticStreamHash $semanticStream } finally { $semanticStream.Dispose() }
            $semanticExpectedHash = $semanticArchiveHash
            $semanticActualHash = (Get-FileHash -LiteralPath $semanticFile -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($semanticRelative -eq $semanticTargetRelative) {
                $semanticReader = [IO.StreamReader]::new($semanticEntry.Open())
                try { $semanticOriginalTarget = $semanticReader.ReadToEnd().Replace("`r`n","`n") } finally { $semanticReader.Dispose() }
                $semanticTargetText = $semanticOriginalTarget.TrimEnd("`n") + "`n" + $TargetAddition.Replace("`r`n","`n").TrimEnd("`n") + "`n"
                $semanticCurrentText = [IO.File]::ReadAllText($semanticFile).Replace("`r`n","`n")
                if ($semanticCurrentText -ne $semanticOriginalTarget -and $semanticCurrentText -ne $semanticTargetText) {
                    throw "Unregistered edit in minimal target file: $semanticRelative"
                }
                $semanticBytes = [Text.UTF8Encoding]::new($false).GetBytes($semanticTargetText)
                $semanticExpectedStream = [IO.MemoryStream]::new($semanticBytes)
                try { $semanticExpectedHash = Get-SemanticStreamHash $semanticExpectedStream } finally { $semanticExpectedStream.Dispose() }
            } elseif ($semanticActualHash -ne $semanticExpectedHash) {
                throw "Unregistered upstream source modification: $semanticRelative"
            }
            $semanticOriginal[$semanticRelative] = $true
            $semanticRows.Add([ordered]@{path=$semanticRelative; baseline_sha256=$semanticArchiveHash; expected_sha256=$semanticExpectedHash; registered_patch=($semanticRelative -eq $semanticTargetRelative)})
        }
    } finally { $semanticArchive.Dispose() }
    # Bazel creates these convenience links and the module resolution lock. They
    # are recorded separately and never treated as original upstream source.
    $semanticConvenienceLinks = @('bazel-bin','bazel-out','bazel-testlogs','bazel-src')
    $semanticGenerated = [Collections.Generic.List[object]]::new()
    $semanticDirectories = [Collections.Generic.Stack[string]]::new()
    $semanticDirectories.Push($semanticSource)
    while ($semanticDirectories.Count) {
        $semanticDirectory = $semanticDirectories.Pop()
        foreach ($semanticItem in Get-ChildItem -LiteralPath $semanticDirectory -Force) {
            $semanticRelative = $semanticItem.FullName.Substring($semanticSource.Length+1).Replace('\','/')
            if ($semanticItem.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                if ($semanticDirectory -eq $semanticSource -and $semanticConvenienceLinks -contains $semanticItem.Name) { continue }
                throw "Unregistered source reparse point: $semanticRelative"
            }
            if ($semanticItem.PSIsContainer) { $semanticDirectories.Push($semanticItem.FullName); continue }
            if ($semanticOriginal.ContainsKey($semanticRelative)) { continue }
            if ($semanticRelative -eq 'MODULE.bazel.lock') {
                $semanticGenerated.Add([ordered]@{path=$semanticRelative; sha256=(Get-FileHash -LiteralPath $semanticItem.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); purpose='Bazel generated dependency resolution lock'})
            } else { throw "Unregistered additional source file: $semanticRelative" }
        }
    }
    if (-not $semanticTargetText) { throw 'Missing registered C ABI build target in upstream archive' }
    # Only perform the declared deterministic edit after every other source file
    # has passed verification. Unknown changes are never silently overwritten.
    [IO.File]::WriteAllText((Join-Path $semanticSource $semanticTargetRelative), $semanticTargetText, [Text.UTF8Encoding]::new($false))
    $semanticOldLines = $semanticOriginalTarget.TrimEnd("`n").Split("`n").Count
    $semanticAddedLines = $TargetAddition.Replace("`r`n","`n").TrimEnd("`n").Split("`n")
    $semanticPatch = "--- a/$semanticTargetRelative`n+++ b/$semanticTargetRelative`n@@ -$semanticOldLines,0 +$($semanticOldLines+1),$($semanticAddedLines.Count) @@`n"
    $semanticPatch += (($semanticAddedLines | ForEach-Object { '+' + $_ }) -join "`n") + "`n"
    [IO.File]::WriteAllText((Join-Path $ReceiptDirectory 'source-minimum-c-api.patch'), $semanticPatch, [Text.UTF8Encoding]::new($false))
    [ordered]@{schema='orca.semantic-upstream-source-audit/v1'; source_commit=$Commit; source=$semanticSource; audited_utc=[DateTime]::UtcNow.ToString('o'); files=$semanticRows; generated_files=$semanticGenerated; unregistered_changes=0} |
        ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $ReceiptDirectory 'source-files.json') -Encoding UTF8
    Write-Host "Verified $($semanticRows.Count) upstream files and the registered C ABI target patch."
}

Export-ModuleMember -Function Assert-SemanticChildPath,Assert-SemanticSourceSnapshot
