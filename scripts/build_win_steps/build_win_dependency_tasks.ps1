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

function Test-DepsTree {
    param([string]$DepsPrefix, [string]$DepsManifest, [switch]$ForceRebuild)

    $currentHash = Get-DepsManifestHash
    $storedHash = if (Test-Path $DepsManifest) { Get-Content $DepsManifest -TotalCount 1 } else { $null }

    $needsRebuild = $ForceRebuild -or -not (Test-Path $DepsPrefix -PathType Container)
    if (-not $needsRebuild -and -not $storedHash) {
        # CI caches only deps/build/OrcaSlicer_dep, so a cache-restored tree arrives
        # without the manifest that used to live in its parent. Treating that as
        # "never completed" rebuilt every dependency from source on every CI job -
        # hours of work that stalled the self-hosted runner. Adopt the tree and
        # record its hash; only a missing prefix counts as incomplete.
        $currentHash | Set-Content $DepsManifest -Encoding ASCII
        $storedHash = $currentHash
    }
    if (-not $needsRebuild -and $storedHash -and $currentHash -ne $storedHash) {
        $needsRebuild = $true
        Write-Host "  [deps] Dependency source changed since last build; rebuilding..."
    }

    return [pscustomobject]@{ Hash = $currentHash; NeedsRebuild = [bool]$needsRebuild }
}

function Invoke-DepsBuild {
    param([string]$Cmake, [string]$Compiler, [string]$Config, [string]$DepsBuildDir, [int]$MaxJobs)

    # The deps CMakeLists still declare a policy version below 3.5.
    $env:CMAKE_POLICY_VERSION_MINIMUM = "3.5"
    # Ninja and Make honour this for the deps build.
    if ($MaxJobs -gt 0) {
        $env:CMAKE_BUILD_PARALLEL_LEVEL = $MaxJobs.ToString()
    }

    $configureArgs = @("-S", "deps", "-B", $DepsBuildDir, "-G", "Ninja Multi-Config")
    if ($Compiler) {
        # Pass as separate array elements so paths with spaces (e.g.
        # "C:\Program Files\LLVM\bin\clang-cl.exe") survive intact.
        $configureArgs += "-DCMAKE_C_COMPILER=$Compiler"
        $configureArgs += "-DCMAKE_CXX_COMPILER=$Compiler"
        # Do NOT explicitly pass MASM compiler; let cmake auto-detect from VS env
    }
    Write-Host "  [deps] Building dependencies..."
    Invoke-BuildStep -Label "Dependency configuration" -Command $Cmake -Arguments $configureArgs -LogPath (New-BuildLogPath "deps-configure")

    $buildArgs = @("--build", $DepsBuildDir, "--config", $Config, "--target", "deps")
    if ($MaxJobs -gt 0) {
        $buildArgs += "--parallel"
        $buildArgs += $MaxJobs.ToString()
    }
    $buildArgs += "--verbose"
    Invoke-BuildStep -Label "Dependency build" -Command $Cmake -Arguments $buildArgs -LogPath (New-BuildLogPath "deps-build")
}

function Check-Dependencies {
    param([string]$Cmake, [string]$Compiler, [string]$Config, [switch]$ForceRebuild, [string]$DepsBuildDir, [int]$MaxJobs = 0)
    if (-not $DepsBuildDir) { $DepsBuildDir = "deps/build" }
    $depsPrefix = "$DepsBuildDir/OrcaSlicer_dep/usr/local"
    # Keep the manifest inside OrcaSlicer_dep so it travels with the CI cache and
    # deps artifact, which archive only that directory.
    $depsManifest = "$DepsBuildDir/OrcaSlicer_dep/.deps-manifest"

    $state = Test-DepsTree -DepsPrefix $depsPrefix -DepsManifest $depsManifest -ForceRebuild:$ForceRebuild
    if ($state.NeedsRebuild) {
        Invoke-DepsBuild -Cmake $Cmake -Compiler $Compiler -Config $Config -DepsBuildDir $DepsBuildDir -MaxJobs $MaxJobs
        $state.Hash | Set-Content $depsManifest -Encoding ASCII
        Write-Host "  [deps] Dependencies built successfully" -ForegroundColor Green
        return
    }

    if (-not (Test-Path $depsManifest)) {
        $state.Hash | Set-Content $depsManifest -Encoding ASCII
    }
    # A deps tree without Boost usually means a failed or partial install, and
    # the error at configure time does not say so.
    $boostLibPath = "${depsPrefix}/lib/libboost*.lib"
    if (-not (Get-ChildItem $boostLibPath -ErrorAction SilentlyContinue)) {
        Write-Host "  [deps] WARNING: No Boost libraries found in deps. This may indicate a build" -ForegroundColor Yellow
        Write-Host "         failure or partial installation. OrcaSlicer requires Boost to build." -ForegroundColor Yellow
        Write-Host "         Try rebuilding dependencies or install Boost system-wide." -ForegroundColor Yellow
    }
    Write-Host "  [deps] Found at $depsPrefix (manifest matches)" -ForegroundColor DarkGray
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
