<#
.SYNOPSIS
    Tests scripts/check_exception_throw_info.ps1 on canned llvm-pdbutil dumps.

.DESCRIPTION
    Each case runs the check in its own powershell -File process, as CI does, and asserts on the
    exit code and output. Dumps are written to a temporary folder, so no build is needed.

.EXAMPLE
    powershell -File scripts/test_check_exception_throw_info.ps1
#>
$ErrorActionPreference = 'Stop'

$check = Join-Path $PSScriptRoot 'check_exception_throw_info.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) 'check_exception_throw_info_tests'
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Write-Dump([string] $path, [string[]] $symbols) {
    New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
    $i = 0
    $lines = @('                       Public Symbols', '============================================================')
    foreach ($symbol in $symbols) {
        $i++
        $lines += "  $i | S_PUB32 [size = 48] ``$symbol``", '           flags = none, addr = 0007:0000'
    }
    Set-Content -Path $path -Encoding ascii -Value $lines
}

$bitwise = '_CT??_R0?AVfuture_error@std@@@840'
$copy = '_CT??_R0?AVfuture_error@std@@@8??0future_error@std@@QEAA@AEBV01@@Z40'
$trivial = '_CT??_R0?AUtag@@@81'
Write-Dump "$work\broken.txt" @($bitwise, $copy, $trivial)
Write-Dump "$work\clean.txt" @($copy, $trivial)
Write-Dump "$work\empty.txt" @()
Write-Dump "$work\a dir, with a comma\clean.txt" @($copy)

# A build directory whose compiler has an llvm-pdbutil beside it but no OrcaSlicer.pdb.
# Nothing runs the stand-in; the check stops before it gets there.
$build = "$work\build"
New-Item -ItemType Directory -Force -Path "$build\bin", "$build\src\Release" | Out-Null
Copy-Item "$env:SystemRoot\System32\where.exe" "$build\bin\llvm-pdbutil.exe"
Set-Content -Path "$build\CMakeCache.txt" -Encoding ascii -Value "CMAKE_CXX_COMPILER:FILEPATH=$build\bin\clang-cl.exe"
Set-Content -Path "$build\src\Release\orca-slicer.pdb" -Value ''
$bare = "$work\bare"
New-Item -ItemType Directory -Force -Path $bare | Out-Null
Set-Content -Path "$bare\CMakeCache.txt" -Encoding ascii -Value 'CMAKE_CXX_COMPILER:STRING=no-such-clang-cl.exe'

$cases = @(
    @{ Name = 'a class with and without a copy constructor fails'; Args = @('-Dump', "$work\broken.txt")
       Exit = 1; Contains = @('1 with broken throw info', 'future_error@std') }
    @{ Name = 'copy constructors and trivially copyable classes pass'; Args = @('-Dump', "$work\clean.txt")
       Exit = 0; Contains = @('2 exception classes, 0 with broken throw info') }
    @{ Name = 'no exception classes at all fails'; Args = @('-Dump', "$work\empty.txt")
       Exit = 1; Contains = @('No exception classes found') }
    @{ Name = 'a comma-separated list checks each dump'; Args = @('-Dump', "$work\broken.txt,$work\clean.txt")
       Exit = 1; Contains = @('broken.txt: 2 exception classes', 'clean.txt: 2 exception classes') }
    @{ Name = 'a path with a comma stays whole'; Args = @('-Dump', "$work\a dir, with a comma\clean.txt")
       Exit = 0; Contains = @('clean.txt: 1 exception classes') }
    @{ Name = 'a build without OrcaSlicer.pdb fails'; Args = @('-BuildDir', $build)
       Exit = 1; Contains = @('No OrcaSlicer.pdb in') }
    @{ Name = 'a compiler that is not on PATH is named'; Args = @('-BuildDir', $bare)
       Exit = 1; Contains = @('no-such-clang-cl.exe, the compiler in') }
    @{ Name = 'a missing build directory is named'; Args = @('-BuildDir', "$work\nowhere")
       Exit = 1; Contains = @('No CMakeCache.txt in') }
    @{ Name = 'no build directory and no dump fails'; Args = @()
       Exit = 1; Contains = @('Name the slicer build directory') }
)

$failed = 0
foreach ($case in $cases) {
    $ErrorActionPreference = 'Continue'
    $output = (& powershell -NoProfile -File $check @($case.Args) 2>&1 | Out-String)
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $problems = @()
    if ($code -ne $case.Exit) { $problems += "exit $code, expected $($case.Exit)" }
    foreach ($text in $case.Contains) {
        if (-not $output.Contains($text)) { $problems += "no '$text' in the output" }
    }
    if ($problems) {
        $failed++
        "  FAIL $($case.Name)"
        $problems | ForEach-Object { "       $_" }
        $output.TrimEnd() -split "`r?`n" | ForEach-Object { "       | $_" }
    } else {
        "  ok   $($case.Name)"
    }
}

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
"$($cases.Count - $failed) passed, $failed failed"
exit [int]($failed -gt 0)
