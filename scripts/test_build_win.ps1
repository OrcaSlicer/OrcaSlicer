<#
.SYNOPSIS
    Tests build_win.bat's option handling and the commands it generates.

.DESCRIPTION
    Cases run the script with --dry-run, so nothing is configured, built or
    deleted and the suite finishes in seconds. Each case asserts on the exit
    code and on the command lines the script echoes.

    Adding a case means adding one row to $cases. A bare string starts a new
    group. Defaults: ExpectExit is 0 and --dry-run is appended, so a row only
    states what is unusual about it.

        Name        what the case proves, in words
        Args        arguments, as an array
        ExpectExit  expected exit code            (default 0)
        DryRun      append --dry-run              (default $true)
        First       regex the first output line must match
        Env         environment for this case only
        Contains    literal strings the output must have
        NotContains literal strings it must not have
        Match       regexes; each must match at least one output line
        NotMatch    regexes; none may match any output line
        NotExists   paths that must not exist after the case runs

.PARAMETER Name
    Run only the cases whose name matches this regex. Headings with no
    matching case are not printed, and a pattern that matches nothing is a
    failure rather than an empty pass.

.EXAMPLE
    powershell -File scripts/test_build_win.ps1

.EXAMPLE
    powershell -File scripts/test_build_win.ps1 -Name solution
#>
[CmdletBinding()]
param(
    [string] $Script,
    [string] $Name
)

$ErrorActionPreference = 'Stop'

if (-not $Script) {
    $here = $PSScriptRoot
    if (-not $here) { $here = Split-Path -Parent $MyInvocation.MyCommand.Path }
    $Script = Join-Path (Split-Path -Parent $here) 'build_win.bat'
}
if (-not (Test-Path $Script)) { throw "build_win.bat not found at $Script" }

# cmd resumes a batch file by byte offset after `call :label`, and with LF
# endings that offset lands wrong and the label lookup fails. .gitattributes
# pins CRLF; this catches a checkout or an editor that did not honour it.
if ((Get-Content -Raw $Script) -match "(?<!`r)`n") {
    throw "$Script has LF line endings; cmd needs CRLF to resume after call :label"
}
$Script = (Resolve-Path $Script).Path

# Read the long options out of the script itself, so this cannot go stale when
# an option is added. Field order is: add_arg <var> <type> <short> <long>.
$longFlags = @(
    Select-String -Path $Script -Pattern '^call :add_arg \S+ \S+ \S+ (\S+) ' |
        ForEach-Object { '--' + $_.Matches[0].Groups[1].Value }
)
if ($longFlags.Count -lt 20) { throw "only found $($longFlags.Count) options in $Script; the parser above is wrong" }

# A winget that always fails, so the prerequisite failure path runs without
# touching the machine. It has to be an .exe: a .bat invoked without `call`
# transfers control and never comes back, which would end the script instead.
# where.exe returns 1 when its patterns match nothing and never prompts.
$fixtures = Join-Path ([IO.Path]::GetTempPath()) 'build_win_test_fixtures'
New-Item -ItemType Directory -Force -Path $fixtures | Out-Null
Copy-Item "$env:SystemRoot\System32\where.exe" (Join-Path $fixtures 'winget.exe') -Force
$stubPath = "$fixtures;C:\Windows\system32;C:\Windows"

# Stand-in ninjas that only report a version, so the 1.12 boundary in the
# progress format can be exercised on a machine whose real ninja is newer.
# A dry run skips the dev shell, so PATH here is what the script sees.
$ninjaPaths = @{}
foreach ($v in @{ old = '1.11.1'; new = '1.12.0' }.GetEnumerator()) {
    $d = Join-Path $fixtures "ninja-$($v.Key)"
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    Set-Content -Path (Join-Path $d 'ninja.bat') -Encoding ascii -Value @('@echo off', "echo $($v.Value)")
    $ninjaPaths[$v.Key] = "$d;$env:PATH"
}

# The pack stamp is checked against real dates, so a locale-dependent parse
# in the script cannot pass by looking date-shaped. Yesterday is accepted too,
# so a run that crosses midnight does not flake.
$dateStamps = @((Get-Date -Format 'yyyyMMdd'), (Get-Date).AddDays(-1).ToString('yyyyMMdd'))
$stampPattern = '_(' + ($dateStamps -join '|') + ')\.zip$'

