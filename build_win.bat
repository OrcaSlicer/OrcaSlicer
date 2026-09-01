@echo off

REM OrcaSlicer build script for Windows. Run with -h for the options.

REM ===========================================================================
REM  Script setup
REM ===========================================================================

setlocal enableDelayedExpansion

REM A manually set errorlevel shadows the real one, and cmd then stops
REM updating it after each command. Clear it, inside the scope above so
REM the caller keeps whatever it had.
set errorlevel=

REM Everything here is relative to the repository root: deps/, build/ and
REM the 7z in tools/. Work from there whatever directory this was called
REM from. setlocal above restores the caller directory on exit.
cd /d "%~dp0"
set "WP=%CD%"
set "script_name=%~nx0"

set argdefn=0

REM Both macros stop on a non-zero errorlevel; error_check also says so.
REM
REM error_check jumps to :die rather than calling exit /b where it stands.
REM Every use of it is inside a parenthesised block, and an exit /b there
REM ends the script but leaves the process exit code at zero, so a failed
REM build reported success. goto unwinds the block first.
set "repeat_error=if not ^!errorlevel^! == 0 exit /b ^!errorlevel^!"
set "error_check=if not ^!errorlevel^! == 0 (set rc=^!errorlevel^!& goto :die)"

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

REM ===========================================================================
REM  Command line options
REM ===========================================================================

call :add_section "Actions"
call :add_arg build_deps bool d deps "Download and build the dependencies, needed before -s"
call :add_arg build_slicer bool s slicer "Build OrcaSlicer"
call :add_arg build_tests bool "" tests "Build the unit tests"
call :add_arg run_tests bool "" run-tests "Build the unit tests and run them"
call :add_arg pack_deps bool p pack "Bundle the built dependencies into a zip file"
call :add_arg install_deps bool u install-deps "Install or update CMake, Perl and Git with WinGet"
call :add_arg install_vs string "" install-vs "Also install Visual Studio: buildtools or ide"
call :add_arg kill_jobs bool k kill-jobs "Kill any running build and compiler processes"
call :add_arg print_help bool h help "Print this help message"

call :add_section "Build configuration"
call :add_arg config string "" config "release, debug, relwithdebinfo or minsizerel (default: release)"
call :add_arg target_arch string "" arch "x64 or arm64 (default: the host architecture)"
call :add_arg slicer_asan bool a asan "Build the slicer with ASAN enabled"

call :add_section "Toolchain"
call :add_arg use_clang_cl bool l clang-cl "Use clang-cl as the compiler"
call :add_arg use_msvc bool "" msvc "Use cl as the compiler (default)"
call :add_arg use_ninja bool x ninja "Use the Ninja Multi-Config generator"
call :add_arg use_msbuild bool "" msbuild "Use the Visual Studio generator (default)"
call :add_arg vs_version string "" vs "Visual Studio release: 2019, 2022 or 2026 (default: autodetect)"
call :add_arg clang_path string "" clang-path "Path to clang-cl.exe, requires -x (default: the one from Visual Studio)"

call :add_section "How much gets rebuilt"
call :add_arg slicer_target string "" slicer-target "Build one slicer target instead of all, e.g. libslic3r"
call :add_arg deps_target string t deps-target "Build one dependency instead of all, e.g. dep_Boost"
call :add_arg no_configure bool "" no-configure "Build the existing tree without configuring"
call :add_arg no_gettext bool "" no-gettext "Skip regenerating the translations"
call :add_arg install_slicer bool i install "Install into the build tree's OrcaSlicer folder"
call :add_arg jobs string j jobs "Limit the build to N parallel jobs"
call :add_arg clean bool c clean "Remove the trees this run builds, deps with -d, slicer with -s"

call :add_section "Paths and extra arguments"
call :add_arg deps_dir string "" deps-dir "Dependency tree to build in or use, instead of the one named for this build"
call :add_arg slicer_dir string "" build-dir "Slicer build directory, instead of the one named for this build"
call :add_arg deps_args rawstring "" deps-args "Extra arguments for the deps configure, quoted"
call :add_arg slicer_args rawstring "" slicer-args "Extra arguments for the slicer configure, quoted"

call :add_section "Diagnostics"
call :add_arg verbose bool v verbose "Show the compiler command lines"
call :add_arg dry_run bool D dry-run "Print the commands instead of running them"

REM ===========================================================================
REM  Lookup tables
REM ===========================================================================

REM Known build configurations: the CMake build type, the directory to build
REM in, and the dependency tree to build against. Adding one means adding all
REM three entries in its group. The name is matched case-insensitively because
REM batch variable names are, so --config Debug finds cfg_type_debug.
REM
REM Release, RelWithDebInfo and MinSizeRel all link the /MD dependencies, so
REM they share one tree rather than each paying for its own. Debug needs /MDd
REM and cannot. deps/CMakeLists.txt makes the same split: only Debug turns on
REM DEP_DEBUG, while RelWithDebInfo just adds debug info to a release build.
set "cfg_type_release=Release"
set "cfg_dir_release=build"
set "cfg_dep_release=build"

set "cfg_type_debug=Debug"
set "cfg_dir_debug=build-dbg"
set "cfg_dep_debug=build-dbg"

set "cfg_type_relwithdebinfo=RelWithDebInfo"
set "cfg_dir_relwithdebinfo=build-dbginfo"
set "cfg_dep_relwithdebinfo=build"

set "cfg_type_minsizerel=MinSizeRel"
set "cfg_dir_minsizerel=build-minsize"
set "cfg_dep_minsizerel=build"

set "cfg_default=release"

REM Known Visual Studio releases: the generator name, the suffix in the
REM WinGet id, and the major version vswhere and msbuild report. Adding a
REM release means adding all three entries in its group.
set "vs_gen_2019=Visual Studio 16 2019"
set "vs_winget_2019=.2019"
set "vs_year_16=2019"

set "vs_gen_2022=Visual Studio 17 2022"
set "vs_winget_2022=.2022"
set "vs_year_17=2022"

REM 2026 is the unversioned WinGet id: there is no Microsoft.VisualStudio.2026.
set "vs_gen_2026=Visual Studio 18 2026"
set "vs_winget_2026="
set "vs_year_18=2026"

set "vs_default=2026"

REM What --install-vs asks WinGet for.
set "vs_edition_buildtools=BuildTools"
set "vs_edition_ide=Community"

REM ===========================================================================
REM  Command line handling
REM ===========================================================================

call :handle_args %*
%error_check%

