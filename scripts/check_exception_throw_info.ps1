<#
.SYNOPSIS
    Fails when a Windows build has an exception class with broken throw info.

.DESCRIPTION
    Clang on the MSVC ABI can leave the copy constructor out of the CatchableType it emits for
    an exception class that comes through a precompiled header
    (https://github.com/llvm/llvm-project/issues/53486). The linker keeps one throw info per
    class, so when it keeps such a record, a copy of that exception (std::exception_ptr, a catch
    by value) shares its data with the original and the data is freed twice.

    A class is either trivially copyable or not, so a binary should never have both a CatchableType
    without a copy constructor (_CT??_R0?AV<class>@@@8<size>) and one with it
    (_CT??_R0?AV<class>@@@8??0<class>...). The script lists each PDB's public symbols with the
    llvm-pdbutil next to the build's compiler and reports every class that has both.

    scripts\test_check_exception_throw_info.ps1 tests it.

.PARAMETER BuildDir
    The slicer build directory. Its CMakeCache.txt names the compiler, and its src\<Config>
    holds the PDBs to check, which must include OrcaSlicer.pdb.

.PARAMETER Config
    The configuration whose PDBs to check (default: Release).

.PARAMETER Pdb
    PDBs to check instead of the ones in <BuildDir>\src\<Config>, as a list or comma-separated.

.PARAMETER Dump
    Saved `llvm-pdbutil dump --publics` output to check instead of PDBs. Needs no build.

.EXAMPLE
    scripts\check_exception_throw_info.ps1 -BuildDir build
#>
param(
    [string] $BuildDir,
    [string] $Config = 'Release',
    [string[]] $Pdb,
    [string[]] $Dump
)
$ErrorActionPreference = 'Stop'

# powershell -File passes "a.pdb,b.pdb" as one string. A path that exists as given stays whole.
function Split-PathList([string[]] $values) {
    @($values | Where-Object { $_ } | ForEach-Object {
        if ((Test-Path -LiteralPath $_) -or $_ -notmatch ',') { $_ } else { $_ -split ',' | Where-Object { $_ } }
    })
}

$Dump = Split-PathList $Dump
if (-not $Dump) {
    if (-not $BuildDir) {
        throw 'Name the slicer build directory with -BuildDir.'
    }
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path $cache)) {
        throw "No CMakeCache.txt in $BuildDir. Name the slicer build directory."
    }
    $entry = Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:\w+=(.+)$' | Select-Object -First 1
    if (-not $entry) {
        throw "No CMAKE_CXX_COMPILER in $cache."
    }
    $compiler = $entry.Matches[0].Groups[1].Value
    # A tree configured with a bare compiler name found it on PATH.
    if (-not (Split-Path $compiler)) {
        $found = Get-Command $compiler -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $found) {
            throw "$compiler, the compiler in $cache, is not on PATH."
        }
        $compiler = $found.Source
    }
    $pdbutil = Join-Path (Split-Path $compiler) 'llvm-pdbutil.exe'
    if (-not (Test-Path $pdbutil)) {
        throw "No llvm-pdbutil.exe next to $compiler. This check needs a build made with Clang."
    }

    $Pdb = Split-PathList $Pdb
    if (-not $Pdb) {
        # OrcaSlicer.dll holds the slicer itself, so a check without it proves nothing.
        $pdbDir = Join-Path $BuildDir "src\$Config"
        if (-not (Test-Path (Join-Path $pdbDir 'OrcaSlicer.pdb'))) {
            throw "No OrcaSlicer.pdb in $pdbDir."
        }
        $Pdb = @(Get-ChildItem $pdbDir -Filter *.pdb | ForEach-Object FullName)
    }
}

$catchable = [regex]'`_CT\?\?_R0\?A[UV](.+?)@@@8([^`]*)`'
$tmp = [IO.Path]::GetTempFileName()
$failed = $false
$classes = 0
try {
    foreach ($file in $(if ($Dump) { $Dump } else { $Pdb })) {
        $text = $file
        if (-not $Dump) {
            $run = Start-Process -FilePath $pdbutil -ArgumentList 'dump', '--publics', "`"$file`"" -NoNewWindow -Wait -PassThru -RedirectStandardOutput $tmp
            if ($run.ExitCode -ne 0) {
                throw "llvm-pdbutil failed on $file."
            }
            $text = $tmp
        }

        $kinds = @{}
        foreach ($match in $catchable.Matches([IO.File]::ReadAllText((Resolve-Path -LiteralPath $text).Path))) {
            $class = $match.Groups[1].Value
            if (-not $kinds.ContainsKey($class)) { $kinds[$class] = @{} }
            $kinds[$class][$(if ($match.Groups[2].Value.StartsWith('??0')) { 'copy' } else { 'bitwise' })] = $true
        }

        $broken = @($kinds.Keys | Where-Object { $kinds[$_].Count -eq 2 } | Sort-Object)
        "$(Split-Path -Leaf $file): $($kinds.Count) exception classes, $($broken.Count) with broken throw info"
        foreach ($class in $broken) { "    $class" }
        if ($broken.Count -gt 0) { $failed = $true }
        $classes += $kinds.Count
    }
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}

# The slicer's PDBs always contain exception throw info, so finding none means the dump format or the PDBs changed.
if ($classes -eq 0) {
    'No exception classes found. Check that llvm-pdbutil still prints public symbols in backticks.'
    $failed = $true
}

exit [int]$failed
