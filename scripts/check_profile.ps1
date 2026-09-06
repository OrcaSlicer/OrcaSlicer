<#
.SYNOPSIS
    Local twin of the "Check profiles" CI job (.github/workflows/check_profiles.yml), for Windows.

.DESCRIPTION
    The Windows counterpart of scripts/check_profile.sh, and kept deliberately close to it.

    Runs the same five checks, in the same order, with the same validator flags, and with the
    same semantics: every check runs even after an earlier one fails (the workflow's
    continue-on-error), then the script exits non-zero once at the end.

        extra_json_check              scripts/orca_extra_profile_check.py
        validate_system               validator -p <profiles> -l <level>
        validate_slice                validator -p <profiles> -s -l <level>
        validate_filament_subtypes    validator -p <profiles> -l <level> -f
        validate_custom               validator against every released custom-preset fixture

    Everything that has to be downloaded - the profile validator and the custom-preset fixture
    archives - lands under <repo>\.test\check_profiles and is reused on the next run. That
    directory also holds one log per check plus a copy of the comment CI would post on the PR.

    resources\profiles\user, which the validator creates as its data dir but a CI checkout never
    has, is moved aside for the duration of the run and restored on exit. Only one run per work
    dir at a time.

    x64 and ARM64 hosts are both supported. A locally built validator is chosen by the machine
    type in its PE header rather than by the name of its build tree, so build\ (x64) and
    build-arm64\ side by side resolve correctly; the published nightly is x64 only and runs
    under emulation on ARM64.

.PARAMETER ProfilesDir
    Profile tree to validate (default: resources\profiles). extra_json_check always looks at the
    tree next to the script, so this only redirects the validator checks.

.PARAMETER Validator
    OrcaSlicer_profile_validator.exe to use; also $env:ORCA_PROFILE_VALIDATOR. Default: the local
    build*\ Release build (then RelWithDebInfo, MinSizeRel, Debug) for this architecture, else
    the nightly release build is downloaded.

.PARAMETER Download
    Ignore local builds and use the downloaded nightly validator.

.PARAMETER Refresh
    Re-download the validator and fixtures instead of using the cache.

.PARAMETER WorkDir
    Downloads, logs and fixture trees (default: .test\check_profiles). Point it somewhere short,
    such as D:\t, if a fixture tree trips Windows' 260-character path limit.

.PARAMETER LogLevel
    Validator log level (default: 2, as in CI).

.PARAMETER Checks
    Checks to run, by name (default: all of them, in the order listed above).

.EXAMPLE
    scripts\check_profile.bat

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\check_profile.ps1 validate_system validate_slice
#>

# PositionalBinding is off so that the check names are the only positional arguments; left on,
# a bare "validate_system" would bind to whichever named parameter came next in this block.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Alias('p')] [string] $ProfilesDir,
    [string] $Validator,
    [string] $WorkDir,
    [Alias('l')] [int] $LogLevel = 2,
    [switch] $Download,
    [switch] $Refresh,
    [Alias('h')] [switch] $Help,
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] [string[]] $Checks
)

$ErrorActionPreference = 'Stop'

# Windows PowerShell 5.1 still negotiates TLS 1.0/1.1, which github.com refuses.
[Net.ServicePointManager]::SecurityProtocol =
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$ValidatorReleaseUrl = 'https://github.com/OrcaSlicer/OrcaSlicer/releases/download/nightly-builds'
$FixtureReleaseUrl = 'https://github.com/OrcaSlicer/OrcaSlicer-profile-validator/releases/download/fixture-archive'

$RepoRoot = Split-Path -Parent $PSScriptRoot

# PROCESSOR_ARCHITECTURE reports the architecture of the *shell*, so a 32-bit PowerShell on a
# 64-bit OS says x86; ARCHITEW6432 is the machine's own in that case.
$HostArch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
$HostArch = switch ($HostArch) {
    'ARM64' { 'arm64' }
    'AMD64' { 'x64' }
    default { 'x86' }
}