if "%debugscript%" == "ON" (
    set /A range_end = %argdefn% - 1
    for /L %%i in (0, 1, !range_end!) do (
        call :echo_var !argdefs[%%i].VARIABLE_NAME!
    )
)

if "%~1" == "" (
	set print_help=ON
	goto :before_print_help
)

set "all_args=%*"
if "%all_args:"=%" == "" (
	set print_help=ON
)

:before_print_help

if "%print_help%" == "ON" (
    call :print_help_msg
    exit /b 0
)

REM Say so before the first + line scrolls past.
if "%dry_run%" == "ON" echo Dry run: printing commands without running them.

if "%kill_jobs%" == "ON" (
	echo Stopping build processes.
	call :kill_image MSBuild.exe
	call :kill_image ninja.exe
	call :kill_image cl.exe
	call :kill_image clang-cl.exe
	exit /b 0
)

REM Neither test option can happen without building the slicer, so asking for
REM one asks for that, unless another action was already named.
if "%build_deps%%build_slicer%%pack_deps%%install_deps%%install_vs%" == "" (
    if "%build_tests%" == "ON" set "build_slicer=ON"
    if "%run_tests%" == "ON" set "build_slicer=ON"
)

REM Options like --config or -j only shape a build. Without one of the actions
REM there is nothing for them to shape, so say so rather than resolving a whole
REM build and reporting it took no time. The block above may have added one.
if "%build_deps%%build_slicer%%pack_deps%%install_deps%%install_vs%" == "" (
    echo Nothing to do. Pick an action: -d, -s, -p or -u. Run -h for the full list.
    exit /b 1
)

REM ===========================================================================
REM  Visual Studio and target architecture
REM ===========================================================================

REM Asking for Visual Studio is asking to install prerequisites. Resolve the
REM edition here rather than testing whether the name is defined: `if defined`
REM stops at the first space, so "ide " would pass and then expand to nothing.
set "vs_edition="
if not "%install_vs%" == "" (
    set "install_deps=ON"
    set "vs_edition=!vs_edition_%install_vs%!"
    if "!vs_edition!" == "" (
        echo Unknown Visual Studio edition "%install_vs%". Known editions: buildtools, ide.
        exit /b 1
    )
)
REM Autodetection overwrites vs_version, so snapshot whether it was pinned.
set "vs_pinned=%vs_version%"

if not "%vs_version%" == "" if "%use_ninja%" == "ON" (
    echo --vs and --ninja select different generators.
    exit /b 1
)
REM install(TARGETS OrcaSlicer) carries no OPTIONAL, so installing a tree
REM whose executable was never built fails. Say so before the build starts.
if "%build_slicer%" == "ON" if "%install_slicer%" == "ON" if not "%slicer_target%" == "" if /I not "%slicer_target%" == "OrcaSlicer" (
    echo --install needs the executable, but --slicer-target names "%slicer_target%".
    exit /b 1
)
if not "%vs_version%" == "" if "!vs_gen_%vs_version%!" == "" (
    echo Unknown Visual Studio release "%vs_version%". Known releases: 2019, 2022, 2026.
    exit /b 1
)

call :autodetect_vs
%error_check%

REM autodetect leaves this empty when it finds nothing usable.
if "%vs_version%" == "" set "vs_version=%vs_default%"

REM Default to the host CPU. PROCESSOR_ARCHITEW6432 covers a 32-bit shell on
REM a 64-bit OS, where PROCESSOR_ARCHITECTURE reads x86.
set arch=x64
if /I "%PROCESSOR_ARCHITECTURE%" == "ARM64" set arch=ARM64
if /I "%PROCESSOR_ARCHITEW6432%" == "ARM64" set arch=ARM64
if not "%target_arch%" == "" (
	if /I "%target_arch%" == "arm64" (
		set arch=ARM64
	) else (
		if /I "%target_arch%" == "x64" (
			set arch=x64
		) else (
			echo Unknown architecture "%target_arch%". Expected x64 or arm64.
			exit /b 1
		)
	)
)

REM ===========================================================================
REM  Installing prerequisites
REM ===========================================================================

if "%install_deps%" == "ON" (
    where winget >nul 2>nul
    if not !errorlevel! == 0 (
        echo WinGet was not found
        exit /b 1
    )
    REM Keep going after one failure so the rest still get installed, then
    REM name the ones that did not rather than claiming they all did.
    set "install_failed="
    set "winget_args=-e --source=winget"
    if not "%install_vs%" == "" (
        set "vs_year=!vs_winget_%vs_version%!"
        set "ide_component_flag="
        if /I "%install_vs%" == "ide" set "ide_component_flag=Microsoft.VisualStudio.Component.VC.CoreIde"
        REM Two components: the clang-cl compiler itself, and the MSBuild
        REM toolset that lets the Visual Studio generator drive it. One
        REM install covers both x64 and ARM64.
        set "clang_cl_flag="
        if "%use_clang_cl%" == "ON" set "clang_cl_flag=Microsoft.VisualStudio.Component.VC.Llvm.Clang Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset"
        REM The x64 tools build the host tooling either way; targeting ARM64
        REM needs its own toolset on top of them.
        set "arm64_tools_flag="
        if /I "%arch%" == "ARM64" set "arm64_tools_flag=Microsoft.VisualStudio.Component.VC.Tools.ARM64"
        call :print_and_run winget install !winget_args! --id=Microsoft.VisualStudio!vs_year!.!vs_edition! --force --custom "--add !ide_component_flag! Microsoft.VisualStudio.Component.VC.Tools.x86.x64 !arm64_tools_flag! Microsoft.VisualStudio.Component.VC.CMake.Project Microsoft.VisualStudio.Component.Windows11SDK.22621 !clang_cl_flag!"
        call :note_failed "Visual Studio" !errorlevel!
    )

    REM CMake 4 dropped pre-3.5 policy support and ships incomplete ASM_ARMASM
    REM linker modules, which breaks Boost.Context on ARM64. CI pins the same way.
    set "cmake_version_flag="
    if /I "%arch%" == "ARM64" set "cmake_version_flag=--version 3.31.8"
    call :print_and_run winget install !winget_args! --id=Kitware.CMake !cmake_version_flag!
    call :note_failed CMake !errorlevel!
    call :print_and_run winget install !winget_args! --id=StrawberryPerl.StrawberryPerl
    call :note_failed Perl !errorlevel!
    call :print_and_run winget install !winget_args! --id=Git.Git
    call :note_failed Git !errorlevel!

    if defined install_failed (
        set "die_reason=Failed to install:!install_failed!"
        set rc=1
        goto :die
    )

    REM Flags to repeat back in the deferred build command.
    set "next_flags="
    if "%use_clang_cl%" == "ON" set "next_flags=!next_flags! -l"
    if "%use_ninja%" == "ON" set "next_flags=!next_flags! -x"
    if not "%target_arch%" == "" set "next_flags=!next_flags! --arch %target_arch%"

    REM Name the build that was deferred, not a fuller one.
    set "next_actions="
    if "%build_deps%" == "ON" set "next_actions=!next_actions!d"
    if "%build_slicer%" == "ON" set "next_actions=!next_actions!s"
    if "%pack_deps%" == "ON" set "next_actions=!next_actions!p"
    if "!next_actions!" == "" set "next_actions=ds"

    echo.
    echo -------------------------------------------------------------
    REM A dry run installed nothing, so do not report that it did.
    if "%dry_run%" == "ON" (
        echo Dry run: nothing was installed.
    ) else (
        echo Installed the prerequisites.
    )
    echo.
    echo Next
    REM The new PATH cannot reach this shell, so the build runs in the next one.
    echo     Restart this shell so the new PATH takes effect, then
    echo     build_win.bat -!next_actions!!next_flags!
    echo -------------------------------------------------------------
    exit /b 0
)