$cases = @(
    'argument handling'
    @{ Name = 'no arguments prints help'; Args = @(); DryRun = $false
       Contains = @('Usage: build_win.bat [options]', '--clang-cl') }
    @{ Name = "--help lists all $($longFlags.Count) options the script defines"; Args = @('--help'); DryRun = $false
       Contains = $longFlags }
    @{ Name = 'help is grouped and shows usage, examples and environment'; Args = @('--help'); DryRun = $false
       Contains = @('Usage: build_win.bat [options]', 'Actions:', 'Build configuration:', 'Toolchain:',
                    'How much gets rebuilt:', 'Paths and extra arguments:', 'Diagnostics:',
                    'Examples:', 'Environment:')  }
    @{ Name = 'the environment section shows what to set'; Args = @('--help'); DryRun = $false
       Contains = @('ORCA_DEPS_CMAKE_ARGS', 'ORCA_SLICER_CMAKE_ARGS', 'ORCA_UPDATER_SIG_KEY', 'NINJA_STATUS',
                    'set ORCA_SLICER_CMAKE_ARGS=-DSLIC3R_PCH=OFF', '(PowerShell)', 'debugscript') }
    @{ Name = 'section headers do not widen the flag column'; Args = @('--help'); DryRun = $false
       Match = @('^   -d, --deps  +Download') }
    # Windows Terminal opens at 120 columns and wraps at 120, so 119 is the
    # limit. Anyone still on the old conhost gets 80 and will see wrapping.
    @{ Name = 'every help line fits a 120 column console'; Args = @('--help'); DryRun = $false
       NotMatch = @('^.{120,}$') }
    @{ Name = 'an unknown long option is rejected'; Args = @('--nonsense'); ExpectExit = 1
       Contains = @('Failed to find arg') }
    @{ Name = 'an unknown short option is rejected'; Args = @('-Z'); ExpectExit = 1
       Contains = @('Failed to find arg') }
    @{ Name = 'a bare argument is rejected'; Args = @('deps'); ExpectExit = 1
       Contains = @('Unknown argument') }
    @{ Name = 'an unknown architecture is rejected'; Args = @('-d', '--arch', 'sparc'); ExpectExit = 1
       Contains = @('Unknown architecture') }
    @{ Name = 'a string option without a value is rejected'; Args = @('-d', '--arch'); ExpectExit = 1
       Contains = @('requires a value') }
    @{ Name = 'short options can be bundled'; Args = @('-dx')
       Contains = @('-G "Ninja Multi-Config"', '--target deps') }

    'generator and compiler selection'
    @{ Name = 'deps default to the Visual Studio generator'; Args = @('-d')
       Contains = @('-G "Visual Studio', '-A x64', '--target deps')
       NotContains = @('Ninja', 'clang-cl') }
    @{ Name = '-x selects Ninja without changing compiler'; Args = @('-d', '-x')
       Contains = @('-G "Ninja Multi-Config"')
       NotContains = @('clang-cl', '-A x64') }
    @{ Name = '-l -x builds with clang-cl under Ninja'; Args = @('-d', '-l', '-x')
       Contains = @('-G "Ninja Multi-Config"', '-DCMAKE_C_COMPILER=clang-cl.exe', '-DCMAKE_CXX_COMPILER=clang-cl.exe') }
    @{ Name = '-l alone uses the ClangCL toolset on the VS generator'; Args = @('-d', '-l')
       Contains = @('-G "Visual Studio', '-T ClangCL')
       NotContains = @('-DCMAKE_C_COMPILER') }
    # --msvc and --msbuild name the defaults, so a caller can be explicit and a
    # contradictory pair can be caught rather than silently resolved.
    @{ Name = '--msvc is the compiler default spelled out'; Args = @('-d', '--msvc')
       Contains = @('-G "Visual Studio', '-A x64')
       NotContains = @('clang-cl') }
    @{ Name = '--msbuild is the generator default spelled out'; Args = @('-d', '--msbuild')
       Contains = @('-G "Visual Studio')
       NotContains = @('Ninja') }
    @{ Name = '--msvc with -x gives Ninja driving cl'; Args = @('-d', '--msvc', '-x')
       Contains = @('-G "Ninja Multi-Config"')
       NotContains = @('clang-cl') }
    @{ Name = '--msbuild with -l gives the VS generator and the ClangCL toolset'; Args = @('-d', '--msbuild', '-l')
       Contains = @('-G "Visual Studio', '-T ClangCL') }
    # A developer with a standalone LLVM points at it; the VS-bundled clang
    # is what a bare clang-cl.exe resolves to after the dev shell runs.
    @{ Name = '--clang-path names the compiler, quoted for its spaces'; Args = @('-d', '-x', '--clang-path', 'C:\Program Files\LLVM\bin\clang-cl.exe')
       Contains = @('-DCMAKE_C_COMPILER="C:\Program Files\LLVM\bin\clang-cl.exe"',
                    '-DCMAKE_CXX_COMPILER="C:\Program Files\LLVM\bin\clang-cl.exe"') }
    @{ Name = '--clang-path is a clang request on its own'; Args = @('-d', '-x', '--clang-path', 'C:\Program Files\LLVM\bin\clang-cl.exe')
       Contains = @('deps/build-clang') }
    @{ Name = '--clang-path needs Ninja to take effect'; Args = @('-d', '--clang-path', 'C:\Program Files\LLVM\bin\clang-cl.exe'); ExpectExit = 1
       Contains = @('needs the Ninja generator') }
    # `exist` is true for a directory too, and a directory would reach CMake
    # as the compiler.
    @{ Name = '--clang-path must name the exe, not its folder'; Args = @('-d', '-x', '--clang-path', 'C:\Program Files\LLVM'); ExpectExit = 1
       Contains = @('is a directory') }
    @{ Name = 'a clang-cl that is not there is caught early'; Args = @('-d', '-x', '--clang-path', 'C:\nope\clang-cl.exe'); ExpectExit = 1
       Contains = @('No clang-cl at')
       NotContains = @('cmake -S deps') }
    @{ Name = '--clang-path contradicting --msvc is rejected'; Args = @('-d', '-x', '--msvc', '--clang-path', 'C:\Program Files\LLVM\bin\clang-cl.exe'); ExpectExit = 1
       Contains = @('select different compilers') }
    @{ Name = '--clang-cl and --msvc together are rejected'; Args = @('-d', '-l', '--msvc'); ExpectExit = 1
       Contains = @('select different compilers') }
    @{ Name = '--ninja and --msbuild together are rejected'; Args = @('-d', '-x', '--msbuild'); ExpectExit = 1
       Contains = @('select different generators') }
    # One option with a value, driven by a table, rather than a flag per
    # release. Adding a release should not need a new flag.
    @{ Name = '--vs 2019 pins that release and skips autodetect'; Args = @('-d', '--vs', '2019')
       Contains = @('-G "Visual Studio 16 2019"')
       NotContains = @('Detecting Visual Studio') }
    @{ Name = '--vs 2022 pins that release'; Args = @('-d', '--vs', '2022')
       Contains = @('-G "Visual Studio 17 2022"') }
    @{ Name = '--vs 2026 pins that release'; Args = @('-d', '--vs', '2026')
       Contains = @('-G "Visual Studio 18 2026"') }
    @{ Name = 'an unknown release is rejected and the known ones listed'; Args = @('-d', '--vs', '2015'); ExpectExit = 1
       Contains = @('Unknown Visual Studio release', '2019, 2022, 2026') }
    @{ Name = '--vs and --ninja together are rejected'; Args = @('-d', '--vs', '2022', '-x'); ExpectExit = 1
       Contains = @('select different generators') }
    @{ Name = 'without --vs the release is autodetected'; Args = @('-d')
       Contains = @('Detecting Visual Studio') }

    'architecture'
    @{ Name = 'x64 is the default'; Args = @('-d')
       Contains = @('Configuration: Release, x64')
       NotContains = @('build-arm64') }
    @{ Name = 'arm64 sets the generator platform and deps tree'; Args = @('-d', '--arch', 'arm64')
       Contains = @('-A ARM64', 'deps/build-arm64') }
    @{ Name = 'the architecture is matched case-insensitively'; Args = @('-d', '--arch', 'ARM64')
       Contains = @('-A ARM64', 'deps/build-arm64') }
    @{ Name = 'arm64 under Ninja has no -A but keeps the arm64 tree'; Args = @('-d', '--arch', 'arm64', '-x', '-l')
       Contains = @('deps/build-clang-arm64', '-DCMAKE_C_COMPILER=clang-cl.exe')
       NotContains = @('-A ') }

    'build configurations'
    # One option with a value, driven by a table, so Release is named rather
    # than being whatever is left when no flag is passed.
    @{ Name = 'release is the default'; Args = @('-s')
       Contains = @('-DCMAKE_BUILD_TYPE=Release', 'cmake -B "build" ') }
    @{ Name = '--config release is the default spelled out'; Args = @('-s', '--config', 'release')
       Contains = @('-DCMAKE_BUILD_TYPE=Release', 'cmake -B "build" ') }
    @{ Name = '--config debug builds into build-dbg'; Args = @('-s', '--config', 'debug')
       Contains = @('-DCMAKE_BUILD_TYPE=Debug', 'cmake -B "build-dbg" ') }
    @{ Name = '--config relwithdebinfo builds into build-dbginfo'; Args = @('-s', '--config', 'relwithdebinfo')
       Contains = @('-DCMAKE_BUILD_TYPE=RelWithDebInfo', 'cmake -B "build-dbginfo" ') }
    @{ Name = '--config minsizerel builds into build-minsize'; Args = @('-s', '--config', 'minsizerel')
       Contains = @('-DCMAKE_BUILD_TYPE=MinSizeRel', 'cmake -B "build-minsize" ') }
    # Batch variable names are case-insensitive, so the table lookup is too.
    @{ Name = 'the configuration name is matched case-insensitively'; Args = @('-s', '--config', 'RelWithDebInfo')
       Contains = @('-DCMAKE_BUILD_TYPE=RelWithDebInfo') }
    @{ Name = 'an unknown configuration is rejected and the known ones listed'; Args = @('-s', '--config', 'bogus'); ExpectExit = 1
       Contains = @('Unknown configuration', 'release, debug, relwithdebinfo, minsizerel') }
    # Release, RelWithDebInfo and MinSizeRel all link the /MD dependencies, so
    # one tree serves all three. Debug is /MDd and cannot share.
    @{ Name = 'minsizerel builds against the release deps'; Args = @('-d', '-s', '--config', 'minsizerel')
       Contains = @('cmake -S deps -B "deps/build"', 'cmake -B "build-minsize" ')
       NotContains = @('deps/build-minsize') }
    @{ Name = 'relwithdebinfo builds against them too'; Args = @('-d', '-s', '-l', '--config', 'relwithdebinfo')
       Contains = @('cmake -S deps -B "deps/build-clang"', 'cmake -B "build-dbginfo-clang" ')
       NotContains = @('deps/build-dbginfo') }
    @{ Name = 'debug keeps a dependency tree of its own'; Args = @('-d', '--config', 'debug')
       Contains = @('cmake -S deps -B "deps/build-dbg"') }
    # The build tree and the deps tree now have different names, so the
    # derivation from the binary directory name cannot work and it is named.
    @{ Name = 'a shared deps tree is named outright'; Args = @('-s', '-l', '--config', 'relwithdebinfo')
       Match = @('-DDEP_BUILD_DIR="[A-Za-z]:\\.*\\deps\\build-clang"') }
    @{ Name = 'configuration and arch combine into build-dbg-arm64'; Args = @('-s', '--config', 'debug', '--arch', 'arm64')
       Contains = @('cmake -B "build-dbg-arm64" ') }

    'what gets built'
    @{ Name = 'the deps target defaults to deps'; Args = @('-d')
       Contains = @('--target deps') }
    @{ Name = '-t overrides the deps target'; Args = @('-d', '-t', 'dep_Boost')
       Contains = @('--target dep_Boost') }
    @{ Name = 'unit tests are off unless asked for'; Args = @('-s')
       Contains = @('-DBUILD_TESTS=OFF') }
    @{ Name = '--tests turns the unit tests on'; Args = @('-s', '--tests')
       Contains = @('-DBUILD_TESTS=ON') }
    @{ Name = '-a enables ASAN for the slicer'; Args = @('-s', '-a')
       Contains = @('-DSLIC3R_ASAN=ON') }
    @{ Name = 'the slicer build runs gettext'; Args = @('-s')
       Contains = @('run_gettext.bat') }
    # tools\7z.exe needs a 7z.dll beside it, which the repo does not carry,
    # so the pack falls back to the bsdtar Windows ships. Either is correct;
    # what matters is that one is chosen and handed the right names.
    @{ Name = '-p packs the deps tree with whichever archiver is usable'; Args = @('-d', '-p')
       Match = @('^\+ .*(7z\.exe a|tar\.exe -a -c -f) OrcaSlicer_dep_win-\S+\.zip OrcaSlicer_dep$') }
    # The bundle is only good for what built it, so it carries the same three
    # axes as the tree. Release x64 on cl keeps the historical plain name.
    @{ Name = 'the bundle name carries compiler and deps flavour'; Args = @('-p', '-l', '--config', 'debug', '--arch', 'arm64')
       Contains = @('OrcaSlicer_dep_win-ARM64-clang-dbg_') }
    @{ Name = 'a relwithdebinfo pack is the release bundle'; Args = @('-p', '--config', 'relwithdebinfo')
       Contains = @('OrcaSlicer_dep_win-x64_')
       NotContains = @('-dbg', 'dbginfo') }
    @{ Name = 'a plain release bundle keeps its old name'; Args = @('-p')
       Contains = @('OrcaSlicer_dep_win-x64_')
       NotContains = @('-clang', '-Release') }
    @{ Name = 'the bundle is stamped with today, not a shuffled date'; Args = @('-p')
       Match = @($stampPattern) }
    # powershell.exe is not in System32 itself, so a trimmed PATH used to
    # leave the stamp empty and the bundle named OrcaSlicer_dep_win-x64_.zip.
    @{ Name = 'the bundle is stamped even with a bare PATH'; Args = @('-p')
       Env = @{ PATH = 'C:\Windows\system32;C:\Windows' }
       Match = @($stampPattern) }
    @{ Name = '-p packs without rebuilding'; Args = @('-p')
       Match = @('^\+ .*(7z\.exe a|tar\.exe -a -c -f) ')
       NotContains = @('cmake -S deps') }
    @{ Name = 'deps and slicer build in one invocation'; Args = @('-d', '-s', '-x', '-l')
       Contains = @('cmake -S deps', 'cmake -B "build-clang" ') }

    'the developer loop'
    @{ Name = '--slicer-target builds one target'; Args = @('-s', '--slicer-target', 'libslic3r')
       Contains = @('--config Release --target libslic3r') }
    @{ Name = '--no-configure skips the slicer configure'; Args = @('-s', '--no-configure')
       Contains = @('cmake --build "build"')
       NotContains = @('cmake -B "build" ') }
    @{ Name = '--no-configure skips the deps configure'; Args = @('-d', '--no-configure')
       Contains = @('cmake --build "deps/build"')
       NotContains = @('cmake -S deps') }
    @{ Name = '--no-gettext skips the translation step'; Args = @('-s', '--no-gettext')
       Contains = @('cmake --build "build"')
       NotContains = @('run_gettext') }
    # Installing copies the whole tree again for a layout only releases need,
    # so it is asked for rather than assumed.
    @{ Name = 'nothing is installed unless asked'; Args = @('-s')
       Contains = @('run_gettext')
       NotContains = @('--target install') }
    @{ Name = '-i adds the install step'; Args = @('-s', '-i')
       Contains = @('--target install') }
    # install(TARGETS OrcaSlicer) has no OPTIONAL, so this would fail partway
    # through a build instead of before it.
    @{ Name = 'installing a tree with no executable is refused'; Args = @('-s', '-i', '--slicer-target', 'glad'); ExpectExit = 1
       Contains = @('--install needs the executable')
       NotContains = @('cmake --build') }
    # Without -s there is no install step, so there is nothing to refuse.
    @{ Name = 'the same flags without a slicer build are left alone'; Args = @('-d', '-i', '--slicer-target', 'glad')
       Contains = @('cmake -S deps')
       NotContains = @('--install needs the executable') }
    @{ Name = 'naming the executable target is allowed'; Args = @('-s', '-i', '--slicer-target', 'OrcaSlicer')
       Contains = @('--target install') }
    # Ninja counts compilers, so -j goes on the command line. MSBuild's -j
    # counts projects while /MP still runs one cl per core inside each, so the
    # cap goes to CL_MPCount in the environment instead. A dry run can only
    # show that no -j is passed.
    @{ Name = '-j on Ninja passes -j to cmake'; Args = @('-s', '-x', '-j', '4')
       Contains = @('Parallel jobs: 4', '--config Release  -j 4') }
    @{ Name = '-j on Ninja reaches the deps build'; Args = @('-d', '-x', '-j', '2')
       Contains = @('--target deps -j 2') }
    @{ Name = '-j on MSBuild does not pass -j, which would count projects'; Args = @('-s', '-j', '4')
       Contains = @('Parallel jobs: 4')
       NotContains = @('-j 4') }
    @{ Name = '-j on MSBuild leaves the deps build without -j too'; Args = @('-d', '-j', '2')
       Contains = @('--target deps')
       NotContains = @('-j 2') }
    @{ Name = 'no -j means no job limit'; Args = @('-s')
       NotContains = @('parallel jobs', '-j ') }
    @{ Name = 'a non-numeric -j is rejected'; Args = @('-s', '-j', 'abc'); ExpectExit = 1
       Contains = @('Invalid --jobs value') }
    @{ Name = 'a zero -j is rejected'; Args = @('-s', '-j', '0'); ExpectExit = 1
       Contains = @('Invalid --jobs value') }
    @{ Name = 'the loop options combine into a single build command'; Args = @('-s', '-x', '--no-configure', '--no-gettext', '--slicer-target', 'libslic3r_tests', '-j', '8')
       Contains = @('--target libslic3r_tests -j 8')
       NotContains = @('cmake -B "build" ', 'run_gettext', '--target install') }

    'one tree per configuration, compiler and architecture'
    # CMake resets its cache and carries on when the compiler changes under an
    # existing tree, leaving stamps from the old toolchain. Give each its own.
    @{ Name = 'clang builds land in their own trees'; Args = @('-d', '-s', '-l')
       Contains = @('cmake -S deps -B "deps/build-clang"', 'cmake -B "build-clang" ') }
    @{ Name = 'MSVC keeps the historical plain names'; Args = @('-d', '-s', '--msvc')
       Contains = @('cmake -S deps -B "deps/build"', 'cmake -B "build" ')
       NotContains = @('build-clang') }
    @{ Name = 'configuration, compiler and arch all name the tree'; Args = @('-d', '-s', '-l', '--config', 'debug', '--arch', 'arm64')
       Contains = @('cmake -S deps -B "deps/build-dbg-clang-arm64"', 'cmake -B "build-dbg-clang-arm64" ') }

    'locating the dependency tree'
    # Without --deps-dir nothing is passed, so CMakeLists derives the path from
    # the binary directory name as it always has.
    @{ Name = 'the deps path is left to CMake by default'; Args = @('-s')
       NotContains = @('-DDEP_BUILD_DIR') }
    @{ Name = '--deps-dir tells the slicer where the deps are'; Args = @('-s', '--deps-dir', 'D:\orca-deps')
       Contains = @('-DDEP_BUILD_DIR="D:\orca-deps"') }
    @{ Name = '--deps-dir also redirects the deps build'; Args = @('-d', '--deps-dir', 'D:\orca-deps')
       Contains = @('cmake -S deps -B "D:\orca-deps"', 'cmake --build "D:\orca-deps"')
       NotContains = @('deps/build') }
    # Paths are quoted throughout, so one with spaces survives the whole run.
    @{ Name = 'a deps path with spaces survives'; Args = @('-d', '-s', '--deps-dir', 'C:\Program Files\deps')
       Contains = @('-B "C:\Program Files\deps"', '-DDEP_BUILD_DIR="C:\Program Files\deps"') }
    @{ Name = '--deps-dir also redirects the pack'; Args = @('-p', '--deps-dir', 'D:\orca-deps')
       Contains = @('cd /d "D:\orca-deps"') }
    @{ Name = 'pack uses an absolute default path'; Args = @('-p')
       Match = @('^\+ cd /d "[A-Za-z]:\\.*\\deps\\build"') }
    # CMakeLists derives DEP_BUILD_DIR from the build directory's name, so
    # pointing the build elsewhere has to name the deps tree outright.
    @{ Name = '--build-dir moves the slicer build'; Args = @('-s', '-l', '-x', '--build-dir', 'out/build/x64-clang')
       Contains = @('cmake -B "out/build/x64-clang" ') }
    @{ Name = '--build-dir still names the deps tree'; Args = @('-s', '-l', '-x', '--build-dir', 'out/build/x64-clang')
       Match = @('-DDEP_BUILD_DIR="[A-Za-z]:\\.*\\deps\\build-clang"') }
    @{ Name = '--deps-dir wins over the derived tree'; Args = @('-s', '--build-dir', 'out/build/x64-clang', '--deps-dir', 'D:\orca-deps')
       Contains = @('-DDEP_BUILD_DIR="D:\orca-deps"') }
    # A value ending in a backslash escapes the closing quote it is spliced
    # into, so cmake would receive D:\tree" and swallow the next argument.
    @{ Name = 'a trailing backslash is trimmed off a path'; Args = @('-s', '--build-dir', 'D:\tree\')
       Contains = @('cmake -B "D:\tree" ') }
    @{ Name = 'a build directory with spaces survives'; Args = @('-s', '--build-dir', 'C:\Program Files\tree')
       Contains = @('cmake -B "C:\Program Files\tree" ') }
    @{ Name = 'the default build names no deps tree'; Args = @('-s')
       NotContains = @('DEP_BUILD_DIR') }

    'saying what mode and toolchain are in play'
    @{ Name = 'a dry run announces itself before the first command'; Args = @('-u')
       First = '^Dry run: printing commands without running them\.$' }
    @{ Name = 'the announcement leads even a full build'; Args = @('-ds')
       First = '^Dry run: ' }
    # Autodetect only runs for the Visual Studio generator, and only when no
    # release was pinned, so it needs a Visual Studio on the machine.
    @{ Name = 'the detected Visual Studio names its release year'; Args = @('-s')
       Match = @('^Detected Visual Studio \d+ \(20\d\d\)$') }
    @{ Name = 'pinning a release skips detection'; Args = @('-s', '--vs', '2022')
       NotContains = @('Detected Visual Studio') }

    'an action has to be asked for'
    # Neither can happen without building the slicer, so they stand alone the
    # way --install-vs does.
    @{ Name = '--run-tests is an action on its own'; Args = @('--run-tests')
       Contains = @('-DBUILD_TESTS=ON', 'ctest --test-dir')
       NotContains = @('Nothing to do') }
    @{ Name = '--tests is too'; Args = @('--tests')
       Contains = @('-DBUILD_TESTS=ON')
       NotContains = @('Nothing to do', 'ctest --test-dir') }
    # Naming an action means that action, not a fuller build.
    @{ Name = 'they do not add a slicer build to one already asked for'; Args = @('-d', '--tests')
       Contains = @('cmake -S deps')
       NotContains = @('cmake -B "build"') }
    @{ Name = 'shaping options alone are not an action'; Args = @('--config', 'debug'); ExpectExit = 1
       Contains = @('Nothing to do.')
       NotContains = @('Build completed') }
    @{ Name = '-j alone is not an action either'; Args = @('-j', '4'); ExpectExit = 1
       Contains = @('Nothing to do.') }
    @{ Name = '--install-vs counts as an action on its own'; Args = @('--install-vs', 'ide')
       NotContains = @('Nothing to do.') }

    'diagnostics'
    @{ Name = '-v asks cmake for the command lines'; Args = @('-s', '-v')
       Contains = @('--verbose') }
    @{ Name = '-v applies to the deps build too'; Args = @('-d', '-v')
       Contains = @('--target deps', '--verbose') }
    # ninja's own default shows neither a percentage nor a time. %w and %W
    # need ninja 1.12, and an unknown placeholder is fatal, so the format is
    # chosen from the version rather than hardcoded.
    @{ Name = '-v names the ninja progress format'; Args = @('-s', '-x', '-v')
       Match = @('^Ninja progress format: \[.*%p.*\]') }
    @{ Name = 'a ninja older than 1.12 gets the format it understands'; Args = @('-s', '-x', '-v')
       Env = @{ PATH = $ninjaPaths['old'] }
       Contains = @('Ninja progress format: [%s/%t %p :: %e]') }
    @{ Name = 'ninja 1.12 gets elapsed and remaining'; Args = @('-s', '-x', '-v')
       Env = @{ PATH = $ninjaPaths['new'] }
       Contains = @('Ninja progress format: [%f/%t %p :: %w / %W]') }
    @{ Name = 'a format you set yourself is left alone'; Args = @('-s', '-x', '-v')
       Env = @{ NINJA_STATUS = '[mine] ' }
       Contains = @('Ninja progress format: [mine]') }
    @{ Name = 'MSBuild builds mention no ninja format'; Args = @('-s', '-v')
       NotContains = @('Ninja progress format') }
    @{ Name = 'builds are quiet without -v'; Args = @('-s')
       NotContains = @('--verbose') }

    'installing prerequisites'
    # -u installs CMake, Perl and Git. Visual Studio is a separate ask, because
    # most people already have it, and which one you want is a real choice.
    @{ Name = '-u alone installs no Visual Studio'; Args = @('-u')
       Contains = @('Kitware.CMake', 'StrawberryPerl', 'Git.Git')
       NotContains = @('Microsoft.VisualStudio') }
    # Asking for Visual Studio is asking to install prerequisites, so it does
    # not also need -u; requiring both meant --install-vs alone did nothing.
    @{ Name = '--install-vs works without -u'; Args = @('--install-vs', 'ide')
       Contains = @('id=Microsoft.VisualStudio.Community', 'Kitware.CMake', 'Git.Git') }
    @{ Name = '--install-vs buildtools asks for the build tools'; Args = @('-u', '--install-vs', 'buildtools')
       Contains = @('id=Microsoft.VisualStudio.BuildTools')
       NotContains = @('VC.CoreIde') }
    @{ Name = '--install-vs ide asks for Community with the IDE component'; Args = @('-u', '--install-vs', 'ide')
       Contains = @('id=Microsoft.VisualStudio.Community', 'VC.CoreIde') }
    @{ Name = 'an unknown edition is rejected and the known ones listed'; Args = @('-u', '--install-vs', 'bogus'); ExpectExit = 1
       Contains = @('Unknown Visual Studio edition', 'buildtools, ide') }
    @{ Name = '--vs picks which release to install'; Args = @('-u', '--vs', '2022', '--install-vs', 'buildtools')
       Contains = @('id=Microsoft.VisualStudio.2022.BuildTools') }
    @{ Name = '-l adds the clang compiler and the MSBuild toolset'; Args = @('-u', '--install-vs', 'buildtools', '-l')
       Contains = @('VC.Llvm.Clang ', 'VC.Llvm.ClangToolset') }
    # CMake 4.x breaks Boost.Context on ARM64, so the installer pins 3.31 there
    # and leaves x64 on the current release.
    @{ Name = 'CMake is pinned to 3.31 when installing for arm64'; Args = @('-u', '--arch', 'arm64')
       Contains = @('Kitware.CMake --version 3.31.8') }
    @{ Name = 'CMake is not pinned for x64'; Args = @('-u', '--arch', 'x64')
       Contains = @('Kitware.CMake')
       NotContains = @('--version') }
    @{ Name = 'installing for arm64 asks for the ARM64 toolset'; Args = @('--install-vs', 'buildtools', '--arch', 'arm64')
       Contains = @('Microsoft.VisualStudio.Component.VC.Tools.ARM64') }
    @{ Name = 'an x64 install asks only for the x64 toolset'; Args = @('--install-vs', 'buildtools')
       Contains = @('Microsoft.VisualStudio.Component.VC.Tools.x86.x64')
       NotContains = @('VC.Tools.ARM64') }

    'dry run changes nothing'
    # clean_tree resolves the path before removing it, so the line echoed is
    # absolute. It is the last thing printed before a directory goes.
    @{ Name = '-c echoes the deps rmdir rather than running it'; Args = @('-d', '-c')
       Match = @('^\+ rmdir /S /Q "[A-Za-z]:\\.*\\deps\\build"$') }
    @{ Name = '-c removes the tree --deps-dir named, not the default one'; Args = @('-d', '-c', '--deps-dir', 'D:\orca-deps')
       Match = @('^\+ rmdir /S /Q "D:\\orca-deps"$')
       NotContains = @('\deps\build') }
    @{ Name = '-c echoes the slicer rmdir rather than running it'; Args = @('-s', '-c')
       Match = @('^\+ rmdir /S /Q "[A-Za-z]:\\.*\\build"$') }
    @{ Name = 'cleaning a slicer build leaves the deps tree alone'; Args = @('-s', '-c')
       NotMatch = @('rmdir.*\\deps\\') }
    @{ Name = 'cleaning both builds removes both trees'; Args = @('-d', '-s', '-c')
       Match = @('^\+ rmdir /S /Q "[A-Za-z]:\\[^"]*\\deps\\build"$',
                 '^\+ rmdir /S /Q "[A-Za-z]:\\(?!.*\\deps\\)[^"]*\\build"$') }

    # Nothing below should be reachable. They are a floor under a bug that
    # hands clean_tree a path far shorter than it looks.
    @{ Name = 'a configuration whose lookup comes back empty is rejected'; Args = @('-d', '-c', '--config', 'release '); ExpectExit = 1
       Contains = @('Unknown configuration')
       NotContains = @('rmdir') }
    @{ Name = 'a drive root is refused'; Args = @('-d', '-c', '--deps-dir', 'D:\'); ExpectExit = 1
       Contains = @('that is a drive root')
       NotContains = @('+ rmdir') }
    @{ Name = 'the repository itself is refused'; Args = @('-d', '-c', '--deps-dir', '.'); ExpectExit = 1
       Contains = @('that is the repository itself')
       NotContains = @('+ rmdir') }
    @{ Name = '-k echoes the taskkills rather than running them'; Args = @('-k')
       Contains = @('+ taskkill /F /IM MSBuild.exe', '+ taskkill /F /IM cl.exe') }
    @{ Name = '-k also covers the Ninja toolchain'; Args = @('-k')
       Contains = @('+ taskkill /F /IM ninja.exe', '+ taskkill /F /IM clang-cl.exe') }
    @{ Name = '-k announces the dry run like every other action'; Args = @('-k')
       First = '^Dry run: ' }
    @{ Name = '-u echoes the winget installs rather than running them'; Args = @('-u')
       Contains = @('+ winget install', 'Kitware.CMake')
       NotContains = @('cmake -S deps') }
    # Not a dry run: winget is a stub that fails, so nothing is installed.
    @{ Name = 'a failed install is reported, not claimed as success'; Args = @('-u')
       DryRun = $false; ExpectExit = 1
       Env = @{ PATH = $stubPath }
       Contains = @('Failed to install:', 'CMake', 'Perl', 'Git')
       NotContains = @('Installed the prerequisites') }
    # The reason belongs inside the frame. Every command this path ran had
    # already succeeded, so naming the last one would point at the wrong thing.
    @{ Name = 'a failed install names the reason, not the last command'; Args = @('-u')
       DryRun = $false; ExpectExit = 1
       Env = @{ PATH = $stubPath }
       Contains = @('####', 'Failed to install:')
       NotContains = @('Failed: winget') }
    @{ Name = 'a dry run does not claim the install happened'; Args = @('-u')
       Contains = @('Dry run: nothing was installed.')
       NotContains = @('are in place') }
    @{ Name = '-u with a build defers that build, not a fuller one'; Args = @('-u', '-d')
       Contains = @('build_win.bat -d')
       NotContains = @('build_win.bat -ds') }
    @{ Name = '-u on its own suggests the whole build'; Args = @('-u')
       Contains = @('build_win.bat -ds') }
    @{ Name = 'a dry run only echoes, it never configures'; Args = @('-d')
       Contains = @('+ cmake')
       NotContains = @('CMake Error', 'Configuring done') }

    'extra configure arguments'
    # --deps-args and --slicer-args are declared "rawstring", so a value that
    # looks like an option is allowed through. Plain string options still
    # reject one, since there it almost always means a forgotten value.
    @{ Name = '--deps-args reaches the deps configure'; Args = @('-d', '--deps-args', 'FOO')
       Contains = @('-DCMAKE_BUILD_TYPE=Release FOO') }
    @{ Name = '--slicer-args reaches the slicer configure'; Args = @('-s', '--slicer-args', 'BAZ')
       Contains = @('BAZ') }
    @{ Name = '--deps-args accepts a value that starts with a dash'; Args = @('-d', '--deps-args', '-DFOO')
       Contains = @('-DFOO') }
    @{ Name = 'a plain string option still rejects a dash-leading value'; Args = @('-d', '--deps-target', '--tests'); ExpectExit = 1
       Contains = @('looks like another option') }
    # cmd splits arguments on "=" as well as spaces, so an unquoted -D reaches
    # the script as two arguments however it was invoked.
    @{ Name = 'an unquoted value containing = is rejected'; Args = @('-d', '--deps-args', 'FOO=BAR'); ExpectExit = 1
       Contains = @('Unknown argument') }
    # The environment overrides exist for that reason; nothing tokenises them.
    @{ Name = 'ORCA_DEPS_CMAKE_ARGS reaches the deps configure'; Args = @('-d')
       Env = @{ ORCA_DEPS_CMAKE_ARGS = '-DFOO=BAR -DBAZ=QUX' }
       Contains = @('-DFOO=BAR -DBAZ=QUX') }
    @{ Name = 'ORCA_SLICER_CMAKE_ARGS reaches the slicer configure'; Args = @('-s')
       Env = @{ ORCA_SLICER_CMAKE_ARGS = '-DWANTED=1' }
       Contains = @('-DWANTED=1') }
    @{ Name = 'the deps override does not leak into the slicer configure'; Args = @('-s')
       Env = @{ ORCA_DEPS_CMAKE_ARGS = '-DDEPSONLY=1' }
       NotContains = @('-DDEPSONLY=1') }
    @{ Name = 'the help points at the environment for a spaced argument'; Args = @('--help'); DryRun = $false
       Contains = @('Neither form supports a value containing an ampersand') }
    @{ Name = 'the help lists the environment overrides'; Args = @('--help'); DryRun = $false
       Contains = @('Environment:', 'ORCA_DEPS_CMAKE_ARGS', 'ORCA_SLICER_CMAKE_ARGS') }

    'running the unit tests'
    @{ Name = '--tests builds them without running them'; Args = @('-s', '--tests')
       Contains = @('-DBUILD_TESTS=ON')
       NotContains = @('ctest') }
    @{ Name = '--run-tests builds and runs them'; Args = @('-s', '--run-tests')
       Contains = @('-DBUILD_TESTS=ON', 'ctest --test-dir "build/tests" -C Release --output-on-failure') }
    @{ Name = '--run-tests follows the build type and directory'; Args = @('-s', '--run-tests', '--config', 'debug')
       Contains = @('ctest --test-dir "build-dbg/tests" -C Debug') }
    @{ Name = 'no tests are run by default'; Args = @('-s')
       NotContains = @('ctest') }

    'failures are reported'
    @{ Name = 'a missing cmake is caught and exits non-zero'; Args = @('-d'); ExpectExit = 1
       Env = @{ PATH = 'C:\Windows\system32;C:\Windows' }
       Contains = @('CMake was not found') }
    @{ Name = 'packing does not need cmake, only an archiver'; Args = @('-p')
       Env = @{ PATH = 'C:\Windows\system32;C:\Windows' }
       NotContains = @('CMake was not found') }
    # Not a dry run: a cd to a missing drive is a real failure inside a
    # parenthesised block, which is where exit /b silently loses its code.
    # Without the jump to :die this exits 0 and a failed build reads as a
    # successful one.
    @{ Name = 'a failure inside a build block reaches the caller'; Args = @('-p', '--deps-dir', 'Z:\nope')
       DryRun = $false; ExpectExit = 1
       Contains = @('Exit code 1.', '####')
       NotContains = @('Build completed', 'Try') }
    # The retry follows the stage that failed. Offering it for the whole run
    # would clean a dependency tree that was not at fault.
    @{ Name = 'a failure names a retry scoped to the stage that failed'; Args = @('-d', '-s', '--deps-dir', 'Z:\nope')
       DryRun = $false; ExpectExit = 1
       Contains = @('build_win.bat -d --deps-dir "Z:\nope" -c')
       NotContains = @('build_win.bat -ds') }
    # CMake's own failure here is hundreds of lines about package resolution.
    @{ Name = 'a missing dependency tree is named, not left to CMake'; Args = @('-s', '--deps-dir', 'Z:\nope')
       DryRun = $false; ExpectExit = 1
       Contains = @('Dependencies not found at', 'Build them with build_win.bat -d --deps-dir "Z:\nope"')
       NotContains = @('cmake -B', 'Try') }
    # Every other suggestion carries the flags that reproduce the run; a bare
    # -d would point at the MSVC tree after a clang build.
    @{ Name = 'the missing-deps hint names this toolchain'; Args = @('-s', '-l', '-x', '--deps-dir', 'deps/not-built')
       DryRun = $false; ExpectExit = 1
       Contains = @('Build them with build_win.bat -d -l -x --deps-dir "deps/not-built"')
       NotExists = @('deps/not-built') }
    # A dry run configures nothing, so it must not depend on which trees happen
    # to exist on the machine running the suite.
    @{ Name = 'a dry run does not check for the deps tree'; Args = @('-s', '--deps-dir', 'Z:\nope')
       Contains = @('cmake -B "build"')
       NotContains = @('Dependencies not found') }
    # -d is about to build them, so there is nothing to report yet.
    @{ Name = 'building the deps in the same run skips the check'; Args = @('-d', '-s', '--deps-dir', 'Z:\nope')
       DryRun = $false; ExpectExit = 1
       NotContains = @('Dependencies not found') }
    # A configure fails before any compiler runs, so -v has nothing to show.
    @{ Name = 'a configure failure is not offered a verbose rebuild'; Args = @('-d', '--deps-dir', 'Z:\nope')
       DryRun = $false; ExpectExit = 1
       Contains = @('-c     discard that tree')
       NotContains = @('-v     show the failing') }
    # A bare --no-configure succeeds on a machine that already has a usable
    # build tree, so name one that cannot exist instead.
    @{ Name = 'a build failure is'; Args = @('-s', '--no-configure', '--build-dir', 'deps/no-such-tree')
       DryRun = $false; ExpectExit = 1
       Contains = @('-v     show the failing')
       NotExists = @('deps/no-such-tree') }
    # --build-dir names the slicer tree, which a deps failure has nothing to do
    # with. --deps-dir stays, because that is the tree that failed.
    @{ Name = 'a deps retry leaves out the slicer tree'; Args = @('-d', '-s', '--deps-dir', 'Z:\nope', '--build-dir', 'D:\b')
       DryRun = $false; ExpectExit = 1
       Contains = @('build_win.bat -d --deps-dir "Z:\nope" -c')
       NotContains = @('--build-dir') }
    @{ Name = 'an unknown configuration stays a single line'; Args = @('-s', '--config', 'bogus'); ExpectExit = 1
       Contains = @('Unknown configuration')
       NotContains = @('####', 'Try') }
    @{ Name = 'a bad --jobs value is not framed either'; Args = @('-s', '-j', 'x'); ExpectExit = 1
       Contains = @('Invalid --jobs value')
       NotContains = @('####') }

    'the summary says what to do next'
    # Every suggested command carries the flags that reproduce this run.
    @{ Name = 'a deps build points at the slicer build'; Args = @('-d', '-l')
       Contains = @('Build the slicer      build_win.bat -s -l')
       NotContains = @('Run it') }
    @{ Name = 'a ninja slicer build offers a single target'; Args = @('-s', '-l', '-x')
       Contains = @('Rebuild after edits   build_win.bat -s -l -x --no-configure', 'Rebuild one target')
       NotContains = @('Solution', 'Open in Visual Studio') }
    @{ Name = 'a visual studio build names the solution instead'; Args = @('-s')
       Contains = @('Solution      ', 'Open in Visual Studio build\OrcaSlicer.sln',
                    'Rebuild after edits   build_win.bat -s --no-configure')
       NotContains = @('Rebuild one target') }
    @{ Name = 'the configuration and architecture come back'; Args = @('-s', '-l', '-x', '--config', 'debug', '--arch', 'arm64')
       Contains = @('build_win.bat -s -l -x --config debug --arch arm64 --no-configure') }
    @{ Name = 'the tree overrides come back quoted'; Args = @('-s', '--deps-dir', 'D:\d', '--build-dir', 'D:\b')
       Contains = @('--deps-dir "D:\d" --build-dir "D:\b"') }
    @{ Name = 'a pinned visual studio release comes back'; Args = @('-s', '--vs', '2022')
       Contains = @('build_win.bat -s --vs 2022 --no-configure') }
    # Autodetection writes what it found into the same variable, so a detected
    # release must not come back as though it had been asked for.
    @{ Name = 'a detected release does not'; Args = @('-s')
       NotContains = @('--vs') }
    @{ Name = 'the binary is named in the build tree it was built in'; Args = @('-s', '-l', '-x')
       Contains = @('build-clang\src\Release\orca-slicer.exe') }
    @{ Name = 'installing names the installed copy instead'; Args = @('-s', '-l', '-x', '-i')
       Contains = @('build-clang\OrcaSlicer\orca-slicer.exe') }
    # -i changes where the binary lands, so a rebuild that dropped it would
    # leave the path above pointing at a stale copy.
    @{ Name = 'the rebuild suggestion keeps -i'; Args = @('-s', '-l', '-x', '-i')
       Contains = @('Rebuild after edits   build_win.bat -s -l -x -i --no-configure') }
    # A deps retry has no install step to repeat.
    @{ Name = 'a deps retry drops it'; Args = @('-d', '-s', '-i', '--deps-dir', 'Z:\nope')
       DryRun = $false; ExpectExit = 1
       Contains = @('build_win.bat -d --deps-dir "Z:\nope" -c')
       NotContains = @('-d -i') }
    # Naming a target builds it and its dependencies, not its dependents, so
    # the binary on disk is whatever the last full build left there.
    @{ Name = 'a single-target build does not claim the whole binary'; Args = @('-s', '-l', '-x', '--slicer-target', 'glad')
       Contains = @('Target        glad', 'Relink the binary     build_win.bat -s -l -x --no-configure')
       NotContains = @('Run it', 'orca-slicer.exe', 'Rebuild after edits') }
    # The executable has a target of its own, and naming that one does relink.
    @{ Name = 'naming the executable target still claims the binary'; Args = @('-s', '-l', '-x', '--slicer-target', 'OrcaSlicer')
       Contains = @('Run it', 'orca-slicer.exe', 'Rebuild after edits')
       NotContains = @('Target        OrcaSlicer', 'Relink the binary') }
    @{ Name = '--run-tests offers the ctest line'; Args = @('-s', '-l', '-x', '--run-tests')
       Contains = @('Re-run the tests      ctest --test-dir build-clang/tests -C Release') }
    @{ Name = 'packing names the bundle and how to use it'; Args = @('-p', '-l')
       Contains = @('Bundle        ', 'Share the bundle') }
    # The line supplies its own tree, so the one this run used must not ride
    # along and contradict it.
    @{ Name = 'the bundle line names one tree, not two'; Args = @('-s', '-p', '-l', '-x', '--deps-dir', 'D:\shared')
       Contains = @('then build_win.bat -s -l -x --deps-dir <path>')
       NotContains = @('--deps-dir "D:\shared" --deps-dir') }
    @{ Name = 'installing prerequisites suggests the build that follows'; Args = @('-u', '-l')
       Contains = @('Restart this shell', 'build_win.bat -ds -l')
       NotContains = @('Run it') }
    # Everything below the header line is worked out the same way in either
    # run, which is why a dry run can cover it.
    @{ Name = 'a dry run does not claim a build happened'; Args = @('-s', '-l', '-x')
       Contains = @('Dry run: nothing was built.')
       NotContains = @('Build completed in') }
    # --no-configure is the iteration loop and still gets the block; four
    # lines after a rebuild is not enough to be worth suppressing.
    @{ Name = '--no-configure still gets the summary'; Args = @('-s', '-l', '-x', '--no-configure')
       Contains = @('Next', 'Rebuild after edits') }

    'pointing at the solution'
    @{ Name = 'the VS generator says where the solution is'; Args = @('-s')
       Match = @('^  Solution      .*\\build\\OrcaSlicer\.sln$') }
    @{ Name = 'the solution path follows the configuration'; Args = @('-s', '--config', 'debug')
       Match = @('^  Solution      .*\\build-dbg\\OrcaSlicer\.sln$') }
    @{ Name = 'the solution line survives an install'; Args = @('-s', '-i')
       Contains = @('  Solution      ') }
    # The path is resolved, not pasted onto the repository root, so it is
    # right whether --build-dir came absolute or with forward slashes.
    @{ Name = 'a moved build still prints one real path'; Args = @('-s', '--build-dir', 'out/build/x64-clang')
       Match = @('^  Solution      [A-Za-z]:\\[^/]+\\OrcaSlicer\.sln$') }
    @{ Name = 'an absolute --build-dir is not glued onto the repo root'; Args = @('-s', '--build-dir', 'D:\tree')
       Contains = @('Solution      D:\tree\OrcaSlicer.sln') }
)

function Invoke-BuildScript {
    param([string[]] $Arguments, [hashtable] $Environment)

    $saved = @{}
    if ($Environment) {
        foreach ($key in $Environment.Keys) {
            $saved[$key] = [Environment]::GetEnvironmentVariable($key)
            Set-Item -Path "env:$key" -Value $Environment[$key]
        }
    }
    try {
        # 'Stop' turns a native command's stderr into a terminating error, and
        # a case that exercises a real failure writes to stderr. Let the output
        # through and judge the run by its exit code instead. The assignment is
        # scoped to this function, so the rest of the suite keeps 'Stop'.
        $ErrorActionPreference = 'Continue'
        if ($Arguments.Count -eq 0) {
            $out = & $Script 2>&1 | Out-String
        } else {
            $out = & $Script @Arguments 2>&1 | Out-String
        }
        return [pscustomobject]@{ Output = $out; Exit = $LASTEXITCODE }
    } finally {
        foreach ($key in $saved.Keys) {
            if ($null -eq $saved[$key]) { Remove-Item -Path "env:$key" -ErrorAction SilentlyContinue }
            else { Set-Item -Path "env:$key" -Value $saved[$key] }
        }
    }
}

$knownFields = @(
    'Name', 'Args', 'ExpectExit', 'DryRun', 'First', 'Env',
    'Contains', 'NotContains', 'Match', 'NotMatch', 'NotExists'
)

function Test-Case {
    param([hashtable] $Case)

    # Read fields with the indexer, not dot notation. A hashtable exposes its
    # own members too, so $Case.Contains returns the Contains *method* whenever
    # the case has no key by that name.
    $argv = @($Case['Args'])
    if (-not $Case.ContainsKey('DryRun') -or $Case['DryRun']) { $argv += '--dry-run' }

    $expect = 0
    if ($Case.ContainsKey('ExpectExit')) { $expect = $Case['ExpectExit'] }

    $result = Invoke-BuildScript -Arguments $argv -Environment $Case['Env']

    $problems = @()

    # A misspelled field is silently ignored by the checks below, which
    # leaves the case asserting nothing at all and passing.
    foreach ($field in $Case.Keys) {
        if ($knownFields -notcontains $field) { $problems += "unknown field '$field'" }
    }

    if ($result.Exit -ne $expect) { $problems += "exit $($result.Exit), expected $expect" }
    foreach ($needle in $Case['Contains']) {
        if (-not $result.Output.Contains($needle)) { $problems += "missing '$needle'" }
    }
    foreach ($needle in $Case['NotContains']) {
        if ($result.Output.Contains($needle)) { $problems += "unexpected '$needle'" }
    }
    $lines = $result.Output -split "`r?`n"
    if ($Case['First'] -and $lines[0] -notmatch $Case['First']) {
        $problems += "first line was '$($lines[0])'"
    }
    foreach ($pattern in $Case['Match']) {
        if (@($lines | Where-Object { $_ -match $pattern }).Count -eq 0) {
            $problems += "no line matching /$pattern/"
        }
    }
    foreach ($pattern in $Case['NotMatch']) {
        foreach ($line in @($lines | Where-Object { $_ -match $pattern })) {
            $problems += "line matches /$pattern/: $line"
        }
    }
    # Output cannot show what a run did not create.
    foreach ($path in $Case['NotExists']) {
        $full = Join-Path (Split-Path -Parent $Script) $path
        if (Test-Path $full) {
            $problems += "created '$path'"
        }
    }
    return ,$problems
}

$pass = 0
$failed = @()
# Held back so a filtered run does not print headings for groups it skipped.
$heading = $null

foreach ($case in $cases) {
    if ($case -is [string]) {
        $heading = $case
        continue
    }
    if ($Name -and $case['Name'] -notmatch $Name) { continue }
    if ($heading) {
        Write-Host ''
        Write-Host $heading -ForegroundColor Cyan
        $heading = $null
    }

    $problems = Test-Case -Case $case
    if ($problems.Count -eq 0) {
        $pass++
        Write-Host ('  ok   ' + $case['Name'])
    } else {
        $failed += $case['Name']
        Write-Host ('  FAIL ' + $case['Name']) -ForegroundColor Red
        foreach ($problem in $problems) { Write-Host ('         ' + $problem) -ForegroundColor Red }
        Write-Host ('         args: ' + (@($case['Args']) -join ' '))
    }
}

Remove-Item -Recurse -Force $fixtures -ErrorAction SilentlyContinue

Write-Host ''
# A pattern that matched nothing has proved nothing, so do not report it as
# a clean run.
if ($Name -and $pass -eq 0 -and $failed.Count -eq 0) {
    Write-Host "no case matched /$Name/" -ForegroundColor Red
    exit 1
}
Write-Host "$pass passed, $($failed.Count) failed"
if ($failed.Count -gt 0) {
    foreach ($name in $failed) { Write-Host "  failed: $name" -ForegroundColor Red }
    exit 1
}
exit 0