$AllChecks = @('extra_json_check', 'validate_system', 'validate_slice', 'validate_filament_subtypes', 'validate_custom')

$script:LogWriter = $null
$script:Python = ''

# ---------------------------------------------------------------------------- helpers

function Die([string] $Message) {
    Write-Host "check_profile.ps1: $Message" -ForegroundColor Red
    exit 2
}

# UTF-8 without a BOM, so a log reads the same here as the artifact CI uploads.
function New-LogWriter([string] $Path) {
    New-Object IO.StreamWriter($Path, $false, (New-Object Text.UTF8Encoding($false)))
}

# Output of the check being run: shown, and kept in that check's log.
function Write-CheckLog([string] $Text) {
    Write-Host $Text
    if ($script:LogWriter) { $script:LogWriter.WriteLine($Text) }
}

# Runs a program and returns its exit code, streaming stdout and stderr into the current check's
# log. -OutFile sends that output to a file of its own instead, silently.
function Invoke-Tool {
    param([string] $Exe, [string[]] $Arguments, [string] $OutFile)

    # Under 'Stop', 2>&1 turns every stderr line of a native command into a terminating error.
    # Assigning the preference here scopes it to this function, so it undoes itself on return.
    $ErrorActionPreference = 'Continue'

    $writer = if ($OutFile) { New-LogWriter $OutFile } else { $null }
    try {
        & $Exe @Arguments 2>&1 | ForEach-Object {
            if ($writer) { $writer.WriteLine("$_") } else { Write-CheckLog "$_" }
        }
        return $LASTEXITCODE
    } catch {
        if ($writer) { $writer.WriteLine("$_") } else { Write-CheckLog "$_" }
        return 1
    } finally {
        if ($writer) { $writer.Dispose() }
    }
}

# The first $Limit characters of a log, as CI truncates them for the PR comment.
function Get-LogHead([string] $Path, [int] $Limit) {
    $text = if (Test-Path -LiteralPath $Path) { [IO.File]::ReadAllText($Path) } else { '' }
    if (-not $text) { return 'No output captured' }
    if ($text.Length -gt $Limit) { return $text.Substring(0, $Limit) }
    return $text
}

# ---------------------------------------------------------------------------- arguments

if ($Help) { Get-Help $PSCommandPath -Detailed; exit 0 }

foreach ($name in $Checks) {
    if ($name -like '-*') { Die "unknown option '$name' (try -Help)" }
    if ($AllChecks -notcontains $name) { Die "unknown check '$name' (try -Help)" }
}
if (-not $Checks) { $Checks = $AllChecks }

if (-not $ProfilesDir) { $ProfilesDir = Join-Path $RepoRoot 'resources\profiles' }
if (-not (Test-Path -LiteralPath $ProfilesDir -PathType Container)) { Die "profile directory not found: $ProfilesDir" }
$ProfilesDir = (Resolve-Path -LiteralPath $ProfilesDir).Path

if (-not $WorkDir) { $WorkDir = Join-Path $RepoRoot '.test\check_profiles' }
$LogDir = Join-Path $WorkDir 'logs'
try { New-Item -ItemType Directory -Force -Path $LogDir | Out-Null } catch { Die "cannot create ${LogDir}: $_" }
$WorkDir = (Resolve-Path -LiteralPath $WorkDir).Path
$LogDir = Join-Path $WorkDir 'logs'

if (-not $Validator) { $Validator = $env:ORCA_PROFILE_VALIDATOR }

# ------------------------------------------------------------------- clean profile tree

# The validator points its data dir at the profile tree, so it creates - and, with -g, fills -
# <profiles>\user. A CI checkout never has that directory, and anything left in it from an
# earlier local run would be loaded as user presets and validated too. Move it aside for the
# duration of the run so what gets checked is what CI checks.
$script:StashedUserDir = ''