REM ===========================================================================
REM  Resolving the build
REM ===========================================================================

REM A value ending in a backslash escapes the closing quote when it is
REM spliced into a command line: -B "D:\tree\" reaches cmake as D:\tree"
REM and swallows the argument after it. No path here needs one.
call :trim_slash deps_dir
call :trim_slash slicer_dir
call :trim_slash clang_path

REM Naming a specific clang-cl is naming clang-cl. Doing it before the
REM conflict check below means --clang-path with --msvc is caught there.
if not "%clang_path%" == "" set "use_clang_cl=ON"
if not "%clang_path%" == "" if not exist "%clang_path%" (
    echo No clang-cl at "%clang_path%".
    exit /b 1
)
REM A trailing backslash matches directories only, and a directory would
REM otherwise reach CMake as the compiler.
if not "%clang_path%" == "" if exist "%clang_path%\" (
    echo "%clang_path%" is a directory. Name clang-cl.exe itself.
    exit /b 1
)

REM Naming a default explicitly is fine; naming both sides is not.
if "%use_clang_cl%" == "ON" if "%use_msvc%" == "ON" (
    echo --clang-cl and --msvc select different compilers.
    exit /b 1
)
if "%use_ninja%" == "ON" if "%use_msbuild%" == "ON" (
    echo --ninja and --msbuild select different generators.
    exit /b 1
)

set "generator=!vs_gen_%vs_version%!"
if "%use_ninja%" == "ON" (
    set "generator=Ninja Multi-Config"
    call :setup_dev_env
    %error_check%
    set "using_ninja=ON"
)

if "%using_ninja%" == "ON" (
	if "%use_clang_cl%" == "ON" (
		REM Bare, so it resolves from the PATH the dev shell just set up, which
		REM is the clang shipped with Visual Studio. --clang-path names another.
		set "clang_exe=clang-cl.exe"
		if not "%clang_path%" == "" set "clang_exe="%clang_path%""
		set "gen_args=-DCMAKE_C_COMPILER=!clang_exe! -DCMAKE_CXX_COMPILER=!clang_exe!"
	)
) else (
	set "gen_args=-A !arch!"
	if "%use_clang_cl%" == "ON" (
		set "gen_args=!gen_args! -T ClangCL"
	)
)

if not "%clang_path%" == "" if not "%using_ninja%" == "ON" (
    echo --clang-path needs the Ninja generator; add -x. The Visual Studio
    echo generator builds clang-cl through its own ClangCL toolset.
    exit /b 1
)

REM Ninja prints [123/456] by default, which says nothing about how far
REM along it is or how long is left. %p is a percentage, %w and %W are
REM elapsed and remaining, but those two arrived in ninja 1.12 and an
REM unknown placeholder is fatal rather than a warning, so an older ninja
REM would abort the build instead of degrading. Ask which one is on PATH.
REM Keep %p first inside the brackets: VS Code reads the first digits%
REM it finds there to drive its progress bar.
if "%using_ninja%" == "ON" if not defined NINJA_STATUS (
    set "nj_major=0"
    set "nj_minor=0"
    for /f "tokens=1,2 delims=." %%a in ('ninja --version 2^>nul') do (
        set "nj_major=%%a"
        set "nj_minor=%%b"
    )
    set "NINJA_STATUS=[%%s/%%t %%p :: %%e] "
    if !nj_major! GTR 1 set "NINJA_STATUS=[%%f/%%t %%p :: %%w / %%W] "
    if !nj_major! EQU 1 if !nj_minor! GEQ 12 set "NINJA_STATUS=[%%f/%%t %%p :: %%w / %%W] "
)
if "%verbose%" == "ON" if defined NINJA_STATUS echo Ninja progress format: !NINJA_STATUS!

if "%deps_target%" == "" (
	set "deps_target=deps"
)

REM Only the builds need CMake. -p zips an existing tree with 7z.
if "%build_deps%%build_slicer%" == "" goto :cmake_ready

cmake --version >nul 2>nul
if not !errorlevel! == 0 (
    echo CMake was not found. Have you installed the system dependencies?
    exit /b 1
)

REM Strawberry Perl ships a c/bin full of GNU tools, and the top-level
REM CMakeLists refuses to configure when it precedes CMake on PATH. Put a
REM real CMake first, skipping any hit from Strawberry's own cmake.exe, or
REM this pins exactly the order it is meant to undo. The findstr needle
REM must not end in a backslash, which escapes the quote and matches nothing.
set "cmake_bin="
for /f "delims=" %%i in ('where cmake 2^>nul') do (
    if not defined cmake_bin (
        echo %%~dpi| findstr /i /c:"\Strawberry\c\bin" >nul || set "cmake_bin=%%~dpi"
    )
)
if defined cmake_bin set "PATH=%cmake_bin%;%PATH%"
:cmake_ready

if "%config%" == "" set "config=%cfg_default%"

REM Judge what the lookup returned rather than whether the name is defined.
REM "release " passed `if defined` and then expanded to nothing, leaving an
REM empty build directory for --clean to remove.
set "build_type=!cfg_type_%config%!"
set "tree_name=!cfg_dir_%config%!"
set "dep_name=!cfg_dep_%config%!"
set "bad_config="
if "!build_type!" == "" set "bad_config=ON"
if "!tree_name!" == "" set "bad_config=ON"
if "!dep_name!" == "" set "bad_config=ON"
if defined bad_config (
    echo Unknown configuration "%config%". Known configurations: release, debug, relwithdebinfo, minsizerel.
    exit /b 1
)

