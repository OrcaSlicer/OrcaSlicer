# Unit: Dependency management
# Verifies dependency source integrity via manifest hash and builds deps when needed.

function Get-DepsManifestHash {
    # Hash the dep source directories so we detect upstream dependency changes
    # instead of relying on a stale prebuilt tree. Hashing relative path + content
    # (not mtimes) keeps the manifest reproducible across checkouts/copies.
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $manifestBuilder = New-Object System.Text.StringBuilder
    $depSrcs = Get-ChildItem "$RepoRoot\deps_src" -Directory | Sort-Object Name
    foreach ($dep in $depSrcs) {
        $files = Get-ChildItem $dep.FullName -Recurse -File | Sort-Object FullName
        foreach ($file in $files) {
            $relative = $file.FullName.Substring($RepoRoot.Length).TrimStart('\', '/')
            $manifestBuilder.AppendLine($relative.Replace('\', '/')) > $null
            $fileHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
            $manifestBuilder.AppendLine($fileHash) > $null
        }
    }
    $manifestBytes = [System.Text.Encoding]::UTF8.GetBytes($manifestBuilder.ToString())
    return [BitConverter]::ToString($sha.ComputeHash($manifestBytes)).Replace("-", "").ToLower()
}

function Check-Dependencies {
    param([string]$Cmake, [string]$Compiler, [string]$Config, [switch]$ForceRebuild, [string]$DepsBuildDir)
    if (-not $DepsBuildDir) { $DepsBuildDir = "deps/build-clang" }
    $depsPrefix   = "$DepsBuildDir/OrcaSlicer_dep/usr/local"
    # Keep the manifest inside OrcaSlicer_dep so it travels with the CI cache and
    # deps artifact, which archive only that directory.
    $depsManifest = "$DepsBuildDir/OrcaSlicer_dep/.deps-manifest"

    $currentHash = Get-DepsManifestHash
    $storedHash = $null
    if (Test-Path $depsManifest) { $storedHash = Get-Content $depsManifest -TotalCount 1 }

    $needsRebuild = $ForceRebuild -or -not (Test-Path $depsPrefix -PathType Container)
    if (-not $needsRebuild -and -not $storedHash) {
        # CI caches only deps/build/OrcaSlicer_dep, so a cache-restored tree arrives
        # without the manifest that used to live in its parent. Treating that as
        # "never completed" rebuilt every dependency from source on every CI job -
        # hours of work that build_win.bat skips (it trusts any existing usr/local),
        # which both stalled the self-hosted runner and skewed the .bat/.ps1
        # comparison. Adopt the tree and record its hash; only a missing prefix
        # counts as incomplete.
        $currentHash | Set-Content $depsManifest -Encoding ASCII
        $storedHash = $currentHash
    }
    if (-not $needsRebuild -and $storedHash -and $currentHash -ne $storedHash) {
        $needsRebuild = $true
        Write-Host "  [deps] Dependency source changed since last build; rebuilding..."
    }

    if ($needsRebuild) {
        $depsConfigureArgs = @("-S", "deps", "-B", $DepsBuildDir, "-G", "Ninja Multi-Config")
        if ($Compiler) {
            # Pass as separate array elements so paths with spaces (e.g.
            # "C:\Program Files\LLVM\bin\clang-cl.exe") survive intact.
            $depsConfigureArgs += "-DCMAKE_C_COMPILER=$Compiler"
            $depsConfigureArgs += "-DCMAKE_CXX_COMPILER=$Compiler"
            # Ensure MASM assembler is findable for Boost.build. cmake's
            # auto-detection can find a stale/wrong ml64.exe path; pass the
            # one from the active VS environment explicitly.
            $ml64 = Get-Command ml64.exe -ErrorAction SilentlyContinue
            if ($ml64) {
                $depsConfigureArgs += "-DCMAKE_ASM_MASM_COMPILER=$($ml64.Source)"
            }
        }
        Write-Host "  [deps] Building dependencies..."
        Invoke-BuildStep -Label "Dependency configuration" -Command $Cmake -Arguments $depsConfigureArgs -LogPath (New-BuildLogPath "deps-configure")
        Invoke-BuildStep -Label "Dependency build" -Command $Cmake -Arguments @(
            "--build", $DepsBuildDir, "--config", $Config, "--target", "deps", "--", "--verbose") -LogPath (New-BuildLogPath "deps-build")
        $currentHash | Set-Content $depsManifest -Encoding ASCII
        Write-Host "  [deps] Dependencies built successfully" -ForegroundColor Green
    } else {
        if (-not (Test-Path $depsManifest)) {
            $currentHash | Set-Content $depsManifest -Encoding ASCII
        }
        # Check if Boost is available system-wide before using deps
        $boostLibPath = "${depsPrefix}/lib/libboost*.lib"
        if (-not (Get-ChildItem $boostLibPath -ErrorAction SilentlyContinue)) {
            Write-Host "  [deps] WARNING: No Boost libraries found in deps. This may indicate a build" -ForegroundColor Yellow
            Write-Host "         failure or partial installation. OrcaSlicer requires Boost to build." -ForegroundColor Yellow
            Write-Host "         Try rebuilding dependencies or install Boost system-wide." -ForegroundColor Yellow
        }
        Write-Host "  [deps] Found at $depsPrefix (manifest matches)" -ForegroundColor DarkGray
    }
}

function Pack-Dependencies {
    param([string]$DepsBuildDir, [string]$OutputPath)
    $depsPrefix = "$DepsBuildDir/OrcaSlicer_dep"
    if (-not (Test-Path $depsPrefix -PathType Container)) {
        throw "Dependencies not found at $depsPrefix. Build them first."
    }

    $date = Get-Date -Format "yyyyMMdd"
    $arch = "x64"
    if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { $arch = "arm64" }

    # Honor the caller's requested path when provided; otherwise derive a dated
    # name in the same directory.
    $zipPath = if ($OutputPath) { $OutputPath }
               else {
                   $zipName = "OrcaSlicer_dep_win-${arch}_${date}.zip"
                   Join-Path $RepoRoot (Join-Path "deps" $zipName)
               }

    # Detect an archiver. Use an array so multi-token invocations work: a single
    # string with embedded spaces would be treated as one command name.
    $zipper = $null
    if (Test-Path "$env:SystemRoot\System32\tar.exe") {
        $zipper = @("$env:SystemRoot\System32\tar.exe", "-a", "-c", "-f")
    } elseif (Test-Path "$RepoRoot\tools\7z.exe") {
        $zipper = @("$RepoRoot\tools\7z.exe", "a")
    }

    if (-not $zipper) {
        throw "No archiver found. Install 7-Zip or ensure Windows has System32\tar.exe."
    }

    Write-Host "  [deps] Packing to $zipPath..." -ForegroundColor Yellow
    Push-Location $DepsBuildDir
    try {
        $cmd = $zipper[0]
        $args = @($zipper[1..($zipper.Count - 1)])
        & $cmd @args $zipPath OrcaSlicer_dep
    } finally {
        Pop-Location
    }

    if (-not (Test-Path $zipPath)) {
        throw "Failed to create dependency archive: $zipPath"
    }

    Write-Host "  [deps] Packed: $zipPath" -ForegroundColor Green
    return $zipPath
}