function Push-UserPresets {
    $user = Join-Path $ProfilesDir 'user'
    if (-not (Test-Path -LiteralPath $user -PathType Container)) { return }
    $script:StashedUserDir = Join-Path $WorkDir "user-presets-$PID"
    Remove-Item -LiteralPath $script:StashedUserDir -Recurse -Force -ErrorAction SilentlyContinue
    try { Move-Item -LiteralPath $user -Destination $script:StashedUserDir }
    catch { $script:StashedUserDir = ''; Die "cannot move $user aside: $_" }
    Write-Host "moved $user aside for the run (restored on exit)"
}

function Pop-UserPresets {
    # The validator leaves an empty user\default\{filament,machine,process} skeleton behind.
    # Prune it directory by directory, never wholesale: one that holds a real file survives and
    # is reported instead of being deleted. Runs even when nothing was stashed, so a tree that
    # had no user\ before the run does not gain one.
    $user = Join-Path $ProfilesDir 'user'
    Remove-EmptyDirs $user
    if (-not $script:StashedUserDir) { return }
    if (Test-Path -LiteralPath $user) {
        Write-Host "$user is not empty; your presets stay in $($script:StashedUserDir)"
    } else {
        Move-Item -LiteralPath $script:StashedUserDir -Destination $user
    }
    $script:StashedUserDir = ''
}

function Remove-EmptyDirs([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return }
    Get-ChildItem -LiteralPath $Path -Recurse -Directory -Force |
        Sort-Object { $_.FullName.Length } -Descending |
        ForEach-Object {
            if (-not (Get-ChildItem -LiteralPath $_.FullName -Force)) {
                Remove-Item -LiteralPath $_.FullName -Force
            }
        }
    if (-not (Get-ChildItem -LiteralPath $Path -Force)) { Remove-Item -LiteralPath $Path -Force }
}

# ---------------------------------------------------------------------------- downloads

# Cached; -Refresh, or -Force, pulls a new copy. Release assets never change, so caching them is
# safe; the fixture manifest is the index that grows with every release, and CI re-reads it on
# every run.
function Save-Asset {
    param([string] $Url, [string] $Dest, [switch] $Force)

    if (-not $Refresh -and -not $Force -and (Test-Path -LiteralPath $Dest -PathType Leaf) -and
        (Get-Item -LiteralPath $Dest).Length -gt 0) {
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Dest) | Out-Null
    Write-Host "downloading $(Split-Path -Leaf $Dest) ..."

    # Invoke-WebRequest spends most of a large download repainting its progress bar.
    $ProgressPreference = 'SilentlyContinue'
    $part = "$Dest.part"
    for ($attempt = 1; ; $attempt++) {
        try {
            Invoke-WebRequest -Uri $Url -OutFile $part -UseBasicParsing
            break
        } catch {
            if ($attempt -ge 3) {
                Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
                throw "download failed: $Url"
            }
        }
    }
    Move-Item -LiteralPath $part -Destination $Dest -Force
}