REM A tree is only good for the configuration, compiler and architecture it
REM was made with. Change one underneath it and CMake resets its cache and
REM carries on, leaving ExternalProject stamps from the old toolchain and
REM sub-builds that fail scattershot on things that build fine from scratch.
REM So name the tree for all three. MSVC x64 keeps the historical names.
if "%use_clang_cl%" == "ON" set "tree_name=!tree_name!-clang"
if /I "%arch%" == "ARM64" set "tree_name=!tree_name!-arm64"
if "%use_clang_cl%" == "ON" set "dep_name=!dep_name!-clang"
if /I "%arch%" == "ARM64" set "dep_name=!dep_name!-arm64"

set "build_dir=!tree_name!"
if not "%slicer_dir%" == "" set "build_dir=%slicer_dir%"

REM Resolve it once. --build-dir may arrive absolute, relative, or with
REM forward slashes, and anything that prints the directory has to show a
REM real path rather than one glued onto the repository root.
for %%p in ("!build_dir!") do set "build_full=%%~fp"
echo Configuration: %build_type%, %arch%

set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"

set "TESTS_FLAG=-DBUILD_TESTS=OFF"
if "%build_tests%" == "ON" set "TESTS_FLAG=-DBUILD_TESTS=ON"
if "%run_tests%" == "ON" set "TESTS_FLAG=-DBUILD_TESTS=ON"

set "SLICER_TARGET_FLAG="
if not "%slicer_target%" == "" set "SLICER_TARGET_FLAG=--target %slicer_target%"

set "DEP_TREE=deps/!dep_name!"
set "DEP_TREE_PACK=%WP%\deps\!dep_name!"
set "DEP_TREE_FLAG="
if not "%deps_dir%" == "" (
    set "DEP_TREE=%deps_dir%"
    set "DEP_TREE_PACK=%deps_dir%"
    set "DEP_TREE_FLAG=-DDEP_BUILD_DIR="%deps_dir%""
)

REM CMakeLists derives DEP_BUILD_DIR from the build directory NAME, which is
REM wrong whenever the build tree and the dependency tree are named
REM differently, as they are for every configuration that shares the release
REM dependencies. Name it outright once the two stop matching.
if "!DEP_TREE_FLAG!" == "" if not "%build_dir%" == "!dep_name!" (
    set "DEP_TREE_FLAG=-DDEP_BUILD_DIR="%WP%\deps\!dep_name!""
)

set "VERBOSE_FLAG="
if "%verbose%" == "ON" set "VERBOSE_FLAG=--verbose"

REM The two generators count differently.
REM
REM Ninja counts compilers, so -j is exact. The deps superbuild reads
REM CMAKE_BUILD_PARALLEL_LEVEL for the sub-builds it drives.
REM
REM MSBuild turns -j into /m, which counts projects, not compilers: /MP still
REM runs one cl per core inside each one, so -j 1 would not give one
REM compiler. CL_MPCount sets the /MP degree instead, and MSBuild reads
REM properties from the environment, so nested deps builds inherit it. /m
REM stays at its default of one project at a time.
set "JOBS_FLAG="
if "%jobs%" == "" goto :jobs_ready
echo %jobs%| findstr /r /c:"^[1-9][0-9]*$" >nul
if errorlevel 1 (
    echo Invalid --jobs value "%jobs%". Expected a positive integer.
    exit /b 1
)
if "%using_ninja%" == "ON" (
    set "JOBS_FLAG=-j %jobs%"
    set "CMAKE_BUILD_PARALLEL_LEVEL=%jobs%"
) else (
    set "CL_MPCount=%jobs%"
)
echo Parallel jobs: %jobs%
:jobs_ready

REM The flags that reproduce this run, for the commands the summary suggests.
REM Kept in three parts because they are not all relevant everywhere: a deps
REM retry has no use for --build-dir, and the bundle line names its own tree.
set "recall_tc="
if "%use_clang_cl%" == "ON" set "recall_tc=!recall_tc! -l"
if "%using_ninja%" == "ON" set "recall_tc=!recall_tc! -x"
if /I not "%config%" == "%cfg_default%" set "recall_tc=!recall_tc! --config %config%"
if not "%target_arch%" == "" set "recall_tc=!recall_tc! --arch %target_arch%"
if not "%vs_pinned%" == "" set "recall_tc=!recall_tc! --vs %vs_pinned%"
if not "%clang_path%" == "" set "recall_tc=!recall_tc! --clang-path "%clang_path%""

set "recall_i="
if "%install_slicer%" == "ON" set "recall_i= -i"
set "recall_dd="
if not "%deps_dir%" == "" set "recall_dd= --deps-dir "%deps_dir%""
set "recall_bd="
if not "%slicer_dir%" == "" set "recall_bd= --build-dir "%slicer_dir%""
set "recall=!recall_tc!!recall_i!!recall_dd!!recall_bd!"

REM ===========================================================================
REM  Running the build
REM ===========================================================================

REM CMake 4 refuses a pre-3.5 policy version; several deps still ask for one.
set CMAKE_POLICY_VERSION_MINIMUM=3.5

set _START_TIME=%TIME%

if "%build_deps%" == "ON" (
    REM Which stage is running, so a failure can name it and suggest a
    REM retry scoped to it rather than to the whole run.
    set "stage=d"
    echo Building the dependencies...

    if "%clean%" == "ON" (
        call :clean_tree "!DEP_TREE!"
        %error_check%
    )

    if not "%no_configure%" == "ON" (
        call :print_and_run cmake -S deps -B "!DEP_TREE!" -G "%generator%" %gen_args% -DCMAKE_BUILD_TYPE=%build_type% !deps_args! %ORCA_DEPS_CMAKE_ARGS%
        %error_check%
    )

    call :print_and_run cmake --build "!DEP_TREE!" --config %build_type% --target %deps_target% %JOBS_FLAG% %VERBOSE_FLAG%
    %error_check%
)

