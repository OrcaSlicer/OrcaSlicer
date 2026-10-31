# Runner Diagnostics — 2026-10-29

## Summary

All 8 Windows Build Deps CI jobs on `orca-win-server` fail identically with a **silent ninja failure after all 320 build steps complete**. The failure produces zero compiler or linker error output, making diagnosis difficult. This affects `.bat` baseline and `.ps1` candidate jobs equally, confirming it is **not a script bug**.

## Affected Jobs

| Preset | Script | Status |
|--------|--------|--------|
| x64-msvc | `.bat` (baseline) | FAIL |
| x64-msvc | `.ps1` (candidate) | FAIL |
| x64-clang | `.bat` (baseline) | FAIL |
| x64-clang | `.ps1` (candidate) | FAIL |
| x64-clang-cache | `.bat` (baseline) | FAIL |
| x64-clang-cache | `.ps1` (top candidate) | FAIL |
| x64-msbuild | `.bat` (baseline) | FAIL |
| arm64-clang | `.bat` (baseline) | FAIL |

## Failure Details

### What succeeds
- All 320 ninja build steps for libjpeg-turbo complete successfully (step 320/320 = "Linking C executable test\Release\md5sum.exe")
- Timestamps on all steps are `[0:1:9]` (same second, suggesting either very fast cached steps or buffered output)

### What fails
```
[0:1:9] ninja: build stopped: subcommand failed.
```
- Ninja reports failure **after** all 320 subcommands complete
- **No** compiler error (no `error Cxxxx`, `fatal error`, `LNKxxxx`)
- **No** linker error output
- **No** cmake error message
- The `Invoke-BuildStep` log tail (last 30 lines) shows only the final build steps, ending at step 309 — steps 310-320 and the failure message appear in the CI console but not the log (indicating the log capture captures the build output but not the final ninja failure line, which may come from a different stream)

### Key observation
The failure occurs **after** all build steps report success. This means:
1. A subcommand's failure output is not captured by `2>&1` redirection
2. OR a post-build step (install, verification, copy) fails silently
3. OR ninja itself encounters an internal error after all commands succeed

## Environment Analysis

### What the runner has
- Self-hosted Windows runner: `orca-win-server`
- Visual Studio BuildTools installed
- Boost pre-installed via `choco install boost`
- Ninja available
- `ml64.exe` from VS BuildTools (path fixed in script commit `6e4d1ccae9`)

### What might be missing/wrong
1. **MASM (ml64.exe) component** — VS BuildTools on the runner may not have the "C++ Build Tools" → "MASM" → "MSVC v143 build tools" → "C++ MASM for v143 build tools" component installed. Without it, Boost.build (which uses MASM directly) fails.
2. **Runner resource exhaustion** — The install step after building all 320 libjpeg-turbo targets might be failing due to disk space, memory, or file locks.
3. **ExternalProject install step** — After `BUILD_COMMAND` succeeds, `INSTALL_COMMAND` runs (`cmake --build . --target install --config Release`). If install fails (permissions, missing dirs), the overall deps target fails.

## Comparison with Main Branch

| Branch | Build Deps | Check Cache | Slicer Build |
|--------|-----------|-------------|-------------|
| `main` | ✅ Passes (cache hit) | ✅ Passes | ✅ Passes |
| `feat/build-performance` | ❌ FAILS (fresh Boost build) | ✅ Passes | ❌ BLOCKED |

**Why main passes:** Main's CI uses a cached deps build (cache key `deps-cache-${{ github.ref }}` with `restore-keys` to parent branches). The cache hit means libjpeg-turbo targets are restored and skip building entirely.

**Why our branch fails:** Our branch has a different cache key (branch-scoped), so on the first run it must build all deps from scratch. The fresh libjpeg-turbo build succeeds but ninja dies silently right at the end.

## What Was Tried

| Attempt | Commit | Result |
|---------|--------|--------|
| Fix `ml64.exe` PATH in `vsenv.ps1` + pass `-DCMAKE_ASM_MASM_COMPILER` | `6e4d1ccae9` | Still fails — failure is AFTER build, not during |
| Skip Boost ExternalProject build entirely, use `choco install boost` + `-DSKIP_BOOST=ON` | `ec2a4d0848`, `adfcbe8316` | Still fails — Boost skip works, but libjpeg-turbo step 320 fails |
| Log capture: buffer output, write to file, show 30-line tail on failure | `4f9127a3b6` | Log tail shows steps 293-309 but NO error message — the actual failure is not in stdout/stderr |
| Add `--verbose` to ninja via `-- --verbose` | `90301f8658` | Shows full build commands but no error output from the failing subcommand |
| Fix deps/CMakeLists.txt: `add_custom_target` dummy instead of `ExternalProject_Add` | `adfcbe8316` | Boost skip now works correctly; libjpeg-turbo still fails at 320/320 |

## Why Logs Are Not Helpful

The `Invoke-BuildStep` log capture (commit `4f9127a3b6`) buffers all stdout+stderr via `2>&1` and writes to a `.log` file, then shows the last 30 lines on failure. However:

- The last 30 lines show build steps, not error messages
- Steps 310-320 are NOT in the tail (they appear in CI console but log only shows up to ~309)
- After step 320, the log ends — no error text, no stack trace, no "FAILED:" line from ninja

This strongly suggests the error output goes to a stream that PowerShell's `2>&1` pipeline does **not** capture (e.g., a separate Windows event, a child process' stderr that gets detached, or ninja writing to a file descriptor we can't intercept).

## Recommended Next Steps

### Option A: Fix runner environment (requires infra access)
1. **Add MASM component** to VS BuildTools on `orca-win-server`:
   - VS Installer → Modify → "Desktop development with C++" → "C++ CMake tools for Windows" → Individual components → "C++ MASM for v143 build tools"
2. **Check disk space** — ensure `orca-win-server` has free disk space (>10 GB) for deps build
3. **Check file permissions** — ensure `$RUNNER_WORK` directory is fully accessible
4. **Add explicit error capture** — have ninja run with `-d explain` or `--keepdepfile` to see post-build step errors

### Option B: Cache deps on our branch (workaround)
1. Change cache key to include a parent fallback that matches `main`'s cache
2. This would give us a cache hit and skip the deps build entirely
3. But this only tests the slicer build, not the deps build scripts

### Option C: Local Docker benchmarking (parallel path)
1. Use `Dockerfile.local` to build a Linux-based benchmark environment
2. Run `.bat` vs `.ps1` comparison using WSL + Docker with container resource bounds
3. This would show PS1 performance characteristics but on Linux (clang-cl + Ninja)
4. Can be done immediately without runner fixes

### Option D: Add post-failure log extraction
1. After ninja fails, run `cmake --build . --target install` manually with stderr captured to a separate file
2. Or use `ninja -C . -t clean && ninja -C . 2>ninja-stderr.log` to capture stderr separately

## Current State

- All script bug fixes are **committed and pushed** to `feat/build-performance` (head SHA `4f9127a3b6`)
- CI run `40886998694` dispatched and running
- Windows Build Deps jobs **block** all downstream jobs (slicer build, tests, observability)
- The `.bat` vs `.ps1` performance comparison **cannot proceed** until deps build succeeds
- Local test scripts (`test_build_win.ps1`, `test_prune_cached_presets.ps1`, `test_check_exception_throw_info.ps1`) pass locally (except environment-specific LLVM/winget path failures)