function Get-Sha256([string] $Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

# Directories a built validator can sit in, best first: build_win.bat names its trees build,
# build-dbginfo, build-minsize and build-dbg, each optionally -clang and -arm64 suffixed, and
# CMake puts the binary in src\<config>. Each pattern is matched both directly under a build root
# and one level down (build\x64), for trees laid out the way the macOS ones are.
function Get-ValidatorSearchDirs {
    foreach ($config in 'Release', 'RelWithDebInfo', 'MinSizeRel', 'Debug') {
        Join-Path $RepoRoot "build*\src\$config"
        Join-Path $RepoRoot "build*\*\src\$config"
    }
    Join-Path $RepoRoot 'build*\src'
    Join-Path $RepoRoot 'build*\*\src'
}

# The machine type from the PE header, which is what actually decides whether an .exe can run
# here - the name of the build tree only says what it was meant to be.
function Get-ExeArch([string] $Path) {
    try {
        $stream = [IO.File]::OpenRead($Path)
        try {
            $reader = New-Object IO.BinaryReader($stream)
            $stream.Position = 0x3C                     # e_lfanew: offset of the PE header
            $stream.Position = $reader.ReadInt32()
            if ($reader.ReadUInt32() -ne 0x00004550) { return '' }   # "PE\0\0"
            switch ($reader.ReadUInt16()) {
                0x8664 { 'x64' }
                0xAA64 { 'arm64' }
                0x014C { 'x86' }
                default { '' }
            }
        } finally { $stream.Dispose() }
    } catch { '' }
}

# First locally built OrcaSlicer_profile_validator.exe, in the order above. An x86 build, and on
# ARM64 an x64 one, is kept only as a fallback: it runs, but emulated. An ARM64 build on an x64
# host does not run at all and is never offered.
function Find-LocalValidator {
    $emulated = ''
    foreach ($pattern in (Get-ValidatorSearchDirs)) {
        foreach ($dir in @(Resolve-Path -Path $pattern -ErrorAction SilentlyContinue)) {
            $candidate = Join-Path $dir.Path 'OrcaSlicer_profile_validator.exe'
            if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
            $arch = Get-ExeArch $candidate
            if (-not $arch -or $arch -eq $HostArch) { return $candidate }
            if (-not $emulated -and ($arch -eq 'x86' -or $HostArch -eq 'arm64')) { $emulated = $candidate }
        }
    }
    if ($emulated) { Write-Host "no $HostArch build found, falling back to $emulated (emulated)" }
    return $emulated
}

# The nightly release build, same one CI uses. Windows ships the bare .exe, x64 only.
function Save-NightlyValidator {
    if ($HostArch -ne 'x64') {
        Write-Host "the nightly Windows validator is x64; it runs here under emulation"
    }
    $exe = Join-Path $WorkDir 'validator\OrcaSlicer_profile_validator.exe'
    Save-Asset -Url "$ValidatorReleaseUrl/OrcaSlicer_profile_validator_Windows_nightly.exe" -Dest $exe
    return $exe
}

function Resolve-Validator {
    if ($Validator) {
        if (-not (Test-Path -LiteralPath $Validator -PathType Leaf)) { Die "validator not found: $Validator" }
        return (Resolve-Path -LiteralPath $Validator).Path
    }
    if (-not $Download) {
        $local = Find-LocalValidator
        if ($local) {
            Write-Host "using locally built validator: $local"
            return $local
        }
    }
    try { $downloaded = Save-NightlyValidator } catch { Die "could not obtain a profile validator: $_" }
    Write-Host "using downloaded validator: $downloaded"
    return $downloaded
}

# python3 is rarely on PATH on Windows: the py launcher is the reliable way in, and a bare
# `python` may be the Store stub, which prints an advert and exits non-zero. Probe each for the
# interpreter it actually resolves to, and use that.
function Resolve-Python {
    $ErrorActionPreference = 'Continue'
    if ($script:Python) { return $script:Python }
    foreach ($candidate in 'py -3', 'python', 'python3') {
        $words = $candidate -split ' '
        $exe = Get-Command $words[0] -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $exe) { continue }
        $leading = @($words | Select-Object -Skip 1)
        $found = & $exe.Source @leading -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0 -and $found) {
            $script:Python = "$found"
            return $script:Python
        }
    }
    Die 'no Python 3 found; install it (or the py launcher) and re-run'
}

# ---------------------------------------------------------------------------- checks