if "%pack_deps%" == "ON" (
    set "stage=p"
    setlocal ENABLEDELAYEDEXPANSION
    call :print_and_run cd /d "!DEP_TREE_PACK!"
    %error_check%
    REM date /t prints in the machine locale and its field order varies by
    REM region, which is how the inherited parse produced YYYYDDMM. Ask for
    REM an unambiguous stamp instead. powershell.exe lives under
    REM System32\WindowsPowerShell rather than System32, so a trimmed PATH
    REM cannot find it and the stamp comes back empty.
    set "ps=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
    set "build_date="
    for /f %%d in ('!ps! -NoProfile -Command "Get-Date -Format yyyyMMdd"') do set "build_date=%%d"
    if "!build_date!" == "" (
        set "die_reason=Could not read the date from !ps!."
        set rc=1
        goto :die
    )

    REM A bundle is only good for what built it, so it carries the same parts
    REM as the dependency tree. Release x64 on cl keeps the plain name.
    set "dep_flavour=!cfg_dep_%config%!"
    set "dep_flavour=!dep_flavour:build=!"
    set "dep_variant=%arch%"
    if "%use_clang_cl%" == "ON" set "dep_variant=!dep_variant!-clang"
    set "dep_variant=!dep_variant!!dep_flavour!"
    echo Packing the dependencies: OrcaSlicer_dep_win-!dep_variant!_!build_date!.zip

    REM tools\7z.exe loads its codecs from a 7z.dll, and the repo does not
    REM carry one, so it can only archive on a machine that has 7-Zip
    REM installed. Windows has shipped bsdtar in System32 since 10 1803 and
    REM it writes an ordinary deflate zip, so fall back to that rather than
    REM failing on a machine that has everything it needs.
    set "zipper="
    if exist "%WP%\tools\7z.dll" set "zipper=%WP%/tools/7z.exe a"
    if not defined zipper if exist "%SystemRoot%\System32\tar.exe" set "zipper=%SystemRoot%\System32\tar.exe -a -c -f"
    if not defined zipper (
        set "die_reason=No archiver. tools\7z.exe needs a 7z.dll, and this Windows has no System32\tar.exe."
        set "die_hint=Install 7-Zip, or copy its 7z.dll into tools\."
        set rc=1
        goto :die
    )

    call :print_and_run !zipper! OrcaSlicer_dep_win-!dep_variant!_!build_date!.zip OrcaSlicer_dep
    %error_check%
    REM endlocal is about to discard the name, so carry the path out with it.
    for %%z in ("!DEP_TREE_PACK!\OrcaSlicer_dep_win-!dep_variant!_!build_date!.zip") do endlocal & set "bundle=%%~fz"
)

if "%build_slicer%" == "ON" (
    set "stage=s"
    echo Building OrcaSlicer...

    if "%clean%" == "ON" (
        call :clean_tree "%build_dir%"
        %error_check%
    )

    if "%slicer_asan%" == "ON" (
        set "slicer_args=!slicer_args! -DSLIC3R_ASAN=ON"
    )

    REM Configuring against a tree that was never built fails deep inside
    REM package resolution. Name it here instead. Skipped when -d is about to
    REM build it in this same run, and under a dry run, which configures
    REM nothing and must not depend on what happens to be on the machine.
    if not "%dry_run%" == "ON" if not "%no_configure%" == "ON" if not "%build_deps%" == "ON" if not exist "!DEP_TREE!\OrcaSlicer_dep\usr\local\" (
        for %%p in ("!DEP_TREE!") do set "die_reason=Dependencies not found at %%~fp"
        set "die_hint=Build them with build_win.bat -d!recall!."
        REM Only worth suggesting to someone who did not name a tree.
        if "%deps_dir%" == "" set "die_hint=Build them with build_win.bat -d!recall!, or point --deps-dir at an existing tree."
        set rc=1
        goto :die
    )

    if not "%no_configure%" == "ON" (
        call :print_and_run cmake -B "%build_dir%" -G "%generator%" %gen_args% -DORCA_TOOLS=ON %SIG_FLAG% %TESTS_FLAG% %DEP_TREE_FLAG% -DCMAKE_BUILD_TYPE=%build_type% !slicer_args! %ORCA_SLICER_CMAKE_ARGS%
        %error_check%
    )

    call :print_and_run cmake --build "%build_dir%" --config %build_type% %SLICER_TARGET_FLAG% %JOBS_FLAG% %VERBOSE_FLAG%
    %error_check%

    if "%run_tests%" == "ON" (
        call :print_and_run ctest --test-dir "%build_dir%/tests" -C %build_type% --output-on-failure
        %error_check%
    )

    if not "%no_gettext%" == "ON" (
        call :print_and_run call scripts/run_gettext.bat
        %error_check%
    )

    if "%install_slicer%" == "ON" (
        call :print_and_run cmake --build "%build_dir%" --target install --config %build_type%
        %error_check%
    )
)

REM Elapsed wall clock. The 1%%a-100 trick strips a leading zero, which set /A
REM would otherwise read as octal, and a negative total means the build ran
REM past midnight.
for /f "tokens=1-3 delims=:.," %%a in ("%_START_TIME: =0%") do set /a "_start_s=(1%%a-100)*3600+(1%%b-100)*60+(1%%c-100)"
for /f "tokens=1-3 delims=:.," %%a in ("%TIME: =0%") do set /a "_end_s=(1%%a-100)*3600+(1%%b-100)*60+(1%%c-100)"
set /a "_elapsed=_end_s - _start_s"
if %_elapsed% lss 0 set /a "_elapsed+=86400"
set /a "_hours=_elapsed / 3600"
set /a "_remainder=_elapsed - _hours * 3600"
set /a "_mins=_remainder / 60"
set /a "_secs=_remainder - _mins * 60"
call :summary
exit /b 0

REM Reached only by error_check, from the top level, where exit /b works.
REM The hash block is what CMakeLists already uses for a build that cannot
REM continue, so it means the same thing here.
:die
echo.
echo #############################################################
REM The paths that set die_reason had no command fail, so last_cmd there
REM names one that succeeded.
if defined die_reason echo !die_reason!
if not defined die_reason if defined last_cmd echo Failed: !last_cmd!
if defined die_hint echo !die_hint!
echo Exit code %rc%.
REM -v only makes the build verbose, so it has nothing to offer a configure
REM that failed before any compiler ran.
set "failed_configure="
if "!last_cmd:~0,9!" == "cmake -B " set "failed_configure=ON"
if "!last_cmd:~0,9!" == "cmake -S " set "failed_configure=ON"
REM Scoped to the stage that failed. Offering -c for the whole run would
REM discard a dependency tree that was not at fault, and neither flag does
REM anything for a failed pack. A named reason already carries its own advice.
if "!stage!" == "d" set "recall=!recall_tc!!recall_dd!"
if not defined die_reason if defined stage if not "!stage!" == "p" (
    echo.
    echo Try
    if not defined failed_configure echo     build_win.bat -!stage!!recall! -v     show the failing compiler command line
    echo     build_win.bat -!stage!!recall! -c     discard that tree and configure from scratch
)
echo #############################################################
exit /b %rc%

REM ===========================================================================
REM  Function definitions
REM ===========================================================================

REM summary - what was produced, and what to do with it next. A dry run says
REM so rather than claiming the files exist, but every line below that is
REM worked out the same way in either run.
:summary
    for %%p in ("!DEP_TREE!") do set "dep_full=%%~fp"
    REM The binary only leaves the build tree when it is installed.
    set "slicer_exe=%build_dir%\src\%build_type%\orca-slicer.exe"
    if "%install_slicer%" == "ON" set "slicer_exe=%build_dir%\OrcaSlicer\orca-slicer.exe"
    for %%p in ("!slicer_exe!") do set "slicer_full=%%~fp"
    REM Naming a target builds it and its dependencies, not its dependents,
    REM so only a full build or the executable's own target relinks.
    set "linked=ON"
    if not "%slicer_target%" == "" set "linked="
    if /I "%slicer_target%" == "OrcaSlicer" set "linked=ON"

    echo.
    echo -------------------------------------------------------------
    if "%dry_run%" == "ON" (
        echo Dry run: nothing was built. A real run would report:
    ) else (
        echo Build completed in %_hours%h %_mins%m %_secs%s
    )

    if "%build_deps%" == "ON" echo   Dependencies  !dep_full!
    if "%build_slicer%" == "ON" if "%linked%" == "ON" echo   OrcaSlicer    !slicer_full!
    if "%build_slicer%" == "ON" if not "%linked%" == "ON" echo   Target        %slicer_target%
    if "%build_slicer%" == "ON" if not "%using_ninja%" == "ON" echo   Solution      %build_full%\OrcaSlicer.sln
    if "%pack_deps%" == "ON" if defined bundle echo   Bundle        !bundle!

    echo.
    echo Next
    if "%build_slicer%" == "ON" (
        if "%linked%" == "ON" echo     Run it                !slicer_exe!
        if not "%using_ninja%" == "ON" echo     Open in Visual Studio %build_dir%\OrcaSlicer.sln
        if "%linked%" == "ON" echo     Rebuild after edits   build_win.bat -s!recall! --no-configure
        if not "%linked%" == "ON" echo     Relink the binary     build_win.bat -s!recall! --no-configure
        if "%linked%" == "ON" if "%using_ninja%" == "ON" echo     Rebuild one target    build_win.bat -s!recall! --no-configure --slicer-target libslic3r
        if "%run_tests%" == "ON" echo     Re-run the tests      ctest --test-dir %build_dir%/tests -C %build_type% --output-on-failure
    ) else (
        if "%build_deps%" == "ON" echo     Build the slicer      build_win.bat -s!recall!
    )
    if "%pack_deps%" == "ON" echo     Share the bundle      unzip it elsewhere, then build_win.bat -s!recall_tc! --deps-dir ^<path^>
    echo -------------------------------------------------------------
    exit /b 0

:debug_msg
    if "%debugscript%" == "ON" echo %*
    exit /b 0

REM get_str_len <string> -> length in %ret%
:get_str_len
    setlocal
    set "in=%~1"
    for /L %%i in (0, 1, 100) do (
        if "!in:~%%i,1!" == "" (
            set out=%%i
            goto :break_str_len
        )
    )

    echo error in get_str_len: string is too long
    endlocal
    exit /b 1

    :break_str_len
    endlocal & set ret=%out%
    exit /b 0

:print_help_msg
    setlocal

    REM Measure the widest flag column. Section headers have no flags and
    REM must not widen it.
    set flags=
    set max_len=0
    set /A range_end = %argdefn% - 1
    for /L %%i in (0, 1, %range_end%) do (
        if "!argdefs[%%i].TYPE!" == "section" (
            set "flags[%%i]="
        ) else (
            set str_placeholder=
            if not "!argdefs[%%i].TYPE!" == "bool" (
                set "str_placeholder= <value>"
            )

            REM The placeholder goes on the long form only. Repeating it on
            REM the short one says nothing extra and costs eight columns.
            set "flag_str="
            if not "!argdefs[%%i].SHORT_FLAG!" == "" (
                set "flag_str=-!argdefs[%%i].SHORT_FLAG!"
            )

            if not "!argdefs[%%i].LONG_FLAG!" == "" (
                if "!flag_str!" == "" (
                    REM No short flag: pad so the long one lines up with the others.
                    set "flag_str=    "
                ) else (
                    set "flag_str=!flag_str!, "
                )
                set "flag_str=!flag_str!--!argdefs[%%i].LONG_FLAG!!str_placeholder!"
            ) else (
                set "flag_str=!flag_str!!str_placeholder!"
            )
            set "flag_str=!flag_str!  "
            set "flags[%%i]=!flag_str!"
            call :get_str_len "!flag_str!"
            if !ret! GTR !max_len! (
                set max_len=!ret!
            )
        )
    )

    set padding=
    for /L %%i in (0, 1, %max_len%) do set "padding=!padding! "

    echo Builds OrcaSlicer and its dependencies on Windows.
    echo.
    echo Usage: %script_name% [options]
    set /A range_end = %argdefn% - 1
    for /L %%i in (0, 1, !range_end!) do (
        if "!argdefs[%%i].TYPE!" == "section" (
            echo.
            echo !argdefs[%%i].HELP_TEXT!:
        ) else (
            set "flag=!flags[%%i]!%padding%"
            echo    !flag:~0,%max_len%!!argdefs[%%i].HELP_TEXT!
        )
    )
    echo.
    echo Examples:
    echo    %script_name% --install-vs ide          Set up a new machine, then restart the shell
    echo    %script_name% -ds                       Build the dependencies, then the slicer
    echo    %script_name% -s -l -x                  Rebuild the slicer with clang-cl and Ninja
    echo    %script_name% -s --no-configure -j 8    Rebuild quickly while iterating
    echo    %script_name% -s --slicer-target glad   Compile one target to check the toolchain
    echo    %script_name% -l -x --run-tests         Test that toolchain's build, not the default one
    echo.
    echo Environment:
    echo    ORCA_DEPS_CMAKE_ARGS      Extra arguments for the deps configure
    echo    ORCA_SLICER_CMAKE_ARGS    Extra arguments for the slicer configure
    echo    ORCA_UPDATER_SIG_KEY      Update signing key baked into the slicer
    echo    git_commit_hash           Revision to stamp, so a commit does not rebuild everything
    echo    NINJA_STATUS              Ninja progress format, if you want your own
    echo    debugscript               Set to ON to trace this script
    echo.
    echo       set ORCA_SLICER_CMAKE_ARGS=-DSLIC3R_PCH=OFF -DSLIC3R_MSVC_PDB=OFF
    echo       $env:ORCA_SLICER_CMAKE_ARGS = '-DSLIC3R_PCH=OFF'      (PowerShell)
    echo.
    echo    --deps-args and --slicer-args cannot carry a value with spaces; use
    echo    these instead. Neither form supports a value containing an ampersand.
    endlocal
    exit /b 0