$CheckBodies = @{

    extra_json_check = {
        Invoke-Tool -Exe (Resolve-Python) -Arguments @((Join-Path $RepoRoot 'scripts\orca_extra_profile_check.py'))
    }

    validate_system = {
        Invoke-Tool -Exe $Validator -Arguments @('-p', $ProfilesDir, '-l', "$LogLevel")
    }

    # Slices a two-colour cube through every printer so all custom g-code (incl.
    # change_filament_gcode) is expanded - catches undefined-placeholder / invalid-flow bugs the
    # static checks cannot see.
    validate_slice = {
        Invoke-Tool -Exe $Validator -Arguments @('-p', $ProfilesDir, '-s', '-l', "$LogLevel")
    }

    validate_filament_subtypes = {
        Invoke-Tool -Exe $Validator -Arguments @('-p', $ProfilesDir, '-l', "$LogLevel", '-f')
    }

    # Every released fixture is a snapshot of user presets saved by that OrcaSlicer version; each
    # is unpacked over the current system profiles and validated, so a profile change that would
    # break an existing user's presets fails here.
    validate_custom = {
        $fixturesDir = Join-Path $WorkDir 'profile-fixtures'
        $outputDir = Join-Path $WorkDir 'custom-preset-validation'
        New-Item -ItemType Directory -Force -Path $fixturesDir, $outputDir | Out-Null

        $manifestPath = Join-Path $fixturesDir 'manifest.json'
        Save-Asset -Url "$FixtureReleaseUrl/manifest.json" -Dest $manifestPath -Force

        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $entries = if ($manifest -is [array]) { $manifest } else { $manifest.fixtures }
        $fixtures = @($entries | Where-Object { $_.version -and $_.asset })
        if (-not $fixtures) {
            Write-CheckLog "No custom preset fixtures found in $FixtureReleaseUrl/manifest.json"
            return 1
        }

        $status = 0
        $failedLogs = @()
        $summary = @('## Custom Preset Fixture Validation', '', '| Version | Status | Log |', '| --- | --- | --- |')

        foreach ($fixture in $fixtures) {
            $version = $fixture.version
            $asset = $fixture.asset
            $fixtureZip = Join-Path $fixturesDir $asset
            $profileTree = Join-Path $outputDir "profiles-$version"
            $logPath = Join-Path $outputDir "$version.log"
            $assetUrl = "$FixtureReleaseUrl/$([uri]::EscapeDataString($asset))"

            Save-Asset -Url $assetUrl -Dest $fixtureZip

            $expected = $fixture.asset_sha256
            if ($expected -and $expected -ne '<sha256>' -and (Get-Sha256 $fixtureZip) -ne $expected.ToUpperInvariant()) {
                # A cached zip can be stale or truncated; the release asset itself is immutable,
                # so deleting it forces Save-Asset to pull a fresh copy.
                Write-Host "checksum mismatch for $asset, re-downloading"
                Remove-Item -LiteralPath $fixtureZip -Force
                Save-Asset -Url $assetUrl -Dest $fixtureZip
                $actual = Get-Sha256 $fixtureZip
                if ($actual -ne $expected.ToUpperInvariant()) {
                    Write-CheckLog "${asset}: expected $expected, got $actual"
                    return 1
                }
            }

            Write-Host "validating custom presets from $version ..."
            Remove-Item -LiteralPath $profileTree -Recurse -Force -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Force -Path $profileTree | Out-Null
            # Piped rather than copied through <profiles>\*, so a vendor directory whose name
            # holds a wildcard character is still copied by its literal path.
            Get-ChildItem -LiteralPath $ProfilesDir -Force | Copy-Item -Destination $profileTree -Recurse -Force
            Remove-Item -LiteralPath (Join-Path $profileTree 'user') -Recurse -Force -ErrorAction SilentlyContinue
            Expand-Archive -LiteralPath $fixtureZip -DestinationPath $profileTree -Force

            $result = Invoke-Tool -Exe $Validator -Arguments @('-p', $profileTree, '-l', "$LogLevel") -OutFile $logPath
            if ($result -eq 0) {
                $summary += "| $version | PASS | $version.log |"
                # Only failures are worth keeping; each tree is a full copy of resources\profiles.
                Remove-Item -LiteralPath $profileTree -Recurse -Force
            } else {
                $summary += "| $version | FAIL | $version.log |"
                $failedLogs += $logPath
                $status = 1
            }
        }

        [IO.File]::WriteAllLines((Join-Path $outputDir 'summary.md'), [string[]] $summary)
        $summary | ForEach-Object { Write-CheckLog $_ }

        if ($failedLogs) {
            Write-CheckLog ''
            Write-CheckLog '## Failed Fixture Logs'
            foreach ($logPath in $failedLogs) {
                Write-CheckLog ''
                Write-CheckLog "### $([IO.Path]::GetFileNameWithoutExtension($logPath))"
                Write-CheckLog '```'
                Write-CheckLog (Get-LogHead $logPath 12000)
                Write-CheckLog '```'
            }
        }
        return $status
    }
}