REM add_section <title>
:add_section
    set argdefs[%argdefn%].VARIABLE_NAME=
    set argdefs[%argdefn%].TYPE=section
    set argdefs[%argdefn%].SHORT_FLAG=
    set argdefs[%argdefn%].LONG_FLAG=
    set argdefs[%argdefn%].HELP_TEXT=%~1
    set /A argdefn+=1
    exit /b 0

REM add_arg <variable> <type:bool,string,rawstring> <short> <long> <help>
:add_arg
    set argdefs[%argdefn%].VARIABLE_NAME=%~1
    set argdefs[%argdefn%].TYPE=%~2
    set argdefs[%argdefn%].SHORT_FLAG=%~3
    set argdefs[%argdefn%].LONG_FLAG=%~4
    set argdefs[%argdefn%].HELP_TEXT=%~5

    REM Start every option unset, so "defined" means the user gave it.
    set %~1=

    if "%debugscript%" == "ON" (
        echo add_arg VARIABLE_NAME: !argdefs[%argdefn%].VARIABLE_NAME!
        echo add_arg TYPE: !argdefs[%argdefn%].TYPE!
        echo add_arg SHORT_FLAG: !argdefs[%argdefn%].SHORT_FLAG!
        echo add_arg LONG_FLAG: !argdefs[%argdefn%].LONG_FLAG!
        echo add_arg HELP_TEXT: !argdefs[%argdefn%].HELP_TEXT!
    )

    set /A argdefn+=1

    exit /b 0

REM find_arg <type:short,long> <flag> -> index in %ret%
:find_arg
    setlocal
    call :debug_msg starting function: find_arg "%~1" "%~2"

    set type=
    if /I "%~1" == "short" set type=SHORT
    if /I "%~1" == "long" set type=LONG
    if not defined type (
        endlocal
        set ret=
        exit /b 1
    )

    call :debug_msg find_arg type=%type%
    set /A range_end = %argdefn% - 1
    for /L %%i in (0, 1, %range_end%) do (
        if "!argdefs[%%i].%type%_FLAG!" == "%~2" (
            set idx=%%i
            goto :find_arg_cont
        )
    )

    echo Error in find_arg: Failed to find arg "%~2"
    call :print_help_msg

    endlocal
    set ret=
    exit /b 1

    :find_arg_cont
    call :debug_msg find_arg: found at %idx%
    endlocal & (
        set ret=%idx%
    )
    exit /b 0

REM set_arg <index> [<value>]
:set_arg
    call :debug_msg starting function: set_arg "%~1" "%~2"

    if "%~1" == "" (
        echo Error in set_arg: no index provided
        exit /b 1
    )

    setlocal
    if /I "!argdefs[%~1].TYPE!" == "bool" (
        call :debug_msg set_arg: setting bool type to ON
        set val=ON
    ) else (
        set "val=%~2"
        set quote_char="
        REM Delayed expansion throughout: %val% here would be the value from the
        REM previous call, since the whole block is parsed before it runs.
        if "!val:~0,1!" == "!quote_char!" (
            if "!val:~-1,1!" == "!quote_char!" (
                set "val=!val:~1,-1!"
            )
        )

        call :debug_msg set_arg: setting string type to %~2
    )
    set var_name=!argdefs[%~1].VARIABLE_NAME!

    endlocal & (
        REM Set variable in parent scope
        set "%var_name%=%val%"

        REM Add variable to finalize command
        set "finalize_cmd=%finalize_cmd% & set "%var_name%=%val%""
    )

    exit /b 0

REM get_arg_type <index> -> type in %ret%
:get_arg_type
    call :debug_msg starting function get_arg_type "%~1"
    setlocal
    set type=!argdefs[%~1].TYPE!
    endlocal & set ret=%type%
    exit /b 0

REM echo_var <variable>
:echo_var
    echo %~1=!%~1!
    exit /b 0

:autodetect_vs
	REM Nothing to detect once the release is pinned, or under Ninja.
	if not "%vs_version%" == "" exit /b 0
	if "%use_ninja%" == "ON" exit /b 0

	setlocal

	%VSWHERE% -nologo >nul 2>nul
	if not !errorlevel! == 0 (
		REM vswhere is not in its usual place; try msbuild instead.
		goto :msbuild_check
	)

	echo Detecting Visual Studio version using vswhere...
	for /f "tokens=1 delims=." %%i in ('%VSWHERE% -nologo -products * -latest -property catalog_productDisplayVersion') do (
		set "VS_MAJOR=%%i"
		goto :version_found
	)

	:msbuild_check
	where msbuild >nul 2>nul
	if not !errorlevel! == 0 (
		REM No msbuild either; leave the release empty and let the default apply.
		endlocal
		exit /b 0
	)

	echo Detecting Visual Studio version using msbuild...

	REM The version line varies by release, so try two patterns for it.
	set VS_MAJOR=
	for /f "tokens=*" %%i in ('msbuild -version 2^>^&1 ^| findstr /r "^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"') do (
		for /f "tokens=1 delims=." %%a in ("%%i") do set VS_MAJOR=%%a
		set MSBUILD_OUTPUT=%%i
		goto :version_found
	)

	REM The same pattern unanchored, for releases that print a banner first.
	if "%VS_MAJOR%"=="" (
		for /f "tokens=*" %%i in ('msbuild -version 2^>^&1 ^| findstr /r "[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"') do (
			for /f "tokens=1 delims=." %%a in ("%%i") do set VS_MAJOR=%%a
			set MSBUILD_OUTPUT=%%i
			goto :version_found
		)
	)

	:version_found
	set "detected=!vs_year_%VS_MAJOR%!"
	if not defined detected (
		echo Error: Unsupported Visual Studio major version: %VS_MAJOR%
		endlocal
		exit /b 1
	)
	echo Detected Visual Studio %VS_MAJOR% ^(!detected!^)
	endlocal & set "vs_version=%detected%"

	exit /b 0

:setup_dev_env
	REM A dry run runs nothing, and VsDevCmd costs seconds.
	if "%dry_run%" == "ON" exit /b 0
	set "dev_arch=x64"
	if /I "!arch!" == "ARM64" set "dev_arch=arm64"
	%VSWHERE% -nologo >nul 2>nul
	if not !errorlevel! == 0 (
		REM vswhere is not in its usual place; use the environment as it stands.
		exit /b 0
	)

	for /f "tokens=*" %%i in ('%VSWHERE% -nologo -products * -latest -property resolvedInstallationPath') do (
		set "VS_PATH=%%i"
		goto :vs_path_found
	)
	exit /b 0

	:vs_path_found
	call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=!dev_arch! >nul 2>nul
	set "VS_PATH="

	exit /b 0

REM clean_tree <path> - remove a build tree, refusing anything that is not
REM one. Nothing here should ever fire; it is a floor under a bug that
REM produced a path far shorter than it looks.
:clean_tree
    setlocal
    set "target=%~1"
    if not defined target (
        echo Refusing to clean: no directory to remove.
        exit /b 1
    )

    REM Resolve first, because "." and "deps/.." are shorter than they read.
    for %%p in ("!target!") do set "full=%%~fp"
    if "!full:~1!" == ":\" (
        echo Refusing to clean "!full!": that is a drive root.
        exit /b 1
    )
    if /I "!full!" == "%WP%" (
        echo Refusing to clean "!full!": that is the repository itself.
        exit /b 1
    )

    call :print_and_run rmdir /S /Q "!full!"
    exit /b !errorlevel!

REM note_failed <name> <code> - record a failed install. Two winget codes
REM mean the tool is already there, which is what -u is for:
REM   -1978335135 (0x8A150061) winget declined: a version is already there
REM   -1978335189 (0x8A15002B) installed and already the newest version
:note_failed
    if "%~2" == "0" exit /b 0
    if "%~2" == "-1978335135" exit /b 0
    if "%~2" == "-1978335189" exit /b 0
    set "install_failed=%install_failed% %~1"
    exit /b 0

REM trim_slash <variable> - drop a trailing backslash from a path value.
REM A drive root keeps its own: "C:\" is not the same place as "C:".
:trim_slash
    set "trim_value=!%~1!"
    if not defined trim_value exit /b 0
    if not "!trim_value:~-1!" == "\" exit /b 0
    if "!trim_value:~-2!" == ":\" exit /b 0
    set "%~1=!trim_value:~0,-1!"
    exit /b 0

REM kill_image <name> - stop every process of one image. The count is printed
REM first because taskkill can take a while and says nothing until it returns.
REM Never fails, so one stubborn image cannot stop the rest.
:kill_image
    setlocal
    REM A dry run does not count, so it prints the same on every machine.
    if "%dry_run%" == "ON" (
        call :print_and_run taskkill /F /IM %~1
        endlocal
        exit /b 0
    )
    set "img_count=0"
    for /f "tokens=1" %%a in ('tasklist /nh /fi "IMAGENAME eq %~1" 2^>nul') do (
        if /I "%%a" == "%~1" set /a img_count+=1
    )
    if "!img_count!" == "0" (
        echo    %~1: none running
        endlocal
        exit /b 0
    )
    echo    %~1: stopping !img_count!
    call :print_and_run taskkill /F /IM %~1
    endlocal
    exit /b 0

REM print_and_run <command...>
:print_and_run
    echo + %*
    REM Recorded before the command runs. A set afterwards would clear the
    REM errorlevel the next line returns, reporting every failure as a pass.
    set "last_cmd=%*"
    if not "%dry_run%" == "ON" (
        %*
        exit /b !errorlevel!
    )
    exit /b 0

REM handle_args <args...>
:handle_args
    call :debug_msg starting function handle_args "%*"
    if "%~1" == "" exit /b 0

    setlocal
    set arg=%~1
    set finalize_cmd=

    call :debug_msg Begin handling arg "%arg%"

    if not "%arg:~0,2%" == "--" goto :HAL_check_short

    call :debug_msg Processing long arg

    call :find_arg long %arg:~2%
    %repeat_error%
    set idx=%ret%

    call :get_arg_type %idx%
    %repeat_error%
    set type=%ret%

    if /I not "%type%" == "bool" (
        set "string_val=%~2"
        if not defined string_val (
            echo Error in handle_args_loop: The option "%arg:~2%" requires a value
            exit /b 1
        )

        REM rawstring options carry cmake arguments, which start with a dash, so
        REM only plain string options reject a value that looks like an option.
        if /I "%type%" == "string" (
            if "!string_val:~0,1!" == "-" (
                echo Error in handle_args_loop: The option "%arg:~2%" requires a value. "!string_val!" looks like another option.
                exit /b 1
            )
        )
        shift
    )

    call :set_arg %idx% "%string_val%"
    %repeat_error%
    goto :handle_args_loop_reset

    :HAL_check_short
    if not "%arg:~0,1%" == "-" (
        echo Unknown argument: %arg%
        call :print_help_msg
        exit /b 1
    )

    call :debug_msg processing short args
    set /A charidx=1
    :short_arg_loop
        if "!arg:~%charidx%,1!" == "" goto :handle_args_loop_reset

        call :find_arg short !arg:~%charidx%,1!
        %repeat_error%
        set idx=%ret%

        call :get_arg_type %idx%
        %repeat_error%
        set type=%ret%

        set /A start_idx = %charidx% + 1
        if /I not "%type%" == "bool" (
            REM A value may be attached (-j8) or be the next argument (-j 8).
            set remaining=!arg:~%start_idx%!
            if defined remaining (
                set string_val=!remaining!
            ) else (
                set "string_val=%~2"
                shift
            )
            if not defined string_val (
                echo Error in handle_args_loop: The option "!arg:~%charidx%,1!" requires a value
                exit /b 1
            )

            REM As in the long-option branch above.
            if /I "%type%" == "string" (
                if "!string_val:~0,1!" == "-" (
                    echo Error in handle_args_loop: The option "!arg:~%charidx%,1!" requires a value. "!string_val!" looks like another option.
                    exit /b 1
                )
            )
            call :set_arg !idx! "!string_val!"
            %repeat_error%
            goto :handle_args_loop_reset
        )

        call :set_arg %idx%
        %repeat_error%
        set /A charidx+=1
    goto :short_arg_loop

    :handle_args_loop_reset
    REM endlocal drops this iteration scope; finalize_cmd re-applies the options
    REM it set, in the caller scope.
    endlocal %finalize_cmd%
    shift
    goto :handle_args