# Heading CI puts above this check's log in the PR comment.
$CommentHeadings = @{
    extra_json_check           = '### Extra JSON Check Failed'
    validate_system            = '### System Profile Validation Failed'
    validate_slice             = '### Slice Validation Failed (custom g-code expansion)'
    validate_filament_subtypes = '### Filament Subtype Validation Failed'
    validate_custom            = '### Custom Preset Validation Failed'
}

# ---------------------------------------------------------------------------- run

function Invoke-Check([string] $Name) {
    $log = Join-Path $LogDir "$Name.log"
    Write-Host ''
    Write-Host "==> $Name" -ForegroundColor Cyan

    $script:LogWriter = New-LogWriter $log
    try {
        $result = & $CheckBodies[$Name] | Select-Object -Last 1
    } catch {
        Write-CheckLog "$_"
        $result = 1
    } finally {
        $script:LogWriter.Dispose()
        $script:LogWriter = $null
    }

    if ([int] $result -eq 0) {
        Write-Host "    $Name passed" -ForegroundColor Green
        return $true
    }
    Write-Host "    $Name failed (exit $result)" -ForegroundColor Red
    return $false
}

# The fixture trees under the work dir are shared scratch space keyed by fixture version, so a
# second run would delete a tree the first one is validating.
$LockDir = Join-Path $WorkDir '.lock'
try { New-Item -ItemType Directory -Path $LockDir -ErrorAction Stop | Out-Null }
catch { Die "another run is using $WorkDir (pass -WorkDir, or remove $LockDir if no run is active)" }

# The validator writes UTF-8, but PowerShell decodes a child process's output using the console
# code page, which mangles the accented and CJK preset names in its messages.
$PreviousOutputEncoding = [Console]::OutputEncoding

try {
    [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
    Push-UserPresets

    if ($Checks | Where-Object { $_ -ne 'extra_json_check' }) { $Validator = Resolve-Validator }

    $results = [ordered] @{}
    foreach ($name in $AllChecks) {
        if ($Checks -contains $name) { $results[$name] = Invoke-Check $name }
    }

    Write-Host ''
    Write-Host '==> summary' -ForegroundColor Cyan
    foreach ($name in $results.Keys) {
        if ($results[$name]) {
            Write-Host "    PASS  $name" -ForegroundColor Green
        } else {
            Write-Host "    FAIL  $name  ($(Join-Path $LogDir "$name.log"))" -ForegroundColor Red
        }
    }

    $failed = @($results.Keys | Where-Object { -not $results[$_] })
    if (-not $failed) {
        Remove-Item -LiteralPath (Join-Path $WorkDir 'pr_comment.md') -Force -ErrorAction SilentlyContinue
        Write-Host ''
        Write-Host "All checks passed. Logs: $LogDir" -ForegroundColor Green
        exit 0
    }

    # The comment check_profiles_comment.yml would post when something fails.
    $comment = @(
        # Marker matched by check_profiles_comment.yml to delete prior comments.
        '<!-- profile-validation-comment -->'
        '## :x: Profile Validation Errors'
        ''
        foreach ($name in $failed) {
            $CommentHeadings[$name]
            ''
            '```'
            Get-LogHead (Join-Path $LogDir "$name.log") 30000
            '```'
            ''
        }
        '---'
        '*Please fix the above errors and push a new commit.*'
    )
    $commentPath = Join-Path $WorkDir 'pr_comment.md'
    [IO.File]::WriteAllLines($commentPath, [string[]] $comment)

    Write-Host ''
    Write-Host "One or more profile checks failed. Logs: $LogDir" -ForegroundColor Red
    Write-Host "The comment CI would post: $commentPath"
    exit 1
} finally {
    Pop-UserPresets
    Remove-Item -LiteralPath $LockDir -Force -ErrorAction SilentlyContinue
    [Console]::OutputEncoding = $PreviousOutputEncoding
}
